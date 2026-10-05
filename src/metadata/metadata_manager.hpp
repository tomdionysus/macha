// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "metadata/metadata_server.hpp"
#include "contract/metadata_view.hpp"
#include "contract/thread_safety.hpp"
#include "contract/time_source.hpp"
#include "contract/work.hpp"
#include "cluster/cluster.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace macha {

// Discovery incomplete: cluster forming, write floor unavailable, or divergent
// histories awaiting reconciliation. A lifecycle state, not daemon-fatal.
class MetadataNotReady final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};



// A snapshot carries its namespace as a tree root or an entry map, never both:
// both would let the two disagree and every reader pick. The encoders refuse to
// write that state; this refuses to hand it out. Throws MetadataNotReady.
void require_coherent_namespace(const MetadataSnapshot&);

class DistributedStore;


const char* metadata_availability_name(MetadataAvailability) noexcept;


struct MetadataPublicationContext {
    NodeId origin{};
    uint64_t sequence{};
    const MetadataRecord& parent;
    const MetadataSnapshot& proposed;
    const MetadataDelta* delta{};
};



struct MetadataHistoryTransferDiagnostics {
    uint64_t transfers{};
    uint64_t entries_submitted{};
    uint64_t peak_in_flight{};
};

class MetadataManager final : public MetadataView, public MetadataMaintenance {
    NodeRuntime& node_;
    LocalState& local_;
    MetadataServer& metadata_server_;
    DistributedStore* namespace_store_{};
    const TimeSource& time_;
    // Guards no state: serialises mutations, repair and history checkpoints.
    // Held across replica RPCs and metadata commits.
    IoMutex mutation_mutex_;
    // Guards no state: serialises the multi-head merge-and-publish branch of
    // read_group(), held across its history imports and commit fan-out.
    // mutate_impl() holds mutation_mutex_ while calling read_group().
    IoMutex reconciliation_mutex_ MACHA_ACQUIRED_AFTER(mutation_mutex_);
    // Retry cooldown for peer certificates this replica cannot accept (record
    // fails to materialise locally). read_group() runs on nearly every read;
    // without it each call rejects and logs them afresh.
    mutable Mutex unacceptable_head_mutex_;
    mutable std::map<Hash256, Clock::time_point>
        unacceptable_head_retry_at_ MACHA_GUARDED_BY(unacceptable_head_mutex_);
    // A head this node cannot merge with its own now (no ancestor in common
    // is known, or its content cannot be fetched from a node present) is set
    // aside in the replica: out of reads, writes and release until the
    // membership changes, when the merge is tried again. This is the
    // membership the heads were set aside under.
    mutable std::atomic_uint64_t set_aside_stamp_{};
    mutable std::atomic_uint64_t set_aside_count_{};
    std::atomic_uint64_t head_holders_{};
    std::atomic_uint64_t head_present_{};
    uint64_t membership_stamp() const;
    // The accepted heads not set aside.
    std::vector<MetadataRecord> usable_heads() const;
    // Of several heads, the one carrying this node's own latest mutation.
    MetadataRecord own_head(const std::vector<MetadataRecord>&) const;
    void set_aside(const MetadataRecord&, std::string_view reason) const;
    mutable Mutex cache_mutex_;
    std::optional<MetadataRecord> cache_ MACHA_GUARDED_BY(cache_mutex_);
    Clock::time_point cache_until_ MACHA_GUARDED_BY(cache_mutex_){};
    uint64_t cache_remote_epoch_ MACHA_GUARDED_BY(cache_mutex_){};
    std::shared_ptr<const MetadataSnapshot> decoded_cache_ MACHA_GUARDED_BY(cache_mutex_);
    uint64_t decoded_generation_ MACHA_GUARDED_BY(cache_mutex_){};
    std::atomic_uint64_t available_generation_{};
    uint64_t decoded_namespace_revision_ MACHA_GUARDED_BY(cache_mutex_){};
    std::atomic_uint64_t available_namespace_revision_{};
    Hash256 decoded_hash_ MACHA_GUARDED_BY(cache_mutex_){};
    std::function<void(const MetadataPublicationContext&)> publication_retention_;

    // Observational replica state: lock-free reads, no I/O. Refreshed by the
    // background repair owner.
    std::atomic_uint64_t replica_generation_{};
    std::atomic_uint64_t replica_observed_unix_ms_{};
    std::atomic_uint32_t metadata_replicas_{};
    std::atomic_uint32_t metadata_replicas_online_{};
    std::atomic_uint32_t metadata_write_replicas_required_{};
    std::atomic_bool metadata_replica_set_stable_{};
    std::atomic_bool metadata_write_available_{};
    std::atomic<MetadataAvailability> metadata_availability_{MetadataAvailability::unavailable};
    std::atomic_uint64_t history_transfers_{};
    std::atomic_uint64_t history_entries_submitted_{};
    std::atomic_uint64_t history_peak_in_flight_{};
    // Discipline 4: conflicts decided by a later mutation, and ones an
    // operator resolved.
    std::atomic_uint64_t conflicts_superseded_{};
    std::atomic_uint64_t conflicts_resolved_{};
    // Mutation wall time split into retention barrier and commit fan-out;
    // totals and maxima since start.
    std::atomic_uint64_t mutations_{};
    std::atomic_uint64_t mutation_retention_ms_total_{};
    std::atomic_uint64_t mutation_retention_ms_max_{};
    std::atomic_uint64_t mutation_publish_ms_total_{};
    std::atomic_uint64_t mutation_publish_ms_max_{};

    std::optional<NodeInfo> node_info(const NodeId&) const;
    std::vector<NodeInfo> replica_nodes(const std::vector<NodeId>&) const;
    MetadataRecord discover_or_form();
    struct RecoverySurvey {
        bool complete{};
        bool durable_history{};
    };

    RecoverySurvey recover_from_committed_checkpoints(const std::vector<NodeInfo>& active);
    MetadataRecord read_group(const std::vector<NodeId>&,
                              FrameType frame_type = FrameType::control);
    MetadataRecord read_record_base();
    MetadataRecord maybe_reconfigure(const MetadataRecord&);
    MetadataRecord read_record_uncached();
    MetadataRecord cache_record(const MetadataRecord&);
    MetadataRecord cache_record(const MetadataRecord&,
                                std::shared_ptr<const MetadataSnapshot> decoded);
    std::optional<MetadataRecord> cached_record();
    std::optional<MetadataSnapshotView> cached_snapshot_view();
    bool import_history_from_peer(const NodeInfo&, const Hash256&, FrameType);
    bool push_history_to_peer(const NodeInfo&, const Hash256&, FrameType);

    struct PublishedCommit {
        MetadataRecord record;
        MetadataAcceptance acceptance;
        std::vector<NodeInfo> stored_on;
    };
    MetadataHistoryEntry commit_history_entry(const MetadataRecord&,
                                               std::span<const uint8_t> delta = {}) const;
    // A peer that made no progress on a commit call for write_stall is not
    // asked again until dead_after has passed: by then membership has either
    // dropped it or it is answering.
    mutable Mutex stalled_mutex_;
    mutable std::map<NodeId, Clock::time_point> stalled_until_ MACHA_GUARDED_BY(stalled_mutex_);
    bool stalled(const NodeId&) const;
    // A call to a peer on the commit path, waited for only while it makes
    // progress. Throws when the peer is stalled or stalls.
    RpcReply commit_call(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    bool store_commit_on(const NodeInfo&, const MetadataHistoryEntry&,
                         const MetadataRecord&, FrameType);
    bool accept_commit_on(const NodeInfo&, const MetadataAcceptance&, FrameType);
    PublishedCommit publish_commit(const std::vector<NodeInfo>&, const MetadataRecord&,
                                   std::span<const uint8_t> delta, FrameType);
    std::vector<std::pair<NodeInfo, MetadataAcceptance>> discover_accepted_heads(
        const std::vector<NodeInfo>&, FrameType);
    // Full-roster discover_accepted_heads() for attempt_history_checkpoint():
    // compaction must not proceed on a partial view. Returns nullopt if any
    // participant is missing, errors, or does not recognise the request.
    std::optional<std::vector<std::pair<NodeInfo, MetadataAcceptance>>>
    discover_accepted_heads_required(const std::vector<NodeInfo>&, FrameType);
    bool replicate_accepted_head(const NodeInfo&, const MetadataRecord&,
                                 const MetadataAcceptance&, FrameType);
    // Offers a head this node already holds to the nodes present.
    void offer_accepted_head(const std::vector<NodeInfo>&, const MetadataRecord&, FrameType);
    bool propose_history_floor_on(const NodeInfo&, const HistoryCheckpointProof&, FrameType);
    bool commit_history_floor_on(const NodeInfo&, const Hash256& floor_hash, const Hash256& epoch,
                                 FrameType);

    MetadataRecord mutate_impl(
        const std::function<void(MetadataSnapshot&, MetadataDelta*)>&, bool exact_delta,
        size_t retries, std::optional<MetadataMutationIdentity> identity);
    void publish_replica_state(bool validated, std::string_view reason = {});

  public:
    // `namespace_store`: where tree namespace nodes live; construction installs
    // tree-delta commit application through it (spec B2). Without it only
    // map-backed namespaces are served. `publication_retention`: runs before
    // each commit is published (the claims barrier: every object the new head
    // refers to is durably claimed first).
    using PublicationRetention = std::function<void(const MetadataPublicationContext&)>;
    // `time`: what the decoded cache's lifetime (config metadata_cache) is
    // measured by.
    MetadataManager(NodeRuntime&, LocalState&, MetadataServer&,
                    DistributedStore* namespace_store = nullptr,
                    PublicationRetention publication_retention = {},
                    const TimeSource& time = steady_time_source());

    MetadataRecord read_record();
    MetadataRecord record() override { return read_record(); }
    MetadataSnapshot snapshot();
    MetadataSnapshotView snapshot_view();
    // May refresh a stale cache from the replicas: waits on the state device
    // and the network; the wait guard refuses it to control work.
    MetadataSnapshotView snapshot_view(const WorkContext&);
    std::optional<MetadataSnapshotView> available_snapshot_view() const;
    MetadataClusterStatus cluster_status() const noexcept;
    MetadataHistoryTransferDiagnostics history_transfer_diagnostics() const noexcept {
        return {
            history_transfers_.load(std::memory_order_relaxed),
            history_entries_submitted_.load(std::memory_order_relaxed),
            history_peak_in_flight_.load(std::memory_order_relaxed),
        };
    }
    using MutationTiming = MetadataMutationTiming;
    MutationTiming mutation_timing() const noexcept override {
        return {mutations_.load(std::memory_order_relaxed),
                mutation_retention_ms_total_.load(std::memory_order_relaxed),
                mutation_retention_ms_max_.load(std::memory_order_relaxed),
                mutation_publish_ms_total_.load(std::memory_order_relaxed),
                mutation_publish_ms_max_.load(std::memory_order_relaxed)};
    }
    // The catalogue root at the common ancestor of two heads, when this
    // node's history still reaches it: the base the catalogue merges two
    // roots over. The outer value is empty when it does not.
    std::optional<std::optional<ObjectId>>
    common_ancestor_catalogue_root(const Hash256& left, const Hash256& right) const override;
    MetadataHeadStanding head_standing() const noexcept override {
        return {head_holders_.load(std::memory_order_relaxed),
                head_present_.load(std::memory_order_relaxed),
                set_aside_count_.load(std::memory_order_relaxed)};
    }
    uint64_t conflicts_superseded() const noexcept override {
        return conflicts_superseded_.load(std::memory_order_relaxed);
    }
    uint64_t conflicts_resolved() const noexcept override {
        return conflicts_resolved_.load(std::memory_order_relaxed);
    }
    // Installs the chosen alternative ("left", "right" or "base") and drops the
    // conflict in one commit. False if no such conflict stands;
    // std::invalid_argument for an unknown choice.
    bool resolve_conflict(const std::string& id, std::string_view choice) override;
    void note_replica_validation(bool available, std::string_view reason = {}) override {
        publish_replica_state(available, reason);
    }
    uint64_t available_snapshot_generation() const noexcept {
        return available_generation_.load(std::memory_order_acquire);
    }
    uint64_t available_namespace_revision() const noexcept {
        return available_namespace_revision_.load(std::memory_order_acquire);
    }
    uint64_t current_generation() const noexcept override {
        return available_snapshot_generation();
    }
    uint64_t current_namespace_revision() const noexcept override {
        return available_namespace_revision();
    }
    MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&,
                          size_t retries = 8) override;
    // Caller supplies the exact delta; avoids a second deep copy of the
    // namespace. `identity` is an idempotency key: applied only if
    // mutation_sequences[identity.origin] < identity.sequence, which it then
    // stamps, so a caller can tell after a crash whether it took effect.
    MetadataRecord mutate_delta(
        const std::function<void(MetadataSnapshot&, MetadataDelta&)>&, size_t retries = 8,
        std::optional<MetadataMutationIdentity> identity = {}) override;
    // Snapshot at the last causal stability horizon; for retention release only.
    std::optional<MetadataSnapshotView> retention_release_view() const;
    void repair_once();
    // One propose/ack/commit round toward re-rooting local history. No-op
    // unless size thresholds are met, there is one local accepted head, and
    // every known participant is directly reachable. Thresholds are
    // parameters so tests can force a round.
    void attempt_history_checkpoint(size_t record_threshold = 256,
                                    uint64_t byte_threshold = 64ULL * 1024 * 1024) override;
    // Fetches each head flagged unreconstructable as a full body from a
    // reachable peer and re-anchors it locally. Returns heads repaired.
    size_t repair_unreconstructable_heads(FrameType frame_type = FrameType::control) override;

    // MetadataView (spec B2) under the contract's names.
    std::optional<MetadataSnapshotView> current() const override {
        return available_snapshot_view();
    }
    MetadataSnapshotView converged() override { return snapshot_view(); }
    MetadataSnapshotView converged(const WorkContext& context) override {
        return snapshot_view(context);
    }
    std::optional<MetadataSnapshotView> release_head() const override {
        return retention_release_view();
    }
    MetadataClusterStatus status() const noexcept override { return cluster_status(); }
    void repair_step() override { repair_once(); }
    Page<std::pair<std::string, FsEntry>, std::string>
    entries(const MetadataSnapshotView& view, Cursor<std::string> from, Budget& budget) override;
};
} // namespace macha

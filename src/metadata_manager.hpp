// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace macha {

// Metadata discovery can legitimately be incomplete while a new cluster is
// forming, the configured write floor is unavailable, or divergent histories
// are waiting for reconciliation. This is a lifecycle state, not a daemon-fatal
// error.
class MetadataNotReady final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct MetadataSnapshotView {
    uint64_t generation{};
    uint64_t namespace_revision{};
    Hash256 hash{};
    std::shared_ptr<const MetadataSnapshot> snapshot;
};


enum class MetadataAvailability : uint8_t {
    unavailable = 0,
    read_only = 1,
    writable = 2,
};

const char* metadata_availability_name(MetadataAvailability) noexcept;


struct MetadataPublicationContext {
    NodeId origin{};
    uint64_t sequence{};
    const MetadataRecord& parent;
    const MetadataSnapshot& proposed;
    const MetadataDelta* delta{};
};

struct MetadataClusterStatus {
    uint64_t generation{};
    uint64_t observed_unix_ms{};
    uint32_t replicas{};
    uint32_t replicas_online{};
    uint32_t write_replicas_required{};
    MetadataAvailability availability{MetadataAvailability::unavailable};
    bool stable{};
    bool write_available{};
};

// See MetadataManager::mutate_delta(). `origin` is any NodeId-shaped key the
// caller owns (a node's own id, or one derived from it for a sub-system);
// `sequence` must increase across that caller's mutations.
struct MetadataMutationIdentity {
    NodeId origin{};
    uint64_t sequence{};
};

struct MetadataHistoryTransferDiagnostics {
    uint64_t transfers{};
    uint64_t entries_submitted{};
    uint64_t peak_in_flight{};
};

class MetadataManager {
    NodeRuntime& node_;
    std::mutex mutation_mutex_;
    // Guards only the multi-head merge-and-publish branch of read_group().
    // Kept separate from mutation_mutex_ because mutate_impl() already holds
    // mutation_mutex_ across its whole body while calling read_group()
    // internally; reusing mutation_mutex_ here would self-deadlock on that path.
    std::mutex reconciliation_mutex_;
    // An observed peer certificate that this replica cannot accept (its
    // record fails to materialize locally) is rejected fresh on every single
    // read_group() call otherwise -- read_group() runs on essentially every
    // ordinary metadata read cluster-wide, and unlike MetadataReplica's own
    // accepted-head reconstruction (which has its own cooldown, see
    // unreconstructable_head_retry_at_ in metadata.hpp), this rejection path
    // is a separate call (MetadataReplica::accept_commit()'s own early
    // materialize check, not accepted_heads()) and was not covered by that
    // fix. Bounds the resulting "ignoring metadata head without a valid
    // acceptance certificate" log volume the same way.
    mutable std::mutex unacceptable_head_mutex_;
    mutable std::map<Hash256, Clock::time_point> unacceptable_head_retry_at_;
    mutable std::mutex cache_mutex_;
    std::optional<MetadataRecord> cache_;
    Clock::time_point cache_until_{};
    uint64_t cache_remote_epoch_{};
    std::shared_ptr<const MetadataSnapshot> decoded_cache_;
    uint64_t decoded_generation_{};
    std::atomic_uint64_t available_generation_{};
    uint64_t decoded_namespace_revision_{};
    std::atomic_uint64_t available_namespace_revision_{};
    Hash256 decoded_hash_{};
    std::function<void(const MetadataPublicationContext&)> publication_retention_;

    // Published operational metadata-replica state. This is observational only:
    // reads are lock-free and never initiate metadata/network I/O. The state is
    // refreshed by the existing background metadata repair owner.
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
    // Discipline 4: conflicts that left the snapshot because a later
    // mutation decided them, and ones an operator resolved explicitly.
    std::atomic_uint64_t conflicts_superseded_{};
    std::atomic_uint64_t conflicts_resolved_{};

    std::optional<NodeInfo> node_info(const NodeId&) const;
    std::vector<NodeInfo> replica_nodes(const std::vector<NodeId>&) const;
    std::vector<NodeInfo> compatible_replicas(const std::vector<NodeInfo>&) const;
    void require_metadata_policy_match(const std::vector<NodeInfo>&) const;
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
    bool store_commit_on(const NodeInfo&, const MetadataHistoryEntry&,
                         const MetadataRecord&, FrameType);
    bool accept_commit_on(const NodeInfo&, const MetadataAcceptance&, FrameType);
    size_t acceptance_floor_for(const MetadataRecord&) const;
    PublishedCommit publish_commit(const std::vector<NodeInfo>&, const MetadataRecord&,
                                   std::span<const uint8_t> delta, FrameType);
    std::vector<std::pair<NodeInfo, MetadataAcceptance>> discover_accepted_heads(
        const std::vector<NodeInfo>&, FrameType);
    // Full-roster, abort-on-any-miss sibling of discover_accepted_heads(). Used
    // only by attempt_history_checkpoint(): a compaction proposal must never
    // proceed against an incomplete or uncertain view of the cluster. Returns
    // nullopt (not a partial result) the moment any participant is missing,
    // errors, or does not recognise the request.
    std::optional<std::vector<std::pair<NodeInfo, MetadataAcceptance>>>
    discover_accepted_heads_required(const std::vector<NodeInfo>&, FrameType);
    bool replicate_accepted_head(const NodeInfo&, const MetadataRecord&,
                                 const MetadataAcceptance&, FrameType);
    void ensure_accepted_head_durable(const std::vector<NodeInfo>&, const MetadataRecord&,
                                      size_t, FrameType);
    bool propose_history_floor_on(const NodeInfo&, const HistoryCheckpointProof&, FrameType);
    bool commit_history_floor_on(const NodeInfo&, const Hash256& floor_hash, const Hash256& epoch,
                                 FrameType);

    MetadataRecord mutate_impl(
        const std::function<void(MetadataSnapshot&, MetadataDelta*)>&, bool exact_delta,
        size_t retries, std::optional<MetadataMutationIdentity> identity);
    void publish_replica_state(bool validated, std::string_view reason = {});

  public:
    explicit MetadataManager(NodeRuntime&);
    void set_publication_retention(std::function<void(const MetadataPublicationContext&)> guard) {
        publication_retention_ = std::move(guard);
    }
    MetadataRecord read_record();
    MetadataSnapshot snapshot();
    MetadataSnapshotView snapshot_view();
    std::optional<MetadataSnapshotView> available_snapshot_view() const;
    MetadataClusterStatus cluster_status() const noexcept;
    MetadataHistoryTransferDiagnostics history_transfer_diagnostics() const noexcept {
        return {
            history_transfers_.load(std::memory_order_relaxed),
            history_entries_submitted_.load(std::memory_order_relaxed),
            history_peak_in_flight_.load(std::memory_order_relaxed),
        };
    }
    uint64_t conflicts_superseded() const noexcept {
        return conflicts_superseded_.load(std::memory_order_relaxed);
    }
    uint64_t conflicts_resolved() const noexcept {
        return conflicts_resolved_.load(std::memory_order_relaxed);
    }
    // Operator resolution of one standing conflict: install the chosen
    // alternative ("left", "right" or "base") for its subject and drop the
    // record, in one metadata commit. Returns false when no conflict with
    // that id stands (already superseded, resolved, or never existed);
    // throws std::invalid_argument for an unknown choice.
    bool resolve_conflict(const std::string& id, std::string_view choice);
    void note_replica_validation(bool available, std::string_view reason = {}) {
        publish_replica_state(available, reason);
    }
    uint64_t available_snapshot_generation() const noexcept {
        return available_generation_.load(std::memory_order_acquire);
    }
    uint64_t available_namespace_revision() const noexcept {
        return available_namespace_revision_.load(std::memory_order_acquire);
    }
    MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&, size_t retries = 8);
    // Fast path for callers that can describe the exact delta as they mutate the
    // decoded snapshot. Avoids retaining a second deep copy of the namespace.
    // `identity`, when given, is an idempotency key in the snapshot's
    // mutation-sequence clock: the mutation is applied only if
    // mutation_sequences[identity.origin] < identity.sequence, and stamps that
    // clock to identity.sequence when it is. A caller that must know after a
    // crash whether its mutation took effect (the FUSE namespace loop's mixed
    // batches) reads the clock instead of re-deriving per-operation effects.
    MetadataRecord mutate_delta(
        const std::function<void(MetadataSnapshot&, MetadataDelta&)>&, size_t retries = 8,
        std::optional<MetadataMutationIdentity> identity = {});
    // Snapshot at the last causal stability horizon. Retention release may use
    // this view; ordinary reads must use snapshot_view()/available_snapshot_view().
    std::optional<MetadataSnapshotView> retention_release_view() const;
    void repair_once();
    // One propose/ack/commit round toward safely re-rooting local history.
    // Gated internally on local size thresholds, a single local accepted
    // head, and every durably-known participant being currently, directly
    // reachable (mirrors Membership::all_known_reachable()'s existing use as
    // the destructive-GC fence). A no-op most of the time: it returns
    // immediately unless compaction is actually due. Driven by the same
    // maintenance cycle as repair_once(). Thresholds default to
    // compact_history_if_safe()'s own defaults and exist as parameters for
    // the same reason that method's do: so tests can force an otherwise
    // rare, size-gated round deterministically.
    void attempt_history_checkpoint(size_t record_threshold = 256,
                                    uint64_t byte_threshold = 64ULL * 1024 * 1024);
    // Live repair for accepted heads the local replica has flagged as
    // unreconstructable (MetadataReplica::unreconstructable_heads()): ask each
    // reachable peer for the record as a self-contained full body
    // (get_metadata_history_record) and re-anchor it locally
    // (MetadataReplica::reanchor_history()). No restart, no quarantine. Driven
    // by the maintenance cycle; a no-op when nothing is flagged. Returns the
    // number of heads repaired this call.
    size_t repair_unreconstructable_heads(FrameType frame_type = FrameType::control);
};
} // namespace macha

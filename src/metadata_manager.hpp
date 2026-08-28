// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"

#include <atomic>
#include <functional>
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

class MetadataManager {
    NodeRuntime& node_;
    std::mutex mutation_mutex_;
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
    MetadataRecord cache_record(const MetadataRecord&, std::shared_ptr<MetadataSnapshot> decoded);
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
    bool replicate_accepted_head(const NodeInfo&, const MetadataRecord&,
                                 const MetadataAcceptance&, FrameType);
    void ensure_accepted_head_durable(const std::vector<NodeInfo>&, const MetadataRecord&,
                                      size_t, FrameType);

    MetadataRecord mutate_impl(
        const std::function<void(MetadataSnapshot&, MetadataDelta*)>&, bool exact_delta,
        size_t retries);
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
    MetadataRecord mutate_delta(
        const std::function<void(MetadataSnapshot&, MetadataDelta&)>&, size_t retries = 8);
    // Snapshot at the last causal stability horizon. Retention release may use
    // this view; ordinary reads must use snapshot_view()/available_snapshot_view().
    std::optional<MetadataSnapshotView> retention_release_view() const;
    void repair_once();
};
} // namespace macha

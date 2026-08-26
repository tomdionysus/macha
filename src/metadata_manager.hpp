// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "placement.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace macha {

// Metadata discovery can legitimately be incomplete while a new cluster is
// forming or while a previously committed voter group is recovering.  This is
// a lifecycle state, not a daemon-fatal error.
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

class MetadataManager {
    NodeRuntime& node_;
    Hash256 placement_key_;
    std::mutex mutation_mutex_;
    mutable std::mutex cache_mutex_;
    std::optional<MetadataRecord> cache_;
    Clock::time_point cache_until_{};
    std::shared_ptr<const MetadataSnapshot> decoded_cache_;
    uint64_t decoded_generation_{};
    std::atomic_uint64_t available_generation_{};
    uint64_t decoded_namespace_revision_{};
    std::atomic_uint64_t available_namespace_revision_{};
    Hash256 decoded_hash_{};

    struct CasResult {
        size_t success{};
        bool conflict{};
        std::optional<MetadataRecord> committed;
    };

    std::optional<NodeInfo> node_info(const NodeId&) const;
    std::vector<NodeInfo> voter_nodes(const std::vector<NodeId>&) const;
    MetadataRecord latest(const std::vector<MetadataRecord>&) const;
    MetadataRecord discover_or_form();
    struct RecoverySurvey {
        bool complete{};
        bool durable_history{};
        std::optional<MetadataRecord> recovered;
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

    bool seed_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required,
                     FrameType frame_type = FrameType::control);
    bool checkpoint_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required,
                           FrameType frame_type = FrameType::control);
    bool commit_quorum(const std::vector<NodeInfo>&, uint64_t generation, const Hash256&,
                       size_t required, FrameType frame_type);
    void commit_all_best_effort(const std::vector<NodeInfo>&, uint64_t generation,
                                const Hash256&, FrameType frame_type);
    void refresh_cache_identity(const MetadataIdentity&);
    void seed_all_best_effort(const std::vector<NodeInfo>&, const MetadataRecord&,
                              FrameType frame_type = FrameType::control);
    CasResult cas_quorum(const std::vector<NodeInfo>&, const MetadataRecord&,
                         std::span<const uint8_t>, size_t required,
                         FrameType frame_type = FrameType::control);
    CasResult cas_delta_quorum(const std::vector<NodeInfo>&, const MetadataRecord&,
                               std::span<const uint8_t> delta, Bytes proposed_payload,
                               size_t required, FrameType frame_type = FrameType::control);
    MetadataRecord mutate_impl(
        const std::function<void(MetadataSnapshot&, MetadataDelta*)>&, bool exact_delta,
        size_t retries);

  public:
    explicit MetadataManager(NodeRuntime&);
    MetadataRecord read_record();
    MetadataSnapshot snapshot();
    MetadataSnapshotView snapshot_view();
    std::optional<MetadataSnapshotView> available_snapshot_view() const;
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
    void repair_once();
};
} // namespace macha

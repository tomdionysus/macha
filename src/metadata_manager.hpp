// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "placement.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace macha {
struct MetadataSnapshotView {
    uint64_t generation{};
    Hash256 hash{};
    std::shared_ptr<const MetadataSnapshot> snapshot;
};

class MetadataManager {
    NodeRuntime& node_;
    Hash256 placement_key_;
    std::mutex mutation_mutex_;
    std::mutex cache_mutex_;
    std::optional<MetadataRecord> cache_;
    Clock::time_point cache_until_{};
    std::shared_ptr<const MetadataSnapshot> decoded_cache_;
    uint64_t decoded_generation_{};
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
    std::optional<MetadataRecord> recover_from_committed_checkpoints(
        const std::vector<NodeInfo>& active);
    MetadataRecord read_group(const std::vector<NodeId>&,
                              FrameType frame_type = FrameType::control);
    MetadataRecord read_record_base();
    MetadataRecord maybe_reconfigure(const MetadataRecord&);
    MetadataRecord read_record_uncached();
    MetadataRecord cache_record(const MetadataRecord&);
    std::optional<MetadataRecord> cached_record();
    std::optional<MetadataSnapshotView> cached_snapshot_view();

    bool seed_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required,
                     FrameType frame_type = FrameType::control);
    bool checkpoint_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required,
                           FrameType frame_type = FrameType::control);
    bool commit_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required,
                       FrameType frame_type);
    void seed_all_best_effort(const std::vector<NodeInfo>&, const MetadataRecord&,
                              FrameType frame_type = FrameType::control);
    CasResult cas_quorum(const std::vector<NodeInfo>&, const MetadataRecord&,
                         std::span<const uint8_t>, size_t required,
                         FrameType frame_type = FrameType::control);

  public:
    explicit MetadataManager(NodeRuntime&);
    MetadataRecord read_record();
    MetadataSnapshot snapshot();
    MetadataSnapshotView snapshot_view();
    MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&, size_t retries = 8);
    void repair_once();
};
} // namespace macha

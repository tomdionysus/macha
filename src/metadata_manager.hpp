// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "placement.hpp"

#include <functional>
#include <mutex>
#include <optional>

namespace macha {
class MetadataManager {
    NodeRuntime& node_;
    Hash256 placement_key_;
    std::mutex mutation_mutex_;
    std::mutex cache_mutex_;
    std::optional<MetadataRecord> cache_;
    Clock::time_point cache_until_{};

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
    MetadataRecord read_group(const std::vector<NodeId>&);
    MetadataRecord maybe_reconfigure(const MetadataRecord&);
    MetadataRecord read_record_uncached();
    MetadataRecord cache_record(const MetadataRecord&);
    std::optional<MetadataRecord> cached_record();

    bool seed_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required);
    bool checkpoint_quorum(const std::vector<NodeInfo>&, const MetadataRecord&, size_t required);
    void seed_all_best_effort(const std::vector<NodeInfo>&, const MetadataRecord&);
    CasResult cas_quorum(const std::vector<NodeInfo>&, const MetadataRecord&,
                         std::span<const uint8_t>, size_t required);

  public:
    explicit MetadataManager(NodeRuntime&);
    MetadataRecord read_record();
    MetadataSnapshot snapshot();
    MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&, size_t retries = 8);
    void repair_once();
};
} // namespace macha

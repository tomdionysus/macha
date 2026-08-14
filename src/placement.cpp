// SPDX-License-Identifier: GPL-3.0-or-later
#include "placement.hpp"

#include "crypto.hpp"

#include <algorithm>
#include <set>

namespace macha {
std::vector<NodeInfo> rendezvous_nodes(std::span<const uint8_t> key,
                                       const std::vector<NodeInfo>& nodes, size_t count) {
    struct ScoredNode {
        std::array<uint8_t, 32> score;
        NodeInfo node;
    };

    std::vector<ScoredNode> scored;
    scored.reserve(nodes.size());
    for (const auto& node : nodes) {
        Bytes material(key.begin(), key.end());
        material.insert(material.end(), node.id.bytes.begin(), node.id.bytes.end());
        scored.push_back({sha256(material).bytes, node});
    }
    std::sort(scored.begin(), scored.end(), [](const ScoredNode& a, const ScoredNode& b) {
        return a.score != b.score ? a.score > b.score : a.node.id > b.node.id;
    });

    count = std::min(count, scored.size());
    std::vector<NodeInfo> out;
    out.reserve(count);
    std::set<std::string> domains;
    std::set<NodeId> chosen;

    // Prefer failure-domain diversity while preserving HRW order within each
    // choice. Placement remains deterministic on every node; no coordinator-local
    // topology knowledge is required.
    for (const auto& item : scored) {
        if (out.size() == count)
            break;
        auto domain = item.node.failure_domain.empty() ? to_string(item.node.id)
                                                       : item.node.failure_domain;
        if (domains.insert(domain).second) {
            out.push_back(item.node);
            chosen.insert(item.node.id);
        }
    }

    // If there are fewer domains than replicas, fill the remaining slots using
    // ordinary rendezvous order.
    for (const auto& item : scored) {
        if (out.size() == count)
            break;
        if (chosen.insert(item.node.id).second)
            out.push_back(item.node);
    }
    return out;
}
} // namespace macha

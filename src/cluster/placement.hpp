// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"

#include <chrono>
#include <functional>
#include <optional>

namespace macha {

// Replica order for a critical-path fan-out (metadata commits, CONTROL
// retention puts): the local replica first (the caller continues from its own
// store), then peers by measured CONTROL-lane round trip, unmeasured last.
std::vector<NodeInfo> order_commit_replicas(
    std::vector<NodeInfo> replicas, const NodeId& local,
    const std::function<std::optional<std::chrono::milliseconds>(const NodeId&)>& latency);

// Data placement uses a virtual 32-bit shard space: an object ID's first 32
// bits name its shard. Capacity changes move ownership boundaries rather than
// creating a free-space feedback loop.
using PlacementShard = uint32_t;
constexpr uint64_t placement_shards = uint64_t{1} << 32U;

// Plain HRW, for metadata and other capacity-independent placement.
std::vector<NodeInfo> rendezvous_nodes(std::span<const uint8_t>, const std::vector<NodeInfo>&,
                                       size_t);

// Data placement: the first `count` entries are the preferred owners, the rest
// deterministic capacity-aware fallbacks; distinct failure domains preferred.
std::vector<NodeInfo> capacity_placement_nodes(std::span<const uint8_t>,
                                               const std::vector<NodeInfo>&, size_t count);

// Maximum logical bytes storable as `replicas` distinct-node copies, honouring
// failure-domain diversity where possible; what statfs exposes (not sum/R).
uint64_t placement_logical_capacity(const std::vector<NodeInfo>&, size_t replicas);

} // namespace macha

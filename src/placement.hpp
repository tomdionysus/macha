// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"

namespace macha {

// Placement is deliberately quantised into a fixed 32-bit shard space. Object
// IDs are already uniformly distributed hashes, so their first 32 bits identify
// one of 2^32 stable placement shards. The space is virtual: no shard table is
// materialised. Capacity changes alter ownership boundaries rather than
// introducing a free-space feedback loop.
using PlacementShard = uint32_t;
constexpr uint64_t placement_shards = uint64_t{1} << 32U;

// Metadata and other capacity-independent policy continues to use ordinary HRW.
std::vector<NodeInfo> rendezvous_nodes(std::span<const uint8_t>, const std::vector<NodeInfo>&,
                                       size_t);

// Data placement. The first `count` entries are the preferred replica owners;
// remaining entries are deterministic capacity-aware fallbacks. Distinct
// failure domains are preferred exactly as with rendezvous_nodes().
std::vector<NodeInfo> capacity_placement_nodes(std::span<const uint8_t>,
                                               const std::vector<NodeInfo>&, size_t count);

// Maximum logical bytes that can be represented with `replicas` distinct node
// copies, respecting configured failure-domain diversity when enough domains
// exist. This is the capacity that statfs should expose, not sum(capacity)/R.
uint64_t placement_logical_capacity(const std::vector<NodeInfo>&, size_t replicas);

} // namespace macha

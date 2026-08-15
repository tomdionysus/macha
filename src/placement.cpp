// SPDX-License-Identifier: GPL-3.0-or-later
#include "placement.hpp"

#include "crypto.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>

namespace macha {
namespace {

struct CapacityEntity {
    std::string stable_id;
    uint64_t capacity{};
    std::vector<NodeInfo> nodes;
};

uint64_t saturated_add(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a)
        return std::numeric_limits<uint64_t>::max();
    return a + b;
}

PlacementShard shard_for(std::span<const uint8_t> key) {
    std::array<uint8_t, 4> bytes{};
    if (key.size() >= bytes.size()) {
        std::copy_n(key.begin(), bytes.size(), bytes.begin());
    } else {
        auto hash = sha256(key);
        std::copy_n(hash.bytes.begin(), bytes.size(), bytes.begin());
    }
    return (static_cast<PlacementShard>(bytes[0]) << 24U) |
           (static_cast<PlacementShard>(bytes[1]) << 16U) |
           (static_cast<PlacementShard>(bytes[2]) << 8U) |
           static_cast<PlacementShard>(bytes[3]);
}

std::array<uint8_t, 4> shard_key(std::span<const uint8_t> key) {
    const auto shard = shard_for(key);
    return {static_cast<uint8_t>(shard >> 24U), static_cast<uint8_t>(shard >> 16U),
            static_cast<uint8_t>(shard >> 8U), static_cast<uint8_t>(shard)};
}

struct WaterLevel {
    __uint128_t numerator{}; // L = numerator / denominator
    uint64_t denominator{1};
};

WaterLevel water_level(const std::vector<uint64_t>& capacities, size_t replicas) {
    if (capacities.empty() || !replicas)
        return {};
    replicas = std::min(replicas, capacities.size());

    auto sorted = capacities;
    std::sort(sorted.begin(), sorted.end());
    __uint128_t prefix = 0;
    for (size_t k = 0; k < sorted.size(); ++k) {
        prefix += sorted[k];
        const auto signed_denominator = static_cast<int64_t>(replicas) -
                                        static_cast<int64_t>(sorted.size()) +
                                        static_cast<int64_t>(k) + 1;
        if (signed_denominator <= 0)
            continue;
        const uint64_t denominator = static_cast<uint64_t>(signed_denominator);
        if (k + 1 == sorted.size() ||
            prefix <= static_cast<__uint128_t>(denominator) * sorted[k + 1])
            return {prefix, denominator};
    }
    return {};
}

uint64_t max_logical_capacity(const std::vector<uint64_t>& capacities, size_t replicas) {
    auto level = water_level(capacities, replicas);
    if (!level.numerator)
        return 0;
    const __uint128_t value = level.numerator / level.denominator;
    return value > std::numeric_limits<uint64_t>::max()
               ? std::numeric_limits<uint64_t>::max()
               : static_cast<uint64_t>(value);
}

std::vector<uint64_t> shard_quotas(const std::vector<uint64_t>& capacities, size_t replicas) {
    const size_t n = capacities.size();
    std::vector<uint64_t> quota(n);
    if (!n || !replicas)
        return quota;
    replicas = std::min(replicas, n);
    const __uint128_t required = static_cast<__uint128_t>(replicas) * placement_shards;

    auto level = water_level(capacities, replicas);
    if (!level.numerator) {
        // No capacity-capable R-way placement exists (or every capacity is
        // unknown/zero). Keep the mapping deterministic and evenly spread so
        // startup/tests still have a complete fallback order; writes will fail
        // normally if the selected nodes really cannot store data.
        const uint64_t base = static_cast<uint64_t>(required / n);
        uint64_t remainder = static_cast<uint64_t>(required % n);
        for (size_t i = 0; i < n; ++i) {
            quota[i] = base + (remainder ? 1 : 0);
            if (remainder)
                --remainder;
        }
        return quota;
    }

    struct Fraction {
        size_t index{};
        __uint128_t remainder{};
    };
    std::vector<Fraction> fractions;
    fractions.reserve(n);
    __uint128_t assigned = 0;

    for (size_t i = 0; i < n; ++i) {
        const __uint128_t scaled_capacity =
            static_cast<__uint128_t>(capacities[i]) * level.denominator;
        if (scaled_capacity >= level.numerator) {
            quota[i] = placement_shards;
            assigned += placement_shards;
            fractions.push_back({i, 0});
            continue;
        }

        const __uint128_t exact_numerator = scaled_capacity * placement_shards;
        const uint64_t base = static_cast<uint64_t>(exact_numerator / level.numerator);
        quota[i] = base;
        assigned += base;
        fractions.push_back({i, exact_numerator % level.numerator});
    }

    std::sort(fractions.begin(), fractions.end(), [](const auto& a, const auto& b) {
        if (a.remainder != b.remainder)
            return a.remainder > b.remainder;
        return a.index < b.index;
    });
    for (const auto& item : fractions) {
        if (assigned >= required)
            break;
        if (quota[item.index] < placement_shards) {
            ++quota[item.index];
            ++assigned;
        }
    }

    // The sum of ideal quotas is exactly R*S; flooring can lose fewer than n
    // slots. This branch is defensive against future arithmetic changes.
    for (size_t i = 0; assigned < required && i < quota.size(); ++i) {
        if (quota[i] < placement_shards) {
            ++quota[i];
            ++assigned;
        }
    }
    return quota;
}

std::vector<size_t> systematic_shard_sample(std::span<const uint8_t> key,
                                            const std::vector<uint64_t>& capacities,
                                            size_t replicas) {
    std::vector<size_t> selected;
    if (capacities.empty() || !replicas)
        return selected;
    replicas = std::min(replicas, capacities.size());
    auto quotas = shard_quotas(capacities, replicas);
    const uint64_t shard = shard_for(key);

    // Quotas partition [0, R*S) into intervals of at most S slots. For shard s
    // the R sample points s, s+S, ... visit exactly one owner per replica slot.
    // Across all S shards each entity is selected exactly quota[i] times.
    __uint128_t begin = 0;
    for (size_t i = 0; i < quotas.size(); ++i) {
        const __uint128_t end = begin + quotas[i];
        for (size_t copy = 0; copy < replicas; ++copy) {
            const __uint128_t point = shard + static_cast<__uint128_t>(copy) * placement_shards;
            if (point >= begin && point < end) {
                selected.push_back(i);
                break;
            }
        }
        begin = end;
    }

    // Integer quota construction should make this exact. Retain a deterministic
    // safety fallback rather than returning a short owner set if future changes
    // violate that invariant.
    if (selected.size() < replicas) {
        for (size_t i = 0; i < capacities.size() && selected.size() < replicas; ++i) {
            if (std::find(selected.begin(), selected.end(), i) == selected.end())
                selected.push_back(i);
        }
    }
    return selected;
}

std::vector<CapacityEntity> failure_domains(const std::vector<NodeInfo>& nodes) {
    std::vector<CapacityEntity> groups;
    for (const auto& node : nodes) {
        const std::string domain = node.failure_domain.empty()
                                       ? std::string("node:") + to_string(node.id)
                                       : std::string("domain:") + node.failure_domain;
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const auto& group) { return group.stable_id == domain; });
        if (it == groups.end()) {
            groups.push_back({domain, node.capacity, {node}});
        } else {
            it->capacity = saturated_add(it->capacity, node.capacity);
            it->nodes.push_back(node);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        return a.stable_id < b.stable_id;
    });
    for (auto& group : groups) {
        std::sort(group.nodes.begin(), group.nodes.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
    }
    return groups;
}

std::vector<uint64_t> capacities_of(const std::vector<CapacityEntity>& entities) {
    std::vector<uint64_t> capacities;
    capacities.reserve(entities.size());
    for (const auto& entity : entities)
        capacities.push_back(entity.capacity);
    return capacities;
}

NodeInfo choose_within_group(std::span<const uint8_t> key, const CapacityEntity& group) {
    if (group.nodes.size() == 1)
        return group.nodes.front();

    std::vector<uint64_t> capacities;
    capacities.reserve(group.nodes.size());
    for (const auto& node : group.nodes)
        capacities.push_back(node.capacity);

    Bytes material(key.begin(), key.end());
    material.insert(material.end(), group.stable_id.begin(), group.stable_id.end());
    auto inner = sha256(material);
    auto picked = systematic_shard_sample(inner.bytes, capacities, 1);
    return group.nodes[picked.empty() ? 0 : picked.front()];
}

long double fallback_score(std::span<const uint8_t> key, const NodeInfo& node) {
    Bytes material(key.begin(), key.end());
    static constexpr std::string_view label = "macha/data-fallback/v7";
    material.insert(material.end(), label.begin(), label.end());
    material.insert(material.end(), node.id.bytes.begin(), node.id.bytes.end());
    auto hash = sha256(material);
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8U) | hash.bytes[i];
    const long double u = (static_cast<long double>(value) + 1.0L) /
                          (static_cast<long double>(std::numeric_limits<uint64_t>::max()) + 2.0L);
    const long double weight = node.capacity ? static_cast<long double>(node.capacity) : 1.0L;
    return -std::log(u) / weight;
}

} // namespace

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

std::vector<NodeInfo> capacity_placement_nodes(std::span<const uint8_t> key,
                                               const std::vector<NodeInfo>& nodes, size_t count) {
    if (nodes.empty() || !count)
        return {};
    count = std::min(count, nodes.size());

    // R=1 has a simpler and stronger solution: weighted rendezvous over the
    // stable shard key. Adding a backend/node only steals shards won by the
    // newcomer; existing owners never exchange shards among themselves. This
    // is particularly important for node-local backend growth.
    if (count == 1) {
        auto out = nodes;
        const auto stable_key = shard_key(key);
        std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
            const auto as = fallback_score(stable_key, a);
            const auto bs = fallback_score(stable_key, b);
            if (as != bs)
                return as < bs;
            return a.id < b.id;
        });
        return out;
    }

    auto groups = failure_domains(nodes);
    std::vector<NodeInfo> preferred;
    preferred.reserve(count);
    std::set<NodeId> chosen;

    const size_t diverse = std::min(count, groups.size());
    auto selected_groups = systematic_shard_sample(key, capacities_of(groups), diverse);
    for (auto index : selected_groups) {
        auto node = choose_within_group(key, groups[index]);
        if (chosen.insert(node.id).second)
            preferred.push_back(std::move(node));
    }

    // Fewer failure domains than replicas: retain one node from every domain,
    // then fill the remaining slots from the unused nodes by capacity.
    if (preferred.size() < count) {
        std::vector<NodeInfo> remaining;
        for (const auto& node : nodes) {
            if (!chosen.contains(node.id))
                remaining.push_back(node);
        }
        std::sort(remaining.begin(), remaining.end(), [](const auto& a, const auto& b) {
            return a.id < b.id;
        });
        std::vector<uint64_t> capacities;
        for (const auto& node : remaining)
            capacities.push_back(node.capacity);

        Bytes material(key.begin(), key.end());
        static constexpr std::string_view label = "macha/data-extra-domain/v7";
        material.insert(material.end(), label.begin(), label.end());
        auto extra_key = sha256(material);
        auto extra = systematic_shard_sample(extra_key.bytes, capacities, count - preferred.size());
        for (auto index : extra) {
            if (chosen.insert(remaining[index].id).second)
                preferred.push_back(remaining[index]);
        }
    }

    // Preferred owners are followed by deterministic capacity-aware fallbacks.
    // A full/offline preferred owner can therefore spill without changing the
    // replica target or making free-space itself part of the placement weight.
    std::vector<NodeInfo> fallback;
    for (const auto& node : nodes) {
        if (!chosen.contains(node.id))
            fallback.push_back(node);
    }
    std::sort(fallback.begin(), fallback.end(), [&](const auto& a, const auto& b) {
        const auto as = fallback_score(key, a);
        const auto bs = fallback_score(key, b);
        if (as != bs)
            return as < bs;
        return a.id < b.id;
    });
    preferred.insert(preferred.end(), fallback.begin(), fallback.end());
    return preferred;
}

uint64_t placement_logical_capacity(const std::vector<NodeInfo>& nodes, size_t replicas) {
    if (nodes.empty() || !replicas)
        return 0;
    replicas = std::min(replicas, nodes.size());

    auto groups = failure_domains(nodes);
    std::vector<uint64_t> capacities;
    if (groups.size() >= replicas) {
        capacities = capacities_of(groups);
    } else {
        capacities.reserve(nodes.size());
        for (const auto& node : nodes)
            capacities.push_back(node.capacity);
    }
    return max_logical_capacity(capacities, replicas);
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "replica_selector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace macha {
namespace {
constexpr double default_transfer_ms = 100.0;
constexpr double failure_penalty_ms = 500.0;
constexpr double ewma_alpha = 0.20;
}

double ReplicaSelector::estimate_ms(const State& state) {
    return state.ewma_transfer_ms > 0.0 ? state.ewma_transfer_ms : default_transfer_ms;
}

double ReplicaSelector::score(const State& state, ReplicaWorkClass work) {
    const double transfer = estimate_ms(state);
    const double failures =
        std::min<uint32_t>(state.consecutive_failures, 8) * failure_penalty_ms;

    if (work == ReplicaWorkClass::foreground) {
        // Foreground optimises completion latency. Existing foreground work is
        // the strongest ordinary load signal; speculative work still matters,
        // but foreground is free to use the same peer if it remains clearly
        // faster than the alternatives.
        return transfer + transfer * 1.50 * state.foreground_in_flight +
               transfer * 0.75 * state.speculative_in_flight + failures;
    }

    // Speculation yields aggressively to foreground and prefers unused peers,
    // which naturally stripes concurrent hydration over a healthy replica set.
    if (state.foreground_in_flight)
        return 1.0e12 + transfer * state.foreground_in_flight + failures;
    return transfer + transfer * 2.0 * state.speculative_in_flight + failures;
}

std::vector<NodeInfo> ReplicaSelector::order(const std::vector<NodeInfo>& candidates,
                                             size_t stripe,
                                             ReplicaWorkClass work) const {
    if (candidates.size() < 2)
        return candidates;

    struct Ranked {
        NodeInfo node;
        double score{};
        size_t tie{};
    };

    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());

    std::lock_guard lock(mutex_);
    const size_t rotation = stripe % candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto it = states_.find(candidates[i].id);
        const State empty;
        const State& state = it == states_.end() ? empty : it->second;
        ranked.push_back({candidates[i], score(state, work),
                          (i + candidates.size() - rotation) % candidates.size()});
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (std::abs(a.score - b.score) > 0.001)
            return a.score < b.score;
        return a.tie < b.tie;
    });

    std::vector<NodeInfo> out;
    out.reserve(ranked.size());
    for (auto& item : ranked)
        out.push_back(std::move(item.node));
    return out;
}

void ReplicaSelector::started(const NodeInfo& node, ReplicaWorkClass work) {
    std::lock_guard lock(mutex_);
    auto& state = states_[node.id];
    ++state.selections;
    if (work == ReplicaWorkClass::foreground)
        ++state.foreground_in_flight;
    else
        ++state.speculative_in_flight;
}

void ReplicaSelector::promoted(const NodeInfo& node) {
    std::lock_guard lock(mutex_);
    auto& state = states_[node.id];
    if (!state.speculative_in_flight)
        return;
    --state.speculative_in_flight;
    ++state.foreground_in_flight;
}

void ReplicaSelector::finished(const NodeInfo& node, ReplicaWorkClass work, size_t,
                               std::chrono::steady_clock::duration elapsed,
                               bool success) {
    const double ms = std::max(0.001,
        std::chrono::duration<double, std::milli>(elapsed).count());

    std::lock_guard lock(mutex_);
    auto& state = states_[node.id];
    if (work == ReplicaWorkClass::foreground) {
        if (state.foreground_in_flight)
            --state.foreground_in_flight;
    } else if (state.speculative_in_flight) {
        --state.speculative_in_flight;
    }

    if (success) {
        ++state.successes;
        state.consecutive_failures = 0;
        state.ewma_transfer_ms = state.ewma_transfer_ms > 0.0
                                     ? state.ewma_transfer_ms * (1.0 - ewma_alpha) + ms * ewma_alpha
                                     : ms;
    } else {
        ++state.failures;
        state.consecutive_failures = std::min<uint32_t>(state.consecutive_failures + 1, 8);
    }
}

ReplicaTransferStats ReplicaSelector::stats(const NodeId& id) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(id);
    if (it == states_.end())
        return {};
    const auto& state = it->second;
    return {state.foreground_in_flight, state.speculative_in_flight,
            state.ewma_transfer_ms, state.successes, state.failures,
            state.selections};
}

} // namespace macha

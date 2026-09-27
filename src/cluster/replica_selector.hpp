// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace macha {

enum class ReplicaWorkClass : uint8_t {
    foreground,
    speculative,
};

struct ReplicaTransferStats {
    size_t foreground_in_flight{};
    size_t speculative_in_flight{};
    double ewma_transfer_ms{};
    uint64_t successes{};
    uint64_t failures{};
    uint64_t selections{};
};

// Shared read-source policy for foreground extent retrieval and speculative
// cache hydration. It contains no network code: callers supply candidate
// replicas, reserve the chosen source, then report completion.
class ReplicaSelector {
    struct State {
        size_t foreground_in_flight{};
        size_t speculative_in_flight{};
        double ewma_transfer_ms{};
        uint64_t successes{};
        uint64_t failures{};
        uint32_t consecutive_failures{};
        uint64_t selections{};
    };

    mutable std::mutex mutex_;
    std::map<NodeId, State> states_;

    static double estimate_ms(const State&);
    static double score(const State&, ReplicaWorkClass);

  public:
    // Returns all candidates ordered from most to least suitable. With no
    // observations, stripe spreads equal candidates deterministically. Once
    // observations exist, faster and less-loaded peers receive more work.
    std::vector<NodeInfo> order(const std::vector<NodeInfo>& candidates,
                                size_t stripe, ReplicaWorkClass work) const;

    void started(const NodeInfo&, ReplicaWorkClass);
    void promoted(const NodeInfo&); // speculative transfer became foreground
    void finished(const NodeInfo&, ReplicaWorkClass, size_t bytes,
                  std::chrono::steady_clock::duration elapsed, bool success);

    ReplicaTransferStats stats(const NodeId&) const;
};

} // namespace macha

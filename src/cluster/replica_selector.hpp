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

// Read-source policy for foreground extent fetches and speculative hydration.
// No network code: callers supply candidates, call started(), then finished().
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
    // All candidates, most suitable first. `stripe` rotates equal-scored
    // candidates deterministically.
    std::vector<NodeInfo> order(const std::vector<NodeInfo>& candidates,
                                size_t stripe, ReplicaWorkClass work) const;

    void started(const NodeInfo&, ReplicaWorkClass);
    void promoted(const NodeInfo&); // speculative transfer became foreground
    void finished(const NodeInfo&, ReplicaWorkClass, size_t bytes,
                  std::chrono::steady_clock::duration elapsed, bool success);

    ReplicaTransferStats stats(const NodeId&) const;
};

} // namespace macha

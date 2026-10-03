// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include "contract/thread_safety.hpp"

namespace macha {

enum class NodeEvent : uint8_t {
    storage,
    metadata,
    topology,
};

// What has happened on this node, as one monotonic count per kind. Producers
// only notify; consumers keep the counts they last saw and react to the
// difference, so nothing calls out of a producer's thread and no consumer has
// to be installed or removed. Owned by the root, which outlives every producer
// and consumer.
//
// notify() and count() are safe from any thread. A consumer waits on wait_cv
// under wait_mutex with a predicate over count(); notify() takes wait_mutex
// before waking, so a count advanced after the predicate is checked is never
// missed.
class NodeEvents {
  public:
    static constexpr size_t kinds = 3;

    void notify(NodeEvent kind) {
        counts_[index(kind)].fetch_add(1, std::memory_order_release);
        {
            Lock lock(wait_mutex);
        }
        wait_cv.notify_all();
    }
    uint64_t count(NodeEvent kind) const noexcept {
        return counts_[index(kind)].load(std::memory_order_acquire);
    }
    // Advances whenever any count does.
    uint64_t total() const noexcept {
        uint64_t sum = 0;
        for (const auto& count : counts_)
            sum += count.load(std::memory_order_acquire);
        return sum;
    }

    // Guards nothing; held only to order a notify against a waiter's check.
    Mutex wait_mutex;
    std::condition_variable_any wait_cv;

  private:
    static size_t index(NodeEvent kind) noexcept { return static_cast<size_t>(kind); }
    std::array<std::atomic_uint64_t, kinds> counts_{};
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/node_events.hpp"
#include "types.hpp"

#include <dannan/contract/time_source.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>

namespace macha {

// The time the maintenance pass decides by: grace periods, quiet windows,
// back-offs, credit accrual, repair's share and every wake-up deadline.
// Injected so tests can step time. Time only measured (stage durations for
// logs or observations) stays on the real steady clock. It is Dannan's time
// source and wall clock, so the store node's maintenance steps run on it too.
//
// Contract: now() is monotonic non-decreasing and wall_ns() moves with it;
// both are safe from any thread. wait_until() returns once `ready()` holds,
// `stop` is requested, or now() has reached `deadline` (never, for
// Clock::time_point::max()), looking again at every event on `events`; it
// may return early and callers re-check.
class MaintenanceClock : public dannan::contract::TimeSource, public dannan::contract::WallClock {
  public:
    ~MaintenanceClock() override = default;
    Clock::time_point now() const noexcept override = 0;
    // Nanoseconds since the Unix epoch.
    virtual int64_t wall_ns() const noexcept = 0;
    uint64_t wall_ms() const noexcept {
        const auto ns = wall_ns();
        return ns > 0 ? static_cast<uint64_t>(ns) / 1'000'000 : 0;
    }
    uint64_t unix_ms() const noexcept override {
        return wall_ms();
    }
    virtual void wait_until(NodeEvents& events, std::stop_token stop, Clock::time_point deadline,
                            const std::function<bool()>& ready) = 0;
};

// Production: the steady and system clocks, and an ordinary timed wait.
class SystemMaintenanceClock final : public MaintenanceClock {
  public:
    Clock::time_point now() const noexcept override;
    int64_t wall_ns() const noexcept override;
    std::chrono::milliseconds
    wake_within(std::chrono::milliseconds remaining) const noexcept override {
        return remaining;
    }
    void wait_until(NodeEvents& events, std::stop_token stop, Clock::time_point deadline,
                    const std::function<bool()>& ready) override;
};

// Tests: time stands still until advance() moves it. It starts at the real
// clocks' readings so real-time stamps (a filesystem-retired tombstone, say)
// compare with it. Waiters poll, so they notice an advance within one poll
// interval and their own predicate stays authoritative.
class ManualMaintenanceClock final : public MaintenanceClock {
    std::atomic<Clock::rep> steady_;
    std::atomic<int64_t> wall_ns_;
    std::chrono::milliseconds poll_;

  public:
    explicit ManualMaintenanceClock(std::chrono::milliseconds poll = std::chrono::milliseconds(2));
    // Precondition: by >= 0.
    void advance(Clock::duration by);
    Clock::time_point now() const noexcept override;
    int64_t wall_ns() const noexcept override;
    void wait_until(NodeEvents& events, std::stop_token stop, Clock::time_point deadline,
                    const std::function<bool()>& ready) override;
};

} // namespace macha

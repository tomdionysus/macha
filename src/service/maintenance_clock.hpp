// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>

namespace macha {

// The time the maintenance pass decides by: grace periods, quiet windows,
// back-offs, credit accrual, repair's share and every wake-up deadline. It is
// the pass's first injected dependency (the object ledger plan, T1), so a
// test can step time instead of sleeping through it. Time that is only
// measured -- how long a stage took, for a log line or an observation --
// stays on the real steady clock.
//
// Contract: now() is monotonic non-decreasing and wall_ns() moves with it;
// both are safe from any thread. wait_until() returns once `ready()` holds,
// `stop` is requested, or now() has reached `deadline` (never, for
// Clock::time_point::max()); it may return early and callers re-check.
class MaintenanceClock {
  public:
    virtual ~MaintenanceClock() = default;
    virtual Clock::time_point now() const = 0;
    // Nanoseconds since the Unix epoch.
    virtual int64_t wall_ns() const = 0;
    uint64_t wall_ms() const {
        const auto ns = wall_ns();
        return ns > 0 ? static_cast<uint64_t>(ns) / 1'000'000 : 0;
    }
    virtual void wait_until(std::condition_variable_any&, std::unique_lock<std::mutex>&,
                            std::stop_token stop, Clock::time_point deadline,
                            const std::function<bool()>& ready) = 0;
};

// Production: the steady and system clocks, and an ordinary timed wait.
class SystemMaintenanceClock final : public MaintenanceClock {
  public:
    Clock::time_point now() const override;
    int64_t wall_ns() const override;
    void wait_until(std::condition_variable_any&, std::unique_lock<std::mutex>&,
                    std::stop_token stop, Clock::time_point deadline,
                    const std::function<bool()>& ready) override;
};

// Tests: time stands still until advance() moves it. It starts at the real
// clocks' readings so that anything stamped with real time (a tombstone
// retired by the filesystem, say) is comparable with it.
//
// A waiter notices an advance within one poll interval of real time: the
// clock cannot notify a condition variable it was never told about, and
// polling keeps the waiter's own predicate authoritative.
class ManualMaintenanceClock final : public MaintenanceClock {
    std::atomic<Clock::rep> steady_;
    std::atomic<int64_t> wall_ns_;
    std::chrono::milliseconds poll_;

  public:
    explicit ManualMaintenanceClock(std::chrono::milliseconds poll = std::chrono::milliseconds(2));
    // Precondition: by >= 0.
    void advance(Clock::duration by);
    Clock::time_point now() const override;
    int64_t wall_ns() const override;
    void wait_until(std::condition_variable_any&, std::unique_lock<std::mutex>&,
                    std::stop_token stop, Clock::time_point deadline,
                    const std::function<bool()>& ready) override;
};

} // namespace macha

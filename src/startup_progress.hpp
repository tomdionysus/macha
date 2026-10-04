// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace macha {

// Process-wide startup progress, ticked by each unit of long recovery work
// (history frame, delta, journal record, pack, readiness stage).
// Service::wait_services_ready kills the process only when this has not moved
// for `service_startup_no_progress_ms`, never merely because startup is slow.
inline std::atomic<uint64_t>& startup_progress_counter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline void note_startup_progress(uint64_t units = 1) {
    startup_progress_counter().fetch_add(units, std::memory_order_relaxed);
}

inline uint64_t startup_progress() {
    return startup_progress_counter().load(std::memory_order_relaxed);
}

// Decides when a startup has stalled: progress has not moved for
// `no_progress`, or `ceiling` has passed since it began. Zero turns either
// off. Fed each progress reading with the time it was taken; one caller.
class StartupStallGate {
  public:
    using Clock = std::chrono::steady_clock;

    StartupStallGate(std::chrono::milliseconds no_progress, std::chrono::milliseconds ceiling,
                     Clock::time_point started, uint64_t progress)
        : no_progress_(no_progress), ceiling_(ceiling), started_(started),
          last_progress_at_(started), last_progress_(progress) {}

    bool stalled(Clock::time_point now, uint64_t progress) {
        if (progress != last_progress_) {
            last_progress_ = progress;
            last_progress_at_ = now;
        }
        return (no_progress_.count() > 0 && now - last_progress_at_ >= no_progress_) ||
               (ceiling_.count() > 0 && now - started_ >= ceiling_);
    }
    // How often to take readings: a second, or a quarter of a shorter
    // no-progress window, so a stall is seen within a quarter window of it.
    std::chrono::milliseconds poll_interval() const {
        constexpr std::chrono::milliseconds most{1000};
        if (no_progress_.count() <= 0)
            return most;
        return std::clamp(no_progress_ / 4, std::chrono::milliseconds{1}, most);
    }
    Clock::time_point started() const { return started_; }
    Clock::time_point last_progress_at() const { return last_progress_at_; }

  private:
    const std::chrono::milliseconds no_progress_;
    const std::chrono::milliseconds ceiling_;
    const Clock::time_point started_;
    Clock::time_point last_progress_at_;
    uint64_t last_progress_;
};

} // namespace macha

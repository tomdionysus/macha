// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"
#include "types.hpp"

namespace macha {

// The steady time a component decides by: when a cached answer expires, when
// a deadline has passed. Handed to the component at construction and owned by
// whoever constructed it, which keeps it alive for the component's lifetime.
// Time only measured for logs or observations stays on the real clock.
class TimeSource {
  public:
    virtual ~TimeSource() = default;

    // Monotonic non-decreasing. An atomic read.
    static constexpr Waits now_waits = Waits::none;
    static constexpr ThreadSafety now_safety = ThreadSafety::thread_safe;
    virtual Clock::time_point now() const noexcept = 0;
};

// The process's steady clock. Stateless.
class SteadyTimeSource final : public TimeSource {
  public:
    Clock::time_point now() const noexcept override { return Clock::now(); }
};

// The process's SteadyTimeSource. It holds no state, so one instance living
// for the whole process serves every component.
inline const TimeSource& steady_time_source() noexcept {
    static const SteadyTimeSource source;
    return source;
}

} // namespace macha

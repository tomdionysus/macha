// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/time_source.hpp"

#include <atomic>
#include <cstdint>

namespace macha::test_support {

// A TimeSource that stands still until advance() moves it. It starts at the
// steady clock's reading, so its time points compare with real ones.
class SteppedTime final : public TimeSource {
    std::atomic<Clock::rep> now_{Clock::now().time_since_epoch().count()};
    mutable std::atomic_uint64_t reads_{};

  public:
    // Precondition: by >= 0.
    void advance(Clock::duration by) noexcept { now_.fetch_add(by.count()); }
    Clock::time_point now() const noexcept override {
        reads_.fetch_add(1);
        return Clock::time_point(Clock::duration(now_.load()));
    }
    // How many times now() has been read: lets a test wait for another thread
    // to have taken its reading before moving time on.
    uint64_t reads() const noexcept { return reads_.load(); }
};

} // namespace macha::test_support

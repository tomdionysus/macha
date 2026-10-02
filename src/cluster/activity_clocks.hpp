// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace macha {

// When this node last moved viewer, interactive and loader bytes, and how many
// since the last take. Lock-free; every operation is safe from any thread and
// never waits.
//
// The loader clock is kept apart from the viewer clocks: viewer reserves and
// the DATA pressure gate key off the viewer clocks alone, so an import must
// neither look like a viewer nor leave maintenance believing the node idle.
class ActivityClocks {
  public:
    // Activity times read `clock` (default: the steady clock).
    using Source = std::function<Clock::time_point()>;
    explicit ActivityClocks(Source clock = {});

    void note(FrameType, uint64_t bytes = 0);
    uint64_t take_bytes(FrameType);
    // 24 h when the class has never been active.
    std::chrono::milliseconds idle_for(FrameType) const;
    bool viewer_recently_active(std::chrono::milliseconds window) const;

  private:
    Source clock_;
    std::atomic_uint64_t playback_bytes_{};
    std::atomic_uint64_t interactive_bytes_{};
    std::atomic_uint64_t loader_bytes_{};
    std::atomic_int64_t last_playback_ms_{};
    std::atomic_int64_t last_interactive_ms_{};
    std::atomic_int64_t last_loader_ms_{};

    int64_t now_ms() const;
};

} // namespace macha

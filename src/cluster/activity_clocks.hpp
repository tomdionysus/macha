// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace macha {

// When work of each class was last present on this node, and how many bytes
// each frame moved since the last take. Lock-free; every operation is safe
// from any thread and never waits.
//
// Presence is per work class, the one definition (work_class()): a viewer is
// present when viewer-class work was, whether it moved bytes (playback, the
// mount) or not (an API request). The loader is kept apart from the viewer,
// so an import neither looks like a viewer nor leaves maintenance believing
// the node idle. Bytes stay per frame for the traffic they report.
class ActivityClocks {
  public:
    // Activity times read `clock` (default: the steady clock).
    using Source = std::function<Clock::time_point()>;
    explicit ActivityClocks(Source clock = {});

    void note(FrameType, uint64_t bytes = 0);
    // Work of `work_class` present, moving no bytes.
    void note(WorkClass);
    uint64_t take_bytes(FrameType);
    // 24 h when the class has never been present.
    std::chrono::milliseconds idle_for(WorkClass) const;
    bool viewer_recently_active(std::chrono::milliseconds window) const;

  private:
    static constexpr size_t classes = 4;
    Source clock_;
    std::atomic_uint64_t foreground_bytes_{};
    std::atomic_uint64_t read_ahead_bytes_{};
    std::atomic_uint64_t loader_bytes_{};
    std::array<std::atomic_int64_t, classes> last_ms_{};

    int64_t now_ms() const;
};

} // namespace macha

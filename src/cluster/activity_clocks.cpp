// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/activity_clocks.hpp"

#include <algorithm>

namespace macha {

ActivityClocks::ActivityClocks(Source clock)
    : clock_(clock ? std::move(clock) : [] { return Clock::now(); }) {}

int64_t ActivityClocks::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_().time_since_epoch())
        .count();
}

void ActivityClocks::note(FrameType type, uint64_t bytes) {
    const auto now = now_ms();
    if (type == FrameType::foreground) {
        playback_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_playback_ms_.store(now, std::memory_order_relaxed);
    } else if (type == FrameType::read_ahead) {
        interactive_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_interactive_ms_.store(now, std::memory_order_relaxed);
    } else if (type == FrameType::loader) {
        loader_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_loader_ms_.store(now, std::memory_order_relaxed);
    }
}

uint64_t ActivityClocks::take_bytes(FrameType type) {
    if (type == FrameType::foreground)
        return playback_bytes_.exchange(0, std::memory_order_relaxed);
    if (type == FrameType::read_ahead)
        return interactive_bytes_.exchange(0, std::memory_order_relaxed);
    if (type == FrameType::loader)
        return loader_bytes_.exchange(0, std::memory_order_relaxed);
    return 0;
}

std::chrono::milliseconds ActivityClocks::idle_for(FrameType type) const {
    int64_t last = 0;
    if (type == FrameType::foreground)
        last = last_playback_ms_.load(std::memory_order_relaxed);
    else if (type == FrameType::read_ahead)
        last = last_interactive_ms_.load(std::memory_order_relaxed);
    else if (type == FrameType::loader)
        last = last_loader_ms_.load(std::memory_order_relaxed);
    if (!last)
        return std::chrono::hours(24);
    return std::chrono::milliseconds(std::max<int64_t>(0, now_ms() - last));
}

bool ActivityClocks::viewer_recently_active(std::chrono::milliseconds window) const {
    return idle_for(FrameType::foreground) < window || idle_for(FrameType::read_ahead) < window;
}

} // namespace macha

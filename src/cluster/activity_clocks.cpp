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
    if (type == FrameType::foreground)
        foreground_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    else if (type == FrameType::read_ahead)
        read_ahead_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    else if (type == FrameType::loader)
        loader_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    note(work_class(type));
}

void ActivityClocks::note(WorkClass work) {
    last_ms_[static_cast<size_t>(work)].store(now_ms(), std::memory_order_relaxed);
}

uint64_t ActivityClocks::take_bytes(FrameType type) {
    if (type == FrameType::foreground)
        return foreground_bytes_.exchange(0, std::memory_order_relaxed);
    if (type == FrameType::read_ahead)
        return read_ahead_bytes_.exchange(0, std::memory_order_relaxed);
    if (type == FrameType::loader)
        return loader_bytes_.exchange(0, std::memory_order_relaxed);
    return 0;
}

std::chrono::milliseconds ActivityClocks::idle_for(WorkClass work) const {
    const auto last = last_ms_[static_cast<size_t>(work)].load(std::memory_order_relaxed);
    if (!last)
        return std::chrono::hours(24);
    return std::chrono::milliseconds(std::max<int64_t>(0, now_ms() - last));
}

bool ActivityClocks::viewer_recently_active(std::chrono::milliseconds window) const {
    return idle_for(WorkClass::viewer) < window;
}

} // namespace macha

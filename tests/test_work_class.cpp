// SPDX-License-Identifier: GPL-3.0-or-later
//
// One definition of work class: every frame's class, and the activity clocks
// keyed by it (Dannan's, which the store node reads).
#include "cluster/data_work.hpp"
#include "cluster/frame_type.hpp"
#include "test_framework.hpp"

#include "dannan/contract/time_source.h"
#include "dannan/status/activity.h"

#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace macha;
using namespace std::chrono_literals;

namespace {

// Stands still until moved.
class Stepped final : public dannan::contract::TimeSource {
    std::atomic<std::chrono::steady_clock::rep> now_{
        (std::chrono::steady_clock::time_point{} + std::chrono::hours(1))
            .time_since_epoch()
            .count()};

  public:
    void advance(std::chrono::steady_clock::duration by) noexcept {
        now_.fetch_add(by.count());
    }
    std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(now_.load()));
    }
};

MACHA_FAST_TEST("work_class", test_every_frame_has_one_class) {
    CHECK(work_class(FrameType::control) == WorkClass::control);
    CHECK(work_class(FrameType::foreground) == WorkClass::foreground);
    CHECK(work_class(FrameType::read_ahead) == WorkClass::foreground);
    CHECK(work_class(FrameType::loader) == WorkClass::loader);
    CHECK(work_class(FrameType::speculative) == WorkClass::speculative);
}

MACHA_FAST_TEST("work_class", test_each_class_moves_data_on_its_own_frame) {
    for (const auto work :
         {WorkClass::control, WorkClass::foreground, WorkClass::loader, WorkClass::speculative})
        CHECK(work_class(frame_for(work)) == work);
    CHECK(frame_for(WorkClass::foreground) == FrameType::foreground);
}

MACHA_FAST_TEST("work_class", test_presence_is_per_class_and_bytes_per_frame) {
    Stepped time;
    dannan::status::ActivityClocks clocks(&time);
    for (const auto work :
         {WorkClass::control, WorkClass::foreground, WorkClass::loader, WorkClass::speculative})
        CHECK(clocks.idle_for(work) == 24h);

    // Either viewer frame is a viewer present; neither is a loader.
    clocks.note(FrameType::read_ahead, 100);
    CHECK(clocks.idle_for(WorkClass::foreground) == 0ms);
    CHECK(clocks.idle_for(WorkClass::loader) == 24h);
    CHECK(clocks.recently_active(WorkClass::foreground, 1s));
    time.advance(5s);
    clocks.note(FrameType::foreground, 200);
    CHECK(clocks.idle_for(WorkClass::foreground) == 0ms);

    // A loader is a loader, never a viewer.
    time.advance(5s);
    clocks.note(FrameType::loader, 300);
    CHECK(clocks.idle_for(WorkClass::loader) == 0ms);
    CHECK(clocks.idle_for(WorkClass::foreground) == 5s);

    // A request moves no bytes and is a viewer present all the same.
    time.advance(5s);
    clocks.note(WorkClass::foreground);
    CHECK(clocks.idle_for(WorkClass::foreground) == 0ms);

    // Bytes are still counted per frame.
    CHECK(clocks.take_bytes(FrameType::foreground) == 200);
    CHECK(clocks.take_bytes(FrameType::read_ahead) == 100);
    CHECK(clocks.take_bytes(FrameType::loader) == 300);
    CHECK(clocks.take_bytes(FrameType::foreground) == 0);
}

} // namespace

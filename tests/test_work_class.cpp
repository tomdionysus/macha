// SPDX-License-Identifier: GPL-3.0-or-later
//
// One definition of work class: every frame's class, and the activity clocks
// keyed by it.
#include "cluster/activity_clocks.hpp"
#include "cluster/data_work.hpp"
#include "cluster/frame_type.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <stdexcept>

using namespace macha;
using namespace std::chrono_literals;

namespace {

MACHA_FAST_TEST("work_class", test_every_frame_has_one_class) {
    CHECK(work_class(FrameType::control) == WorkClass::control);
    CHECK(work_class(FrameType::foreground) == WorkClass::viewer);
    CHECK(work_class(FrameType::read_ahead) == WorkClass::viewer);
    CHECK(work_class(FrameType::loader) == WorkClass::loader);
    CHECK(work_class(FrameType::speculative) == WorkClass::speculative);
}

MACHA_FAST_TEST("work_class", test_each_class_moves_data_on_its_own_frame) {
    for (const auto work : {WorkClass::control, WorkClass::viewer, WorkClass::loader,
                            WorkClass::speculative})
        CHECK(work_class(frame_for(work)) == work);
    CHECK(frame_for(WorkClass::viewer) == FrameType::foreground);
}

MACHA_FAST_TEST("work_class", test_presence_is_per_class_and_bytes_per_frame) {
    auto now = Clock::time_point{} + 1h;
    ActivityClocks clocks([&] { return now; });
    for (const auto work : {WorkClass::control, WorkClass::viewer, WorkClass::loader,
                            WorkClass::speculative})
        CHECK(clocks.idle_for(work) == 24h);

    // Either viewer frame is a viewer present; neither is a loader.
    clocks.note(FrameType::read_ahead, 100);
    CHECK(clocks.idle_for(WorkClass::viewer) == 0ms);
    CHECK(clocks.idle_for(WorkClass::loader) == 24h);
    CHECK(clocks.viewer_recently_active(1s));
    now += 5s;
    clocks.note(FrameType::foreground, 200);
    CHECK(clocks.idle_for(WorkClass::viewer) == 0ms);

    // A loader is a loader, never a viewer.
    now += 5s;
    clocks.note(FrameType::loader, 300);
    CHECK(clocks.idle_for(WorkClass::loader) == 0ms);
    CHECK(clocks.idle_for(WorkClass::viewer) == 5s);

    // A request moves no bytes and is a viewer present all the same.
    now += 5s;
    clocks.note(WorkClass::viewer);
    CHECK(clocks.idle_for(WorkClass::viewer) == 0ms);

    // Bytes are still counted per frame.
    CHECK(clocks.take_bytes(FrameType::foreground) == 200);
    CHECK(clocks.take_bytes(FrameType::read_ahead) == 100);
    CHECK(clocks.take_bytes(FrameType::loader) == 300);
    CHECK(clocks.take_bytes(FrameType::foreground) == 0);
}

MACHA_FAST_TEST("work_class", test_control_never_forms_data_work) {
    bool refused = false;
    try {
        DataWorkContext context(FrameType::control, 1024);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
}

} // namespace

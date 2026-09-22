// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "data_work.hpp"
#include "io_pressure.hpp"

#include <chrono>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

DataWorkContext work(FrameType frame_type) {
    return DataWorkContext(frame_type, 0,
                           DataWorkContext::Clock::now() + 2s);
}

MACHA_FAST_TEST("io_pressure", test_an_ordinary_large_write_is_not_a_slow_device) {
    // The bug this model exists to fix, first. A 4 MiB extent write taking
    // 200 ms is a spinning disk doing its job; under a flat 50 ms threshold it
    // was "pressure", so every storage node declared itself in trouble nine
    // seconds after boot and held an operator's import to one lease for an
    // afternoon. Judged against what an operation of that size should cost, it
    // is unremarkable.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 2000ms});
    for (int i = 0; i < 100; ++i)
        monitor.note(200ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
    // Expected for 4 MiB is 25 + 480 = 505 ms, so 200 ms is comfortably under.
    CHECK(monitor.sample().slowdown_percent < 100);

    // Small operations are judged on their own scale rather than swamped by
    // the large ones: 4 KiB taking 200 ms is a device in trouble.
    DiskServiceMonitor small(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 2000ms});
    for (int i = 0; i < 40; ++i)
        small.note(200ms, 4096);
    CHECK(small.pressured());
}

MACHA_FAST_TEST("io_pressure", test_a_device_far_slower_than_it_should_be_is_noticed) {
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 2000ms});
    CHECK(!monitor.pressured());

    for (int i = 0; i < 50; ++i)
        monitor.note(100ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());

    // The 17.7 s extent write that started all of this. Against fifty healthy
    // samples it moves the moving average from about 19% to 237% -- under the
    // 300% line, so the ratio alone would have let it pass. It trips the
    // absolute outlier instead, which is why that exists: a multi-second
    // operation is a device in trouble now, not a statistic.
    monitor.note(17700ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    CHECK(monitor.pressure_onsets() == 1);
    CHECK(monitor.sample().worst_us >= 17000000);

    // Sustained degradation keeps it there.
    for (int i = 0; i < 20; ++i)
        monitor.note(3000ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());

    // And recovery needs a run of healthy operations, not one lucky write.
    monitor.note(100ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    for (int i = 0; i < 80; ++i)
        monitor.note(80ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
}

MACHA_FAST_TEST("io_pressure", test_a_viewer_is_never_refused_because_the_disk_is_slow) {
    // The rule the whole stage exists for, and the one it would be worst to get
    // backwards: if the device is slow, the person waiting on it gets all of
    // it. Pressure may only ever refuse work nobody is waiting for.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 2000ms});
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);

    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // Viewers sail through, repeatedly, while the device is pressured.
    std::vector<DataResourceArbiter::Lease> viewers;
    for (int i = 0; i < 3; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::foreground), 1024 * 1024);
        REQUIRE(lease.has_value());
        viewers.push_back(std::move(*lease));
    }
    auto read_ahead = arbiter.try_acquire(work(FrameType::read_ahead), 1024 * 1024);
    CHECK(read_ahead.has_value());

    const auto stats = arbiter.stats();
    CHECK(stats.device_pressured);
    CHECK(stats.device_service_us > 50000);
}

MACHA_FAST_TEST("io_pressure", test_the_loader_runs_freely_under_pressure_with_no_viewer) {
    // Law 2: "Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would
    // Make The Viewer Wait." A slow device with nobody reading from it is a
    // device doing its job, and holding an operator's import back for it is the
    // violation the law names. It cost a 36 GB import an afternoon at 2 MB/s on
    // 2026-09-22 with nothing being watched.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 2000ms});
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // No viewer anywhere: the loader takes its full concurrency.
    std::vector<DataResourceArbiter::Lease> loaders;
    for (int i = 0; i < 4; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
        loaders.push_back(std::move(*lease));
    }

    // Speculative work stands aside regardless -- it sits below the loader and
    // nobody is waiting for it.
    auto speculative = arbiter.try_acquire(work(FrameType::speculative), 1024 * 1024);
    CHECK(!speculative.has_value());
}

MACHA_FAST_TEST("io_pressure", test_the_loader_yields_to_a_viewer_on_a_slow_device) {
    // The other half of law 2: once a viewer is in the picture, the loader does
    // yield, and to a trickle rather than a stop so an import still drains.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 2000ms});
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // A viewer holding credit is a viewer present.
    auto viewer = arbiter.try_acquire(work(FrameType::foreground), 1024 * 1024);
    REQUIRE(viewer.has_value());

    auto first = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    REQUIRE(first.has_value());
    auto second = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(!second.has_value());

    // Bounded, never stopped: releasing the trickle lease lets the next unit of
    // loader work through, so the import proceeds slowly rather than hanging.
    first.reset();
    auto next = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(next.has_value());

    // And the viewer keeps sailing through while all of that happens.
    auto another_viewer = arbiter.try_acquire(work(FrameType::read_ahead), 1024 * 1024);
    CHECK(another_viewer.has_value());

    // Once the viewer is gone and the device is still slow, the loader gets its
    // concurrency back: nothing is waiting on the disk but the import.
    viewer.reset();
    another_viewer.reset();
    auto unblocked = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(unblocked.has_value());
}

MACHA_FAST_TEST("io_pressure", test_with_no_device_admission_is_exactly_what_it_was) {
    // The mechanism is off unless a device is being watched, so nothing that
    // does not front a store changes behaviour, and io_pressure_target_ms: 0
    // restores the previous arbiter exactly.
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    std::vector<DataResourceArbiter::Lease> loaders;
    for (int i = 0; i < 6; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
        loaders.push_back(std::move(*lease));
    }
    const auto stats = arbiter.stats();
    CHECK(!stats.device_pressured);
    CHECK(stats.device_service_us == 0);
}

} // namespace

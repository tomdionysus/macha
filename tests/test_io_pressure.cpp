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

MACHA_FAST_TEST("io_pressure", test_a_slow_device_is_noticed_and_a_recovered_one_forgiven) {
    // The primitive that did not exist. Every other bound on DATA work is
    // declared up front; this one is derived from what the disk did.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{50ms, 20ms});
    CHECK(!monitor.pressured());

    // A fast device stays unpressured however many operations it serves.
    for (int i = 0; i < 100; ++i)
        monitor.note(2ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
    CHECK(monitor.sample().operations == 100);

    // A modest outlier is absorbed rather than gating the loader.
    monitor.note(200ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());

    // A catastrophic one gates it on its own, and that is the intent: a single
    // 4-second write means the device is already in trouble, and a write of
    // exactly that kind is what starved a viewer on 2026-09-19. This was the
    // one expectation in this file I got backwards first time -- the EWMA is
    // there to absorb noise, not to sit through a disaster.
    monitor.note(4000ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    CHECK(monitor.pressure_onsets() == 1);

    // It stays pressured while the device stays slow.
    for (int i = 0; i < 20; ++i)
        monitor.note(500ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    // The worst case is kept, because a mean of 40 ms hides the 17 s write that
    // broke a viewer on 2026-09-19.
    CHECK(monitor.sample().worst_us >= 4000000);

    // Hysteresis: crossing back under the target is not enough, it has to reach
    // the release floor, or a device sitting at the threshold makes the loader
    // stutter instead of yielding.
    monitor.note(30ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    // ~50 fast samples are needed to decay a half-second mean below the 20 ms
    // release floor at 1/16 weight, which is the point: the disk is not handed
    // back to the loader on one lucky write.
    for (int i = 0; i < 80; ++i)
        monitor.note(1ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
}

MACHA_FAST_TEST("io_pressure", test_a_viewer_is_never_refused_because_the_disk_is_slow) {
    // The rule the whole stage exists for, and the one it would be worst to get
    // backwards: if the device is slow, the person waiting on it gets all of
    // it. Pressure may only ever refuse work nobody is waiting for.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{50ms, 20ms});
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);

    for (int i = 0; i < 30; ++i)
        monitor.note(900ms, 4 * 1024 * 1024);
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

MACHA_FAST_TEST("io_pressure", test_the_loader_is_held_to_a_trickle_and_not_stopped) {
    // Law 2: bounded, never stopped. A loader that is itself the reason the
    // disk is busy must still drain, or the node deadlocks on its own
    // publication instead of merely slowing it down.
    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{50ms, 20ms});
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);

    // Unpressured, the loader takes several leases at once.
    std::vector<DataResourceArbiter::Lease> before;
    for (int i = 0; i < 4; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
        before.push_back(std::move(*lease));
    }
    before.clear();

    for (int i = 0; i < 30; ++i)
        monitor.note(900ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // Pressured, exactly one background lease is admitted -- the trickle --
    // and the second is refused while the first is held.
    auto first = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    REQUIRE(first.has_value());
    auto second = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(!second.has_value());
    auto speculative = arbiter.try_acquire(work(FrameType::speculative), 1024 * 1024);
    CHECK(!speculative.has_value());

    // Bytes were never the constraint: a viewer gets credit from the same pool
    // in the same moment. That is the distinction between this and
    // data_viewer_reserve_bytes, which was satisfied while a viewer waited 17 s.
    auto viewer = arbiter.try_acquire(work(FrameType::foreground), 1024 * 1024);
    CHECK(viewer.has_value());

    // Releasing the trickle lease lets the next unit of loader work in, so the
    // ingest completes at reduced throughput rather than hanging.
    first.reset();
    auto next = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(next.has_value());

    // And when the device recovers, the loader gets its concurrency back with
    // no intervention.
    for (int i = 0; i < 100; ++i)
        monitor.note(1ms, 4 * 1024 * 1024);
    REQUIRE(!monitor.pressured());
    auto recovered = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(recovered.has_value());
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "data_work.hpp"
#include "io_pressure.hpp"
#include "storage_pool.hpp"

#include <chrono>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// The shipped defaults, so a test that passes here says something about what
// the nodes actually run.
constexpr DiskServiceMonitor::Thresholds defaults{25ms, 120ms, 300, 150, 1000};

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
    DiskServiceMonitor monitor(defaults);
    for (int i = 0; i < 100; ++i)
        monitor.note(200ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
    // Expected for 4 MiB is 25 + 480 = 505 ms, so 200 ms is comfortably under.
    CHECK(monitor.sample().slowdown_percent < 100);

    // Small operations are judged on their own scale rather than swamped by
    // the large ones: 4 KiB taking 200 ms is a device in trouble.
    DiskServiceMonitor small(defaults);
    for (int i = 0; i < 40; ++i)
        small.note(200ms, 4096);
    CHECK(small.pressured());
}

MACHA_FAST_TEST("io_pressure", test_pressure_onset_and_release_are_each_logged_once) {
    // 2026-09-23: the only evidence of DATA pressure was the torrent clamp
    // line, which fired only with a viewer. Each transition is now one line,
    // and staying in a state says nothing more.
    struct Capture final : Logger {
        std::mutex mutex;
        std::vector<std::string> lines;
        bool enabled(LogLevel) const noexcept override { return true; }
        void log(LogLevel, const std::string& line) override {
            std::lock_guard lock(mutex);
            lines.push_back(line);
        }
        size_t count(std::string_view what) {
            std::lock_guard lock(mutex);
            return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [&](const auto& line) {
                return line.find(what) != std::string::npos;
            }));
        }
    };
    auto capture = std::make_shared<Capture>();
    Log::set_logger(capture);

    DiskServiceMonitor monitor(defaults);
    for (int i = 0; i < 20; ++i) monitor.note(100ms, 4 * 1024 * 1024);
    CHECK(capture->count("pressure") == 0);

    for (int i = 0; i < 20; ++i) monitor.note(3000ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    CHECK(capture->count("DATA device pressure onset") == 1);

    for (int i = 0; i < 200; ++i) monitor.note(10ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
    CHECK(capture->count("DATA device pressure released") == 1);
    CHECK(capture->count("DATA device pressure onset") == 1);

    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
}

MACHA_FAST_TEST("io_pressure", test_a_device_far_slower_than_it_should_be_is_noticed) {
    DiskServiceMonitor monitor(defaults);
    CHECK(!monitor.pressured());

    for (int i = 0; i < 50; ++i)
        monitor.note(100ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());

    // The 17.7 s extent write that started all of this. Against fifty healthy
    // samples it moves the moving average from about 19% to 237% -- under the
    // 300% line, so the average alone would have let it pass. It trips the
    // single-sample outlier instead, which is why that exists: one operation
    // 3,505% past its own expectation is a device in trouble now, not a
    // statistic.
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

MACHA_FAST_TEST("io_pressure", test_the_single_sample_trip_scales_with_the_operation) {
    // The outlier was an absolute 2 s until 0.53.0 and it was the last number
    // in this model that was a guess about hardware -- an unequal one, because
    // the same 2 s is 396% of expectation for a 4 MiB write and 8,000% of it
    // for a 4 KiB read. As a ratio it means the same thing at every size.

    // 2 s moving 32 MiB is 51% of expectation. It is a large operation on a
    // working device and must not trip anything. The old absolute threshold
    // tripped on it.
    DiskServiceMonitor large(defaults);
    for (int i = 0; i < 10; ++i)
        large.note(2000ms, 32 * 1024 * 1024);
    CHECK(!large.pressured());

    // 2 s for a 4 KiB read is 8,000% and trips on the first sample, as it did
    // before -- the change costs nothing at the small end.
    DiskServiceMonitor tiny(defaults);
    tiny.note(2000ms, 4096);
    CHECK(tiny.pressured());
    CHECK(tiny.pressure_onsets() == 1);

    // And the founding event still trips on its first sample, which is the
    // whole reason a single-sample trip exists beside the moving average.
    DiskServiceMonitor founding(defaults);
    for (int i = 0; i < 50; ++i)
        founding.note(100ms, 4 * 1024 * 1024);
    REQUIRE(!founding.pressured());
    founding.note(17700ms, 4 * 1024 * 1024);
    CHECK(founding.pressured());

    // Zero disables the trip and leaves the moving average alone in charge.
    auto no_trip = defaults;
    no_trip.outlier_percent = 0;
    DiskServiceMonitor averaged(no_trip);
    for (int i = 0; i < 50; ++i)
        averaged.note(100ms, 4 * 1024 * 1024);
    averaged.note(17700ms, 4 * 1024 * 1024);
    CHECK(!averaged.pressured());
}

MACHA_TEST("io_pressure", test_a_read_is_measured_with_the_bytes_it_returned) {
    // The defect this test exists for shipped with the model and survived two
    // rounds of corrections to it. StoragePool::get() started its timer with
    // zero bytes because a read's size is only known once it succeeds, and the
    // note_bytes() call that was supposed to fill it in was never written. So
    // every read was judged against the fixed 25 ms per-operation overhead
    // alone, with no per-size allowance at all -- the exact flat threshold the
    // whole model was built to replace, still live on the read path.
    //
    // On 2026-09-22 that made a healthy spinner serving 240 KB reads in 31 ms
    // score over 1000%, and es-1 entered and left pressure twelve times in
    // thirty-four minutes with nobody watching anything.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto state = t.path() / "state";
    auto backend = t.path() / "backend";
    std::filesystem::create_directories(backend);
    auto node = load_or_create_node_id(state);

    StoragePackingConfig no_packing;
    no_packing.threshold = 0;
    no_packing.target_size = 0;
    StoragePool pool(state, node, {{backend, 64ULL * 1024 * 1024, 0}}, keys.storage, 0ms,
                     no_packing);
    pool.configure_service_monitor(defaults);

    std::vector<ObjectId> ids;
    uint64_t written = 0;
    for (size_t i = 0; i < 8; ++i) {
        auto data = pattern(256 * 1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        written += data.size();
        ids.push_back(id);
    }

    const auto after_writes = pool.service_monitor().sample();
    CHECK(after_writes.bytes == written);

    uint64_t read_back = 0;
    for (const auto& id : ids) {
        auto data = pool.get(id);
        REQUIRE(data.has_value());
        read_back += data->size();
    }

    const auto after_reads = pool.service_monitor().sample();
    // The reads must have been charged their real size. Before the fix this
    // difference was exactly zero however much was read.
    CHECK(after_reads.bytes - after_writes.bytes == read_back);
    CHECK(after_reads.operations >= after_writes.operations + ids.size());
    // And a local temp-dir store serving quarter-megabyte objects is not a
    // device in trouble. It read as one when the size was dropped.
    CHECK(!pool.service_monitor().pressured());
}

MACHA_TEST("io_pressure", test_maintenance_reads_are_part_of_the_service_time_they_spend) {
    // Coverage, and the gap that hid the 2026-09-22 finding. Pool maintenance
    // reads its backends directly rather than through StoragePool::get(), so
    // the heaviest reader macha has was invisible to the signal that decides
    // the device is busy: macha-maint read 51.6 MB/s off a spindle sitting at
    // 91% utilisation and moved the measured service time not at all.
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto state = t.path() / "state";
    auto backend = t.path() / "backend";
    std::filesystem::create_directories(backend);
    auto node = load_or_create_node_id(state);

    StoragePackingConfig no_packing;
    no_packing.threshold = 0;
    no_packing.target_size = 0;
    StoragePool pool(state, node, {{backend, 64ULL * 1024 * 1024, 0}}, keys.storage, 0ms,
                     no_packing);
    pool.configure_service_monitor(defaults);

    uint64_t stored = 0;
    for (size_t i = 0; i < 8; ++i) {
        auto data = pattern(256 * 1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        stored += data.size();
    }

    const auto before = pool.service_monitor().sample();
    auto scrub = pool.scrub_step(0, 64, {});
    REQUIRE(scrub.bytes == stored);
    const auto after = pool.service_monitor().sample();
    CHECK(after.operations > before.operations);
    CHECK(after.bytes - before.bytes == stored);
}

MACHA_FAST_TEST("io_pressure", test_a_viewer_is_never_refused_because_the_disk_is_slow) {
    // The rule the whole stage exists for, and the one it would be worst to get
    // backwards: if the device is slow, the person waiting on it gets all of
    // it. Pressure may only ever refuse work nobody is waiting for.
    DiskServiceMonitor monitor(defaults);
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
    // A viewer is never refused, so no viewer ever contributes a refusal.
    CHECK(stats.pressure_refusals == 0);
}

MACHA_FAST_TEST("io_pressure", test_the_loader_runs_freely_under_pressure_with_no_viewer) {
    // Law 3: "Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would
    // Make The Viewer Wait." A slow device with nobody reading from it is a
    // device doing its job, and holding an operator's import back for it is the
    // violation the law names. It cost a 36 GB import an afternoon at 2 MB/s on
    // 2026-09-22 with nothing being watched.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    arbiter.observe_viewers([] { return false; });
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
    CHECK(arbiter.stats().pressure_refusals == 0);

    // Speculative work stands aside regardless -- it sits below the loader and
    // nobody is waiting for it.
    auto speculative = arbiter.try_acquire(work(FrameType::speculative), 1024 * 1024);
    CHECK(!speculative.has_value());
    // And that refusal is the mechanism's doing, so it is counted and an
    // operator can see what the throttle cost rather than infer it.
    CHECK(arbiter.stats().pressure_refusals == 1);
}

MACHA_FAST_TEST("io_pressure", test_the_loader_yields_to_a_viewer_on_a_slow_device) {
    // The other half of law 3: once a viewer is in the picture, the loader does
    // yield, and to a trickle rather than a stop so an import still drains.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    arbiter.observe_viewers([] { return false; });
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
    CHECK(arbiter.stats().pressure_refusals == 1);

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

MACHA_FAST_TEST("io_pressure", test_a_viewer_between_two_extents_is_still_a_viewer) {
    // "Viewer present" was byte credit held at this instant, and playback does
    // not hold credit between extents. So every gap in a stream readmitted the
    // loader at full concurrency onto a slow disk, and the viewer's next read
    // queued behind the extent write the gap had just let in. The rest of the
    // system answers this with an activity clock and a quiet window; the
    // arbiter now reads the same answer.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);

    bool watching = true;
    arbiter.observe_viewers([&] { return watching; });
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // Nobody is holding a single byte of viewer credit here -- the viewer is
    // between extents -- and the loader still yields to its trickle.
    auto first = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    REQUIRE(first.has_value());
    auto second = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(!second.has_value());

    // A viewer arriving mid-gap is still never refused.
    auto viewer = arbiter.try_acquire(work(FrameType::foreground), 1024 * 1024);
    CHECK(viewer.has_value());

    // Once the stream really has stopped -- the window lapses and the last
    // viewer credit goes with it -- the import takes the disk back. Law 3 is
    // not a licence to throttle for ever.
    watching = false;
    viewer.reset();
    auto resumed = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(resumed.has_value());
}

MACHA_FAST_TEST("io_pressure", test_background_work_always_drains_on_a_pressured_device) {
    // Law 4. A gate that can refuse every unit of background work on a device
    // that is slow *because of* that work has no way back, and the node stays
    // up and reports itself healthy the whole time. min_background is the
    // floor that makes recovery possible: with nothing active, one lease is
    // always admitted, it completes, it feeds the monitor, and the average can
    // fall. The signal can never starve itself of input.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    arbiter.observe_viewers([] { return true; });
    for (int i = 0; i < 200; ++i)
        monitor.note(30000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // Sustained catastrophic overload, a viewer present throughout: the
    // trickle still turns over, one lease at a time, indefinitely.
    for (int i = 0; i < 50; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
    }

    // And a device that recovers releases: the state describes now, not what
    // it once saw.
    for (int i = 0; i < 100; ++i)
        monitor.note(80ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
    std::vector<DataResourceArbiter::Lease> loaders;
    for (int i = 0; i < 4; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
        loaders.push_back(std::move(*lease));
    }
}

MACHA_FAST_TEST("io_pressure", test_control_cannot_enter_the_data_arbiter_at_all) {
    // Law 1. Control never queues behind or runs inline with bulk data work,
    // whatever the DATA devices are doing, and the structural guarantee is that it never
    // enters this object: the control store is a separate LocalStore on a
    // separate device, nothing on its path feeds the monitor, and an attempt
    // to take DATA credit for control work is a programming error rather than
    // a slow path.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    bool rejected = false;
    try {
        DataWorkContext control(FrameType::control);
        (void)control;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

MACHA_FAST_TEST("io_pressure", test_with_no_device_admission_is_exactly_what_it_was) {
    // The mechanism is off unless a device is being watched, so nothing that
    // does not front a store changes behaviour, and
    // io_pressure_slowdown_percent: 0 restores the previous arbiter exactly.
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
    CHECK(stats.pressure_refusals == 0);
}

} // namespace

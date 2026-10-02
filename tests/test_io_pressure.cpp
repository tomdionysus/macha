// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "cluster/data_work.hpp"
#include "storage/io_pressure.hpp"
#include "storage/storage_pool.hpp"

#include <chrono>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// The shipped defaults, so these tests exercise what the nodes run.
constexpr DiskServiceMonitor::Thresholds defaults{25ms, 120ms, 300, 150, 1000};

DataWorkContext work(FrameType frame_type) {
    return DataWorkContext(frame_type, 0,
                           DataWorkContext::Clock::now() + 2s);
}

MACHA_FAST_TEST("io_pressure", test_an_ordinary_large_write_is_not_a_slow_device) {
    // A 4 MiB write taking 200 ms is judged against what that size should
    // cost, and is unremarkable.
    DiskServiceMonitor monitor(defaults);
    for (int i = 0; i < 100; ++i)
        monitor.note(200ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
    // Expected for 4 MiB is 25 + 480 = 505 ms, so 200 ms is comfortably under.
    CHECK(monitor.sample().slowdown_percent < 100);

    // Small operations are judged on their own scale: 4 KiB taking 200 ms is
    // a device in trouble.
    DiskServiceMonitor small(defaults);
    for (int i = 0; i < 40; ++i)
        small.note(200ms, 4096);
    CHECK(small.pressured());
}

MACHA_FAST_TEST("io_pressure", test_pressure_onset_and_release_are_each_logged_once) {
    // Each transition logs one line; staying in a state logs nothing.
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

    // After fifty healthy samples a 17.7 s write lifts the moving average only
    // to about 237%, under the 300% line; at 3,505% of its own expectation it
    // trips the single-sample outlier instead.
    monitor.note(17700ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    CHECK(monitor.pressure_onsets() == 1);
    CHECK(monitor.sample().worst_us >= 17000000);

    // Sustained degradation keeps it there.
    for (int i = 0; i < 20; ++i)
        monitor.note(3000ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());

    // Recovery needs a run of healthy operations, not one.
    monitor.note(100ms, 4 * 1024 * 1024);
    CHECK(monitor.pressured());
    for (int i = 0; i < 80; ++i)
        monitor.note(80ms, 4 * 1024 * 1024);
    CHECK(!monitor.pressured());
}

MACHA_FAST_TEST("io_pressure", test_the_single_sample_trip_scales_with_the_operation) {
    // The outlier trip is a ratio of expectation, so it means the same at
    // every size. 2 s moving 32 MiB is 51% of expectation and must not trip.
    DiskServiceMonitor large(defaults);
    for (int i = 0; i < 10; ++i)
        large.note(2000ms, 32 * 1024 * 1024);
    CHECK(!large.pressured());

    // 2 s for a 4 KiB read is 8,000% and trips on the first sample.
    DiskServiceMonitor tiny(defaults);
    tiny.note(2000ms, 4096);
    CHECK(tiny.pressured());
    CHECK(tiny.pressure_onsets() == 1);

    // A 17.7 s write after a healthy run trips on its first sample.
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
    // A read's size is known only once it succeeds; StoragePool::get() must
    // still charge it, or reads are judged on per-operation overhead alone.
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
    // The reads were charged their real size.
    CHECK(after_reads.bytes - after_writes.bytes == read_back);
    CHECK(after_reads.operations >= after_writes.operations + ids.size());
    // A temp-dir store serving quarter-megabyte objects is not under pressure.
    CHECK(!pool.service_monitor().pressured());
}

MACHA_TEST("io_pressure", test_maintenance_reads_are_part_of_the_service_time_they_spend) {
    // Maintenance reads backends directly rather than through
    // StoragePool::get(); the monitor must still see them.
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
    // Pressure may only refuse work nobody is waiting for.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);

    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

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
    CHECK(stats.pressure_refusals == 0);
}

MACHA_FAST_TEST("io_pressure", test_the_loader_runs_freely_under_pressure_with_no_viewer) {
    // Law 3: the loader waits only if it would make a viewer wait. A slow
    // device with no viewer does not hold the loader back.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    arbiter.observe_viewers([] { return false; });
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    std::vector<DataResourceArbiter::Lease> loaders;
    for (int i = 0; i < 4; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
        loaders.push_back(std::move(*lease));
    }
    CHECK(arbiter.stats().pressure_refusals == 0);

    // Speculative work, below the loader, still stands aside.
    auto speculative = arbiter.try_acquire(work(FrameType::speculative), 1024 * 1024);
    CHECK(!speculative.has_value());
    // Refusals are counted so the throttle's cost is visible.
    CHECK(arbiter.stats().pressure_refusals == 1);
}

MACHA_FAST_TEST("io_pressure", test_the_loader_yields_to_a_viewer_on_a_slow_device) {
    // With a viewer present the loader yields to a trickle, not a stop.
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

    // Releasing the trickle lease lets the next unit of loader work through.
    first.reset();
    auto next = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(next.has_value());

    auto another_viewer = arbiter.try_acquire(work(FrameType::read_ahead), 1024 * 1024);
    CHECK(another_viewer.has_value());

    // With the viewer gone, the loader gets its concurrency back on the still
    // slow device.
    viewer.reset();
    another_viewer.reset();
    auto unblocked = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(unblocked.has_value());
}

MACHA_FAST_TEST("io_pressure", test_a_viewer_between_two_extents_is_still_a_viewer) {
    // Playback holds no credit between extents, so viewer presence comes from
    // the activity signal passed to observe_viewers(), not from held credit.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);

    bool watching = true;
    arbiter.observe_viewers([&] { return watching; });
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // No viewer credit is held, yet the loader still yields to its trickle.
    auto first = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    REQUIRE(first.has_value());
    auto second = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(!second.has_value());

    // A viewer arriving mid-gap is still never refused.
    auto viewer = arbiter.try_acquire(work(FrameType::foreground), 1024 * 1024);
    CHECK(viewer.has_value());

    // Once the stream has stopped, the loader takes the disk back.
    watching = false;
    viewer.reset();
    auto resumed = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
    CHECK(resumed.has_value());
}

MACHA_FAST_TEST("io_pressure", test_background_work_always_drains_on_a_pressured_device) {
    // Law 4. min_background always admits one lease when nothing is active, so
    // background work keeps feeding the monitor and the average can fall.
    DiskServiceMonitor monitor(defaults);
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 8, 500ms);
    arbiter.observe_device(&monitor, 1);
    arbiter.observe_viewers([] { return true; });
    for (int i = 0; i < 200; ++i)
        monitor.note(30000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    // Under sustained overload with a viewer present, the trickle still turns
    // over one lease at a time.
    for (int i = 0; i < 50; ++i) {
        auto lease = arbiter.try_acquire(work(FrameType::loader), 1024 * 1024);
        REQUIRE(lease.has_value());
    }

    // A recovered device releases pressure.
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
    // Law 1. Control work never enters the DATA arbiter: constructing a
    // DataWorkContext for it throws.
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
    // Without an observed device, pressure never refuses anything.
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

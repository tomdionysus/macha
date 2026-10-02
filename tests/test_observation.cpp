// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "observation.hpp"
#include "test_support.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

HistogramSnapshot recorded(std::initializer_list<uint64_t> values) {
    LatencyHistogram histogram;
    for (auto value : values)
        histogram.record(value);
    return histogram.snapshot();
}

MACHA_FAST_TEST("observation", test_observation_buckets_tile_the_value_range) {
    constexpr auto top = std::numeric_limits<uint64_t>::max();
    CHECK(observation_bucket_lower(0) == 0);
    CHECK(observation_bucket_upper(observation_bucket_count - 1) == top);
    CHECK(observation_bucket(top) == observation_bucket_count - 1);
    for (size_t bucket = 0; bucket < observation_bucket_count; ++bucket) {
        const auto lower = observation_bucket_lower(bucket);
        const auto upper = observation_bucket_upper(bucket);
        CHECK(lower <= upper);
        CHECK(observation_bucket(lower) == bucket);
        CHECK(observation_bucket(upper) == bucket);
        if (bucket + 1 < observation_bucket_count)
            CHECK(observation_bucket_lower(bucket + 1) == upper + 1);
        // Relative width: exact below 8, at most an eighth above.
        if (bucket < 8)
            CHECK(lower == upper);
        else
            CHECK(upper - lower + 1 <= lower / 8);
    }
}

MACHA_FAST_TEST("observation", test_observation_bucket_contains_every_small_value) {
    size_t previous = 0;
    for (uint64_t value = 0; value <= (1U << 20); ++value) {
        const auto bucket = observation_bucket(value);
        REQUIRE(bucket >= previous);
        REQUIRE(observation_bucket_lower(bucket) <= value);
        REQUIRE(value <= observation_bucket_upper(bucket));
        previous = bucket;
    }
    for (unsigned shift = 3; shift < 64; ++shift) {
        const auto power = uint64_t{1} << shift;
        for (auto value : {power - 1, power, power + 1}) {
            const auto bucket = observation_bucket(value);
            CHECK(observation_bucket_lower(bucket) <= value);
            CHECK(value <= observation_bucket_upper(bucket));
        }
    }
}

MACHA_FAST_TEST("observation", test_observation_quantiles) {
    CHECK(HistogramSnapshot{}.quantile(0.5) == 0);

    const auto small = recorded({1, 2, 3, 4, 5, 6, 7});
    CHECK(small.count == 7);
    CHECK(small.sum == 28);
    CHECK(small.max == 7);
    CHECK(small.quantile(0.0) == 1);
    CHECK(small.quantile(-1.0) == 1);
    CHECK(small.quantile(0.5) == 4);
    CHECK(small.quantile(1.0) == 7);
    CHECK(small.quantile(2.0) == 7);

    // Above 8 a quantile is its bucket's upper bound, never past the maximum.
    const auto wide = recorded({100, 1000, 1001});
    CHECK(wide.quantile(0.3) == observation_bucket_upper(observation_bucket(100)));
    CHECK(wide.quantile(1.0) == 1001);

    // A hand-built snapshot whose count exceeds its buckets answers max.
    HistogramSnapshot inconsistent;
    inconsistent.count = 3;
    inconsistent.max = 42;
    inconsistent.buckets[1] = 1;
    CHECK(inconsistent.quantile(1.0) == 42);
}

MACHA_FAST_TEST("observation", test_observation_snapshot_windows) {
    LatencyHistogram histogram;
    histogram.record(5);
    histogram.record(900);
    const auto first = histogram.snapshot();
    histogram.record(6);
    histogram.record(7);
    const auto second = histogram.snapshot();

    const auto window = second.since(first);
    CHECK(window.count == 2);
    CHECK(window.sum == 13);
    CHECK(window.buckets[6] == 1);
    CHECK(window.buckets[7] == 1);
    // The cumulative max is 900; the window's highest value is 7.
    CHECK(window.max == 7);

    const auto empty = second.since(second);
    CHECK(empty.count == 0);
    CHECK(empty.max == 0);

    // Reversed operands saturate at zero rather than wrapping.
    const auto reversed = first.since(second);
    CHECK(reversed.count == 0);
    CHECK(reversed.sum == 0);
}

MACHA_FAST_TEST("observation", test_observation_histogram_records_from_many_threads) {
    LatencyHistogram histogram;
    std::vector<std::thread> threads;
    for (uint64_t thread = 0; thread < 4; ++thread)
        threads.emplace_back([&histogram, thread] {
            for (uint64_t value = 1; value <= 20'000; ++value)
                histogram.record(value * 4 + thread);
        });
    for (auto& thread : threads)
        thread.join();
    const auto snapshot = histogram.snapshot();
    CHECK(snapshot.count == 80'000);
    CHECK(snapshot.max == 80'003);
    uint64_t expected_sum = 0;
    for (uint64_t thread = 0; thread < 4; ++thread)
        for (uint64_t value = 1; value <= 20'000; ++value)
            expected_sum += value * 4 + thread;
    CHECK(snapshot.sum == expected_sum);
}

MACHA_FAST_TEST("observation", test_observation_registry_is_bounded) {
    Observations registry(2, 2);
    auto& first = registry.histogram("a");
    CHECK(&registry.histogram("a") == &first);
    registry.record("b", 3);
    registry.record("c", 4); // past the bound: folded into the overflow series
    registry.add("x");
    registry.add("y", 2);
    registry.add("z", 5); // past the bound
    CHECK(&registry.counter("x") == &registry.counter("x"));

    const auto snapshot = registry.snapshot();
    CHECK(snapshot.histograms.contains("a"));
    CHECK(snapshot.histograms.contains("b"));
    CHECK(!snapshot.histograms.contains("c"));
    CHECK(snapshot.histograms.at(std::string(Observations::overflow_series)).count == 1);
    CHECK(snapshot.counters.at("x") == 1);
    CHECK(snapshot.counters.at("y") == 2);
    CHECK(!snapshot.counters.contains("z"));
    CHECK(snapshot.counters.at(std::string(Observations::overflow_series)) == 2);
    CHECK(snapshot.counters.at(std::string(Observations::events_dropped_series)) == 0);
}

MACHA_FAST_TEST("observation", test_observation_events_are_bounded_oldest_first) {
    Observations registry(4, 2);
    for (uint64_t at = 1; at <= 3; ++at)
        registry.event({at, "e" + std::to_string(at), {}, {}});
    const auto events = registry.drain_events();
    REQUIRE(events.size() == 2);
    CHECK(events[0].name == "e2");
    CHECK(events[1].name == "e3");
    CHECK(registry.drain_events().empty());
    CHECK(registry.snapshot().counters.at(std::string(Observations::events_dropped_series)) == 1);
}

MACHA_FAST_TEST("observation", test_observation_registry_windows) {
    Observations registry;
    registry.add("kept", 3);
    registry.record("latency", 10);
    const auto first = registry.snapshot();
    registry.add("kept", 2);
    registry.add("new", 7);
    registry.record("latency", 11);
    const auto window = registry.snapshot().since(first);
    CHECK(window.counters.at("kept") == 2);
    CHECK(window.counters.at("new") == 7);
    CHECK(window.histograms.at("latency").count == 1);
    // A series the earlier snapshot lacks counts from zero.
    const auto all = registry.snapshot().since(ObservationSnapshot{});
    CHECK(all.histograms.at("latency").count == 2);
}

MACHA_FAST_TEST("observation", test_observation_process_registry_and_durations) {
    CHECK(&observations() == &observations());
    LatencyHistogram histogram;
    {
        ObservedDuration timed(histogram);
        std::this_thread::sleep_for(2ms);
    }
    const auto snapshot = histogram.snapshot();
    CHECK(snapshot.count == 1);
    CHECK(snapshot.max >= 2000);
    CHECK(elapsed_us(Clock::now() + 1h) == 0);
}

MACHA_FAST_TEST("observation", test_observation_route_labels) {
    const std::vector<std::tuple<std::string, std::string, std::string>> cases{
        {"GET", "/api/v1/status", "api GET /api/v1/status"},
        {"POST", "/api/v1/torrents/jobs", "api POST /api/v1/torrents/jobs"},
        {"PUT", "/api/v1/catalogue/items/3f9a0c", "api PUT /api/v1/catalogue/items/:id"},
        {"PATCH", "/api/v1/playback/sessions/abc/segments/12.ts",
         "api PATCH /api/v1/playback/sessions/abc"},
        {"DELETE", "/api/v1/a//b", "api DELETE /api/v1/a/:id/b"},
        {"GET", "/api/v1/" + std::string(33, 'a'), "api GET /api/v1/:id"},
        {"GET", "/api/v1/Status", "api GET /api/v1/:id"},
        {"GET", "/api/v1/~status", "api GET /api/v1/:id"},
        {"GET", "/api/v1/media_information/x-y", "api GET /api/v1/media_information/x-y"},
        {"GET", "/api/v1/", "api GET /api/v1"},
        {"GET", "/api/v2/status", "api GET /api/:other"},
        {"HEAD", "/index.html", "api OTHER web"},
        {"GET", "/", "api GET web"},
    };
    for (const auto& [method, path, label] : cases)
        CHECK(observation_route_label(method, path) == label);
}

MACHA_FAST_TEST("observation", test_observation_window_rendering) {
    ObservationSnapshot window;
    window.histograms["idle"] = HistogramSnapshot{};
    window.histograms["x"] = recorded({3, 3, 5});
    window.counters["moved"] = 4;
    window.counters["still"] = 0;
    const auto line =
        render_observation_window(1000, 61000, "0.74.0", window, {{"rss_bytes", 7}});
    CHECK(line ==
          R"({"counters":{"moved":4},"end_ms":61000,"gauges":{"rss_bytes":7},)"
          R"("histograms":{"x":{"buckets":[[3,2],[5,1]],"count":3,"max":5,"p50":3,)"
          R"("p90":5,"p99":5,"sum":11}},"kind":"window","start_ms":1000,"version":"0.74.0"})");
    CHECK(Json::parse(line).isObject());
}

MACHA_FAST_TEST("observation", test_observation_event_rendering) {
    const ObservationEvent event{5, "backend_online", {{"elapsed_ms", 12}},
                                 {{"path", "/mnt/\"d\""}}};
    CHECK(render_observation_event(event) ==
          R"({"at_ms":5,"event":"backend_online","fields":{"elapsed_ms":12,)"
          R"("path":"/mnt/\"d\""},"kind":"event"})");
}

MACHA_FAST_TEST("observation", test_observation_log_rotates_at_its_bound) {
    TempDir dir;
    const auto path = dir.path() / "nested" / "observations.jsonl";
    ObservationLog log(path, 10);
    log.append("12345");  // creates the directory and the file (6 bytes)
    log.append("abc");    // 6 + 4 = 10: fits
    log.append("more");   // 10 + 5 > 10: rotates first
    CHECK(read_lines(path) == std::vector<std::string>{"more"});
    auto rotated = path;
    rotated += ".1";
    CHECK(read_lines(rotated) == (std::vector<std::string>{"12345", "abc"}));
    log.append("0123456789"); // larger than the bound on its own: rotates, then written
    CHECK(read_lines(path) == std::vector<std::string>{"0123456789"});
    CHECK(read_lines(rotated) == std::vector<std::string>{"more"});

    // An existing empty file is appended to, never rotated.
    const auto empty = dir.path() / "empty.jsonl";
    { std::ofstream touch(empty); }
    ObservationLog empty_log(empty, 1);
    empty_log.append("first");
    CHECK(read_lines(empty) == std::vector<std::string>{"first"});
    auto empty_rotated = empty;
    empty_rotated += ".1";
    CHECK(!std::filesystem::exists(empty_rotated));

    // The newline counts: 6 + 4 characters + 1 newline is one byte over.
    const auto edge = dir.path() / "edge.jsonl";
    ObservationLog edge_log(edge, 10);
    edge_log.append("12345");
    edge_log.append("abcd");
    CHECK(read_lines(edge) == std::vector<std::string>{"abcd"});
}

MACHA_FAST_TEST("observation", test_observation_log_survives_an_unwritable_path) {
    TempDir dir;
    const auto path = dir.path() / "observations.jsonl";
    // Count the DEBUG lines the log writes: one per run of failures.
    struct Counting final : Logger {
        std::atomic<int> lines{};
        bool enabled(LogLevel) const noexcept override {
            return true;
        }
        void log(LogLevel, const std::string& message) override {
            if (message.starts_with("observation: cannot write"))
                ++lines;
        }
    };
    auto counting = std::make_shared<Counting>();
    Log::set_logger(counting);

    std::filesystem::create_directories(path); // a directory where the file should be
    ObservationLog log(path, 1024);
    CHECK(log.path() == path);
    log.append("lost");
    log.append("lost again");
    CHECK(counting->lines == 1);
    std::filesystem::remove(path);
    log.append("kept");
    CHECK(read_lines(path) == std::vector<std::string>{"kept"});
    std::filesystem::remove(path);
    std::filesystem::create_directories(path);
    log.append("lost after a success");
    CHECK(counting->lines == 2);
    Log::set_logger(std::make_shared<ConsoleLogger>());
}

MACHA_FAST_TEST("observation", test_observation_recorder_writes_windows_and_events) {
    TempDir dir;
    const auto path = dir.path() / "observations.jsonl";
    Observations registry;
    auto log = std::make_shared<ObservationLog>(path, 1 << 20);
    int samples = 0;
    ObservationRecorder recorder(registry, log, "0.74.0", 1h, [&] {
        if (++samples == 2)
            throw std::runtime_error("sampler failed");
        return std::map<std::string, uint64_t>{{"sample", static_cast<uint64_t>(samples)}};
    });
    registry.add("n", 2);
    registry.event({7, "startup", {{"ready_ms", 3}}, {}});
    recorder.tick(100);
    registry.add("n", 1);
    recorder.tick(200);

    const auto lines = read_lines(path);
    REQUIRE(lines.size() == 3);
    CHECK(Json::parse(lines[0]).find("event")->asString() == "startup");
    const auto first = Json::parse(lines[1]);
    CHECK(first.find("counters")->find("n")->asUInt64() == 2);
    CHECK(first.find("gauges")->find("sample")->asUInt64() == 1);
    CHECK(first.find("end_ms")->asUInt64() == 100);
    const auto second = Json::parse(lines[2]);
    CHECK(second.find("counters")->find("n")->asUInt64() == 1);
    CHECK(second.find("gauges")->asObject().empty());
    CHECK(second.find("start_ms")->asUInt64() == 100);

    // Only a std::exception from the sampler is absorbed; anything else leaves
    // tick() for the supervised loop to record.
    ObservationRecorder strange(registry, log, "v", 1h, []() -> std::map<std::string, uint64_t> {
        throw 42;
    });
    bool propagated = false;
    try {
        strange.tick(300);
    } catch (int) {
        propagated = true;
    }
    CHECK(propagated);
    CHECK(read_lines(path).size() == 3);
}

MACHA_FAST_TEST("observation", test_observation_recorder_thread_and_final_window) {
    TempDir dir;
    const auto path = dir.path() / "observations.jsonl";
    Observations registry;
    auto log = std::make_shared<ObservationLog>(path, 1 << 20);
    {
        ObservationRecorder never_started(registry, log, "v", 1ms, {});
        never_started.stop();
    }
    CHECK(!std::filesystem::exists(path));

    ObservationRecorder recorder(registry, log, "v", 5ms, {});
    recorder.start();
    const auto deadline = Clock::now() + 10s;
    while (read_lines(path).size() < 2 && Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    const auto periodic = read_lines(path).size();
    CHECK(periodic >= 2);
    registry.event({1, "shutdown", {}, {}});
    recorder.stop();
    const auto lines = read_lines(path);
    REQUIRE(lines.size() >= periodic + 2);
    CHECK(Json::parse(lines[lines.size() - 2]).find("event")->asString() == "shutdown");
    recorder.stop(); // a second stop writes nothing
    CHECK(read_lines(path).size() == lines.size());
}

MACHA_TEST("observation", test_a_service_writes_its_lifecycle_to_the_observation_file) {
    TestService fixture("observation-lifecycle");
    const auto path = fixture.config().state_path / "observation" / "observations.jsonl";
    auto& service = fixture.start();
    service.stop();

    std::vector<std::string> events;
    size_t windows = 0;
    for (const auto& line : read_lines(path)) {
        const auto record = Json::parse(line);
        if (record.find("kind")->asString() == "event") {
            events.push_back(record.find("event")->asString());
        } else {
            ++windows;
            CHECK(record.find("version")->asString() == kBuildIdentity);
            CHECK(record.find("gauges")->find("rss_bytes") != nullptr);
        }
    }
    // A backend coming online during construction may precede "start".
    const auto at = [&](std::string_view name) {
        return std::find(events.begin(), events.end(), name) - events.begin();
    };
    REQUIRE(at("shutdown") < static_cast<std::ptrdiff_t>(events.size()));
    CHECK(at("start") < at("services_ready"));
    CHECK(at("services_ready") < at("shutdown"));
    CHECK(events.back() == "shutdown");
    CHECK(windows >= 1);

    // A second stop, as the fixture's destructor makes, writes nothing more.
    const auto written = read_lines(path).size();
    service.stop();
    CHECK(read_lines(path).size() == written);
    CHECK(observations().drain_events().empty());
}

} // namespace

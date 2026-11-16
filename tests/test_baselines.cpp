// SPDX-License-Identifier: GPL-3.0-or-later
//
// In-suite microbenchmarks for the observation probes' own cost. Each prints one line,
// "BENCH name=<name> ns_per_op=<n> ops=<n>"; run with
// `--filter baseline --verbose`. They assert only that the timed operation
// did its work.
#include "observation.hpp"
#include "test_backend_support.hpp"

#include <iostream>
#include <limits>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

void report(std::string_view name, Clock::duration elapsed, uint64_t ops) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    std::cout << "BENCH name=" << name
              << " ns_per_op=" << (ops ? ns / static_cast<int64_t>(ops) : 0) << " ops=" << ops
              << '\n';
}

MACHA_FAST_TEST("baseline", test_baseline_observation_probe_costs) {
    constexpr uint64_t ops = 1'000'000;
    Observations registry;
    auto& histogram = registry.histogram("bench");
    auto& counter = registry.counter("bench");

    auto started = Clock::now();
    for (uint64_t i = 0; i < ops; ++i)
        histogram.record(i);
    report("observation.histogram_record", Clock::now() - started, ops);

    started = Clock::now();
    for (uint64_t i = 0; i < ops; ++i)
        counter.fetch_add(1, std::memory_order_relaxed);
    report("observation.counter_add", Clock::now() - started, ops);

    started = Clock::now();
    uint64_t sink = 0;
    for (uint64_t i = 0; i < ops; ++i)
        sink += elapsed_us(started);
    report("observation.elapsed_us", Clock::now() - started, ops);

    // The HTTP probe's whole cost: label, lookup under the registry mutex, record.
    constexpr uint64_t requests = 100'000;
    started = Clock::now();
    for (uint64_t i = 0; i < requests; ++i)
        registry.record(observation_route_label("GET", "/api/v1/catalogue/items/3f9a0c"), i);
    report("observation.http_request_probe", Clock::now() - started, requests);

    CHECK(histogram.snapshot().count == ops);
    CHECK(counter.load() == ops);
    CHECK(registry.snapshot().histograms.at("api GET /api/v1/catalogue/items/:id").count ==
          requests);
    CHECK(sink != std::numeric_limits<uint64_t>::max());
}

} // namespace

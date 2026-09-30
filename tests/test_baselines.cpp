// SPDX-License-Identifier: GPL-3.0-or-later
//
// In-suite microbenchmarks: the object ledger experiment's T0 baselines for
// the costs later steps must not raise (the claim walk's per-object cost,
// has() on the DATA store) and for the cost of T0's own probes. Each prints
// one line, "BENCH name=<name> ns_per_op=<n> ops=<n>"; run them with
// `--filter baseline --verbose` to read the figures. They assert only that
// the operation did what it was timed doing.
#include "observation.hpp"
#include "service/claim_walk.hpp"
#include "storage/retention.hpp"
#include "storage/retention_ledger.hpp"
#include "storage/storage_pool.hpp"
#include "test_backend_support.hpp"

#include <cstring>
#include <iostream>
#include <limits>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

constexpr size_t object_count = 2000;

// 4 KiB of pattern with its first eight bytes replaced by n, so every n
// names a distinct object.
Bytes numbered(uint64_t n) {
    auto bytes = pattern(4096);
    std::memcpy(bytes.data(), &n, sizeof(n));
    return bytes;
}

void report(std::string_view name, Clock::duration elapsed, uint64_t ops) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    std::cout << "BENCH name=" << name << " ns_per_op=" << (ops ? ns / static_cast<int64_t>(ops) : 0)
              << " ops=" << ops << '\n';
}

// A one-backend DATA pool holding object_count small objects, and the ids of
// as many objects it does not hold.
struct Populated {
    TempDir dir;
    std::unique_ptr<StoragePool> pool;
    std::vector<ObjectId> present;
    std::vector<ObjectId> absent;
    ClusterKeys keys;

    Populated() {
        const auto keyfile = dir.path() / "key";
        write_key(keyfile);
        keys = load_cluster_keys(keyfile);
        const auto backend = dir.path() / "data";
        std::filesystem::create_directories(backend);
        pool = std::make_unique<StoragePool>(
            dir.path() / "pool-state", random_node_id(),
            std::vector<StorageBackendConfig>{{backend, 1ULL << 30}}, keys.storage);
        REQUIRE(wait_until([&] { return pool->online_backends() == 1; }));
        for (size_t i = 0; i < object_count; ++i) {
            const auto bytes = numbered(i);
            const auto id = object_id(bytes);
            REQUIRE(pool->put(id, bytes));
            present.push_back(id);
            absent.push_back(object_id(numbered(i + object_count)));
        }
    }
};

MACHA_TEST("baseline", test_baseline_data_store_has) {
    Populated store;
    constexpr int rounds = 20;
    size_t found = 0;
    auto started = Clock::now();
    for (int round = 0; round < rounds; ++round)
        for (const auto& id : store.present)
            found += store.pool->has(id) ? 1 : 0;
    report("data_store.has.present", Clock::now() - started, rounds * object_count);
    CHECK(found == rounds * object_count);

    size_t missing = 0;
    started = Clock::now();
    for (int round = 0; round < rounds; ++round)
        for (const auto& id : store.absent)
            missing += store.pool->has(id) ? 0 : 1;
    report("data_store.has.absent", Clock::now() - started, rounds * object_count);
    CHECK(missing == rounds * object_count);
}

// The maintenance loop's claim walk: next_retained, then has() on the store
// the claim's class names, per object (src/service/service.cpp).
MACHA_TEST("baseline", test_baseline_claim_walk_per_object) {
    Populated store;
    RetentionStore retention(store.dir.path() / "retention", store.keys.storage);
    retention.retain_batch(RetentionClass::data, store.present, {random_node_id(), 1});
    constexpr int passes = 10;
    size_t walked = 0;
    size_t held = 0;
    const auto started = Clock::now();
    for (int pass = 0; pass < passes; ++pass) {
        std::optional<ObjectId> cursor;
        for (;;) {
            bool complete = false;
            auto id = retention.next_retained(RetentionClass::data, cursor, complete);
            if (!id)
                break;
            ++walked;
            held += store.pool->has(*id) ? 1 : 0;
        }
    }
    report("claim_walk.per_object", Clock::now() - started, walked);
    CHECK(walked == passes * object_count);
    CHECK(held == walked);
}

// The same walk through the contracts: ClaimWalk over a RetentionLedger, in
// its 16-claim steps.
MACHA_TEST("baseline", test_baseline_claim_walk_on_the_ledger_per_object) {
    Populated store;
    RetentionStore retention(store.dir.path() / "retention", store.keys.storage);
    retention.retain_batch(RetentionClass::data, store.present, {random_node_id(), 1});
    const RetentionLedger ledger(retention, *store.pool, *store.pool);
    struct Unreached final : ClaimRestorer {
        size_t calls{};
        Outcome restore(RetentionClass, const ObjectId&) override {
            ++calls;
            return Outcome::not_restored;
        }
    } restorer;
    constexpr int passes = 10;
    size_t walked = 0;
    ClaimWalk walk(RetentionClass::data);
    const auto started = Clock::now();
    for (int pass = 0; pass < passes; ++pass)
        for (;;) {
            const auto step = walk.step(ledger, restorer);
            walked += step.examined;
            if (!step.unfinished)
                break;
        }
    report("claim_walk.ledger_per_object", Clock::now() - started, walked);
    CHECK(walked == passes * object_count);
    CHECK(restorer.calls == 0u);
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

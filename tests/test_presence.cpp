// SPDX-License-Identifier: GPL-3.0-or-later
//
// Authoritative presence (the object ledger plan, P). PresenceIndex is a
// primitive and is tested exhaustively against a reference model; the store
// and the pool are tested for what the plan requires of has(): it never
// reports an object a put is still writing, never waits for that put, never
// consults the device once warm-up has finished, and is exact across a
// restart and across a backend going offline and being re-adopted.
#include "storage/local_store.hpp"
#include "storage/presence_index.hpp"
#include "storage/storage_pool.hpp"
#include "test_backend_support.hpp"

#include <cstring>
#include <functional>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

// ---- PresenceIndex against its model ---------------------------------------

struct ModelId {
    bool present{};
    bool pruned{};
    bool forgotten{};
};

struct Model {
    std::array<ModelId, 2> ids{};
    bool authoritative{};

    PresenceIndex::Answer answer(size_t index) const {
        if (ids[index].present)
            return PresenceIndex::Answer::present;
        return authoritative ? PresenceIndex::Answer::absent : PresenceIndex::Answer::unknown;
    }
};

enum class Op { installed, observed, forgotten, pruned, listed_one, listed_both, warmed };
constexpr std::array ops{Op::installed, Op::observed, Op::forgotten, Op::pruned,
                         Op::listed_one, Op::listed_both, Op::warmed};

// Applies one event to both the index and the model, the model written from
// the table in presence_index.hpp rather than from the implementation.
void apply(PresenceIndex& index, Model& model, Op op, size_t target,
           const std::array<ObjectId, 2>& ids) {
    auto& id = model.ids[target];
    switch (op) {
    case Op::installed:
        index.installed(ids[target]);
        id.present = true;
        id.pruned = false;
        break;
    case Op::observed:
        index.observed(ids[target]);
        id.present = true;
        break;
    case Op::forgotten:
        index.forgotten(ids[target]);
        id.present = false;
        if (!model.authoritative)
            id.forgotten = true;
        break;
    case Op::pruned:
        index.pruned(ids[target]);
        id.present = false;
        id.pruned = true;
        if (!model.authoritative)
            id.forgotten = true;
        break;
    case Op::listed_one: {
        const std::array one{ids[target]};
        index.listed(one);
        if (!id.pruned && !id.forgotten)
            id.present = true;
        break;
    }
    case Op::listed_both:
        index.listed(ids);
        for (auto& each : model.ids)
            if (!each.pruned && !each.forgotten)
                each.present = true;
        break;
    case Op::warmed:
        index.warmed();
        model.authoritative = true;
        for (auto& each : model.ids)
            each.forgotten = false;
        break;
    }
}

MACHA_FAST_TEST("presence", test_presence_index_matches_its_model_over_every_short_history) {
    std::array<ObjectId, 2> ids{};
    ids[0].bytes[0] = 1;
    ids[1].bytes[0] = 2;
    // Every event, on either id, in every order, up to five events long:
    // (7 x 2)^5 histories, each checked after every event.
    constexpr size_t choices = ops.size() * 2;
    constexpr size_t length = 5;
    size_t histories = 1;
    for (size_t i = 0; i < length; ++i)
        histories *= choices;
    size_t checked = 0;
    for (size_t history = 0; history < histories; ++history) {
        PresenceIndex index;
        Model model;
        auto code = history;
        for (size_t step = 0; step < length; ++step) {
            const auto choice = code % choices;
            code /= choices;
            apply(index, model, ops[choice / 2], choice % 2, ids);
            for (size_t target = 0; target < 2; ++target)
                REQUIRE(index.answer(ids[target]) == model.answer(target));
            REQUIRE(index.authoritative() == model.authoritative);
            REQUIRE(index.size() ==
                    static_cast<size_t>(model.ids[0].present) + model.ids[1].present);
            ++checked;
        }
    }
    CHECK(checked == histories * length);
}

MACHA_FAST_TEST("presence", test_presence_index_warmup_race_cases_by_name) {
    ObjectId removed{};
    removed.bytes[0] = 7;
    const std::array listing{removed};

    // Removed after warm-up listed it and before the listing was published.
    PresenceIndex index;
    index.observed(removed);
    index.forgotten(removed);
    index.listed(listing);
    CHECK(index.answer(removed) == PresenceIndex::Answer::unknown);
    index.warmed();
    CHECK(index.answer(removed) == PresenceIndex::Answer::absent);

    // Put again after that removal: present, whatever warm-up listed.
    index.installed(removed);
    CHECK(index.answer(removed) == PresenceIndex::Answer::present);

    // Once authoritative, a later removal needs no remembering, and a stale
    // listing (there is none after warm-up, but if there were) cannot
    // republish a pruned object.
    PresenceIndex pruned;
    pruned.warmed();
    pruned.pruned(removed);
    pruned.listed(listing);
    CHECK(pruned.answer(removed) == PresenceIndex::Answer::absent);
}

// ---- LocalStore --------------------------------------------------------------

Bytes numbered(uint64_t n) {
    auto bytes = pattern(4096);
    std::memcpy(bytes.data(), &n, sizeof(n));
    return bytes;
}

struct Keys {
    TempDir dir;
    ClusterKeys keys;
    Keys() {
        write_key(dir.path() / "key");
        keys = load_cluster_keys(dir.path() / "key");
    }
};

MACHA_TEST("presence", test_has_answers_a_put_in_progress_without_waiting_for_it) {
    Keys k;
    LocalStore store(k.dir.path() / "store", 64ULL << 20, k.keys.storage);
    REQUIRE(wait_until([&] { return store.presence_authoritative(); }, 5s));
    const auto bytes = numbered(1);
    const auto id = object_id(bytes);

    std::mutex mutex;
    std::condition_variable cv;
    bool writing = false;
    bool release = false;
    store.set_before_loose_write_for_tests([&](const ObjectId&) {
        std::unique_lock lock(mutex);
        writing = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    std::jthread put([&] { REQUIRE(store.put(id, bytes)); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 5s, [&] { return writing; }));
    }
    // The put holds the object's lock across its write; has() neither waits
    // for it nor sees the object before the write completes.
    const auto asked = Clock::now();
    CHECK(!store.has(id));
    CHECK(Clock::now() - asked < 500ms);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    put.join();
    CHECK(store.has(id));
}

MACHA_TEST("presence", test_a_put_that_fails_midway_is_never_present) {
    Keys k;
    LocalStore store(k.dir.path() / "store", 64ULL << 20, k.keys.storage);
    REQUIRE(wait_until([&] { return store.presence_authoritative(); }, 5s));
    const auto bytes = numbered(1);
    const auto id = object_id(bytes);
    store.set_before_loose_write_for_tests(
        [](const ObjectId&) { throw std::runtime_error("device failed mid-write"); });
    bool threw = false;
    try {
        (void)store.put(id, bytes);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!store.has(id));
    // And the store is still usable: the next put of it succeeds.
    store.set_before_loose_write_for_tests({});
    REQUIRE(store.put(id, bytes));
    CHECK(store.has(id));
}

MACHA_TEST("presence", test_has_does_not_consult_the_device_after_warm_up) {
    Keys k;
    const auto root = k.dir.path() / "store";
    LocalStore store(root, 64ULL << 20, k.keys.storage);
    REQUIRE(wait_until([&] { return store.presence_authoritative(); }, 5s));
    const auto held_bytes = numbered(1);
    const auto held = object_id(held_bytes);
    REQUIRE(store.put(held, held_bytes));
    const auto planted = object_id(numbered(2));

    // Behind the store's back, where a device check would notice: the held
    // object's file disappears and another object's file appears. has()
    // reports what the store knows, which is what it wrote.
    std::filesystem::create_directories(store.object_path(planted).parent_path());
    std::filesystem::copy_file(store.object_path(held), store.object_path(planted));
    std::filesystem::remove(store.object_path(held));
    CHECK(store.has(held));
    CHECK(!store.has(planted));
}

MACHA_TEST("presence", test_presence_is_exact_across_a_restart) {
    Keys k;
    const auto root = k.dir.path() / "store";
    std::vector<ObjectId> held;
    {
        LocalStore store(root, 64ULL << 20, k.keys.storage);
        for (uint64_t n = 0; n < 20; ++n) {
            const auto bytes = numbered(n);
            held.push_back(object_id(bytes));
            REQUIRE(store.put(held.back(), bytes));
        }
        REQUIRE(store.remove(held[3]));
    }
    LocalStore store(root, 64ULL << 20, k.keys.storage);
    REQUIRE(wait_until([&] { return store.presence_authoritative(); }, 5s));
    for (size_t n = 0; n < held.size(); ++n)
        CHECK(store.has(held[n]) == (n != 3));
    CHECK(!store.has(object_id(numbered(100))));
}

// ---- StoragePool ---------------------------------------------------------------

MACHA_TEST("presence", test_a_backend_offline_and_readopted_leaves_presence_exact) {
    Keys k;
    const auto backend = k.dir.path() / "data";
    std::filesystem::create_directories(backend);
    StoragePool pool(k.dir.path() / "pool-state", random_node_id(),
                     std::vector<StorageBackendConfig>{{backend, 64ULL << 20}}, k.keys.storage);
    REQUIRE(wait_until([&] { return pool.online_backends() == 1; }, 5s));
    const auto bytes = numbered(1);
    const auto id = object_id(bytes);
    REQUIRE(pool.put(id, bytes));
    const auto absent = object_id(numbered(2));
    CHECK(pool.has(id));

    auto away = backend;
    away += ".away";
    std::filesystem::rename(backend, away);
    pool.refresh();
    REQUIRE(pool.online_backends() == 0);
    CHECK(!pool.has(id));
    CHECK(!pool.has(absent));

    std::filesystem::rename(away, backend);
    pool.refresh();
    REQUIRE(pool.online_backends() == 1);
    REQUIRE(wait_until([&] { return pool.has(id); }, 5s));
    CHECK(!pool.has(absent));
}

} // namespace

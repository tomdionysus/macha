// SPDX-License-Identifier: GPL-3.0-or-later
//
// RetentionLedger is checked against the RetentionStore it pages; ClaimWalk is
// checked step by step against a reference walk over fakes, for every held
// pattern and every credit limit that reaches a different decision.
#include "crypto.hpp"
#include "service/claim_walk.hpp"
#include "ledger/retention_ledger.hpp"
#include "test_framework.hpp"
#include "test_backend_support.hpp"

#include <map>
#include <set>
#include <vector>

using namespace macha;
using namespace macha::test_support;

namespace {

ObjectId id_of(size_t i) {
    ObjectId id;
    id.bytes[30] = static_cast<uint8_t>(i >> 8);
    id.bytes[31] = static_cast<uint8_t>(i + 1);
    return id;
}

struct FakeStore final : ObjectStore {
    uint64_t losses() const noexcept override { return 0; }
    bool indexed() const noexcept override { return true; }
    std::set<ObjectId> objects;
    mutable size_t asked{};
    bool has(const ObjectId& id) const override {
        ++asked;
        return objects.contains(id);
    }
};

// Claim counts 0..9 (every third released, so the ledger must skip entries) and
// page bounds 1..10: each live claim comes once, in order, pages stay within
// the bound, only the last page is complete, and its cursor is the start again.
MACHA_FAST_TEST("claim_walk", test_retention_ledger_pages_every_live_claim_once) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto origin = random_node_id();
    FakeStore data;
    FakeStore control;
    for (size_t claims = 0; claims <= 9; ++claims) {
        RetentionStore retention(t.path() / ("state-" + std::to_string(claims)), keys.storage);
        std::vector<ObjectId> live;
        std::vector<ObjectId> released;
        for (size_t i = 0; i < claims; ++i) {
            retention.retain(RetentionClass::data, id_of(i), {origin, 1});
            (i % 3 == 2 ? released : live).push_back(id_of(i));
        }
        // A control claim the data pages must never show.
        retention.retain(RetentionClass::control, id_of(500), {origin, 1});
        if (!released.empty()) {
            std::vector<ObjectId> keep = live;
            CHECK(retention.release_unreferenced(RetentionClass::data, keep,
                                                 RetentionClock{{origin, 1}}, 64) ==
                  released.size());
        }
        const RetentionLedger ledger(retention, data, control);
        for (size_t bound = 1; bound <= 10; ++bound) {
            std::vector<ObjectId> walked;
            Cursor<ObjectId> cursor;
            for (size_t pages = 0;; ++pages) {
                CHECK(pages <= claims + 1);
                Budget budget;
                budget.operations(bound);
                const auto page = ledger.claimed(RetentionClass::data, cursor, budget);
                CHECK(page.items.size() <= bound);
                walked.insert(walked.end(), page.items.begin(), page.items.end());
                if (page.complete()) {
                    CHECK(!page.next.after);
                    break;
                }
                CHECK(page.stopped == Stop::budget);
                CHECK(page.items.size() == bound);
                CHECK(page.next.after == page.items.back());
                cursor = page.next;
            }
            CHECK(walked == live);
        }
        // No budget: nothing is read and the cursor does not move.
        Budget none;
        none.operations(0);
        const Cursor<ObjectId> from{id_of(0)};
        const auto page = ledger.claimed(RetentionClass::data, from, none);
        CHECK(page.items.empty());
        CHECK(page.stopped == Stop::budget);
        CHECK(page.next == from);
    }
}

MACHA_FAST_TEST("claim_walk", test_retention_ledger_stops_for_cancellation_first) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    RetentionStore retention(t.path() / "state", keys.storage);
    retention.retain(RetentionClass::data, id_of(0), {random_node_id(), 1});
    FakeStore stores;
    const RetentionLedger ledger(retention, stores, stores);
    std::atomic_bool cancelled{true};
    Budget budget(WorkContext(FrameType::control, {}, &cancelled));
    const auto page = ledger.claimed(RetentionClass::data, {}, budget);
    CHECK(page.items.empty());
    CHECK(page.stopped == Stop::cancelled);
}

MACHA_FAST_TEST("claim_walk", test_retention_ledger_holds_by_class) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    RetentionStore retention(t.path() / "state", keys.storage);
    FakeStore data;
    FakeStore control;
    data.objects = {id_of(1)};
    control.objects = {id_of(2)};
    const RetentionLedger ledger(retention, data, control);
    for (size_t i : {1, 2, 3}) {
        CHECK(ledger.held(RetentionClass::data, id_of(i)) == (i == 1));
        CHECK(ledger.held(RetentionClass::control, id_of(i)) == (i == 2));
    }
    CHECK(data.asked == 3u);
    CHECK(control.asked == 3u);
}

MACHA_FAST_TEST("claim_walk", test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    RetentionStore retention(t.path() / "state", keys.storage);
    FakeStore data;
    FakeStore control;
    RetentionLedger ledger(retention, data, control);
    CHECK(!ledger.inventory());
    CHECK(!ledger.release());

    const auto first = std::make_shared<const InventoryHorizon>(
        3, true, std::vector<ObjectId>{id_of(1)}, std::vector<ObjectId>{}, std::vector<GarbageRef>{});
    ledger.publish(first);
    const auto held = ledger.inventory();
    CHECK(held == first);
    const auto second = std::make_shared<const InventoryHorizon>(
        4, false, std::vector<ObjectId>{}, std::vector<ObjectId>{}, std::vector<GarbageRef>{});
    ledger.publish(second);
    CHECK(ledger.inventory() == second);
    // A handle taken before the publish still reads its own snapshot.
    CHECK(held->generation() == 3);

    Hash256 head{};
    head.bytes[0] = 1;
    const auto complete = std::make_shared<const ReleaseHorizon>(
        head, RetentionClock{}, std::vector<ObjectId>{id_of(1)}, std::vector<ObjectId>{});
    CHECK(!ledger.publish(ReleaseBuild{complete, false}));
    CHECK(!ledger.release());
    CHECK(ledger.publish(ReleaseBuild{complete, true}));
    CHECK(ledger.release() == complete);
    Hash256 newer{};
    newer.bytes[0] = 2;
    const auto partial = std::make_shared<const ReleaseHorizon>(
        newer, RetentionClock{}, std::vector<ObjectId>{}, std::vector<ObjectId>{});
    CHECK(!ledger.publish(ReleaseBuild{partial, false}));
    // The previous, complete horizon is kept.
    CHECK(ledger.release() == complete);
}

MACHA_FAST_TEST("claim_walk", test_retention_ledger_forwards_claims_releases_prunes_and_compacts) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    RetentionStore retention(t.path() / "state", keys.storage);
    FakeStore data;
    FakeStore control;
    RetentionLedger ledger(retention, data, control);
    const auto origin = random_node_id();

    // Claims as the store has them, per class.
    retention.retain(RetentionClass::data, id_of(1), {origin, 1});
    retention.retain(RetentionClass::data, id_of(2), {origin, 2});
    retention.retain(RetentionClass::control, id_of(3), {origin, 3});
    CHECK(ledger.retained(RetentionClass::data, id_of(1)));
    CHECK(!ledger.retained(RetentionClass::control, id_of(1)));
    CHECK(ledger.retained(RetentionClass::control, id_of(3)));
    CHECK(!ledger.retained(RetentionClass::data, id_of(4)));

    // A claim the horizon does not refer to, written before its clock, is
    // released; one it refers to stays; the other class is untouched.
    const ReleaseHorizon release(Hash256{}, {{origin, 10}}, {id_of(2)}, {});
    CHECK(ledger.release_unreferenced(RetentionClass::data, release, 64) == 1);
    CHECK(!ledger.retained(RetentionClass::data, id_of(1)));
    CHECK(ledger.retained(RetentionClass::data, id_of(2)));
    CHECK(ledger.retained(RetentionClass::control, id_of(3)));

    // Prune forgets a released object's tombstone only once the class's store no longer holds it.
    data.objects = {id_of(1)};
    CHECK(ledger.prune_unclaimed(RetentionClass::data, 64) == 0);
    data.objects.clear();
    CHECK(ledger.prune_unclaimed(RetentionClass::data, 64) == 1);
    // Nothing of control was released, and control's store keeps no data tombstone.
    CHECK(ledger.prune_unclaimed(RetentionClass::control, 64) == 0);

    CHECK(!ledger.compact_if_needed(1000000));
    CHECK(ledger.compact_if_needed(1));
}

// Claims in id order; the ledger honours the budget as the contract says.
struct FakeLedger final : ObjectLedger {
    std::vector<ObjectId> claims;
    std::set<ObjectId> held_ids;
    Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                     Budget& budget) const override {
        Page<ObjectId, ObjectId> page;
        page.next = from;
        auto it = from.after ? std::upper_bound(claims.begin(), claims.end(), *from.after)
                             : claims.begin();
        for (;; ++it) {
            if (!budget.take_operation()) {
                page.stopped = Stop::budget;
                return page;
            }
            if (it == claims.end()) {
                page.next = {};
                return page;
            }
            page.items.push_back(*it);
            page.next.after = *it;
        }
    }
    bool held(RetentionClass, const ObjectId& id) const override {
        return held_ids.contains(id);
    }
    uint64_t held_losses(RetentionClass) const noexcept override { return 0; }
    bool held_indexed(RetentionClass) const noexcept override { return true; }
    // The claim walk reads no horizon.
    InventoryHandle inventory() const override { return {}; }
    ReleaseHandle release() const override { return {}; }
    void publish(InventoryHandle) override {}
    bool publish(ReleaseBuild) override { return false; }
    bool retained(RetentionClass, const ObjectId&) const override { return false; }
    size_t release_unreferenced(RetentionClass, const ReleaseHorizon&, size_t) override { return 0; }
    size_t prune_unclaimed(RetentionClass, size_t) override { return 0; }
    bool compact_if_needed(size_t) override { return false; }
};

// Restores until its credit runs out; `refused` ids are fetched but not
// restored and cost nothing. Records every call.
struct FakeRestorer final : ClaimRestorer {
    size_t credit{};
    std::set<ObjectId> refused;
    std::vector<std::pair<ObjectId, Outcome>> calls;
    Outcome restore(RetentionClass, const ObjectId& id) override {
        Outcome outcome = Outcome::restored;
        if (!credit)
            outcome = Outcome::waiting_for_credit;
        else if (refused.contains(id))
            outcome = Outcome::not_restored;
        else
            --credit;
        calls.emplace_back(id, outcome);
        return outcome;
    }
};

// The reference walk: next_retained() one claim at a time, 16 per step.
struct ReferenceWalk {
    std::optional<ObjectId> cursor;
    ClaimWalkStep step(const FakeLedger& ledger, FakeRestorer& restorer) {
        ClaimWalkStep result;
        for (size_t examined = 0;; ++examined) {
            if (examined == 16) {
                result.unfinished = true;
                break;
            }
            const auto resume = cursor;
            auto it = cursor ? std::upper_bound(ledger.claims.begin(), ledger.claims.end(), *cursor)
                             : ledger.claims.begin();
            if (it == ledger.claims.end()) {
                cursor.reset();
                break;
            }
            cursor = *it;
            ++result.examined;
            if (ledger.held(RetentionClass::data, *it))
                continue;
            ++result.missing;
            const auto outcome = restorer.restore(RetentionClass::data, *it);
            if (outcome == ClaimRestorer::Outcome::waiting_for_credit) {
                cursor = resume;
                result.waiting_for_credit = true;
                break;
            }
            if (outcome == ClaimRestorer::Outcome::restored)
                ++result.restored;
        }
        return result;
    }
};

bool same(const ClaimWalkStep& a, const ClaimWalkStep& b) {
    return a.examined == b.examined && a.missing == b.missing && a.restored == b.restored &&
           a.waiting_for_credit == b.waiting_for_credit && a.unfinished == b.unfinished;
}

// Run both walks side by side for `steps` steps, credit topped up by `refill`
// each step; every step's result, every restorer call and every cursor agree.
void compare(const FakeLedger& ledger, const std::set<ObjectId>& refused, size_t initial,
             size_t refill, size_t steps) {
    FakeRestorer ours;
    FakeRestorer theirs;
    ours.refused = theirs.refused = refused;
    ours.credit = theirs.credit = initial;
    ClaimWalk walk(RetentionClass::data);
    CHECK(walk.type() == RetentionClass::data);
    ReferenceWalk reference;
    for (size_t s = 0; s < steps; ++s) {
        const auto a = walk.step(ledger, ours);
        const auto b = reference.step(ledger, theirs);
        CHECK(same(a, b));
        CHECK(ours.calls == theirs.calls);
        CHECK(walk.cursor().after == reference.cursor);
        CHECK(a.examined <= ClaimWalk::step_bound);
        ours.credit += refill;
        theirs.credit += refill;
    }
}

// Every held pattern over up to eight claims, credit 0..3 with and without
// refill, and a refused claim.
MACHA_FAST_TEST("claim_walk", test_claim_walk_matches_the_walk_it_replaced_exhaustively) {
    for (size_t n = 0; n <= 8; ++n)
        for (unsigned pattern = 0; pattern < (1U << n); ++pattern) {
            FakeLedger ledger;
            for (size_t i = 0; i < n; ++i) {
                ledger.claims.push_back(id_of(i));
                if (pattern & (1U << i))
                    ledger.held_ids.insert(id_of(i));
            }
            for (size_t initial = 0; initial <= 3; ++initial)
                for (size_t refill : {0, 1}) {
                    compare(ledger, {}, initial, refill, n + 3);
                    if (n)
                        compare(ledger, {id_of(n / 2)}, initial, refill, n + 3);
                }
        }
}

// Around the step bound: 15, 16, 17, 32, 33 and 50 claims, all held, none
// held, and every fifth missing, over enough steps to wrap twice.
MACHA_FAST_TEST("claim_walk", test_claim_walk_matches_the_walk_it_replaced_at_the_bound) {
    for (size_t n : {15, 16, 17, 32, 33, 50})
        for (int shape = 0; shape < 3; ++shape) {
            FakeLedger ledger;
            for (size_t i = 0; i < n; ++i) {
                ledger.claims.push_back(id_of(i));
                if (shape == 0 || (shape == 2 && i % 5))
                    ledger.held_ids.insert(id_of(i));
            }
            for (size_t initial : {0, 3, 100})
                for (size_t refill : {0, 2})
                    compare(ledger, {}, initial, refill, 2 * (n / 16 + 2));
        }
}

// Claims appear and disappear between steps: the walk resumes after its
// cursor, whatever is there now.
MACHA_FAST_TEST("claim_walk", test_claim_walk_follows_a_changing_ledger) {
    FakeLedger ledger;
    for (size_t i = 0; i < 40; ++i)
        ledger.claims.push_back(id_of(i * 2));
    FakeRestorer ours;
    FakeRestorer theirs;
    ours.credit = theirs.credit = 1000;
    ClaimWalk walk(RetentionClass::data);
    ReferenceWalk reference;
    for (size_t s = 0; s < 12; ++s) {
        CHECK(same(walk.step(ledger, ours), reference.step(ledger, theirs)));
        CHECK(ours.calls == theirs.calls);
        // Insert one between the cursor and its successor, drop the first.
        if (walk.cursor().after) {
            const auto after = *walk.cursor().after;
            ObjectId between = after;
            ++between.bytes[31];
            ledger.claims.insert(
                std::upper_bound(ledger.claims.begin(), ledger.claims.end(), after), between);
            std::sort(ledger.claims.begin(), ledger.claims.end());
            ledger.claims.erase(std::unique(ledger.claims.begin(), ledger.claims.end()),
                                ledger.claims.end());
        }
        if (!ledger.claims.empty())
            ledger.claims.erase(ledger.claims.begin());
    }
}

} // namespace

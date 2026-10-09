// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ledger's predicate queries. Each predicate is checked over all sixteen
// combinations of its facts against the spec's table below; each query runs
// against a fake ledger and placement, over every page bound, and is compared
// with the predicate applied to the whole set at once.
#include "contract/object_ledger.hpp"
#include "contract/predicates.hpp"
#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <set>
#include <stdexcept>
#include <vector>

using namespace macha;

namespace {

ObjectId id(uint8_t n) {
    ObjectId out{};
    out.bytes[0] = n;
    return out;
}

constexpr std::array all_predicates{Predicate::to_pull,      Predicate::missing_here,
                                    Predicate::held_owned,   Predicate::to_push_from,
                                    Predicate::surplus,      Predicate::releasable,
                                    Predicate::garbage};

// The spec's table: for each predicate, the facts that must hold (1), must
// not hold (0) or do not matter (-), in the order referenced, held, owned,
// claimed.
struct Row {
    Predicate predicate;
    std::array<int, 4> facts;
};
constexpr std::array table{
    Row{Predicate::to_pull, {1, 0, 1, -1}},      Row{Predicate::missing_here, {1, 0, -1, 1}},
    Row{Predicate::held_owned, {1, 1, 1, -1}},   Row{Predicate::to_push_from, {1, 1, 0, -1}},
    Row{Predicate::surplus, {1, 1, 0, 0}},       Row{Predicate::releasable, {0, -1, -1, 1}},
    Row{Predicate::garbage, {0, 1, -1, 0}},
};

MACHA_FAST_TEST("predicates", test_each_predicate_is_its_row_over_every_combination_of_facts) {
    for (const auto& row : table)
        for (unsigned bits = 0; bits < 16; ++bits) {
            const std::array<bool, 4> value{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0,
                                            (bits & 8) != 0};
            bool expected = true;
            for (size_t i = 0; i < 4; ++i)
                if (row.facts[i] != -1 && value[i] != (row.facts[i] == 1))
                    expected = false;
            const ObjectFacts facts{value[0], value[1], value[2], value[3]};
            CHECK(satisfies(row.predicate, facts) == expected);
        }
    CHECK(table.size() == all_predicates.size());
}

// A ledger whose horizons, holdings and claims are sets the test writes.
struct FakeLedger final : ObjectLedger {
    InventoryHandle inventory_handle;
    ReleaseHandle release_handle;
    std::set<ObjectId> held_ids;
    std::vector<ObjectId> claims; // sorted
    mutable size_t claimed_calls{};

    Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                     Budget& budget) const override {
        ++claimed_calls;
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
    bool held(RetentionClass type, const ObjectId& id) const override {
        return type == RetentionClass::data && held_ids.contains(id);
    }
    HeldView held_view(RetentionClass) const override { return {}; }
    InventoryHandle inventory() const override { return inventory_handle; }
    ReleaseHandle release() const override { return release_handle; }
    void publish(InventoryHandle) override {}
    bool publish(ReleaseBuild) override { return false; }
    bool retained(RetentionClass type, const ObjectId& id) const override {
        return type == RetentionClass::data &&
               std::binary_search(claims.begin(), claims.end(), id);
    }
    size_t release_unreferenced(RetentionClass, const ReleaseHorizon&, size_t) override { return 0; }
    size_t prune_unclaimed(RetentionClass, size_t) override { return 0; }
    bool compact_if_needed(size_t) override { return false; }
};

struct FakePlacement final : Placement {
    std::set<ObjectId> owned;
    bool owns(const ObjectId& id) const override { return owned.contains(id); }
};

// Every combination of the four facts once: id n has referenced = bit 0,
// held = bit 1, owned = bit 2, claimed = bit 3 of n-1, for n in 1..16, so
// the inventory refers to the even ids. The release horizon refers to the
// multiples of three, a different set, so a query that read the wrong
// horizon would differ; the inventory's control set must not leak into the
// DATA queries.
struct World {
    FakeLedger ledger;
    FakePlacement placement;
    World() {
        std::vector<ObjectId> referenced;
        std::vector<ObjectId> released_against;
        for (uint8_t n = 1; n <= 16; ++n) {
            const unsigned bits = n - 1;
            if (bits & 1)
                referenced.push_back(id(n));
            if (bits & 2)
                ledger.held_ids.insert(id(n));
            if (bits & 4)
                placement.owned.insert(id(n));
            if (bits & 8)
                ledger.claims.push_back(id(n));
            if (n % 3 == 0)
                released_against.push_back(id(n));
        }
        ledger.inventory_handle = std::make_shared<const InventoryHorizon>(
            1, true, referenced, std::vector<ObjectId>{id(200)}, std::vector<GarbageRef>{});
        ledger.release_handle = std::make_shared<const ReleaseHorizon>(
            Hash256{}, RetentionClock{}, released_against, std::vector<ObjectId>{});
    }
    // The predicate over the whole set at once, as the reference.
    std::vector<ObjectId> expected(Predicate predicate) const {
        std::vector<ObjectId> out;
        for (uint8_t n = 1; n <= 16; ++n) {
            const bool in_release = n % 3 == 0;
            ObjectFacts facts{ledger.inventory_handle->referenced(RetentionClass::data, id(n)),
                              ledger.held_ids.contains(id(n)), placement.owned.contains(id(n)),
                              std::binary_search(ledger.claims.begin(), ledger.claims.end(), id(n))};
            if (predicate == Predicate::releasable) {
                if (!facts.claimed)
                    continue;
                facts.referenced = in_release;
            } else if (!facts.referenced) {
                continue;
            }
            if (satisfies(predicate, facts))
                out.push_back(id(n));
        }
        return out;
    }
};

std::vector<ObjectId> run_paged(Predicate predicate, const World& world, size_t bound,
                                size_t& pages) {
    std::vector<ObjectId> out;
    Cursor<ObjectId> cursor;
    pages = 0;
    for (;;) {
        Budget budget;
        budget.operations(bound);
        const auto page = query(predicate, world.ledger, world.placement, cursor, budget);
        ++pages;
        out.insert(out.end(), page.items.begin(), page.items.end());
        if (page.complete()) {
            CHECK(!page.next.after.has_value());
            return out;
        }
        CHECK(page.stopped == Stop::budget);
        cursor = page.next;
        if (pages > 64) {
            CHECK(!"query never completed");
            return out;
        }
    }
}

MACHA_FAST_TEST("predicates", test_each_query_pages_to_the_predicate_over_the_whole_set) {
    const World world;
    for (const auto predicate : all_predicates) {
        if (predicate == Predicate::garbage)
            continue;
        const auto expected = world.expected(predicate);
        // Both sources (the referenced ids, the claims) hold 8 ids; a page
        // examines at most `bound` of them.
        for (size_t bound = 1; bound <= 20; ++bound) {
            size_t pages = 0;
            CHECK(run_paged(predicate, world, bound, pages) == expected);
            CHECK(pages >= (8 + bound - 1) / bound);
        }
    }
    // Spot values, so a wrong reference cannot hide a wrong query. For id n
    // the facts are the bits of n-1: id 6 (5 = referenced, owned) is to pull.
    size_t pages = 0;
    const auto to_pull = run_paged(Predicate::to_pull, world, 100, pages);
    CHECK(std::find(to_pull.begin(), to_pull.end(), id(6)) != to_pull.end());
    CHECK(to_pull.size() == 2); // n-1 in {5, 13}
    const auto releasable = run_paged(Predicate::releasable, world, 100, pages);
    // Claimed (n-1 >= 8, so n in 9..16) and not a multiple of three.
    CHECK((releasable == std::vector<ObjectId>{id(10), id(11), id(13), id(14), id(16)}));
}

MACHA_FAST_TEST("predicates", test_queries_without_horizons_examine_nothing) {
    FakeLedger ledger;
    FakePlacement placement;
    for (const auto predicate : all_predicates) {
        if (predicate == Predicate::garbage)
            continue;
        Budget budget;
        budget.operations(4);
        const auto page = query(predicate, ledger, placement, {}, budget);
        CHECK(page.items.empty());
        CHECK(page.complete());
    }
    CHECK(ledger.claimed_calls == 0);
}

MACHA_FAST_TEST("predicates", test_garbage_is_not_queryable_at_stage_0) {
    const World world;
    Budget budget;
    bool threw = false;
    try {
        (void)query(Predicate::garbage, world.ledger, world.placement, {}, budget);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

MACHA_FAST_TEST("predicates", test_a_cancelled_budget_stops_a_query_before_it_examines) {
    const World world;
    std::atomic_bool cancelled{true};
    Budget budget(WorkContext(FrameType::control, {}, &cancelled));
    budget.operations(100);
    const auto page = query(Predicate::to_pull, world.ledger, world.placement, {}, budget);
    CHECK(page.items.empty());
    CHECK(page.stopped == Stop::cancelled);
}

} // namespace

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ledger's horizons and destructive gates. Each gate is tested over every
// combination of the facts it reads against a reference model of the
// maintenance pass's condition, reason and trace.
#include "contract/gates.hpp"
#include "contract/horizon.hpp"
#include "test_framework.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace macha;

namespace {

ObjectId id(uint8_t n) {
    ObjectId out{};
    out.bytes[0] = n;
    return out;
}

GarbageRef garbage(uint8_t n, int64_t retired_at_ns) {
    GarbageRef out;
    out.id = id(n);
    out.retired_at_ns = retired_at_ns;
    return out;
}

std::vector<ObjectId> ids_of(std::span<const ObjectId> span) { return {span.begin(), span.end()}; }

MACHA_FAST_TEST("ledger", test_referenced_sets_are_sorted_unique_and_per_class) {
    const ReferencedSets sets({id(3), id(1), id(3), id(2)}, {id(9), id(9), id(5)});
    CHECK((ids_of(sets.referenced_ids(RetentionClass::data)) ==
           std::vector<ObjectId>{id(1), id(2), id(3)}));
    CHECK((ids_of(sets.referenced_ids(RetentionClass::control)) ==
           std::vector<ObjectId>{id(5), id(9)}));
    CHECK(sets.size(RetentionClass::data) == 3);
    CHECK(sets.size(RetentionClass::control) == 2);
    for (uint8_t n = 0; n < 12; ++n) {
        CHECK(sets.referenced(RetentionClass::data, id(n)) == (n >= 1 && n <= 3));
        CHECK(sets.referenced(RetentionClass::control, id(n)) == (n == 5 || n == 9));
    }
    const ReferencedSets empty({}, {});
    CHECK(empty.size(RetentionClass::data) == 0);
    CHECK(empty.referenced_ids(RetentionClass::control).empty());
    CHECK(!empty.referenced(RetentionClass::data, id(0)));
}

MACHA_FAST_TEST("ledger", test_inventory_splits_tombstones_by_whether_data_still_refers_to_them) {
    // A tombstone whose id is live data again is stale; one referenced only
    // as control is not.
    const InventoryHorizon inventory(
        7, false, {id(2), id(4)}, {id(6)},
        {garbage(4, 10), garbage(1, 0), garbage(6, 30), garbage(2, 40), garbage(1, 50)});
    CHECK(inventory.generation() == 7);
    CHECK(!inventory.catalogue_complete());
    CHECK(inventory.garbage().size() == 3);
    CHECK(inventory.garbage()[0].id == id(1) && inventory.garbage()[0].retired_at_ns == 0);
    CHECK(inventory.garbage()[1].id == id(6) && inventory.garbage()[1].retired_at_ns == 30);
    CHECK(inventory.garbage()[2].id == id(1) && inventory.garbage()[2].retired_at_ns == 50);
    CHECK(inventory.stale_garbage().size() == 2);
    CHECK(inventory.stale_garbage()[0].id == id(4) && inventory.stale_garbage()[0].retired_at_ns == 10);
    CHECK(inventory.stale_garbage()[1].id == id(2) && inventory.stale_garbage()[1].retired_at_ns == 40);
    CHECK(InventoryHorizon(0, true, {}, {}, {}).catalogue_complete());
}

MACHA_FAST_TEST("ledger", test_release_horizon_carries_its_head_and_clock) {
    Hash256 head{};
    head.bytes[0] = 0xab;
    NodeId node{};
    node.bytes[0] = 1;
    const ReleaseHorizon release(head, {{node, 12}}, {id(2), id(1)}, {id(3)});
    CHECK(release.head() == head);
    CHECK(release.clock().at(node) == 12);
    CHECK((ids_of(release.referenced_ids(RetentionClass::data)) ==
           std::vector<ObjectId>{id(1), id(2)}));
    CHECK(release.referenced(RetentionClass::control, id(3)));
}

// The reference model's state: an inventory or none, its generation, its
// catalogue completeness (true before the first inventory).
struct Reference {
    bool garbage_due, gc_due, busy, waiting, rebuilt, reachable, metadata_stable, release_view,
        baseline;
    std::optional<uint64_t> generation;
    bool catalogue_complete;
    uint64_t known;
};

std::string flag(std::string_view name, bool value) {
    return std::string(name) + (value ? "=1" : "=0");
}

// Each kind of inventory the gates can see.
std::vector<std::optional<InventoryHorizon>> inventories() {
    std::vector<std::optional<InventoryHorizon>> out;
    out.emplace_back(std::nullopt);
    for (uint64_t generation : {4, 5, 6})
        for (bool complete : {false, true})
            out.emplace_back(InventoryHorizon(generation, complete, {id(1)}, {id(2)}, {}));
    return out;
}

template <class Check> void over_every_pass(Check&& check) {
    for (unsigned bits = 0; bits < (1U << 9); ++bits)
        for (const auto& inventory : inventories()) {
            const auto bit = [bits](unsigned n) { return ((bits >> n) & 1U) != 0; };
            Reference ref{bit(0), bit(1), bit(2), bit(3), bit(4), bit(5), bit(6), bit(7), bit(8),
                          inventory ? std::optional<uint64_t>(inventory->generation())
                                    : std::nullopt,
                          inventory ? inventory->catalogue_complete() : true, 5};
            PassFacts facts;
            facts.garbage_due = ref.garbage_due;
            facts.gc_due = ref.gc_due;
            facts.busy = ref.busy;
            facts.gc_waiting_for_event = ref.waiting;
            facts.rebuilt_inventory = ref.rebuilt;
            facts.reachable = ref.reachable;
            facts.metadata_stable = ref.metadata_stable;
            facts.release_view = ref.release_view;
            facts.retention_baseline_complete = ref.baseline;
            facts.known_generation = ref.known;
            check(ref, facts, inventory ? &*inventory : nullptr);
        }
}

MACHA_FAST_TEST("ledger", test_tombstone_gate_is_the_pass_condition_over_every_input) {
    over_every_pass([](const Reference& r, const PassFacts& facts, const InventoryHorizon* inventory) {
        const bool cluster_gc_stable = r.reachable && r.metadata_stable;
        const bool expected = r.garbage_due && !r.rebuilt && cluster_gc_stable && r.catalogue_complete;
        const auto verdict = tombstone_gate(facts, inventory);
        CHECK(verdict.permitted == expected);
        CHECK(verdict.reason.empty() == expected);
        CHECK(verdict.conditions ==
              flag("due", r.garbage_due) + " " + flag("rebuilt", r.rebuilt) + " " +
                  flag("reachable", r.reachable) + " " + flag("stable", cluster_gc_stable) + " " +
                  flag("catalogue_complete", r.catalogue_complete));
    });
}

MACHA_FAST_TEST("ledger", test_control_gate_is_the_pass_condition_over_every_input) {
    over_every_pass([](const Reference& r, const PassFacts& facts, const InventoryHorizon* inventory) {
        const bool cluster_gc_stable = r.reachable && r.metadata_stable;
        const bool destructive = cluster_gc_stable && r.release_view && r.baseline;
        const uint64_t generation = r.generation.value_or(0);
        const bool expected = r.gc_due && !r.rebuilt && destructive && r.catalogue_complete &&
                              r.generation.has_value() && generation >= r.known;
        const auto verdict = control_gate(facts, inventory);
        CHECK(verdict.permitted == expected);
        CHECK(verdict.reason.empty() == expected);
        CHECK(verdict.conditions ==
              flag("due", r.gc_due) + " " + flag("rebuilt", r.rebuilt) + " " +
                  flag("destructive", destructive) + " " +
                  flag("catalogue_complete", r.catalogue_complete) + " " +
                  flag("control_live", r.generation.has_value()) + " " +
                  flag("generation_current", generation >= r.known));
    });
}

MACHA_FAST_TEST("ledger", test_data_gate_is_the_pass_condition_and_reason_over_every_input) {
    over_every_pass([](const Reference& r, const PassFacts& facts, const InventoryHorizon* inventory) {
        const bool cluster_gc_healthy = r.reachable;
        const bool cluster_gc_stable = r.reachable && r.metadata_stable;
        const bool destructive = cluster_gc_stable && r.release_view && r.baseline;
        const uint64_t generation = r.generation.value_or(0);
        const bool expected = r.gc_due && !r.rebuilt && destructive && r.catalogue_complete &&
                              r.generation.has_value() && generation >= r.known;
        std::string reason;
        if (!r.gc_due)
            reason = r.busy ? "foreground busy"
                     : r.waiting ? "complete; waiting for an event"
                                 : "quiet window";
        else if (r.rebuilt)
            reason = "inventory rebuilt this pass";
        else if (!cluster_gc_stable)
            reason = cluster_gc_healthy ? "metadata not stable" : "not every known node reachable";
        else if (!r.release_view)
            reason = "no sole accepted head for retention release";
        else if (!r.baseline)
            reason = "retention baseline incomplete";
        else if (!r.catalogue_complete)
            reason = "catalogue inventory incomplete";
        else if (!r.generation)
            reason = "no reachability inventory";
        else if (generation < r.known)
            reason = "inventory generation " + std::to_string(generation) + " behind known " +
                     std::to_string(r.known);
        const auto verdict = data_gate(facts, inventory);
        CHECK(verdict.permitted == expected);
        CHECK(verdict.reason == reason);
        CHECK(verdict.conditions ==
              flag("due", r.gc_due) + " " + flag("rebuilt", r.rebuilt) + " " +
                  flag("reachable", cluster_gc_healthy) + " " + flag("stable", cluster_gc_stable) +
                  " " + flag("release_view", r.release_view) + " " +
                  flag("baseline", r.release_view && r.baseline) + " " +
                  flag("catalogue_complete", r.catalogue_complete) + " " +
                  flag("live", r.generation.has_value()) + " " +
                  flag("generation_current", generation >= r.known));
    });
}

MACHA_FAST_TEST("ledger", test_each_shut_gate_names_the_first_condition_that_failed) {
    PassFacts open;
    open.garbage_due = open.gc_due = true;
    open.reachable = open.metadata_stable = true;
    open.release_view = open.retention_baseline_complete = true;
    open.known_generation = 5;
    const InventoryHorizon current(5, true, {}, {}, {});
    CHECK(tombstone_gate(open, &current).permitted);
    CHECK(control_gate(open, &current).permitted);
    CHECK(data_gate(open, &current).permitted);

    auto shut = open;
    shut.garbage_due = false;
    CHECK(tombstone_gate(shut, &current).reason == "not due");
    shut = open;
    shut.rebuilt_inventory = true;
    CHECK(tombstone_gate(shut, &current).reason == "inventory rebuilt this pass");
    CHECK(control_gate(shut, &current).reason == "inventory rebuilt this pass");
    shut = open;
    shut.reachable = false;
    CHECK(tombstone_gate(shut, &current).reason == "not every known node reachable");
    CHECK(control_gate(shut, &current).reason == "destructive GC not enabled");
    shut = open;
    shut.metadata_stable = false;
    CHECK(tombstone_gate(shut, &current).reason == "metadata not stable");
    const InventoryHorizon incomplete(5, false, {}, {}, {});
    CHECK(tombstone_gate(open, &incomplete).reason == "catalogue inventory incomplete");
    CHECK(control_gate(open, &incomplete).reason == "catalogue inventory incomplete");
    CHECK(control_gate(open, nullptr).reason == "no reachability inventory");
    const InventoryHorizon behind(4, true, {}, {}, {});
    CHECK(control_gate(open, &behind).reason == "inventory generation behind known");
    shut = open;
    shut.gc_due = false;
    CHECK(control_gate(shut, &current).reason == "quiet window");
    shut.busy = true;
    CHECK(control_gate(shut, &current).reason == "foreground busy");
}

} // namespace

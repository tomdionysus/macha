// SPDX-License-Identifier: GPL-3.0-or-later
#include "contract/object_ledger.hpp"
#include "contract/predicates.hpp"

#include <algorithm>
#include <stdexcept>

namespace macha {

namespace {

ObjectFacts facts_of(const ObjectId& id, bool referenced, const ObjectLedger& ledger,
                     const Placement& placement) {
    return {referenced, ledger.held(RetentionClass::data, id), placement.owns(id),
            ledger.retained(RetentionClass::data, id)};
}

// Walks the inventory's referenced DATA ids after the cursor.
Page<ObjectId, ObjectId> query_referenced(Predicate predicate, const ObjectLedger& ledger,
                                          const Placement& placement, Cursor<ObjectId> from,
                                          Budget& budget) {
    Page<ObjectId, ObjectId> page;
    page.next = from;
    const auto inventory = ledger.inventory();
    if (!inventory) {
        page.next = {};
        return page;
    }
    const auto ids = inventory->referenced_ids(RetentionClass::data);
    auto it = from.after ? std::upper_bound(ids.begin(), ids.end(), *from.after) : ids.begin();
    for (;; ++it) {
        if (const auto stop = budget.must_stop()) {
            page.stopped = *stop;
            return page;
        }
        if (it == ids.end()) {
            page.next = {};
            page.stopped = Stop::end;
            return page;
        }
        if (!budget.take_operation()) {
            page.stopped = Stop::budget;
            return page;
        }
        if (satisfies(predicate, facts_of(*it, true, ledger, placement)))
            page.items.push_back(*it);
        page.next.after = *it;
    }
}

// Pages the ledger's DATA claims and keeps those the release horizon no
// longer refers to. Without a release horizon nothing is releasable.
Page<ObjectId, ObjectId> query_releasable(const ObjectLedger& ledger, const Placement& placement,
                                          Cursor<ObjectId> from, Budget& budget) {
    const auto release = ledger.release();
    if (!release) {
        Page<ObjectId, ObjectId> page;
        return page;
    }
    auto page = ledger.claimed(RetentionClass::data, from, budget);
    std::erase_if(page.items, [&](const ObjectId& id) {
        return !satisfies(Predicate::releasable,
                          facts_of(id, release->referenced(RetentionClass::data, id), ledger,
                                   placement));
    });
    return page;
}

} // namespace

Page<ObjectId, ObjectId> query(Predicate predicate, const ObjectLedger& ledger,
                               const Placement& placement, Cursor<ObjectId> from,
                               Budget& budget) {
    switch (predicate) {
    case Predicate::releasable:
        return query_releasable(ledger, placement, from, budget);
    case Predicate::garbage:
        throw std::invalid_argument("garbage is not queryable before the store can walk what it holds");
    default:
        return query_referenced(predicate, ledger, placement, from, budget);
    }
}

} // namespace macha

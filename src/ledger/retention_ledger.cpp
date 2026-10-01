// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/retention_ledger.hpp"

namespace macha {

Page<ObjectId, ObjectId> RetentionLedger::claimed(RetentionClass type, Cursor<ObjectId> from,
                                                  Budget& budget) const {
    Page<ObjectId, ObjectId> page;
    page.next = from;
    std::optional<ObjectId> position = from.after;
    for (;;) {
        if (const auto stop = budget.must_stop()) {
            page.stopped = *stop;
            return page;
        }
        if (!budget.take_operation()) {
            page.stopped = Stop::budget;
            return page;
        }
        bool complete = false;
        const auto id = claims_.next_retained(type, position, complete);
        if (!id) {
            page.next = {};
            page.stopped = Stop::end;
            return page;
        }
        page.items.push_back(*id);
        page.next.after = *id;
    }
}

bool RetentionLedger::held(RetentionClass type, const ObjectId& id) const {
    return type == RetentionClass::data ? data_.has(id) : control_.has(id);
}

void RetentionLedger::publish(InventoryHandle inventory) { inventory_.publish(std::move(inventory)); }

bool RetentionLedger::publish(ReleaseBuild build) {
    if (!build.complete)
        return false;
    release_.publish(std::move(build.horizon));
    return true;
}

} // namespace macha

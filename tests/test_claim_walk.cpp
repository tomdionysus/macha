// SPDX-License-Identifier: GPL-3.0-or-later
//
// RetentionLedger is checked against the RetentionStore it pages; ClaimWalk is
// checked step by step against a reference walk over fakes, for every held
// pattern and every credit limit that reaches a different decision.
#include "contract/claim_store.hpp"
#include "crypto.hpp"
#include "ledger/retention_ledger.hpp"
#include "service/claim_walk.hpp"
#include "test_backend_support.hpp"
#include "test_framework.hpp"

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
    HeldView held_view() const override {
        return {};
    }
    std::set<ObjectId> objects;
    mutable size_t asked{};
    bool has(const ObjectId& id) const override {
        ++asked;
        return objects.contains(id);
    }
};

// Two deletes of one id: the mark lasts until the last of them lets go.
MACHA_FAST_TEST("claim_walk", test_a_delete_mark_lasts_until_the_last_delete_of_the_id_ends) {
    ClaimLocks locks;
    const auto id = id_of(7);
    CHECK(!locks.deleting(id));
    std::optional<ClaimDeletion> first(locks.begin_deleting(id));
    {
        auto second = locks.begin_deleting(id);
        CHECK(locks.deleting(id));
    }
    CHECK(locks.deleting(id));
    first.reset();
    CHECK(!locks.deleting(id));
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
    HeldView held_view(RetentionClass) const override {
        return {};
    }
    // The claim walk reads no horizon.
    InventoryHandle inventory() const override {
        return {};
    }
    ReleaseHandle release() const override {
        return {};
    }
    void publish(InventoryHandle) override {}
    bool publish(ReleaseBuild) override {
        return false;
    }
    bool retained(RetentionClass, const ObjectId&) const override {
        return false;
    }
    size_t release_unreferenced(RetentionClass, const ReleaseHorizon&, size_t) override {
        return 0;
    }
    size_t prune_unclaimed(RetentionClass, size_t) override {
        return 0;
    }
    bool compact_if_needed(size_t) override {
        return false;
    }
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

} // namespace

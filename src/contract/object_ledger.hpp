// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/object_store.hpp"
#include "contract/walk.hpp"
#include "storage/retention.hpp"

// Edges by child (the object ledger spec, B3), as much of it as the claim
// walk needs: which objects this node has promised to keep (claimed), and
// whether it holds them (held). Implemented at stage 0 by RetentionLedger
// over today's RetentionStore and object stores.
namespace macha {

class ObjectLedger {
  public:
    virtual ~ObjectLedger() = default;

    // The claimed objects of one class after `from`, in id order, as far as
    // the budget allows: one budget operation per claim returned. The page's
    // next cursor resumes after its last item; at the end of the claims it
    // is the start again and the page is complete. Thread-safe; waits on
    // nothing (the claim map is in memory).
    static constexpr Waits claimed_waits = Waits::none;
    virtual Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                             Budget&) const = 0;

    // Whether this node holds the object: the class's object store. Waits
    // as ObjectStore::has does.
    static constexpr Waits held_waits = ObjectStore::has_waits;
    virtual bool held(RetentionClass, const ObjectId&) const = 0;
};

} // namespace macha

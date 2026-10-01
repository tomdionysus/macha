// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/horizon.hpp"
#include "contract/object_store.hpp"
#include "contract/walk.hpp"
#include "storage/retention.hpp"

// Edges by child (the object ledger spec, B3): which objects this node has
// promised to keep (claimed), whether it holds them (held), and what the
// two horizons refer to. Implemented at stage 0 by RetentionLedger over
// today's RetentionStore and object stores. The ledger holds horizons; the
// horizon builder (B4) derives them and the maintenance pass publishes them.
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

    // The published horizons: null until the first publish of each. A
    // handle keeps its snapshot for as long as it is held; a publish never
    // changes it. Thread-safe; waits on nothing but a pointer copy.
    static constexpr Waits horizon_waits = Waits::none;
    using InventoryHandle = std::shared_ptr<const InventoryHorizon>;
    using ReleaseHandle = std::shared_ptr<const ReleaseHorizon>;
    virtual InventoryHandle inventory() const = 0;
    virtual ReleaseHandle release() const = 0;

    // Publishes a horizon. A release horizon is published only when its
    // build was complete: an incomplete one is refused and the previous kept,
    // so a published release horizon is always complete. Returns whether it
    // was published. Single owner (the maintenance pass); waits on nothing.
    static constexpr Waits publish_waits = Waits::none;
    virtual void publish(InventoryHandle) = 0;
    virtual bool publish(ReleaseBuild) = 0;
};

} // namespace macha

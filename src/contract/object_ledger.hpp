// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/claim_store.hpp"
#include "contract/horizon.hpp"
#include "contract/object_store.hpp"
#include "contract/walk.hpp"
#include "storage/retention.hpp"

// Edges by child (object ledger spec, B3): which objects this node has
// promised to keep (claimed), whether it holds them (held), and what the two
// horizons refer to. Implemented by RetentionLedger over RetentionStore and
// the object stores. The horizon builder (B4) derives horizons, the
// maintenance pass publishes them, the ledger holds them.
namespace macha {

class ObjectLedger {
  public:
    virtual ~ObjectLedger() = default;

    // The claimed objects of one class after `from`, in id order, one budget
    // operation per claim. At the end the next cursor is the start and the
    // page is complete. Thread-safe; waits on nothing (the map is in memory).
    static constexpr Waits claimed_waits = Waits::none;
    virtual Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                             Budget&) const = 0;

    // Whether this node holds the object, from the class's object store.
    static constexpr Waits held_waits = ObjectStore::has_waits;
    virtual bool held(RetentionClass, const ObjectId&) const = 0;

    // The published horizons, null until first published. A held handle's
    // snapshot never changes. Thread-safe; waits on nothing but a pointer copy.
    static constexpr Waits horizon_waits = Waits::none;
    using InventoryHandle = std::shared_ptr<const InventoryHorizon>;
    using ReleaseHandle = std::shared_ptr<const ReleaseHorizon>;
    virtual InventoryHandle inventory() const = 0;
    virtual ReleaseHandle release() const = 0;

    // Publishes a horizon. An incomplete release build is refused and the
    // previous horizon kept; returns whether it was published. Single owner
    // (the maintenance pass); waits on nothing.
    static constexpr Waits publish_waits = Waits::none;
    virtual void publish(InventoryHandle) = 0;
    virtual bool publish(ReleaseBuild) = 0;

    // Claims, forwarded to the node's ClaimStore.
    // Whether the object is claimed. Thread-safe; waits on nothing.
    virtual bool retained(RetentionClass, const ObjectId&) const = 0;
    // Releases claims `release` no longer refers to and whose writes its
    // clock has observed. Bounded; waits on the state device.
    static constexpr Waits release_waits = ClaimStore::write_waits;
    virtual size_t release_unreferenced(RetentionClass, const ReleaseHorizon& release,
                                        size_t operation_budget) = 0;
    // Forgets causality tombstones once no claim remains and the object is
    // not held. Bounded; waits as held() and on the state device.
    static constexpr Waits prune_waits = held_waits | ClaimStore::write_waits;
    virtual size_t prune_unclaimed(RetentionClass, size_t operation_budget) = 0;
    // Compacts the claims' journal past a threshold. Waits on the state device.
    static constexpr Waits compact_waits = ClaimStore::write_waits;
    virtual bool compact_if_needed(size_t record_threshold) = 0;
};

} // namespace macha

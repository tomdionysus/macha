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
    // page is complete. A claim read per item.
    static constexpr Waits claimed_waits = ClaimStore::read_waits;
    static constexpr ThreadSafety claimed_safety = ThreadSafety::thread_safe;
    virtual Page<ObjectId, ObjectId> claimed(RetentionClass, Cursor<ObjectId> from,
                                             Budget&) const = 0;

    // Whether this node holds the object, from the class's object store.
    static constexpr Waits held_waits = ObjectStore::has_waits;
    static constexpr ThreadSafety held_safety = ThreadSafety::thread_safe;
    virtual bool held(RetentionClass, const ObjectId&) const = 0;
    // The class's object store's losses(): advances when something held may
    // no longer be.
    virtual uint64_t held_losses(RetentionClass) const noexcept = 0;
    // The class's object store's indexed(): held() costs no device read.
    virtual bool held_indexed(RetentionClass) const noexcept = 0;

    // The published horizons, null until first published. A held handle's
    // snapshot never changes. A pointer copy.
    static constexpr Waits horizon_waits = Waits::none;
    static constexpr ThreadSafety horizon_safety = ThreadSafety::thread_safe;
    using InventoryHandle = std::shared_ptr<const InventoryHorizon>;
    using ReleaseHandle = std::shared_ptr<const ReleaseHorizon>;
    virtual InventoryHandle inventory() const = 0;
    virtual ReleaseHandle release() const = 0;

    // Publishes a horizon. An incomplete release build is refused and the
    // previous horizon kept; returns whether it was published. A null handle
    // throws. Single owner (the maintenance pass).
    static constexpr Waits publish_waits = Waits::none;
    static constexpr ThreadSafety publish_safety = ThreadSafety::single_owner;
    virtual void publish(InventoryHandle) = 0;
    virtual bool publish(ReleaseBuild) = 0;

    // Claims, forwarded to the node's ClaimStore.
    // Whether the object is claimed.
    static constexpr Waits retained_waits = ClaimStore::read_waits;
    static constexpr ThreadSafety retained_safety = ThreadSafety::thread_safe;
    virtual bool retained(RetentionClass, const ObjectId&) const = 0;
    // Releases claims `release` no longer refers to and whose writes its
    // clock has observed. Bounded. Single owner (the maintenance pass).
    static constexpr Waits release_waits = ClaimStore::release_waits;
    static constexpr ThreadSafety release_safety = ClaimStore::release_safety;
    virtual size_t release_unreferenced(RetentionClass, const ReleaseHorizon& release,
                                        size_t operation_budget) = 0;
    // Forgets causality tombstones once no claim remains and the object is
    // not held. Bounded. Single owner (the maintenance pass).
    static constexpr Waits prune_waits = held_waits | ClaimStore::prune_waits;
    static constexpr ThreadSafety prune_safety = ClaimStore::prune_safety;
    virtual size_t prune_unclaimed(RetentionClass, size_t operation_budget) = 0;
    // Compacts the claims' journal past a threshold.
    static constexpr Waits compact_waits = ClaimStore::compact_waits;
    static constexpr ThreadSafety compact_safety = ClaimStore::compact_safety;
    virtual bool compact_if_needed(size_t record_threshold) = 0;
};

} // namespace macha

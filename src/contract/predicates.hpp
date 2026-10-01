// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/walk.hpp"
#include "contract/work.hpp"
#include "types.hpp"

#include <cstdint>

// The ledger's predicate queries (the object ledger spec, B3): what later
// stages ask of one DATA object, from four facts. Implemented and tested at
// stage 0, called by nothing: routing repair or GC through them would change
// what they visit and in what order.
namespace macha {

class ObjectLedger;

// Where the object stands on this node. `referenced` is against the
// inventory horizon, except for `releasable`, which reads the release
// horizon.
struct ObjectFacts {
    bool referenced{};
    bool held{};
    bool owned{};
    bool claimed{};
};

enum class Predicate : uint8_t {
    to_pull,      // referenced, owned, not held: fetch it
    missing_here, // referenced, claimed, not held: a promise this node cannot keep
    held_owned,   // referenced, owned, held: a copy placement wants here
    to_push_from, // referenced, held, not owned: a copy that belongs elsewhere
    surplus,      // referenced, held, not owned, not claimed: droppable once owners hold it
    releasable,   // claimed, not referenced at the release horizon
    garbage,      // held, not referenced, not claimed
};

constexpr bool satisfies(Predicate predicate, const ObjectFacts& f) noexcept {
    switch (predicate) {
    case Predicate::to_pull:
        return f.referenced && f.owned && !f.held;
    case Predicate::missing_here:
        return f.referenced && f.claimed && !f.held;
    case Predicate::held_owned:
        return f.referenced && f.owned && f.held;
    case Predicate::to_push_from:
        return f.referenced && f.held && !f.owned;
    case Predicate::surplus:
        return f.referenced && f.held && !f.owned && !f.claimed;
    case Predicate::releasable:
        return f.claimed && !f.referenced;
    case Predicate::garbage:
        return f.held && !f.referenced && !f.claimed;
    }
    return false;
}

// Which nodes placement wants an object on; here, whether this node is one.
// Implemented by DistributedStore (its deterministic owner set).
class Placement {
  public:
    virtual ~Placement() = default;
    // Reads the membership under its lock; no I/O.
    static constexpr Waits owns_waits = Waits::locks;
    virtual bool owns(const ObjectId&) const = 0;
};

// The DATA ids satisfying `predicate`, in id order, from the set it is
// defined over: the inventory's referenced ids for every predicate but
// `releasable`, which pages the ledger's DATA claims and reads the release
// horizon. One budget operation per id examined; the page resumes after its
// cursor. With no horizon published there is nothing to examine and the
// page is complete. `garbage` is not queryable at stage 0 (it needs a walk
// of what the store holds, which B1 adds later) and throws
// std::invalid_argument.
Page<ObjectId, ObjectId> query(Predicate, const ObjectLedger&, const Placement&,
                               Cursor<ObjectId> from, Budget&);

} // namespace macha

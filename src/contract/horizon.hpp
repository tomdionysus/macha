// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "metadata/metadata.hpp"
#include "storage/retention.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// The ledger's two reachability horizons (object ledger spec, B3): what one
// metadata head refers to, per retention class, as an immutable snapshot
// published per A2. A refresh publishes a new snapshot, never changing one
// in use.
namespace macha {

// The objects a head refers to, one sorted, duplicate-free set per class.
class ReferencedSets {
  public:
    // Takes ids in any order, with duplicates; keeps them sorted and unique.
    ReferencedSets(std::vector<ObjectId> data, std::vector<ObjectId> control);

    bool referenced(RetentionClass, const ObjectId&) const;
    // In id order.
    std::span<const ObjectId> referenced_ids(RetentionClass) const;
    size_t size(RetentionClass) const;

  private:
    const std::vector<ObjectId>& of(RetentionClass) const;

    std::vector<ObjectId> data_;
    std::vector<ObjectId> control_;
};

// The maintenance inventory at a metadata generation: what repair,
// tombstone collection, control GC and the DATA sweep act on.
class InventoryHorizon : public ReferencedSets {
  public:
    // `garbage` is every tombstone the namespace walk found; one whose id is
    // still referenced as data is stale (its object came back to life), the
    // rest collectable. Both lists keep the given order.
    InventoryHorizon(uint64_t generation, bool catalogue_complete, std::vector<ObjectId> data,
                     std::vector<ObjectId> control, const std::vector<GarbageRef>& garbage);

    // The stamp: the generation the inventory was built at.
    uint64_t generation() const noexcept { return generation_; }
    bool catalogue_complete() const noexcept { return catalogue_complete_; }
    const std::vector<GarbageRef>& garbage() const noexcept { return garbage_; }
    const std::vector<GarbageRef>& stale_garbage() const noexcept { return stale_garbage_; }

  private:
    uint64_t generation_;
    bool catalogue_complete_;
    std::vector<GarbageRef> garbage_;
    std::vector<GarbageRef> stale_garbage_;
};

// The retention release horizon at the sole accepted head: what claims are
// released against. Only a complete horizon is published.
class ReleaseHorizon : public ReferencedSets {
  public:
    ReleaseHorizon(Hash256 head, RetentionClock clock, std::vector<ObjectId> data,
                   std::vector<ObjectId> control);

    // The stamp: the head's hash and the mutation clock of the claims it saw.
    const Hash256& head() const noexcept { return head_; }
    const RetentionClock& clock() const noexcept { return clock_; }

  private:
    Hash256 head_;
    RetentionClock clock_;
};

// A release horizon as built; complete when every catalogue root and tree
// node it needed was readable.
struct ReleaseBuild {
    std::shared_ptr<const ReleaseHorizon> horizon;
    bool complete{};
};

} // namespace macha

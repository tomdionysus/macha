// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/horizon.hpp"
#include "contract/work.hpp"
#include "filesystem/filesystem.hpp"
#include "metadata/metadata_manager.hpp"

#include <memory>

// The horizon builder (the object ledger spec, B4): what a metadata head
// refers to, derived. The maintenance pass decides when to build; the
// builder derives; the ledger holds what is published. Only the pass calls
// it. Single owner (the pass's thread), background class.
namespace macha {

class HorizonBuilder {
  public:
    virtual ~HorizonBuilder() = default;

    // The namespace's own objects and tombstones at the current metadata
    // generation: the filesystem's cached walk, cheap while the generation
    // has not moved. The pass reads its generation to decide whether to
    // build an inventory from it.
    static constexpr Waits namespace_objects_waits = Waits::state_device | Waits::network;
    virtual std::shared_ptr<const MaintenanceObjects> namespace_objects() = 0;

    // The inventory at that generation: the namespace's objects and the
    // catalogue's. Until T4 this runs the catalogue's repair first (spec A4,
    // the one side effect a build has at stage 0).
    static constexpr Waits inventory_waits = Waits::state_device | Waits::network;
    virtual std::shared_ptr<const InventoryHorizon> inventory(const MaintenanceObjects&) = 0;

    // The release horizon at a head: its files' extents, the conflict roots,
    // every catalogue root's retained objects and the namespace tree's own
    // nodes. Incomplete when a catalogue root or a tree node is unreadable.
    static constexpr Waits release_waits = Waits::state_device | Waits::network;
    virtual ReleaseBuild release(const MetadataSnapshotView& head) = 0;
};

} // namespace macha

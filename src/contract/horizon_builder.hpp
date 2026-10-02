// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "contract/horizon.hpp"
#include "contract/work.hpp"
#include "filesystem/filesystem.hpp"
#include "metadata/metadata_manager.hpp"

#include <memory>

// The horizon builder (object ledger spec, B4): derives what a metadata head
// refers to. The maintenance pass decides when to build and is the only
// caller; the ledger holds what is published. Single owner (the pass's
// thread), background class.
namespace macha {

class HorizonBuilder {
  public:
    virtual ~HorizonBuilder() = default;

    // The namespace's objects and tombstones at the current metadata
    // generation: the filesystem's cached walk, cheap while the generation
    // holds. The pass reads its generation to decide whether to build an
    // inventory from it.
    static constexpr Waits namespace_objects_waits = Waits::state_device | Waits::network;
    virtual std::shared_ptr<const MaintenanceObjects> namespace_objects() = 0;

    // The inventory at that generation: the namespace's objects and the
    // catalogue's, read against `head` (captured before the catalogue's
    // repair ran) and whether that repair succeeded. Fetches missing
    // catalogue objects into the control store; repairs and commits nothing
    // (spec A4).
    static constexpr Waits inventory_waits = Waits::state_device | Waits::network;
    virtual std::shared_ptr<const InventoryHorizon>
    inventory(const MaintenanceObjects&, const CatalogueMaintenanceHead& head, bool repaired) = 0;

    // The release horizon at a head: its files' extents, the conflict roots,
    // every catalogue root's retained objects and the namespace tree's own
    // nodes. Incomplete when a catalogue root or a tree node is unreadable.
    static constexpr Waits release_waits = Waits::state_device | Waits::network;
    virtual ReleaseBuild release(const MetadataSnapshotView& head) = 0;
};

} // namespace macha

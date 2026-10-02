// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "contract/horizon_builder.hpp"
#include "metadata/namespace_tree.hpp"

#include <functional>

// The horizon builder over the node's filesystem, catalogue and control
// store, delegating to two builds that are pure functions of their inputs.
namespace macha {

class DistributedStore;
class NodeRuntime;

// Data is both live sets, control the catalogue's, tombstones the namespace's.
std::shared_ptr<const InventoryHorizon> build_inventory(const MaintenanceObjects& namespace_objects,
                                                        const CatalogueMaintenance& catalogue);

// One catalogue root's retained objects; throws when the root is unreadable.
using CatalogueRetentionSource = std::function<CatalogueRetentionObjects(const ObjectId& root)>;

// The release horizon at `head`, reading tree nodes from `nodes`; incomplete
// when a catalogue root or tree node cannot be read.
ReleaseBuild build_release(const MetadataSnapshotView& head, const NamespaceNodeStore& nodes,
                           const CatalogueRetentionSource& catalogue);

class NodeHorizonBuilder final : public HorizonBuilder {
  public:
    NodeHorizonBuilder(FileSystem&, CatalogueManager&, LocalStore& control, DistributedStore&) noexcept;

    std::shared_ptr<const MaintenanceObjects> namespace_objects() override;
    std::shared_ptr<const InventoryHorizon> inventory(const MaintenanceObjects&,
                                                      const CatalogueMaintenanceHead& head,
                                                      bool repaired) override;
    ReleaseBuild release(const MetadataSnapshotView& head) override;

  private:
    FileSystem& filesystem_;
    CatalogueManager& catalogue_;
    LocalStore& control_;
    DistributedStore& store_;
};

} // namespace macha

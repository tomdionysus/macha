// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/node_horizon_builder.hpp"

#include "log.hpp"
#include "metadata/namespace_control_store.hpp"

#include <utility>

namespace macha {

std::shared_ptr<const InventoryHorizon> build_inventory(const MaintenanceObjects& namespace_objects,
                                                        const CatalogueMaintenance& catalogue) {
    std::vector<ObjectId> data = namespace_objects.live;
    data.insert(data.end(), catalogue.live.begin(), catalogue.live.end());
    std::vector<ObjectId> control(catalogue.control_live.begin(), catalogue.control_live.end());
    return std::make_shared<const InventoryHorizon>(namespace_objects.metadata_generation,
                                                    catalogue.complete, std::move(data),
                                                    std::move(control), namespace_objects.garbage);
}

ReleaseBuild build_release(const MetadataSnapshotView& head, const NamespaceNodeStore& nodes,
                           const CatalogueRetentionSource& catalogue) {
    std::vector<ObjectId> data;
    std::vector<ObjectId> control;
    bool complete = true;
    // The live set destructive GC acts on: an empty one means "collect
    // everything".
    for_each_namespace_entry(*head.snapshot, &nodes, [&](const std::string&, const FsEntry& entry) {
        if (entry.type != EntryType::file)
            return;
        for (const auto& extent_ref : entry.extents)
            if (!extent_ref.hole)
                data.push_back(extent_ref.id);
    });
    const auto conflict_extents = metadata_conflict_extent_roots(*head.snapshot);
    data.insert(data.end(), conflict_extents.begin(), conflict_extents.end());

    for (const auto& root : metadata_catalogue_root_set(*head.snapshot)) {
        try {
            auto retained = catalogue(root);
            data.insert(data.end(), retained.data.begin(), retained.data.end());
            control.insert(control.end(), retained.control.begin(), retained.control.end());
        } catch (const std::exception& error) {
            complete = false;
            Log::debug("retention release horizon catalogue unavailable root=" + to_string(root) +
                       " error=" + error.what());
        }
    }
    // The namespace tree's own nodes: omitting them would let the collector
    // delete the namespace, so an unreadable node marks the whole set
    // incomplete and nothing is released against it.
    if (head.snapshot->namespace_root) {
        try {
            collect_namespace_tree_nodes(*head.snapshot->namespace_root, nodes, control);
        } catch (const std::exception& error) {
            complete = false;
            Log::warn("retention release horizon namespace tree unavailable root=" +
                      to_string(*head.snapshot->namespace_root) + " error=" + error.what());
        }
    }
    return {std::make_shared<const ReleaseHorizon>(head.hash, head.snapshot->mutation_sequences,
                                                   std::move(data), std::move(control)),
            complete};
}

NodeHorizonBuilder::NodeHorizonBuilder(FileSystem& filesystem, CatalogueManager& catalogue,
                                       NodeRuntime& node, DistributedStore& store) noexcept
    : filesystem_(filesystem), catalogue_(catalogue), node_(node), store_(store) {}

std::shared_ptr<const MaintenanceObjects> NodeHorizonBuilder::namespace_objects() {
    return filesystem_.maintenance_objects_cached();
}

std::shared_ptr<const InventoryHorizon>
NodeHorizonBuilder::inventory(const MaintenanceObjects& namespace_objects,
                              const CatalogueMaintenanceHead& head, bool repaired) {
    return build_inventory(namespace_objects, catalogue_.maintenance_objects(head, repaired));
}

ReleaseBuild NodeHorizonBuilder::release(const MetadataSnapshotView& head) {
    auto nodes = ControlNamespaceNodeStore::for_reading(node_, store_);
    return build_release(head, nodes, [this](const ObjectId& root) {
        return catalogue_.retention_objects(std::nullopt, root);
    });
}

} // namespace macha

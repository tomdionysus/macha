// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "contract/metadata_view.hpp"
#include "filesystem/filesystem.hpp"
#include "metadata/namespace_tree.hpp"

#include "dagda/contract/reachability.h"

#include <functional>
#include <optional>

namespace dagda {
class Context;
}

namespace macha {

class MetadataServer;

// The catalogue's DATA and CONTROL objects under one root; throws when the
// root cannot be read.
using CatalogueRetentionSource = std::function<CatalogueRetentionObjects(const ObjectId& root)>;

// The inventory's answer: the namespace's counted views, its conflict
// alternatives and tombstones, the catalogue's sets; complete when the
// catalogue's half is.
dagda::contract::Reachability reachability_inventory(const MaintenanceObjects& namespace_objects,
                                                     const CatalogueMaintenance& catalogue);
// The release's answer at `head`: the namespace's references (`counted` at
// its tree when given, else a walk through `nodes`), its conflict
// alternatives, every catalogue root it names and its tombstone batches,
// stamped with the head and its clock. Incomplete, never shorter, when a
// catalogue root or a tree node cannot be read.
dagda::contract::Reachability reachability_release(const MetadataSnapshotView& head,
                                                   const NamespaceNodeStore& nodes,
                                                   const CatalogueRetentionSource& catalogue,
                                                   const NamespaceReferences* counted = nullptr);

// What Macha's state refers to, as the store node's collector asks it: the
// namespace (its counted extents and tree nodes, its conflict alternatives,
// its tombstones and their batches) and the catalogue (its DATA and CONTROL
// objects, each root a head names). Registered by the root for Macha's domain;
// the collector alone calls it, on Macha's maintenance thread.
class MachaReachability final : public dagda::contract::ReachabilityProvider {
  public:
    MachaReachability(FileSystem&, CatalogueManager&, MetadataView&, MetadataServer&,
                      dagda::Context& store) noexcept;

    uint64_t generation() override;
    uint64_t known_generation() override;
    dagda::contract::Reachability inventory(const std::function<void()>& pause) override;
    std::optional<Hash256> release_head() override;
    dagda::contract::Reachability release(const Hash256& head) override;
    std::optional<dagda::contract::ControlEpoch> control_epoch() override;
    bool within_control_epoch(uint64_t sequence, const std::function<void()>& removal) override;
    void collect_garbage(const dagda::contract::GarbageCollection&) override;

  private:
    // Drops the batches holding a retirement to erase; the rest of each is
    // written again at its own time. A later retirement of the same object is
    // another batch and is kept.
    void erase_retirements(const std::vector<dagda::contract::Garbage>& erase);
    // Drops batches whose grace has run and that the head still names.
    void drop_batches(const std::vector<ObjectId>& matured);

    FileSystem& filesystem_;
    CatalogueManager& catalogue_;
    MetadataView& metadata_;
    MetadataServer& metadata_server_;
    dagda::Context& store_;
};

} // namespace macha

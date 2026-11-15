// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/macha_reachability.hpp"

#include "filesystem/filesystem.hpp"
#include "log.hpp"
#include "metadata/metadata_server.hpp"
#include "metadata/namespace_control_store.hpp"

#include "dagda/context.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace macha {
namespace {

dagda::contract::Garbage as_garbage(const GarbageRef& ref) {
    return {ref.id, ref.retired_at_ns, ref.retirement_id};
}

dagda::contract::GarbageBatch as_batch(const TombstoneBatch& batch) {
    return {batch.id, batch.retired_at_ns, batch.count};
}

} // namespace

dagda::contract::Reachability reachability_inventory(const MaintenanceObjects& namespace_objects,
                                                     const CatalogueMaintenance& catalogue) {
    dagda::contract::Reachability out;
    // The namespace's references are its counts' frozen views; the
    // catalogue's objects and the conflict alternatives are held beside them.
    // The tree's nodes are in the counted nodes: without them the collector
    // would delete the namespace.
    out.views = {namespace_objects.referenced_extents, namespace_objects.referenced_nodes};
    out.data = namespace_objects.conflict_live;
    out.data.insert(out.data.end(), catalogue.live.begin(), catalogue.live.end());
    out.control.assign(catalogue.control_live.begin(), catalogue.control_live.end());
    out.garbage.reserve(namespace_objects.garbage.size());
    for (const auto& ref : namespace_objects.garbage)
        out.garbage.push_back(as_garbage(ref));
    for (const auto& batch : namespace_objects.tombstone_batches)
        out.batches.push_back(as_batch(batch));
    out.complete = catalogue.complete;
    out.generation = namespace_objects.metadata_generation;
    out.entries = namespace_objects.entries;
    out.extents = namespace_objects.extents;
    return out;
}

dagda::contract::Reachability reachability_release(const MetadataSnapshotView& head,
                                                   const NamespaceNodeStore& nodes,
                                                   const CatalogueRetentionSource& catalogue,
                                                   const NamespaceReferences* counted) {
    dagda::contract::Reachability out;
    const auto& snapshot = *head.snapshot;
    out.head = head.hash;
    out.clock = snapshot.mutation_sequences;
    out.complete = true;
    if (counted) {
        out.views = {counted->extents, counted->nodes};
    } else {
        for_each_namespace_entry(snapshot, &nodes, [&](const std::string&, const FsEntry& entry) {
            if (entry.type != EntryType::file)
                return;
            for (const auto& extent : entry.extents)
                if (!extent.hole)
                    out.data.push_back(extent.id);
        });
    }
    const auto conflict_extents = metadata_conflict_extent_roots(snapshot);
    out.data.insert(out.data.end(), conflict_extents.begin(), conflict_extents.end());
    for (const auto& root : metadata_catalogue_root_set(snapshot)) {
        try {
            auto retained = catalogue(root);
            out.data.insert(out.data.end(), retained.data.begin(), retained.data.end());
            out.control.insert(out.control.end(), retained.control.begin(), retained.control.end());
        } catch (const std::exception& error) {
            out.complete = false;
            Log::debug("retention release horizon catalogue unavailable root=" + to_string(root) +
                       " error=" + error.what());
        }
    }
    // The tree's own nodes: an unreadable one marks the answer incomplete,
    // and nothing is released against it.
    if (!counted && snapshot.namespace_root) {
        try {
            collect_namespace_tree_nodes(*snapshot.namespace_root, nodes, out.control);
        } catch (const std::exception& error) {
            out.complete = false;
            Log::warn("retention release horizon namespace tree unavailable root=" +
                      to_string(*snapshot.namespace_root) + " error=" + error.what());
        }
    }
    for (const auto& batch : snapshot.tombstone_batches)
        out.control.push_back(batch.id);
    return out;
}

MachaReachability::MachaReachability(FileSystem& filesystem, CatalogueManager& catalogue,
                                     MetadataView& metadata, MetadataServer& metadata_server,
                                     dagda::Context& store) noexcept
    : filesystem_(filesystem), catalogue_(catalogue), metadata_(metadata),
      metadata_server_(metadata_server), store_(store) {}

uint64_t MachaReachability::generation() {
    return metadata_.current_generation();
}

uint64_t MachaReachability::known_generation() {
    return metadata_server_.known_generation();
}

dagda::contract::Reachability MachaReachability::inventory(const std::function<void()>& pause) {
    const auto objects = filesystem_.maintenance_objects_cached(pause);
    return reachability_inventory(*objects,
                                  catalogue_.maintenance_objects(catalogue_.maintenance_head()));
}

std::optional<Hash256> MachaReachability::release_head() {
    const auto head = metadata_.release_head();
    if (!head)
        return {};
    return head->hash;
}

dagda::contract::Reachability MachaReachability::release(const Hash256& head) {
    const auto view = metadata_.release_head();
    // The head moved since it was named: an incomplete answer, never a shorter one.
    if (!view || view->hash != head) {
        dagda::contract::Reachability out;
        out.head = head;
        return out;
    }
    // The filesystem's census brought to this head's tree: no walk.
    std::optional<NamespaceReferences> counted;
    try {
        counted = filesystem_.namespace_references(*view->snapshot);
    } catch (const std::exception& error) {
        Log::debug("retention release horizon walks the namespace: " + std::string(error.what()));
    }
    const auto nodes = ControlNamespaceNodeStore::for_reading(store_);
    return reachability_release(
        *view, nodes,
        [this](const ObjectId& root) { return catalogue_.retention_objects(std::nullopt, root); },
        counted ? &*counted : nullptr);
}

std::optional<dagda::contract::ControlEpoch> MachaReachability::control_epoch() {
    return catalogue_.control_epoch();
}

bool MachaReachability::within_control_epoch(uint64_t sequence,
                                             const std::function<void()>& removal) {
    return catalogue_.within_control_epoch(sequence, removal);
}

void MachaReachability::collect_garbage(const dagda::contract::GarbageCollection& decided) {
    // Every Macha retirement is stamped when it is made (a batch carries its
    // time), so there is nothing to stamp.
    if (!decided.stamp.empty())
        Log::warn("maintenance retirements to stamp count=" + std::to_string(decided.stamp.size()) +
                  "; Macha writes none unstamped");
    erase_retirements(decided.erase);
    drop_batches(decided.matured_batches);
}

void MachaReachability::erase_retirements(const std::vector<dagda::contract::Garbage>& erase) {
    if (erase.empty())
        return;
    std::map<ObjectId, dagda::contract::Garbage> expected;
    for (const auto& candidate : erase)
        expected[candidate.id] = candidate;
    metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        if (snapshot.tombstone_batches.empty())
            return;
        const auto reader = ControlNamespaceNodeStore::for_reading(store_);
        std::vector<TombstoneBatch> kept;
        for (const auto& batch : snapshot.tombstone_batches) {
            const auto bytes = reader.get(batch.id);
            if (!bytes) {
                kept.push_back(batch);
                continue;
            }
            auto content = decode_tombstone_batch(*bytes);
            MetadataDelta::PendingTombstones rest{false, content.retired_at_ns, {}};
            for (const auto& id : content.ids) {
                const auto found = expected.find(id);
                if (found == expected.end() || content.retired_at_ns > found->second.retired_at_ns)
                    rest.ids.push_back(id);
            }
            if (rest.ids.size() == content.ids.size()) {
                kept.push_back(batch);
                continue;
            }
            delta.drop_tombstone_batches.push_back(batch.id);
            if (!rest.ids.empty())
                delta.pending_tombstones.push_back(std::move(rest));
        }
        snapshot.tombstone_batches = std::move(kept);
        std::sort(delta.drop_tombstone_batches.begin(), delta.drop_tombstone_batches.end());
    });
}

void MachaReachability::drop_batches(const std::vector<ObjectId>& matured_ids) {
    std::set<ObjectId> matured(matured_ids.begin(), matured_ids.end());
    // Only what the head still names: the erase may have dropped one already.
    const auto view = metadata_.converged();
    std::erase_if(matured, [&](const ObjectId& id) {
        const auto& named = view.snapshot->tombstone_batches;
        return std::none_of(named.begin(), named.end(),
                            [&](const TombstoneBatch& batch) { return batch.id == id; });
    });
    if (matured.empty())
        return;
    metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        // A batch is immutable, so dropping it by id cannot drop a later
        // retirement of the same objects: that is another batch.
        std::erase_if(snapshot.tombstone_batches, [&](const TombstoneBatch& batch) {
            const bool drop = matured.contains(batch.id);
            if (drop)
                delta.drop_tombstone_batches.push_back(batch.id);
            return drop;
        });
    });
}

} // namespace macha

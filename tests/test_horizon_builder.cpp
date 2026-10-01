// SPDX-License-Identifier: GPL-3.0-or-later
//
// The horizon builder's two builds (the object ledger spec, B4), against fake
// sources: an in-memory namespace tree and a catalogue given as a function.
// Each build is compared with the maintenance pass's own build as it stood
// before the builder existed (src/service/maintenance.cpp at be930c9),
// copied here as the reference.
#include "ledger/node_horizon_builder.hpp"
#include "metadata/namespace_tree.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace macha;

namespace {

ObjectId id(uint8_t n) {
    ObjectId out{};
    out.bytes[0] = n;
    return out;
}

std::vector<ObjectId> ids_of(std::span<const ObjectId> span) { return {span.begin(), span.end()}; }

FsEntry file(std::initializer_list<std::pair<uint8_t, bool>> extents) {
    FsEntry entry;
    entry.type = EntryType::file;
    uint64_t offset = 0;
    for (const auto& [n, hole] : extents) {
        ExtentRef ref;
        ref.offset = offset;
        ref.length = 10;
        ref.id = id(n);
        ref.hole = hole;
        entry.extents.push_back(ref);
        offset += 10;
    }
    return entry;
}

FsEntry directory() {
    FsEntry entry;
    entry.type = EntryType::directory;
    return entry;
}

// Serves every node once, then refuses any node read a second time: the
// namespace walk reads each node once and succeeds, the tree-node collection
// that follows it reads them again and fails.
class OnceNodeStore final : public NamespaceNodeStore {
  public:
    explicit OnceNodeStore(const NamespaceNodeStore& inner) : inner_(inner) {}
    ObjectId put(std::span<const uint8_t>) override { throw std::logic_error("read-only"); }
    std::optional<Bytes> get(const ObjectId& node) const override {
        if (!served_.insert(node).second)
            return std::nullopt;
        return inner_.get(node);
    }

  private:
    const NamespaceNodeStore& inner_;
    mutable std::set<ObjectId> served_;
};

// The pass's release build before the builder, verbatim but for its sources.
struct ReferenceRelease {
    std::vector<ObjectId> data, control;
    bool complete{};
};
ReferenceRelease reference_release(const MetadataSnapshotView& floor_view,
                                   const NamespaceNodeStore& release_nodes,
                                   const CatalogueRetentionSource& catalogue) {
    const auto* floor = &floor_view;
    std::vector<ObjectId> data_live;
    std::vector<ObjectId> control_live;
    bool complete = true;
    for_each_namespace_entry(*floor->snapshot, &release_nodes,
                             [&](const std::string&, const FsEntry& entry) {
                                 if (entry.type != EntryType::file)
                                     return;
                                 for (const auto& extent_ref : entry.extents)
                                     if (!extent_ref.hole)
                                         data_live.push_back(extent_ref.id);
                             });
    const auto conflict_extents = metadata_conflict_extent_roots(*floor->snapshot);
    data_live.insert(data_live.end(), conflict_extents.begin(), conflict_extents.end());
    for (const auto& root : metadata_catalogue_root_set(*floor->snapshot)) {
        try {
            auto retained = catalogue(root);
            data_live.insert(data_live.end(), retained.data.begin(), retained.data.end());
            control_live.insert(control_live.end(), retained.control.begin(),
                                retained.control.end());
        } catch (const std::exception&) {
            complete = false;
        }
    }
    if (floor->snapshot->namespace_root) {
        try {
            collect_namespace_tree_nodes(*floor->snapshot->namespace_root, release_nodes,
                                         control_live);
        } catch (const std::exception&) {
            complete = false;
        }
    }
    std::sort(data_live.begin(), data_live.end());
    data_live.erase(std::unique(data_live.begin(), data_live.end()), data_live.end());
    std::sort(control_live.begin(), control_live.end());
    control_live.erase(std::unique(control_live.begin(), control_live.end()), control_live.end());
    return {data_live, control_live, complete};
}

// A catalogue whose roots hold fixed objects; a root it does not know throws.
CatalogueRetentionSource catalogue_of(std::map<ObjectId, CatalogueRetentionObjects> roots) {
    return [roots = std::move(roots)](const ObjectId& root) {
        const auto found = roots.find(root);
        if (found == roots.end())
            throw std::runtime_error("catalogue root unreadable");
        return found->second;
    };
}

MetadataSnapshotView view_of(MetadataSnapshot snapshot, uint8_t hash) {
    MetadataSnapshotView view;
    view.hash.bytes[0] = hash;
    view.snapshot = std::make_shared<const MetadataSnapshot>(std::move(snapshot));
    return view;
}

void check_matches_reference(const MetadataSnapshotView& head, const NamespaceNodeStore& nodes,
                             const NamespaceNodeStore& reference_nodes,
                             const CatalogueRetentionSource& catalogue) {
    const auto built = build_release(head, nodes, catalogue);
    const auto expected = reference_release(head, reference_nodes, catalogue);
    CHECK(built.complete == expected.complete);
    CHECK(ids_of(built.horizon->referenced_ids(RetentionClass::data)) == expected.data);
    CHECK(ids_of(built.horizon->referenced_ids(RetentionClass::control)) == expected.control);
    CHECK(built.horizon->head() == head.hash);
    CHECK(built.horizon->clock() == head.snapshot->mutation_sequences);
}

MACHA_FAST_TEST("horizon_builder", test_inventory_is_both_live_sets_and_the_catalogue_control_set) {
    MaintenanceObjects namespace_objects;
    namespace_objects.live = {id(3), id(1)};
    namespace_objects.metadata_generation = 9;
    GarbageRef revived;
    revived.id = id(2);
    revived.retired_at_ns = 5;
    GarbageRef dead;
    dead.id = id(8);
    dead.retired_at_ns = 6;
    namespace_objects.garbage = {revived, dead};
    CatalogueMaintenance catalogue;
    catalogue.live = {id(2), id(3)};
    catalogue.control_live = {id(7), id(6)};
    catalogue.complete = false;

    const auto inventory = build_inventory(namespace_objects, catalogue);
    CHECK(inventory->generation() == 9);
    CHECK(!inventory->catalogue_complete());
    CHECK((ids_of(inventory->referenced_ids(RetentionClass::data)) ==
           std::vector<ObjectId>{id(1), id(2), id(3)}));
    CHECK((ids_of(inventory->referenced_ids(RetentionClass::control)) ==
           std::vector<ObjectId>{id(6), id(7)}));
    // A tombstone the catalogue's data revives is stale, as one the namespace
    // revives is.
    CHECK(inventory->stale_garbage().size() == 1 && inventory->stale_garbage()[0].id == id(2));
    CHECK(inventory->garbage().size() == 1 && inventory->garbage()[0].id == id(8));

    catalogue.complete = true;
    CHECK(build_inventory(namespace_objects, catalogue)->catalogue_complete());
}

MACHA_FAST_TEST("horizon_builder", test_release_over_a_tree_matches_the_pass_build) {
    MemoryNamespaceNodeStore nodes;
    const std::map<std::string, FsEntry> entries{
        {"/a", directory()},
        {"/a/one", file({{1, false}, {2, true}, {3, false}})},
        {"/a/two", file({{3, false}, {4, false}})},
        {"/b", file({{5, true}})},
    };
    MetadataSnapshot snapshot;
    snapshot.namespace_root = build_namespace_tree(entries, nodes);
    snapshot.catalogue_root = id(40);
    NodeId writer{};
    writer.bytes[0] = 1;
    snapshot.mutation_sequences[writer] = 17;
    MetadataConflict namespace_conflict;
    namespace_conflict.kind = MetadataConflictKind::namespace_entry;
    namespace_conflict.left_entry = file({{6, false}, {7, true}});
    namespace_conflict.right_entry = file({{1, false}});
    snapshot.conflicts["/c"] = namespace_conflict;
    MetadataConflict catalogue_conflict;
    catalogue_conflict.kind = MetadataConflictKind::catalogue_root;
    catalogue_conflict.left_catalogue_root = id(41);
    snapshot.conflicts["catalogue"] = catalogue_conflict;
    const auto head = view_of(snapshot, 0x51);
    const auto catalogue = catalogue_of({{id(40), {{id(8), id(1)}, {id(9)}}},
                                         {id(41), {{id(10)}, {id(11), id(9)}}}});

    check_matches_reference(head, nodes, nodes, catalogue);
    const auto built = build_release(head, nodes, catalogue);
    CHECK(built.complete);
    CHECK((ids_of(built.horizon->referenced_ids(RetentionClass::data)) ==
           std::vector<ObjectId>{id(1), id(3), id(4), id(6), id(8), id(10)}));
    // The control set is both catalogue roots' control objects and every
    // node of the namespace tree.
    CHECK(built.horizon->referenced(RetentionClass::control, id(9)));
    CHECK(built.horizon->referenced(RetentionClass::control, id(11)));
    CHECK(built.horizon->size(RetentionClass::control) == 2 + nodes.nodes());
}

MACHA_FAST_TEST("horizon_builder", test_an_unreadable_catalogue_root_leaves_the_release_incomplete) {
    MetadataSnapshot snapshot;
    snapshot.entries["/f"] = file({{1, false}});
    snapshot.catalogue_root = id(40);
    MetadataConflict catalogue_conflict;
    catalogue_conflict.kind = MetadataConflictKind::catalogue_root;
    catalogue_conflict.right_catalogue_root = id(42);
    snapshot.conflicts["catalogue"] = catalogue_conflict;
    const auto head = view_of(snapshot, 0x52);
    const auto catalogue = catalogue_of({{id(40), {{id(8)}, {id(9)}}}});
    MemoryNamespaceNodeStore nodes;

    check_matches_reference(head, nodes, nodes, catalogue);
    const auto built = build_release(head, nodes, catalogue);
    CHECK(!built.complete);
    // What could be read is still there: the trace reports its sizes.
    CHECK((ids_of(built.horizon->referenced_ids(RetentionClass::data)) ==
           std::vector<ObjectId>{id(1), id(8)}));
    CHECK((ids_of(built.horizon->referenced_ids(RetentionClass::control)) ==
           std::vector<ObjectId>{id(9)}));
}

MACHA_FAST_TEST("horizon_builder", test_an_unreadable_tree_node_leaves_the_release_incomplete) {
    MemoryNamespaceNodeStore nodes;
    const std::map<std::string, FsEntry> entries{{"/f", file({{1, false}})},
                                                 {"/g", file({{2, false}})}};
    MetadataSnapshot snapshot;
    snapshot.namespace_root = build_namespace_tree(entries, nodes);
    const auto head = view_of(snapshot, 0x53);
    const auto catalogue = catalogue_of({});

    OnceNodeStore once(nodes);
    OnceNodeStore reference_once(nodes);
    check_matches_reference(head, once, reference_once, catalogue);
    OnceNodeStore again(nodes);
    const auto built = build_release(head, again, catalogue);
    CHECK(!built.complete);
    CHECK((ids_of(built.horizon->referenced_ids(RetentionClass::data)) ==
           std::vector<ObjectId>{id(1), id(2)}));
}

MACHA_FAST_TEST("horizon_builder", test_an_empty_head_releases_against_nothing) {
    MemoryNamespaceNodeStore nodes;
    const auto head = view_of(MetadataSnapshot{}, 0x54);
    const auto built = build_release(head, nodes, catalogue_of({}));
    CHECK(built.complete);
    CHECK(built.horizon->size(RetentionClass::data) == 0);
    CHECK(built.horizon->size(RetentionClass::control) == 0);
}

} // namespace

// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "metadata/metadata_manager.hpp"
#include "metadata/namespace_tree.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <vector>
#include <string>

using namespace macha;
using namespace macha::test_support;

namespace {

ObjectId fake_object(uint64_t seed) {
    ObjectId id{};
    for (size_t i = 0; i < id.bytes.size(); ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        id.bytes[i] = static_cast<uint8_t>(seed >> 33);
    }
    return id;
}

FsEntry make_file(uint64_t seed, size_t extents) {
    FsEntry entry;
    entry.type = EntryType::file;
    entry.mode = 0644;
    entry.uid = 1000;
    entry.gid = 1000;
    entry.size = static_cast<uint64_t>(extents) * 4 * 1024 * 1024;
    entry.ctime_ns = static_cast<int64_t>(seed) * 1000;
    entry.mtime_ns = static_cast<int64_t>(seed) * 2000;
    entry.version = 1 + seed % 7;
    entry.extents.reserve(extents);
    for (size_t i = 0; i < extents; ++i) {
        ExtentRef extent;
        extent.offset = static_cast<uint64_t>(i) * 4 * 1024 * 1024;
        extent.length = 4 * 1024 * 1024;
        extent.id = fake_object(seed * 1000 + i);
        entry.extents.push_back(extent);
    }
    return entry;
}

FsEntry make_directory(uint64_t seed) {
    FsEntry entry;
    entry.type = EntryType::directory;
    entry.mode = 0755;
    entry.ctime_ns = static_cast<int64_t>(seed);
    entry.mtime_ns = static_cast<int64_t>(seed);
    return entry;
}

// A media library's shape: deeply nested, unbalanced, a few very large files
// and a long tail of small ones.
std::map<std::string, FsEntry> library(size_t shows, size_t episodes_per_show) {
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/TV"] = make_directory(1);
    for (size_t s = 0; s < shows; ++s) {
        const auto show = "/TV/Show " + std::to_string(s);
        entries[show] = make_directory(100 + s);
        for (size_t e = 0; e < episodes_per_show; ++e) {
            const auto season = show + "/Season " + std::to_string(e % 4 + 1);
            entries[season] = make_directory(200 + s * 10 + e);
            // Every 16th episode has enough extents to force an external list.
            const size_t extents = (s * episodes_per_show + e) % 16 == 0 ? 600 : 3;
            entries[season + "/Episode " + std::to_string(e) + ".mkv"] =
                make_file(1000 + s * 100 + e, extents);
        }
    }
    return entries;
}

MACHA_TEST("namespace_tree", test_the_tree_round_trips_every_namespace_it_is_given) {
    MemoryNamespaceNodeStore store;

    // The empty namespace is a tree like any other.
    const auto empty_root = build_namespace_tree({}, store);
    CHECK(read_namespace_tree(empty_root, store).empty());

    const auto entries = library(12, 9);
    const auto root = build_namespace_tree(entries, store);
    const auto read_back = read_namespace_tree(root, store);
    REQUIRE(read_back.size() == entries.size());
    CHECK(read_back == entries);

    // Externalised extent lists come back whole and in order.
    const auto& original = entries.at("/TV/Show 0/Season 1/Episode 0.mkv");
    REQUIRE(original.extents.size() == 600);
    CHECK(read_back.at("/TV/Show 0/Season 1/Episode 0.mkv").extents == original.extents);
}

// An entry's provenance is part of the tree: it comes back whole through a
// read, a lookup, the stats walk and the node collection, and an entry that
// has none leaves the tree exactly as it was.
MACHA_TEST("namespace_tree", test_the_tree_carries_each_entrys_provenance) {
    MemoryNamespaceNodeStore store;
    auto entries = library(6, 5);
    const auto plain_root = build_namespace_tree(entries, store);

    NodeId author{};
    author.bytes[0] = 7;
    uint64_t sequence = 0;
    for (auto& [path, entry] : entries) {
        // Every third entry stays as it was written before provenance.
        if (++sequence % 3 == 0)
            continue;
        entry.provenance.file_id = legacy_file_id(path);
        entry.provenance.content = {author, sequence};
        entry.provenance.name = {author, sequence / 2 + 1};
    }
    const auto root = build_namespace_tree(entries, store);
    CHECK(root != plain_root);
    CHECK(read_namespace_tree(root, store) == entries);
    for (const auto& [path, entry] : entries) {
        const auto found = namespace_tree_lookup(root, path, store);
        REQUIRE(found.has_value());
        CHECK(found->provenance == entry.provenance);
    }
    const auto stats = namespace_tree_stats(root, store);
    CHECK(stats.entries == entries.size());
    std::vector<ObjectId> nodes;
    collect_namespace_tree_nodes(root, store, nodes);
    CHECK(nodes.size() == stats.leaves + stats.branches + stats.extent_nodes);

    // Taking the provenance away again gives back the original tree.
    for (auto& [_, entry] : entries)
        entry.provenance = {};
    CHECK(build_namespace_tree(entries, store) == plain_root);
}

MACHA_TEST("namespace_tree", test_the_root_is_a_function_of_the_entry_set_and_nothing_else) {
    // History independence: nodes reaching the same namespace by different
    // insertions and deletions must produce the same root.
    const auto entries = library(8, 7);

    MemoryNamespaceNodeStore direct;
    const auto direct_root = build_namespace_tree(entries, direct);

    // The same set, reached by adding a path and taking it away again.
    auto detour = entries;
    detour["/TV/Show 3/Season 2/Episode 99.mkv"] = make_file(999, 40);
    MemoryNamespaceNodeStore scratch;
    (void)build_namespace_tree(detour, scratch);
    detour.erase("/TV/Show 3/Season 2/Episode 99.mkv");
    const auto detour_root = build_namespace_tree(detour, scratch);

    CHECK(direct_root == detour_root);

    // A different store, built from scratch, agrees.
    MemoryNamespaceNodeStore elsewhere;
    CHECK(build_namespace_tree(detour, elsewhere) == direct_root);
}

MACHA_TEST("namespace_tree", test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library) {
    auto entries = library(40, 12);
    MemoryNamespaceNodeStore store;
    const auto before_root = build_namespace_tree(entries, store);
    const auto whole_library_nodes = store.written().size();
    const auto whole_library_bytes = store.bytes();
    store.forget_written();

    // An ordinary namespace write: one file's mtime and size move.
    auto& entry = entries.at("/TV/Show 17/Season 2/Episode 5.mkv");
    entry.mtime_ns += 1000;
    entry.size += 4096;
    const auto after_root = build_namespace_tree(entries, store);

    CHECK(after_root != before_root);
    const auto rewritten = store.written().size();
    REQUIRE(rewritten > 0);

    // A write costs the tree's depth, not the library's size; the exact count
    // depends on boundary hashes, so assert the order of magnitude.
    CHECK(rewritten <= 12);
    CHECK(rewritten * 20 < whole_library_nodes);
    CHECK(whole_library_bytes > 0);

    // The changed value is what comes back.
    const auto read_back = read_namespace_tree(after_root, store);
    CHECK(read_back.at("/TV/Show 17/Season 2/Episode 5.mkv").mtime_ns == entry.mtime_ns);
}

MACHA_TEST("namespace_tree", test_appending_an_extent_does_not_rewrite_the_extent_list) {
    // Extent chunk boundaries follow each extent's content address, not its
    // position, so an append cannot move the boundaries already written.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/film.mkv"] = make_file(7, 4000);

    MemoryNamespaceNodeStore store;
    (void)build_namespace_tree(entries, store);
    const auto whole_file_nodes = store.written().size();
    store.forget_written();

    auto& film = entries.at("/film.mkv");
    ExtentRef appended;
    appended.offset = film.extents.back().offset + film.extents.back().length;
    appended.length = 4 * 1024 * 1024;
    appended.id = fake_object(424242);
    film.extents.push_back(appended);
    film.size += appended.length;

    const auto root = build_namespace_tree(entries, store);
    const auto rewritten = store.written().size();

    // The append rewrites the tail chunk, the spine above it and the entry's
    // leaf: bounded by the extent tree's depth, not its size.
    CHECK(rewritten <= 5);
    CHECK(rewritten < whole_file_nodes / 3);
    CHECK(read_namespace_tree(root, store).at("/film.mkv").extents == film.extents);
}

MACHA_TEST("namespace_tree", test_a_lookup_reads_a_path_to_the_root_rather_than_the_namespace) {
    const auto entries = library(20, 10);
    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(entries, store);

    for (const auto& [path, entry] : entries) {
        auto found = namespace_tree_lookup(root, path, store);
        REQUIRE(found.has_value());
        CHECK(*found == entry);
    }

    // Absent paths sorting before everything, between leaves, and after everything.
    CHECK(!namespace_tree_lookup(root, "", store).has_value());
    CHECK(!namespace_tree_lookup(root, "/AAA", store).has_value());
    CHECK(!namespace_tree_lookup(root, "/TV/Show 3/Season 1/Episode 999.mkv", store).has_value());
    CHECK(!namespace_tree_lookup(root, "/zzz", store).has_value());
}

MACHA_TEST("namespace_tree", test_a_leaf_stays_small_however_many_extents_a_file_has) {
    // A leaf holds stat data and, past a threshold, a reference to the extent
    // list, so a lookup never reads a film's extents inline.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/small.mkv"] = make_file(1, 4);
    entries["/huge.mkv"] = make_file(2, 12500);

    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(entries, store);
    const auto stats = namespace_tree_stats(root, store);

    CHECK(stats.entries == 3);
    CHECK(stats.extents == 12504);
    REQUIRE(stats.extent_nodes > 0);

    // 12,500 extents inline would be hundreds of kilobytes.
    CHECK(stats.largest_node_bytes < 64 * 1024);

    // A small file's extents stay inline: one node, not two.
    const auto read_back = read_namespace_tree(root, store);
    CHECK(read_back.at("/small.mkv").extents.size() == 4);
    CHECK(read_back.at("/huge.mkv").extents.size() == 12500);
}

MACHA_TEST("namespace_tree", test_a_corrupt_node_is_refused_rather_than_trusted) {
    // Nodes come from the control store, so a corrupt or forged one must be
    // refused, not trusted. This catches crashes, hangs and accepted garbage,
    // not an oversized allocation that still succeeds.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/a.mkv"] = make_file(1, 3);
    entries["/b.mkv"] = make_file(2, 900);
    for (int i = 0; i < 40; ++i)
        entries["/dir/f" + std::to_string(i) + ".mkv"] = make_file(10 + i, 2);

    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(entries, store);
    const auto clean = read_namespace_tree(root, store);
    REQUIRE(clean.size() == entries.size());

    // One bit flipped at every byte of every node: the reader may throw, return
    // less or refuse the lookup, but must not crash or hang.
    size_t nodes_tried = 0;
    size_t refused = 0;
    size_t survived = 0;
    for (const auto& id : store.written()) {
        auto body = store.get(id);
        REQUIRE(body);
        ++nodes_tried;
        for (size_t at = 0; at < body->size(); at += 7) {
            auto damaged = *body;
            damaged[at] ^= 0x40;
            MemoryNamespaceNodeStore broken;
            for (const auto& other : store.written()) {
                auto bytes = store.get(other);
                REQUIRE(bytes);
                // Re-store under the original id, so the corruption is reachable.
                broken.put_at(other, other == id ? damaged : *bytes);
            }
            try {
                auto out = read_namespace_tree(root, broken);
                ++survived;
            } catch (const std::exception&) {
                ++refused;
            }
            try {
                (void)namespace_tree_lookup(root, "/a.mkv", broken);
            } catch (const std::exception&) {
            }
        }
    }
    CHECK(nodes_tried > 0);
    // Both outcomes are acceptable; reaching here at all is the assertion.
    CHECK(refused + survived > 0);
}

MACHA_TEST("namespace_tree", test_a_stat_only_lookup_fetches_no_extent_nodes) {
    // A getattr reads the root-to-leaf path and nothing else: no extent node,
    // the target's or a neighbour's. Every entry has an external spine, so any
    // extent fetch shows up in the read count.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    for (int i = 0; i < 200; ++i)
        entries["/film" + std::to_string(i) + ".mkv"] = make_file(100 + i, 400);

    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(entries, store);
    const auto shape = namespace_tree_stats(root, store);
    REQUIRE(shape.extent_nodes > 0);
    REQUIRE(shape.depth >= 2);

    store.forget_reads();
    const auto stat = namespace_tree_lookup(root, "/film137.mkv", store, false);
    const auto stat_reads = store.reads();
    REQUIRE(stat);
    CHECK(stat->size == entries.at("/film137.mkv").size);
    CHECK(stat->extents.empty());
    // One node per level and no more.
    CHECK(stat_reads <= shape.depth);

    // Asking for extents pays for them, so the stat-only lookup avoided real work.
    store.forget_reads();
    const auto full = namespace_tree_lookup(root, "/film137.mkv", store, true);
    const auto full_reads = store.reads();
    REQUIRE(full);
    CHECK(full->extents.size() == 400);
    CHECK(full_reads > stat_reads);

    // A stat for an absent path is equally cheap.
    store.forget_reads();
    CHECK(!namespace_tree_lookup(root, "/absent.mkv", store, false));
    CHECK(store.reads() <= shape.depth);
}

// A snapshot with every SM14 field populated, so a round trip proves the whole
// record survives, not merely the namespace.
MetadataSnapshot populated_snapshot(std::map<std::string, FsEntry> entries) {
    MetadataSnapshot snapshot;
    snapshot.entries = std::move(entries);
    snapshot.data_replication = 3;
    snapshot.extent_size = 4 * 1024 * 1024;
    snapshot.metadata_write_replicas_required = 2;
    snapshot.retention_baseline_complete = true;
    snapshot.catalogue_root = fake_object(7);
    snapshot.metadata_branch_floor.bytes = fake_object(8).bytes;

    NodeId first{}, second{};
    first.bytes[0] = 1;
    second.bytes[0] = 2;
    snapshot.metadata_voters.push_back(first);
    snapshot.mutation_sequences[first] = 4242;
    snapshot.mutation_sequences[second] = 11;
    snapshot.metadata_participants.insert(first);
    snapshot.metadata_participants.insert(second);

    GarbageRef garbage;
    garbage.id = fake_object(9);
    garbage.retired_at_ns = 1758400000000000000;
    garbage.retirement_id = second;
    snapshot.garbage.push_back(garbage);

    PersistedNodeStatus status;
    status.observed_unix_ms = 1758400000000;
    status.version = "0.48.2";
    status.host = "es-1.macha.network";
    status.failure_domain = "es";
    status.port = 9443;
    status.storage_capacity = 8ULL * 1024 * 1024 * 1024 * 1024;
    status.storage_used = 1618ULL * 1024 * 1024 * 1024;
    status.metadata_generation = 31663;
    status.storage_backends_online = 2;
    snapshot.node_status[first] = status;

    IdentityAssociationReset reset;
    reset.host = "gbni-2.macha.network";
    reset.port = 9443;
    reset.stale_node_id = second;
    reset.epoch = 5;
    reset.reset_unix_ms = 1758300000000;
    reset.reset_by = first;
    reset.reason = "endpoint reassigned";
    snapshot.identity_resets[identity_reset_key(reset.host, reset.port)] = reset;

    Hash256 parent{};
    parent.bytes = fake_object(10).bytes;
    snapshot.merge_parents.push_back(parent);
    return snapshot;
}

MACHA_TEST("namespace_tree", test_an_sm14_record_points_at_the_namespace_instead_of_carrying_it) {
    const auto entries = library(20, 10);
    const auto snapshot = populated_snapshot(entries);
    const auto sm13 = encode_snapshot(snapshot);

    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(snapshot, store);
    REQUIRE(detached.namespace_root.has_value());
    CHECK(detached.entries.empty());
    const auto sm14 = encode_snapshot_v14(detached);

    // SM13 carries the library; SM14 carries cluster state plus a 32-byte root.
    CHECK(sm14.size() < 1024);
    CHECK(sm13.size() > 100 * sm14.size());

    // Decoding needs no store: a record does not materialise its namespace.
    const auto decoded = decode_snapshot(sm14);
    REQUIRE(decoded.namespace_root.has_value());
    CHECK(*decoded.namespace_root == *detached.namespace_root);
    CHECK(decoded.entries.empty());

    // Re-encoding the reattached snapshot as SM13 reproduces the original
    // payload byte for byte, covering every other field.
    const auto reattached = attach_namespace(decoded, store);
    CHECK(reattached.entries == entries);
    CHECK(!reattached.namespace_root.has_value());
    CHECK(encode_snapshot(reattached) == sm13);
}

MACHA_TEST("namespace_tree", test_the_record_stops_growing_with_the_library) {
    // Two libraries an order of magnitude apart encode to one payload size.
    MemoryNamespaceNodeStore small_store, large_store;
    const auto small = encode_snapshot_v14(
        detach_namespace(populated_snapshot(library(2, 4)), small_store));
    const auto large = encode_snapshot_v14(
        detach_namespace(populated_snapshot(library(40, 20)), large_store));

    CHECK(small.size() == large.size());
    // They differ only in the root's content address.
    size_t differing = 0;
    for (size_t i = 0; i < small.size(); ++i)
        if (small[i] != large[i])
            ++differing;
    CHECK(differing <= 32);
}

MACHA_TEST("namespace_tree", test_a_snapshot_never_carries_its_namespace_in_two_places) {
    // A record never holds both a map and a root, which could disagree.
    MemoryNamespaceNodeStore store;
    const auto snapshot = populated_snapshot(library(2, 2));
    const auto detached = detach_namespace(snapshot, store);

    // SM13 has nowhere to put a root, so it refuses rather than dropping it.
    bool refused = false;
    try {
        (void)encode_snapshot(detached);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    // SM14 has nowhere to put entries, and refuses for the same reason.
    refused = false;
    try {
        (void)encode_snapshot_v14(snapshot);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    // Neither direction can be run twice.
    refused = false;
    try {
        (void)detach_namespace(detached, store);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    refused = false;
    try {
        (void)attach_namespace(snapshot, store);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    // A namespace with no root directory is refused; for SM14 the reader with
    // the store checks it, since the decoder has none.
    std::map<std::string, FsEntry> rootless;
    rootless["/a.mkv"] = make_file(1, 2);
    MemoryNamespaceNodeStore rootless_store;
    const auto bad = detach_namespace(populated_snapshot(rootless), rootless_store);
    refused = false;
    try {
        (void)attach_namespace(decode_snapshot(encode_snapshot_v14(bad)), rootless_store);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_TEST("namespace_tree", test_a_stat_against_a_decoded_record_never_materialises_the_namespace) {
    // A getattr answered from a decoded record without materialising the map.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    for (int i = 0; i < 200; ++i)
        entries["/film" + std::to_string(i) + ".mkv"] = make_file(100 + i, 400);

    MemoryNamespaceNodeStore store;
    const auto payload = encode_snapshot_v14(detach_namespace(populated_snapshot(entries), store));
    const auto decoded = decode_snapshot(payload);
    REQUIRE(decoded.namespace_root.has_value());
    const auto shape = namespace_tree_stats(*decoded.namespace_root, store);

    store.forget_reads();
    const auto stat = namespace_tree_lookup(*decoded.namespace_root, "/film137.mkv", store, false);
    REQUIRE(stat);
    CHECK(stat->size == entries.at("/film137.mkv").size);
    CHECK(store.reads() <= shape.depth);
    CHECK(decoded.entries.empty());
}

MACHA_TEST("namespace_tree", test_a_damaged_record_is_refused_rather_than_trusted) {
    // The record arrives from the network: a bit flipped at every position must
    // be refused or survived, never crash or hang.
    MemoryNamespaceNodeStore store;
    const auto payload = encode_snapshot_v14(detach_namespace(populated_snapshot(library(2, 2)), store));

    size_t refused = 0, accepted = 0;
    for (size_t at = 0; at < payload.size(); ++at) {
        auto damaged = payload;
        damaged[at] ^= 0x40;
        try {
            const auto decoded = decode_snapshot(damaged);
            ++accepted;
            // Anything that decodes still points at a root and carries no entries.
            CHECK(decoded.entries.empty());
            CHECK(decoded.namespace_root.has_value());
        } catch (const std::exception&) {
            ++refused;
        }
    }
    // A flip in an address decodes cleanly (the missing node is caught a layer
    // down); structural fields are checked.
    CHECK(refused > 0);
    CHECK(refused + accepted == payload.size());
}

MACHA_TEST("namespace_tree", test_a_snapshot_handed_to_a_reader_names_its_namespace_once) {
    // A snapshot handed to a reader has a root or a map, never both.
    MemoryNamespaceNodeStore store;
    const auto snapshot = populated_snapshot(library(2, 2));

    // Either form alone is fine, including the empty namespace.
    require_coherent_namespace(snapshot);
    require_coherent_namespace(MetadataSnapshot{});
    const auto detached = detach_namespace(snapshot, store);
    require_coherent_namespace(detached);
    require_coherent_namespace(attach_namespace(detached, store));

    // Both at once is refused.
    auto incoherent = detached;
    incoherent.entries = snapshot.entries;
    bool refused = false;
    try {
        require_coherent_namespace(incoherent);
    } catch (const MetadataNotReady&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_TEST("namespace_tree", test_a_reader_sees_the_same_namespace_in_either_form) {
    // The primitives give the same answer whichever form the namespace is in.
    const auto entries = library(6, 5);
    const auto attached = populated_snapshot(entries);
    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(attached, store);

    std::map<std::string, FsEntry> from_map, from_tree;
    for_each_namespace_entry(attached, nullptr, [&](const std::string& path, const FsEntry& e) {
        from_map[path] = e;
    });
    for_each_namespace_entry(detached, &store, [&](const std::string& path, const FsEntry& e) {
        from_tree[path] = e;
    });
    CHECK(from_map == entries);
    CHECK(from_tree == entries);

    // Visited in path order in both forms, which is what a prefix scan needs.
    std::vector<std::string> order;
    for_each_namespace_entry(detached, &store,
                             [&](const std::string& path, const FsEntry&) { order.push_back(path); });
    CHECK(std::is_sorted(order.begin(), order.end()));

    // The point lookup agrees in both forms, including for an absent path.
    for (const auto& [path, entry] : entries) {
        const auto by_map = namespace_entry(attached, nullptr, path);
        const auto by_tree = namespace_entry(detached, &store, path);
        REQUIRE(by_map.has_value());
        REQUIRE(by_tree.has_value());
        CHECK(*by_map == entry);
        CHECK(*by_tree == entry);
    }
    CHECK(!namespace_entry(attached, nullptr, "/nothing/here.mkv").has_value());
    CHECK(!namespace_entry(detached, &store, "/nothing/here.mkv").has_value());
}

MACHA_TEST("namespace_tree", test_a_detached_namespace_without_a_store_refuses_rather_than_reporting_empty) {
    // A walk over a detached snapshot with no store must fail with a named
    // error, not report an empty (all-dead) library.
    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(populated_snapshot(library(2, 2)), store);

    size_t visited = 0;
    bool refused = false;
    try {
        for_each_namespace_entry(detached, nullptr,
                                 [&](const std::string&, const FsEntry&) { ++visited; });
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(visited == 0);

    refused = false;
    try {
        (void)namespace_entry(detached, nullptr, "/");
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_TEST("namespace_tree", test_an_incremental_update_produces_the_tree_a_rebuild_would) {
    // An update to an existing root produces the same root as a full build of
    // the result. Random change sets reach leaf-emptying deletes, boundary
    // inserts and count-cap splits.
    auto entries = library(8, 6);
    MemoryNamespaceNodeStore store;
    auto root = build_namespace_tree(entries, store);

    uint64_t seed = 99;
    const auto next = [&] {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    };

    for (int round = 0; round < 60; ++round) {
        NamespaceChanges changes;
        const size_t count = 1 + next() % 6;
        for (size_t i = 0; i < count; ++i) {
            const auto pick = next() % 3;
            if (pick == 0 && !entries.empty()) {
                // Delete an existing path, but never the root directory.
                auto it = entries.begin();
                std::advance(it, static_cast<ptrdiff_t>(next() % entries.size()));
                if (it->first == "/")
                    continue;
                changes[it->first] = std::nullopt;
            } else if (pick == 1 && !entries.empty()) {
                // Change a value in place.
                auto it = entries.begin();
                std::advance(it, static_cast<ptrdiff_t>(next() % entries.size()));
                auto updated = it->second;
                updated.mtime_ns += 1000;
                updated.size += 4096;
                changes[it->first] = updated;
            } else {
                // Insert a new path, sometimes with enough extents to force an
                // external spine.
                const auto path = "/TV/new " + std::to_string(next() % 10000) + ".mkv";
                changes[path] = make_file(next(), next() % 4 == 0 ? 700 : 2);
            }
        }
        if (changes.empty())
            continue;

        for (const auto& [path, value] : changes) {
            if (value)
                entries[path] = *value;
            else
                entries.erase(path);
        }

        MemoryNamespaceNodeStore fresh;
        const auto rebuilt = build_namespace_tree(entries, fresh);
        root = update_namespace_tree(root, store, changes);
        CHECK(root == rebuilt);
        if (root != rebuilt)
            break;
    }

    // The namespace is what the changes said it should be.
    CHECK(read_namespace_tree(root, store) == entries);
}

MACHA_TEST("namespace_tree", test_a_write_rewrites_a_path_rather_than_the_library) {
    // One ordinary write (mtime and size on one file) against a large library.
    auto entries = library(120, 20);
    MemoryNamespaceNodeStore store;
    auto root = build_namespace_tree(entries, store);
    const auto shape = namespace_tree_stats(root, store);

    auto changed = entries.at("/TV/Show 7/Season 2/Episode 5.mkv");
    changed.mtime_ns += 1000;
    changed.size += 4096;

    store.forget_written();
    root = update_namespace_tree(root, store, {{"/TV/Show 7/Season 2/Episode 5.mkv", changed}});
    const auto written = store.written().size();

    // New nodes are exactly the path from the root. The spine is recomputed,
    // not spliced, but unchanged branches re-encode to the same address, so
    // they are not new nodes.
    CHECK(written == shape.depth);
    CHECK(written < shape.leaves);
    CHECK(read_namespace_tree(root, store).at("/TV/Show 7/Season 2/Episode 5.mkv") == changed);

    // Every other entry is unchanged, and unchanged leaves keep their addresses.
    auto after = read_namespace_tree(root, store);
    CHECK(after.size() == entries.size());
    entries["/TV/Show 7/Season 2/Episode 5.mkv"] = changed;
    CHECK(after == entries);
}

MACHA_TEST("namespace_tree", test_a_delta_applied_to_the_tree_lands_where_the_map_lands) {
    // A mutation's delta applied to the tree gives the root a build over the
    // map with the same delta gives.
    auto entries = library(10, 6);
    MemoryNamespaceNodeStore store;
    auto root = build_namespace_tree(entries, store);
    MetadataSnapshot mapped;
    mapped.entries = entries;

    // Find a file with an external extent spine.
    std::string appended;
    for (const auto& [path, entry] : entries)
        if (entry.extents.size() == 600) {
            appended = path;
            break;
        }
    REQUIRE(!appended.empty());

    MetadataDelta delta;
    // An upsert, an erase, a create, and an append onto a file that already
    // has an external extent spine.
    auto touched = entries.at("/TV/Show 1/Season 2/Episode 5.mkv");
    touched.mtime_ns += 5000;
    delta.upsert_entries["/TV/Show 1/Season 2/Episode 5.mkv"] = touched;
    delta.upsert_entries["/TV/Show 9/Season 4/new.mkv"] = make_file(4242, 3);
    delta.erase_entries.push_back("/TV/Show 2/Season 3/Episode 7.mkv");
    MetadataDelta::EntryAppend append;
    append.base_extents = 600;
    append.extents = {make_file(77, 2).extents[0], make_file(78, 2).extents[1]};
    append.size = entries.at(appended).size + 8ULL * 1024 * 1024;
    append.mtime_ns = 12345;
    append.ctime_ns = 12345;
    append.version = 9;
    delta.append_entries[appended] = append;

    const auto updated = apply_delta_to_namespace_tree(root, store, delta);

    apply_metadata_delta_in_place(mapped, delta);
    MemoryNamespaceNodeStore fresh;
    const auto rebuilt = build_namespace_tree(mapped.entries, fresh);

    CHECK(updated == rebuilt);
    CHECK(read_namespace_tree(updated, store) == mapped.entries);
    CHECK(read_namespace_tree(updated, store).at(appended).extents.size() == 602);

    // A delta whose append base does not match is refused, as the map path refuses it.
    MetadataDelta wrong;
    MetadataDelta::EntryAppend mismatched = append;
    mismatched.base_extents = 599;
    wrong.append_entries[appended] = mismatched;
    bool refused = false;
    try {
        (void)apply_delta_to_namespace_tree(updated, store, wrong);
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_TEST("namespace_tree", test_history_replay_applies_a_delta_to_the_tree_not_the_map) {
    // History replay over a tree-backed namespace applies the delta to the tree.
    auto entries = library(6, 4);
    MemoryNamespaceNodeStore store;
    MetadataSnapshot mapped = populated_snapshot(entries);
    auto tree_backed = detach_namespace(mapped, store);

    MetadataDelta delta;
    auto touched = entries.at("/TV/Show 2/Season 3/Episode 2.mkv");
    touched.mtime_ns += 7000;
    delta.upsert_entries["/TV/Show 2/Season 3/Episode 2.mkv"] = touched;
    delta.upsert_entries["/TV/Show 0/Season 1/late.mkv"] = make_file(31337, 5);
    delta.erase_entries.push_back("/TV/Show 4/Season 2/Episode 1.mkv");

    // Without an applier (no node store) the delta is refused, not applied to
    // an empty map.
    bool refused = false;
    try {
        auto victim = tree_backed;
        apply_metadata_delta_in_place(victim, delta);
    } catch (const DecodeError&) {
        refused = true;
    }
    CHECK(refused);

    apply_metadata_delta_in_place(tree_backed, delta,
                                  [&](const ObjectId& root, const MetadataDelta& d) {
                                      return apply_delta_to_namespace_tree(root, store, d);
                                  });
    apply_metadata_delta_in_place(mapped, delta);

    // The replayed record stays tree-backed and matches the map form's namespace.
    REQUIRE(tree_backed.namespace_root.has_value());
    CHECK(tree_backed.entries.empty());
    MemoryNamespaceNodeStore fresh;
    CHECK(*tree_backed.namespace_root == build_namespace_tree(mapped.entries, fresh));
    CHECK(read_namespace_tree(*tree_backed.namespace_root, store) == mapped.entries);

    // Non-entry fields move exactly as in the map form.
    CHECK(tree_backed.mutation_sequences == mapped.mutation_sequences);
    CHECK(tree_backed.garbage == mapped.garbage);
}

MACHA_TEST("namespace_tree", test_a_stat_only_read_costs_no_extents_in_either_form) {
    // Path resolution, the hottest read, must neither copy (map) nor fetch
    // (tree) a file's extent list.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/film.mkv"] = make_file(1, 12500);

    const auto attached = populated_snapshot(entries);
    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(attached, store);

    CHECK(namespace_contains(attached, nullptr, "/film.mkv"));
    CHECK(!namespace_contains(attached, nullptr, "/missing.mkv"));

    store.forget_reads();
    CHECK(namespace_contains(detached, &store, "/film.mkv"));
    const auto contains_reads = store.reads();
    CHECK(!namespace_contains(detached, &store, "/missing.mkv"));

    // A stat-only read carries no extents from either form.
    const auto stat_from_map = namespace_entry(attached, nullptr, "/film.mkv", false);
    REQUIRE(stat_from_map.has_value());
    CHECK(stat_from_map->extents.empty());
    CHECK(stat_from_map->size == entries.at("/film.mkv").size);

    store.forget_reads();
    const auto stat_from_tree = namespace_entry(detached, &store, "/film.mkv", false);
    REQUIRE(stat_from_tree.has_value());
    CHECK(stat_from_tree->extents.empty());
    CHECK(store.reads() == contains_reads);

    // Asking for extents costs more, so the stat-only path avoided real work.
    store.forget_reads();
    const auto full = namespace_entry(detached, &store, "/film.mkv", true);
    REQUIRE(full.has_value());
    CHECK(full->extents.size() == 12500);
    CHECK(store.reads() > contains_reads);
}

MACHA_TEST("namespace_tree", test_a_prefix_scan_descends_rather_than_walking_the_library) {
    // The tree is keyed by path so prefix queries descend to the prefix
    // instead of reading everything.
    const auto entries = library(30, 8);
    const auto attached = populated_snapshot(entries);
    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(attached, store);

    const std::string prefix = "/TV/Show 7/";
    std::map<std::string, FsEntry> expected;
    for (const auto& [path, entry] : entries)
        if (path.starts_with(prefix))
            expected[path] = entry;
    REQUIRE(expected.size() > 4);
    REQUIRE(expected.size() * 4 < entries.size());

    // Same answer from both forms, and the same answer as filtering a full walk.
    std::map<std::string, FsEntry> from_map, from_tree;
    for_each_namespace_entry_with_prefix(attached, nullptr, prefix,
                                         [&](const std::string& p, const FsEntry& e) {
                                             from_map[p] = e;
                                         });
    store.forget_reads();
    for_each_namespace_entry_with_prefix(detached, &store, prefix,
                                         [&](const std::string& p, const FsEntry& e) {
                                             from_tree[p] = e;
                                         });
    const auto prefix_reads = store.reads();
    CHECK(from_map == expected);
    CHECK(from_tree == expected);

    // It reads a fraction of what a full walk of the same tree reads.
    store.forget_reads();
    size_t walked = 0;
    for_each_namespace_entry(detached, &store,
                             [&](const std::string&, const FsEntry&) { ++walked; });
    const auto full_reads = store.reads();
    CHECK(walked == entries.size());
    CHECK(prefix_reads * 4 < full_reads);

    // An empty prefix is every entry; an unmatched prefix costs almost nothing.
    size_t all = 0;
    for_each_namespace_entry_with_prefix(detached, &store, "",
                                         [&](const std::string&, const FsEntry&) { ++all; });
    CHECK(all == entries.size());

    store.forget_reads();
    size_t none = 0;
    for_each_namespace_entry_with_prefix(detached, &store, "/zzz-nothing/",
                                         [&](const std::string&, const FsEntry&) { ++none; });
    CHECK(none == 0);
    CHECK(store.reads() * 4 < full_reads);
}

// A directory listing reads the directory, not what lies beneath it: each
// child directory is stepped over in one descent. Names that sort between a
// directory and its contents ("b.txt" between "b" and "b/x") are still
// listed once and in order.
MACHA_TEST("namespace_tree", test_a_listing_reads_the_directory_not_its_descendants) {
    auto entries = library(40, 200);
    entries["/TV/Show 3.nfo"] = make_file(9001, 1);
    entries["/TV/Show 3-extras"] = make_directory(9002);
    entries["/TV/Show 3-extras/clip.mkv"] = make_file(9003, 2);
    const auto attached = populated_snapshot(entries);
    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(attached, store);

    const auto expected_children = [&](const std::string& directory) {
        std::vector<std::pair<std::string, std::string>> out;
        const auto prefix = directory == "/" ? std::string("/") : directory + "/";
        for (const auto& [path, entry] : entries) {
            if (!path.starts_with(prefix) || path == prefix)
                continue;
            const auto rest = path.substr(prefix.size());
            if (rest.find('/') == std::string::npos)
                out.emplace_back(rest, path);
        }
        return out;
    };
    const auto children_of = [&](const MetadataSnapshot& snapshot, const NamespaceNodeStore* nodes,
                                 const std::string& directory) {
        std::vector<std::pair<std::string, std::string>> out;
        for_each_namespace_child(snapshot, nodes, directory,
                                 [&](const std::string& name, const std::string& path,
                                     const FsEntry& stat) {
                                     CHECK(stat.type == entries.at(path).type);
                                     CHECK(stat.size == entries.at(path).size);
                                     CHECK(stat.extents.empty() ||
                                           stat.extents == entries.at(path).extents);
                                     out.emplace_back(name, path);
                                 });
        return out;
    };

    for (const std::string directory :
         {"/", "/TV", "/TV/Show 3", "/TV/Show 3/Season 1", "/TV/Show 3-extras", "/nowhere"}) {
        const auto expected = expected_children(directory);
        CHECK(children_of(attached, nullptr, directory) == expected);
        CHECK(children_of(detached, &store, directory) == expected);
    }
    REQUIRE(expected_children("/TV").size() == 42);

    // Listing /TV visits its 42 children, at most one descent each; the
    // thousands of entries beneath them are in leaves it never reads.
    const auto stats = namespace_tree_stats(*detached.namespace_root, store);
    store.forget_reads();
    (void)children_of(detached, &store, "/TV");
    const auto listing_reads = store.reads();
    CHECK(listing_reads <= (42 + 3) * stats.depth);
    CHECK(listing_reads < stats.leaves);

    // The emptiness test stops at the first entry it finds.
    store.forget_reads();
    const auto first = first_namespace_path_under(detached, &store, "/TV");
    REQUIRE(first.has_value());
    CHECK(*first == first_namespace_path_under(attached, nullptr, "/TV"));
    CHECK(first->starts_with("/TV/"));
    CHECK(store.reads() <= 8);
    CHECK(!first_namespace_path_under(detached, &store, "/TV/Show 3.nfo").has_value());
    CHECK(!first_namespace_path_under(detached, &store, "/nowhere").has_value());
    // Entries the caller has erased are passed over.
    const auto skipping = first_namespace_path_under(
        detached, &store, "/TV/Show 3-extras",
        [](const std::string& path) { return path == "/TV/Show 3-extras/clip.mkv"; });
    CHECK(!skipping.has_value());
}

// A scan can jump: the entries between where it was and where it seeks to
// are never visited, and it ends when told to.
MACHA_TEST("namespace_tree", test_a_scan_seeks_and_stops_where_it_is_told) {
    const auto entries = library(20, 8);
    const auto attached = populated_snapshot(entries);
    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(attached, store);

    for (const auto* form : {&attached, &detached}) {
        const NamespaceNodeStore* nodes = form == &detached ? &store : nullptr;
        std::vector<std::string> seen;
        scan_namespace(*form, nodes, "/TV/Show 1/", [&](const std::string& path, const FsEntry&) {
            seen.push_back(path);
            if (seen.size() == 1)
                return NamespaceScanStep::seek("/TV/Show 5");
            if (seen.size() == 4)
                return NamespaceScanStep::stop();
            return NamespaceScanStep::next();
        });
        REQUIRE(seen.size() == 4);
        auto expect = entries.lower_bound("/TV/Show 1/");
        CHECK(seen[0] == expect->first);
        expect = entries.lower_bound("/TV/Show 5");
        for (size_t i = 1; i < 4; ++i, ++expect)
            CHECK(seen[i] == expect->first);
        // A scan from past the last key visits nothing.
        size_t none = 0;
        scan_namespace(*form, nodes, "/zzz", [&](const std::string&, const FsEntry&) {
            ++none;
            return NamespaceScanStep::next();
        });
        CHECK(none == 0);
        // Seeking backwards is a caller's error.
        bool refused = false;
        try {
            scan_namespace(*form, nodes, "/TV", [&](const std::string&, const FsEntry&) {
                return NamespaceScanStep::seek("/");
            });
        } catch (const std::logic_error&) {
            refused = true;
        }
        CHECK(refused);
    }
}

MACHA_TEST("namespace_tree", test_every_tree_node_is_reachable_for_the_collector) {
    // The collector's set is exactly the nodes the build wrote: branches,
    // leaves and every extent spine node.
    const auto entries = library(12, 8);
    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(entries, store);

    std::vector<ObjectId> reachable;
    collect_namespace_tree_nodes(root, store, reachable);
    std::sort(reachable.begin(), reachable.end());
    reachable.erase(std::unique(reachable.begin(), reachable.end()), reachable.end());

    auto written = store.written();
    std::sort(written.begin(), written.end());
    CHECK(reachable == written);
    CHECK(reachable.size() == store.nodes());

    const auto shape = namespace_tree_stats(root, store);
    CHECK(reachable.size() == shape.leaves + shape.branches + shape.extent_nodes);
}

MACHA_TEST("namespace_tree", test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest) {
    // A commit claims everything the new root reaches that the old did not,
    // by a parallel walk that skips shared subtrees. Over-claiming is allowed,
    // under-claiming is not. The library is large enough for pruning to matter.
    auto entries = library(60, 20);
    MemoryNamespaceNodeStore store;
    const auto before = build_namespace_tree(entries, store);
    const auto before_shape = namespace_tree_stats(before, store);

    auto changed = entries.at("/TV/Show 4/Season 1/Episode 4.mkv");
    changed.mtime_ns += 999;
    store.forget_written();
    const auto after = update_namespace_tree(before, store, {{"/TV/Show 4/Season 1/Episode 4.mkv", changed}});
    auto written = store.written();
    std::sort(written.begin(), written.end());
    REQUIRE(!written.empty());

    std::vector<ObjectId> claimed;
    collect_namespace_tree_changes(before, after, store, claimed);
    std::sort(claimed.begin(), claimed.end());
    claimed.erase(std::unique(claimed.begin(), claimed.end()), claimed.end());

    // Every node the commit wrote is claimed.
    CHECK(std::includes(claimed.begin(), claimed.end(), written.begin(), written.end()));
    // The walk pruned: far fewer nodes than the tree holds.
    const auto total = before_shape.leaves + before_shape.branches + before_shape.extent_nodes;
    CHECK(claimed.size() * 4 < total);
    // A change that rewrote no extent list claims no extent list: the leaf's
    // other files, and this one's own unchanged list, are already named.
    CHECK(claimed == written);

    // A file given new extents has its new list claimed, and no other file's.
    {
        auto regrown = make_file(424242, 600);
        store.forget_written();
        const auto grown =
            update_namespace_tree(after, store, {{"/TV/Show 4/Season 1/Episode 4.mkv", regrown}});
        auto wrote = store.written();
        std::sort(wrote.begin(), wrote.end());
        std::vector<ObjectId> introduced;
        collect_namespace_tree_changes(after, grown, store, introduced);
        std::sort(introduced.begin(), introduced.end());
        introduced.erase(std::unique(introduced.begin(), introduced.end()), introduced.end());
        CHECK(introduced == wrote);
        const auto shape = namespace_tree_stats(grown, store);
        CHECK(introduced.size() * 4 < shape.leaves + shape.branches + shape.extent_nodes);
    }

    // With no `before`, everything is new.
    std::vector<ObjectId> everything;
    collect_namespace_tree_changes(std::nullopt, after, store, everything);
    std::sort(everything.begin(), everything.end());
    everything.erase(std::unique(everything.begin(), everything.end()), everything.end());
    std::vector<ObjectId> all;
    collect_namespace_tree_nodes(after, store, all);
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    CHECK(everything == all);

    // An unreadable node fails the walk rather than returning a short list.
    MemoryNamespaceNodeStore damaged;
    for (const auto& id : store.written())
        if (auto bytes = store.get(id))
            damaged.put_at(id, *bytes);
    bool refused = false;
    try {
        std::vector<ObjectId> partial;
        collect_namespace_tree_nodes(after, damaged, partial);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

} // namespace

namespace {

struct Random {
    uint64_t seed;
    uint64_t next() {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    }
};

Hash256 head_of(uint8_t first) {
    Hash256 head{};
    head.bytes[0] = first;
    return head;
}

// One node's lineage: the namespace it holds and the mutations it authors on
// it, stamped as a commit stamps what it writes.
struct Lineage {
    NodeId author{};
    uint64_t sequence{};
    uint64_t created{};
    MetadataSnapshot head;

    explicit Lineage(uint8_t tag) { author.bytes[0] = tag; }

    MetadataDot next() {
        if (!head.legacy_clock)
            head.legacy_clock = head.mutation_sequences;
        head.mutation_sequences[author] = ++sequence;
        return {author, sequence};
    }
    // Creates the entry, or changes the one already at the path.
    void put(const std::string& path, FsEntry entry) {
        const auto prior = head.entries.find(path);
        entry.provenance = {};
        if (prior == head.entries.end()) {
            // A file id of this lineage's own, the same on every run.
            entry.provenance.file_id.bytes[0] = author.bytes[0];
            const auto number = ++created;
            for (size_t i = 0; i < 8; ++i)
                entry.provenance.file_id.bytes[8 + i] = static_cast<uint8_t>(number >> (8 * i));
        }
        const auto dot = next();
        stamp_entry_provenance(entry, path, prior == head.entries.end() ? nullptr : &prior->second,
                               dot);
        head.entries[path] = std::move(entry);
    }
    // Changes the file at the path: its size, and the time it says.
    void edit(const std::string& path, uint64_t grow, int64_t later) {
        auto entry = head.entries.at(path);
        entry.size += grow;
        entry.mtime_ns += later;
        put(path, std::move(entry));
    }
    // Removes the entry and everything beneath it, one mutation each.
    void erase(const std::string& path) {
        for (auto it = head.entries.begin(); it != head.entries.end();) {
            if (it->first != path && !it->first.starts_with(path + "/")) {
                ++it;
                continue;
            }
            (void)next();
            it = head.entries.erase(it);
        }
    }
    void rename(const std::string& from, const std::string& to) {
        auto entry = head.entries.at(from);
        if (entry.provenance.file_id == NodeId{})
            entry.provenance.file_id = legacy_file_id(from);
        const auto dot = next();
        head.entries.erase(from);
        const auto prior = head.entries.find(to);
        stamp_entry_provenance(entry, to, prior == head.entries.end() ? nullptr : &prior->second,
                               dot);
        head.entries[to] = std::move(entry);
    }
    // Takes the namespace another lineage reached, keeping its own identity.
    void adopt(const MetadataSnapshot& other) { head = other; }
};

// A lineage's mutations between two merges. Targets come from a small pool as
// often as not, so two lineages change the same paths; a removed directory
// takes what is beneath it, so the other can add a file there.
void mutate(Lineage& lineage, Random& random, size_t edits) {
    const auto& entries = lineage.head.entries;
    for (size_t i = 0; i < edits; ++i) {
        const size_t pool = random.next() % 2 ? 12 : entries.size();
        auto target = entries.begin();
        std::advance(target,
                     static_cast<ptrdiff_t>(random.next() % std::min(pool, entries.size())));
        const auto path = target->first;
        auto entry = target->second;
        if (path == "/" || path == "/TV")
            continue;
        switch (random.next() % 6) {
        case 0: // change in place
            entry.mtime_ns += 1 + static_cast<int64_t>(random.next() % 3);
            entry.size += random.next() % 2 ? 4096 : 0;
            lineage.put(path, std::move(entry));
            break;
        case 1: // the same content, another time: no disagreement
            entry.ctime_ns += 7;
            lineage.put(path, std::move(entry));
            break;
        case 2: // remove, with everything beneath a directory
            lineage.erase(path);
            break;
        case 3: { // move a file, onto another as often as not
            const auto to = "/TV/moved " + std::to_string(random.next() % 5) + ".mkv";
            if (entry.type == EntryType::file && to != path)
                lineage.rename(path, to);
            break;
        }
        case 4: { // a new file beside the target, or beneath it
            const auto parent = entry.type == EntryType::directory ? path : parent_path(path);
            const auto seed = random.next();
            lineage.put(parent + "/new " + std::to_string(random.next() % 4) + ".mkv",
                        make_file(seed, random.next() % 5 == 0 ? 700 : 2));
            break;
        }
        default: // a new directory at a name both lineages may choose
            lineage.put("/TV/made " + std::to_string(random.next() % 3),
                        make_directory(random.next() % 2));
            break;
        }
    }
}

// What a tree merge must share with the merge of the materialised heads: the
// root, the conflicts and every other field of the record. Its delta, applied
// to the primary parent as a replica would, gives that record byte for byte.
// Returns the encoded sizes of the delta and of the record.
std::pair<uint64_t, uint64_t> check_tree_merge(const MetadataMergeResult& expected,
                                               const NamespaceTreeMerge& merge,
                                               const ObjectId& root,
                                               const MetadataSnapshot& primary,
                                               const Hash256& secondary_head,
                                               MemoryNamespaceNodeStore& store) {
    MemoryNamespaceNodeStore fresh;
    const auto expected_root = build_namespace_tree(expected.snapshot.entries, fresh);
    CHECK(root == expected_root);
    CHECK(merge.merged.conflicts_created == expected.conflicts_created);
    CHECK(merge.merged.conflicts_superseded == expected.conflicts_superseded);
    CHECK(merge.merged.snapshot.conflicts == expected.snapshot.conflicts);
    CHECK(merge.merged.snapshot.entries.empty());

    auto expected_tree = expected.snapshot;
    expected_tree.entries.clear();
    expected_tree.namespace_root = expected_root;
    auto merged_tree = merge.merged.snapshot;
    merged_tree.namespace_root = root;
    CHECK(encode_snapshot_v14(merged_tree) == encode_snapshot_v14(expected_tree));

    CHECK(merge.onto == *primary.namespace_root);
    merged_tree.merge_parents = {secondary_head};
    const auto delta = tree_merge_delta(primary, merged_tree, merge.changes);
    REQUIRE(delta.has_value());
    const auto replayed =
        apply_metadata_delta(primary, decode_metadata_delta(encode_metadata_delta(*delta)),
                             [&](const ObjectId& tree, const MetadataDelta& applied) {
                                 return apply_delta_to_namespace_tree(tree, store, applied);
                             });
    CHECK(encode_snapshot_v14(replayed) == encode_snapshot_v14(merged_tree));
    return {encode_metadata_delta(*delta).size(), encode_snapshot_v14(merged_tree).size()};
}

} // namespace

MACHA_FAST_TEST("namespace_tree", test_a_diff_is_the_paths_that_differ_and_reads_only_their_leaves) {
    MemoryNamespaceNodeStore store;
    auto before = library(200, 20);
    const auto root_before = build_namespace_tree(before, store);
    CHECK(diff_namespace_trees(root_before, root_before, store).empty());

    auto after = before;
    after.erase("/TV/Show 3/Season 1/Episode 0.mkv");
    after["/TV/Show 3/Season 1/Episode 4.mkv"].mtime_ns += 1;
    after["/TV/Show 40/Season 2/added.mkv"] = make_file(77, 700);
    // An external extent list one extent longer.
    auto& grown = after.at("/TV/Show 0/Season 1/Episode 0.mkv");
    grown.extents.push_back(make_file(5, 1).extents.front());
    const auto root_after = build_namespace_tree(after, store);

    store.forget_reads();
    const auto differences = diff_namespace_trees(root_before, root_after, store);
    const auto reads = store.reads();
    REQUIRE(differences.size() == 4);
    for (const auto& [path, difference] : differences) {
        const auto was = before.find(path);
        const auto is = after.find(path);
        CHECK(difference.before ==
              (was == before.end() ? std::optional<FsEntry>() : std::optional(was->second)));
        CHECK(difference.after ==
              (is == after.end() ? std::optional<FsEntry>() : std::optional(is->second)));
    }
    // Both ways round, and against the empty tree.
    const auto reversed = diff_namespace_trees(root_after, root_before, store);
    REQUIRE(reversed.size() == 4);
    CHECK(reversed.at("/TV/Show 40/Season 2/added.mkv").after == std::nullopt);
    const auto empty = build_namespace_tree({}, store);
    CHECK(diff_namespace_trees(empty, root_before, store).size() == before.size());

    // Four changes in a library of over a hundred leaves: both trees' few
    // branch nodes, the leaves that differ, and the changed files' extents.
    const auto stats = namespace_tree_stats(root_before, store);
    CHECK(stats.leaves > 100);
    CHECK(reads < stats.leaves / 2);
}

MACHA_FAST_TEST("namespace_tree", test_distant_changes_read_no_leaf_between_them) {
    MemoryNamespaceNodeStore store;
    auto entries = library(200, 20);
    const auto root = build_namespace_tree(entries, store);
    const auto stats = namespace_tree_stats(root, store);

    NamespaceChanges changes;
    auto first = entries.at("/TV/Show 0/Season 1/Episode 0.mkv");
    first.mtime_ns += 1;
    changes["/TV/Show 0/Season 1/Episode 0.mkv"] = first;
    changes["/TV/Show 50/Season 4/Episode 19.mkv"] = std::nullopt;
    changes["/zz-last.mkv"] = make_file(3, 2);
    for (const auto& [path, value] : changes)
        if (value)
            entries[path] = *value;
        else
            entries.erase(path);

    store.forget_reads();
    const auto updated = update_namespace_tree(root, store, changes);
    const auto reads = store.reads();
    MemoryNamespaceNodeStore fresh;
    CHECK(updated == build_namespace_tree(entries, fresh));
    // The branch nodes, the three windows' leaves and those leaves' extent
    // nodes; no leaf between the windows.
    CHECK(reads < stats.leaves);
}

MACHA_FAST_TEST("namespace_tree", test_a_tree_merge_is_the_merge_of_the_materialised_heads) {
    // The merge of two materialised heads is the oracle: the tree merge must
    // reach the same root, the same conflicts and the same record. Two
    // lineages diverge from what the last round merged to, standing conflicts
    // included.
    Lineage origin{9};
    origin.head.extent_size = 4 * 1024 * 1024;
    origin.head.metadata_write_replicas_required = 1;
    for (const auto& [path, entry] : library(10, 8))
        origin.put(path, entry);
    Lineage a{1}, b{2};
    a.adopt(origin.head);
    b.adopt(origin.head);

    uint64_t delta_bytes = 0, full_bytes = 0;
    const auto merge_round = [&](int round) {
        const auto left_head = head_of(round % 2 ? 1 : 2);
        const auto right_head = head_of(round % 2 ? 2 : 1);
        auto expected = merge_metadata_heads(a.head, b.head, left_head, right_head);

        MemoryNamespaceNodeStore store;
        const auto left = detach_namespace(a.head, store);
        const auto right = detach_namespace(b.head, store);
        const auto merge = merge_tree_backed_heads(left, right, left_head, right_head, store);
        const auto root = update_namespace_tree(merge.onto, store, merge.changes);
        const bool left_primary = left_head < right_head;
        const auto [delta, full] =
            check_tree_merge(expected, merge, root, left_primary ? left : right,
                             left_primary ? right_head : left_head, store);
        delta_bytes += delta;
        full_bytes += full;
        a.adopt(expected.snapshot);
        b.adopt(expected.snapshot);
        return expected;
    };

    // One of each way two heads come to differ.
    const std::string added_a = "/TV/Show 1/Season 1/from a.mkv";
    const std::string added_b = "/TV/Show 8/Season 3/from b.mkv";
    const std::string removed = "/TV/Show 2/Season 2/Episode 1.mkv";
    const std::string disputed = "/TV/Show 3/Season 1/Episode 0.mkv";
    const std::string renamed_from = "/TV/Show 4/Season 2/Episode 5.mkv";
    const std::string renamed_to = "/TV/Show 4/Season 2/renamed.mkv";
    const std::string emptied = "/TV/Show 5/Season 4";
    const std::string far_a = "/TV/Show 0/Season 1/Episode 0.mkv";
    const std::string far_b = "/TV/Show 9/Season 4/Episode 7.mkv";
    a.put(added_a, make_file(9001, 2));
    b.put(added_b, make_file(9002, 2));
    a.erase(removed);
    a.edit(disputed, 4096, 1);
    b.edit(disputed, 8192, 2);
    a.rename(renamed_from, renamed_to);
    b.edit(renamed_from, 4096, 1);
    a.erase(emptied);
    b.put(emptied + "/added.mkv", make_file(9003, 2));
    a.edit(far_a, 4096, 1);
    b.edit(far_b, 4096, 1);
    const auto edited = b.head.entries.at(renamed_from);
    const auto later = b.head.entries.at(disputed);
    const auto far_a_size = a.head.entries.at(far_a).size;
    const auto far_b_size = b.head.entries.at(far_b).size;
    {
        const auto merged = merge_round(0);
        const auto& entries = merged.snapshot.entries;
        CHECK(entries.contains(added_a));
        CHECK(entries.contains(added_b));
        CHECK(!entries.contains(removed));
        CHECK(merged.conflicts_created == 1);
        REQUIRE(merged.snapshot.conflicts.size() == 1);
        CHECK(merged.snapshot.conflicts.begin()->second.key == disputed);
        CHECK(entries.at(disputed) == later);
        CHECK(!entries.contains(renamed_from));
        REQUIRE(entries.contains(renamed_to));
        CHECK(entries.at(renamed_to).size == edited.size);
        REQUIRE(entries.contains(emptied));
        CHECK(entries.contains(emptied + "/added.mkv"));
        CHECK(!entries.contains(emptied + "/Episode 3.mkv"));
        CHECK(entries.at(far_a).size == far_a_size);
        CHECK(entries.at(far_b).size == far_b_size);
    }

    Random random{2026};
    size_t conflicted = 0, clean = 0;
    for (int round = 1; round <= 40; ++round) {
        mutate(a, random, 1 + random.next() % 6);
        mutate(b, random, 1 + random.next() % 6);
        // Every third round both heads create one path, differently.
        if (round % 3 == 0) {
            const auto clash = "/TV/clash " + std::to_string(round) + ".mkv";
            a.put(clash, make_file(7000 + static_cast<uint64_t>(round), 2));
            b.put(clash, make_file(8000 + static_cast<uint64_t>(round), 3));
        }
        (merge_round(round).conflicts_created ? conflicted : clean) += 1;
    }
    // The rounds reached both outcomes.
    CHECK(conflicted > 5);
    CHECK(clean > 5);
    // Every round had a delta to publish; the reconciler stores whichever of
    // delta and record is smaller.
    CHECK(delta_bytes > 0);
    CHECK(full_bytes > 0);

    // One head twice is that head.
    MemoryNamespaceNodeStore store;
    const auto tree = detach_namespace(a.head, store);
    const auto same = merge_tree_backed_heads(tree, tree, head_of(1), head_of(1), store);
    CHECK(same.changes.empty());
    CHECK(same.onto == *tree.namespace_root);

    // A map-backed head is not a tree.
    bool refused = false;
    try {
        (void)merge_tree_backed_heads(tree, a.head, head_of(1), head_of(2), store);
    } catch (const std::logic_error&) {
        refused = true;
    }
    CHECK(refused);
}

// A two-head reconciliation at the live library's size (8,700 paths, 900,000
// extents), each head one cycle of a loader and a few changes to the library:
// the materialising merge against the tree merge. Prints
// "BENCH name=<name> ns_per_op=<n> ops=1"; run with `--filter baseline --verbose`.
MACHA_HEAVY_TEST("baseline", test_baseline_reconcile_materialised_against_tree) {
    MemoryNamespaceNodeStore store;
    Lineage origin{9};
    origin.head.extent_size = 4 * 1024 * 1024;
    origin.head.metadata_write_replicas_required = 1;
    origin.put("/", make_directory(0));
    origin.put("/Library", make_directory(1));
    for (size_t d = 0; d < 700; ++d) {
        const auto directory = "/Library/Title " + std::to_string(d);
        origin.put(directory, make_directory(10 + d));
        for (size_t f = 0; f < 11; ++f)
            origin.put(directory + "/part " + std::to_string(f) + ".mkv",
                       make_file(d * 100 + f, f == 0 ? 600 : 60));
    }
    Lineage a{1}, b{2};
    a.adopt(origin.head);
    b.adopt(origin.head);
    const auto load = [](Lineage& lineage, const std::string& name, uint64_t seed) {
        lineage.put("/load-" + name, make_directory(seed));
        lineage.put("/load-" + name + "/load.bin", make_file(seed, 8));
    };
    load(a, "a", 7001);
    load(b, "b", 7002);
    // One of each way two heads come to differ, far apart in the library.
    const std::string removed = "/Library/Title 100/part 3.mkv";
    const std::string disputed = "/Library/Title 250/part 1.mkv";
    const std::string renamed_from = "/Library/Title 400/part 2.mkv";
    const std::string renamed_to = "/Library/Title 400/renamed.mkv";
    const std::string emptied = "/Library/Title 550";
    a.erase(removed);
    a.edit(disputed, 4096, 1);
    b.edit(disputed, 8192, 2);
    a.rename(renamed_from, renamed_to);
    b.edit(renamed_from, 4096, 1);
    a.erase(emptied);
    b.put(emptied + "/added.mkv", make_file(7003, 2));
    b.edit("/Library/Title 699/part 10.mkv", 4096, 1);

    const auto left = detach_namespace(a.head, store);
    const auto right = detach_namespace(b.head, store);
    const auto left_head = head_of(1);
    const auto right_head = head_of(2);
    const auto report = [](std::string_view name, Clock::duration elapsed) {
        std::cout << "BENCH name=" << name << " ns_per_op="
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
                  << " ops=1\n";
    };

    auto started = Clock::now();
    const auto materialised = merge_metadata_heads(attach_namespace(left, store),
                                                   attach_namespace(right, store), left_head,
                                                   right_head);
    const auto rebuilt = detach_namespace(materialised.snapshot, store);
    report("reconcile.materialised", Clock::now() - started);

    store.forget_reads();
    started = Clock::now();
    const auto merge = merge_tree_backed_heads(left, right, left_head, right_head, store);
    const auto root = update_namespace_tree(merge.onto, store, merge.changes);
    report("reconcile.tree", Clock::now() - started);
    std::cout << "BENCH name=reconcile.tree.node_reads ns_per_op=0 ops=" << store.reads() << '\n';

    CHECK(root == *rebuilt.namespace_root);
    (void)check_tree_merge(materialised, merge, root, left, right_head, store);
    CHECK(materialised.conflicts_created == 1);
    CHECK(namespace_tree_lookup(root, "/load-a/load.bin", store).has_value());
    CHECK(namespace_tree_lookup(root, "/load-b/load.bin", store).has_value());
    CHECK(!namespace_tree_lookup(root, removed, store).has_value());
    CHECK(namespace_tree_lookup(root, disputed, store) == b.head.entries.at(disputed));
    CHECK(!namespace_tree_lookup(root, renamed_from, store).has_value());
    const auto moved = namespace_tree_lookup(root, renamed_to, store);
    REQUIRE(moved.has_value());
    CHECK(moved->size == b.head.entries.at(renamed_from).size);
    CHECK(namespace_tree_lookup(root, emptied, store).has_value());
    CHECK(namespace_tree_lookup(root, emptied + "/added.mkv", store).has_value());
    CHECK(!namespace_tree_lookup(root, emptied + "/part 0.mkv", store).has_value());
}

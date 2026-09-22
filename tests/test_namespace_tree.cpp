// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "metadata_manager.hpp"
#include "namespace_tree.hpp"

#include <algorithm>
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

// A namespace with the shape a media library actually has: deeply nested,
// unbalanced, a few very large files and a long tail of small ones.
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
            // Every 16th episode is a feature-length file with enough extents
            // to force an external list.
            const size_t extents = (s * episodes_per_show + e) % 16 == 0 ? 600 : 3;
            entries[season + "/Episode " + std::to_string(e) + ".mkv"] =
                make_file(1000 + s * 100 + e, extents);
        }
    }
    return entries;
}

MACHA_TEST("namespace_tree", test_the_tree_round_trips_every_namespace_it_is_given) {
    MemoryNamespaceNodeStore store;

    // The empty namespace is a tree like any other, so a fresh cluster is not a
    // special case at the call site.
    const auto empty_root = build_namespace_tree({}, store);
    CHECK(read_namespace_tree(empty_root, store).empty());

    const auto entries = library(12, 9);
    const auto root = build_namespace_tree(entries, store);
    const auto read_back = read_namespace_tree(root, store);
    REQUIRE(read_back.size() == entries.size());
    CHECK(read_back == entries);

    // Including the entries whose extent lists were externalised: a 600-extent
    // file has to come back with all 600, in order.
    const auto& original = entries.at("/TV/Show 0/Season 1/Episode 0.mkv");
    REQUIRE(original.extents.size() == 600);
    CHECK(read_back.at("/TV/Show 0/Season 1/Episode 0.mkv").extents == original.extents);
}

MACHA_TEST("namespace_tree", test_the_root_is_a_function_of_the_entry_set_and_nothing_else) {
    // History independence. Once metadata_namespace_signature and cache_record's
    // entries-comparison witness become root comparisons, two nodes that
    // reconcile independently to the same namespace must produce the same root
    // or they will report divergence that does not exist. A tree whose shape
    // depended on the order of insertions and deletions could not do that.
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

    // And a different store, built from scratch, agrees: the root addresses
    // content, not a particular replica's history.
    MemoryNamespaceNodeStore elsewhere;
    CHECK(build_namespace_tree(detour, elsewhere) == direct_root);
}

MACHA_TEST("namespace_tree", test_changing_one_file_rewrites_a_path_to_the_root_and_not_the_library) {
    // This is the measurement Stage B exists to produce. Today a single file
    // write re-serialises and re-hashes the whole namespace; the question is
    // what it costs against a tree.
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

    // The whole point: the cost of a write is the depth of the tree, not the
    // size of the library. Ten nodes against a library of hundreds is the
    // claim; the exact figure varies with the boundary hashes, so assert the
    // order of magnitude rather than a number that would pin the hash function.
    CHECK(rewritten <= 12);
    CHECK(rewritten * 20 < whole_library_nodes);
    CHECK(whole_library_bytes > 0);

    // And the value that changed is what comes back.
    const auto read_back = read_namespace_tree(after_root, store);
    CHECK(read_back.at("/TV/Show 17/Season 2/Episode 5.mkv").mtime_ns == entry.mtime_ns);
}

MACHA_TEST("namespace_tree", test_appending_an_extent_does_not_rewrite_the_extent_list) {
    // Extent chunk boundaries are decided by each extent's own content address
    // rather than by position, so appending cannot move the boundaries of the
    // extents already written. Without that, every write to a large file would
    // rewrite its whole extent list -- which is the pathology
    // record_entry_change already goes out of its way to avoid at the delta
    // level (src/metadata.cpp:483-501).
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

    // Measured: 4,000 extents is 15 nodes, and the append rewrites 3 of them --
    // the tail chunk the new extent joined, the extent spine above it, and the
    // leaf carrying the entry. Bounded by the depth of the file's extent tree,
    // not by how many extents it has.
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

    // Absent paths, including ones that sort before everything, between two
    // leaves, and after everything.
    CHECK(!namespace_tree_lookup(root, "", store).has_value());
    CHECK(!namespace_tree_lookup(root, "/AAA", store).has_value());
    CHECK(!namespace_tree_lookup(root, "/TV/Show 3/Season 1/Episode 999.mkv", store).has_value());
    CHECK(!namespace_tree_lookup(root, "/zzz", store).has_value());
}

MACHA_TEST("namespace_tree", test_a_leaf_stays_small_however_many_extents_a_file_has) {
    // The residency claim. A leaf holds stat data and, past a threshold, a
    // reference -- so a film with thousands of extents does not put hundreds of
    // kilobytes inside the node a path lookup has to read. Without this, Stage D
    // (demand-loaded extents) would need another format change rather than a
    // change of when a node is fetched.
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

    // 12,500 extents is 612 KB at the 49 bytes each costs in the snapshot
    // today. No node in the tree comes close to that.
    CHECK(stats.largest_node_bytes < 64 * 1024);

    // The small file's extents stayed inline, so an ordinary file is still one
    // node rather than two.
    const auto read_back = read_namespace_tree(root, store);
    CHECK(read_back.at("/small.mkv").extents.size() == 4);
    CHECK(read_back.at("/huge.mkv").extents.size() == 12500);
}

MACHA_TEST("namespace_tree", test_a_corrupt_node_is_refused_rather_than_trusted) {
    // The acceptance the plan owed Stage B, in the shape of the FUSE journal
    // fuzz: corrupt any byte of any node and the reader must refuse it, not
    // trust it. Nodes come from the content-addressed control store, so a
    // corrupt or forged one is the input this decoder actually has to survive
    // once SM14 makes it reachable from a record.
    //
    // A defect was found while writing this, and honesty about which found it
    // matters: reading the decoder did, not this test. The external-extent
    // branch read a uint64 count from the node and reserved against it, capped
    // at 10,000,000 -- 560 MB at the 56 bytes an ExtentRef occupies, sized
    // from an unvalidated integer. Every other reserve in that file is bounded
    // by `reader.remaining()`; that one could not be, because the extents live
    // in other nodes. It is now not reserved at all.
    //
    // This test would probably NOT have caught it. Corrupting that count makes
    // the old code allocate half a gigabyte and carry on succeeding, which
    // looks identical to passing. A fuzz case catches crashes, hangs and
    // accepted garbage; it does not catch "worked, expensively". Worth knowing
    // before anyone treats a green fuzz run as evidence that a decoder is
    // safe against forged sizes -- for that, read every reserve and ask what
    // bounds it.
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

    // Every node, every byte position, one bit flipped. The reader may throw,
    // may return something smaller, may refuse the lookup -- what it must not
    // do is crash, hang, or size an allocation from the damaged bytes.
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
                // Re-store under the ORIGINAL id, so the corruption is
                // reachable rather than simply becoming a different node that
                // nothing points at.
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
    // The claim the whole structure rests on, made provable by counting reads
    // rather than asserted in a comment: a getattr fetches the path from the
    // root to one leaf and nothing else. Not the namespace, and not a single
    // extent node -- neither the target's nor those of the entries the leaf
    // scan walks past on the way to it.
    //
    // A library of films, so every entry has an external extent spine and any
    // accidental extent fetch shows up immediately.
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    for (int i = 0; i < 200; ++i)
        entries["/film" + std::to_string(i) + ".mkv"] = make_file(100 + i, 400);

    MemoryNamespaceNodeStore store;
    const auto root = build_namespace_tree(entries, store);
    const auto shape = namespace_tree_stats(root, store);
    REQUIRE(shape.extent_nodes > 0);
    REQUIRE(shape.depth >= 2);

    // Stat only.
    store.forget_reads();
    const auto stat = namespace_tree_lookup(root, "/film137.mkv", store, false);
    const auto stat_reads = store.reads();
    REQUIRE(stat);
    CHECK(stat->size == entries.at("/film137.mkv").size);
    // The stat answer carries no extents, by construction.
    CHECK(stat->extents.empty());
    // One node per level and no more. Depth is small and bounded by log n,
    // which is the entire point -- the library has 201 entries and thousands
    // of extents behind them.
    CHECK(stat_reads <= shape.depth);

    // The same lookup asking for extents pays for them, which is how we know
    // the first one was actually avoiding work rather than the tree being
    // empty of extent nodes.
    store.forget_reads();
    const auto full = namespace_tree_lookup(root, "/film137.mkv", store, true);
    const auto full_reads = store.reads();
    REQUIRE(full);
    CHECK(full->extents.size() == 400);
    CHECK(full_reads > stat_reads);

    // And a stat for a path that is not there is equally cheap: it settles on
    // the leaf without descending into anything.
    store.forget_reads();
    CHECK(!namespace_tree_lookup(root, "/absent.mkv", store, false));
    CHECK(store.reads() <= shape.depth);
}

// A snapshot with every field SM14 has to carry populated, so a round trip
// proves the format preserves the record and not merely the namespace. The
// entries are the caller's; everything else here is cluster state that must
// survive the re-rooting untouched.
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
    // The Stage B deliverable: the record shape. Everything before this built
    // a tree nothing could reach; this is the encoding that reaches it.
    const auto entries = library(20, 10);
    const auto snapshot = populated_snapshot(entries);
    const auto sm13 = encode_snapshot(snapshot);

    MemoryNamespaceNodeStore store;
    const auto detached = detach_namespace(snapshot, store);
    REQUIRE(detached.namespace_root.has_value());
    CHECK(detached.entries.empty());
    const auto sm14 = encode_snapshot_v14(detached);

    // The number this stage exists for. The SM13 payload is the library; the
    // SM14 payload is the cluster's own state plus a 32-byte pointer at it.
    // Measured on this fixture: 434,731 bytes against 590, for 302 entries.
    // The ratio is asserted rather than either size, because the ratio is the
    // claim and it has no ceiling -- es-1's real head is 22,525,100 SM13 bytes
    // against the same ~590.
    CHECK(sm14.size() < 1024);
    CHECK(sm13.size() > 100 * sm14.size());

    // Read it back with no store in sight -- which is the point: decoding a
    // record no longer materialises a namespace.
    const auto decoded = decode_snapshot(sm14);
    REQUIRE(decoded.namespace_root.has_value());
    CHECK(*decoded.namespace_root == *detached.namespace_root);
    CHECK(decoded.entries.empty());

    // Nothing else moved. Re-encoding the reattached snapshot as SM13 has to
    // reproduce the original payload byte for byte, which covers every field
    // individually without listing them: garbage, node status, identity
    // resets, merge parents, the write floor, the participant roster, the
    // branch floor and the retention baseline.
    const auto reattached = attach_namespace(decoded, store);
    CHECK(reattached.entries == entries);
    CHECK(!reattached.namespace_root.has_value());
    CHECK(encode_snapshot(reattached) == sm13);
}

MACHA_TEST("namespace_tree", test_the_record_stops_growing_with_the_library) {
    // Stated as a property rather than a measurement, because it is the whole
    // claim: a commit's cost has to stop being a function of how much media
    // the cluster holds. Two libraries an order of magnitude apart, one
    // payload size.
    MemoryNamespaceNodeStore small_store, large_store;
    const auto small = encode_snapshot_v14(
        detach_namespace(populated_snapshot(library(2, 4)), small_store));
    const auto large = encode_snapshot_v14(
        detach_namespace(populated_snapshot(library(40, 20)), large_store));

    CHECK(small.size() == large.size());
    // And the two are the same record apart from where they point, so the
    // difference between them is exactly one content address.
    size_t differing = 0;
    for (size_t i = 0; i < small.size(); ++i)
        if (small[i] != large[i])
            ++differing;
    CHECK(differing <= 32);
}

MACHA_TEST("namespace_tree", test_a_snapshot_never_carries_its_namespace_in_two_places) {
    // A record with both forms populated would let them disagree, and no
    // reader would have a rule for which one is the namespace. Every path into
    // that state is closed, and the closure is what stops a half-migrated
    // snapshot publishing an empty library under a valid-looking hash.
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

    // A namespace with no root directory is not a filesystem. SM13's decoder
    // checks that on every read; SM14's cannot, because it has no store, so
    // the check moved to the reader that does.
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
    // The two halves joined: a record off the wire, and a getattr answered
    // from it without the library ever existing as a map. This is what Stage C
    // converts the FUSE path to, and it is already true here.
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
    // The record is the one part of this that arrives from the network, so its
    // decoder gets the same treatment the node decoder got: flip a bit at
    // every position and require refusal or coping, never a crash, a hang or
    // an allocation sized from the damaged bytes.
    MemoryNamespaceNodeStore store;
    const auto payload = encode_snapshot_v14(detach_namespace(populated_snapshot(library(2, 2)), store));

    size_t refused = 0, accepted = 0;
    for (size_t at = 0; at < payload.size(); ++at) {
        auto damaged = payload;
        damaged[at] ^= 0x40;
        try {
            const auto decoded = decode_snapshot(damaged);
            ++accepted;
            // Anything that decodes is still a record that points somewhere
            // rather than one that carries a namespace; a flipped byte must
            // never produce entries out of nothing.
            CHECK(decoded.entries.empty());
            CHECK(decoded.namespace_root.has_value());
        } catch (const std::exception&) {
            ++refused;
        }
    }
    // A flip in the root address or a node id decodes cleanly and addresses
    // something that is not there -- that is the content-addressing doing its
    // job one layer down, not a decoder failure. What matters is that the
    // structural fields are checked, and they are.
    CHECK(refused > 0);
    CHECK(refused + accepted == payload.size());
}

MACHA_TEST("namespace_tree", test_a_detached_namespace_never_reaches_a_reader_that_wants_entries) {
    // The Stage C guard, stated where it can be tested. Around forty call
    // sites read `snapshot->entries` directly and none of them treats an empty
    // map as a failure; for the three that compute reachability it reads as
    // "nothing is live", which is what destructive GC acts on. So the manager
    // refuses to hand out a view over a detached namespace at all, and this is
    // the predicate it refuses with.
    MemoryNamespaceNodeStore store;
    const auto snapshot = populated_snapshot(library(2, 2));

    // An ordinary materialised snapshot passes, including an empty one: a
    // cluster with no files is not the same thing as a namespace that is
    // somewhere else.
    require_materialised_namespace(snapshot);
    require_materialised_namespace(MetadataSnapshot{});

    const auto detached = detach_namespace(snapshot, store);
    bool refused = false;
    try {
        require_materialised_namespace(detached);
    } catch (const MetadataNotReady&) {
        refused = true;
    }
    CHECK(refused);

    // And it passes again once the namespace is actually there, so the guard
    // tracks where the entries are rather than which format produced them.
    require_materialised_namespace(attach_namespace(detached, store));
}

MACHA_TEST("namespace_tree", test_a_reader_sees_the_same_namespace_in_either_form) {
    // What the converted reachability sites depend on: the primitives give the
    // same answer whichever form the namespace is in, so a reader that uses
    // them works before and after the cutover and cannot be quietly wrong on
    // one side of it.
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

    // And the point lookup agrees with both, including for a path that is not
    // there.
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
    // The failure mode the primitives exist to remove. Before them, a
    // reachability walk over a detached snapshot visited nothing and reported
    // success, which reads as "no object in this library is live" -- the exact
    // input destructive GC wants. It must be an error, and a named one.
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

} // namespace

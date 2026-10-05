// SPDX-License-Identifier: GPL-3.0-or-later
//
// The merge of two heads from the heads alone. Each case builds two or three
// nodes' namespaces by the mutations they made, as the commit path stamps
// them, and merges what they ended with.
#include "crypto.hpp"
#include "metadata/metadata.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include <map>
#include <string>

using namespace macha;
using namespace macha::test_support;

namespace {

// One node's lineage: a snapshot and the mutations it authors on it.
struct Author {
    NodeId id{};
    uint64_t sequence{};
    MetadataSnapshot head;
    // Whether this node's code stamps what it writes.
    bool stamps{true};

    explicit Author(uint8_t tag, bool stamping = true) : stamps(stamping) {
        id.bytes[0] = tag;
        head = decode_snapshot(genesis_metadata().payload);
    }

    MetadataDot next() {
        if (stamps && !head.legacy_clock)
            head.legacy_clock = head.mutation_sequences;
        head.mutation_sequences[id] = ++sequence;
        return {id, sequence};
    }
    void put(const std::string& path, FsEntry entry) {
        const auto prior = head.entries.find(path);
        const auto dot = next();
        if (stamps)
            stamp_entry_provenance(entry, path,
                                   prior == head.entries.end() ? nullptr : &prior->second, dot);
        head.entries[path] = std::move(entry);
    }
    void file(const std::string& path, uint64_t size, int64_t mtime = 100) {
        FsEntry entry;
        entry.type = EntryType::file;
        entry.size = size;
        entry.mtime_ns = mtime;
        put(path, entry);
    }
    void directory(const std::string& path) {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        put(path, entry);
    }
    // Changes the file already at the path, as a writer does: it starts from
    // what is there.
    void edit(const std::string& path, uint64_t size, int64_t mtime) {
        auto entry = head.entries.at(path);
        entry.size = size;
        entry.mtime_ns = mtime;
        entry.provenance = {};
        put(path, entry);
    }
    void erase(const std::string& path) {
        (void)next();
        head.entries.erase(path);
    }
    void rename(const std::string& from, const std::string& to) {
        auto entry = head.entries.at(from);
        if (stamps && entry.provenance.file_id == NodeId{})
            entry.provenance.file_id = legacy_file_id(from);
        const auto dot = next();
        head.entries.erase(from);
        if (stamps)
            stamp_entry_provenance(entry, to, nullptr, dot);
        head.entries[to] = std::move(entry);
    }
    // Takes everything another node has, keeping its own identity.
    void adopt(const MetadataSnapshot& other) { head = other; }
};

Hash256 head_of(const MetadataSnapshot& snapshot) {
    return sha256(encode_snapshot(snapshot));
}

// The merge, checked to be the same whichever head is named first.
MetadataMergeResult merge(const MetadataSnapshot& a, const MetadataSnapshot& b) {
    auto forward = merge_metadata_heads(a, b, head_of(a), head_of(b));
    const auto backward = merge_metadata_heads(b, a, head_of(b), head_of(a));
    CHECK(encode_snapshot(forward.snapshot) == encode_snapshot(backward.snapshot));
    CHECK(forward.conflicts_created == backward.conflicts_created);
    return forward;
}

bool has(const MetadataSnapshot& snapshot, const std::string& path) {
    return snapshot.entries.contains(path);
}

// Two nodes that share a namespace with one file in it.
struct Pair {
    Author a{1};
    Author b{2};
    Pair() {
        a.directory("/d");
        a.file("/d/shared", 10);
        b.adopt(a.head);
    }
};

MACHA_FAST_TEST("metadata_merge", test_a_file_one_head_never_saw_is_kept) {
    Pair p;
    p.a.file("/d/from-a", 1);
    p.b.file("/d/from-b", 2);
    const auto merged = merge(p.a.head, p.b.head).snapshot;
    CHECK(has(merged, "/d/from-a"));
    CHECK(has(merged, "/d/from-b"));
    CHECK(has(merged, "/d/shared"));
    CHECK(merged.conflicts.empty());
    CHECK(merged.mutation_sequences.at(p.a.id) == p.a.sequence);
    CHECK(merged.mutation_sequences.at(p.b.id) == p.b.sequence);
}

MACHA_FAST_TEST("metadata_merge", test_a_file_one_head_removed_stays_removed) {
    Pair p;
    p.a.erase("/d/shared");
    p.b.file("/d/other", 2);
    const auto merged = merge(p.a.head, p.b.head).snapshot;
    CHECK(!has(merged, "/d/shared"));
    CHECK(has(merged, "/d/other"));
    CHECK(merged.conflicts.empty());

    // Merging the result with either head again changes nothing.
    CHECK(merge(merged, p.a.head).snapshot.entries == merged.entries);
    CHECK(merge(merged, p.b.head).snapshot.entries == merged.entries);
}

MACHA_FAST_TEST("metadata_merge", test_an_edit_survives_a_concurrent_removal) {
    Pair p;
    p.a.erase("/d/shared");
    p.b.edit("/d/shared", 99, 500);
    const auto merged = merge(p.a.head, p.b.head).snapshot;
    REQUIRE(has(merged, "/d/shared"));
    CHECK(merged.entries.at("/d/shared").size == 99);
    CHECK(merged.conflicts.empty());
}

MACHA_FAST_TEST("metadata_merge", test_the_newer_of_two_values_wins_without_a_conflict) {
    Pair p;
    p.a.edit("/d/shared", 20, 200);
    p.b.adopt(p.a.head);
    p.b.edit("/d/shared", 30, 150); // newer by cause, older by its clock
    const auto merged = merge(p.a.head, p.b.head).snapshot;
    CHECK(merged.entries.at("/d/shared").size == 30);
    CHECK(merged.conflicts.empty());
}

MACHA_FAST_TEST("metadata_merge", test_concurrent_edits_install_the_later_and_keep_both) {
    Pair p;
    p.a.edit("/d/shared", 20, 200);
    p.b.edit("/d/shared", 30, 300);
    const auto result = merge(p.a.head, p.b.head);
    const auto& merged = result.snapshot;
    CHECK(result.conflicts_created == 1);
    CHECK(merged.entries.at("/d/shared").size == 30);
    REQUIRE(merged.conflicts.size() == 1);
    const auto& conflict = merged.conflicts.begin()->second;
    CHECK(conflict.later_installed);
    CHECK(conflict.key == "/d/shared");
    CHECK(!conflict.base_entry.has_value());
    REQUIRE(conflict.left_entry.has_value());
    REQUIRE(conflict.right_entry.has_value());
    CHECK(conflict.left_entry->size == 30);
    CHECK(conflict.right_entry->size == 20);
    CHECK(conflict_installed_entry(conflict) == merged.entries.at("/d/shared"));

    // Either head merged in again leaves the same conflict, once.
    const auto again = merge(merged, p.a.head).snapshot;
    CHECK(again.entries == merged.entries);
    CHECK(again.conflicts == merged.conflicts);

    // A later change of the subject is the decision: the record goes.
    Author c{3};
    c.adopt(merged);
    c.edit("/d/shared", 40, 400);
    auto decided = c.head;
    CHECK(prune_superseded_conflicts(decided) == 1);
    const auto after = merge(decided, merged).snapshot;
    CHECK(after.entries.at("/d/shared").size == 40);
    CHECK(after.conflicts.empty());
}

MACHA_FAST_TEST("metadata_merge", test_the_same_content_written_twice_is_no_conflict) {
    Pair p;
    p.a.file("/d/both", 5, 100);
    p.b.file("/d/both", 5, 200);
    const auto result = merge(p.a.head, p.b.head);
    CHECK(result.conflicts_created == 0);
    CHECK(has(result.snapshot, "/d/both"));
}

// A rename on one side meets an edit of the same file on the other: one
// file, at the new name, with the edit.
MACHA_FAST_TEST("metadata_merge", test_a_rename_meets_a_concurrent_edit_of_the_same_file) {
    Pair p;
    p.a.rename("/d/shared", "/d/renamed");
    p.b.edit("/d/shared", 77, 700);
    const auto result = merge(p.a.head, p.b.head);
    const auto& merged = result.snapshot;
    CHECK(!has(merged, "/d/shared"));
    REQUIRE(has(merged, "/d/renamed"));
    CHECK(merged.entries.at("/d/renamed").size == 77);
    CHECK(merged.entries.at("/d/renamed").provenance.name ==
          p.a.head.entries.at("/d/renamed").provenance.name);
    CHECK(merged.entries.at("/d/renamed").provenance.content ==
          p.b.head.entries.at("/d/shared").provenance.content);
    CHECK(result.conflicts_created == 0);
    // Stable under being merged back into either side.
    CHECK(merge(merged, p.b.head).snapshot.entries == merged.entries);
    CHECK(merge(merged, p.a.head).snapshot.entries == merged.entries);

    // A rename alone is simply taken.
    Pair q;
    q.a.rename("/d/shared", "/d/renamed");
    q.b.file("/d/unrelated", 1);
    const auto plain = merge(q.a.head, q.b.head).snapshot;
    CHECK(!has(plain, "/d/shared"));
    CHECK(has(plain, "/d/renamed"));
}

MACHA_FAST_TEST("metadata_merge", test_two_renames_of_one_file_keep_one_name) {
    Pair p;
    p.a.rename("/d/shared", "/d/by-a");
    p.b.rename("/d/shared", "/d/by-b");
    const auto merged = merge(p.a.head, p.b.head).snapshot;
    CHECK(!has(merged, "/d/shared"));
    CHECK(has(merged, "/d/by-a") != has(merged, "/d/by-b"));
}

// A directory removed on one side while the other puts a file in it comes
// back, since the file needs it.
MACHA_FAST_TEST("metadata_merge", test_a_removed_directory_returns_for_a_file_added_under_it) {
    Pair p;
    p.a.erase("/d/shared");
    p.a.erase("/d");
    p.b.file("/d/new", 3);
    const auto merged = merge(p.a.head, p.b.head).snapshot;
    CHECK(has(merged, "/d/new"));
    REQUIRE(has(merged, "/d"));
    CHECK(merged.entries.at("/d").type == EntryType::directory);
    CHECK(!has(merged, "/d/shared"));
}

// A path made a file on one side and a directory with a file in it on the
// other: the directory stands and the file is the conflict's alternative.
MACHA_FAST_TEST("metadata_merge", test_a_directory_with_entries_wins_a_clash_with_a_file) {
    Pair p;
    p.a.file("/d/clash", 9, 900);
    p.b.directory("/d/clash");
    p.b.file("/d/clash/inside", 1, 100);
    const auto result = merge(p.a.head, p.b.head);
    const auto& merged = result.snapshot;
    REQUIRE(has(merged, "/d/clash"));
    CHECK(merged.entries.at("/d/clash").type == EntryType::directory);
    CHECK(has(merged, "/d/clash/inside"));
    REQUIRE(merged.conflicts.size() == 1);
    const auto& conflict = merged.conflicts.begin()->second;
    CHECK(conflict.key == "/d/clash");
    CHECK(conflict.left_entry->type == EntryType::directory);
    CHECK(conflict.right_entry->type == EntryType::file);
}

// Entries from before provenance. Two nodes that began stamping from the
// same namespace: a legacy file one removed stays removed, and one changed
// wins over one left alone.
MACHA_FAST_TEST("metadata_merge", test_entries_from_before_provenance_merge_by_the_legacy_clock) {
    Author old{1, false};
    old.directory("/d");
    old.file("/d/kept", 1);
    old.file("/d/removed", 2);
    old.file("/d/changed", 3);
    Author a{1};
    a.sequence = old.sequence;
    a.adopt(old.head);
    Author b{2};
    b.adopt(old.head);
    REQUIRE(a.head.entries.at("/d/kept").provenance.empty());

    a.erase("/d/removed");
    b.edit("/d/changed", 33, 300);
    b.file("/d/new", 4);
    const auto merged = merge(a.head, b.head).snapshot;
    CHECK(has(merged, "/d/kept"));
    CHECK(merged.entries.at("/d/kept").provenance.empty());
    CHECK(!has(merged, "/d/removed"));
    CHECK(merged.entries.at("/d/changed").size == 33);
    CHECK(merged.entries.at("/d/changed").provenance.file_id == legacy_file_id("/d/changed"));
    CHECK(has(merged, "/d/new"));
    CHECK(merged.conflicts.empty());
    REQUIRE(merged.legacy_clock.has_value());
    CHECK(merged.legacy_clock->at(old.id) == old.sequence);
}

// A node that was away, never stamped, and wrote things nobody saw: what it
// alone has is kept, since nothing shows the others removed it; what the
// others wrote since is kept; and a node that wrote nothing unseen has its
// stale entries removed.
MACHA_FAST_TEST("metadata_merge", test_a_returning_node_from_before_provenance_loses_nothing) {
    Author old{1, false};
    old.directory("/d");
    old.file("/d/common", 1);
    old.file("/d/later-removed", 2);

    Author away{9, false};
    away.adopt(old.head);
    away.file("/d/written-while-away", 7);

    Author cluster{1};
    cluster.sequence = old.sequence;
    cluster.adopt(old.head);
    cluster.erase("/d/later-removed");
    cluster.file("/d/written-since", 8);

    const auto merged = merge(away.head, cluster.head).snapshot;
    CHECK(has(merged, "/d/common"));
    CHECK(has(merged, "/d/written-while-away"));
    CHECK(has(merged, "/d/written-since"));
    // The away node has commits the cluster never saw, so its copy of the
    // removed file cannot be told from one of those: it comes back.
    CHECK(has(merged, "/d/later-removed"));

    // Away but silent: everything it has, the cluster saw, so the removal holds.
    Author silent{8, false};
    silent.adopt(old.head);
    const auto quiet = merge(silent.head, cluster.head).snapshot;
    CHECK(!has(quiet, "/d/later-removed"));
    CHECK(has(quiet, "/d/written-since"));
    CHECK(has(quiet, "/d/common"));
}

MACHA_FAST_TEST("metadata_merge", test_three_heads_merge_to_the_same_namespace_in_any_order) {
    Author a{1};
    a.directory("/d");
    a.file("/d/one", 1);
    a.file("/d/two", 2);
    a.file("/d/three", 3);
    Author b{2};
    b.adopt(a.head);
    Author c{3};
    c.adopt(a.head);

    a.erase("/d/one");
    a.file("/d/a", 10);
    b.edit("/d/two", 22, 220);
    b.rename("/d/three", "/d/three-b");
    c.edit("/d/two", 23, 230);
    c.edit("/d/three", 33, 330);
    c.file("/d/c", 30);

    const auto ab_c = merge(merge(a.head, b.head).snapshot, c.head).snapshot;
    const auto a_bc = merge(a.head, merge(b.head, c.head).snapshot).snapshot;
    const auto ac_b = merge(merge(a.head, c.head).snapshot, b.head).snapshot;
    CHECK(ab_c.entries == a_bc.entries);
    CHECK(ab_c.entries == ac_b.entries);
    CHECK(ab_c.mutation_sequences == a_bc.mutation_sequences);
    CHECK(!has(ab_c, "/d/one"));
    CHECK(has(ab_c, "/d/a"));
    CHECK(has(ab_c, "/d/c"));
    CHECK(ab_c.entries.at("/d/two").size == 23);
    CHECK(!has(ab_c, "/d/three"));
    CHECK(ab_c.entries.at("/d/three-b").size == 33);
}

MACHA_FAST_TEST("metadata_merge", test_the_catalogue_root_follows_its_dot) {
    const auto root = [](uint8_t n) { return object_id(pattern(8, n)); };
    Author a{1};
    a.directory("/d");
    a.head.catalogue_root = root(1);
    a.head.catalogue_dot = a.next();
    Author b{2};
    b.adopt(a.head);

    // One side moved it: taken.
    b.head.catalogue_root = root(2);
    b.head.catalogue_dot = b.next();
    a.file("/d/x", 1);
    auto merged = merge(a.head, b.head).snapshot;
    CHECK(merged.catalogue_root == root(2));
    CHECK(merged.catalogue_dot == b.head.catalogue_dot);
    CHECK(merged.conflicts.empty());

    // Both moved it: one stands, the other is kept for the catalogue to merge.
    a.head.catalogue_root = root(3);
    a.head.catalogue_dot = a.next();
    merged = merge(a.head, b.head).snapshot;
    REQUIRE(merged.conflicts.size() == 1);
    const auto& conflict = merged.conflicts.begin()->second;
    CHECK(conflict.kind == MetadataConflictKind::catalogue_root);
    CHECK(conflict.later_installed);
    CHECK(conflict.left_catalogue_root == merged.catalogue_root);
    CHECK(conflict_installed_catalogue_root(conflict) == merged.catalogue_root);
    CHECK((merged.catalogue_root == root(2) || merged.catalogue_root == root(3)));
    CHECK(conflict.right_catalogue_root != conflict.left_catalogue_root);
}

} // namespace

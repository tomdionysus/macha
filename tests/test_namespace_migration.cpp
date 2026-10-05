// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "storage/local_store.hpp"
#include "metadata/metadata.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "service/service.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>

using namespace macha;
using namespace macha::test_support;

namespace {

// What macha-namespace-migrate does to one stopped node, through the same
// plan_namespace_migration the tool uses.
struct MigrationResult {
    MetadataRecord record;
    Hash256 hash{};
    ObjectId root{};
    size_t entries{};
    uint64_t payload_before{};
    uint64_t payload_after{};
};

MigrationResult migrate_state(const Config& config, const ClusterKeys& keys,
                              const std::vector<NodeId>& witnesses, bool install = true) {
    MetadataReplica replica(config.state_path, keys.storage);
    const auto head = replica.committed();
    REQUIRE(valid_metadata_record(head));

    // An empty metadata_store.path means <state_path>/metadata-objects, as
    // normalize_config resolves it.
    const auto object_path = config.metadata_store.path.empty()
                                 ? config.state_path / "metadata-objects"
                                 : config.metadata_store.path;
    LocalStore objects(object_path,
                       LocalStoreOptions{config.metadata_store.limit, 0,
                                         config.metadata_store.packing.threshold,
                                         config.metadata_store.packing.target_size},
                       keys.storage);
    LocalNamespaceNodeStore nodes(objects);
    const auto migration = plan_namespace_migration(head, nodes);
    if (install)
        REQUIRE(replica.install_migrated_head(migration.record, witnesses, "test migration"));
    return {migration.record, migration.record.hash, migration.root, migration.entries,
            migration.previous_payload_bytes, migration.record.payload.size()};
}

// Two stopped nodes re-rooted onto one record, as the tool's --adopt does:
// each builds the tree into its own store, and the second installs the
// first's record once its own namespace yields the same root.
void migrate_pair(const Config& first, const Config& second, const ClusterKeys& keys,
                  const std::vector<NodeId>& witnesses) {
    const auto leader = migrate_state(first, keys, witnesses);
    const auto follower = migrate_state(second, keys, witnesses, false);
    REQUIRE(follower.root == leader.root);
    MetadataReplica replica(second.state_path, keys.storage);
    REQUIRE(replica.install_migrated_head(leader.record, witnesses, "test migration"));
}

// Every tree node the service's head reaches, read through its own store.
std::vector<ObjectId> reachable_tree_nodes(Service& service) {
    const auto snapshot = service.metadata_manager().snapshot();
    REQUIRE(snapshot.namespace_root.has_value());
    std::vector<ObjectId> nodes;
    collect_namespace_tree_nodes(*snapshot.namespace_root, service.filesystem().namespace_nodes(),
                                 nodes);
    return nodes;
}

// One node is the whole cluster here, so the service writing the library needs
// a write floor of one.
void make_solo(Config& config) {
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.write_copies = 1;
}

// A snapshot with the policy fields a real one carries, so encode/merge paths
// behave as they do in production.
MetadataSnapshot populated_snapshot(std::map<std::string, FsEntry> entries) {
    MetadataSnapshot snapshot;
    snapshot.entries = std::move(entries);
    snapshot.data_replication = 2;
    snapshot.extent_size = 4 * 1024 * 1024;
    snapshot.metadata_write_replicas_required = 2;
    snapshot.retention_baseline_complete = true;
    return snapshot;
}

FsEntry make_directory(uint64_t seed) {
    FsEntry entry;
    entry.type = EntryType::directory;
    entry.mode = 0755;
    entry.ctime_ns = static_cast<int64_t>(seed);
    entry.mtime_ns = static_cast<int64_t>(seed);
    return entry;
}

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
    entry.size = static_cast<uint64_t>(extents) * 4 * 1024 * 1024;
    entry.ctime_ns = static_cast<int64_t>(seed) * 1000;
    entry.mtime_ns = static_cast<int64_t>(seed) * 2000;
    entry.version = 1;
    for (size_t i = 0; i < extents; ++i) {
        ExtentRef extent;
        extent.offset = static_cast<uint64_t>(i) * 4 * 1024 * 1024;
        extent.length = 4 * 1024 * 1024;
        extent.id = fake_object(seed * 1000 + i);
        entry.extents.push_back(extent);
    }
    return entry;
}

std::vector<uint8_t> pattern(size_t bytes, uint8_t seed) {
    std::vector<uint8_t> out(bytes);
    for (size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<uint8_t>(seed + i * 7);
    return out;
}

void write_file(Service& service, const std::string& path, const std::vector<uint8_t>& bytes) {
    service.filesystem().create_file(path, 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(path, true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
}

std::vector<uint8_t> read_file(Service& service, const std::string& path, size_t bytes) {
    auto reader = service.filesystem().open_read(path);
    std::vector<uint8_t> out(bytes);
    const auto got = reader->read(0, out);
    out.resize(got);
    return out;
}

// Every write says where it came from, on a namespace held as a map and on
// one held as a tree: a new file gets an identity and its two dots, a change
// moves the content dot, a rename keeps the identity and moves the name dot,
// and an append travels as a delta that carries its dot.
MACHA_TEST("namespace_migration", test_every_write_records_where_it_came_from) {
    TestCluster cluster;
    auto config = cluster.node_config("provenance");
    make_solo(config);
    NodeId node_id{};
    // Whether `dot` is the head's latest mutation by its author.
    const auto newest = [](Service& service, const MetadataDot& dot) {
        const auto clock = service.metadata_manager().snapshot().mutation_sequences;
        const auto found = clock.find(dot.author);
        return found != clock.end() && found->second == dot.sequence;
    };
    const auto check_lifecycle = [&](Service& service, const std::string& dir) {
        auto& fs = service.filesystem();
        fs.mkdir(dir, 0755, getuid(), getgid());
        const auto made = fs.getattr(dir);
        CHECK(made.provenance.file_id != NodeId{});
        CHECK(made.provenance.content == made.provenance.name);
        CHECK(clock_covers(service.metadata_manager().snapshot().mutation_sequences,
                           made.provenance.name));

        write_file(service, dir + "/a.bin", pattern(64 * 1024, 7));
        const auto written = fs.getattr(dir + "/a.bin");
        CHECK(written.provenance.file_id != NodeId{});
        CHECK(written.provenance.file_id != made.provenance.file_id);
        CHECK(written.provenance.name.sequence < written.provenance.content.sequence);

        fs.chmod(dir + "/a.bin", 0600);
        const auto changed = fs.getattr(dir + "/a.bin");
        CHECK(changed.provenance.file_id == written.provenance.file_id);
        CHECK(changed.provenance.name == written.provenance.name);
        CHECK(changed.provenance.content.sequence > written.provenance.content.sequence);

        fs.rename(dir + "/a.bin", dir + "/b.bin", false);
        const auto moved = fs.getattr(dir + "/b.bin");
        CHECK(moved.provenance.file_id == written.provenance.file_id);
        CHECK(moved.provenance.content == changed.provenance.content);
        CHECK(moved.provenance.name.sequence > changed.provenance.content.sequence);

        // A second extent appended: a delta commit, and the content dot moves.
        const auto more = pattern(64 * 1024, 8);
        auto writer = fs.open_write(dir + "/b.bin", false);
        REQUIRE(writer->write(moved.size, more) == more.size());
        writer->commit();
        const auto grown = fs.getattr(dir + "/b.bin");
        CHECK(grown.size == moved.size + more.size());
        CHECK(grown.provenance.file_id == written.provenance.file_id);
        CHECK(grown.provenance.name == moved.provenance.name);
        CHECK(grown.provenance.content.sequence > moved.provenance.content.sequence);
        CHECK(newest(service, grown.provenance.content));
        // A map-form record is far larger than the append, so it travels as
        // a delta; a tree-form record is already smaller than one.
        if (!service.metadata_manager().snapshot().namespace_root) {
            const auto head = service.local_state().replica().accepted_heads().front();
            const auto entry = service.local_state().replica().history_entry(head.hash);
            REQUIRE(entry.has_value());
            CHECK(entry->body == MetadataHistoryEntry::Body::delta);
        }

        // A directory rename carries every identity under it.
        fs.rename(dir, dir + "-moved", false);
        CHECK(fs.getattr(dir + "-moved").provenance.file_id == made.provenance.file_id);
        const auto carried = fs.getattr(dir + "-moved/b.bin");
        CHECK(carried.provenance.file_id == written.provenance.file_id);
        CHECK(carried.provenance.content == grown.provenance.content);
        CHECK(newest(service, carried.provenance.name));
    };
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        REQUIRE(wait_metadata_writable(service));
        check_lifecycle(service, "/map");
        const auto snapshot = service.metadata_manager().snapshot();
        REQUIRE(!snapshot.namespace_root.has_value());
        REQUIRE(snapshot.legacy_clock.has_value());
        // The root was there before provenance and nothing has changed it.
        CHECK(service.filesystem().getattr("/").provenance.empty());
        service.stop();
    }
    (void)migrate_state(config, cluster.keys(), {node_id});
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    REQUIRE(service.metadata_manager().snapshot().namespace_root.has_value());
    // What was written before the tree is still there, provenance and all.
    CHECK(service.filesystem().getattr("/map-moved/b.bin").provenance.file_id != NodeId{});
    check_lifecycle(service, "/tree");
    service.stop();
}

MACHA_TEST("namespace_migration", test_a_migrated_node_serves_and_writes_its_library) {
    // End to end on one node: write a library, stop, re-root onto the tree,
    // restart. Every read must match and the filesystem must stay writable.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-serves");
    make_solo(config);
    const auto payload_a = pattern(3 * 1024 * 1024, 11);
    const auto payload_b = pattern(512 * 1024, 200);

    NodeId node_id{};
    {
        // Destroyed, not merely stopped: a stopped Service still holds the
        // storage lock the migration needs.
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        service.filesystem().mkdir("/TV", 0755, getuid(), getgid());
        service.filesystem().mkdir("/TV/Show", 0755, getuid(), getgid());
        write_file(service, "/TV/Show/one.mkv", payload_a);
        write_file(service, "/TV/Show/two.mkv", payload_b);
        service.filesystem().mkdir("/Music", 0755, getuid(), getgid());
        CHECK(read_file(service, "/TV/Show/one.mkv", payload_a.size()) == payload_a);
        service.stop();
    }

    // The sole witness is this node, the whole cluster here.
    const auto migration = migrate_state(config, cluster.keys(), {node_id});
    CHECK(migration.entries == 6); // /, /TV, /TV/Show, two files, /Music
    // The record no longer carries the entry map.
    CHECK(migration.payload_after < migration.payload_before);

    // The head is tree-backed, with no entry map.
    {
        MetadataReplica replica(config.state_path, cluster.keys().storage);
        const auto head = replica.committed();
        CHECK(head.hash == migration.hash);
        const auto snapshot = decode_snapshot(head.payload);
        REQUIRE(snapshot.namespace_root.has_value());
        CHECK(*snapshot.namespace_root == migration.root);
        CHECK(snapshot.entries.empty());
    }

    // A new Service over the same state directory: the node restarting.
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    (void)service.filesystem();

    CHECK(service.filesystem().getattr("/TV/Show/one.mkv").size == payload_a.size());
    CHECK(service.filesystem().getattr("/TV/Show").type == EntryType::directory);
    auto listing = service.filesystem().readdir("/TV/Show");
    CHECK(listing.size() == 2);
    CHECK(read_file(service, "/TV/Show/one.mkv", payload_a.size()) == payload_a);
    CHECK(read_file(service, "/TV/Show/two.mkv", payload_b.size()) == payload_b);

    // Writes prove the commit path against a tree: mkdir, create, overwrite,
    // rename and unlink, each read back.
    service.filesystem().mkdir("/TV/Show/Season 2", 0755, getuid(), getgid());
    const auto payload_c = pattern(256 * 1024, 42);
    write_file(service, "/TV/Show/Season 2/three.mkv", payload_c);
    CHECK(read_file(service, "/TV/Show/Season 2/three.mkv", payload_c.size()) == payload_c);

    const auto payload_d = pattern(1024 * 1024, 99);
    {
        auto writer = service.filesystem().open_write("/TV/Show/two.mkv", true);
        REQUIRE(writer->write(0, payload_d) == payload_d.size());
        writer->commit();
    }
    CHECK(read_file(service, "/TV/Show/two.mkv", payload_d.size()) == payload_d);

    service.filesystem().rename("/TV/Show/one.mkv", "/TV/Show/renamed.mkv", false);
    CHECK(read_file(service, "/TV/Show/renamed.mkv", payload_a.size()) == payload_a);
    bool gone = false;
    try {
        (void)service.filesystem().getattr("/TV/Show/one.mkv");
    } catch (const FsError&) {
        gone = true;
    }
    CHECK(gone);

    service.filesystem().unlink("/TV/Show/Season 2/three.mkv");
    service.filesystem().rmdir("/TV/Show/Season 2");
    service.filesystem().rmdir("/Music");

    auto final_listing = service.filesystem().readdir("/TV/Show");
    CHECK(final_listing.size() == 2);
    service.stop();
}

MACHA_TEST("namespace_migration", test_every_node_computes_the_same_record_from_the_same_head) {
    // The record is a pure function of the head, so nodes migrate without a
    // coordinator; --expect-hash relies on this. Two independent object stores
    // stand in for two stopped nodes.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-agree");
    make_solo(config);
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        service.filesystem().mkdir("/Films", 0755, getuid(), getgid());
        write_file(service, "/Films/a.mkv", pattern(128 * 1024, 5));
        write_file(service, "/Films/b.mkv", pattern(64 * 1024, 9));
        service.stop();
    }

    MetadataRecord head;
    {
        MetadataReplica replica(config.state_path, cluster.keys().storage);
        head = replica.committed();
    }
    REQUIRE(valid_metadata_record(head));

    TempDir left_dir, right_dir;
    LocalStore left_store(left_dir.path() / "objects",
                          LocalStoreOptions{1ULL << 30, 0, 0, 0}, cluster.keys().storage);
    LocalStore right_store(right_dir.path() / "objects",
                           LocalStoreOptions{1ULL << 30, 0, 0, 0}, cluster.keys().storage);
    LocalNamespaceNodeStore left_nodes(left_store);
    LocalNamespaceNodeStore right_nodes(right_store);

    const auto left = plan_namespace_migration(head, left_nodes);
    const auto right = plan_namespace_migration(head, right_nodes);

    CHECK(left.root == right.root);
    CHECK(left.record.hash == right.record.hash);
    CHECK(left.record.payload == right.record.payload);
    // The same tree nodes were written, not merely the same root.
    CHECK(left_nodes.written().size() == right_nodes.written().size());
    CHECK(left_nodes.bytes_written() == right_nodes.bytes_written());
}

MACHA_TEST("namespace_migration", test_a_migration_refuses_what_it_cannot_re_root_safely) {
    TestCluster cluster;
    auto config = cluster.node_config("migrate-refuses");
    make_solo(config);
    NodeId node_id{};
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        service.filesystem().mkdir("/Films", 0755, getuid(), getgid());
        write_file(service, "/Films/a.mkv", pattern(32 * 1024, 3));
        service.stop();
    }

    MetadataReplica replica(config.state_path, cluster.keys().storage);
    const auto head = replica.committed();
    LocalStore objects(config.state_path / "metadata-objects",
                       LocalStoreOptions{config.metadata_store.limit, 0, 0, 0},
                       cluster.keys().storage);
    LocalNamespaceNodeStore nodes(objects);

    // An already-migrated namespace is refused.
    const auto migration = plan_namespace_migration(head, nodes);
    REQUIRE(replica.install_migrated_head(migration.record, {node_id}, "test"));
    const auto migrated_head = replica.committed();
    bool refused = false;
    try {
        (void)plan_namespace_migration(migrated_head, nodes);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);

    // Fewer witnesses than the write floor is refused.
    {
        TestCluster floor_cluster;
        auto floor_config = floor_cluster.node_config("migrate-floor");
        floor_config.replication = 1;
        floor_config.write_copies = 1;
        floor_config.metadata_write_copies = 1;
        NodeId floor_node{};
        {
            Service service(floor_config, floor_cluster.keys(), test_durability_window);
            service.start();
            floor_node = service.node().node_id();
            service.filesystem().mkdir("/Films", 0755, getuid(), getgid());
            service.stop();
        }
        MetadataReplica floor_replica(floor_config.state_path, floor_cluster.keys().storage);
        LocalStore floor_objects(floor_config.state_path / "metadata-objects",
                                 LocalStoreOptions{1ULL << 30, 0, 0, 0},
                                 floor_cluster.keys().storage);
        LocalNamespaceNodeStore floor_nodes(floor_objects);
        const auto plan = plan_namespace_migration(floor_replica.committed(), floor_nodes);
        CHECK(!floor_replica.install_migrated_head(plan.record, {}, "no witnesses"));
        // With the one witness the floor asks for, it installs: the refusal
        // above is about the count alone.
        CHECK(floor_replica.install_migrated_head(plan.record, {floor_node}, "one witness"));
    }

    // A record that fails valid_metadata_record is refused.
    auto tampered = head;
    REQUIRE(!tampered.payload.empty());
    Bytes altered(tampered.payload.begin(), tampered.payload.end());
    altered[altered.size() / 2] ^= 0x20;
    tampered.payload = std::move(altered);
    refused = false;
    try {
        (void)plan_namespace_migration(tampered, nodes);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

MACHA_TEST("namespace_migration", test_the_pre_migration_state_is_kept_not_deleted) {
    // The re-root discards ancestry by renaming the files, not removing them,
    // so a node can recover from its own disk.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-keeps-state");
    make_solo(config);
    NodeId node_id{};
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        service.filesystem().mkdir("/Films", 0755, getuid(), getgid());
        write_file(service, "/Films/a.mkv", pattern(16 * 1024, 7));
        service.stop();
    }

    const auto state = config.state_path / "metadata";
    std::set<std::string> before;
    for (const auto& item : std::filesystem::directory_iterator(state))
        if (item.is_regular_file())
            before.insert(item.path().filename().string());
    REQUIRE(before.contains("checkpoint.meta"));
    REQUIRE(before.contains("history.log"));

    (void)migrate_state(config, cluster.keys(), {node_id});

    // Ancestry files survive under a .pre-migration. name; the live ones are
    // written fresh.
    std::set<std::string> preserved;
    for (const auto& item : std::filesystem::directory_iterator(state)) {
        const auto name = item.path().filename().string();
        const auto marker = name.find(".pre-migration.");
        if (marker != std::string::npos)
            preserved.insert(name.substr(0, marker));
    }
    // Only ancestry is quarantined; other files, such as the mutation sequence
    // clock, carry across untouched.
    for (const auto& name : {"checkpoint.meta", "history.log", "current.meta", "committed.meta"})
        if (before.contains(name))
            CHECK(preserved.contains(name));
    CHECK(std::filesystem::exists(state / "checkpoint.meta"));
    CHECK(std::filesystem::exists(state / "heads.meta"));
}

MACHA_TEST("namespace_migration", test_reconciling_two_tree_backed_branches_keeps_the_namespace) {
    // The three-way merge works on entry maps, which are empty in tree-backed
    // snapshots. The manager materialises the branches, merges, and re-roots;
    // this exercises that sequence without a cluster.
    MemoryNamespaceNodeStore store;

    std::map<std::string, FsEntry> base_entries;
    base_entries["/"] = make_directory(0);
    base_entries["/Films"] = make_directory(1);
    base_entries["/Films/shared.mkv"] = make_file(10, 3);
    auto base = populated_snapshot(base_entries);

    // Each branch adds a file the other has not seen.
    auto left = base;
    left.entries["/Films/left.mkv"] = make_file(20, 4);
    auto right = base;
    right.entries["/Films/right.mkv"] = make_file(30, 5);

    Hash256 left_head{}, right_head{};
    left_head.bytes[0] = 1;
    right_head.bytes[0] = 2;

    const auto expected = merge_metadata_snapshots(base, left, right, left_head, right_head);
    CHECK(expected.snapshot.entries.size() == 5);
    CHECK(expected.conflicts_created == 0);

    // Merging the tree-backed forms directly is refused, not an empty result.
    const auto base_tree = detach_namespace(base, store);
    const auto left_tree = detach_namespace(left, store);
    const auto right_tree = detach_namespace(right, store);
    bool refused = false;
    try {
        (void)merge_metadata_snapshots(base_tree, left_tree, right_tree, left_head, right_head);
    } catch (const std::logic_error&) {
        refused = true;
    }
    CHECK(refused);

    // Materialised, merged, re-rooted: the root equals one built from scratch.
    auto merged = merge_metadata_snapshots(attach_namespace(base_tree, store),
                                           attach_namespace(left_tree, store),
                                           attach_namespace(right_tree, store), left_head,
                                           right_head);
    CHECK(merged.snapshot.entries == expected.snapshot.entries);
    const auto merged_tree = detach_namespace(merged.snapshot, store);
    REQUIRE(merged_tree.namespace_root.has_value());

    MemoryNamespaceNodeStore fresh;
    CHECK(*merged_tree.namespace_root == build_namespace_tree(expected.snapshot.entries, fresh));
    CHECK(read_namespace_tree(*merged_tree.namespace_root, store) == expected.snapshot.entries);
}

MACHA_TEST("namespace_migration", test_a_lagging_node_adopts_the_leaders_record_only_if_it_agrees) {
    // Nodes stopped at different generations compute different records; one
    // may adopt another's record only if its own namespace yields the same root.
    MemoryNamespaceNodeStore leader_store, follower_store, diverged_store;

    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/Films"] = make_directory(1);
    entries["/Films/a.mkv"] = make_file(10, 3);

    // A follower a few catalogue commits behind: same namespace, different
    // catalogue root.
    auto leader = populated_snapshot(entries);
    leader.catalogue_root = fake_object(77);
    auto follower = populated_snapshot(entries);
    follower.catalogue_root = fake_object(78);

    const auto leader_root = detach_namespace(leader, leader_store).namespace_root;
    const auto follower_root = detach_namespace(follower, follower_store).namespace_root;
    REQUIRE(leader_root.has_value());
    REQUIRE(follower_root.has_value());

    CHECK(*leader_root == *follower_root);

    // A diverged namespace computes a different root, so adoption is refused.
    auto diverged_entries = entries;
    diverged_entries["/Films/b.mkv"] = make_file(11, 2);
    auto diverged = populated_snapshot(diverged_entries);
    const auto diverged_root = detach_namespace(diverged, diverged_store).namespace_root;
    REQUIRE(diverged_root.has_value());
    CHECK(*diverged_root != *leader_root);
}

MACHA_TEST("namespace_migration", test_a_namespace_change_is_visible_as_a_change) {
    // Change detection must compare namespace roots, since tree-backed (SM14)
    // snapshots have empty entry maps.
    MemoryNamespaceNodeStore store;
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/Films"] = make_directory(1);
    const auto before = populated_snapshot(entries);

    auto changed_entries = entries;
    changed_entries["/Films/new.mkv"] = make_file(5, 2);
    const auto after = populated_snapshot(changed_entries);

    // Map-backed form.
    CHECK(namespace_differs(before, after));
    CHECK(!namespace_differs(before, before));
    CHECK(metadata_namespace_signature(before) != metadata_namespace_signature(after));

    // Tree-backed form.
    const auto before_tree = detach_namespace(before, store);
    const auto after_tree = detach_namespace(after, store);
    CHECK(namespace_differs(before_tree, after_tree));
    CHECK(!namespace_differs(before_tree, before_tree));
    CHECK(metadata_namespace_signature(before_tree) != metadata_namespace_signature(after_tree));

    // The same namespace built in another store agrees.
    MemoryNamespaceNodeStore elsewhere;
    const auto same_elsewhere = detach_namespace(populated_snapshot(entries), elsewhere);
    CHECK(!namespace_differs(before_tree, same_elsewhere));
    CHECK(metadata_namespace_signature(before_tree) ==
          metadata_namespace_signature(same_elsewhere));

    // Tree-backed and map-backed forms never sign alike, so a cutover is a
    // change.
    CHECK(metadata_namespace_signature(before) != metadata_namespace_signature(before_tree));
}

MACHA_TEST("namespace_migration", test_a_tree_backed_delta_reconstructs_its_record) {
    // The replica keeps a delta body only if replaying it reproduces the record
    // byte for byte, so a tree-backed replay must re-encode successfully.
    MemoryNamespaceNodeStore store;
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/Films"] = make_directory(1);
    entries["/Films/a.mkv"] = make_file(3, 2);
    auto parent = detach_namespace(populated_snapshot(entries), store);

    MetadataDelta delta;
    delta.upsert_entries["/Films/b.mkv"] = make_file(4, 3);

    auto successor = parent;
    apply_metadata_delta_in_place(successor, delta,
                                 [&](const ObjectId& root, const MetadataDelta& d) {
                                     return apply_delta_to_namespace_tree(root, store, d);
                                 });
    REQUIRE(successor.namespace_root.has_value());
    CHECK(*successor.namespace_root != *parent.namespace_root);

    // The delta-versioned encoder is internal, so this asserts its premise: a
    // tree-backed snapshot has an SM14 encoding and encode_snapshot (SM13)
    // throws on it.
    CHECK(!encode_snapshot_v14(successor).empty());
    bool sm13_refused = false;
    try {
        (void)encode_snapshot(successor);
    } catch (const std::exception&) {
        sm13_refused = true;
    }
    CHECK(sm13_refused);

    // A replay produces the payload a commit would.
    auto committed = parent;
    committed.namespace_root = apply_delta_to_namespace_tree(*parent.namespace_root, store, delta);
    CHECK(encode_snapshot_v14(committed) == encode_snapshot_v14(successor));
}


// On a tree, a commit that changes only the catalogue leaves the tree as it
// is and claims the catalogue it installs.
MACHA_TEST("namespace_migration", test_a_catalogue_change_on_a_tree_leaves_the_tree_alone) {
    TestCluster cluster;
    auto config = cluster.node_config("tree-catalogue");
    make_solo(config);
    NodeId node_id{};
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
        write_file(service, "/Movies/film.mkv", pattern(256 * 1024, 71));
        service.stop();
    }
    const auto migration = migrate_state(config, cluster.keys(), {node_id});

    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    (void)service.filesystem();
    REQUIRE(wait_metadata_writable(service));
    CatalogueItem item;
    item.id = "movie:film";
    item.kind = CatalogueKind::movie;
    item.title = "Film";
    (void)service.catalogue().upsert(item);

    const auto after = service.metadata_manager().snapshot();
    REQUIRE(after.namespace_root.has_value());
    CHECK(*after.namespace_root == migration.root);
    REQUIRE(after.catalogue_root.has_value());
    CHECK(service.local_state().retention().retained(RetentionClass::control,
                                                     *after.catalogue_root));
    CHECK(service.filesystem().getattr("/Movies/film.mkv").size == 256 * 1024);
    service.stop();
}

// Claims are not what keeps a namespace: with none on either node, a zero
// grace and the catalogue root moving on, the control collector sweeps what
// nothing reaches and leaves every node of the accepted tree on both nodes.
MACHA_HEAVY_TEST("namespace_migration", test_the_control_collector_keeps_an_unclaimed_namespace) {
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("keep-tree-1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("keep-tree-2", p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 2;
        config->write_copies = 1;
        config->metadata_write_copies = 2;
        config->catalogue.scanner.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
        config->maintenance.garbage_grace = 0ms;
        config->maintenance.foreground_quiet = 200ms;
        config->maintenance.no_progress_backoff = 1000ms;
    }
    const auto film = pattern(256 * 1024, 81);
    const auto converged = [](Service& a, Service& b) {
        return wait_until([&] {
            return a.local_state().replica().committed().hash ==
                       b.local_state().replica().committed().hash &&
                   a.local_state().replica().accepted_heads().size() == 1 &&
                   b.local_state().replica().accepted_heads().size() == 1;
        }, 10s);
    };

    // A library big enough for a tree of several leaves under a branch.
    std::vector<NodeId> witnesses;
    {
        Service s1(c1, cluster.keys(), test_durability_window);
        Service s2(c2, cluster.keys(), test_durability_window);
        s1.start();
        s2.start();
        REQUIRE(wait_metadata_writable(s1));
        witnesses = {s1.node().node_id(), s2.node().node_id()};
        s1.filesystem().mkdir("/Shows", 0755, getuid(), getgid());
        for (int i = 0; i < 80; ++i)
            s1.filesystem().mkdir("/Shows/" + std::to_string(i), 0755, getuid(), getgid());
        write_file(s1, "/Shows/film.mkv", film);
        REQUIRE(converged(s1, s2));
        s2.stop();
        s1.stop();
    }
    migrate_pair(c1, c2, cluster.keys(), witnesses);

    // Started once so whatever a migrated namespace is due is done, then every
    // claim on both nodes is lost.
    {
        Service s1(c1, cluster.keys(), test_durability_window);
        Service s2(c2, cluster.keys(), test_durability_window);
        s1.start();
        s2.start();
        REQUIRE(wait_metadata_writable(s1));
        REQUIRE(converged(s1, s2));
        s2.stop();
        s1.stop();
    }
    std::filesystem::remove_all(c1.state_path / "retention");
    std::filesystem::remove_all(c2.state_path / "retention");

    Service s1(c1, cluster.keys(), test_durability_window);
    Service s2(c2, cluster.keys(), test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_metadata_writable(s1));
    REQUIRE(converged(s1, s2));
    const auto tree = reachable_tree_nodes(s1);
    REQUIRE(tree.size() > 1);
    REQUIRE(reachable_tree_nodes(s2) == tree);
    for (Service* service : {&s1, &s2})
        for (const auto& node : tree) {
            REQUIRE(service->local_state().control().has(node));
            REQUIRE(!service->local_state().retention().retained(RetentionClass::control, node));
        }

    // An orphan on each node: once the collector has swept it, it has swept
    // everything it holds unreferenced.
    std::vector<std::pair<Service*, ObjectId>> orphans;
    for (Service* service : {&s1, &s2}) {
        Bytes orphan = pattern(4096, static_cast<uint8_t>(orphans.size() + 90));
        const auto id = object_id(orphan);
        REQUIRE(service->local_state().control().put(id, orphan));
        orphans.emplace_back(service, id);
    }
    const auto swept = [&] {
        return std::none_of(orphans.begin(), orphans.end(), [](const auto& orphan) {
            return orphan.first->local_state().control().has(orphan.second);
        });
    };
    // The collector deletes only across a catalogue root change, so each
    // round moves the root.
    for (int round = 0; round < 20 && !swept(); ++round) {
        CatalogueItem item;
        item.id = "movie:round-" + std::to_string(round);
        item.kind = CatalogueKind::movie;
        item.title = "Round " + std::to_string(round);
        (void)s1.catalogue().upsert(item);
        (void)wait_until(swept, 5s);
    }
    REQUIRE(swept());

    for (Service* service : {&s1, &s2}) {
        for (const auto& node : tree)
            CHECK(service->local_state().control().has(node));
        CHECK(service->filesystem().readdir("/Shows").size() == 81);
        CHECK(read_file(*service, "/Shows/film.mkv", film.size()) == film);
    }
    s2.stop();
    s1.stop();
}

// A reconciliation's merge commit claims what it introduces before it is
// published: the tree nodes it adds to its primary parent and the extents of
// the conflict alternatives it records, under a dot of its own that its
// clock does not carry and a later head of this node's covers.
MACHA_TEST("namespace_migration", test_a_merge_claims_what_it_introduces) {
    TestCluster cluster;
    auto config = cluster.node_config("merge-claims");
    make_solo(config);
    NodeId node_id{};
    FsEntry first;
    FsEntry second;
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        service.filesystem().mkdir("/Films", 0755, getuid(), getgid());
        for (int i = 0; i < 60; ++i)
            service.filesystem().mkdir("/Films/" + std::to_string(i), 0755, getuid(), getgid());
        write_file(service, "/first.bin", pattern(256 * 1024, 101));
        write_file(service, "/second.bin", pattern(256 * 1024, 102));
        first = service.filesystem().getattr("/first.bin");
        second = service.filesystem().getattr("/second.bin");
        service.filesystem().unlink("/first.bin");
        service.filesystem().unlink("/second.bin");
        service.stop();
    }
    REQUIRE(!first.extents.empty());
    REQUIRE(!second.extents.empty());
    (void)migrate_state(config, cluster.keys(), {node_id});
    // Started once so whatever a migrated namespace is due is done, then
    // every claim is lost.
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        REQUIRE(wait_metadata_writable(service));
        service.stop();
    }
    std::filesystem::remove_all(config.state_path / "retention");

    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    auto& claims = service.local_state().retention();
    for (const auto* entry : {&first, &second})
        for (const auto& extent : entry->extents)
            REQUIRE(!claims.retained(RetentionClass::data, extent.id));

    // Two branches from one head, each adding a directory of its own and a
    // different file at the same path. Each claims the tree nodes it wrote,
    // as its commit would.
    auto& replica = service.local_state().replica();
    const auto base = replica.committed();
    const auto base_snapshot = decode_snapshot(base.payload);
    REQUIRE(base_snapshot.namespace_root.has_value());
    auto nodes = ControlNamespaceNodeStore::for_commit(service.local_state().control(),
                                                       service.filesystem().store());
    const auto make_branch = [&](const std::string& directory, const FsEntry& disputed) {
        // Each branch is another author's one mutation, stamped as a commit
        // stamps what it writes.
        const MetadataDot dot{random_node_id(), 1};
        MetadataDelta delta;
        delta.upsert_entries[directory] = make_directory(7);
        delta.upsert_entries["/disputed.bin"] = disputed;
        for (auto& [path, entry] : delta.upsert_entries)
            stamp_entry_provenance(entry, path, nullptr, dot);
        auto snapshot = base_snapshot;
        snapshot.namespace_root =
            apply_delta_to_namespace_tree(*base_snapshot.namespace_root, nodes, delta);
        snapshot.mutation_sequences[dot.author] = dot.sequence;
        std::vector<ObjectId> written;
        collect_namespace_tree_changes(base_snapshot.namespace_root, *snapshot.namespace_root,
                                       nodes, written);
        claims.retain_batch(RetentionClass::control, written,
                            RetentionDot{dot.author, dot.sequence});

        MetadataRecord branch;
        branch.generation = base.generation + 1;
        branch.previous = base.hash;
        branch.payload = encode_snapshot_v14(snapshot);
        branch.hash = metadata_hash(branch.generation, branch.previous, branch.payload);
        REQUIRE(replica.store_commit(branch));
        MetadataAcceptance acceptance;
        acceptance.generation = branch.generation;
        acceptance.hash = branch.hash;
        acceptance.required = 1;
        acceptance.replicas = {node_id};
        REQUIRE(service.metadata_server().accept_commit(acceptance));
        return std::pair{branch, snapshot};
    };
    const auto left_branch = make_branch("/Films/left", first);
    const auto right_branch = make_branch("/Films/right", second);
    const auto& [left, left_snapshot] = left_branch;
    const auto& right_snapshot = right_branch.second;
    const auto left_generation = left.generation;

    REQUIRE(wait_until(
        [&] {
            try {
                (void)service.metadata_manager().read_record();
                const auto heads = replica.accepted_heads();
                return heads.size() == 1 && heads.front().generation > left_generation;
            } catch (const std::exception&) {
                return false;
            }
        },
        10s));
    const auto head = replica.accepted_heads().front();
    const auto merged = decode_snapshot(head.payload);
    REQUIRE(merged.merge_parents.size() == 1);
    REQUIRE(merged.namespace_root.has_value());
    // A pure join: the clock is the branches', so any reconciler mints it.
    auto joined = left_snapshot.mutation_sequences;
    for (const auto& [author, sequence] : right_snapshot.mutation_sequences)
        joined[author] = std::max(joined[author], sequence);
    CHECK(merged.mutation_sequences == joined);

    const auto& primary = head.previous == left.hash ? left_snapshot : right_snapshot;
    std::vector<ObjectId> introduced;
    collect_namespace_tree_changes(primary.namespace_root, *merged.namespace_root,
                                   service.filesystem().namespace_nodes(), introduced);
    REQUIRE(!introduced.empty());
    // The merge's claim on each: a dot of this node's, beyond the merged
    // head's clock, so a release at this head keeps it.
    const auto uncovered = [&](const ObjectId& node, const std::map<NodeId, uint64_t>& clock) {
        const auto held = claims.claims(RetentionClass::control, node);
        return std::count_if(held.adds.begin(), held.adds.end(), [&](const auto& add) {
            return !clock_covers(clock, MetadataDot{add.first, add.second});
        });
    };
    for (const auto& node : introduced)
        CHECK(uncovered(node, merged.mutation_sequences) == 1);
    REQUIRE(!merged.conflicts.empty());
    for (const auto* entry : {&first, &second})
        for (const auto& extent : entry->extents)
            CHECK(claims.retained(RetentionClass::data, extent.id));

    // This node's next mutation is a head whose clock covers the merge's dot.
    service.filesystem().mkdir("/after-merge", 0755, getuid(), getgid());
    const auto after = service.metadata_manager().snapshot();
    for (const auto& node : introduced)
        CHECK(uncovered(node, after.mutation_sequences) == 0);
    CHECK(service.filesystem().getattr("/Films/left").type == EntryType::directory);
    CHECK(service.filesystem().getattr("/Films/right").type == EntryType::directory);
    service.stop();
}



} // namespace

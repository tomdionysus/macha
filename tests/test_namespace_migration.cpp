// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#include "local_store.hpp"
#include "metadata.hpp"
#include "namespace_control_store.hpp"
#include "namespace_tree.hpp"
#include "service.hpp"

#include <filesystem>
#include <set>
#include <string>

using namespace macha;
using namespace macha::test_support;

namespace {

// Everything macha-namespace-migrate does to one stopped node, with the same
// shared plan_namespace_migration underneath, so this exercises the tool's
// behaviour rather than a second implementation of it.
struct MigrationResult {
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

    // An unnormalised config leaves metadata_store.path empty, which means
    // <state_path>/metadata-objects -- the same resolution normalize_config
    // does, and the same place the daemon will look for these nodes.
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
    return {migration.record.hash, migration.root, migration.entries, migration.previous_payload_bytes,
            migration.record.payload.size()};
}

// A single node is the whole cluster in these tests, so the write floor has to
// say so. The migration itself is indifferent to the floor -- it is offline and
// writes locally -- but the service that writes the library beforehand is not.
void make_solo(Config& config) {
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.min_write_replicas = 1;
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

MACHA_TEST("namespace_migration", test_a_migrated_node_serves_and_writes_its_library) {
    // The cutover, end to end on one node: a library written the ordinary way,
    // the node stopped, the namespace re-rooted onto the tree, the node
    // started again. Afterwards every read must return what it returned
    // before, and the filesystem must still be writable -- a node that comes
    // back able to read and not to write is a node that has to be migrated
    // back under pressure.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-serves");
    make_solo(config);
    const auto payload_a = pattern(3 * 1024 * 1024, 11);
    const auto payload_b = pattern(512 * 1024, 200);

    NodeId node_id{};
    {
        // Destroyed, not merely stopped: a stopped Service still holds the
        // storage lock, and the migration opens the same object store.
        Service service(config, cluster.keys());
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

    // The witness is this node, which is the whole cluster here. On a real
    // cutover the operator names every node.
    const auto migration = migrate_state(config, cluster.keys(), {node_id});
    CHECK(migration.entries == 6); // /, /TV, /TV/Show, two files, /Music
    // The record stopped carrying the library. Small absolute numbers here --
    // this is a six-entry namespace -- but the shape is the claim.
    CHECK(migration.payload_after < migration.payload_before);

    // The head really is tree-backed, with no entry map in it at all.
    {
        MetadataReplica replica(config.state_path, cluster.keys().storage);
        const auto head = replica.committed();
        CHECK(head.hash == migration.hash);
        const auto snapshot = decode_snapshot(head.payload);
        REQUIRE(snapshot.namespace_root.has_value());
        CHECK(*snapshot.namespace_root == migration.root);
        CHECK(snapshot.entries.empty());
    }

    // Same state directory, new Service over it: this is the node coming back
    // after the cutover.
    Service service(config, cluster.keys());
    service.start();
    (void)service.filesystem();

    // Reads first: stat, listing, and the bytes themselves.
    CHECK(service.filesystem().getattr("/TV/Show/one.mkv").size == payload_a.size());
    CHECK(service.filesystem().getattr("/TV/Show").type == EntryType::directory);
    auto listing = service.filesystem().readdir("/TV/Show");
    CHECK(listing.size() == 2);
    CHECK(read_file(service, "/TV/Show/one.mkv", payload_a.size()) == payload_a);
    CHECK(read_file(service, "/TV/Show/two.mkv", payload_b.size()) == payload_b);

    // Then writes, which is the half that proves the commit path works against
    // a tree: a new directory, a new file, an overwrite, a rename and an
    // unlink, each read back.
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

    // And the namespace still reads correctly after all of that, straight from
    // the tree the commits have been rewriting.
    auto final_listing = service.filesystem().readdir("/TV/Show");
    CHECK(final_listing.size() == 2);
    service.stop();
}

MACHA_TEST("namespace_migration", test_every_node_computes_the_same_record_from_the_same_head) {
    // What makes a cluster-wide cutover possible with no coordinator: the
    // record is a pure function of the head. Every node holding the converged
    // head computes the same tree, the same root and the same record hash on
    // its own, so nothing is distributed and there is no half-finished
    // distribution to recover from. It is also what --expect-hash checks.
    //
    // Two independent object stores, the same head, no contact between them --
    // which is exactly the position two stopped nodes are in.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-agree");
    make_solo(config);
    {
        Service service(config, cluster.keys());
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
    // And the same nodes, not merely the same root: every object one wrote,
    // the other wrote too, which is why a node that was down during the
    // cutover can be migrated later and still address what its peers address.
    CHECK(left_nodes.written().size() == right_nodes.written().size());
    CHECK(left_nodes.bytes_written() == right_nodes.bytes_written());
}

MACHA_TEST("namespace_migration", test_a_migration_refuses_what_it_cannot_re_root_safely) {
    // The refusals, because an operator runs this once on a live cluster and
    // every one of them is cheaper than the recovery it prevents.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-refuses");
    make_solo(config);
    NodeId node_id{};
    {
        Service service(config, cluster.keys());
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

    // An already-migrated namespace is refused rather than re-rooted again,
    // which would discard the ancestry of the migration itself.
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

    // Fewer witnesses than the write floor is refused: the certificate would
    // claim a floor it cannot name replicas for, and nothing here may invent
    // an acknowledgement that has not happened.
    {
        TestCluster floor_cluster;
        auto floor_config = floor_cluster.node_config("migrate-floor");
        floor_config.replication = 1;
        floor_config.min_write_replicas = 1;
        floor_config.metadata_min_write_replicas = 1;
        NodeId floor_node{};
        {
            Service service(floor_config, floor_cluster.keys());
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
        // And with the one witness this one-node cluster's floor asks for, it
        // installs -- so the refusal above is about the count, not about
        // anything else being wrong.
        CHECK(floor_replica.install_migrated_head(plan.record, {floor_node}, "one witness"));
    }

    // A record that does not verify is refused. Nothing forges one in
    // practice, so it is forged here: a head whose payload has been altered
    // fails valid_metadata_record, which is the first thing the plan checks.
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
    // The operator's way back. The re-root discards ancestry, but by renaming
    // the files rather than removing them, so a migration that goes wrong on
    // one node is recoverable from that node's own disk rather than from a
    // peer that may have been migrated too.
    TestCluster cluster;
    auto config = cluster.node_config("migrate-keeps-state");
    make_solo(config);
    NodeId node_id{};
    {
        Service service(config, cluster.keys());
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

    // Every file that was there is still there under a .pre-migration. name,
    // and the live ones have been written fresh.
    std::set<std::string> preserved;
    for (const auto& item : std::filesystem::directory_iterator(state)) {
        const auto name = item.path().filename().string();
        const auto marker = name.find(".pre-migration.");
        if (marker != std::string::npos)
            preserved.insert(name.substr(0, marker));
    }
    // The seven files the install quarantines. Anything else in there --
    // the mutation sequence clock, for one -- is not ancestry and legitimately
    // carries across untouched.
    for (const auto& name : {"checkpoint.meta", "history.log", "current.meta", "committed.meta"})
        if (before.contains(name))
            CHECK(preserved.contains(name));
    CHECK(std::filesystem::exists(state / "checkpoint.meta"));
    CHECK(std::filesystem::exists(state / "heads.meta"));
}

MACHA_TEST("namespace_migration", test_reconciling_two_tree_backed_branches_keeps_the_namespace) {
    // The failure this test exists for: the three-way merge is path-wise over
    // three entry maps, and a tree-backed snapshot has an empty one. Merging
    // two empty maps succeeds, reports no conflicts, and produces an empty
    // namespace -- a reconciliation that deletes the library and looks like
    // agreement. This cluster reconciles routinely, so that would have been
    // found in production within a day.
    //
    // What the manager does instead is tested here without a cluster:
    // materialise both branches, merge them as before, and re-root the result.
    MemoryNamespaceNodeStore store;

    std::map<std::string, FsEntry> base_entries;
    base_entries["/"] = make_directory(0);
    base_entries["/Films"] = make_directory(1);
    base_entries["/Films/shared.mkv"] = make_file(10, 3);
    auto base = populated_snapshot(base_entries);

    // Two branches: each adds a file the other has not seen.
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

    // Now the same three branches as trees. A merge over them directly is
    // refused rather than quietly producing nothing.
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

    // Materialised, merged, re-rooted: the same namespace, and a root
    // identical to building the merged namespace from scratch.
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

} // namespace

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

// Every write says where it came from, before and after a restart: a new
// file gets an identity and its two dots, a change
// moves the content dot, a rename keeps the identity and moves the name dot,
// and an append travels as a delta that carries its dot.
MACHA_TEST("namespace_service", test_every_write_records_where_it_came_from) {
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
        check_lifecycle(service, "/first");
        const auto snapshot = service.metadata_manager().snapshot();
        REQUIRE(snapshot.namespace_root.has_value());
        REQUIRE(snapshot.legacy_clock.has_value());
        // The root was founded before any provenance and nothing has changed it.
        CHECK(service.filesystem().getattr("/").provenance.empty());
        service.stop();
    }
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    // What was written before the restart is still there, provenance and all.
    CHECK(service.filesystem().getattr("/first-moved/b.bin").provenance.file_id != NodeId{});
    check_lifecycle(service, "/second");
    service.stop();
}

// The maintenance inventory is, at every step, what a walk of the whole
// namespace finds: files with inline and with external extent lists, the
// same content at two paths, renames, overwrites and removals, before and
// after a restart. It gets there by following the tree from one head to the
// next, and a lookup by media id follows the same changes.
MACHA_TEST("namespace_service", test_the_inventory_and_the_media_index_follow_the_tree) {
    TestCluster cluster;
    auto config = cluster.node_config("census");
    make_solo(config);
    NodeId node_id{};
    const auto check = [&](Service& service, const std::string& root) {
    auto& fs = service.filesystem();
    const auto extent = config.extent_size;
    const auto agrees = [&] {
        const auto followed = fs.maintenance_objects_cached();
        const auto walked = fs.walk_namespace_references();
        const auto extents = followed->referenced_extents.ids();
        const auto nodes = followed->referenced_nodes.ids();
        CHECK(extents == walked.extents);
        CHECK(nodes == walked.nodes);
        // What the release horizon is given in place of its own walk.
        const auto counted = fs.namespace_references(service.metadata_manager().snapshot());
        REQUIRE(counted.has_value());
        CHECK(counted->extents.ids() == walked.extents);
        CHECK(counted->nodes.ids() == walked.nodes);
        return extents == walked.extents && nodes == walked.nodes;
    };
    const auto write = [&](const std::string& path, const Bytes& bytes) {
        try {
            fs.create_file(path, 0644, getuid(), getgid());
        } catch (const FsError&) {
        }
        auto writer = fs.open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    };
    fs.mkdir(root, 0755, getuid(), getgid());
    REQUIRE(agrees());
    const auto small = pattern(extent + extent / 2, 3); // two extents, inline
    const auto large = pattern(extent * 9 + 17, 5);    // ten, a spine
    write(root + "/small.bin", small);
    REQUIRE(agrees());
    write(root + "/large.bin", large);
    REQUIRE(fs.getattr(root + "/large.bin").extents.size() > 8);
    REQUIRE(agrees());
    // The same content at a second path: the same extents and the same spine.
    write(root + "/large-copy.bin", large);
    REQUIRE(agrees());
    const auto large_id = file_media_id(fs.getattr(root + "/large.bin"));
    REQUIRE(fs.find_media(large_id).has_value());
    fs.unlink(root + "/large.bin");
    REQUIRE(agrees());
    // The content is still held at its other path.
    const auto survivor = fs.find_media(large_id);
    REQUIRE(survivor.has_value());
    CHECK(survivor->first == root + "/large-copy.bin");
    const auto still = fs.maintenance_objects_cached();
    for (const auto& extent : fs.getattr(root + "/large-copy.bin").extents)
        CHECK(still->referenced_extents.get(extent.id).has_value());

    for (int i = 0; i < 40; ++i)
        fs.mkdir(root + "/d" + std::to_string(i), 0755, getuid(), getgid());
    REQUIRE(agrees());
    write(root + "/d7/inside.bin", pattern(extent * 9, 9));
    const auto inside_id = file_media_id(fs.getattr(root + "/d7/inside.bin"));
    REQUIRE(fs.find_media(inside_id).has_value());
    fs.rename(root + "/d7", root + "/moved", false);
    REQUIRE(agrees());
    const auto moved = fs.find_media(inside_id);
    REQUIRE(moved.has_value());
    CHECK(moved->first == root + "/moved/inside.bin");
    write(root + "/small.bin", pattern(extent * 10, 11)); // overwritten, now a spine
    REQUIRE(agrees());
    fs.chmod(root + "/large-copy.bin", 0600);
    REQUIRE(agrees());
    fs.unlink(root + "/large-copy.bin");
    fs.unlink(root + "/moved/inside.bin");
    fs.unlink(root + "/small.bin");
    REQUIRE(agrees());
    CHECK(!fs.find_media(large_id).has_value());
    CHECK(!fs.find_media(inside_id).has_value());

    // A mount over the same filesystem lists what the filesystem lists,
    // whatever is changed outside it and mixed with its own operations: on a
    // tree, by applying what differs between one head and the next.
    auto fuse = config.fuse;
    fuse.spool_path = config.state_path / ("fuse-" + root.substr(1));
    fuse.operation_journal_path = *fuse.spool_path / "operations.log";
    auto frontend = make_fuse_frontend(fs, service.resources().memory, fuse);
    const auto names = [](const auto& listing) {
        std::vector<std::string> out;
        for (const auto& item : listing)
            out.push_back(item.first);
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto mount_agrees = [&](const std::string& directory) {
        return names(frontend->readdir(directory)) == names(fs.readdir(directory));
    };
    const auto outside = root + "/outside";
    CHECK(mount_agrees(root)); // the first refresh walks
    fs.mkdir(outside, 0755, getuid(), getgid());
    fs.mkdir(outside + "/show", 0755, getuid(), getgid());
    for (const char* name : {"/show/e1.bin", "/show/e2.bin", "/show.nfo"})
        fs.create_file(outside + name, 0644, getuid(), getgid());
    CHECK(mount_agrees(outside));
    CHECK(mount_agrees(outside + "/show"));
    frontend->mkdir(outside + "/own", 0755, getuid(), getgid());
    REQUIRE(frontend->wait_for_idle(20s));
    fs.rename(outside + "/show", outside + "/renamed", false);
    CHECK(mount_agrees(outside));
    CHECK(mount_agrees(outside + "/renamed"));
    CHECK(frontend->getattr(outside + "/renamed/e2.bin").type == EntryType::file);
    fs.chmod(outside + "/renamed/e1.bin", 0600);
    CHECK((frontend->getattr(outside + "/renamed/e1.bin").mode & 0777U) == 0600U);
    fs.unlink(outside + "/renamed/e1.bin");
    fs.unlink(outside + "/renamed/e2.bin");
    fs.rmdir(outside + "/renamed");
    fs.rmdir(outside + "/own");
    CHECK(mount_agrees(outside));
    CHECK(mount_agrees(root));
    CHECK(mount_agrees("/"));
    frontend->stop();
    };
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        REQUIRE(wait_metadata_writable(service));
        REQUIRE(service.metadata_manager().snapshot().namespace_root.has_value());
        check(service, "/first");
        service.stop();
    }
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    check(service, "/second");
    service.stop();
}

// Tombstones live in batches in the head: a delete writes its extents' batch
// (a control object), the batches survive a restart, maintenance reads every
// tombstone from them, and drops each batch once its grace has run.
MACHA_TEST("namespace_service", test_tombstones_live_in_batches_in_a_tree_backed_head) {
    TestCluster cluster;
    auto config = cluster.node_config("tombstone-batches");
    make_solo(config);
    const auto extent = config.extent_size;
    const auto write = [&](FileSystem& fs, const std::string& path, const Bytes& bytes) {
        fs.create_file(path, 0644, getuid(), getgid());
        auto writer = fs.open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    };
    NodeId node_id{};
    std::vector<ObjectId> map_extents;
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        REQUIRE(wait_metadata_writable(service));
        auto& fs = service.filesystem();
        write(fs, "/before.bin", pattern(extent * 3, 21));
        for (const auto& x : fs.getattr("/before.bin").extents)
            map_extents.push_back(x.id);
        fs.unlink("/before.bin");
        const auto head = service.metadata_manager().snapshot();
        REQUIRE(head.garbage.empty());
        REQUIRE(!head.tombstone_batches.empty());
        service.stop();
    }
    // Longer than the first passes take, so the batches are still there to see.
    config.maintenance.garbage_grace = 20s;
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    auto& fs = service.filesystem();
    auto& metadata = service.metadata_manager();
    REQUIRE(metadata.snapshot().namespace_root.has_value());
    REQUIRE(!metadata.snapshot().tombstone_batches.empty());

    // A delete in the tree writes one batch naming its extents.
    write(fs, "/after.bin", pattern(extent * 4, 22));
    std::vector<ObjectId> tree_extents;
    for (const auto& x : fs.getattr("/after.bin").extents)
        tree_extents.push_back(x.id);
    std::sort(tree_extents.begin(), tree_extents.end());
    tree_extents.erase(std::unique(tree_extents.begin(), tree_extents.end()), tree_extents.end());
    const auto batches_before = metadata.snapshot().tombstone_batches;
    fs.unlink("/after.bin");
    const auto head = metadata.snapshot();
    CHECK(head.garbage.empty());
    std::vector<TombstoneBatch> added;
    std::set_difference(head.tombstone_batches.begin(), head.tombstone_batches.end(),
                        batches_before.begin(), batches_before.end(), std::back_inserter(added));
    REQUIRE(added.size() == 1);
    CHECK(added.front().count == tree_extents.size());
    const auto object = service.local_state().control().get(added.front().id);
    REQUIRE(object.has_value());
    CHECK(decode_tombstone_batch(*object).ids == tree_extents);
    // The head stays small: batches, not tombstones.
    CHECK(encode_snapshot_v14(head).size() < 4096);
    CHECK(decode_snapshot(encode_snapshot_v14(head)).tombstone_batches == head.tombstone_batches);

    // Maintenance sees every tombstone, from the batches.
    REQUIRE(wait_until([&] {
        const auto objects = fs.maintenance_objects_cached();
        const auto has = [&](const ObjectId& id) {
            return std::binary_search(objects->garbage.begin(), objects->garbage.end(),
                                      GarbageRef{id, 0, {}},
                                      [](const GarbageRef& a, const GarbageRef& b) {
                                          return a.id < b.id;
                                      });
        };
        return std::all_of(tree_extents.begin(), tree_extents.end(), has) &&
               std::all_of(map_extents.begin(), map_extents.end(), has);
    }, 20s));

    // Past their grace, maintenance drops the batches whole.
    REQUIRE(wait_until([&] { return metadata.snapshot().tombstone_batches.empty(); }, 60s));
    CHECK(metadata.snapshot().garbage.empty());
    service.stop();
}

// A tombstone made stale by its content being written again lives in a batch
// on a tree-backed head. Maintenance drops the batch, writes the rest of it
// again at its own retirement time, and then has nothing to commit: the
// generation holds still.
MACHA_TEST("namespace_service", test_a_stale_tombstone_in_a_batch_is_erased_once) {
    TestCluster cluster;
    auto config = cluster.node_config("stale-batch");
    make_solo(config);
    const auto extent = config.extent_size;
    const auto write = [&](FileSystem& fs, const std::string& path, const Bytes& bytes) {
        fs.create_file(path, 0644, getuid(), getgid());
        auto writer = fs.open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    };
    // Nothing matures: only staleness erases a tombstone here.
    config.maintenance.garbage_grace = 1h;
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    auto& fs = service.filesystem();
    auto& metadata = service.metadata_manager();
    REQUIRE(metadata.snapshot().namespace_root.has_value());

    // One file, two extents of different content, one delete: one batch
    // naming both.
    auto two = pattern(extent, 31);
    const auto second = pattern(extent, 32);
    two.insert(two.end(), second.begin(), second.end());
    write(fs, "/gone.bin", two);
    const auto extents = fs.getattr("/gone.bin").extents;
    REQUIRE(extents.size() == 2);
    const auto revived = extents[0].id;
    const auto other = extents[1].id;
    REQUIRE(revived != other);
    fs.unlink("/gone.bin");
    const auto batches_before = metadata.snapshot().tombstone_batches;
    REQUIRE(!batches_before.empty());

    // The first extent's content is live again: its tombstone is stale.
    write(fs, "/again.bin", pattern(extent, 31));
    REQUIRE(fs.getattr("/again.bin").extents.front().id == revived);

    const auto tombstoned = [&](const ObjectId& id) {
        const auto objects = fs.maintenance_objects_cached();
        return std::binary_search(objects->garbage.begin(), objects->garbage.end(),
                                  GarbageRef{id, 0, {}},
                                  [](const GarbageRef& a, const GarbageRef& b) {
                                      return a.id < b.id;
                                  });
    };
    REQUIRE(wait_until([&] { return !tombstoned(revived); }, 30s));
    // The rest of the batch was written again, not lost.
    CHECK(tombstoned(other));
    const auto head = metadata.snapshot();
    CHECK(head.garbage.empty());
    CHECK(head.tombstone_batches != batches_before);
    CHECK(encode_snapshot_v14(head).size() < 4096);

    // With the stale tombstone gone the pass has nothing to say: no commit
    // follows another for as long as it would take the defect to make several.
    const auto generation = [&] {
        return service.local_state().replica().committed_generation();
    };
    REQUIRE(wait_until([&] {
        const auto before = generation();
        std::this_thread::sleep_for(1s);
        return generation() == before;
    }, 30s));
    const auto settled = generation();
    std::this_thread::sleep_for(4s);
    CHECK(generation() == settled);
    CHECK(!tombstoned(revived));
    CHECK(tombstoned(other));
    service.stop();
}

// Namespace operations that arrive while a commit is in flight are committed
// together by the next one, before and after a restart. One that cannot apply
// fails alone and leaves the rest of its commit intact.
MACHA_TEST("namespace_service", test_operations_waiting_on_a_commit_share_the_next_one) {
    TestCluster cluster;
    auto config = cluster.node_config("group-commit");
    make_solo(config);
    NodeId node_id{};
    const auto check_group = [&](Service& service, const std::string& root) {
        auto& fs = service.filesystem();
        fs.mkdir(root, 0755, getuid(), getgid());
        constexpr int writers = 24;
        for (int i = 0; i < writers; ++i)
            fs.create_file(root + "/f" + std::to_string(i), 0644, getuid(), getgid());
        const auto before = service.metadata_manager().snapshot_view().generation;

        // A mutation that changes nothing holds the commit path while every
        // unlink arrives; one unlink names a file that is not there.
        TestGate held;
        std::thread holder([&] {
            (void)service.metadata_manager().mutate_delta(
                [&](MetadataSnapshot&, MetadataDelta&) { held.enter_and_wait(); });
        });
        REQUIRE(held.wait_for_entries(1, 10s));
        std::atomic_int started{0}, succeeded{0}, missing{0};
        std::vector<std::thread> threads;
        for (int i = 0; i <= writers; ++i)
            threads.emplace_back([&, i] {
                const auto path = i == writers ? root + "/not-there" : root + "/f" + std::to_string(i);
                FilesystemNamespaceMutation op;
                op.kind = FilesystemNamespaceMutation::Kind::unlink;
                op.from = path;
                ++started;
                try {
                    (void)fs.apply_namespace_batch({&op, 1});
                    ++succeeded;
                } catch (const FsError& error) {
                    if (error.code() == ENOENT)
                        ++missing;
                }
            });
        REQUIRE(wait_until([&] { return started.load() == writers + 1; }, 10s));
        std::this_thread::sleep_for(50ms); // every thread is now queued or blocked
        held.open();
        holder.join();
        for (auto& thread : threads)
            thread.join();

        CHECK(succeeded.load() == writers);
        CHECK(missing.load() == 1);
        CHECK(fs.readdir(root).empty());
        // The first to arrive committed alone; the rest shared one commit.
        const auto after = service.metadata_manager().snapshot_view().generation;
        CHECK(after - before <= 3);
        CHECK(after - before >= 1);
    };
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        node_id = service.node().node_id();
        REQUIRE(wait_metadata_writable(service));
        check_group(service, "/first");
        service.stop();
    }
    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    check_group(service, "/second");
    service.stop();
}

MACHA_TEST("namespace_service", test_reconciling_two_tree_backed_branches_keeps_the_namespace) {
    // Two branches from one head, each another author's file: the tree merge
    // reads what differs between the trees and lands on the namespace both
    // branches imply, the root a build of that namespace from scratch gives.
    MemoryNamespaceNodeStore store;

    std::map<std::string, FsEntry> base_entries;
    base_entries["/"] = make_directory(0);
    base_entries["/Films"] = make_directory(1);
    base_entries["/Films/shared.mkv"] = make_file(10, 3);
    const auto base = populated_snapshot(base_entries);

    auto expected = base_entries;
    // Each branch is another author's one mutation, stamped as a commit
    // stamps what it writes: a file the other has not seen.
    const auto make_branch = [&](uint8_t author, const std::string& path, const FsEntry& file) {
        MetadataDot dot;
        dot.author.bytes[0] = author;
        dot.sequence = 1;
        auto branch = base;
        branch.legacy_clock = base.mutation_sequences;
        branch.mutation_sequences[dot.author] = dot.sequence;
        auto entry = file;
        stamp_entry_provenance(entry, path, nullptr, dot);
        branch.entries[path] = entry;
        expected[path] = entry;
        return detach_namespace(branch, store);
    };
    const auto left_tree = make_branch(1, "/Films/left.mkv", make_file(20, 4));
    const auto right_tree = make_branch(2, "/Films/right.mkv", make_file(30, 5));

    Hash256 left_head{}, right_head{};
    left_head.bytes[0] = 1;
    right_head.bytes[0] = 2;

    const auto tree_merge =
        merge_tree_backed_heads(left_tree, right_tree, left_head, right_head, store);
    CHECK(tree_merge.merged.snapshot.conflicts.empty());
    const auto merged_root = update_namespace_tree(tree_merge.onto, store, tree_merge.changes);
    CHECK(read_namespace_tree(merged_root, store) == expected);
    MemoryNamespaceNodeStore fresh;
    CHECK(merged_root == build_namespace_tree(expected, fresh));
}

MACHA_TEST("namespace_service", test_a_namespace_tree_is_a_function_of_its_entries) {
    // The tree depends on the entries alone: nodes that agree on the namespace
    // build the same root whatever else differs, which is what lets founders
    // agree without a coordinator; a different namespace builds another.
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

    // A different namespace builds a different root.
    auto diverged_entries = entries;
    diverged_entries["/Films/b.mkv"] = make_file(11, 2);
    auto diverged = populated_snapshot(diverged_entries);
    const auto diverged_root = detach_namespace(diverged, diverged_store).namespace_root;
    REQUIRE(diverged_root.has_value());
    CHECK(*diverged_root != *leader_root);
}

MACHA_TEST("namespace_service", test_a_namespace_change_is_visible_as_a_change) {
    // Change detection compares namespace roots: a snapshot carries no entry
    // map.
    MemoryNamespaceNodeStore store;
    std::map<std::string, FsEntry> entries;
    entries["/"] = make_directory(0);
    entries["/Films"] = make_directory(1);
    const auto before = populated_snapshot(entries);

    auto changed_entries = entries;
    changed_entries["/Films/new.mkv"] = make_file(5, 2);
    const auto after = populated_snapshot(changed_entries);

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
}

MACHA_TEST("namespace_service", test_a_tree_backed_delta_reconstructs_its_record) {
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

    CHECK(!encode_snapshot_v14(successor).empty());

    // A replay produces the payload a commit would.
    auto committed = parent;
    committed.namespace_root = apply_delta_to_namespace_tree(*parent.namespace_root, store, delta);
    CHECK(encode_snapshot_v14(committed) == encode_snapshot_v14(successor));
}


// On a tree, a commit that changes only the catalogue leaves the tree as it
// is and claims the catalogue it installs.
MACHA_TEST("namespace_service", test_a_catalogue_change_on_a_tree_leaves_the_tree_alone) {
    TestCluster cluster;
    auto config = cluster.node_config("tree-catalogue");
    make_solo(config);
    std::optional<ObjectId> root;
    {
        Service service(config, cluster.keys(), test_durability_window);
        service.start();
        service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
        write_file(service, "/Movies/film.mkv", pattern(256 * 1024, 71));
        root = service.metadata_manager().snapshot().namespace_root;
        service.stop();
    }
    REQUIRE(root.has_value());

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
    CHECK(*after.namespace_root == *root);
    REQUIRE(after.catalogue_root.has_value());
    CHECK(service.local_state().retention().retained(RetentionClass::control,
                                                     *after.catalogue_root));
    CHECK(service.filesystem().getattr("/Movies/film.mkv").size == 256 * 1024);
    service.stop();
}

// Claims are not what keeps a namespace: with none on either node, a zero
// grace and the catalogue root moving on, the control collector sweeps what
// nothing reaches and leaves every node of the accepted tree on both nodes.
MACHA_HEAVY_TEST("namespace_service", test_the_control_collector_keeps_an_unclaimed_namespace) {
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
    {
        Service s1(c1, cluster.keys(), test_durability_window);
        Service s2(c2, cluster.keys(), test_durability_window);
        s1.start();
        s2.start();
        REQUIRE(wait_metadata_writable(s1));
        s1.filesystem().mkdir("/Shows", 0755, getuid(), getgid());
        for (int i = 0; i < 80; ++i)
            s1.filesystem().mkdir("/Shows/" + std::to_string(i), 0755, getuid(), getgid());
        write_file(s1, "/Shows/film.mkv", film);
        REQUIRE(converged(s1, s2));
        s2.stop();
        s1.stop();
    }
    // Started again so whatever the namespace is due is done, then every claim
    // on both nodes is lost.
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
MACHA_TEST("namespace_service", test_a_merge_claims_what_it_introduces) {
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
    // Started again so whatever the namespace is due is done, then every claim
    // is lost.
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
    // The merge that joined the two branches is found in the head's ancestry.
    // It need not be the head: bringing tombstoned extents back makes
    // maintenance erase the stale tombstones in a commit of its own, which a
    // mutation makes on this node's head without waiting for a merge, so it
    // may lie under the merge, over it, or beside it and be merged in turn.
    std::shared_ptr<const MetadataMaterialization> merge;
    std::shared_ptr<const MetadataMaterialization> primary_parent;
    std::vector<ObjectId> introduced;
    {
        std::vector<Hash256> pending{replica.accepted_heads().front().hash};
        std::set<Hash256> visited;
        while (!pending.empty() && !merge) {
            const auto hash = pending.back();
            pending.pop_back();
            if (!visited.insert(hash).second)
                continue;
            const auto at = replica.materialized(hash);
            if (!at || at->record.generation <= left_generation)
                continue;
            pending.push_back(at->record.previous);
            for (const auto& parent : at->snapshot->merge_parents)
                pending.push_back(parent);
            if (at->snapshot->merge_parents.empty())
                continue;
            const auto parent = replica.materialized(at->record.previous);
            if (!parent || !parent->snapshot->namespace_root)
                continue;
            std::vector<ObjectId> nodes;
            collect_namespace_tree_changes(parent->snapshot->namespace_root,
                                           *at->snapshot->namespace_root,
                                           service.filesystem().namespace_nodes(), nodes);
            // A merge with the tombstone commit changes no entry.
            if (nodes.empty())
                continue;
            merge = at;
            primary_parent = parent;
            introduced = std::move(nodes);
        }
    }
    REQUIRE(merge != nullptr);
    const auto& merged = *merge->snapshot;
    REQUIRE(merged.merge_parents.size() == 1);
    REQUIRE(merged.namespace_root.has_value());
    const auto other_parent = replica.materialized(merged.merge_parents.front());
    REQUIRE(other_parent != nullptr);
    // A pure join: the clock is the parents', so any reconciler mints it.
    auto joined = primary_parent->snapshot->mutation_sequences;
    for (const auto& [author, sequence] : other_parent->snapshot->mutation_sequences)
        joined[author] = std::max(joined[author], sequence);
    CHECK(merged.mutation_sequences == joined);
    for (const auto* branch : {&left_snapshot, &right_snapshot})
        for (const auto& [author, sequence] : branch->mutation_sequences)
            CHECK(clock_covers(merged.mutation_sequences, MetadataDot{author, sequence}));

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

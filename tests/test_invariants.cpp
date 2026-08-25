// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

#if defined(__linux__)
#include <sys/syscall.h>
#endif

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {
Config config_for(const std::filesystem::path& path, const std::filesystem::path& key,
                  uint16_t port, std::vector<Endpoint> bootstrap = {}) {
    return macha::test_support::config_for(path, key, port, std::move(bootstrap),
                                           macha::test_support::ConfigProfile::isolated);
}

#if defined(__linux__)
std::atomic_bool track_fsync{false};
std::atomic_uint64_t fsync_calls{0};
#endif

MACHA_TEST("invariants", test_fuse_open_inode_identity_survives_external_replace_and_unlink) {
    TestNode fixture("node");
    auto& config = fixture.config();
    fixture.start();
    auto& fs = fixture.filesystem();

    const auto old_bytes = pattern(4096, 3);
    const auto new_bytes = pattern(4096, 4);
    write_file(fs, "/replace.bin", old_bytes);
    write_file(fs, "/unlink.bin", old_bytes);

    FuseFrontend frontend(fs, config.fuse);
    const auto replaced_handle = frontend.open("/replace.bin", true, false, false, false);
    const auto unlinked_handle = frontend.open("/unlink.bin", true, false, false, false);

    // Stand in for another node publishing a new namespace generation.
    fs.rename("/replace.bin", "/old-replace.bin");
    write_file(fs, "/replace.bin", new_bytes);
    fs.unlink("/unlink.bin");

    // Force adoption of the already-decoded newer namespace.
    REQUIRE(frontend.inode_for_path("/replace.bin").has_value());
    const auto stale_name = frontend.inode_for_path("/unlink.bin");

    // The descriptor opened before replacement still denotes the original file.
    CHECK(fuse_read(frontend, replaced_handle.inode, old_bytes.size()) == old_bytes);
    // POSIX unlink removes the pathname immediately even though the open inode lives on.
    CHECK(!stale_name.has_value());
    CHECK(fuse_read(frontend, unlinked_handle.inode, old_bytes.size()) == old_bytes);

    frontend.stop();
}

MACHA_TEST("invariants", test_dirty_open_inode_never_writes_remote_replacement) {
    TestNode fixture("node");
    auto& config = fixture.config();
    fixture.start();
    auto& fs = fixture.filesystem();

    const auto original = pattern(4096, 21);
    const auto replacement = pattern(4096, 22);
    const auto dirty = pattern(2048, 23);
    write_file(fs, "/victim.bin", original);
    write_file(fs, "/incoming.bin", replacement);

    FuseFrontend frontend(fs, config.fuse);
    const auto old = frontend.open("/victim.bin", true, true, false, false);
    REQUIRE(frontend.write(old.inode, 0, dirty) == dirty.size());

    // Stand in for a second node atomically renaming a different inode over the
    // dirty pathname. The old open descriptor remains valid locally, but it no
    // longer owns /victim.bin and must never publish through that name.
    fs.rename("/incoming.bin", "/victim.bin");
    const auto current = frontend.inode_for_path("/victim.bin");
    REQUIRE(current.has_value());
    CHECK(*current != old.inode);
    CHECK(fuse_read(frontend, old.inode, dirty.size()) == dirty);

    frontend.release(old.inode, true);
    REQUIRE(frontend.wait_for_idle(5s));

    auto reader = fs.open_read("/victim.bin");
    Bytes actual(replacement.size());
    size_t offset = 0;
    while (offset < actual.size()) {
        const auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
        REQUIRE(n > 0);
        offset += n;
    }
    CHECK(actual == replacement);

    frontend.stop();
}

MACHA_TEST("invariants", test_failed_catalogue_commit_never_deletes_live_filesystem_object) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.node();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    auto& fs = fixture.filesystem();
    CatalogueManager catalogue(node, store, metadata);
    fixture.start();

    const auto live_bytes = pattern(8192, 5);
    write_file(fs, "/live.bin", live_bytes);
    const auto live_entry = fs.getattr("/live.bin");
    REQUIRE(live_entry.extents.size() == 1);
    const auto live_id = live_entry.extents.front().id;
    REQUIRE(node.local_store().get(live_id).has_value());

    CatalogueItem item;
    item.id = "test:movie:failed-stage";
    item.kind = CatalogueKind::movie;
    item.title = "Failed stage must not delete live data";
    item.artwork.push_back({"poster", live_id, "application/octet-stream"});
    auto missing_bytes = pattern(7777, 6);
    const auto missing_id = object_id(missing_bytes);
    REQUIRE(missing_id != live_id);
    item.artwork.push_back({"backdrop", missing_id, "application/octet-stream"});

    bool failed = false;
    try {
        (void)catalogue.upsert(std::move(item));
    } catch (...) {
        failed = true;
    }
    REQUIRE(failed);

    // Catalogue rollback must never directly erase a hash that is still
    // reachable from namespace metadata; orphan collection belongs to GC.
    bool live_object_readable = false;
    try {
        auto data = node.local_store().get(live_id);
        live_object_readable = data && *data == live_bytes;
    } catch (...) {
        live_object_readable = false;
    }
    CHECK(live_object_readable);

}

MACHA_TEST("invariants", test_scanner_prune_is_fenced_to_scanned_namespace) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.node();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    auto& fs = fixture.filesystem();
    CatalogueManager catalogue(node, store, metadata);
    fixture.start();

    const auto old_bytes = pattern(4096, 31);
    const auto new_bytes = pattern(4096, 32);
    write_file(fs, "/media.bin", old_bytes);
    const auto old_entry = fs.getattr("/media.bin");
    const auto scanned = fs.local_snapshot_view();
    const auto scanned_signature = metadata_namespace_signature(*scanned.snapshot);
    const std::set<std::string> stale_active{file_media_id(old_entry)};

    fs.unlink("/media.bin");
    write_file(fs, "/media.bin", new_bytes);
    const auto new_entry = fs.getattr("/media.bin");
    const auto new_media_id = file_media_id(new_entry);
    REQUIRE(new_media_id != file_media_id(old_entry));

    CatalogueItem item;
    item.id = "test:movie:namespace-fence";
    item.kind = CatalogueKind::movie;
    item.title = "Namespace fence";
    item.external_ids["macha_scanner"] = "1";
    item.media_ids = {new_media_id};
    (void)catalogue.upsert(item);

    bool conflicted = false;
    try {
        catalogue.reconcile_scanner({}, stale_active, true, scanned_signature);
    } catch (const CatalogueConflict&) {
        conflicted = true;
    }
    CHECK(conflicted);
    auto after = catalogue.get(item.id);
    REQUIRE(after.has_value());
    CHECK(after->media_ids == std::vector<std::string>{new_media_id});

}

MACHA_FAST_TEST("invariants", test_scanner_does_not_prune_from_mixed_namespace_generations) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.node();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    auto& fs = fixture.filesystem();
    CatalogueManager catalogue(node, store, metadata);
    fixture.start();

    FsEntry dir;
    dir.type = EntryType::directory;
    dir.mode = 0755;
    dir.uid = getuid();
    dir.gid = getgid();
    dir.ctime_ns = dir.mtime_ns = wall_time_ns();
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.uid = getuid();
    file.gid = getgid();
    file.size = 1234;
    file.ctime_ns = file.mtime_ns = wall_time_ns();
    file.extents.push_back({0, file.size, object_id(pattern(1234, 7)), false});

    metadata.mutate([&](MetadataSnapshot& snapshot) {
        snapshot.entries["/Movies"] = dir;
        snapshot.entries["/Movies/A"] = dir;
        snapshot.entries["/Movies/B"] = dir;
        snapshot.entries["/Movies/A/live.mkv"] = file;
    });

    CatalogueItem item;
    item.id = "test:movie:mixed-generation";
    item.kind = CatalogueKind::movie;
    item.title = "Still live";
    item.external_ids["macha_scanner"] = "1";
    item.media_ids = {file_media_id(file)};
    (void)catalogue.upsert(item);

    // Capture the exact immutable generation a complete scanner pass enumerates.
    // The production scanner calls catalogue_snapshot_files() with this snapshot
    // and derives its destructive-reconciliation signature from the same object.
    const auto scanned = fs.local_snapshot_view();
    const auto scanned_signature = metadata_namespace_signature(*scanned.snapshot);
    REQUIRE(scanned.snapshot->entries.contains("/Movies/A/live.mkv"));
    CHECK(!scanned.snapshot->entries.contains("/Movies/B/live.mkv"));

    const auto discovered = catalogue_snapshot_files("/Movies", *scanned.snapshot);
    REQUIRE(discovered.size() == 1);
    CHECK(discovered.front().first == "/Movies/A/live.mkv");
    CHECK(file_media_id(discovered.front().second) == file_media_id(file));

    std::set<std::string> active;
    for (const auto& [_, entry] : discovered)
        active.insert(file_media_id(entry));

    // Reproduce the historical failure deterministically: the immutable object
    // moves after the scan snapshot has been captured. A snapshot-pinned scan
    // still sees that object's media id, so complete reconciliation must not
    // infer absence merely because the live path now belongs to a later
    // namespace generation.
    fs.rename("/Movies/A/live.mkv", "/Movies/B/live.mkv");
    const auto moved = fs.local_snapshot_view();
    CHECK(!moved.snapshot->entries.contains("/Movies/A/live.mkv"));
    REQUIRE(moved.snapshot->entries.contains("/Movies/B/live.mkv"));
    CHECK(metadata_namespace_signature(*moved.snapshot) != scanned_signature);

    // No catalogue mutation is required here: the immutable media id is still
    // active in the captured generation. In particular, a namespace conflict is
    // not itself required when reconciliation is a no-op.
    catalogue.reconcile_scanner({}, active, true, scanned_signature);
    auto after_move = catalogue.get(item.id);
    REQUIRE(after_move.has_value());
    CHECK(after_move->media_ids == std::vector<std::string>{file_media_id(file)});

    // Now make the stale scan genuinely destructive relative to current state.
    // A new immutable object replaces the moved file and becomes the catalogue
    // binding. Reusing the old scan's active set would prune that live binding,
    // so the old namespace signature must fence the commit.
    const Bytes replacement = pattern(1234, 19);
    fs.unlink("/Movies/B/live.mkv");
    write_file(fs, "/Movies/B/live.mkv", replacement);
    const auto replacement_entry = fs.getattr("/Movies/B/live.mkv");
    const auto replacement_media_id = file_media_id(replacement_entry);
    REQUIRE(replacement_media_id != file_media_id(file));

    auto current_item = catalogue.get(item.id);
    REQUIRE(current_item.has_value());
    current_item->media_ids = {replacement_media_id};
    (void)catalogue.upsert(*current_item, current_item->revision);

    bool conflicted = false;
    try {
        catalogue.reconcile_scanner({}, active, true, scanned_signature);
    } catch (const CatalogueConflict&) {
        conflicted = true;
    }
    CHECK(conflicted);
    auto after_replace = catalogue.get(item.id);
    REQUIRE(after_replace.has_value());
    CHECK(after_replace->media_ids == std::vector<std::string>{replacement_media_id});
}

MACHA_TEST("invariants", test_replica_repair_does_not_count_corrupt_remote_as_healthy) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.metadata_replication = c2.metadata_replication = 1;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    DistributedStore distributed(n1);
    const auto bytes = pattern(128 * 1024, 9);
    const auto id = object_id(bytes);
    REQUIRE(distributed.put(id, bytes));
    REQUIRE(wait_until([&] { return n2.local_store().has(id); }));
    corrupt_object(c2.storage_backends.front().path, id);

    std::vector<ObjectId> live{id};
    std::vector<ObjectId> universal{id};
    for (int i = 0; i < 4; ++i)
        distributed.repair_once(8ULL * 1024 * 1024, &live, &universal);

    bool repaired = false;
    try {
        auto data = n2.local_store().get(id);
        repaired = data && *data == bytes;
    } catch (...) {
        repaired = false;
    }
    CHECK(repaired);

    n2.stop();
    n1.stop();
}

MACHA_TEST("invariants", test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto a = t.path() / "a";
    const auto b = t.path() / "b";
    std::filesystem::create_directories(a);
    std::filesystem::create_directories(b);
    const auto bytes = pattern(96 * 1024, 10);
    const auto id = object_id(bytes);

    // Seed a valid copy on both physical stores before handing them to the pool.
    {
        LocalStore sa(a, 64ULL * 1024 * 1024, keys.storage);
        LocalStore sb(b, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return sa.scan_complete() && sb.scan_complete(); }));
        REQUIRE(sa.put(id, bytes));
        REQUIRE(sb.put(id, bytes));
    }

    StoragePool pool(t.path() / "pool-state", random_node_id(),
                     {{a, 64ULL * 1024 * 1024}, {b, 64ULL * 1024 * 1024}}, keys.storage);
    REQUIRE(wait_until([&] { return pool.online_backends() == 2; }));

    // pool.put() reaffirms/touches only the deterministic preferred backend.
    const auto a_before = std::filesystem::last_write_time(object_path(a, id));
    std::this_thread::sleep_for(20ms);
    REQUIRE(pool.put(id, bytes));
    const auto a_after = std::filesystem::last_write_time(object_path(a, id));
    const auto preferred = a_after != a_before ? a : b;
    const auto secondary = preferred == a ? b : a;
    REQUIRE(std::filesystem::exists(object_path(secondary, id)));

    corrupt_object(preferred, id);
    for (int i = 0; i < 8; ++i) {
        auto result = pool.rebalance_step(8ULL * 1024 * 1024, 8);
        if (result.complete) break;
    }

    // Rebalance may converge back to one physical copy, but it must not remove
    // the last previously-valid copy until the repaired preferred replica has
    // itself passed strong validation.
    CHECK(std::filesystem::exists(object_path(secondary, id)) || pool.valid(id));
    bool readable = false;
    try {
        auto data = pool.get(id);
        readable = data && *data == bytes;
    } catch (...) {
        readable = false;
    }
    CHECK(readable);
}

MACHA_TEST("invariants", test_rpc_pre_auth_admission_is_bounded) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto port = free_port();
    NodeInfo server_info{random_node_id(), "127.0.0.1", "test", port};
    RpcServer server("127.0.0.1", port, keys, server_info,
                     [](const NodeInfo&, FrameType, const RpcMessage&) {
                         return RpcMessage{MessageType::ok, {}};
                     },
                     [](const NodeInfo&) {});
    server.start();

    const auto thread_count = [] {
        size_t count = 0;
        for (const auto& ignored : std::filesystem::directory_iterator("/proc/self/task")) {
            (void)ignored;
            ++count;
        }
        return count;
    };
    const auto before = thread_count();
    std::vector<int> sockets;
    for (int i = 0; i < 32; ++i) sockets.push_back(connect_idle(port));
    std::this_thread::sleep_for(100ms);
    const auto after = thread_count();

    // Unauthenticated sockets must consume bounded resources; one blocking
    // jthread per pre-auth peer is the failure this regression exposes.
    CHECK(after <= before + 8);

    for (auto fd : sockets) close(fd);
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] pre-auth admission check requires /proc/self/task; skipped\n";
#endif
}

MACHA_TEST("invariants", test_control_plane_hint_admission_is_storage_durable) {
#if defined(__linux__)
    TempDir t;
    CatalogueHintQueue hints(t.path() / "state");
    fsync_calls = 0;
    track_fsync = true;
    (void)hints.submit("/Movies/Durable.mkv", "manual", "manual:1",
                       CatalogueHintPriority::manual_rescan);
    track_fsync = false;

    // Crash-safe atomic replacement requires durability of the temp file and
    // publication of the rename in its parent directory before submit returns.
    CHECK(fsync_calls.load() >= 2);
#else
    std::cout << "[ARCH-REGRESSION] fsync interception check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_authenticated_receiver_enforces_transport_lane) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto port = free_port();
    NodeInfo server_info{random_node_id(), "127.0.0.1", "server", port};
    std::atomic_bool object_dispatched{false};
    RpcServer server("127.0.0.1", port, keys, server_info,
                     [&](const NodeInfo&, FrameType, const RpcMessage& message) {
                         if (message.type == MessageType::get_object)
                             object_dispatched = true;
                         return RpcMessage{MessageType::ok, {}};
                     },
                     [](const NodeInfo&) {});
    server.start();

    int fd = connect_idle(port);
    NodeInfo client_info{random_node_id(), "127.0.0.1", "client", free_port()};
    SecureChannel channel(fd, keys, client_info, 64 * 1024);
    (void)channel.client_handshake(TransportLane::control);
    channel.send_fragment(1, FrameType::foreground, MessageType::get_object, true, true, {});
    std::this_thread::sleep_for(100ms);

    // Object traffic on a negotiated CONTROL channel must be rejected before
    // dispatch; sender-side lane selection alone is not protocol enforcement.
    CHECK(!object_dispatched.load());

    channel.shutdown();
    server.stop();
}

MACHA_TEST("invariants", test_http_slow_client_cannot_pin_worker_indefinitely) {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = free_port();
    config.workers = 1;
    config.max_queued_connections = 4;
    config.client_io_timeout = 100ms;

    HttpServer server(config, [](const HttpRequest&) {
        return http_json(200, "{\"ok\":true}");
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() == config.port; }, 2s));

    const int slow = connect_idle(config.port);
    const std::string partial = "GET /slow HTTP/1.1\r\nHost: localhost\r\n";
    REQUIRE(::send(slow, partial.data(), partial.size(), 0) ==
            static_cast<ssize_t>(partial.size()));

    // Queue a complete request behind the sole worker. It must run after the
    // incomplete client exceeds its bounded socket-I/O occupancy.
    const int fast = connect_idle(config.port);
    const std::string request = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";
    REQUIRE(::send(fast, request.data(), request.size(), 0) ==
            static_cast<ssize_t>(request.size()));
    timeval timeout{1, 0};
    REQUIRE(setsockopt(fast, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    std::string response;
    char buffer[1024];
    while (true) {
        const auto n = ::recv(fast, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<size_t>(n));
    }
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);

    ::close(slow);
    ::close(fast);
    server.stop();
}

MACHA_TEST("invariants", test_catalogue_gc_liveness_fails_closed_when_current_root_unavailable) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.node();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    CatalogueManager catalogue(node, store, metadata);
    fixture.start();
    catalogue.repair_once(); // establish a coherent empty cached catalogue

    const auto missing_root = object_id(pattern(32123, 11));
    REQUIRE(!node.local_store().has(missing_root));
    metadata.mutate([&](MetadataSnapshot& snapshot) { snapshot.catalogue_root = missing_root; });

    const auto maintenance = catalogue.maintenance_objects();
    // Even when the current immutable catalogue cannot yet be fetched/decoded,
    // its metadata-referenced root is unconditionally live and GC must fail closed.
    CHECK(maintenance.live.contains(missing_root));
    CHECK(!maintenance.complete);

}

} // namespace

#if defined(__linux__)
extern "C" int fsync(int fd) {
    if (track_fsync.load(std::memory_order_relaxed))
        fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif

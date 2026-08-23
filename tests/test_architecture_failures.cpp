// SPDX-License-Identifier: GPL-3.0-or-later
// Regression tests for architecture invariants repaired after the 0.14.8
// architecture audit. Keep these separate from broad functional coverage so a
// durability/identity regression is visible as a named failure.

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "cluster.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "distributed_store.hpp"
#include "filesystem.hpp"
#include "fuse_frontend.hpp"
#include "http.hpp"
#include "local_store.hpp"
#include "media_catalogue.hpp"
#include "metadata_manager.hpp"
#include "net.hpp"
#include "storage_pool.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

using namespace macha;
using namespace std::chrono_literals;

namespace {

std::atomic_int failures{0};

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr "\n";           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) throw std::runtime_error(std::string("REQUIRE failed: ") + #expr);           \
    } while (0)

class TempDir {
    std::filesystem::path path_;
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("macha-architecture-regression-" + std::to_string(getpid()) + "-" +
                 std::to_string(unix_ms()) + "-" + std::to_string(random_node_id().bytes[0]));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
};

template <typename Fn>
void run_case(std::string_view name, Fn&& fn) {
    const auto before = failures.load();
    std::cout << "[ARCH-REGRESSION] START " << name << '\n' << std::flush;
    try {
        fn();
    } catch (const std::exception& e) {
        std::cerr << "[ARCH-REGRESSION] " << name << " exception=\"" << e.what() << "\"\n";
        ++failures;
    } catch (...) {
        std::cerr << "[ARCH-REGRESSION] " << name << " exception=unknown\n";
        ++failures;
    }
    const auto delta = failures.load() - before;
    std::cout << "[ARCH-REGRESSION] END " << name << " status="
              << (delta ? "FAIL" : "PASS") << " failures=" << delta << '\n' << std::flush;
}

#define RUN_CASE(fn) run_case(#fn, [] { fn(); })

uint16_t free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t size = sizeof(addr);
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size) == 0);
    const auto port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

void write_key(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "architecture regression test cluster key\n";
}

Config config_for(const std::filesystem::path& path, const std::filesystem::path& key,
                  uint16_t port, std::vector<Endpoint> bootstrap = {}) {
    std::filesystem::create_directories(path);
    Config c;
    c.state_path = path.parent_path() / (path.filename().string() + ".state");
    c.storage_backends = {{path, 512ULL * 1024 * 1024}};
    c.key_file = key;
    c.listen_host = "127.0.0.1";
    c.advertise_host = "127.0.0.1";
    c.port = port;
    c.replication = 1;
    c.metadata_replication = 1;
    c.extent_size = 1024 * 1024;
    c.hydration.enabled = false;
    c.catalogue.scanner.enabled = false;
    c.catalogue.api.enabled = false;
    c.ingest.enabled = false;
    c.torrent.enabled = false;
    c.heartbeat = 50ms;
    c.dead_after = 500ms;
    c.connect_timeout = 500ms;
    c.bootstrap = std::move(bootstrap);
    return c;
}

template <typename Fn>
bool wait_until(Fn&& fn, std::chrono::milliseconds timeout = 5s) {
    const auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        if (fn()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return fn();
}

Bytes pattern(size_t n, uint8_t salt = 0) {
    Bytes out(n);
    uint64_t x = 0x123456789abcdef0ULL ^ salt;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<uint8_t>(x);
    }
    return out;
}

std::filesystem::path object_path(const std::filesystem::path& root, const ObjectId& id) {
    auto name = to_string(id);
    return root / "objects" / name.substr(0, 2) / name.substr(2, 2) / (name + ".obj");
}

void corrupt_object(const std::filesystem::path& root, const ObjectId& id) {
    auto path = object_path(root, id);
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.good());
    file.seekg(-1, std::ios::end);
    char byte{};
    file.read(&byte, 1);
    REQUIRE(file.good());
    byte ^= 0x5a;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
    file.flush();
    REQUIRE(file.good());
}

void write_file(FileSystem& fs, const std::string& path, std::span<const uint8_t> bytes) {
    try {
        fs.create_file(path, 0644, getuid(), getgid());
    } catch (const FsError& e) {
        if (e.code() != EEXIST) throw;
    }
    auto writer = fs.open_write(path, true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
}

Bytes fuse_read(FuseFrontend& frontend, uint64_t inode, size_t size) {
    Bytes out(size);
    const auto got = frontend.read(inode, 0, out);
    out.resize(got);
    return out;
}

int connect_idle(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}


#if defined(__linux__)
std::atomic_bool track_fsync{false};
std::atomic_uint64_t fsync_calls{0};
#endif

void test_fuse_open_inode_identity_survives_external_replace_and_unlink() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    FileSystem fs(node, store, metadata);
    node.start();

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
    node.stop();
}

void test_dirty_open_inode_never_writes_remote_replacement() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    FileSystem fs(node, store, metadata);
    node.start();

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
    node.stop();
}

void test_failed_catalogue_commit_never_deletes_live_filesystem_object() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);
    FileSystem fs(node, store, metadata);
    node.start();

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

    node.stop();
}


void test_scanner_prune_is_fenced_to_scanned_namespace() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);
    FileSystem fs(node, store, metadata);
    node.start();

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

    node.stop();
}

#if defined(MACHA_AUDIT_WITH_MEDIA_CATALOGUE)
void test_scanner_does_not_prune_from_mixed_namespace_generations() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);
    FileSystem fs(node, store, metadata);
    CatalogueHintQueue hints(config.state_path);
    node.start();

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

    // Build a broad B subtree in one metadata transaction. walk() visits /Movies/B
    // before /Movies/A (LIFO over sorted children). Once B has been enumerated,
    // these empty children keep the traversal busy long enough to move A/live.mkv
    // into the already-enumerated B root. The mixed-generation walk then sees the
    // file in neither location even though it exists in the final generation.
    metadata.mutate([&](MetadataSnapshot& snapshot) {
        snapshot.entries["/Movies"] = dir;
        snapshot.entries["/Movies/A"] = dir;
        snapshot.entries["/Movies/B"] = dir;
        for (int i = 0; i < 50000; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "/Movies/B/d%05d", i);
            snapshot.entries[name] = dir;
        }
        snapshot.entries["/Movies/A/live.mkv"] = file;
    });

    CatalogueItem item;
    item.id = "test:movie:mixed-generation";
    item.kind = CatalogueKind::movie;
    item.title = "Still live";
    item.external_ids["macha_scanner"] = "1";
    item.media_ids = {file_media_id(file)};
    (void)catalogue.upsert(item);

    CatalogueScannerConfig scanner_config;
    scanner_config.enabled = true;
    scanner_config.movies.roots = {"/Movies"};
    scanner_config.tv.enabled = false;
    scanner_config.music.enabled = false;
    scanner_config.max_provider_requests_per_scan = 0;
    CatalogueScanner scanner(node, fs, catalogue, hints, scanner_config);

    // Warm the namespace index so each attempt reaches /Movies/B immediately.
    (void)fs.readdir("/Movies");

    // There is intentionally no scanner barrier at the generation boundary in
    // 0.14.8. Sweep a small set of delays so the test catches the interval after
    // B has been enumerated but before A is enumerated without depending on one
    // host scheduler's exact thread-start latency. A correct snapshot-pinned scan
    // is safe at every one of these timings.
    bool observed_live_prune = false;
    for (auto delay : {1ms, 2ms, 4ms, 8ms, 16ms, 32ms}) {
        std::atomic_bool started{false};
        std::thread scanning([&] {
            started = true;
            (void)scanner.scan_once();
        });
        REQUIRE(wait_until([&] { return started.load(); }, 1s));
        std::this_thread::sleep_for(delay);
        fs.rename("/Movies/A/live.mkv", "/Movies/B/live.mkv");
        scanning.join();

        REQUIRE(fs.getattr("/Movies/B/live.mkv").size == file.size);
        if (!catalogue.get(item.id).has_value()) {
            observed_live_prune = true;
            break;
        }
        fs.rename("/Movies/B/live.mkv", "/Movies/A/live.mkv");
    }

    // A complete scan may prune only against one coherent namespace generation.
    CHECK(!observed_live_prune);

    node.stop();
}
#endif

void test_local_store_put_repairs_corrupt_existing_object_before_ack() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "store";
    std::filesystem::create_directories(root);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto bytes = pattern(64 * 1024, 8);
    const auto id = object_id(bytes);
    REQUIRE(store.put(id, bytes));
    corrupt_object(root, id);

    // A successful PUT acknowledgement means the named replica contains the
    // requested immutable bytes, not merely that a pathname already existed.
    REQUIRE(store.put(id, bytes));
    bool readable = false;
    try {
        auto recovered = store.get(id);
        readable = recovered && *recovered == bytes;
    } catch (...) {
        readable = false;
    }
    CHECK(readable);
}

void test_replica_repair_does_not_count_corrupt_remote_as_healthy() {
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

void test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy() {
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

void test_rpc_pre_auth_admission_is_bounded() {
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

void test_control_plane_hint_admission_is_storage_durable() {
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

void test_authenticated_receiver_enforces_transport_lane() {
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


void test_http_slow_client_cannot_pin_worker_indefinitely() {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = free_port();
    config.workers = 1;
    config.max_queued_connections = 4;
    config.client_io_timeout = 1s;

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
    timeval timeout{3, 0};
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

void test_catalogue_gc_liveness_fails_closed_when_current_root_unavailable() {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);
    node.start();
    catalogue.repair_once(); // establish a coherent empty cached catalogue

    const auto missing_root = object_id(pattern(32123, 11));
    REQUIRE(!node.local_store().has(missing_root));
    metadata.mutate([&](MetadataSnapshot& snapshot) { snapshot.catalogue_root = missing_root; });

    const auto maintenance = catalogue.maintenance_objects();
    // Even when the current immutable catalogue cannot yet be fetched/decoded,
    // its metadata-referenced root is unconditionally live and GC must fail closed.
    CHECK(maintenance.live.contains(missing_root));
    CHECK(!maintenance.complete);

    node.stop();
}

} // namespace

#if defined(__linux__)
extern "C" int fsync(int fd) {
    if (track_fsync.load(std::memory_order_relaxed))
        fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif

int main() {
    RUN_CASE(test_fuse_open_inode_identity_survives_external_replace_and_unlink);
    RUN_CASE(test_dirty_open_inode_never_writes_remote_replacement);
    RUN_CASE(test_failed_catalogue_commit_never_deletes_live_filesystem_object);
    RUN_CASE(test_scanner_prune_is_fenced_to_scanned_namespace);
#if defined(MACHA_AUDIT_WITH_MEDIA_CATALOGUE)
    RUN_CASE(test_scanner_does_not_prune_from_mixed_namespace_generations);
#endif
    RUN_CASE(test_local_store_put_repairs_corrupt_existing_object_before_ack);
    RUN_CASE(test_replica_repair_does_not_count_corrupt_remote_as_healthy);
    RUN_CASE(test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy);
    RUN_CASE(test_rpc_pre_auth_admission_is_bounded);
    RUN_CASE(test_control_plane_hint_admission_is_storage_durable);
    RUN_CASE(test_authenticated_receiver_enforces_transport_lane);
    RUN_CASE(test_http_slow_client_cannot_pin_worker_indefinitely);
    RUN_CASE(test_catalogue_gc_liveness_fails_closed_when_current_root_unavailable);

    const auto count = failures.load();
    if (count) {
        std::cerr << count << " architecture regression assertion(s) failed\n";
        return 1;
    }
    std::cout << "All architecture regression tests passed\n";
    return 0;
}

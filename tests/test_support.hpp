// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "cluster/call_deadline.hpp"
#include "cluster/distributed_store.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "errno_text.hpp"
#include "filesystem/filesystem.hpp"
#include "fuse/fuse_frontend.hpp"
#include "ledger/retention_ledger.hpp"
#include "metadata/metadata_manager.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "service/service.hpp"
#include "supervised.hpp"
#include "test_framework.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace macha::test_support {

#if defined(__linux__)
// The test binary's fsync (test_invariants.cpp) holds a call on a file whose
// path contains `path` while armed, so a test can show who waits on a sync.
struct FsyncHold {
    std::mutex mutex;
    std::condition_variable changed;
    std::string path;
    bool armed{};
    bool holding{};
};
inline FsyncHold fsync_hold;
#endif

// The catalogue's half of a maintenance inventory: the head, then the read.
// With no installer behind these fixtures, the view is brought to the head
// first, as the installer would have.
inline CatalogueMaintenance maintenance_inventory(CatalogueManager& catalogue) {
    const auto head = catalogue.maintenance_head();
    try {
        catalogue.follow_head();
    } catch (...) {
    }
    return catalogue.maintenance_objects(head);
}

// One node store behind every record a test builds, so a record's root can be
// read back. Locked: a test's replica may apply a delta on another thread.
class TestNamespaceNodes final : public NamespaceNodeStore {
  public:
    ObjectId put(std::span<const uint8_t> node) override {
        std::lock_guard lock(mutex_);
        return nodes_.put(node);
    }
    std::optional<Bytes> get(const ObjectId& id) const override {
        std::lock_guard lock(mutex_);
        return nodes_.get(id);
    }

  private:
    mutable std::mutex mutex_;
    MemoryNamespaceNodeStore nodes_;
};

inline TestNamespaceNodes& test_namespace_nodes() {
    static TestNamespaceNodes nodes;
    return nodes;
}

inline NamespaceDeltaApplier test_namespace_applier() {
    return [](const ObjectId& root, const MetadataDelta& delta) {
        return apply_delta_to_namespace_tree(root, test_namespace_nodes(), delta);
    };
}

// A replica that replays delta bodies against the test store.
class TestReplica : public MetadataReplica {
  public:
    TestReplica(std::filesystem::path path, std::array<uint8_t, 32> key,
                std::optional<MetadataRecord> seed = {}, bool founder = true,
                uint64_t cache_bytes = 128ULL * 1024 * 1024)
        : MetadataReplica(std::move(path), key, std::move(seed), founder, cache_bytes,
                          test_namespace_applier()) {}
};

// The working form of a freshly founded namespace.
inline MetadataSnapshot founded_snapshot() {
    MetadataSnapshot snapshot;
    FsEntry root;
    root.type = EntryType::directory;
    root.mode = 0755;
    root.ctime_ns = root.mtime_ns = 1;
    snapshot.entries["/"] = root;
    snapshot.metadata_write_replicas_required = 1;
    return snapshot;
}

// A record's payload for a working snapshot: its namespace goes into the test
// store. A snapshot with no write floor gets the lowest there is.
inline Bytes tree_payload(MetadataSnapshot snapshot) {
    if (!snapshot.metadata_write_replicas_required)
        snapshot.metadata_write_replicas_required = 1;
    if (!snapshot.namespace_root)
        snapshot = detach_namespace(std::move(snapshot), test_namespace_nodes());
    return encode_snapshot_v14(snapshot);
}

// A record's snapshot in working form, for a test to change and encode again.
inline MetadataSnapshot map_snapshot(std::span<const uint8_t> payload) {
    const auto& genesis = genesis_metadata().payload;
    if (std::equal(payload.begin(), payload.end(), genesis.begin(), genesis.end()))
        return founded_snapshot();
    auto snapshot = decode_snapshot(payload);
    if (snapshot.namespace_root && test_namespace_nodes().get(*snapshot.namespace_root))
        return attach_namespace(std::move(snapshot), test_namespace_nodes());
    return snapshot;
}

using namespace std::chrono_literals;

// The durability batch window the suite's nodes run with: a barrier at once,
// so no publication waits for company. Tests of batching name their own.
inline constexpr std::chrono::milliseconds test_durability_window{0};

enum class ConfigProfile {
    functional,
    isolated,
};

class TempDir {
    std::filesystem::path path_;

  public:
    TempDir() {
        static std::atomic_uint64_t sequence{};
        const auto base = std::filesystem::temp_directory_path();
        path_ = base / ("macha-test-" + std::to_string(getpid()) + "-" +
                        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        // The name is pid-based and a case killed on timeout leaves its
        // directory behind, so clear any leftover: a fresh fixture starts empty.
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const {
        return path_;
    }
};

// The runner's per-run salt, inherited by every case it runs; set
// MACHA_TEST_PORT_SALT to reproduce a run's exact ports and keys.
inline uint64_t runner_salt() {
    static const uint64_t salt = [] {
        const char* value = std::getenv("MACHA_TEST_PORT_SALT");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    return salt;
}

// The factor the runner scales case deadlines by (MACHA_TEST_TIMEOUT_SCALE),
// for a slower build: a bound inside a case scales with it.
inline unsigned time_scale() {
    static const unsigned scale = [] {
        const char* value = std::getenv("MACHA_TEST_TIMEOUT_SCALE");
        const auto parsed = value ? std::strtoul(value, nullptr, 10) : 1UL;
        return static_cast<unsigned>(parsed ? parsed : 1UL);
    }();
    return scale;
}

template <class Rep, class Period>
std::chrono::duration<Rep, Period> scaled(std::chrono::duration<Rep, Period> bound) {
    return bound * time_scale();
}

inline uint16_t free_port() {
    // Each case gets its own 32-port block, so no other test can claim a probed
    // port before the server binds it. Candidates are still bind-probed because
    // unrelated host processes may hold ports in the range. The blocks stay
    // below Linux's ephemeral range (32768 up), where an outbound connection
    // of a case running beside this one could take a probed port before its
    // node binds it.
    static uint16_t within_case{};
    constexpr uint16_t first = 10000;
    constexpr uint16_t block = 32;
    constexpr uint16_t blocks = 711; // 10000..32751
    // The salt shifts which cases of two runners on one machine land on the
    // same block; it cannot keep them apart, there being more cases than
    // blocks. What keeps their nodes apart is the key (write_key).
    const auto case_block =
        static_cast<uint16_t>((macha::test::case_index() + runner_salt()) % blocks);

    int last_bind_error = 0;
    for (uint16_t attempt = 0; attempt < block; ++attempt) {
        const auto slot = static_cast<uint16_t>((within_case + attempt) % block);
        const auto candidate = static_cast<uint16_t>(first + case_block * block + slot);
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        int reuse = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(candidate);
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            close(fd);
            within_case = static_cast<uint16_t>(slot + 1);
            return candidate;
        }
        last_bind_error = errno;
        close(fd);
    }
    if (last_bind_error != EADDRINUSE)
        throw std::runtime_error("cannot probe loopback test port: " +
                                 std::string(errno_text(last_bind_error)));
    throw std::runtime_error("no free port remains in this test case's port namespace");
}

inline void write_key(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    // One key per runner and case: a node of another run on this machine
    // that reaches one of this case's ports fails the handshake instead of
    // joining its cluster.
    out << "macha test cluster key " << runner_salt() << ' ' << macha::test::case_index() << '\n';
    REQUIRE(out.good());
}

inline Config config_for(const std::filesystem::path& path, const std::filesystem::path& key,
                         uint16_t port, std::vector<Endpoint> bootstrap = {},
                         ConfigProfile profile = ConfigProfile::functional) {
    // Production never creates a missing backend path (it could be an unmounted
    // disk), so tests create it.
    std::filesystem::create_directories(path);

    Config c;
    c.state_path = path.parent_path() / (path.filename().string() + ".state");
    c.storage_backends = {{path, 512ULL * 1024 * 1024}};
    c.key_file = key;
    c.listen_host = "127.0.0.1";
    c.advertise_host = "127.0.0.1";
    c.port = port;
    c.extent_size = 1024 * 1024;
    // Test state lives in the temporary directory, often a tmpfs no larger than
    // the production reserve; capacity tests set explicit limits instead.
    c.fuse.spool_reserve_free = 0;
    // The production retry policy's shape with short delays.
    c.fuse.publication_retry =
        RetryPolicy{100, std::chrono::minutes(30), std::chrono::milliseconds(20),
                    std::chrono::milliseconds(200)};
    c.fuse.namespace_retry =
        RetryPolicy{200, std::chrono::minutes(30), std::chrono::milliseconds(20),
                    std::chrono::milliseconds(200)};
    c.hydration.enabled = false;
    c.dead_after = 500ms;
    c.connect_timeout = 500ms;
    c.bootstrap = std::move(bootstrap);
    // No plugins: loading them is costly in every test process and would test
    // whatever is installed rather than this build. Tests that need one point
    // plugin_path at MACHA_TEST_PLUGIN_DIR. The path must be engaged but empty:
    // normalize_config() fills an unset plugin_path with the installed directory.
    c.plugin_path = std::filesystem::path{};

    if (profile == ConfigProfile::functional) {
        c.replication = 3;
        c.metadata_write_copies = 2;
        c.read_ahead_extents = 2;
        c.heartbeat = 100ms;
        c.control_stall_notice = 2s;
        c.data_stall_notice = 5s;
    } else {
        c.replication = 1;
        c.metadata_write_copies = 1;
        c.catalogue.scanner.enabled = false;
        c.catalogue.api.enabled = false;
        c.ingest.enabled = false;
        c.torrent.enabled = false;
        c.heartbeat = 50ms;
    }
    return c;
}

class TestCluster {
  public:
    explicit TestCluster(ConfigProfile profile = ConfigProfile::functional)
        : keyfile_(root_.path() / "cluster.key"), profile_(profile) {
        write_key(keyfile_);
        keys_ = load_cluster_keys(keyfile_);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return root_.path();
    }
    [[nodiscard]] const std::filesystem::path& keyfile() const {
        return keyfile_;
    }
    [[nodiscard]] const ClusterKeys& keys() const {
        return keys_;
    }

    [[nodiscard]] Config node_config(std::string_view name, uint16_t port = 0,
                                     std::vector<Endpoint> bootstrap = {}) const {
        if (!port)
            port = free_port();
        return config_for(root_.path() / std::string(name), keyfile_, port, std::move(bootstrap),
                          profile_);
    }

  private:
    TempDir root_;
    std::filesystem::path keyfile_;
    ClusterKeys keys_{};
    ConfigProfile profile_;
};

class TestService {
    TempDir temp_;
    std::filesystem::path keyfile_;
    ClusterKeys keys_;
    Config config_;
    std::unique_ptr<Service> service_;

  public:
    explicit TestService(std::string_view name, ConfigProfile profile = ConfigProfile::functional)
        : keyfile_(temp_.path() / "cluster.key") {
        write_key(keyfile_);
        keys_ = load_cluster_keys(keyfile_);
        config_ = config_for(temp_.path() / std::string(name), keyfile_, free_port(), {}, profile);
    }

    ~TestService() {
        if (!service_)
            return;
        try {
            service_->stop();
        } catch (...) {
            // Destructors must not mask the test result; tests that assert
            // shutdown behaviour call stop() themselves.
        }
    }

    TestService(const TestService&) = delete;
    TestService& operator=(const TestService&) = delete;

    const std::filesystem::path& path() const noexcept {
        return temp_.path();
    }
    const std::filesystem::path& keyfile() const noexcept {
        return keyfile_;
    }
    const ClusterKeys& keys() const noexcept {
        return keys_;
    }
    Config& config() noexcept {
        return config_;
    }
    const Config& config() const noexcept {
        return config_;
    }

    Service& start() {
        REQUIRE(!service_);
        service_ = std::make_unique<Service>(config_, keys_, test_durability_window);
        service_->start();
        // Return a fully ready service; lifecycle tests that observe the early
        // phases construct Service directly.
        (void)service_->filesystem();
        return *service_;
    }

    Service& service() {
        REQUIRE(service_ != nullptr);
        return *service_;
    }
};

// `count` nodes active, each with its storage capacity known: a peer's figures reach
// this node in the exchange after it joins, and placement weighs by them.
inline bool capacities_known(const std::vector<NodeInfo>& active, size_t count) {
    return active.size() == count &&
           std::all_of(active.begin(), active.end(),
                       [](const NodeInfo& node) { return node.capacity > 0; });
}

// The root parts a NodeRuntime takes, built before it (a base is built first).
struct BareNodeResources {
    StorageLock state_lock;
    NodeIdentity identity;
    RecoveryProgress progress;
    MessageRoutes routes;
    LocalState::StageHook stage_hook;
    NodeBooks books;
    BareNodeResources(const Config& config, ClusterKeys keys, LocalState::StageHook hook)
        : state_lock(config.state_path), identity(config.state_path, std::move(keys)),
          stage_hook(std::move(hook)), books(config) {}
};

// A NodeRuntime with the root parts it takes, owned as Service owns them: the
// lock, identity and books first, then the node, then the resources that charge
// its ledger; start() brings the control plane online and recovers local state
// on its own thread; stop() stops the resources, the node and that recovery, in
// that order. The store accessors are the tests' convenience; each waits,
// bounded, for local state to recover.
class BareNode : public BareNodeResources, public NodeRuntime {
  public:
    NodeResources resources;

    BareNode(Config config, ClusterKeys keys, NodeRuntime::StartupStageHook hook = {},
             std::chrono::milliseconds durability_batch_window = test_durability_window)
        : BareNodeResources(config, keys, hook),
          NodeRuntime(std::move(config), identity, progress, books.transcode_rates, routes,
                      books.events, std::move(hook)),
          resources(NodeRuntime::config(), books, node().memory()),
          durability_batch_window_(durability_batch_window) {}
    ~BareNode() {
        stop();
    }
    BareNode(const BareNode&) = delete;
    BareNode& operator=(const BareNode&) = delete;
    void start() {
        NodeRuntime::start();
        accounts_.start();
        if (recovery_.joinable())
            return;
        recovery_ = std::jthread([this](std::stop_token stop) {
            run_supervised_once("cluster-state-recovery", [this, stop] {
                try {
                    local_ = std::make_unique<LocalServices>(config(), identity, progress,
                                                             stage_hook, stop, *this, resources,
                                                             routes, durability_batch_window_);
                } catch (const std::exception&) {
                    // Cancelled, or recorded in `progress`.
                    return;
                }
                progress.mark_complete(unix_ms());
                signal_telemetry_refresh();
            });
        });
    }
    void request_stop() {
        resources.stop();
        accounts_.stop();
        NodeRuntime::request_stop();
        if (recovery_.joinable())
            recovery_.request_stop();
    }
    void stop() {
        resources.stop();
        accounts_.stop();
        NodeRuntime::stop();
        if (recovery_.joinable()) {
            recovery_.request_stop();
            recovery_.join();
        }
    }

    Accounts& accounts() {
        return accounts_;
    }
    UserStore& users() {
        return accounts_.users();
    }
    SessionManager& sessions() {
        return accounts_.sessions();
    }
    bool apply_session(const AuthSession& session) {
        return accounts_.apply_session(session);
    }
    void propagate_session(const AuthSession& session) {
        accounts_.propagate_session(session);
    }
    bool apply_user(const UserRecord& user) {
        return accounts_.apply_user(user);
    }
    void propagate_users() {
        accounts_.propagate_users();
    }
    LocalState& local_state() {
        return local("local state").state();
    }
    MetadataServer& metadata_server() {
        return local("metadata replica").metadata();
    }
    StoragePool& local_store() {
        return local("data storage").state().data();
    }
    LocalStore& control_store() {
        return local("control storage").state().control();
    }
    PersistentBlockCache& block_cache() {
        return local("persistent cache").state().cache();
    }
    ClaimStore& claims() {
        return local("retention state").state().retention();
    }
    // The ledger over this node's stores, built on first use.
    ObjectLedger& ledger() {
        auto& state = local("the ledger's stores").state();
        std::lock_guard lock(ledger_mutex_);
        if (!ledger_)
            ledger_ =
                std::make_unique<RetentionLedger>(state.retention(), state.data(), state.control());
        return *ledger_;
    }
    MetadataReplica& metadata_replica() {
        return local("metadata replica").state().replica();
    }
    uint64_t known_metadata_generation() {
        return progress.complete() ? local_->metadata().known_generation()
                                   : remote_metadata_generation();
    }

  private:
    // Waits, bounded, for recovery: a test reaching for the stores means "once
    // they exist", and recovery runs on its own thread.
    LocalServices& local(std::string_view what) {
        if (!progress.wait_complete(std::chrono::seconds(10)))
            throw std::runtime_error(std::string(what) + " is still recovering");
        return *local_;
    }

    std::chrono::milliseconds durability_batch_window_;
    Accounts accounts_{config(), identity, *this, resources.events, routes};
    // Declared in this order so the recovery thread goes before what it built.
    std::unique_ptr<LocalServices> local_;
    std::jthread recovery_;
    std::mutex ledger_mutex_;
    std::unique_ptr<RetentionLedger> ledger_;
};

// A DistributedStore over a bare node's own parts, built as Service builds
// one: what a MetadataManager the test constructs keeps its namespace tree in.
inline std::unique_ptr<DistributedStore> distributed_store_for(BareNode& node) {
    return std::make_unique<DistributedStore>(node, node.local_state(), node.resources.activity,
                                              node.resources.data, node.resources.memory,
                                              node.resources.events);
}

// `snapshot` with `delta` applied to its namespace tree, the new nodes written
// to `control`: a head built outside any manager, such as a
// sibling or another author's history.
inline MetadataSnapshot with_namespace_delta(MetadataSnapshot snapshot, LocalStore& control,
                                             DistributedStore& store, const MetadataDelta& delta) {
    auto nodes = ControlNamespaceNodeStore::for_commit(control, store);
    snapshot.namespace_root = apply_delta_to_namespace_tree(*snapshot.namespace_root, nodes, delta);
    return snapshot;
}

// Whether `snapshot`'s namespace holds `path`, read through `control`.
inline bool namespace_holds(const MetadataSnapshot& snapshot, LocalStore& control,
                            DistributedStore& store, std::string_view path) {
    const auto nodes = ControlNamespaceNodeStore::for_reading(control, store);
    return namespace_entry(snapshot, &nodes, path).has_value();
}

class TestNode {
    TempDir temp_;
    std::filesystem::path keyfile_;
    ClusterKeys keys_;
    Config config_;
    std::unique_ptr<BareNode> node_;
    std::unique_ptr<DistributedStore> store_;
    std::function<void(const MetadataPublicationContext&)> publication_guard_;
    std::function<MetadataPublicationClaims(const MetadataPublicationContext&)> publication_claims_;
    std::function<void(const NodeId&, uint64_t, const MetadataPublicationClaims&)> peer_retention_;
    std::unique_ptr<MetadataManager> metadata_;
    std::unique_ptr<FileSystem> filesystem_;
    bool started_{};

  public:
    explicit TestNode(std::string_view name, ConfigProfile profile = ConfigProfile::isolated)
        : keyfile_(temp_.path() / "cluster.key") {
        write_key(keyfile_);
        keys_ = load_cluster_keys(keyfile_);
        config_ = config_for(temp_.path() / std::string(name), keyfile_, free_port(), {}, profile);
    }

    ~TestNode() {
        if (!started_ || !node_)
            return;
        try {
            node_->stop();
        } catch (...) {
        }
    }

    TestNode(const TestNode&) = delete;
    TestNode& operator=(const TestNode&) = delete;

    const std::filesystem::path& path() const noexcept {
        return temp_.path();
    }
    const std::filesystem::path& keyfile() const noexcept {
        return keyfile_;
    }
    const ClusterKeys& keys() const noexcept {
        return keys_;
    }
    Config& config() noexcept {
        return config_;
    }
    const Config& config() const noexcept {
        return config_;
    }

    void prepare() {
        REQUIRE(!node_);
        node_ = std::make_unique<BareNode>(config_, keys_);
    }

    BareNode& start_control_plane() {
        if (!node_)
            prepare();
        REQUIRE(!started_);
        node_->start();
        started_ = true;
        return *node_;
    }

    // Runs before each of this node's metadata commits is published, as
    // Service's claims barrier does. Not synchronised: set it while no commit runs.
    void set_publication_guard(std::function<void(const MetadataPublicationContext&)> guard) {
        publication_guard_ = std::move(guard);
    }
    // What each commit's claims name (none unless set), and the peers' share
    // the replicator makes of them. Set before start.
    void set_publication_claims(
        std::function<MetadataPublicationClaims(const MetadataPublicationContext&)> claims) {
        publication_claims_ = std::move(claims);
    }
    void set_peer_retention(
        std::function<void(const NodeId&, uint64_t, const MetadataPublicationClaims&)> peers) {
        peer_retention_ = std::move(peers);
    }

    BareNode& wait_ready() {
        REQUIRE(node_);
        REQUIRE(started_);
        REQUIRE(node_->wait_local_state_ready(std::chrono::seconds{10}));
        if (!store_) {
            store_ = std::make_unique<DistributedStore>(
                *node_, node_->local_state(), node_->resources.activity, node_->resources.data,
                node_->resources.memory, node_->resources.events);
            metadata_ = std::make_unique<MetadataManager>(
                *node_, node_->local_state(), node_->metadata_server(), *store_,
                MetadataPublicationRetention{[this](const MetadataPublicationContext& context) {
                                                 if (publication_guard_)
                                                     publication_guard_(context);
                                                 return publication_claims_
                                                            ? publication_claims_(context)
                                                            : MetadataPublicationClaims{};
                                             },
                                             [this](const NodeId& origin, uint64_t sequence,
                                                    const MetadataPublicationClaims& claims) {
                                                 if (peer_retention_)
                                                     peer_retention_(origin, sequence, claims);
                                             }});
            filesystem_ = std::make_unique<FileSystem>(
                node_->config(), node_->node_id(), node_->membership(), node_->local_state(),
                node_->metadata_server(), *store_, *metadata_, node_->resources.memory);
        }
        return *node_;
    }

    BareNode& start() {
        start_control_plane();
        return wait_ready();
    }

    BareNode& node() {
        REQUIRE(node_);
        return *node_;
    }
    NodeResources& resources() {
        REQUIRE(node_);
        return node_->resources;
    }
    DistributedStore& store() {
        REQUIRE(store_);
        return *store_;
    }
    MetadataManager& metadata() {
        REQUIRE(metadata_);
        return *metadata_;
    }
    FileSystem& filesystem() {
        REQUIRE(filesystem_);
        return *filesystem_;
    }
};

// A first write on a forming cluster can find the replica set still forming
// (a peer not yet answering its survey); product callers retry
// MetadataNotReady, and so does this, until `write` succeeds or `limit`.
template <class Write>
bool retry_while_not_ready(Write&& write,
                           std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    return wait_until(
        [&] {
            try {
                write();
                return true;
            } catch (const MetadataNotReady&) {
                return false;
            }
        },
        limit);
}

class TestGate {
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    size_t entered_{};
    bool open_{};

  public:
    void enter_and_wait() {
        std::unique_lock lock(mutex_);
        ++entered_;
        cv_.notify_all();
        cv_.wait(lock, [&] { return open_; });
    }

    bool wait_for_entries(size_t count,
                          std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return entered_ >= count; });
    }

    size_t entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    void open() {
        std::lock_guard lock(mutex_);
        open_ = true;
        cv_.notify_all();
    }
};

template <class Fn>
bool wait_until(Fn&& fn, std::chrono::milliseconds timeout = 5s,
                std::chrono::milliseconds poll_interval = 10ms) {
    const auto end = Clock::now() + scaled(timeout);
    while (Clock::now() < end) {
        if (fn())
            return true;
        std::this_thread::sleep_for(poll_interval);
    }
    return fn();
}

// Membership convergence is not write readiness: a node that sees every peer
// may still be forming its metadata replica set or lack its durability floor,
// and refuse mutations. Wait for MetadataClusterStatus::write_available instead.
inline bool wait_metadata_writable(Service& service, std::chrono::milliseconds timeout = 10s) {
    return wait_until([&] { return service.metadata_manager().cluster_status().write_available; },
                      timeout);
}

inline Bytes pattern(size_t n, uint8_t salt = 0) {
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

inline std::filesystem::path object_path(const std::filesystem::path& root, const ObjectId& id) {
    const auto name = to_string(id);
    return root / "objects" / name.substr(0, 2) / name.substr(2, 2) / (name + ".obj");
}

inline void corrupt_object(const std::filesystem::path& root, const ObjectId& id) {
    const auto path = object_path(root, id);
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

inline void write_file(FileSystem& fs, const std::string& path, std::span<const uint8_t> bytes) {
    try {
        fs.create_file(path, 0644, getuid(), getgid());
    } catch (const FsError& e) {
        if (e.code() != EEXIST)
            throw;
    }
    auto writer = fs.open_write(path, true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
}

inline Bytes fuse_read(FuseFrontend& frontend, uint64_t inode, size_t size) {
    Bytes out(size);
    const auto got = frontend.read(inode, 0, out);
    out.resize(got);
    return out;
}

inline int connect_idle(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

} // namespace macha::test_support

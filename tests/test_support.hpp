// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "cluster/distributed_store.hpp"
#include "filesystem/filesystem.hpp"
#include "ledger/retention_ledger.hpp"
#include "fuse/fuse_frontend.hpp"
#include "metadata/metadata_manager.hpp"
#include "service/service.hpp"
#include "supervised.hpp"
#include "test_framework.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace macha::test_support {

// The catalogue's half of a maintenance inventory, taken as the pass takes it:
// head, repair, then read.
inline CatalogueMaintenance maintenance_inventory(CatalogueManager& catalogue) {
    const auto head = catalogue.maintenance_head();
    const bool repaired = catalogue.maintenance_repair();
    return catalogue.maintenance_objects(head, repaired);
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

    const std::filesystem::path& path() const { return path_; }
};

inline uint16_t free_port() {
    // Each case gets its own 64-port block, so no other test can claim a probed
    // port before the server binds it. Candidates are still bind-probed because
    // unrelated host processes may hold ports in the range.
    static uint16_t within_case{};
    constexpr uint16_t first = 20000;
    constexpr uint16_t block = 64;
    constexpr uint16_t blocks = 600; // 20000..58399
    // The runner's per-run salt keeps two runners on one machine out of each
    // other's blocks; otherwise they would share ports and cluster key, and merge.
    static const uint64_t runner_salt = [] {
        const char* value = std::getenv("MACHA_TEST_PORT_SALT");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    const auto case_block =
        static_cast<uint16_t>((macha::test::case_index() + runner_salt) % blocks);

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
                                 std::string(std::strerror(last_bind_error)));
    throw std::runtime_error("no free port remains in this test case's port namespace");
}

inline void write_key(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << "macha deterministic test cluster key\n";
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
    c.fuse.publication_retry = RetryPolicy{100, std::chrono::minutes(30),
                                           std::chrono::milliseconds(20),
                                           std::chrono::milliseconds(200)};
    c.fuse.namespace_retry = RetryPolicy{200, std::chrono::minutes(30),
                                         std::chrono::milliseconds(20),
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
        c.metadata_min_write_replicas = 2;
        c.read_ahead_extents = 2;
        c.heartbeat = 100ms;
        c.control_stall_notice = 2s;
        c.data_stall_notice = 5s;
    } else {
        c.replication = 1;
        c.metadata_min_write_replicas = 1;
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

    [[nodiscard]] const std::filesystem::path& path() const { return root_.path(); }
    [[nodiscard]] const std::filesystem::path& keyfile() const { return keyfile_; }
    [[nodiscard]] const ClusterKeys& keys() const { return keys_; }

    [[nodiscard]] Config node_config(std::string_view name, uint16_t port = 0,
                                     std::vector<Endpoint> bootstrap = {}) const {
        if (!port) port = free_port();
        return config_for(root_.path() / std::string(name), keyfile_, port,
                          std::move(bootstrap), profile_);
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
    explicit TestService(std::string_view name,
                         ConfigProfile profile = ConfigProfile::functional)
        : keyfile_(temp_.path() / "cluster.key") {
        write_key(keyfile_);
        keys_ = load_cluster_keys(keyfile_);
        config_ = config_for(temp_.path() / std::string(name), keyfile_, free_port(), {}, profile);
    }

    ~TestService() {
        if (!service_) return;
        try {
            service_->stop();
        } catch (...) {
            // Destructors must not mask the test result; tests that assert
            // shutdown behaviour call stop() themselves.
        }
    }

    TestService(const TestService&) = delete;
    TestService& operator=(const TestService&) = delete;

    const std::filesystem::path& path() const noexcept { return temp_.path(); }
    const std::filesystem::path& keyfile() const noexcept { return keyfile_; }
    const ClusterKeys& keys() const noexcept { return keys_; }
    Config& config() noexcept { return config_; }
    const Config& config() const noexcept { return config_; }

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

// The root parts a NodeRuntime takes, built before it (a base is built first).
struct BareNodeResources {
    NodeResources resources;
    NodeIdentity identity;
    RecoveryProgress progress;
    MessageRoutes routes;
    NetworkLinks links;
    LocalState::StageHook stage_hook;
    BareNodeResources(const Config& config, ClusterKeys keys, LocalState::StageHook hook)
        : resources(config), identity(config.state_path, std::move(keys)),
          stage_hook(std::move(hook)) {}
};

// A NodeRuntime with the root parts it takes, owned as Service owns them:
// resources first, then the node; start() brings the control plane online
// and recovers local state on its own thread; stop() stops the resources,
// the node and that recovery, in that order. The store accessors are the
// tests' convenience; each waits, bounded, for local state to recover.
class BareNode : public BareNodeResources, public NodeRuntime {
  public:
    BareNode(Config config, ClusterKeys keys, NodeRuntime::StartupStageHook hook = {},
             TelemetryStore::Now telemetry_now = {},
             std::chrono::milliseconds durability_batch_window = test_durability_window)
        : BareNodeResources(config, keys, hook),
          NodeRuntime(std::move(config), identity, progress, resources.memory,
                      resources.transcode_rates, routes,
                      resources.events, links, std::move(hook), std::move(telemetry_now)),
          durability_batch_window_(durability_batch_window) {}
    ~BareNode() { stop(); }
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

    Accounts& accounts() { return accounts_; }
    UserStore& users() { return accounts_.users(); }
    SessionManager& sessions() { return accounts_.sessions(); }
    bool apply_session(const AuthSession& session) { return accounts_.apply_session(session); }
    void propagate_session(const AuthSession& session) { accounts_.propagate_session(session); }
    bool apply_user(const UserRecord& user) { return accounts_.apply_user(user); }
    void propagate_users() { accounts_.propagate_users(); }
    LocalState& local_state() { return local("local state").state(); }
    MetadataServer& metadata_server() { return local("metadata replica").metadata(); }
    StoragePool& local_store() { return local("data storage").state().data(); }
    LocalStore& control_store() { return local("control storage").state().control(); }
    PersistentBlockCache& block_cache() { return local("persistent cache").state().cache(); }
    ClaimStore& claims() { return local("retention state").state().retention(); }
    // The ledger over this node's stores, built on first use.
    ObjectLedger& ledger() {
        auto& state = local("the ledger's stores").state();
        std::lock_guard lock(ledger_mutex_);
        if (!ledger_)
            ledger_ = std::make_unique<RetentionLedger>(state.retention(), state.data(),
                                                        state.control());
        return *ledger_;
    }
    MetadataReplica& metadata_replica() { return local("metadata replica").state().replica(); }
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

class TestNode {
    TempDir temp_;
    std::filesystem::path keyfile_;
    ClusterKeys keys_;
    Config config_;
    std::unique_ptr<BareNode> node_;
    std::unique_ptr<DistributedStore> store_;
    std::function<void(const MetadataPublicationContext&)> publication_guard_;
    std::unique_ptr<MetadataManager> metadata_;
    std::unique_ptr<FileSystem> filesystem_;
    bool started_{};

  public:
    explicit TestNode(std::string_view name,
                      ConfigProfile profile = ConfigProfile::isolated)
        : keyfile_(temp_.path() / "cluster.key") {
        write_key(keyfile_);
        keys_ = load_cluster_keys(keyfile_);
        config_ = config_for(temp_.path() / std::string(name), keyfile_, free_port(), {}, profile);
    }

    ~TestNode() {
        if (!started_ || !node_) return;
        try { node_->stop(); } catch (...) {}
    }

    TestNode(const TestNode&) = delete;
    TestNode& operator=(const TestNode&) = delete;

    const std::filesystem::path& path() const noexcept { return temp_.path(); }
    const ClusterKeys& keys() const noexcept { return keys_; }
    Config& config() noexcept { return config_; }
    const Config& config() const noexcept { return config_; }

    void prepare() {
        REQUIRE(!node_);
        node_ = std::make_unique<BareNode>(config_, keys_);
    }

    BareNode& start_control_plane() {
        if (!node_) prepare();
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

    BareNode& wait_ready() {
        REQUIRE(node_);
        REQUIRE(started_);
        REQUIRE(node_->wait_local_state_ready(std::chrono::seconds{10}));
        if (!store_) {
            store_ = std::make_unique<DistributedStore>(*node_, node_->local_state(), node_->resources.activity,
                                                        node_->resources.data,
                                                        node_->resources.memory, node_->resources.events);
            metadata_ = std::make_unique<MetadataManager>(
                *node_, node_->local_state(), node_->metadata_server(), nullptr, [this](const MetadataPublicationContext& context) {
                    if (publication_guard_)
                        publication_guard_(context);
                });
            filesystem_ = std::make_unique<FileSystem>(node_->config(), node_->node_id(), node_->membership(), node_->local_state(), node_->metadata_server(), *store_, *metadata_,
                                                       node_->resources.memory);
        }
        return *node_;
    }

    BareNode& start() {
        start_control_plane();
        return wait_ready();
    }

    BareNode& node() { REQUIRE(node_); return *node_; }
    NodeResources& resources() { REQUIRE(node_); return node_->resources; }
    DistributedStore& store() { REQUIRE(store_); return *store_; }
    MetadataManager& metadata() { REQUIRE(metadata_); return *metadata_; }
    FileSystem& filesystem() { REQUIRE(filesystem_); return *filesystem_; }
};

// A first write on a forming cluster can find the replica set still forming
// (a peer not yet answering its survey); product callers retry
// MetadataNotReady, and so does this, until `write` succeeds or `limit`.
template <class Write>
bool retry_while_not_ready(Write&& write, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
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
    const auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        if (fn()) return true;
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
        if (e.code() != EEXIST) throw;
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

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "crypto.hpp"
#include "distributed_store.hpp"
#include "filesystem.hpp"
#include "fuse_frontend.hpp"
#include "metadata_manager.hpp"
#include "service.hpp"
#include "test_framework.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
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

using namespace std::chrono_literals;

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
    // Parallel test cases run in separate processes. Give each case its own
    // non-overlapping 64-port namespace so closing the probe socket cannot let
    // another Macha test race in and claim the same port before the server binds.
    // We still bind-probe every candidate because unrelated host processes may
    // legitimately occupy a port in the range.
    static uint16_t within_case{};
    constexpr uint16_t first = 20000;
    constexpr uint16_t block = 64;
    constexpr uint16_t blocks = 600; // 20000..58399
    const auto case_block = static_cast<uint16_t>(macha::test::case_index() % blocks);

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
    // Storage backends represent mounted media. Production deliberately does
    // not create a missing backend path because that could write onto the root
    // filesystem when a disk failed to mount; tests therefore create it explicitly.
    std::filesystem::create_directories(path);

    Config c;
    c.state_path = path.parent_path() / (path.filename().string() + ".state");
    c.storage_backends = {{path, 512ULL * 1024 * 1024}};
    c.key_file = key;
    c.listen_host = "127.0.0.1";
    c.advertise_host = "127.0.0.1";
    c.port = port;
    c.extent_size = 1024 * 1024;
    // Test state lives under the platform temporary directory. Linux commonly
    // mounts /tmp as a 2 GiB tmpfs, equal to or slightly smaller than the
    // production 2 GiB physical reserve after filesystem overhead. Capacity
    // policy is tested with explicit limits; ordinary functional tests must not
    // depend on the host's /tmp mount size.
    c.fuse.spool_reserve_free = 0;
    c.hydration.enabled = false;
    c.dead_after = 500ms;
    c.connect_timeout = 500ms;
    c.bootstrap = std::move(bootstrap);
    // plugin_path is deliberately left unset: an ordinary test wants no
    // subsystem plugins at all, and loading them costs a dlopen of libtorrent
    // and its dependencies in every one of the several hundred isolated test
    // processes. A test that needs the real plugin points plugin_path at this
    // build's MACHA_TEST_PLUGIN_DIR itself -- never at the installed default,
    // which could hold a stale build.

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
            // Destructors must not mask the test result. Explicit stop paths can
            // still be used by a test which needs to assert shutdown behaviour.
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
        service_ = std::make_unique<Service>(config_, keys_);
        service_->start();
        // Ordinary service fixtures preserve the historical fully-ready contract.
        // Lifecycle tests instantiate Service directly so they can intentionally
        // observe the early status/control-plane phases.
        (void)service_->filesystem();
        return *service_;
    }

    Service& service() {
        REQUIRE(service_ != nullptr);
        return *service_;
    }
};

class TestNode {
    TempDir temp_;
    std::filesystem::path keyfile_;
    ClusterKeys keys_;
    Config config_;
    std::unique_ptr<NodeRuntime> node_;
    std::unique_ptr<DistributedStore> store_;
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
        node_ = std::make_unique<NodeRuntime>(config_, keys_);
    }

    NodeRuntime& start_control_plane() {
        if (!node_) prepare();
        REQUIRE(!started_);
        node_->start();
        started_ = true;
        return *node_;
    }

    NodeRuntime& wait_ready() {
        REQUIRE(node_);
        REQUIRE(started_);
        REQUIRE(node_->wait_local_state_ready(std::chrono::seconds{10}));
        if (!store_) {
            store_ = std::make_unique<DistributedStore>(*node_);
            metadata_ = std::make_unique<MetadataManager>(*node_);
            filesystem_ = std::make_unique<FileSystem>(*node_, *store_, *metadata_);
        }
        return *node_;
    }

    NodeRuntime& start() {
        start_control_plane();
        return wait_ready();
    }

    NodeRuntime& node() { REQUIRE(node_); return *node_; }
    DistributedStore& store() { REQUIRE(store_); return *store_; }
    MetadataManager& metadata() { REQUIRE(metadata_); return *metadata_; }
    FileSystem& filesystem() { REQUIRE(filesystem_); return *filesystem_; }
};

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

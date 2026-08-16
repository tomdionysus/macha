// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "filesystem.hpp"
#include "http.hpp"
#include "local_store.hpp"
#include "metadata.hpp"
#include "media_catalogue.hpp"
#include "media_timestamps.hpp"
#include "media_vod.hpp"
#include "net.hpp"
#include "placement.hpp"
#include "service.hpp"
#include "storage_pool.hpp"
#include "persistent_cache.hpp"
#include "replica_selector.hpp"
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace macha;
using namespace std::chrono_literals;

namespace {
int failures = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expr "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            throw std::runtime_error(std::string("REQUIRE failed: ") + #expr);                     \
        }                                                                                          \
    } while (0)

class TempDir {
    std::filesystem::path path_;

  public:
    TempDir() {
        auto base = std::filesystem::temp_directory_path();
        path_ =
            base / ("macha-test-" + std::to_string(getpid()) + "-" + std::to_string(unix_ms()) +
                    "-" + std::to_string(random_node_id().bytes[0]));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const {
        return path_;
    }
};

class FakeMediaEngineSession final : public MediaEngineSession {
    bool running_{true};
    std::shared_ptr<MediaSegmentStore> segments_;
  public:
    explicit FakeMediaEngineSession(std::shared_ptr<MediaSegmentStore> segments)
        : segments_(std::move(segments)) {}
    bool running() const override { return running_; }
    std::optional<int> exit_code() const override { return running_ ? std::optional<int>{} : std::optional<int>{0}; }
    std::string diagnostics() const override { return {}; }
    std::shared_ptr<MediaSegmentStore> segments() const override { return segments_; }
    void note_segment_requested(uint64_t index) override { segments_->note_requested(index); }
    void stop() override { running_ = false; segments_->cancel(); }
};

class FakeMediaEngine final : public MediaEngine {
    mutable std::mutex mutex_;
    std::vector<PlaybackPlan> started_plans_;
    std::atomic_uint probes_{};
    std::atomic_uint vod_prepares_{};
  public:
    MediaEngineStatus status() const override { return {true, "fake", "fake-media-engine", true, true}; }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds = {}) override {
        ++probes_;
        MediaProbeResult result;
        result.format = "mov,mp4,m4a,3gp,3g2,mj2";
        result.duration_seconds = 60.0;
        result.bitrate = 4'000'000;
        result.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920, 1080, 0, 0, 8, true, false});
        result.streams.push_back(MediaStreamInfo{1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0, 2, 48000, 0, true, false});
        result.streams.push_back(MediaStreamInfo{2, MediaStreamType::subtitle, "subrip", "", "eng", 0, 0, 0, 0, 0, false, false});
        return result;
    }
    HlsVodPlan prepare_hls_vod(const MediaSource&, const PlaybackPlan& plan, double duration_seconds,
                               std::chrono::milliseconds segment_duration, bool,
                               std::chrono::milliseconds = {}) override {
        ++vod_prepares_;
        HlsVodPlan vod;
        vod.playback = plan;
        const double segment = segment_duration.count() / 1000.0;
        vod.source_duration_seconds = duration_seconds;
        vod.seek_segment_seconds = segment;
        vod.reusable_seek = true;
        double left = duration_seconds - plan.seek.count() / 1000.0;
        while (left > segment + 0.001) { vod.segment_durations.push_back(segment); left -= segment; }
        vod.segment_durations.push_back(std::max(0.001, left));
        return vod;
    }
    std::unique_ptr<MediaEngineSession> start_hls(const MediaSource&, const HlsVodPlan& vod_plan,
                                                  std::chrono::milliseconds segment_duration,
                                                  size_t max_ahead_segments,
                                                  uint64_t memory_limit,
                                                  const std::filesystem::path& spill_directory) override {
        {
            std::lock_guard lock(mutex_);
            started_plans_.push_back(vod_plan.playback);
        }
        auto store = std::make_shared<MediaSegmentStore>(max_ahead_segments, memory_limit,
                                                         spill_directory, segment_duration,
                                                         vod_plan.segment_durations);
        Bytes init{'i', 'n', 'i', 't'};
        Bytes segment{'s', 'e', 'g', 'm', 'e', 'n', 't'};
        REQUIRE(store->publish_init(std::move(init)));
        REQUIRE(store->publish_segment(std::move(segment), 4.0));
        // The fake models a sequential VOD producer: the complete immutable
        // playlist is available from the prepared duration plan, while only
        // the first fragment needs to exist at startup. Do not mark the store
        // finished after one fragment when the plan advertises more fragments;
        // MediaSegmentStore correctly treats that as a truncated pipeline.
        return std::make_unique<FakeMediaEngineSession>(std::move(store));
    }
    std::string extract_webvtt(const MediaSource&, int,
                               std::chrono::milliseconds) override {
        return "WEBVTT\n\n00:00.000 --> 00:01.000\nsubtitle\n";
    }
    std::vector<PlaybackPlan> started_plans() const {
        std::lock_guard lock(mutex_);
        return started_plans_;
    }
    unsigned probes() const { return probes_.load(); }
    unsigned vod_prepares() const { return vod_prepares_.load(); }
};

class FailingProbeMediaEngine final : public MediaEngine {
  public:
    MediaEngineStatus status() const override { return {true, "fake", "failing-probe", true, true}; }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds) override {
        throw std::runtime_error("synthetic probe failure");
    }
    HlsVodPlan prepare_hls_vod(const MediaSource&, const PlaybackPlan&, double,
                               std::chrono::milliseconds, bool, std::chrono::milliseconds = {}) override {
        throw std::runtime_error("prepare_hls_vod must not be called after a failed probe");
    }
    std::unique_ptr<MediaEngineSession> start_hls(const MediaSource&, const HlsVodPlan&,
                                                  std::chrono::milliseconds, size_t, uint64_t,
                                                  const std::filesystem::path&) override {
        throw std::runtime_error("start_hls must not be called after a failed probe");
    }
    std::string extract_webvtt(const MediaSource&, int, std::chrono::milliseconds) override {
        throw std::runtime_error("extract_webvtt must not be called after a failed probe");
    }
};


class BlockingMediaEngine final : public MediaEngine {
    mutable std::mutex mutex_;
    std::shared_ptr<MediaSegmentStore> pending_;
    std::atomic_uint starts_{};
  public:
    MediaEngineStatus status() const override { return {true, "fake", "blocking", true, true}; }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds = {}) override {
        MediaProbeResult result;
        result.format = "matroska,webm";
        result.duration_seconds = 60.0;
        result.bitrate = 4'000'000;
        result.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920, 1080, 0, 0, 8, true, false});
        result.streams.push_back(MediaStreamInfo{1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0, 2, 48000, 0, true, false});
        return result;
    }
    HlsVodPlan prepare_hls_vod(const MediaSource&, const PlaybackPlan& plan, double duration_seconds,
                               std::chrono::milliseconds segment_duration, bool,
                               std::chrono::milliseconds = {}) override {
        HlsVodPlan vod;
        vod.playback = plan;
        const double segment = segment_duration.count() / 1000.0;
        vod.source_duration_seconds = duration_seconds;
        vod.seek_segment_seconds = segment;
        vod.reusable_seek = true;
        double left = duration_seconds - plan.seek.count() / 1000.0;
        while (left > segment + 0.001) { vod.segment_durations.push_back(segment); left -= segment; }
        vod.segment_durations.push_back(std::max(0.001, left));
        return vod;
    }
    std::unique_ptr<MediaEngineSession> start_hls(const MediaSource&, const HlsVodPlan& vod_plan,
                                                  std::chrono::milliseconds segment_duration,
                                                  size_t max_ahead_segments, uint64_t memory_limit,
                                                  const std::filesystem::path& spill_directory) override {
        auto store = std::make_shared<MediaSegmentStore>(max_ahead_segments, memory_limit,
                                                         spill_directory, segment_duration,
                                                         vod_plan.segment_durations);
        {
            std::lock_guard lock(mutex_);
            pending_ = store;
        }
        ++starts_;
        return std::make_unique<FakeMediaEngineSession>(std::move(store));
    }
    std::string extract_webvtt(const MediaSource&, int, std::chrono::milliseconds) override { return {}; }
    unsigned starts() const { return starts_.load(); }
    void release() {
        std::shared_ptr<MediaSegmentStore> store;
        {
            std::lock_guard lock(mutex_);
            store = pending_;
        }
        REQUIRE(store != nullptr);
        REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
        REQUIRE(store->publish_segment(Bytes{'s', 'e', 'g'}, 4.0));
    }
};

class FakeHttpClient final : public HttpClient {
    struct Route {
        std::string contains;
        RemoteHttpResponse response;
    };
    std::vector<Route> routes_;

  public:
    void add(std::string contains, long status, std::string content_type, std::string body) {
        RemoteHttpResponse response;
        response.status = status;
        response.content_type = std::move(content_type);
        response.body.assign(body.begin(), body.end());
        routes_.push_back({std::move(contains), std::move(response)});
    }
    void add_bytes(std::string contains, long status, std::string content_type, Bytes body) {
        routes_.push_back({std::move(contains),
                           RemoteHttpResponse{status, std::move(content_type), std::move(body)}});
    }
    RemoteHttpResponse get(std::string_view url, const std::vector<std::string>&, size_t) override {
        for (const auto& route : routes_) {
            if (url.find(route.contains) != std::string_view::npos)
                return route.response;
        }
        return RemoteHttpResponse{404, "text/plain", {}};
    }
};

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
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

void write_key(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "test cluster key material - deterministic within this test\n";
}

Config config_for(const std::filesystem::path& path, const std::filesystem::path& key,
                  uint16_t port, std::vector<Endpoint> bootstrap = {}) {
    // Storage backends represent mounted media. Production deliberately does
    // not create a missing backend path because that could write onto the root
    // filesystem when a disk failed to mount.
    std::filesystem::create_directories(path);
    Config c;
    c.state_path = path.parent_path() / (path.filename().string() + ".state");
    c.storage_backends = {{path, 512ULL * 1024 * 1024}};
    c.key_file = key;
    c.listen_host = "127.0.0.1";
    c.advertise_host = "127.0.0.1";
    c.port = port;
    c.replication = 3;
    c.metadata_replication = 3;
    c.extent_size = 1024 * 1024;
    c.read_ahead_extents = 2;
    c.hydration.enabled = false;
    c.heartbeat = 100ms;
    c.dead_after = 500ms;
    c.connect_timeout = 500ms;
    c.control_stall_notice = 2s;
    c.data_stall_notice = 5s;
    c.bootstrap = std::move(bootstrap);
    return c;
}

template <class Fn> bool wait_until(Fn&& fn, std::chrono::milliseconds timeout = 5s) {
    auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        if (fn())
            return true;
        std::this_thread::sleep_for(20ms);
    }
    return fn();
}

class DelayedHttpBody final : public HttpBodySource {
    std::atomic_bool& entered_;
    uint64_t size_;
  public:
    DelayedHttpBody(std::atomic_bool& entered, uint64_t size) : entered_(entered), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_) return 0;
        entered_ = true;
        std::this_thread::sleep_for(75ms);
        auto n = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        std::fill_n(destination.data(), n, static_cast<uint8_t>('s'));
        return n;
    }
};

std::string raw_http_get(uint16_t port, std::string_view path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("http test socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        throw std::runtime_error("http test connect failed");
    }
    auto request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        auto n = send(fd, request.data() + sent, request.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); throw std::runtime_error("http test send failed"); }
        sent += static_cast<size_t>(n);
    }
    std::string response;
    std::array<char, 8192> buffer{};
    while (true) {
        auto n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        response.append(buffer.data(), static_cast<size_t>(n));
    }
    close(fd);
    return response;
}

Bytes pattern(size_t n) {
    Bytes out(n);
    uint64_t x = 0x123456789abcdef0ULL;
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
    char byte = 0;
    file.read(&byte, 1);
    REQUIRE(file.good());
    byte ^= 0x5a;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
    file.flush();
    REQUIRE(file.good());
}

void test_codec_and_crypto() {
    Writer w;
    w.u8(7);
    w.u16(0xabcd);
    w.u32(0x12345678);
    w.u64(0x0123456789abcdefULL);
    w.string("hello");
    Reader r(w.data());
    CHECK(r.u8() == 7);
    CHECK(r.u16() == 0xabcd);
    CHECK(r.u32() == 0x12345678);
    CHECK(r.u64() == 0x0123456789abcdefULL);
    CHECK(r.string() == "hello");
    r.finish();

    NodeInfo advertised;
    advertised.id = random_node_id();
    advertised.host = "media.example";
    advertised.port = 7437;
    advertised.failure_domain = "site-a";
    advertised.capacity = 123456;
    advertised.used = 4567;
    advertised.seen_unix_ms = 9999;
    advertised.metadata_generation = 42;
    Writer node_writer;
    encode_node_info(node_writer, advertised);
    Reader node_reader(node_writer.data());
    auto decoded_node = decode_node_info(node_reader);
    node_reader.finish();
    CHECK(decoded_node.id == advertised.id);
    CHECK(decoded_node.host == advertised.host);
    CHECK(decoded_node.failure_domain == advertised.failure_domain);
    CHECK(decoded_node.metadata_generation == 42);

    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto plain = pattern(65537);
    auto sealed = aes_gcm_seal(keys.storage, plain, keys.cluster_id);
    auto opened =
        aes_gcm_open(keys.storage, sealed.nonce, sealed.tag, sealed.ciphertext, keys.cluster_id);
    CHECK(opened == plain);

    // Regression: authenticated empty frames must not accidentally inherit the
    // AAD byte count as ciphertext length. Persistent framed connections depend on
    // precise framing across consecutive requests.
    Bytes empty;
    Bytes aad{1, 2, 3, 4, 5};
    auto empty_sealed = aes_gcm_seal(keys.auth, empty, aad);
    CHECK(empty_sealed.ciphertext.empty());
    CHECK(aes_gcm_open(keys.auth, empty_sealed.nonce, empty_sealed.tag,
                       empty_sealed.ciphertext, aad).empty());

    auto alice = x25519_generate();
    auto bob = x25519_generate();
    auto alice_shared = x25519_shared(alice.private_key, bob.public_key);
    auto bob_shared = x25519_shared(bob.private_key, alice.public_key);
    CHECK(alice_shared == bob_shared);

    sealed.ciphertext[0] ^= 1;
    bool rejected = false;
    try {
        (void)aes_gcm_open(keys.storage, sealed.nonce, sealed.tag, sealed.ciphertext,
                           keys.cluster_id);
    } catch (...) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_local_store() {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "store";
    auto plain = pattern(1024 * 1024 + 37);
    auto id = object_id(plain);
    uint64_t accounted_used = 0;

    {
        LocalStore store(root, 64 * 1024 * 1024, keys.storage);
        REQUIRE(store.put(id, plain));
        CHECK(store.has(id));
        REQUIRE(store.get(id).has_value());
        CHECK(*store.get(id) == plain);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        accounted_used = store.used();
        CHECK(accounted_used > plain.size());

        bool found_plain = false;
        for (auto& file : std::filesystem::recursive_directory_iterator(root / "objects")) {
            if (!file.is_regular_file())
                continue;
            std::ifstream in(file.path(), std::ios::binary);
            Bytes disk(std::istreambuf_iterator<char>(in), {});
            auto needle = std::span<const uint8_t>(plain).subspan(100, 128);
            found_plain =
                std::search(disk.begin(), disk.end(), needle.begin(), needle.end()) != disk.end();
        }
        CHECK(!found_plain);
    }

    // A clean restart restores exact accounting from the small journal without
    // starting an O(number-of-objects) tree scan.
    CHECK(std::filesystem::exists(root / ".macha.accounting"));
    CHECK(std::filesystem::file_size(root / ".macha.accounting") >= 256);
    {
        LocalStore reopened(root, 64 * 1024 * 1024, keys.storage);
        CHECK(reopened.scan_complete());
        CHECK(reopened.used() == accounted_used);
        REQUIRE(reopened.get(id).has_value());
        CHECK(*reopened.get(id) == plain);
    }

    // Corrupt/missing state is a migration/recovery case: fall back to one full
    // reconciliation, then recreate a trusted checkpoint for later O(1) boots.
    {
        std::ofstream out(root / ".macha.accounting", std::ios::binary | std::ios::trunc);
        Bytes junk(256, 0x5a);
        out.write(reinterpret_cast<const char*>(junk.data()),
                  static_cast<std::streamsize>(junk.size()));
    }
    {
        LocalStore recovered(root, 64 * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return recovered.scan_complete(); }));
        CHECK(recovered.used() == accounted_used);
        REQUIRE(recovered.get(id).has_value());
    }
    {
        LocalStore recovered_restart(root, 64 * 1024 * 1024, keys.storage);
        CHECK(recovered_restart.scan_complete());
        CHECK(recovered_restart.used() == accounted_used);
    }

    {
        StorageLock first(t.path() / "locked");
        bool rejected = false;
        try {
            StorageLock second(t.path() / "locked");
        } catch (...) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

void test_storage_pool_and_persistent_cache() {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto state = t.path() / "state";
    auto disk1 = t.path() / "disk1";
    auto disk2 = t.path() / "disk2";
    auto disk3 = t.path() / "disk3";
    std::filesystem::create_directories(disk1);
    std::filesystem::create_directories(disk2);
    std::filesystem::create_directories(disk3);

    auto node = load_or_create_node_id(state);
    StoragePool pool(state, node,
                     {{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}},
                     keys.storage);
    CHECK(pool.online_backends() == 2);
    CHECK(pool.limit() == 128ULL * 1024 * 1024);

    std::vector<std::pair<ObjectId, Bytes>> objects;
    for (size_t i = 0; i < 32; ++i) {
        auto data = pattern(128 * 1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        objects.push_back({id, std::move(data)});
    }
    for (const auto& [id, data] : objects) {
        auto got = pool.get(id);
        REQUIRE(got.has_value());
        CHECK(*got == data);
    }

    // Maintenance traversal is resumable. A one-object slice must not rebuild
    // or consume the whole object namespace, and a complete pass eventually
    // visits every physical object without blocking foreground pool operations.
    {
        StoragePool::Cursor cursor;
        std::set<ObjectId> seen;
        bool complete = false;
        size_t calls = 0;
        while (!complete && calls++ < 256) {
            auto id = pool.next_object(cursor, complete);
            if (id)
                seen.insert(*id);
        }
        CHECK(complete);
        CHECK(seen.size() == objects.size());
    }
    {
        size_t slices = 0;
        bool complete = false;
        while (!complete && slices++ < 256) {
            auto step = pool.scrub_step(0, 1);
            CHECK(step.objects <= 1);
            complete = step.complete;
        }
        CHECK(complete);
        CHECK(slices > 1);
    }
    {
        auto yielded = pool.rebalance_step(0, 1, [] { return true; });
        CHECK(yielded.yielded);
        CHECK(yielded.objects == 0);
        CHECK(yielded.bytes == 0);
    }

    // Add a third disk live and migrate local placement without changing the
    // node identity or DHT replica accounting.
    pool.reconfigure({{disk1, 64ULL * 1024 * 1024},
                      {disk2, 64ULL * 1024 * 1024},
                      {disk3, 64ULL * 1024 * 1024}});
    pool.refresh();
    CHECK(pool.online_backends() == 3);
    (void)pool.rebalance_once();
    size_t on_disk3 = 0;
    for (const auto& [id, _] : objects)
        on_disk3 += std::filesystem::exists(object_path(disk3, id)) ? 1 : 0;
    CHECK(on_disk3 > 0);

    // A temporary disappearance does not change placement weight. The node can
    // keep serving/falling back to surviving disks without remapping the whole
    // pool; the returning disk resumes its old share and rebalance converges.
    auto parked = t.path() / "disk2.offline";
    std::filesystem::rename(disk2, parked);
    pool.refresh();
    CHECK(pool.online_backends() == 2);
    CHECK(pool.limit() == 192ULL * 1024 * 1024);
    CHECK(load_or_create_node_id(state) == node);
    std::filesystem::rename(parked, disk2);
    pool.refresh();
    CHECK(pool.online_backends() == 3);
    (void)pool.rebalance_once();

    // Configuration removal and later re-addition of a backend is also live.
    pool.reconfigure({{disk1, 64ULL * 1024 * 1024}, {disk2, 64ULL * 1024 * 1024}});
    pool.refresh();
    CHECK(pool.online_backends() == 2);
    CHECK(pool.limit() == 128ULL * 1024 * 1024);
    pool.reconfigure({{disk1, 64ULL * 1024 * 1024},
                      {disk2, 64ULL * 1024 * 1024},
                      {disk3, 64ULL * 1024 * 1024}});
    pool.refresh();
    CHECK(pool.online_backends() == 3);

    // Local authoritative placement uses the same capacity weighting. A backend
    // eight times larger should receive overwhelmingly more objects, without
    // using live free space as part of the score.
    auto weighted_state = t.path() / "weighted-state";
    auto small_disk = t.path() / "weighted-small";
    auto large_disk = t.path() / "weighted-large";
    std::filesystem::create_directories(small_disk);
    std::filesystem::create_directories(large_disk);
    auto weighted_node = load_or_create_node_id(weighted_state);
    StoragePool weighted(weighted_state, weighted_node,
                         {{small_disk, 8ULL * 1024 * 1024},
                          {large_disk, 64ULL * 1024 * 1024}},
                         keys.storage);
    size_t small_objects = 0, large_objects = 0;
    for (size_t i = 0; i < 512; ++i) {
        auto data = pattern(1024 + i);
        data[0] ^= static_cast<uint8_t>(i);
        data[1] ^= static_cast<uint8_t>(i >> 8U);
        auto id = object_id(data);
        REQUIRE(weighted.put(id, data));
        small_objects += std::filesystem::exists(object_path(small_disk, id)) ? 1 : 0;
        large_objects += std::filesystem::exists(object_path(large_disk, id)) ? 1 : 0;
    }
    CHECK(small_objects + large_objects == 512);
    CHECK(large_objects > small_objects * 5);

    auto cache_root = t.path() / "cache";
    MetadataRecord cached_metadata;
    {
        PersistentBlockCache cache({cache_root, 2, true}, keys.storage);
        auto a = pattern(8192);
        auto b = pattern(8193);
        auto c = pattern(8194);
        a[0] ^= 0x11;
        b[0] ^= 0x22;
        c[0] ^= 0x33;
        auto ia = object_id(a), ib = object_id(b), ic = object_id(c);
        REQUIRE(cache.put(ia, a));
        std::this_thread::sleep_for(2ms);
        REQUIRE(cache.put(ib, b));
        REQUIRE(cache.get(ia).has_value()); // a is now the hotter block.
        std::this_thread::sleep_for(2ms);
        REQUIRE(cache.put(ic, c));
        CHECK(cache.blocks() == 2);
        CHECK(cache.has(ia));
        CHECK(cache.has(ic));
        CHECK(!cache.has(ib));

        cached_metadata = genesis_metadata();
        cache.remember_metadata(cached_metadata);
        REQUIRE(cache.metadata().has_value());
        CHECK(cache.metadata()->hash == cached_metadata.hash);
    }
    {
        PersistentBlockCache reopened({cache_root, 2, true}, keys.storage);
        CHECK(reopened.blocks() == 2);
        REQUIRE(reopened.metadata().has_value());
        CHECK(reopened.metadata()->hash == cached_metadata.hash);
        reopened.reconfigure({cache_root, 0, true});
        CHECK(!reopened.enabled());
        CHECK(!reopened.metadata().has_value());
        reopened.reconfigure({cache_root, 2, true});
        CHECK(reopened.enabled());
        REQUIRE(reopened.metadata().has_value());
    }
}

void test_metadata_codec_and_replica() {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    MetadataSnapshot snap = decode_snapshot(genesis_metadata().payload);
    auto a = random_node_id();
    auto b = random_node_id();
    auto c = random_node_id();
    snap.metadata_voters = {a, b, c};
    snap.mutation_sequences[a] = 7;
    snap.mutation_sequences[b] = 11;
    FsEntry file;
    file.type = EntryType::file;
    file.size = 123;
    snap.entries["/movie.mkv"] = file;
    auto garbage_id = object_id(pattern(4096));
    snap.garbage.push_back({garbage_id});
    auto encoded = encode_snapshot(snap);
    auto decoded = decode_snapshot(encoded);
    CHECK(decoded.metadata_voters == snap.metadata_voters);
    CHECK(decoded.mutation_sequences == snap.mutation_sequences);
    CHECK(decoded.entries.at("/movie.mkv").size == 123);
    REQUIRE(decoded.garbage.size() == 1);
    CHECK(decoded.garbage.front().id == garbage_id);

    // 0.4.0 metadata snapshots had no catalogue-root field. 0.5.0 must read
    // them directly so an existing namespace upgrades to an empty catalogue
    // rather than requiring destructive state migration.
    Writer old;
    const std::array<uint8_t, 8> old_magic{'D', 'H', 'T', 'M', 'E', 'T', 'A', '5'};
    old.raw(old_magic);
    old.u32(0); // metadata voters
    old.u32(1); // data replication
    old.u64(4ULL * 1024 * 1024);
    old.u32(1); // root entry
    old.string("/");
    old.u8(static_cast<uint8_t>(EntryType::directory));
    old.u32(0755);
    old.u32(0);
    old.u32(0);
    old.u64(0);
    old.i64(0);
    old.i64(0);
    old.u64(0);
    old.u32(0); // extents
    old.u32(0); // garbage
    auto upgraded = decode_snapshot(old.data());
    CHECK(!upgraded.catalogue_root.has_value());
    CHECK(upgraded.entries.contains("/"));

    auto replica_path = t.path() / "node";
    MetadataReplica replica(replica_path, keys.storage);
    auto current = replica.current();
    CHECK(replica.committed().hash == current.hash);
    MetadataRecord next;
    REQUIRE(replica.cas(current.generation, current.hash, encoded, &next));
    CHECK(next.generation == current.generation + 1);
    CHECK(decode_snapshot(next.payload).metadata_voters.size() == 3);
    // A successful vote is not yet a committed cluster checkpoint. Recovery
    // witnesses advance only after MetadataManager has observed quorum.
    CHECK(replica.committed().hash == current.hash);
    REQUIRE(replica.remember_current_committed(next.generation, next.hash));
    CHECK(replica.committed().hash == next.hash);
    CHECK(!replica.remember_current_committed(next.generation, current.hash));

    MetadataReplica reopened(replica_path, keys.storage);
    CHECK(reopened.current().hash == next.hash);
    CHECK(reopened.committed().hash == next.hash);

    // current.meta without committed.meta is pre-v3/incomplete state. Do not
    // silently promote it into a recovery checkpoint.
    auto legacy_path = t.path() / "legacy-node";
    {
        MetadataReplica legacy(legacy_path, keys.storage);
        (void)legacy;
    }
    std::filesystem::remove(legacy_path / "metadata" / "committed.meta");
    bool legacy_rejected = false;
    try {
        MetadataReplica legacy(legacy_path, keys.storage);
        (void)legacy;
    } catch (const std::exception&) {
        legacy_rejected = true;
    }
    CHECK(legacy_rejected);
}

void test_config() {
    CHECK(Config{}.log_level == LogLevel::info);
    CHECK(parse_log_level("all") == LogLevel::all);
    CHECK(parse_log_level("DEBUG") == LogLevel::debug);
    CHECK(parse_log_level("Info") == LogLevel::info);
    CHECK(parse_log_level("warning") == LogLevel::warn);
    CHECK(parse_log_level("ERROR") == LogLevel::error);

    ConsoleLogger info_logger(LogLevel::info);
    CHECK(!info_logger.enabled(LogLevel::all));
    CHECK(!info_logger.enabled(LogLevel::debug));
    CHECK(info_logger.enabled(LogLevel::info));
    CHECK(info_logger.enabled(LogLevel::warn));
    CHECK(info_logger.enabled(LogLevel::error));
    ConsoleLogger all_logger(LogLevel::all);
    CHECK(all_logger.enabled(LogLevel::all));
    CHECK(all_logger.enabled(LogLevel::debug));
    CHECK(all_logger.enabled(LogLevel::info));
    CHECK(all_logger.enabled(LogLevel::warn));
    CHECK(all_logger.enabled(LogLevel::error));
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::warn));
    CHECK(!Log::enabled(LogLevel::all));
    CHECK(!Log::enabled(LogLevel::debug));
    CHECK(!Log::enabled(LogLevel::info));
    CHECK(Log::enabled(LogLevel::warn));
    CHECK(Log::enabled(LogLevel::error));
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
    MaintenanceConfig maintenance_policy;
    CHECK(maintenance_background_interval(maintenance_policy) == 30000ms);
    maintenance_policy.no_progress_backoff = 2000ms;
    CHECK(maintenance_background_interval(maintenance_policy) == 5000ms);
    maintenance_policy.no_progress_backoff = 45000ms;
    CHECK(maintenance_background_interval(maintenance_policy) == 45000ms);
    CHECK(std::string(message_type_name(MessageType::members)) == "members");
    CHECK(std::string(message_type_name(MessageType::get_object)) == "get_object");

#ifdef MACHA_HAVE_YAML_CPP
    TempDir t;
    auto keyfile = (t.path() / "key").string();
    auto yaml = t.path() / "node.yaml";
    auto state = t.path() / "state";
    auto disk1 = t.path() / "disk1";
    auto disk2 = t.path() / "disk2";
    auto cache_dir = t.path() / "cache";
    {
        std::ofstream out(yaml);
        out << "state_path: " << state.string() << "\n"
            << "key_file: " << keyfile << "\n"
            << "log_level: WARN\n"
            << "storage:\n"
            << "  - path: " << disk1.string() << "\n"
            << "    limit: 10T\n"
            << "  - path: " << disk2.string() << "\n"
            << "    limit: 10T\n"
            << "cache:\n"
            << "  path: " << cache_dir.string() << "\n"
            << "  max_blocks: 4096\n"
            << "  prefer_metadata: true\n"
            << "filesystem:\n"
            << "  allow_other: true\n"
            << "  root_uid: 501\n"
            << "  root_gid: 20\n"
            << "  root_mode: '0750'\n"
            << "network:\n"
            << "  listen: 127.0.0.1\n"
            << "  advertise: media.example\n"
            << "  port: 7440\n"
            << "  failure_domain: site-x\n"
            << "  max_frame_size: 192K\n"
            << "  control_stall_notice_ms: 4100\n"
            << "  data_stall_notice_ms: 88000\n"
            << "dht:\n"
            << "  replicas: 3\n"
            << "  metadata_replicas: 3\n"
            << "  extent_size: 16M\n"
            << "  read_ahead: 5\n"
            << "bootstrap:\n"
            << "  - seed1.example:7440\n"
            << "  - seed2.example:7440\n"
            << "maintenance:\n"
            << "  interval_ms: 250\n"
            << "  garbage_grace_ms: 1234\n"
            << "  busy_bandwidth_fraction: 0.03\n"
            << "  idle_bandwidth_fraction: 0.60\n"
            << "  cpu_target: 0.40\n"
            << "hydration:\n"
            << "  enabled: true\n"
            << "  interval_ms: 75\n"
            << "  active_timeout_ms: 12000\n"
            << "  max_inflight: 6\n"
            << "  catalogue_lookahead: 2\n"
            << "  engines:\n"
            << "    read_ahead: { enabled: true, priority: 1200 }\n"
            << "    current_file: { enabled: true, priority: 650 }\n"
            << "    catalogue: { enabled: true, priority: 250 }\n"
            << "catalogue:\n"
            << "  api:\n"
            << "    enabled: true\n"
            << "    listen: 127.0.0.1\n"
            << "    port: 7441\n"
            << "    token_file: " << (t.path() / "api.token").string() << "\n"
            << "    max_request_bytes: 2M\n"
            << "    workers: 7\n"
            << "    max_queued_connections: 33\n"
            << "    stream_chunk_bytes: 64K\n"
            << "  scanner:\n"
            << "    enabled: true\n"
            << "    interval_ms: 60000\n"
            << "    roots: [/TV, /Movies, /Music]\n"
            << "    max_artwork_bytes: 6M\n"
            << "    providers:\n"
            << "      tmdb:\n"
            << "        enabled: true\n"
            << "        token_file: " << (t.path() / "tmdb.token").string() << "\n"
            << "        language: en-GB\n"
            << "        image_size: w500\n"
            << "      musicbrainz:\n"
            << "        enabled: true\n"
            << "        contact: https://example.test/macha\n"
            << "        cover_size: '500'\n"
            << "streaming:\n"
            << "  enabled: true\n"
            << "  ffmpeg: /legacy/ignored/ffmpeg\n"
            << "  ffprobe: /legacy/ignored/ffprobe\n"
            << "  temp_path: " << (t.path() / "streams").string() << "\n"
            << "  max_sessions: 9\n"
            << "  max_video_transcodes: 2\n"
            << "  max_audio_transcodes: 5\n"
            << "  session_idle_ms: 60000\n"
            << "  startup_timeout_ms: 7000\n"
            << "  segment_duration_ms: 3000\n"
            << "  max_ahead_segments: 11\n"
            << "  segment_memory_bytes: 96M\n"
            << "  probe_bytes: 12M\n"
            << "  probe_analyze_duration_ms: 4000\n"
            << "  probe_timeout_ms: 9000\n";
    }

    std::vector<std::string> yaml_args{"macha", "--config", yaml.string()};
    std::vector<char*> yaml_argv;
    for (auto& arg : yaml_args)
        yaml_argv.push_back(arg.data());
    auto yc = parse_config(static_cast<int>(yaml_argv.size()), yaml_argv.data());
    CHECK(yc.state_path == state);
    CHECK(yc.storage_backends.size() == 2);
    CHECK(yc.storage_backends[0].limit == 10ULL * 1024 * 1024 * 1024 * 1024);
    CHECK(yc.cache.path == cache_dir);
    CHECK(yc.cache.max_blocks == 4096);
    CHECK(yc.log_level == LogLevel::warn);
    CHECK(yc.filesystem.allow_other);
    CHECK(yc.filesystem.root_uid == 501);
    CHECK(yc.filesystem.root_gid == 20);
    CHECK(yc.filesystem.root_mode == 0750);
    CHECK(yc.bootstrap.size() == 2);
    CHECK(yc.port == 7440);
    CHECK(yc.max_frame_size == 192ULL * 1024);
    CHECK(yc.control_stall_notice == 4100ms);
    CHECK(yc.data_stall_notice == 88000ms);
    CHECK(yc.maintenance.interval == 250ms);
    CHECK(yc.maintenance.garbage_grace == 1234ms);
    CHECK(yc.maintenance.busy_bandwidth_fraction == 0.03);
    CHECK(yc.hydration.enabled);
    CHECK(yc.hydration.interval == 75ms);
    CHECK(yc.hydration.active_timeout == 12000ms);
    CHECK(yc.hydration.max_inflight == 6);
    CHECK(yc.hydration.catalogue_lookahead == 2);
    CHECK(yc.hydration.read_ahead.priority == 1200);
    CHECK(yc.hydration.current_file.priority == 650);
    CHECK(yc.hydration.catalogue.priority == 250);
    CHECK(yc.catalogue.api.enabled);
    CHECK(yc.catalogue.api.listen == "127.0.0.1");
    CHECK(yc.catalogue.api.port == 7441);
    REQUIRE(yc.catalogue.api.token_file.has_value());
    CHECK(*yc.catalogue.api.token_file == t.path() / "api.token");
    CHECK(yc.catalogue.api.max_request_bytes == 2ULL * 1024 * 1024);
    CHECK(yc.catalogue.api.workers == 7);
    CHECK(yc.catalogue.api.max_queued_connections == 33);
    CHECK(yc.catalogue.api.stream_chunk_bytes == 64ULL * 1024);
    CHECK(yc.catalogue.scanner.enabled);
    CHECK(yc.catalogue.scanner.interval == 60000ms);
    CHECK(yc.catalogue.scanner.roots.size() == 3);
    CHECK(yc.catalogue.scanner.roots[0] == "/TV");
    CHECK(yc.catalogue.scanner.max_artwork_bytes == 6ULL * 1024 * 1024);
    REQUIRE(yc.catalogue.scanner.tmdb.token_file.has_value());
    CHECK(*yc.catalogue.scanner.tmdb.token_file == t.path() / "tmdb.token");
    CHECK(yc.catalogue.scanner.tmdb.language == "en-GB");
    CHECK(yc.catalogue.scanner.tmdb.image_size == "w500");
    CHECK(yc.catalogue.scanner.musicbrainz.contact == "https://example.test/macha");
    CHECK(yc.catalogue.scanner.musicbrainz.cover_size == "500");
    CHECK(yc.streaming.enabled);
    REQUIRE(yc.streaming.temp_path.has_value());
    CHECK(*yc.streaming.temp_path == t.path() / "streams");
    CHECK(yc.streaming.max_sessions == 9);
    CHECK(yc.streaming.max_video_transcodes == 2);
    CHECK(yc.streaming.max_audio_transcodes == 5);
    CHECK(yc.streaming.session_idle == 60000ms);
    CHECK(yc.streaming.startup_timeout == 7000ms);
    CHECK(yc.streaming.segment_duration == 3000ms);
    CHECK(yc.streaming.max_ahead_segments == 11);
    CHECK(yc.streaming.segment_memory_bytes == 96ULL * 1024 * 1024);
    CHECK(yc.streaming.probe_bytes == 12ULL * 1024 * 1024);
    CHECK(yc.streaming.probe_analyze_duration == 4000ms);
    CHECK(yc.streaming.probe_timeout == 9000ms);

    // CLI remains useful for node-local/runtime overrides, but configuration
    // now always starts from an explicit YAML file.
    std::vector<std::string> override_args{"macha", "--config", yaml.string(),
                                            "--port", "8123", "--replicas", "5",
                                            "--read-ahead", "4", "--failure-domain", "site-a",
                                            "--connect-timeout", "1700",
                                            "--max-frame-size", "320K",
                                            "--control-stall-notice", "4200",
                                            "--data-stall-notice", "90000",
                                            "--metadata-cache", "125",
                                            "--log-level", "DEBUG"};
    std::vector<char*> override_argv;
    for (auto& arg : override_args)
        override_argv.push_back(arg.data());
    auto overridden = parse_config(static_cast<int>(override_argv.size()), override_argv.data());
    CHECK(overridden.port == 8123);
    CHECK(overridden.replication == 5);
    CHECK(overridden.read_ahead_extents == 4);
    CHECK(overridden.failure_domain == "site-a");
    CHECK(overridden.connect_timeout == 1700ms);
    CHECK(overridden.max_frame_size == 320ULL * 1024);
    CHECK(overridden.control_stall_notice == 4200ms);
    CHECK(overridden.data_stall_notice == 90000ms);
    CHECK(overridden.metadata_cache == 125ms);
    CHECK(overridden.log_level == LogLevel::debug);

    // Obsolete configuration surfaces are rejected rather than silently
    // translated onto current semantics.
    for (const auto& legacy : std::vector<std::vector<std::string>>{
             {"--verbose"}, {"--control-timeout", "1000"}, {"--data-timeout", "1000"},
             {"--path", disk1.string()}, {"--limit", "10G"}}) {
        std::vector<std::string> legacy_args{"macha", "--config", yaml.string()};
        legacy_args.insert(legacy_args.end(), legacy.begin(), legacy.end());
        std::vector<char*> legacy_argv;
        for (auto& arg : legacy_args)
            legacy_argv.push_back(arg.data());
        bool rejected = false;
        try {
            (void)parse_config(static_cast<int>(legacy_argv.size()), legacy_argv.data());
        } catch (const std::exception&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    for (size_t i = 0; i < 3; ++i) {
        auto legacy_yaml = t.path() / ("legacy-" + std::to_string(i) + ".yaml");
        std::ofstream out(legacy_yaml);
        out << "state_path: " << state.string() << "\n"
            << "key_file: " << keyfile << "\n"
            << "storage:\n"
            << "  - path: " << disk1.string() << "\n"
            << "    limit: 10G\n";
        if (i == 0)
            out << "verbose: true\n";
        else if (i == 1)
            out << "network:\n  control_timeout_ms: 1000\n";
        else
            out << "network:\n  data_timeout_ms: 1000\n";
        out.close();

        std::vector<std::string> old_args{"macha", "--config", legacy_yaml.string()};
        std::vector<char*> old_argv;
        for (auto& arg : old_args)
            old_argv.push_back(arg.data());
        bool rejected = false;
        try {
            (void)parse_config(static_cast<int>(old_argv.size()), old_argv.data());
        } catch (const std::exception&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    for (const auto& bad : std::vector<std::vector<std::string>>{
             {"--log-level", "TRACE"}, {"--port", "0"}}) {
        std::vector<std::string> bad_args{"macha", "--config", yaml.string()};
        bad_args.insert(bad_args.end(), bad.begin(), bad.end());
        std::vector<char*> bad_argv;
        for (auto& arg : bad_args)
            bad_argv.push_back(arg.data());
        bool rejected = false;
        try {
            (void)parse_config(static_cast<int>(bad_argv.size()), bad_argv.data());
        } catch (const std::exception&) {
            rejected = true;
        }
        CHECK(rejected);
    }
#endif
}

void test_placement() {
    std::vector<NodeInfo> nodes;
    for (int i = 0; i < 8; ++i) {
        NodeInfo n;
        n.id = random_node_id();
        n.host = "127.0.0.1";
        n.port = static_cast<uint16_t>(7000 + i);
        nodes.push_back(n);
    }
    auto key = sha256({reinterpret_cast<const uint8_t*>("placement"), 9});
    auto a = rendezvous_nodes(key.bytes, nodes, 3);
    auto b = rendezvous_nodes(key.bytes, nodes, 3);
    REQUIRE(a.size() == 3);
    CHECK(a[0].id == b[0].id && a[1].id == b[1].id && a[2].id == b[2].id);

    auto old = a;
    NodeInfo extra;
    extra.id = random_node_id();
    extra.host = "127.0.0.1";
    extra.port = 9000;
    nodes.push_back(extra);
    auto newer = rendezvous_nodes(key.bytes, nodes, 3);
    size_t common = 0;
    for (auto& x : old)
        for (auto& y : newer)
            if (x.id == y.id)
                ++common;
    CHECK(common >= 2);

    std::vector<NodeInfo> topology(4);
    for (size_t i = 0; i < topology.size(); ++i) {
        topology[i].id = random_node_id();
        topology[i].host = "127.0.0.1";
        topology[i].port = static_cast<uint16_t>(9100 + i);
    }
    topology[0].failure_domain = "site-a";
    topology[1].failure_domain = "site-a";
    topology[2].failure_domain = "site-b";
    topology[3].failure_domain = "site-c";
    auto diverse = rendezvous_nodes(key.bytes, topology, 3);
    REQUIRE(diverse.size() == 3);
    std::set<std::string> domains;
    for (const auto& node : diverse)
        domains.insert(node.failure_domain);
    CHECK(domains.size() == 3);
}

void test_capacity_placement() {
    constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
    constexpr uint64_t TiB = 1024ULL * GiB;

    auto make_node = [](uint8_t tag, uint64_t capacity, std::string domain = {}) {
        NodeInfo node;
        node.id.bytes.fill(0);
        node.id.bytes.back() = tag;
        node.host = "127.0.0.1";
        node.port = static_cast<uint16_t>(9300 + tag);
        node.capacity = capacity;
        node.failure_domain = std::move(domain);
        return node;
    };

    std::vector<NodeInfo> asymmetric{
        make_node(1, 10 * TiB), make_node(2, 10 * TiB), make_node(3, 8 * GiB)};

    // With three nodes and R=2, all physical capacity can participate: the two
    // 10 TiB nodes are in almost every shard and the 8 GiB node owns only its
    // proportional share. This is ~10 TiB logical, not 8 GiB.
    CHECK(placement_logical_capacity(asymmetric, 2) == 10 * TiB + 4 * GiB);

    // With only 10 TiB + 8 GiB and R=2 every logical byte needs both nodes, so
    // the small node correctly caps the namespace at 8 GiB.
    std::vector<NodeInfo> two_nodes{asymmetric[0], asymmetric[2]};
    CHECK(placement_logical_capacity(two_nodes, 2) == 8 * GiB);

    CHECK(placement_shards == (uint64_t{1} << 32U));
    auto shard_id = [](uint32_t shard) {
        std::array<uint8_t, 32> key{};
        key[0] = static_cast<uint8_t>(shard >> 24U);
        key[1] = static_cast<uint8_t>(shard >> 16U);
        key[2] = static_cast<uint8_t>(shard >> 8U);
        key[3] = static_cast<uint8_t>(shard);
        return key;
    };
    auto preferred_contains = [](const std::vector<NodeInfo>& placed, const NodeId& id,
                                 size_t replicas) {
        return std::any_of(placed.begin(), placed.begin() + std::min(replicas, placed.size()),
                           [&](const auto& node) { return node.id == id; });
    };

    // Exact 32-bit quota arithmetic: the 8 GiB node receives 3,354,133 of
    // 4,294,967,296 shards. With these stable node IDs its interval is the tail
    // of the systematic sample space, so the ownership boundary is exact. This
    // replaces the old exhaustive 65,536-shard walk.
    constexpr uint64_t small_quota = 3'354'133;
    const auto first_small = static_cast<uint32_t>(placement_shards - small_quota);
    auto just_before = capacity_placement_nodes(shard_id(first_small - 1), asymmetric, 2);
    auto at_boundary = capacity_placement_nodes(shard_id(first_small), asymmetric, 2);
    auto at_end = capacity_placement_nodes(shard_id(std::numeric_limits<uint32_t>::max()),
                                            asymmetric, 2);
    REQUIRE(just_before.size() == 3);
    REQUIRE(at_boundary.size() == 3);
    REQUIRE(at_end.size() == 3);
    CHECK(!preferred_contains(just_before, asymmetric[2].id, 2));
    CHECK(preferred_contains(at_boundary, asymmetric[2].id, 2));
    CHECK(preferred_contains(at_end, asymmetric[2].id, 2));
    CHECK(at_boundary[0].id != at_boundary[1].id);

    // R=1 is weighted rendezvous over the stable shard space. Adding a backend
    // or node may steal shards, but must never make two unchanged owners trade
    // shards with each other. Sample deterministically across the 32-bit space;
    // iterating all 2^32 virtual shards is neither necessary nor desirable.
    std::vector<NodeInfo> before{make_node(10, 10 * TiB), make_node(20, 10 * TiB)};
    auto after = before;
    after.push_back(make_node(30, 10 * TiB));
    constexpr size_t placement_samples = 16'384;
    size_t moved_to_new = 0;
    for (size_t i = 0; i < placement_samples; ++i) {
        const auto shard = static_cast<uint32_t>(static_cast<uint64_t>(i) * 2'654'435'761ULL);
        auto key = shard_id(shard);
        auto old_owner = capacity_placement_nodes(key, before, 1).front().id;
        auto new_owner = capacity_placement_nodes(key, after, 1).front().id;
        if (old_owner != new_owner) {
            CHECK(new_owner == after.back().id);
            ++moved_to_new;
        }
    }
    CHECK(moved_to_new > placement_samples / 4);
    CHECK(moved_to_new < placement_samples * 2 / 5);

    // Failure-domain diversity remains a stronger constraint than raw node
    // capacity when enough domains exist. One replica must fit in site-b.
    std::vector<NodeInfo> domains{make_node(1, 10 * TiB, "site-a"),
                                  make_node(2, 10 * TiB, "site-a"),
                                  make_node(3, 8 * GiB, "site-b")};
    CHECK(placement_logical_capacity(domains, 2) == 8 * GiB);
    std::array<uint8_t, 32> key{};
    auto diverse = capacity_placement_nodes(key, domains, 2);
    REQUIRE(diverse.size() == 3);
    CHECK(diverse[0].failure_domain != diverse[1].failure_domain);
}

void test_async_rpc_move_ownership() {
    std::atomic_int cancelled{};

    // AsyncRpc is an owning cancellation handle. Moving it must transfer that
    // ownership; destruction of the moved-from object must be inert. This is
    // particularly important on libc++, where std::function's moved-from state
    // is permitted to remain non-empty.
    std::optional<AsyncRpc> moved;
    {
        std::promise<RpcReply> promise;
        AsyncRpc original(promise.get_future(), [&] { ++cancelled; }, {});
        moved.emplace(std::move(original));
    }
    CHECK(cancelled.load() == 0);
    moved.reset();
    CHECK(cancelled.load() == 1);

    // Move assignment must also cancel any request already owned by the target,
    // while leaving the source destructor inert.
    std::promise<RpcReply> first_promise;
    std::promise<RpcReply> second_promise;
    AsyncRpc first(first_promise.get_future(), [&] { ++cancelled; }, {});
    {
        AsyncRpc second(second_promise.get_future(), [&] { ++cancelled; }, {});
        first = std::move(second);
        CHECK(cancelled.load() == 2);
    }
    CHECK(cancelled.load() == 2);
}

void test_rpc_v10_frame_priority_and_variable_length() {
    CHECK(frame_type_priority(FrameType::control) < frame_type_priority(FrameType::foreground));
    CHECK(frame_type_priority(FrameType::foreground) < frame_type_priority(FrameType::read_ahead));
    CHECK(frame_type_priority(FrameType::read_ahead) <
          frame_type_priority(FrameType::speculative));
    CHECK(default_frame_type(MessageType::ping) == FrameType::control);
    CHECK(default_frame_type(MessageType::get_object) == FrameType::foreground);
    CHECK(std::string(message_type_name(MessageType::commit_metadata)) == "commit_metadata");

    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    std::mutex order_mutex;
    std::vector<uint8_t> order;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object && !request.payload.empty()) {
                std::lock_guard lock(order_mutex);
                order.push_back(request.payload.front());
            }
            return RpcMessage{MessageType::ok, request.payload};
        },
        [](const NodeInfo&) {}, 4096);
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 5s, 30s, 4096);
    Endpoint endpoint{"127.0.0.1", port};

    // A non-multiple of max_frame_size proves that the final frame is naturally
    // short rather than padded to a fixed transport block size.
    auto odd = pattern(12'345);
    auto odd_reply = client.call(endpoint, MessageType::ping, odd, 1s);
    CHECK(odd_reply.message.payload == odd);

    auto metadata_reply = client.call(endpoint, MessageType::get_metadata, Bytes{0x4d},
                                      FrameType::read_ahead, 2s);
    CHECK(metadata_reply.message.type == MessageType::ok);
    CHECK(metadata_reply.message.payload == Bytes{0x4d});

    // Start a large speculative transfer, then introduce foreground work. The
    // writer reconsiders priority after every <=4 KiB variable-length frame, so
    // foreground reaches the server before the speculative message completes.
    Bytes speculative(32 * 1024 * 1024, 0x53);
    auto background = client.call_async(endpoint, MessageType::put_object, speculative,
                                        FrameType::speculative);
    std::this_thread::sleep_for(2ms);
    auto foreground = client.call_async(endpoint, MessageType::put_object, Bytes{0x46},
                                        FrameType::foreground);
    REQUIRE(foreground.wait_for(2s) == std::future_status::ready);
    CHECK(foreground.get().message.type == MessageType::ok);
    {
        std::lock_guard lock(order_mutex);
        REQUIRE(!order.empty());
        CHECK(order.front() == 0x46);
    }
    REQUIRE(background.wait_for(10s) == std::future_status::ready);
    CHECK(background.get().message.type == MessageType::ok);
    CHECK(client.stats().canonical_connections == 2);

    // Cancellation can race with the writer while one frame is outside the
    // outbound deque. The cancelled transfer must not be requeued after that
    // frame, otherwise the peer sees continuation frames after cancel_transfer
    // discarded its assembler state and tears down the canonical connection.
    Bytes cancelled_payload(32 * 1024 * 1024, 0x43);
    auto cancelled = client.call_async(endpoint, MessageType::put_object, cancelled_payload,
                                       FrameType::speculative);
    std::this_thread::sleep_for(2ms);
    cancelled.cancel();
    auto after_cancel = client.call(endpoint, MessageType::ping, Bytes{0x50}, 2s);
    CHECK(after_cancel.message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 2);

    client.stop();
    server.stop();
}

void test_repair_step_is_bounded_and_yields() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;
    c1.maintenance.idle_bandwidth_fraction = 0.0;
    c2.maintenance.idle_bandwidth_fraction = 0.0;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    auto bytes = pattern(512 * 1024);
    auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    REQUIRE(!s2.node().local_store().has(id));
    std::vector<ObjectId> live{id};
    std::vector<ObjectId> universal{id};
    DistributedStore repair(s1.node());
    const auto full_lists_before = s1.node().local_store().full_list_scans();

    auto yielded = repair.repair_step(8ULL * 1024 * 1024, 8, &live, &universal,
                                      [] { return true; });
    CHECK(yielded.yielded);
    CHECK(!yielded.complete);
    CHECK(yielded.bytes_transferred == 0);
    CHECK(!s2.node().local_store().has(id));

    // One remote operation is enough to probe but not both probe and upload.
    // The pass must report itself incomplete rather than being mistaken for a
    // quiescent namespace simply because it transferred zero bytes.
    auto bounded = repair.repair_step(8ULL * 1024 * 1024, 1, &live, &universal);
    CHECK(!bounded.complete);
    CHECK(bounded.bytes_transferred == 0);
    CHECK(bounded.remote_operations == 1);
    CHECK(!s2.node().local_store().has(id));

    auto completed = repair.repair_step(8ULL * 1024 * 1024, 8, &live, &universal);
    CHECK(completed.bytes_transferred == bytes.size());
    CHECK(completed.complete);
    CHECK(completed.remote_operations <= 8);
    CHECK(s2.node().local_store().has(id));
    CHECK(s1.node().local_store().full_list_scans() == full_lists_before);
    CHECK(yielded.push_examined <= 64);
    CHECK(bounded.push_examined <= 64);
    CHECK(completed.push_examined <= 64);
    CHECK(yielded.pull_examined <= 64);
    CHECK(bounded.pull_examined <= 64);
    CHECK(completed.pull_examined <= 64);
    CHECK(yielded.push_examined + yielded.pull_examined <= 64);
    CHECK(bounded.push_examined + bounded.pull_examined <= 64);
    CHECK(completed.push_examined + completed.pull_examined <= 64);

    s2.stop();
    s1.stop();
}

void test_rpc_v10_persistence_and_multiplexing() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (!request.payload.empty() && request.payload.front() == 1)
                std::this_thread::sleep_for(300ms);
            return RpcMessage{MessageType::ok, request.payload};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms);
    Endpoint endpoint{"127.0.0.1", port};

    Bytes slow_payload{1};
    Bytes fast_payload{2};
    auto slow = client.call_async(endpoint, MessageType::ping, slow_payload);
    std::this_thread::sleep_for(10ms);
    auto fast = client.call_async(endpoint, MessageType::ping, fast_payload);

    REQUIRE(fast.wait_for(150ms) == std::future_status::ready);
    auto fast_reply = fast.get();
    CHECK(fast_reply.message.type == MessageType::ok);
    CHECK(fast_reply.message.payload == fast_payload);
    REQUIRE(slow.wait_for(500ms) == std::future_status::ready);
    CHECK(slow.get().message.payload == slow_payload);

    // Empty request immediately follows prior frames on the same channel. This
    // would desynchronise the old GCM framing bug.
    auto empty_reply = client.call(endpoint, MessageType::ping, {}, 1s);
    CHECK(empty_reply.message.type == MessageType::ok);
    CHECK(empty_reply.message.payload.empty());

    auto stats = client.stats();
    CHECK(stats.connections_created == 1);
    CHECK(stats.connections_reused >= 2);

    // The argument to synchronous call() is now a stall-observation interval,
    // not a request deadline. A healthy RPC may take arbitrarily longer than
    // that interval and must still complete without its connection being torn
    // down merely because wall-clock time elapsed.
    Bytes very_slow_payload{1};
    auto started = Clock::now();
    auto very_slow = client.call(endpoint, MessageType::ping, very_slow_payload, 50ms);
    CHECK(very_slow.message.type == MessageType::ok);
    CHECK(very_slow.message.payload == very_slow_payload);
    CHECK(Clock::now() - started >= 250ms);

    // The same persistent connection remains usable after a slow request; there is no
    // timeout-induced backoff/reconnect cycle.
    auto recovered = client.call(endpoint, MessageType::ping, fast_payload, 50ms);
    CHECK(recovered.message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 1);

    client.stop();
    server.stop();
}

void test_rpc_v10_bidirectional_and_deduplication() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    struct TestNode {
        NodeInfo info;
        RpcClient client;
        RpcServer server;

        TestNode(ClusterKeys keys, NodeInfo node, RpcServer::Handler handler)
            : info(std::move(node)),
              client(keys, [this] { return info; }, [](const NodeInfo&) {}, [](uint64_t) {},
                     500ms, 10s, 30s),
              server("127.0.0.1", info.port, keys, info, std::move(handler),
                     [](const NodeInfo&) {}) {
            server.attach_client(client);
            server.start();
        }
        ~TestNode() {
            server.stop();
            client.stop();
        }
    };

    auto node_info = [] {
        NodeInfo node;
        node.id = random_node_id();
        node.host = "127.0.0.1";
        node.port = free_port();
        node.failure_domain = "rpc-test";
        return node;
    };
    auto echo = [](const NodeInfo&, FrameType, const RpcMessage& request) {
        return RpcMessage{MessageType::ok, request.payload};
    };

    // Once A has called B, B can originate an RPC back over the accepted socket.
    // It must not create a second B->A TCP connection.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        CHECK(a.client.call(b.info, MessageType::members, Bytes{1}, 1s).message.payload == Bytes{1});
        CHECK(b.client.stats().connections_created == 0);
        CHECK(b.client.call(a.info, MessageType::members, Bytes{2}, 1s).message.payload == Bytes{2});
        CHECK(b.client.stats().connections_created == 0);

        // DATA is a second independently canonical bidirectional lane. A opens
        // it lazily; B must reuse the accepted data session rather than dial a
        // third physical connection back to A.
        CHECK(a.client.call(b.info, MessageType::put_object, Bytes{3},
                            FrameType::foreground, 1s).message.payload == Bytes{3});
        CHECK(b.client.call(a.info, MessageType::get_object, Bytes{4},
                            FrameType::foreground, 1s).message.payload == Bytes{4});
        CHECK(b.client.stats().connections_created == 0);
        CHECK(a.client.stats().canonical_connections == 2);
        CHECK(b.client.stats().canonical_connections == 2);
    }

    // Simultaneous cross-dial starts with two physical sessions. Both nodes
    // must select the same winner and all later RPCs must reuse it.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        std::atomic_int ready{};
        std::exception_ptr a_error;
        std::exception_ptr b_error;
        std::jthread a_call([&] {
            try {
                ++ready;
                while (ready.load() < 2)
                    std::this_thread::yield();
                auto reply = a.client.call(b.info, MessageType::members, Bytes{3}, 1s);
                if (reply.message.payload != Bytes{3})
                    throw std::runtime_error("bad A cross-dial reply");
            } catch (...) {
                a_error = std::current_exception();
            }
        });
        std::jthread b_call([&] {
            try {
                ++ready;
                while (ready.load() < 2)
                    std::this_thread::yield();
                auto reply = b.client.call(a.info, MessageType::members, Bytes{4}, 1s);
                if (reply.message.payload != Bytes{4})
                    throw std::runtime_error("bad B cross-dial reply");
            } catch (...) {
                b_error = std::current_exception();
            }
        });
        a_call.join();
        b_call.join();
        if (a_error)
            std::rethrow_exception(a_error);
        if (b_error)
            std::rethrow_exception(b_error);

        REQUIRE(wait_until([&] {
            return a.client.stats().canonical_connections == 1 &&
                   b.client.stats().canonical_connections == 1;
        }));
        auto a_created = a.client.stats().connections_created;
        auto b_created = b.client.stats().connections_created;
        CHECK(a.client.call(b.info, MessageType::members, Bytes{5}, 1s).message.payload == Bytes{5});
        CHECK(b.client.call(a.info, MessageType::members, Bytes{6}, 1s).message.payload == Bytes{6});
        CHECK(a.client.stats().connections_created == a_created);
        CHECK(b.client.stats().connections_created == b_created);
    }

    // Endpoint spelling is not peer identity. A hostname alias can cause a
    // transient second dial, but authenticated NodeId dedup leaves one route.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        Endpoint numeric{"127.0.0.1", b.info.port};
        Endpoint alias{"localhost", b.info.port};
        CHECK(a.client.call(numeric, MessageType::members, Bytes{7}, 1s).message.payload == Bytes{7});
        CHECK(a.client.call(alias, MessageType::members, Bytes{8}, 1s).message.payload == Bytes{8});
        REQUIRE(wait_until([&] { return a.client.stats().canonical_connections == 1; }));
        auto created = a.client.stats().connections_created;
        CHECK(a.client.call(alias, MessageType::members, Bytes{9}, 1s).message.payload == Bytes{9});
        CHECK(a.client.stats().connections_created == created);
    }

    // A failed dial puts the endpoint into retry backoff, but that backoff must
    // not mask a canonical route which arrives inbound immediately afterwards.
    // This is the normal recovery shape when a peer reconnects while the other
    // side is still remembering the failed outbound attempt.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        Endpoint b_endpoint{"127.0.0.1", b.info.port};

        b.server.stop();
        bool failed = false;
        try {
            (void)a.client.call(b_endpoint, MessageType::members, Bytes{10}, 1s);
        } catch (...) {
            failed = true;
        }
        REQUIRE(failed);

        b.server.attach_client(b.client);
        b.server.start();
        CHECK(b.client.call(a.info, MessageType::members, Bytes{11}, 1s).message.payload ==
              Bytes{11});
        REQUIRE(wait_until([&] { return a.client.stats().canonical_connections == 1; }));

        // Still inside the failed dial's minimum 250-ms retry-backoff window.
        // Endpoint lookup must reuse the authenticated inbound route before
        // consulting dial backoff.
        CHECK(a.client.call(b_endpoint, MessageType::members, Bytes{12}, 1s).message.payload ==
              Bytes{12});
    }

    // Retirement is a drain, not a reset. Force the higher NodeId to have a
    // slow request outstanding on the connection which cross-dial arbitration
    // will discard, then create the canonical lower->higher connection.
    {
        auto first = node_info();
        auto second = node_info();
        auto lower_info = first.id < second.id ? first : second;
        auto higher_info = first.id < second.id ? second : first;
        std::atomic_bool entered{};
        auto lower_handler = [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (!request.payload.empty() && request.payload.front() == 42) {
                entered = true;
                std::this_thread::sleep_for(300ms);
            }
            return RpcMessage{MessageType::ok, request.payload};
        };
        TestNode lower(keys, lower_info, lower_handler);
        TestNode higher(keys, higher_info, echo);

        auto slow = higher.client.call_async(lower.info, MessageType::members, Bytes{42});
        REQUIRE(wait_until([&] { return entered.load(); }, 1s));
        CHECK(lower.client.call(higher.info, MessageType::members, Bytes{43}, 1s).message.payload ==
              Bytes{43});
        REQUIRE(slow.wait_for(1s) == std::future_status::ready);
        CHECK(slow.get().message.payload == Bytes{42});
        REQUIRE(wait_until([&] {
            return lower.client.stats().canonical_connections == 1 &&
                   higher.client.stats().canonical_connections == 1;
        }));
        auto created = higher.client.stats().connections_created;
        CHECK(higher.client.call(lower.info, MessageType::members, Bytes{44}, 1s).message.payload ==
              Bytes{44});
        CHECK(higher.client.stats().connections_created == created);
    }
}

void test_mutual_bootstrap_prunes_cross_dial() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();

    REQUIRE(wait_until([&] {
        const auto a = n1.rpc_stats();
        const auto b = n2.rpc_stats();
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2 &&
               a.canonical_connections == 1 && b.canonical_connections == 1;
    }));

    const auto a_before = n1.rpc_stats();
    const auto b_before = n2.rpc_stats();
    std::this_thread::sleep_for(500ms); // several configured heartbeats
    const auto a_after = n1.rpc_stats();
    const auto b_after = n2.rpc_stats();

    CHECK(a_after.canonical_connections == 1);
    CHECK(b_after.canonical_connections == 1);
    CHECK(a_after.connections_created == a_before.connections_created);
    CHECK(b_after.connections_created == b_before.connections_created);

    n2.stop();
    n1.stop();
}

void test_rpc_v7_handshake_is_rejected() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server";
    RpcServer server("127.0.0.1", port, keys, server_info,
                     [](const NodeInfo&, FrameType, const RpcMessage&) {
                         return RpcMessage{MessageType::ok, {}};
                     },
                     [](const NodeInfo&) {});
    server.start();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    NodeInfo old_client;
    old_client.id = random_node_id();
    old_client.host = "127.0.0.1";
    old_client.port = free_port();
    old_client.failure_domain = "old-client";
    auto ephemeral = x25519_generate();
    auto nonce_bytes = random_bytes(32);
    std::array<uint8_t, 32> nonce{};
    std::copy(nonce_bytes.begin(), nonce_bytes.end(), nonce.begin());

    Writer hello_writer;
    hello_writer.u16(7);
    hello_writer.u32(256 * 1024); // v7 had no transport-lane byte.
    hello_writer.fixed(keys.cluster_id);
    hello_writer.fixed(old_client.id.bytes);
    hello_writer.fixed(nonce);
    hello_writer.fixed(ephemeral.public_key);
    encode_node_info(hello_writer, old_client);
    auto hello = hello_writer.take();

    Bytes authenticated(reinterpret_cast<const uint8_t*>("client/v7"),
                        reinterpret_cast<const uint8_t*>("client/v7") + 9);
    authenticated.insert(authenticated.end(), hello.begin(), hello.end());
    Writer envelope;
    envelope.bytes(hello);
    envelope.fixed(hmac_sha256(keys.auth, authenticated));
    Writer framed;
    framed.u32(static_cast<uint32_t>(envelope.data().size()));
    framed.raw(envelope.data());

    size_t sent = 0;
    while (sent < framed.data().size()) {
        auto count = send(fd, framed.data().data() + sent, framed.data().size() - sent, 0);
        REQUIRE(count > 0);
        sent += static_cast<size_t>(count);
    }

    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t byte{};
    CHECK(recv(fd, &byte, 1, 0) == 0);
    close(fd);
    OPENSSL_cleanse(ephemeral.private_key.data(), ephemeral.private_key.size());
    server.stop();
}

void test_rpc_slow_control_does_not_abort_data() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object) {
                std::this_thread::sleep_for(600ms);
                return RpcMessage{MessageType::ok, {}};
            }
            if (request.type == MessageType::members) {
                std::this_thread::sleep_for(600ms);
                return RpcMessage{MessageType::ok, request.payload};
            }
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 50ms, 200ms);
    Endpoint endpoint{"127.0.0.1", port};

    // The 50-ms value below is not a deadline: both 600-ms RPCs are healthy and
    // must complete without the peer transport being destroyed. They also
    // outlive the 200-ms peer-death window while priority control pings on the independent control
    // connection prove that the peer itself remains alive.
    Bytes object_payload(256 * 1024, 0x5a);
    auto data = client.call_async(endpoint, MessageType::put_object, object_payload);
    std::this_thread::sleep_for(20ms);

    auto started = Clock::now();
    auto control = client.call(endpoint, MessageType::members, Bytes{1}, 50ms);
    CHECK(control.message.type == MessageType::ok);
    CHECK(Clock::now() - started >= 550ms);

    REQUIRE(data.wait_for(2s) == std::future_status::ready);
    CHECK(data.get().message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 2);

    client.stop();
    server.stop();
}

void test_rpc_health_and_control_not_starved_by_data() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object) {
                std::this_thread::sleep_for(400ms);
                return RpcMessage{MessageType::ok, {}};
            }
            if (request.type == MessageType::members)
                return RpcMessage{MessageType::ok, {}};
            if (request.type == MessageType::ping)
                return RpcMessage{MessageType::ok, {}};
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Occupy every data worker. Health and membership use a separate control TCP
    // stream and must remain prompt regardless of data-lane work.
    std::vector<AsyncRpc> bulk;
    for (int i = 0; i < 8; ++i)
        bulk.push_back(client.call_async(endpoint, MessageType::put_object, Bytes{0x5a},
                                         FrameType::speculative));
    std::this_thread::sleep_for(50ms);

    auto started = Clock::now();
    auto health = client.call(endpoint, MessageType::ping, {}, 20ms);
    CHECK(health.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 150ms);

    started = Clock::now();
    auto control = client.call(endpoint, MessageType::members, {}, 20ms);
    CHECK(control.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 150ms);

    CHECK(client.stats().canonical_connections == 2);

    for (auto& rpc : bulk) {
        REQUIRE(rpc.wait_for(1s) == std::future_status::ready);
        CHECK(rpc.get().message.type == MessageType::ok);
    }

    client.stop();
    server.stop();
}

void test_early_replication_quorum() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto p1 = free_port();
    auto pf = free_port();
    auto ps = free_port();

    // This test is specifically about object PUT quorum latency. Keep metadata
    // consensus out of the test and use two deterministic RPC peers rather than
    // relying on service discovery/background maintenance to make the fast peer
    // reachable. That removes a platform/timing dependency which made this test
    // intermittently (and on macOS, consistently) fail before the assertion it
    // was intended to exercise.
    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    c1.dead_after = 3s;
    c1.metadata_replication = 1;
    // Keep the node's background membership exchange out of this latency test.
    // The peers are injected directly below; the test should measure object
    // quorum completion, not race a 100ms control-plane scheduler.
    c1.heartbeat = 10s;
    Service s1(c1, keys);
    s1.start();

    NodeInfo fast_info;
    fast_info.id = random_node_id();
    fast_info.host = "127.0.0.1";
    fast_info.port = pf;
    fast_info.failure_domain = "fast-site";
    fast_info.capacity = 1024ULL * 1024 * 1024;
    fast_info.seen_unix_ms = unix_ms();

    NodeInfo slow_info;
    slow_info.id = random_node_id();
    slow_info.host = "127.0.0.1";
    slow_info.port = ps;
    slow_info.failure_domain = "slow-site";
    slow_info.capacity = 1024ULL * 1024 * 1024;
    slow_info.seen_unix_ms = unix_ms();

    auto peer_handler = [](const NodeInfo& self, std::chrono::milliseconds put_delay) {
        return [self, put_delay](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object) {
                if (put_delay.count())
                    std::this_thread::sleep_for(put_delay);
                return RpcMessage{MessageType::ok, {}};
            }
            if (request.type == MessageType::members) {
                Writer writer;
                writer.u32(1);
                encode_node_info(writer, self);
                return RpcMessage{MessageType::members_reply, writer.take()};
            }
            return RpcMessage{MessageType::ok, {}};
        };
    };

    RpcServer fast_server("127.0.0.1", pf, keys, fast_info,
                          peer_handler(fast_info, 0ms), [](const NodeInfo&) {});
    RpcServer slow_server("127.0.0.1", ps, keys, slow_info,
                          peer_handler(slow_info, 800ms), [](const NodeInfo&) {});
    fast_server.start();
    slow_server.start();

    s1.node().membership().observe(fast_info, true);
    s1.node().membership().observe(slow_info, true);
    REQUIRE(s1.node().membership().active().size() == 3);

    DistributedStore store(s1.node());
    auto data = pattern(256 * 1024);
    auto started = Clock::now();
    REQUIRE(store.put(data) == object_id(data));
    auto elapsed = Clock::now() - started;
    CHECK(elapsed < 500ms);

    slow_server.stop();
    fast_server.stop();
    s1.stop();
}

void test_joiner_cannot_form_genesis() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();
    auto config = config_for(t.path() / "joiner", keyfile, port, {{"127.0.0.1", port}});
    config.replication = 1;
    config.metadata_replication = 1;

    Service service(config, keys);
    service.start();
    bool rejected = false;
    try {
        (void)service.filesystem().getattr("/");
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find("waiting for bootstrap peer") != std::string::npos;
    }
    CHECK(rejected);
    service.stop();
}


void test_two_node_mutual_bootstrap_metadata_quorum() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.metadata_replication = c2.metadata_replication = 2;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    // Both peers may attempt genesis simultaneously after symmetric discovery.
    // Competing generation-2 proposals must converge on one metadata history.
    std::exception_ptr first_error;
    std::exception_ptr second_error;
    std::atomic<unsigned> ready{};
    std::atomic<bool> go{};
    std::thread first([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            s1.filesystem().mkdir("/from-node-1", 0755, getuid(), getgid());
        } catch (...) {
            first_error = std::current_exception();
        }
    });
    std::thread second([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            s2.filesystem().mkdir("/from-node-2", 0755, getuid(), getgid());
        } catch (...) {
            second_error = std::current_exception();
        }
    });
    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    if (first_error)
        std::rethrow_exception(first_error);
    if (second_error)
        std::rethrow_exception(second_error);

    CHECK(s1.filesystem().getattr("/from-node-2").type == EntryType::directory);
    CHECK(s2.filesystem().getattr("/from-node-1").type == EntryType::directory);

    s1.filesystem().mkdir("/media", 0755, getuid(), getgid());
    s1.filesystem().create_file("/media/two-replicas.bin", 0644, getuid(), getgid());
    auto input = pattern(128 * 1024);
    auto writer = s1.filesystem().open_write("/media/two-replicas.bin", true);
    REQUIRE(writer->write(0, input) == input.size());
    writer->commit();

    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/media/two-replicas.bin").size == input.size();
        } catch (...) {
            return false;
        }
    }));

    MetadataManager m1(s1.node());
    auto snapshot = m1.snapshot();
    CHECK(snapshot.metadata_voters.size() == 2);
    CHECK(snapshot.data_replication == 2);

    auto entry = s1.filesystem().getattr("/media/two-replicas.bin");
    REQUIRE(entry.extents.size() == 1);
    CHECK(s1.node().local_store().has(entry.extents.front().id));
    CHECK(s2.node().local_store().has(entry.extents.front().id));

    s2.stop();
    s1.stop();
}

void test_replication_policy_change_on_restart() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;

    ObjectId object;
    Bytes input = pattern(128 * 1024);
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        s1.filesystem().create_file("/policy.bin", 0644, getuid(), getgid());
        auto writer = s1.filesystem().open_write("/policy.bin", true);
        REQUIRE(writer->write(0, input) == input.size());
        writer->commit();
        auto entry = s1.filesystem().getattr("/policy.bin");
        REQUIRE(entry.extents.size() == 1);
        object = entry.extents.front().id;
        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/policy.bin").size == input.size();
            } catch (...) {
                return false;
            }
        }));
        s2.stop();
        s1.stop();
    }

    // Replica policy is deliberately changed only while the whole cluster is
    // stopped. On restart the old metadata quorum commits the new policy and
    // object repair converges existing content to the new data replica count.
    c1.replication = c2.replication = 2;
    c1.metadata_replication = c2.metadata_replication = 2;
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        s1.filesystem().mkdir("/after-grow", 0755, getuid(), getgid());
        MetadataManager m1(s1.node());
        auto snapshot = m1.snapshot();
        CHECK(snapshot.metadata_voters.size() == 2);
        CHECK(snapshot.data_replication == 2);
        CHECK(s2.filesystem().getattr("/after-grow").type == EntryType::directory);

        DistributedStore r1(s1.node());
        DistributedStore r2(s2.node());
        REQUIRE(wait_until([&] {
            r1.repair_once(1024 * 1024);
            r2.repair_once(1024 * 1024);
            return s1.node().local_store().has(object) && s2.node().local_store().has(object);
        }));

        s2.stop();
        s1.stop();
    }

    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        s2.filesystem().mkdir("/after-shrink", 0755, getuid(), getgid());
        MetadataManager m2(s2.node());
        auto snapshot = m2.snapshot();
        CHECK(snapshot.metadata_voters.size() == 1);
        CHECK(snapshot.data_replication == 1);
        CHECK(s1.filesystem().getattr("/after-shrink").type == EntryType::directory);

        auto reader = s2.filesystem().open_read("/policy.bin");
        Bytes output(input.size());
        REQUIRE(reader->read(0, output) == output.size());
        CHECK(output == input);

        s2.stop();
        s1.stop();
    }
}

void test_genesis_root_configuration() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "single", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.filesystem.root_uid = 501;
    config.filesystem.root_gid = 20;
    config.filesystem.root_mode = 0750;

    Service service(config, keys);
    service.start();
    auto root = service.filesystem().getattr("/");
    CHECK(root.type == EntryType::directory);
    CHECK(root.uid == 501);
    CHECK(root.gid == 20);
    CHECK(root.mode == 0750);
    CHECK(root.ctime_ns > 0);
    CHECK(root.mtime_ns > 0);
    service.stop();
}

void test_open_write_metadata_merge() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "single-write", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;

    Service service(config, keys);
    service.start();

    service.filesystem().create_file("/copy.mkv", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/copy.mkv", true);
    auto input = pattern(3 * config.extent_size + 12345);
    size_t offset = 0;
    while (offset < input.size()) {
        size_t n = std::min<size_t>(4096, input.size() - offset);
        REQUIRE(writer->write(offset, {input.data() + offset, n}) == n);
        offset += n;
    }

    // macOS copyfile/cp can apply mode/ownership/timestamps through the still-open
    // file descriptor before FUSE flush/release publishes the data manifest.
    // These are metadata-only changes and must not invalidate the writer.
    const int64_t preserved_mtime = 1700000000123456789LL;
    service.filesystem().chmod("/copy.mkv", 0600);
    service.filesystem().chown("/copy.mkv", getuid(), getgid(), true, true);
    service.filesystem().utimens("/copy.mkv", preserved_mtime);
    writer->commit();

    auto entry = service.filesystem().getattr("/copy.mkv");
    CHECK(entry.size == input.size());
    CHECK(entry.mode == 0600);
    CHECK(entry.uid == static_cast<uint32_t>(getuid()));
    CHECK(entry.gid == static_cast<uint32_t>(getgid()));
    CHECK(entry.mtime_ns == preserved_mtime);

    auto reader = service.filesystem().open_read("/copy.mkv");
    Bytes output(input.size());
    size_t got = 0;
    while (got < output.size()) {
        auto n = reader->read(got, {output.data() + got, output.size() - got});
        REQUIRE(n > 0);
        got += n;
    }
    CHECK(output == input);

    // A real concurrent content update is still a conflict.
    service.filesystem().create_file("/conflict.bin", 0644, getuid(), getgid());
    auto first = service.filesystem().open_write("/conflict.bin", true);
    auto second = service.filesystem().open_write("/conflict.bin", true);
    auto a = pattern(8192);
    auto b = pattern(8193);
    REQUIRE(first->write(0, a) == a.size());
    REQUIRE(second->write(0, b) == b.size());
    first->commit();
    bool conflicted = false;
    try {
        second->commit();
    } catch (const FsError& e) {
        conflicted = e.code() == EAGAIN;
    }
    CHECK(conflicted);

    service.stop();
}

void test_active_write_size_visibility() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "active-size", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;

    Service service(config, keys);
    service.start();

    service.filesystem().create_file("/.active.tmp", 0600, getuid(), getgid());
    CHECK(!service.filesystem().active_write_size("/.active.tmp").has_value());

    auto writer = service.filesystem().open_write("/.active.tmp", true);
    auto input = pattern(2 * 1024 * 1024 + 12345);
    REQUIRE(writer->write(0, input) == input.size());

    // Authoritative metadata remains uncommitted until close, but FUSE must be
    // able to project the live writer size to the kernel while the handle is open.
    CHECK(service.filesystem().getattr("/.active.tmp").size == 0);
    auto active = service.filesystem().active_write_size("/.active.tmp");
    REQUIRE(active.has_value());
    CHECK(*active == input.size());

    writer->truncate(65536);
    active = service.filesystem().active_write_size("/.active.tmp");
    REQUIRE(active.has_value());
    CHECK(*active == 65536);

    service.filesystem().rename("/.active.tmp", "/active.bin");
    CHECK(!service.filesystem().active_write_size("/.active.tmp").has_value());
    active = service.filesystem().active_write_size("/active.bin");
    REQUIRE(active.has_value());
    CHECK(*active == 65536);

    writer->commit();
    CHECK(service.filesystem().getattr("/active.bin").size == 65536);
    writer.reset();
    CHECK(!service.filesystem().active_write_size("/active.bin").has_value());

    service.stop();
}

void test_open_write_survives_rename() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "single-rename", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;

    Service service(config, keys);
    service.start();

    service.filesystem().create_file("/.upload.tmp", 0600, getuid(), getgid());
    auto writer = service.filesystem().open_write("/.upload.tmp", true);
    auto input = pattern(3 * config.extent_size + 12345);
    size_t offset = 0;
    while (offset < input.size()) {
        size_t n = std::min<size_t>(128 * 1024, input.size() - offset);
        REQUIRE(writer->write(offset, {input.data() + offset, n}) == n);
        offset += n;
    }

    // rsync writes a temporary file, renames it to the destination while the
    // descriptor is still open, then flushes/closes that same descriptor.
    service.filesystem().rename("/.upload.tmp", "/movie.mkv");
    writer->commit();

    bool old_missing = false;
    try {
        (void)service.filesystem().getattr("/.upload.tmp");
    } catch (const FsError& e) {
        old_missing = e.code() == ENOENT;
    }
    CHECK(old_missing);

    auto entry = service.filesystem().getattr("/movie.mkv");
    CHECK(entry.size == input.size());

    auto reader = service.filesystem().open_read("/movie.mkv");
    Bytes output(input.size());
    size_t got = 0;
    while (got < output.size()) {
        auto n = reader->read(got, {output.data() + got, output.size() - got});
        REQUIRE(n > 0);
        got += n;
    }
    CHECK(output == input);

    service.stop();
}

void test_full_replica_fallback() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();
    uint16_t p3 = free_port();
    uint16_t p4 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    auto c3 = config_for(t.path() / "n3", keyfile, p3, {{"127.0.0.1", p1}});
    auto c4 = config_for(t.path() / "n4", keyfile, p4, {{"127.0.0.1", p1}});
    // Equal configured capacities give each node an equal placement share.
    // Node 1 is then filled locally: current free space must not change its
    // placement weight, and the missing replica should spill to the fallback.
    c1.storage_backends.front().limit = 2ULL * 1024 * 1024;
    c2.storage_backends.front().limit = 2ULL * 1024 * 1024;
    c3.storage_backends.front().limit = 2ULL * 1024 * 1024;
    c4.storage_backends.front().limit = 2ULL * 1024 * 1024;

    Service s1(c1, keys);
    Service s2(c2, keys);
    Service s3(c3, keys);
    Service s4(c4, keys);
    s1.start();
    s2.start();
    s3.start();
    s4.start();
    REQUIRE(wait_until([&] { return s2.node().membership().active().size() >= 4; }));

    auto filler = pattern(1800 * 1024);
    filler[0] ^= 0xa5;
    REQUIRE(s1.node().local_store().put(object_id(filler), filler));

    Bytes data;
    std::vector<NodeInfo> ranked;
    for (uint32_t salt = 0; salt < 1000; ++salt) {
        data = pattern(256 * 1024 + salt);
        auto id = object_id(data);
        auto active = s2.node().membership().active();
        ranked = capacity_placement_nodes(id.bytes, active, c2.replication);
        if (ranked.size() == 4 &&
            std::find_if(ranked.begin(), ranked.begin() + 3, [&](const NodeInfo& n) {
                return n.id == s1.node().node_id();
            }) != ranked.begin() + 3) {
            break;
        }
        ranked.clear();
    }
    REQUIRE(ranked.size() == 4);

    auto id = object_id(data);
    DistributedStore store(s2.node());
    REQUIRE(store.put(id, data));
    CHECK(!s1.node().local_store().has(id));

    std::array<Service*, 4> services{&s1, &s2, &s3, &s4};
    auto fallback = std::find_if(services.begin(), services.end(), [&](Service* service) {
        return service->node().node_id() == ranked[3].id;
    });
    REQUIRE(fallback != services.end());

    // Foreground put() commits at quorum and does not wait for the full third
    // owner. Placement repair subsequently spills that missing replica to the
    // next deterministic capacity-aware fallback node.
    REQUIRE(wait_until([&] {
        for (auto* service : services) {
            if (!service->node().local_store().has(id))
                continue;
            DistributedStore repair(service->node());
            repair.repair_once(8ULL * 1024 * 1024);
        }
        return (*fallback)->node().local_store().has(id);
    }));

    size_t copies = 0;
    for (auto* service : services)
        copies += service->node().local_store().has(id) ? 1 : 0;
    CHECK(copies == 3);

    s4.stop();
    s3.stop();
    s2.stop();
    s1.stop();
}


void test_replacement_node_recovers_namespace_and_replication() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.metadata_replication = c2.metadata_replication = 1;

    std::vector<ObjectId> objects;
    Bytes input = pattern(2 * 1024 * 1024 + 12345);
    NodeId old_n1;

    auto s1 = std::make_unique<Service>(c1, keys);
    auto s2 = std::make_unique<Service>(c2, keys);
    s1->start();
    s2->start();
    REQUIRE(wait_until([&] {
        return s1->node().membership().active().size() >= 2 &&
               s2->node().membership().active().size() >= 2;
    }));

    s1->filesystem().mkdir("/media", 0755, getuid(), getgid());
    s1->filesystem().create_file("/media/recovery.bin", 0644, getuid(), getgid());
    auto writer = s1->filesystem().open_write("/media/recovery.bin", true);
    REQUIRE(writer->write(0, input) == input.size());
    writer->commit();
    auto entry = s1->filesystem().getattr("/media/recovery.bin");
    REQUIRE(!entry.extents.empty());
    for (const auto& extent : entry.extents) {
        if (!extent.hole)
            objects.push_back(extent.id);
    }
    REQUIRE(!objects.empty());

    REQUIRE(wait_until([&] {
        try {
            return s2->filesystem().getattr("/media/recovery.bin").size == input.size();
        } catch (...) {
            return false;
        }
    }));
    REQUIRE(wait_until([&] {
        return std::all_of(objects.begin(), objects.end(),
                           [&](const auto& id) { return s2->node().local_store().has(id); });
    }));

    // Force one normal metadata maintenance pass so node 2 is demonstrably a
    // durable committed-checkpoint witness before the voter is destroyed.
    MetadataManager witness_repair(s2->node());
    witness_repair.repair_once();
    CHECK(s2->node().metadata_replica().committed().generation > 1);

    old_n1 = s1->node().node_id();
    s1->stop();
    s1.reset();

    // Simulate complete loss of node 1: identity, namespace state, cache and
    // every authoritative object are gone. The shared cluster key/config remain.
    std::error_code ec;
    std::filesystem::remove_all(c1.state_path, ec);
    std::filesystem::remove_all(c1.storage_backends.front().path, ec);
    if (!c1.cache.path.empty())
        std::filesystem::remove_all(c1.cache.path, ec);
    std::filesystem::create_directories(c1.storage_backends.front().path);

    // A wiped founder cannot discover the survivor unless it is explicitly
    // given a bootstrap route. This is the same replacement-node operation a
    // real deployment performs after losing local state.
    auto replacement_config = c1;
    replacement_config.bootstrap = {{"127.0.0.1", p2}};
    auto replacement = std::make_unique<Service>(replacement_config, keys);
    replacement->start();
    CHECK(replacement->node().node_id() != old_n1);

    REQUIRE(wait_until([&] {
        try {
            return replacement->filesystem().getattr("/media/recovery.bin").size == input.size();
        } catch (...) {
            return false;
        }
    }, 10s));

    // Once the committed namespace is recovered, the existing live-object
    // maintenance walk must automatically repopulate the replacement's DHT
    // ownership from the surviving node.
    REQUIRE(wait_until([&] {
        return std::all_of(objects.begin(), objects.end(), [&](const auto& id) {
            return replacement->node().local_store().has(id);
        });
    }, 10s));

    Bytes output(input.size());
    auto reader = replacement->filesystem().open_read("/media/recovery.bin");
    size_t offset = 0;
    while (offset < output.size()) {
        auto n = reader->read(offset, {output.data() + offset, output.size() - offset});
        REQUIRE(n > 0);
        offset += n;
    }
    CHECK(output == input);

    // Recovery also reconstructs a writable metadata voter group; it is not a
    // read-only salvage mode.
    replacement->filesystem().mkdir("/after-replacement", 0755, getuid(), getgid());
    REQUIRE(wait_until(
        [&] {
            try {
                return s2->filesystem().getattr("/after-replacement").type ==
                       EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        200ms));

    replacement->stop();
    s2->stop();
}

void test_hydration_scheduler_and_prediction() {
    auto make_id = [](uint8_t value) {
        Bytes bytes(32, value);
        return object_id(bytes);
    };

    // Read-ahead and current-file hints reinforce the same ordered run, but
    // weighted virtual time must still service a lower-priority next-file run.
    auto a = make_id(1), b = make_id(2), c = make_id(3), d = make_id(4), e = make_id(5),
         f = make_id(6), n0 = make_id(7), n1 = make_id(8), n2 = make_id(9);
    std::vector<HydrationHint> hints{
        {"current", {a, b}, 1000, "read_ahead"},
        {"current", {a, b, c, d, e, f}, 700, "current_file"},
        {"next", {n0, n1, n2}, 300, "next_episode"},
    };
    HydrationScheduler scheduler;
    std::set<ObjectId> present;
    std::vector<HydrationRequest> requests;
    for (size_t i = 0; i < 7; ++i) {
        auto request = scheduler.next(hints, [&](const ObjectId& id) { return present.contains(id); });
        REQUIRE(request.has_value());
        requests.push_back(*request);
        present.insert(request->object);
    }
    CHECK(requests[0].object == a);
    CHECK(requests[0].priority == 1700);
    CHECK(requests[1].object == b);
    CHECK(requests[2].object == c);
    CHECK(requests[3].object == n0); // interleaved before the current file completes.
    auto n0_at = std::find_if(requests.begin(), requests.end(), [&](const auto& r) { return r.object == n0; });
    auto n1_at = std::find_if(requests.begin(), requests.end(), [&](const auto& r) { return r.object == n1; });
    REQUIRE(n0_at != requests.end());
    REQUIRE(n1_at != requests.end());
    CHECK(n0_at < n1_at); // an ordered speculative run can never start in its middle.

    // A blocked prefix blocks the rest of that run rather than skipping ahead.
    scheduler.reset();
    present.clear();
    auto blocked = [&](const ObjectId& id) { return id == n0; };
    auto blocked_request = scheduler.next({{"next", {n0, n1, n2}, 300, "next_episode"}},
                                          [&](const ObjectId& id) { return present.contains(id); },
                                          blocked);
    CHECK(!blocked_request.has_value());

    PlaybackTracker tracker;
    FsEntry synthetic;
    synthetic.type = EntryType::file;
    synthetic.size = 6;
    for (size_t i = 0; i < 6; ++i)
        synthetic.extents.push_back({i, 1, make_id(static_cast<uint8_t>(20 + i)), false});
    auto session = tracker.open("/synthetic.mkv", synthetic);
    tracker.progress(session, 1);
    HydrationConfig hc;
    ReadAheadHintProvider ahead(tracker, hc, 2);
    CurrentFileHintProvider tail(tracker, hc);
    auto ahead_hints = ahead.hints();
    auto tail_hints = tail.hints();
    REQUIRE(ahead_hints.size() == 1);
    REQUIRE(tail_hints.size() == 1);
    CHECK(ahead_hints[0].objects.size() == 2);
    CHECK(ahead_hints[0].objects[0] == synthetic.extents[2].id);
    CHECK(ahead_hints[0].objects[1] == synthetic.extents[3].id);
    CHECK(tail_hints[0].objects.size() == 4);
    CHECK(tail_hints[0].objects.front() == synthetic.extents[2].id);
    CHECK(tail_hints[0].objects.back() == synthetic.extents[5].id);
    tracker.close(session);

    auto renamed = synthetic;
    renamed.mode = 0600;
    renamed.uid = 1234;
    renamed.gid = 5678;
    renamed.mtime_ns = 999;
    CHECK(file_media_id(synthetic) == file_media_id(renamed));

    // Catalogue prediction is resolved against actual Macha file manifests. It
    // advances within a season, crosses into the next season, and advances a
    // movie collection; every predicted run begins at extent zero.
    TempDir temp;
    auto keyfile = temp.path() / "key";
    write_key(keyfile);
    auto config = config_for(temp.path() / "store", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.hydration.enabled = false;
    auto keys = load_cluster_keys(keyfile);
    Service service(config, keys);
    service.start();
    service.filesystem().mkdir("/TV", 0755, getuid(), getgid());
    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());

    auto make_file = [&](const std::string& path, uint8_t value) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto data = Bytes(2 * 1024 * 1024 + 12345, value);
        auto writer = service.filesystem().open_write(path, true);
        REQUIRE(writer->write(0, data) == data.size());
        writer->commit();
        auto entry = service.filesystem().getattr(path);
        REQUIRE(entry.extents.size() >= 3);
        return entry;
    };

    auto ep1_file = make_file("/TV/s01e01.mkv", 31);
    auto ep2_file = make_file("/TV/s01e02.mkv", 32);
    auto ep3_file = make_file("/TV/s02e01.mkv", 33);
    auto movie1_file = make_file("/Movies/one.mkv", 41);
    auto movie2_file = make_file("/Movies/two.mkv", 42);

    CatalogueItem show;
    show.id = "show:test";
    show.kind = CatalogueKind::show;
    show.title = "Test Show";
    show = service.catalogue().upsert(show);

    CatalogueItem season1;
    season1.id = "season:test:1";
    season1.kind = CatalogueKind::season;
    season1.title = "Season 1";
    season1.parent_id = show.id;
    season1.season_number = 1;
    season1 = service.catalogue().upsert(season1);

    CatalogueItem season2;
    season2.id = "season:test:2";
    season2.kind = CatalogueKind::season;
    season2.title = "Season 2";
    season2.parent_id = show.id;
    season2.season_number = 2;
    season2 = service.catalogue().upsert(season2);

    CatalogueItem ep1;
    ep1.id = "episode:test:1:1";
    ep1.kind = CatalogueKind::episode;
    ep1.title = "One";
    ep1.parent_id = season1.id;
    ep1.season_number = 1;
    ep1.episode_number = 1;
    ep1.media_ids = {file_media_id(ep1_file)};
    ep1 = service.catalogue().upsert(ep1);

    CatalogueItem ep2;
    ep2.id = "episode:test:1:2";
    ep2.kind = CatalogueKind::episode;
    ep2.title = "Two";
    ep2.parent_id = season1.id;
    ep2.season_number = 1;
    ep2.episode_number = 2;
    ep2.media_ids = {file_media_id(ep2_file)};
    ep2 = service.catalogue().upsert(ep2);

    CatalogueItem ep3;
    ep3.id = "episode:test:2:1";
    ep3.kind = CatalogueKind::episode;
    ep3.title = "Three";
    ep3.parent_id = season2.id;
    ep3.season_number = 2;
    ep3.episode_number = 1;
    ep3.media_ids = {file_media_id(ep3_file)};
    ep3 = service.catalogue().upsert(ep3);

    CatalogueItem movie1;
    movie1.id = "movie:test:1";
    movie1.kind = CatalogueKind::movie;
    movie1.title = "First Film";
    movie1.year = 2001;
    movie1.external_ids["collection"] = "test-films";
    movie1.media_ids = {"/Movies/one.mkv"};
    movie1 = service.catalogue().upsert(movie1);

    CatalogueItem movie2;
    movie2.id = "movie:test:2";
    movie2.kind = CatalogueKind::movie;
    movie2.title = "Second Film";
    movie2.year = 2003;
    movie2.external_ids["collection"] = "test-films";
    movie2.media_ids = {"path:/Movies/two.mkv"};
    movie2 = service.catalogue().upsert(movie2);

    HydrationConfig prediction_config;
    prediction_config.catalogue_lookahead = 1;
    PlaybackTracker prediction_tracker;
    CatalogueSequenceHintProvider predictor(prediction_tracker, service.filesystem(),
                                            service.catalogue(), prediction_config);

    auto check_prediction = [&](const std::string& path, const FsEntry& current,
                                const FsEntry& expected, const char* reason) {
        auto active = prediction_tracker.open(path, current);
        prediction_tracker.progress(active, 0);
        auto predicted = predictor.hints();
        REQUIRE(predicted.size() == 1);
        CHECK(predicted[0].reason == reason);
        REQUIRE(!predicted[0].objects.empty());
        CHECK(predicted[0].objects.front() == expected.extents.front().id);
        CHECK(predicted[0].objects.size() == expected.extents.size());
        prediction_tracker.close(active);
    };
    check_prediction("/TV/s01e01.mkv", ep1_file, ep2_file, "next_episode");
    check_prediction("/TV/s01e02.mkv", ep2_file, ep3_file, "next_episode");
    check_prediction("/Movies/one.mkv", movie1_file, movie2_file, "next_movie");

    service.stop();
}

void test_replica_selector() {
    auto node = [](uint8_t value) {
        NodeInfo n;
        n.id.bytes[0] = value;
        n.host = "replica-" + std::to_string(value);
        n.port = static_cast<uint16_t>(7000 + value);
        return n;
    };

    ReplicaSelector selector;
    std::vector<NodeInfo> nodes{node(1), node(2), node(3)};

    // With no measurements, the extent stripe spreads equivalent speculative
    // work over the complete replica set instead of pinning it to peer zero.
    CHECK(selector.order(nodes, 0, ReplicaWorkClass::speculative).front().id == nodes[0].id);
    CHECK(selector.order(nodes, 1, ReplicaWorkClass::speculative).front().id == nodes[1].id);
    CHECK(selector.order(nodes, 2, ReplicaWorkClass::speculative).front().id == nodes[2].id);

    // Outstanding speculative work makes an otherwise equal peer less useful
    // for the next independent extent.
    selector.started(nodes[0], ReplicaWorkClass::speculative);
    CHECK(selector.order(nodes, 0, ReplicaWorkClass::speculative).front().id != nodes[0].id);
    selector.finished(nodes[0], ReplicaWorkClass::speculative, 1024, 100ms, true);

    // Foreground reads optimise latency rather than symmetry.
    selector.started(nodes[0], ReplicaWorkClass::foreground);
    selector.finished(nodes[0], ReplicaWorkClass::foreground, 1024, 20ms, true);
    selector.started(nodes[1], ReplicaWorkClass::foreground);
    selector.finished(nodes[1], ReplicaWorkClass::foreground, 1024, 200ms, true);
    CHECK(selector.order(nodes, 0, ReplicaWorkClass::foreground).front().id == nodes[0].id);

    // Speculative work yields to a peer carrying foreground traffic when an
    // idle replica is available.
    selector.started(nodes[0], ReplicaWorkClass::foreground);
    CHECK(selector.order(nodes, 0, ReplicaWorkClass::speculative).front().id != nodes[0].id);
    selector.finished(nodes[0], ReplicaWorkClass::foreground, 1024, 20ms, true);

    // Promotion moves one active transfer between accounting classes rather
    // than duplicating it.
    selector.started(nodes[2], ReplicaWorkClass::speculative);
    selector.promoted(nodes[2]);
    auto promoted = selector.stats(nodes[2].id);
    CHECK(promoted.speculative_in_flight == 0);
    CHECK(promoted.foreground_in_flight == 1);
    selector.finished(nodes[2], ReplicaWorkClass::foreground, 1024, 50ms, true);
    CHECK(selector.stats(nodes[2].id).foreground_in_flight == 0);

    // A failed source is penalised immediately; a later successful transfer
    // clears the consecutive-failure penalty without erasing history.
    selector.started(nodes[1], ReplicaWorkClass::foreground);
    selector.finished(nodes[1], ReplicaWorkClass::foreground, 0, 10ms, false);
    CHECK(selector.stats(nodes[1].id).failures == 1);
    CHECK(selector.order(nodes, 0, ReplicaWorkClass::foreground).front().id != nodes[1].id);
    selector.started(nodes[1], ReplicaWorkClass::foreground);
    selector.finished(nodes[1], ReplicaWorkClass::foreground, 1024, 25ms, true);
    CHECK(selector.stats(nodes[1].id).failures == 1);
}

void test_cache_hydrator_fetches_to_persistent_cache() {
    TempDir temp;
    auto keyfile = temp.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(temp.path() / "n1", keyfile, p1);
    auto c2 = config_for(temp.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;
    c2.cache.path = temp.path() / "cache2";
    c2.cache.max_blocks = 32;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    DistributedStore source(n1);
    DistributedStore target(n2);
    auto make_remote = [&](uint8_t value) {
        Bytes data(128 * 1024, value);
        auto id = object_id(data);
        REQUIRE(source.replicate_all(id, data, false) >= 2);
        n2.local_store().remove(id);
        n2.block_cache().remove(id);
        REQUIRE(!target.locally_available(id));
        return id;
    };

    const auto a = make_remote(61);
    const auto b = make_remote(62);
    const auto c = make_remote(63);
    const auto n0 = make_remote(64);
    const auto nnext = make_remote(65);

    class StaticHints final : public HydrationHintProvider {
        std::vector<HydrationHint> hints_;
      public:
        explicit StaticHints(std::vector<HydrationHint> hints) : hints_(std::move(hints)) {}
        std::string_view name() const override { return "test"; }
        std::vector<HydrationHint> hints() override { return hints_; }
    } provider({{"current", {a, b}, 1000, "read_ahead"},
                {"current", {a, b, c}, 700, "current_file"},
                {"next", {n0, nnext}, 300, "next_episode"}});

    HydrationConfig config;
    CacheHydrator hydrator(target, config);
    hydrator.add_provider(provider);
    REQUIRE(hydrator.run_once());
    CHECK(hydrator.status().last_object == a);
    CHECK(n2.block_cache().has(a));
    CHECK(!n2.local_store().has(a));
    REQUIRE(hydrator.run_once());
    CHECK(hydrator.status().last_object == b);
    REQUIRE(hydrator.run_once());
    CHECK(hydrator.status().last_object == c);
    REQUIRE(hydrator.run_once());
    CHECK(hydrator.status().last_object == n0);
    CHECK(n2.block_cache().has(n0));
    CHECK(!n2.local_store().has(n0));
    CHECK(!n2.block_cache().has(nnext));

    // The production worker keeps a bounded speculative window in flight so
    // several replicas can contribute bandwidth. Dispatch is still ordered and
    // the configured limit is never exceeded.
    const auto parallel0 = make_remote(66);
    const auto parallel1 = make_remote(67);
    const auto parallel2 = make_remote(68);
    StaticHints concurrent_provider(
        {{"parallel", {parallel0, parallel1, parallel2}, 500, "current_file"}});
    HydrationConfig concurrent_config;
    concurrent_config.interval = 10ms;
    concurrent_config.max_inflight = 2;
    CacheHydrator concurrent(target, concurrent_config);
    concurrent.add_provider(concurrent_provider);
    concurrent.start();
    REQUIRE(wait_until([&] { return concurrent.status().fetched >= 3; }, 5s));
    concurrent.stop();
    auto concurrent_status = concurrent.status();
    CHECK(concurrent_status.peak_in_flight == 2);
    CHECK(concurrent_status.in_flight == 0);
    CHECK(n2.block_cache().has(parallel0));
    CHECK(n2.block_cache().has(parallel1));
    CHECK(n2.block_cache().has(parallel2));

    n2.stop();
    n1.stop();
}


void test_media_probe_and_online_catalogue_scanner() {
    // External IDs are part of the durable catalogue format. Read fields in
    // deterministic order: function-argument evaluation order must not be
    // allowed to swap provider/id pairs during decoding.
    CatalogueSnapshot codec_snapshot;
    CatalogueItem codec_item;
    codec_item.id = "codec:test";
    codec_item.kind = CatalogueKind::movie;
    codec_item.title = "Codec Test";
    codec_item.external_ids["tmdb"] = "42";
    codec_item.external_ids["macha_scanner"] = "1";
    codec_snapshot.items.emplace(codec_item.id, codec_item);
    auto codec_roundtrip = decode_catalogue(encode_catalogue(codec_snapshot));
    REQUIRE(codec_roundtrip.items.contains(codec_item.id));
    CHECK(codec_roundtrip.items.at(codec_item.id).external_ids == codec_item.external_ids);

    FsEntry fake;
    fake.type = EntryType::file;
    fake.size = 123456;

    auto episode = probe_media_path(
        "/TV/The Expanse/Season 02/The.Expanse.S02E05.Home.mkv", fake);
    REQUIRE(episode.has_value());
    CHECK(episode->kind == MediaProbeKind::episode);
    CHECK(episode->series == "The Expanse");
    CHECK(episode->season == 2);
    CHECK(episode->episode == 5);
    CHECK(episode->title == "Home");

    auto movie = probe_media_path(
        "/Movies/Blade.Runner.2049.2017.1080p.BluRay.mkv", fake);
    REQUIRE(movie.has_value());
    CHECK(movie->kind == MediaProbeKind::movie);
    CHECK(movie->title == "Blade Runner 2049");
    CHECK(movie->year == 2017);

    auto track = probe_media_path(
        "/Music/Pink Floyd/The Dark Side of the Moon/01 - Speak to Me.flac", fake);
    REQUIRE(track.has_value());
    CHECK(track->kind == MediaProbeKind::track);
    CHECK(track->artist == "Pink Floyd");
    CHECK(track->album == "The Dark Side of the Moon");
    CHECK(track->track == 1);
    CHECK(track->title == "Speak to Me");

    auto disc_track = probe_media_path(
        "/Music/Pink Floyd/The Wall/CD 2/03 - Hey You.flac", fake);
    REQUIRE(disc_track.has_value());
    CHECK(disc_track->artist == "Pink Floyd");
    CHECK(disc_track->album == "The Wall");
    CHECK(disc_track->disc == 2);
    CHECK(disc_track->track == 3);
    CHECK(disc_track->title == "Hey You");

    // Provider unit: one season lookup yields the show/season/episode hierarchy
    // and all useful visual roles without requiring separate image metadata calls.
    TempDir provider_temp;
    auto token = provider_temp.path() / "tmdb.token";
    {
        std::ofstream out(token);
        out << "test-token\n";
    }
    FakeHttpClient tmdb_http;
    tmdb_http.add("/search/tv", 200, "application/json",
                  R"({"results":[{"id":1402,"name":"The Walking Dead","overview":"Show overview","first_air_date":"2010-10-31","poster_path":"/show.jpg","backdrop_path":"/show-bg.jpg"}]})");
    tmdb_http.add("/tv/1402/season/1", 200, "application/json",
                  R"({"id":3643,"name":"Season 1","overview":"Season overview","poster_path":"/season.jpg","episodes":[{"id":63056,"episode_number":1,"name":"Days Gone Bye","overview":"Episode overview","still_path":"/episode.jpg"}]})");
    CatalogueTmdbConfig tmdb_config;
    tmdb_config.token_file = token;
    TmdbProvider tmdb(tmdb_http, tmdb_config);
    MediaProbe tv_probe;
    tv_probe.kind = MediaProbeKind::episode;
    tv_probe.series = "The Walking Dead";
    tv_probe.season = 1;
    tv_probe.episode = 1;
    tv_probe.media_id = "macha:test-episode";
    auto tv_match = tmdb.lookup(tv_probe);
    REQUIRE(tv_match.has_value());
    CHECK(tv_match->items.size() == 3);
    CHECK(tv_match->items[0].kind == CatalogueKind::show);
    CHECK(tv_match->items[1].kind == CatalogueKind::season);
    CHECK(tv_match->items[2].kind == CatalogueKind::episode);
    CHECK(tv_match->items[2].media_ids == std::vector<std::string>{"macha:test-episode"});
    CHECK(tv_match->artwork.size() == 4);

    // MusicBrainz resolves one release, then maps the local track onto its
    // recording; Cover Art Archive provides the front cover URL.
    FakeHttpClient mb_http;
    mb_http.add("/ws/2/release?", 200, "application/json",
                R"({"releases":[{"id":"rel-1","title":"The Dark Side of the Moon","score":100,"artist-credit":[{"name":"Pink Floyd","artist":{"id":"artist-1","name":"Pink Floyd"}}]}]})");
    mb_http.add("/ws/2/release/rel-1", 200, "application/json",
                R"({"id":"rel-1","title":"The Dark Side of the Moon","date":"1973-03-01","artist-credit":[{"name":"Pink Floyd","artist":{"id":"artist-1","name":"Pink Floyd"}}],"release-group":{"id":"rg-1"},"media":[{"position":1,"tracks":[{"position":1,"title":"Speak to Me","recording":{"id":"rec-1","title":"Speak to Me"}}]}]})");
    mb_http.add("coverartarchive.org/release/rel-1", 200, "application/json",
                R"({"images":[{"front":true,"image":"https://images.example/original.jpg","thumbnails":{"500":"https://images.example/500.jpg"}}]})");
    CatalogueMusicBrainzConfig mb_config;
    mb_config.contact = "https://example.test/macha";
    MusicBrainzProvider mb(mb_http, mb_config);
    MediaProbe music_probe;
    music_probe.kind = MediaProbeKind::track;
    music_probe.artist = "Pink Floyd";
    music_probe.album = "The Dark Side of the Moon";
    music_probe.title = "Speak to Me";
    music_probe.track = 1;
    music_probe.media_id = "macha:test-track";
    auto mb_match = mb.lookup(music_probe);
    REQUIRE(mb_match.has_value());
    CHECK(mb_match->items.size() == 3);
    CHECK(mb_match->items[0].kind == CatalogueKind::artist);
    CHECK(mb_match->items[1].kind == CatalogueKind::album);
    CHECK(mb_match->items[2].kind == CatalogueKind::track);
    CHECK(mb_match->items[2].external_ids.at("musicbrainz") == "rec-1");
    REQUIRE(!mb_match->artwork.empty());
    CHECK(mb_match->artwork.front().role == "cover");
    CHECK(mb_match->artwork.front().url == "https://images.example/500.jpg");

    // End-to-end scanner: resolve a real distributed filesystem entry, fetch
    // poster/backdrop bytes, commit them with the catalogue, then prove a second
    // scan is idempotent and deletion removes only the scanner-owned item.
    TempDir t;
    auto key = t.path() / "cluster.key";
    write_key(key);
    auto config = config_for(t.path() / "disk", key, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    auto keys = load_cluster_keys(key);
    Service service(config, keys);
    service.start();
    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
    service.filesystem().create_file("/Movies/Blade.Runner.2049.2017.1080p.mkv", 0644,
                                     getuid(), getgid());
    auto bytes = pattern(32768);
    auto writer = service.filesystem().open_write(
        "/Movies/Blade.Runner.2049.2017.1080p.mkv", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto entry = service.filesystem().getattr(
        "/Movies/Blade.Runner.2049.2017.1080p.mkv");
    auto media_id = file_media_id(entry);

    auto scanner_token = t.path() / "scanner-tmdb.token";
    {
        std::ofstream out(scanner_token);
        out << "scanner-token\n";
    }
    auto fake_http = std::make_unique<FakeHttpClient>();
    fake_http->add("/search/movie", 200, "application/json",
                   R"({"results":[{"id":335984,"title":"Blade Runner 2049","release_date":"2017-10-04"}]})");
    fake_http->add("/movie/335984", 200, "application/json",
                   R"({"id":335984,"title":"Blade Runner 2049","overview":"A blade runner uncovers a long-buried secret.","release_date":"2017-10-04","poster_path":"/poster.jpg","backdrop_path":"/backdrop.jpg","belongs_to_collection":{"id":422837,"name":"Blade Runner Collection"}})");
    fake_http->add_bytes("/t/p/w500/poster.jpg", 200, "image/jpeg", Bytes{1,2,3,4,5});
    fake_http->add_bytes("/t/p/w500/backdrop.jpg", 200, "image/jpeg", Bytes{6,7,8,9});

    CatalogueScannerConfig scanner_config;
    scanner_config.enabled = true;
    scanner_config.roots = {"/Movies"};
    scanner_config.tmdb.token_file = scanner_token;
    scanner_config.musicbrainz.enabled = false;
    CatalogueScanner scanner(service.node(), service.filesystem(), service.catalogue(),
                             scanner_config, std::move(fake_http));
    CHECK(scanner.scan_once() == 1);
    auto catalogued = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(catalogued.has_value());
    CHECK(catalogued->title == "Blade Runner 2049");
    CHECK(catalogued->external_ids.at("tmdb_collection") == "422837");
    CHECK(catalogued->external_ids.at("macha_scanner") == "1");
    CHECK(catalogued->media_ids == std::vector<std::string>{media_id});
    CHECK(catalogued->artwork.size() == 2);
    for (const auto& art : catalogued->artwork)
        CHECK(service.node().local_store().has(art.id));
    auto revision = catalogued->revision;
    CHECK(scanner.scan_once() == 0);
    REQUIRE(service.catalogue().get("tmdb:movie:335984").has_value());
    CHECK(service.catalogue().get("tmdb:movie:335984")->revision == revision);

    // A second file resolving to the same title adds another binding without
    // losing the already-bound media identity. Deletion reconciles them one at
    // a time and removes the scanner-owned item only after the final copy goes.
    const std::string alternate = "/Movies/Blade.Runner.2049.2017.Remux.mkv";
    service.filesystem().create_file(alternate, 0644, getuid(), getgid());
    auto alternate_bytes = pattern(32769);
    auto alternate_writer = service.filesystem().open_write(alternate, true);
    REQUIRE(alternate_writer->write(0, alternate_bytes) == alternate_bytes.size());
    alternate_writer->commit();
    auto alternate_id = file_media_id(service.filesystem().getattr(alternate));
    CHECK(alternate_id != media_id);
    CHECK(scanner.scan_once() == 1);
    auto twice = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(twice.has_value());
    CHECK(std::find(twice->media_ids.begin(), twice->media_ids.end(), media_id) != twice->media_ids.end());
    CHECK(std::find(twice->media_ids.begin(), twice->media_ids.end(), alternate_id) != twice->media_ids.end());

    service.filesystem().unlink("/Movies/Blade.Runner.2049.2017.1080p.mkv");
    std::this_thread::sleep_for(config.metadata_cache + 50ms);
    CHECK(scanner.scan_once() == 0);
    auto remaining = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(remaining.has_value());
    CHECK(remaining->media_ids == std::vector<std::string>{alternate_id});

    service.filesystem().unlink(alternate);
    std::this_thread::sleep_for(config.metadata_cache + 50ms);
    CHECK(service.filesystem().readdir("/Movies").empty());
    CHECK(scanner.scan_once() == 0);
    CHECK(!service.catalogue().get("tmdb:movie:335984").has_value());
    service.stop();
}


void test_catalogue_cache_ignores_unrelated_metadata_generation() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);
    node.start();

    CatalogueItem item;
    item.id = "test:movie:1";
    item.kind = CatalogueKind::movie;
    item.title = "Cached Movie";
    auto committed = catalogue.upsert(item);
    CHECK(committed.title == "Cached Movie");

    auto status = catalogue.status();
    REQUIRE(status.root.has_value());
    REQUIRE(node.local_store().remove(*status.root));

    // Advance ordinary filesystem metadata without changing catalogue_root.
    // The cached immutable catalogue must remain usable even though the backing
    // root object has deliberately been made unavailable for a reload.
    metadata.mutate([](MetadataSnapshot& snapshot) {
        auto root = snapshot.entries.find("/");
        REQUIRE(root != snapshot.entries.end());
        ++root->second.version;
        ++root->second.mtime_ns;
    });

    auto cached = catalogue.get("test:movie:1");
    REQUIRE(cached.has_value());
    CHECK(cached->title == "Cached Movie");

    // Background convergence should also recognise that an unchanged
    // content-addressed root does not need to be reloaded.
    catalogue.repair_once();
    auto after_repair = catalogue.get("test:movie:1");
    REQUIRE(after_repair.has_value());
    CHECK(after_repair->title == "Cached Movie");

    node.stop();
}

void test_media_index_cache_survives_namespace_churn() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "node", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;

    NodeRuntime node(config, keys);
    DistributedStore store(node);
    MetadataManager metadata(node);
    FileSystem filesystem(node, store, metadata);
    node.start();

    filesystem.mkdir("/media", 0755, getuid(), getgid());
    filesystem.create_file("/media/a.mkv", 0644, getuid(), getgid());
    auto a_bytes = pattern(32 * 1024 + 17);
    auto a_writer = filesystem.open_write("/media/a.mkv", true);
    REQUIRE(a_writer->write(0, a_bytes) == a_bytes.size());
    a_writer->commit();
    auto a_entry = filesystem.getattr("/media/a.mkv");
    auto a_id = file_media_id(a_entry);

    auto first = filesystem.find_media(a_id);
    REQUIRE(first.has_value());
    CHECK(first->first == "/media/a.mkv");

    // Unrelated namespace churn must not invalidate an already resolved,
    // content-addressed media id.
    filesystem.mkdir("/noise", 0755, getuid(), getgid());
    auto cached = filesystem.find_media(a_id);
    REQUIRE(cached.has_value());
    CHECK(cached->first == "/media/a.mkv");
    CHECK(file_media_id(cached->second) == a_id);

    // A genuinely new id is a cache miss and must rebuild against current
    // metadata, after which both the new and old ids remain resolvable.
    filesystem.create_file("/media/b.mkv", 0644, getuid(), getgid());
    auto b_bytes = pattern(48 * 1024 + 29);
    auto b_writer = filesystem.open_write("/media/b.mkv", true);
    REQUIRE(b_writer->write(0, b_bytes) == b_bytes.size());
    b_writer->commit();
    auto b_id = file_media_id(filesystem.getattr("/media/b.mkv"));
    CHECK(b_id != a_id);

    auto second = filesystem.find_media(b_id);
    REQUIRE(second.has_value());
    CHECK(second->first == "/media/b.mkv");
    REQUIRE(filesystem.find_media(a_id).has_value());

    node.stop();
}

void test_catalogue_sync_search_and_artwork_gc() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();
    uint16_t p3 = free_port();

    auto c1 = config_for(t.path() / "cat1", keyfile, p1);
    auto c2 = config_for(t.path() / "cat2", keyfile, p2, {{"127.0.0.1", p1}});
    auto c3 = config_for(t.path() / "cat3", keyfile, p3, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = c3.replication = 1;
    c1.metadata_replication = c2.metadata_replication = c3.metadata_replication = 1;
    c1.maintenance.garbage_grace = 0ms;
    c2.maintenance.garbage_grace = 0ms;
    c3.maintenance.garbage_grace = 0ms;
    // Production defaults back settled maintenance off for 30 seconds. This
    // fixture deliberately exercises cluster GC at the scheduler's 5-second
    // minimum so its 10-second convergence assertion does not depend on the
    // production no-progress interval.
    c1.maintenance.no_progress_backoff = 1000ms;
    c2.maintenance.no_progress_backoff = 1000ms;
    c3.maintenance.no_progress_backoff = 1000ms;
    CHECK(maintenance_background_interval(c1.maintenance) == 5000ms);

    Service s1(c1, keys);
    s1.start();
    REQUIRE(wait_until([&] {
        try {
            s1.catalogue().repair_once();
            return s1.catalogue().status().ready;
        } catch (...) {
            return false;
        }
    }));

    CatalogueItem show;
    show.id = "show:test";
    show.kind = CatalogueKind::show;
    show.title = "Test Programme";
    show.synopsis = "A deliberately small distributed catalogue test.";
    show.external_ids["tmdb"] = "1234";
    show = s1.catalogue().upsert(show);

    CatalogueItem episode;
    episode.id = "episode:test:1:1";
    episode.kind = CatalogueKind::episode;
    episode.title = "The Pilot";
    episode.parent_id = show.id;
    episode.season_number = 1;
    episode.episode_number = 1;
    episode = s1.catalogue().upsert(episode);

    auto first_art_bytes = pattern(64 * 1024 + 17);
    auto first_art = s1.catalogue().put_artwork(show.id, "poster", "image/jpeg",
                                                first_art_bytes, show.revision);
    show = *s1.catalogue().get(show.id);
    CHECK(s1.catalogue().search("pilot").front().id == episode.id);

    // A node joining after the catalogue already exists must become locally
    // browse/search capable, including artwork, without provider access.
    Service s2(c2, keys);
    s2.start();
    REQUIRE(wait_until([&] {
        auto status = s2.catalogue().status();
        return status.ready && status.items == 2 && status.artwork_objects == 1 &&
               status.local_artwork_objects == 1;
    }, 10s));
    REQUIRE(s2.catalogue().get(episode.id).has_value());
    CHECK(s2.catalogue().search("test programme").front().id == show.id);
    CHECK(s2.node().local_store().has(first_art.id));

    Service s3(c3, keys);
    s3.start();
    REQUIRE(wait_until([&] {
        auto status = s3.catalogue().status();
        return status.ready && status.items == 2 && status.local_artwork_objects == 1;
    }, 10s));
    CHECK(s3.catalogue().list(CatalogueKind::episode).size() == 1);
    CHECK(s3.node().local_store().has(first_art.id));

    // Replacing the poster makes the old object a persistent metadata garbage
    // candidate. With a zero grace period in this test, cluster GC must delete
    // it from every connected node while preserving the replacement.
    auto second_art_bytes = pattern(96 * 1024 + 3);
    second_art_bytes[0] ^= 0xa5;
    auto second_art = s2.catalogue().put_artwork(show.id, "poster", "image/jpeg",
                                                 second_art_bytes, show.revision);
    REQUIRE(wait_until([&] {
        return s1.catalogue().status().ready && s2.catalogue().status().ready &&
               s3.catalogue().status().ready && s1.node().local_store().has(second_art.id) &&
               s2.node().local_store().has(second_art.id) &&
               s3.node().local_store().has(second_art.id);
    }, 10s));
    REQUIRE(wait_until([&] {
        return !s1.node().local_store().has(first_art.id) &&
               !s2.node().local_store().has(first_art.id) &&
               !s3.node().local_store().has(first_art.id);
    }, 10s));

    CatalogueApi api(s3.catalogue());
    auto status_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/status",
                                       .query = {},
                                       .headers = {},
                                       .body = {}});
    CHECK(status_response.status == 200);
    std::string status_body(status_response.body.begin(), status_response.body.end());
    CHECK(status_body.find("\"ready\":true") != std::string::npos);
    CHECK(status_body.find("\"server_version\":\"0.8.5\"") != std::string::npos);
    auto search_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/search",
                                       .query = {{"q", "pilot"}},
                                       .headers = {},
                                       .body = {}});
    CHECK(search_response.status == 200);
    std::string search_body(search_response.body.begin(), search_response.body.end());
    CHECK(search_body.find("episode:test:1:1") != std::string::npos);

    s3.stop();
    s2.stop();
    s1.stop();
}

void test_three_node_cluster() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();
    uint16_t p3 = free_port();
    uint16_t p4 = free_port();

    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    auto c3 = config_for(t.path() / "n3", keyfile, p3, {{"127.0.0.1", p1}});
    auto c4 = config_for(t.path() / "n4", keyfile, p4, {{"127.0.0.1", p1}});

    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        Service s3(c3, keys);
        s1.start();
        s2.start();
        s3.start();

        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 3 &&
                   s2.node().membership().active().size() >= 3 &&
                   s3.node().membership().active().size() >= 3;
        }));

        s1.filesystem().mkdir("/media", 0755, getuid(), getgid());
        s1.filesystem().create_file("/media/movie.mkv", 0644, getuid(), getgid());
        auto input = pattern(3 * 1024 * 1024 + 12345);
        auto writer = s1.filesystem().open_write("/media/movie.mkv", true);
        size_t offset = 0;
        while (offset < input.size()) {
            size_t n = std::min<size_t>(77777, input.size() - offset);
            CHECK(writer->write(offset, {input.data() + offset, n}) == n);
            offset += n;
        }
        writer->commit();

        // Removing a committed file records explicit object tombstones; GC never
        // guesses that an unreferenced object is safe to delete.
        s1.filesystem().create_file("/media/delete-me.bin", 0644, getuid(), getgid());
        auto delete_data = pattern(131072);
        auto delete_writer = s1.filesystem().open_write("/media/delete-me.bin", true);
        REQUIRE(delete_writer->write(0, delete_data) == delete_data.size());
        delete_writer->commit();
        auto delete_entry = s1.filesystem().getattr("/media/delete-me.bin");
        REQUIRE(delete_entry.extents.size() == 1);
        auto deleted_id = delete_entry.extents.front().id;
        s1.filesystem().unlink("/media/delete-me.bin");
        auto maintenance = s1.filesystem().maintenance_objects();
        CHECK(std::find(maintenance.garbage.begin(), maintenance.garbage.end(), deleted_id) !=
              maintenance.garbage.end());
        CHECK(std::find(maintenance.live.begin(), maintenance.live.end(), deleted_id) ==
              maintenance.live.end());
        auto inventory1 = s1.filesystem().maintenance_objects_cached();
        auto inventory2 = s1.filesystem().maintenance_objects_cached();
        CHECK(inventory1.get() == inventory2.get());
        CHECK(inventory1->metadata_generation != 0);
        CHECK(inventory1->entries >= 2);
        CHECK(inventory1->extents >= 1);
        CHECK(std::is_sorted(inventory1->live.begin(), inventory1->live.end()));
        CHECK(std::adjacent_find(inventory1->live.begin(), inventory1->live.end()) ==
              inventory1->live.end());
        CHECK(std::is_sorted(inventory1->garbage.begin(), inventory1->garbage.end()));
        s1.filesystem().mkdir("/inventory-generation-change", 0755, getuid(), getgid());
        auto inventory3 = s1.filesystem().maintenance_objects_cached();
        CHECK(inventory3->metadata_generation > inventory1->metadata_generation);
        CHECK(inventory3.get() != inventory1.get());

        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/media/movie.mkv").size == input.size();
            } catch (...) {
                return false;
            }
        }));

        // Warm node 2's namespace cache, mutate through node 1, then require
        // visibility before the cache TTL can expire. The local replica update
        // and generation notice are both valid invalidation paths.
        (void)s2.filesystem().getattr("/media");
        s1.filesystem().mkdir("/cache-invalidation", 0755, getuid(), getgid());
        REQUIRE(wait_until(
            [&] {
                try {
                    return s2.filesystem().getattr("/cache-invalidation").type ==
                           EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            200ms));

        auto reader = s2.filesystem().open_read("/media/movie.mkv");
        Bytes output(input.size());
        size_t got = 0;
        while (got < output.size()) {
            size_t n = std::min<size_t>(131072, output.size() - got);
            auto r = reader->read(got, {output.data() + got, n});
            REQUIRE(r > 0);
            got += r;
        }
        CHECK(output == input);

        Bytes slice(333333);
        auto random_reader = s3.filesystem().open_read("/media/movie.mkv");
        auto n = random_reader->read(987654, slice);
        REQUIRE(n == slice.size());
        CHECK(std::equal(slice.begin(), slice.end(), input.begin() + 987654));

        s1.filesystem().mkdir("/media/not-a-file", 0755, getuid(), getgid());
        bool wrong_type_rejected = false;
        try {
            s1.filesystem().rename("/media/movie.mkv", "/media/not-a-file", false);
        } catch (const FsError& error) {
            wrong_type_rejected = error.code() == EISDIR;
        }
        CHECK(wrong_type_rejected);

        // Loss of any one metadata voter must not stop namespace mutations.
        auto failed_voter = s3.node().node_id();
        s3.stop();
        s2.filesystem().mkdir("/survives-one-node-loss", 0755, getuid(), getgid());
        CHECK(s1.filesystem().getattr("/survives-one-node-loss").type == EntryType::directory);

        // Remove a local replica from node 2; reads must transparently fall back to node 1.
        auto entry = s2.filesystem().getattr("/media/movie.mkv");
        REQUIRE(!entry.extents.empty());

        // A damaged encrypted replica must fail authentication, fall back to a
        // healthy peer, and be restored by the bounded scrub/repair path.
        auto damaged = entry.extents.front().id;
        corrupt_object(c2.storage_backends.front().path, damaged);
        DistributedStore corruption_repair(s2.node());
        auto recovered = corruption_repair.get(damaged);
        REQUIRE(recovered.has_value());
        CHECK(object_id(*recovered) == damaged);
        corruption_repair.scrub_once(128ULL * 1024 * 1024);
        for (size_t attempt = 0; attempt < entry.extents.size() + 2; ++attempt)
            corruption_repair.repair_once(128ULL * 1024 * 1024);
        REQUIRE(s2.node().local_store().get(damaged).has_value());

        // Remove a local replica entirely; reads must transparently fall back.
        s2.node().local_store().remove(damaged);
        auto failover_reader = s2.filesystem().open_read("/media/movie.mkv");
        Bytes first(1024 * 1024);
        REQUIRE(failover_reader->read(0, first) == first.size());
        CHECK(std::equal(first.begin(), first.end(), input.begin()));

        // Enable a persistent SSD-style cache on node 2 at runtime. A playback
        // fetch that node 2 should own is retained independently of DHT storage
        // and also promoted back to authoritative storage without another WAN
        // fetch.
        auto cache_config = c2;
        cache_config.cache.path = t.path() / "n2-cache";
        cache_config.cache.max_blocks = 8;
        s2.node().reconfigure_local(cache_config);
        s2.node().local_store().remove(damaged);
        DistributedStore playback_store(s2.node());
        auto playback_fetch = playback_store.get(damaged, 0, true);
        REQUIRE(playback_fetch.has_value());
        CHECK(object_id(*playback_fetch) == damaged);
        REQUIRE(wait_until([&] { return s2.node().block_cache().has(damaged); }));
        REQUIRE(wait_until([&] { return s2.node().local_store().has(damaged); }));
        // Prove the cache is genuinely independent: discard the DHT copy again;
        // subsequent reads can still use the persistent cache.
        s2.node().local_store().remove(damaged);
        auto cached_fetch = playback_store.get(damaged, 0, true);
        REQUIRE(cached_fetch.has_value());
        CHECK(*cached_fetch == *playback_fetch);

        // Add a replacement storage node. Once the failed voter has expired, a
        // surviving metadata majority can safely replace it with the new node.
        Service s4(c4, keys);
        s4.start();
        REQUIRE(wait_until([&] {
            auto active = s1.node().membership().active();
            bool has_failed = false;
            bool has_new = false;
            for (const auto& peer : active) {
                has_failed |= peer.id == failed_voter;
                has_new |= peer.id == s4.node().node_id();
            }
            return !has_failed && has_new && active.size() >= 3;
        }));
        // A joining owner pulls its assigned live objects automatically. This
        // no longer depends on an old owner being manually prodded to scan/push.
        REQUIRE(wait_until([&] {
            return s4.node().local_store().has(entry.extents.front().id);
        }));

        MetadataManager repair(s1.node());
        repair.repair_once();
        REQUIRE(wait_until([&] {
            try {
                return s4.filesystem().getattr("/media/movie.mkv").size == input.size();
            } catch (...) {
                return false;
            }
        }));

        // The old voter can now also disappear: node 2 + replacement node 4
        // are a majority of the new voter group.
        s1.stop();
        s2.filesystem().mkdir("/after-voter-replacement", 0755, getuid(), getgid());
        CHECK(s4.filesystem().getattr("/after-voter-replacement").type == EntryType::directory);

        // A single surviving voter is a minority and must not split-brain metadata.
        s4.stop();
        bool refused = false;
        try {
            s2.filesystem().mkdir("/must-not-commit", 0755, getuid(), getgid());
        } catch (...) {
            refused = true;
        }
        CHECK(refused);
        s2.stop();
    }

    // Full process-style restart from persisted state.
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        Service s3(c3, keys);
        s1.start();
        s2.start();
        s3.start();
        REQUIRE(wait_until([&] { return s2.node().membership().active().size() >= 3; }));
        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/media/movie.mkv").size > 0;
            } catch (...) {
                return false;
            }
        }));
        auto e = s2.filesystem().getattr("/media/movie.mkv");
        CHECK(e.size == 3 * 1024 * 1024 + 12345);
        Bytes tail(65536);
        auto r = s2.filesystem().open_read("/media/movie.mkv");
        REQUIRE(r->read(e.size - tail.size(), tail) == tail.size());
        auto expected = pattern(e.size);
        CHECK(std::equal(tail.begin(), tail.end(), expected.end() - tail.size()));
        s3.stop();
        s2.stop();
        s1.stop();
    }
}

void test_media_segment_store_backpressure_and_spill() {
    TempDir t;
    auto store = std::make_shared<MediaSegmentStore>(2, 2 * 1024, t.path() / "spill", 4000ms,
                                                     std::vector<double>{4.0, 4.0, 4.0, 4.0});
    REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
    const auto initial_playlist = store->playlist();
    CHECK(initial_playlist.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos);
    CHECK(initial_playlist.find("segment-000003.m4s") != std::string::npos);
    CHECK(initial_playlist.find("#EXT-X-ENDLIST") != std::string::npos);

    REQUIRE(store->publish_segment(Bytes(1024, 0x10), 4.0));
    REQUIRE(store->publish_segment(Bytes(1024, 0x11), 4.0));
    REQUIRE(store->publish_segment(Bytes(1024, 0x12), 4.0));

    std::atomic_bool fourth_published{};
    std::jthread producer([&] {
        fourth_published.store(store->publish_segment(Bytes(1024, 0x13), 4.0));
    });
    std::this_thread::sleep_for(50ms);
    CHECK(!fourth_published.load());

    std::optional<Bytes> waited_segment;
    std::jthread consumer([&] {
        store->note_requested(3);
        waited_segment = store->wait_object("segment-000003.m4s", 1s);
    });
    consumer.join();
    producer.join();
    CHECK(fourth_published.load());
    REQUIRE(waited_segment.has_value());
    CHECK(waited_segment->size() == 1024);
    CHECK((*waited_segment)[0] == 0x13);

    store->finish();
    REQUIRE(store->wait_ready(10ms));
    auto state = store->snapshot();
    CHECK(state.init_ready);
    CHECK(state.finished);
    CHECK(state.segment_count == 4);
    CHECK(state.highest_requested == 3);

    auto playlist = store->playlist();
    CHECK(playlist == initial_playlist);
    CHECK(playlist.find("#EXT-X-MAP:URI=\"init.mp4\"") != std::string::npos);
    CHECK(playlist.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos);
    CHECK(playlist.find("#EXT-X-PLAYLIST-TYPE:EVENT") == std::string::npos);
    CHECK(playlist.find("segment-000003.m4s") != std::string::npos);
    CHECK(playlist.find("#EXT-X-ENDLIST") != std::string::npos);

    auto init = store->object("init.mp4");
    REQUIRE(init.has_value());
    CHECK(std::string(init->begin(), init->end()) == "init");
    for (int i = 0; i < 4; ++i) {
        std::ostringstream name;
        name << "segment-" << std::setfill('0') << std::setw(6) << i << ".m4s";
        auto segment = store->object(name.str());
        REQUIRE(segment.has_value());
        CHECK(segment->size() == 1024);
        CHECK((*segment)[0] == static_cast<uint8_t>(0x10 + i));
    }
}

void test_http_server_serves_streams_concurrently() {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = 0;
    config.workers = 2;
    config.max_queued_connections = 8;
    config.stream_chunk_bytes = 16 * 1024;
    std::atomic_bool entered{};
    HttpServer server(config, [&](const HttpRequest& request) {
        if (request.path == "/slow") {
            HttpResponse response;
            response.content_type = "application/octet-stream";
            response.stream = std::make_shared<DelayedHttpBody>(entered, 128 * 1024);
            return response;
        }
        if (request.path == "/fast")
            return HttpResponse{200, "text/plain", {}, Bytes{'o', 'k'}};
        return http_error(404, "not_found", "not found");
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() != 0; }, 1s));

    std::string slow_response;
    std::jthread slow([&] { slow_response = raw_http_get(server.bound_port(), "/slow"); });
    REQUIRE(wait_until([&] { return entered.load(); }, 1s));
    auto started = Clock::now();
    auto fast = raw_http_get(server.bound_port(), "/fast");
    auto elapsed = Clock::now() - started;
    CHECK(fast.find("200 OK") != std::string::npos);
    CHECK(fast.ends_with("ok"));
    CHECK(elapsed < 300ms);
    slow.join();
    CHECK(slow_response.find("200 OK") != std::string::npos);
    CHECK(slow_response.size() >= 128 * 1024);
    server.stop();
}

void test_media_vod_index_planning_rejects_partial_indexes() {
    CHECK(media_vod::requires_seek_index_materialisation("matroska,webm"));
    CHECK(media_vod::requires_seek_index_materialisation("webm"));
    CHECK(!media_vod::requires_seek_index_materialisation("mov,mp4,m4a,3gp,3g2,mj2"));

    std::vector<double> complete;
    for (double seconds = 0.0; seconds < 120.0; seconds += 2.0) complete.push_back(seconds);
    auto full = media_vod::indexed_plan(complete, 120.0, 0.0, 4.0);
    REQUIRE(full.has_value());
    CHECK(std::abs(full->actual_seek_seconds) < 0.0005);
    CHECK(full->segment_durations.size() == 30);
    for (const auto duration : full->segment_durations) CHECK(duration <= 4.001);

    // Regression for 0.8.0: avformat_find_stream_info() can leave a Matroska
    // AVStream index containing only keyframes encountered during probing. The
    // old planner accepted that as complete and advertised the entire
    // unindexed tail as one fragment, e.g. segments=1 for a full movie.
    const std::vector<double> partial{0.0, 2.0};
    CHECK(!media_vod::indexed_plan(partial, 120.0, 0.0, 4.0).has_value());

    // A partially populated index must also be rejected when it contains
    // enough early entries to produce several apparently sensible fragments.
    const std::vector<double> partial_with_several_starts{0.0, 4.0, 8.0, 12.0, 16.0};
    CHECK(!media_vod::indexed_plan(partial_with_several_starts, 120.0, 0.0, 4.0).has_value());

    // Sparse but complete GOPs can still be remuxed when they remain within
    // the deliberately generous 3x target-duration bound.
    std::vector<double> sparse_complete;
    for (double seconds = 0.0; seconds < 60.0; seconds += 10.0)
        sparse_complete.push_back(seconds);
    CHECK(media_vod::indexed_plan(sparse_complete, 60.0, 0.0, 4.0).has_value());

    // One fragment is legitimate for genuinely short media; the regression
    // is accepting one fragment for a long presentation with an incomplete
    // index, not the segment count itself.
    const std::vector<double> short_index{0.0};
    auto short_plan = media_vod::indexed_plan(short_index, 6.0, 0.0, 4.0);
    REQUIRE(short_plan.has_value());
    CHECK(short_plan->segment_durations.size() == 1);
    CHECK(std::abs(short_plan->segment_durations.front() - 6.0) < 0.0005);

    auto seeked = media_vod::indexed_plan(complete, 120.0, 61.0, 4.0);
    REQUIRE(seeked.has_value());
    CHECK(std::abs(seeked->actual_seek_seconds - 62.0) < 0.0005);
}

void test_reseek_hls_vod_reuses_prepared_random_access_state() {
    HlsVodPlan remux;
    remux.playback.mode = PlaybackMode::remux;
    remux.playback.video = MediaTransform::copy;
    remux.source_duration_seconds = 120.0;
    remux.seek_segment_seconds = 4.0;
    remux.reusable_seek = true;
    for (double seconds = 0.0; seconds < 120.0; seconds += 2.0)
        remux.video_random_access_points.push_back(seconds);

    auto remux_seek = reseek_hls_vod(remux, 61s);
    REQUIRE(remux_seek.has_value());
    CHECK(remux_seek->playback.seek == 62s);
    REQUIRE(!remux_seek->segment_durations.empty());
    CHECK(remux_seek->segment_durations.front() <= 4.001);

    HlsVodPlan transcode;
    transcode.playback.mode = PlaybackMode::transcode;
    transcode.playback.video = MediaTransform::transcode;
    transcode.source_duration_seconds = 120.0;
    transcode.seek_segment_seconds = 4.0;
    transcode.reusable_seek = true;

    auto transcode_seek = reseek_hls_vod(transcode, 61s);
    REQUIRE(transcode_seek.has_value());
    CHECK(transcode_seek->playback.seek == 61s);
    REQUIRE(!transcode_seek->segment_durations.empty());
    CHECK(std::abs(transcode_seek->segment_durations.front() - 4.0) < 0.0005);

    HlsVodPlan unavailable;
    CHECK(!reseek_hls_vod(unavailable, 10s).has_value());
}

void test_media_timestamp_repair() {
    MediaTimestampRepairState state;

    MediaPacketTimestamps first{-69952, -69952, 40};
    normalize_media_timestamps(state, first);
    CHECK(first.pts == -69952);
    CHECK(first.dts == -69952);
    CHECK(state.repair_count() == 0);

    // This is the exact failure shape seen from the MP4 muxer: two packets
    // arrive with equal DTS after a seek/rescale. The second one must advance
    // and the same correction must remain applied to later source timestamps.
    MediaPacketTimestamps equal{-69912, -69952, 40};
    normalize_media_timestamps(state, equal);
    CHECK(equal.dts == -69951);
    CHECK(equal.pts == -69911);
    CHECK(state.nonmonotonic_dts == 1);
    CHECK(state.timeline_shift == 1);

    MediaPacketTimestamps following{-69872, -69912, 40};
    normalize_media_timestamps(state, following);
    CHECK(following.dts == -69911);
    CHECK(following.pts == -69871);
    CHECK(state.nonmonotonic_dts == 1);

    MediaTimestampRepairState missing;
    MediaPacketTimestamps none{kNoMediaTimestamp, kNoMediaTimestamp, 0};
    normalize_media_timestamps(missing, none);
    CHECK(none.dts == 0);
    CHECK(none.pts == 0);
    CHECK(none.duration == 1);
    CHECK(missing.missing_dts == 1);
    CHECK(missing.missing_pts == 1);

    MediaPacketTimestamps missing_dts{100, kNoMediaTimestamp, 40};
    normalize_media_timestamps(missing, missing_dts);
    CHECK(missing_dts.dts == 1);
    CHECK(missing_dts.pts == 100);
    CHECK(missing.missing_dts == 2);

    MediaTimestampRepairState bad_pts;
    MediaPacketTimestamps pts_before{4, 5, 1};
    normalize_media_timestamps(bad_pts, pts_before);
    CHECK(pts_before.pts == 4);
    CHECK(pts_before.dts == 5);
    CHECK(bad_pts.pts_before_dts == 1);
    CHECK(bad_pts.repair_count() == 0);

    int64_t encoder_pts = kNoMediaTimestamp;
    CHECK(normalize_encoder_pts(encoder_pts, kNoMediaTimestamp) == kNoMediaTimestamp);
    CHECK(encoder_pts == kNoMediaTimestamp);
    CHECK(normalize_encoder_pts(encoder_pts, 100) == 100);
    CHECK(normalize_encoder_pts(encoder_pts, 100) == 101);
    CHECK(normalize_encoder_pts(encoder_pts, 99) == 102);
    CHECK(normalize_encoder_pts(encoder_pts, 140) == 140);
}

void test_playback_probe_failure_is_stage_specific() {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_replication = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();

    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/test.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/test.mp4", true);
    auto bytes = pattern(64 * 1024);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/test.mp4"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.probe_timeout = 2s;
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::make_unique<FailingProbeMediaEngine>());
    playback.start();

    Json::Object root{{"media_id", media_id}};
    auto text = Json(std::move(root)).dump();
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/playback/sessions";
    request.body.assign(text.begin(), text.end());
    auto response = playback.handle(request);
    REQUIRE(response.status == 503);
    auto body = Json::parse(std::string(response.body.begin(), response.body.end()));
    CHECK(body.find("error")->asString() == "playback_probe_failed");
    CHECK(body.find("stage")->asString() == "probe");
    REQUIRE(body.find("trace") != nullptr);
    CHECK(!body.find("trace")->asString().empty());
    REQUIRE(response.headers.contains("X-Macha-Playback-Trace"));
    REQUIRE(response.headers.contains("X-Macha-Playback-Stage"));
    CHECK(response.headers.at("X-Macha-Playback-Stage") == "probe");

    playback.stop();
    service.stop();
}

void test_concurrent_transcode_admission_is_reserved() {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_replication = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();

    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/test.mkv", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/test.mkv", true);
    auto bytes = pattern(64 * 1024);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/test.mkv"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.max_sessions = 4;
    streaming.max_video_transcodes = 1;
    streaming.max_audio_transcodes = 1;
    streaming.startup_timeout = 2s;
    auto engine = std::make_unique<BlockingMediaEngine>();
    auto* blocking = engine.get();
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::move(engine));
    playback.start();

    auto request_for = [&](const std::string& id) {
        Json::Object preferences{{"mode", "transcode"}};
        Json::Object root{{"media_id", id}, {"preferences", Json(std::move(preferences))}};
        auto text = Json(std::move(root)).dump();
        HttpRequest request;
        request.method = "POST";
        request.path = "/api/v1/playback/sessions";
        request.body.assign(text.begin(), text.end());
        return request;
    };

    HttpResponse first_response;
    std::jthread first([&] { first_response = playback.handle(request_for(media_id)); });
    REQUIRE(wait_until([&] { return blocking->starts() == 1; }, 1s));

    auto second_response = playback.handle(request_for(media_id));
    CHECK(second_response.status == 429);
    CHECK(blocking->starts() == 1);

    blocking->release();
    first.join();
    REQUIRE(first_response.status == 201);
    auto first_json = Json::parse(std::string(first_response.body.begin(), first_response.body.end()));
    HttpRequest remove;
    remove.method = "DELETE";
    remove.path = "/api/v1/playback/sessions/" + first_json.find("session_id")->asString();
    CHECK(playback.handle(remove).status == 204);

    playback.stop();
    service.stop();
}

void test_playback_sessions_and_streaming_http_bodies() {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();
    auto c = config_for(t.path() / "node", keyfile, port);
    c.replication = 1;
    c.metadata_replication = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();

    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/test.mp4", 0644, getuid(), getgid());
    auto bytes = pattern(512 * 1024 + 37);
    auto writer = service.filesystem().open_write("/media/test.mp4", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/test.mp4"));

    CatalogueApiConfig api;
    api.stream_chunk_bytes = 64 * 1024;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.max_sessions = 4;
    streaming.max_video_transcodes = 1;
    streaming.max_audio_transcodes = 1;
    streaming.startup_timeout = 2s;
    auto fake_engine = std::make_unique<FakeMediaEngine>();
    auto* fake_engine_ptr = fake_engine.get();
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::move(fake_engine));
    playback.start();

    HttpRequest playback_status_request;
    playback_status_request.method = "GET";
    playback_status_request.path = "/api/v1/playback/status";
    auto playback_status_response = playback.handle(playback_status_request);
    REQUIRE(playback_status_response.status == 200);
    auto playback_status_json = Json::parse(std::string(playback_status_response.body.begin(),
                                                        playback_status_response.body.end()));
    REQUIRE(playback_status_json.find("server_version") != nullptr);
    CHECK(playback_status_json.find("server_version")->asString() == "0.8.5");

    // A transformed stream can begin at its resume point in the initial POST.
    // This avoids creating a generation at zero only to destroy it immediately
    // with a PATCH before the player has loaded anything.
    Json::Object initial_seek_preferences{{"mode", "remux"}};
    Json::Object initial_seek_root{{"media_id", media_id},
                                   {"seek_ms", 23000},
                                   {"preferences", Json(std::move(initial_seek_preferences))}};
    auto initial_seek_text = Json(std::move(initial_seek_root)).dump();
    HttpRequest initial_seek;
    initial_seek.method = "POST";
    initial_seek.path = "/api/v1/playback/sessions";
    initial_seek.body.assign(initial_seek_text.begin(), initial_seek_text.end());
    auto initial_seek_response = playback.handle(initial_seek);
    REQUIRE(initial_seek_response.status == 201);
    auto initial_seek_json = Json::parse(std::string(initial_seek_response.body.begin(),
                                                     initial_seek_response.body.end()));
    CHECK(initial_seek_json.find("mode")->asString() == "remux");
    CHECK(initial_seek_json.find("seek_ms")->asInt64() == 23000);
    auto plans = fake_engine_ptr->started_plans();
    REQUIRE(plans.size() == 1);
    CHECK(plans.back().seek == 23s);

    // A transformed seek-only PATCH must reuse the already prepared VOD plan.
    // Re-probing/re-planning here is the 0.8.1 behaviour that made cached
    // seeks take several seconds on Matroska media.
    const auto probes_before_seek = fake_engine_ptr->probes();
    const auto prepares_before_seek = fake_engine_ptr->vod_prepares();
    Json::Object fast_seek_root{{"seek_ms", 35000}};
    auto fast_seek_text = Json(std::move(fast_seek_root)).dump();
    HttpRequest fast_seek;
    fast_seek.method = "PATCH";
    fast_seek.path = "/api/v1/playback/sessions/" + initial_seek_json.find("session_id")->asString();
    fast_seek.body.assign(fast_seek_text.begin(), fast_seek_text.end());
    auto fast_seek_response = playback.handle(fast_seek);
    REQUIRE(fast_seek_response.status == 200);
    auto fast_seek_json = Json::parse(std::string(fast_seek_response.body.begin(),
                                                  fast_seek_response.body.end()));
    CHECK(fast_seek_json.find("seek_ms")->asInt64() == 35000);
    CHECK(fake_engine_ptr->probes() == probes_before_seek);
    CHECK(fake_engine_ptr->vod_prepares() == prepares_before_seek);
    plans = fake_engine_ptr->started_plans();
    REQUIRE(plans.size() == 2);
    CHECK(plans.back().seek == 35s);

    HttpRequest remove_initial_seek;
    remove_initial_seek.method = "DELETE";
    remove_initial_seek.path = "/api/v1/playback/sessions/" + initial_seek_json.find("session_id")->asString();
    CHECK(playback.handle(remove_initial_seek).status == 204);

    Json::Object create_root{{"media_id", media_id}};
    auto create_text = Json(std::move(create_root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.body.assign(create_text.begin(), create_text.end());
    auto created = playback.handle(create);
    REQUIRE(created.status == 201);
    auto created_json = Json::parse(std::string(created.body.begin(), created.body.end()));
    CHECK(created_json.find("mode")->asString() == "direct");
    CHECK(created_json.find("subtitle_url")->isNull());
    REQUIRE(created_json.find("options") != nullptr);
    auto options = created_json.find("options");
    REQUIRE(options->find("audio_streams") != nullptr);
    REQUIRE(options->find("audio_streams")->isArray());
    REQUIRE(!options->find("audio_streams")->asArray().empty());
    CHECK(options->find("audio_streams")->asArray().front().isObject());
    REQUIRE(options->find("media_ids") != nullptr);
    CHECK(options->find("media_ids")->asArray().size() == 1);
    auto session_id = created_json.find("session_id")->asString();
    auto direct_url = created_json.find("stream_url")->asString();

    HttpRequest direct;
    direct.method = "GET";
    direct.path = direct_url;
    direct.headers["range"] = "bytes=100-1099";
    auto direct_response = playback.handle(direct);
    REQUIRE(direct_response.status == 206);
    REQUIRE(direct_response.stream != nullptr);
    CHECK(direct_response.content_length() == 1000);
    Bytes direct_bytes(1000);
    REQUIRE(direct_response.stream->read(0, direct_bytes) == direct_bytes.size());
    CHECK(std::equal(direct_bytes.begin(), direct_bytes.end(), bytes.begin() + 100));

    Json::Object bad_track_preferences{{"audio_stream", 99}};
    Json::Object bad_track_root{{"preferences", Json(std::move(bad_track_preferences))}};
    auto bad_track_text = Json(std::move(bad_track_root)).dump();
    HttpRequest bad_track;
    bad_track.method = "PATCH";
    bad_track.path = "/api/v1/playback/sessions/" + session_id;
    bad_track.body.assign(bad_track_text.begin(), bad_track_text.end());
    CHECK(playback.handle(bad_track).status == 400);

    Json::Object unsupported_caps{{"containers", Json::Array{}},
                                  {"video_codecs", Json::Array{Json("vp9")}},
                                  {"audio_codecs", Json::Array{Json("opus")}},
                                  {"hls_fmp4", true}};
    Json::Object unsupported_prefs{{"mode", "transcode"}};
    Json::Object unsupported_root{{"media_id", media_id},
                                  {"capabilities", Json(std::move(unsupported_caps))},
                                  {"preferences", Json(std::move(unsupported_prefs))}};
    auto unsupported_text = Json(std::move(unsupported_root)).dump();
    HttpRequest unsupported;
    unsupported.method = "POST";
    unsupported.path = "/api/v1/playback/sessions";
    unsupported.body.assign(unsupported_text.begin(), unsupported_text.end());
    CHECK(playback.handle(unsupported).status == 400);

    Json::Object preferences{{"mode", "transcode"}, {"subtitle_stream", 2}};
    Json::Object patch_root{{"preferences", Json(std::move(preferences))}, {"seek_ms", 12000}};
    auto patch_text = Json(std::move(patch_root)).dump();
    HttpRequest patch;
    patch.method = "PATCH";
    patch.path = "/api/v1/playback/sessions/" + session_id;
    patch.body.assign(patch_text.begin(), patch_text.end());
    auto patched = playback.handle(patch);
    REQUIRE(patched.status == 200);
    auto patched_json = Json::parse(std::string(patched.body.begin(), patched.body.end()));
    CHECK(patched_json.find("mode")->asString() == "transcode");
    auto hls_url = patched_json.find("stream_url")->asString();
    auto subtitle_url = patched_json.find("subtitle_url")->asString();

    HttpRequest playlist;
    playlist.method = "GET";
    playlist.path = hls_url;
    auto playlist_response = playback.handle(playlist);
    REQUIRE(playlist_response.status == 200);
    REQUIRE(playlist_response.stream != nullptr);
    Bytes playlist_bytes(static_cast<size_t>(playlist_response.content_length()));
    REQUIRE(playlist_response.stream->read(0, playlist_bytes) == playlist_bytes.size());
    CHECK(std::string(playlist_bytes.begin(), playlist_bytes.end()).find("#EXTM3U") != std::string::npos);

    HttpRequest subtitle;
    subtitle.method = "GET";
    subtitle.path = subtitle_url;
    auto subtitle_response = playback.handle(subtitle);
    REQUIRE(subtitle_response.status == 200);
    CHECK(subtitle_response.content_type.starts_with("text/vtt"));

    Json::Object second_preferences{{"mode", "transcode"}};
    Json::Object second_root{{"media_id", media_id}, {"preferences", Json(std::move(second_preferences))}};
    auto second_text = Json(std::move(second_root)).dump();
    HttpRequest second;
    second.method = "POST";
    second.path = "/api/v1/playback/sessions";
    second.body.assign(second_text.begin(), second_text.end());
    auto limited = playback.handle(second);
    CHECK(limited.status == 429);

    HttpRequest remove;
    remove.method = "DELETE";
    remove.path = "/api/v1/playback/sessions/" + session_id;
    CHECK(playback.handle(remove).status == 204);

    // A playback lease is a snapshot, not a pathname alias. Replacing the file
    // after resolve must not switch bytes underneath an already-running direct stream.
    Json::Object path_root{{"media_id", "path:/media/test.mp4"}};
    auto path_text = Json(std::move(path_root)).dump();
    HttpRequest path_create;
    path_create.method = "POST";
    path_create.path = "/api/v1/playback/sessions";
    path_create.body.assign(path_text.begin(), path_text.end());
    auto path_created = playback.handle(path_create);
    REQUIRE(path_created.status == 201);
    auto path_json = Json::parse(std::string(path_created.body.begin(), path_created.body.end()));
    auto path_session_id = path_json.find("session_id")->asString();
    auto path_stream_url = path_json.find("stream_url")->asString();

    auto replacement_bytes = bytes;
    for (auto& byte : replacement_bytes) byte ^= 0x5a;
    auto replacement_writer = service.filesystem().open_write("/media/test.mp4", true);
    REQUIRE(replacement_writer->write(0, replacement_bytes) == replacement_bytes.size());
    replacement_writer->commit();

    HttpRequest pinned;
    pinned.method = "GET";
    pinned.path = path_stream_url;
    pinned.headers["range"] = "bytes=0-255";
    auto pinned_response = playback.handle(pinned);
    REQUIRE(pinned_response.status == 206);
    Bytes pinned_bytes(256);
    REQUIRE(pinned_response.stream->read(0, pinned_bytes) == pinned_bytes.size());
    CHECK(std::equal(pinned_bytes.begin(), pinned_bytes.end(), bytes.begin()));

    HttpRequest remove_path;
    remove_path.method = "DELETE";
    remove_path.path = "/api/v1/playback/sessions/" + path_session_id;
    CHECK(playback.handle(remove_path).status == 204);
    playback.stop();
    service.stop();
}

} // namespace

int main() {
    try {
        test_codec_and_crypto();
        test_local_store();
        test_storage_pool_and_persistent_cache();
        test_metadata_codec_and_replica();
        test_config();
        test_placement();
        test_capacity_placement();
        test_async_rpc_move_ownership();
        test_rpc_v10_frame_priority_and_variable_length();
        test_repair_step_is_bounded_and_yields();
        test_rpc_v10_persistence_and_multiplexing();
        test_rpc_v10_bidirectional_and_deduplication();
        test_mutual_bootstrap_prunes_cross_dial();
        test_rpc_v7_handshake_is_rejected();
        test_rpc_slow_control_does_not_abort_data();
        test_rpc_health_and_control_not_starved_by_data();
        test_early_replication_quorum();
        test_joiner_cannot_form_genesis();
        test_two_node_mutual_bootstrap_metadata_quorum();
        test_replication_policy_change_on_restart();
        test_genesis_root_configuration();
        test_open_write_metadata_merge();
        test_active_write_size_visibility();
        test_open_write_survives_rename();
        test_full_replica_fallback();
        test_replacement_node_recovers_namespace_and_replication();
        test_hydration_scheduler_and_prediction();
        test_replica_selector();
        test_cache_hydrator_fetches_to_persistent_cache();
        test_media_probe_and_online_catalogue_scanner();
        test_catalogue_cache_ignores_unrelated_metadata_generation();
        test_media_index_cache_survives_namespace_churn();
        test_catalogue_sync_search_and_artwork_gc();
        test_media_segment_store_backpressure_and_spill();
        test_media_vod_index_planning_rejects_partial_indexes();
        test_reseek_hls_vod_reuses_prepared_random_access_state();
        test_media_timestamp_repair();
        test_http_server_serves_streams_concurrently();
        test_playback_probe_failure_is_stage_specific();
        test_concurrent_transcode_admission_is_reserved();
        test_playback_sessions_and_streaming_http_bodies();
        test_three_node_cluster();
    } catch (const std::exception& e) {
        std::cerr << "Unhandled test exception: " << e.what() << '\n';
        return 2;
    }
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}

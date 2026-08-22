// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "filesystem.hpp"
#include "fuse_frontend.hpp"
#include "http.hpp"
#include "local_store.hpp"
#include "macha_version.hpp"
#include "metadata.hpp"
#include "media_catalogue.hpp"
#include "macos_unicode.hpp"
#include "media_timestamps.hpp"
#include "media_vod.hpp"
#include "net.hpp"
#include "placement.hpp"
#include "service.hpp"
#include "storage_pool.hpp"
#include "subtitle_text.hpp"
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
extern "C" {
#include <libavutil/log.h>
}
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



template <typename Fn>
void run_test_case(std::string_view name, Fn&& fn) {
    const auto started = std::chrono::steady_clock::now();
    const auto failures_before = failures;
    std::cout << "[TEST] START " << name << '\n' << std::flush;
    try {
        fn();
    } catch (const std::exception& e) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::cerr << "[TEST] END " << name << " status=FAIL elapsed_ms=" << elapsed.count()
                  << " exception=\"" << e.what() << "\"\n";
        throw;
    } catch (...) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::cerr << "[TEST] END " << name << " status=FAIL elapsed_ms=" << elapsed.count()
                  << " exception=unknown\n";
        throw;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const auto new_failures = failures - failures_before;
    if (new_failures == 0) {
        std::cout << "[TEST] END " << name << " status=PASS elapsed_ms=" << elapsed.count()
                  << '\n' << std::flush;
    } else {
        std::cerr << "[TEST] END " << name << " status=FAIL elapsed_ms=" << elapsed.count()
                  << " checks=" << new_failures << '\n';
    }
}

#define RUN_TEST(test_fn) run_test_case(#test_fn, [] { test_fn(); })

class CapturingLogger final : public Logger {
    LogLevel level_;

  public:
    std::vector<std::pair<LogLevel, std::string>> records;

    explicit CapturingLogger(LogLevel level) : level_(level) {}

    bool enabled(LogLevel level) const noexcept override {
        return level_ == LogLevel::all || static_cast<unsigned char>(level) >= static_cast<unsigned char>(level_);
    }

    void log(LogLevel level, const std::string& message) override {
        records.emplace_back(level, message);
    }
};

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
    std::atomic_uint subtitle_segments_{};
  public:
    MediaEngineStatus status() const override { return {true, "fake", "fake-media-engine", true, true}; }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds = {}) override {
        ++probes_;
        MediaProbeResult result;
        result.format = "mov,mp4,m4a,3gp,3g2,mj2";
        result.duration_seconds = 60.0;
        result.bitrate = 4'000'000;
        result.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920, 1080, 0, 0, 8, true, false, 3'700'000});
        result.streams.push_back(MediaStreamInfo{1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0, 2, 48000, 0, true, false, 192'000});
        result.streams.push_back(MediaStreamInfo{2, MediaStreamType::subtitle, "subrip", "", "eng", 0, 0, 0, 0, 0, false, false});
        result.streams.push_back(MediaStreamInfo{3, MediaStreamType::subtitle, "hdmv_pgs_subtitle", "", "eng", 0, 0, 0, 0, 0, false, false});
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
    std::string extract_webvtt_segment(const MediaSource&, int,
                                       std::chrono::milliseconds range_start,
                                       std::chrono::milliseconds range_end,
                                       std::chrono::milliseconds timeline_origin) override {
        ++subtitle_segments_;
        (void)range_start;
        (void)range_end;
        (void)timeline_origin;
        return "WEBVTT\n\n00:00.000 --> 00:01.000\nsubtitle\n";
    }
    std::vector<PlaybackPlan> started_plans() const {
        std::lock_guard lock(mutex_);
        return started_plans_;
    }
    unsigned probes() const { return probes_.load(); }
    unsigned vod_prepares() const { return vod_prepares_.load(); }
    unsigned subtitle_segments() const { return subtitle_segments_.load(); }
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
    std::string extract_webvtt_segment(const MediaSource&, int, std::chrono::milliseconds,
                                       std::chrono::milliseconds, std::chrono::milliseconds) override {
        throw std::runtime_error("extract_webvtt_segment must not be called after a failed probe");
    }
};


class AttachedPictureAudioEngine final : public MediaEngine {
  public:
    MediaEngineStatus status() const override { return {true, "fake", "attached-picture-audio", true, true}; }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds = {}) override {
        MediaProbeResult result;
        result.format = "mp3";
        result.duration_seconds = 180.0;
        result.bitrate = 192'000;
        MediaStreamInfo picture;
        picture.index = 0;
        picture.type = MediaStreamType::video; // defensive path: disposition must still make this non-playable
        picture.codec = "png";
        picture.width = 600;
        picture.height = 600;
        picture.attached_picture = true;
        result.streams.push_back(picture);
        MediaStreamInfo audio;
        audio.index = 1;
        audio.type = MediaStreamType::audio;
        audio.codec = "mp3";
        audio.channels = 2;
        audio.sample_rate = 44100;
        audio.default_stream = true;
        audio.bitrate = 192'000;
        result.streams.push_back(audio);
        return result;
    }
    HlsVodPlan prepare_hls_vod(const MediaSource&, const PlaybackPlan&, double,
                               std::chrono::milliseconds, bool, std::chrono::milliseconds = {}) override {
        throw std::runtime_error("attached-picture MP3 should direct-play without HLS planning");
    }
    std::unique_ptr<MediaEngineSession> start_hls(const MediaSource&, const HlsVodPlan&,
                                                  std::chrono::milliseconds, size_t, uint64_t,
                                                  const std::filesystem::path&) override {
        throw std::runtime_error("attached-picture MP3 should direct-play without HLS startup");
    }
    std::string extract_webvtt_segment(const MediaSource&, int, std::chrono::milliseconds,
                                       std::chrono::milliseconds, std::chrono::milliseconds) override {
        return {};
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
        result.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920, 1080, 0, 0, 8, true, false, 3'700'000});
        result.streams.push_back(MediaStreamInfo{1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0, 2, 48000, 0, true, false, 192'000});
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
    std::string extract_webvtt_segment(const MediaSource&, int, std::chrono::milliseconds,
                                       std::chrono::milliseconds, std::chrono::milliseconds) override { return {}; }
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
    std::atomic_size_t requests_{};
    mutable std::mutex urls_mutex_;
    std::vector<std::string> urls_;

  public:
    size_t requests() const noexcept { return requests_.load(); }
    size_t requests_containing(std::string_view needle) const {
        std::lock_guard lock(urls_mutex_);
        return static_cast<size_t>(std::count_if(urls_.begin(), urls_.end(), [&](const auto& url) {
            return url.find(needle) != std::string::npos;
        }));
    }
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
        requests_.fetch_add(1);
        {
            std::lock_guard lock(urls_mutex_);
            urls_.emplace_back(url);
        }
        for (const auto& route : routes_) {
            if (url.find(route.contains) != std::string_view::npos)
                return route.response;
        }
        return RemoteHttpResponse{404, "text/plain", {}};
    }
};

class BlockingHttpClient final : public HttpClient {
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic_bool entered_{};
    std::atomic_bool stopped_{};

  public:
    bool entered() const { return entered_.load(); }
    bool stopped() const { return stopped_.load(); }
    void request_stop() noexcept override {
        stopped_.store(true);
        cv_.notify_all();
    }
    void reset_stop() noexcept override { stopped_.store(false); }
    bool stop_requested() const noexcept override { return stopped_.load(); }
    RemoteHttpResponse get(std::string_view, const std::vector<std::string>&, size_t) override {
        entered_.store(true);
        cv_.notify_all();
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stopped_.load(); });
        throw std::runtime_error("synthetic HTTP cancellation");
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

    // Reachability GC is a separate bounded physical cursor. It preserves live
    // and recently-retired objects, ignores young uncommitted objects, and
    // removes an old orphan that has no committed reference or tombstone.
    auto put_gc_object = [&](uint8_t tag) {
        auto data = pattern(96 * 1024 + tag);
        data[0] ^= tag;
        auto id = object_id(data);
        REQUIRE(pool.put(id, data));
        return std::pair<ObjectId, Bytes>{id, std::move(data)};
    };
    auto gc_live_object = put_gc_object(0x31);
    auto gc_protected_object = put_gc_object(0x32);
    auto gc_orphan_object = put_gc_object(0x33);
    auto gc_young_object = put_gc_object(0x34);
    const auto gc_live = gc_live_object.first;
    const auto gc_protected = gc_protected_object.first;
    const auto gc_orphan = gc_orphan_object.first;
    const auto gc_young = gc_young_object.first;
    auto age_object = [&](const ObjectId& id) {
        for (const auto& disk : {disk1, disk2}) {
            auto path = object_path(disk, id);
            if (std::filesystem::exists(path))
                std::filesystem::last_write_time(
                    path, std::filesystem::file_time_type::clock::now() - 48h);
        }
    };
    age_object(gc_live);
    age_object(gc_protected);
    age_object(gc_orphan);

    std::vector<ObjectId> gc_live_set{gc_live};
    std::vector<ObjectId> gc_protected_set{gc_protected};
    std::sort(gc_live_set.begin(), gc_live_set.end());
    std::sort(gc_protected_set.begin(), gc_protected_set.end());
    bool gc_complete = false;
    size_t gc_slices = 0;
    uint64_t gc_reclaimed = 0;
    while (!gc_complete && gc_slices++ < 256) {
        auto step = pool.gc_step(gc_live_set, gc_protected_set, 24h, 3);
        CHECK(step.objects <= 3);
        gc_reclaimed += step.bytes;
        gc_complete = step.complete;
    }
    CHECK(gc_complete);
    CHECK(gc_reclaimed > 0);
    CHECK(pool.has(gc_live));
    CHECK(pool.has(gc_protected));
    CHECK(!pool.has(gc_orphan));
    CHECK(pool.has(gc_young));
    auto gc_yielded = pool.gc_step(gc_live_set, gc_protected_set, 24h, 1, [] { return true; });
    CHECK(gc_yielded.yielded);
    CHECK(gc_yielded.objects == 0);

    // Re-putting an identical hash reaffirms its physical age. This closes the
    // race where a new uncommitted write reuses an ancient orphan already on an owner.
    auto gc_reaffirmed_object = put_gc_object(0x35);
    const auto gc_reaffirmed = gc_reaffirmed_object.first;
    age_object(gc_reaffirmed);
    CHECK(pool.older_than(gc_reaffirmed, 24h));
    REQUIRE(pool.put(gc_reaffirmed, gc_reaffirmed_object.second));
    CHECK(!pool.older_than(gc_reaffirmed, 24h));

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
    const std::string unicode_dir = "/Music/Caf\xc3\xa9 del Mar pack 1 (1999-2004)";
    const std::string unicode_file =
        unicode_dir + "/01.Clannad - Na Buachaill\xc3\xad lainn.mp3";
    FsEntry unicode_directory_entry;
    unicode_directory_entry.type = EntryType::directory;
    snap.entries[unicode_dir] = unicode_directory_entry;
    snap.entries[unicode_file] = file;
    auto garbage_id = object_id(pattern(4096));
    auto retirement_id = random_node_id();
    snap.garbage.push_back({garbage_id, 123456789, retirement_id});
    auto encoded = encode_snapshot(snap);
    auto decoded = decode_snapshot(encoded);
    CHECK(decoded.metadata_voters == snap.metadata_voters);
    CHECK(decoded.mutation_sequences == snap.mutation_sequences);
    CHECK(decoded.entries.at("/movie.mkv").size == 123);
    REQUIRE(decoded.entries.contains(unicode_dir));
    REQUIRE(decoded.entries.contains(unicode_file));
    CHECK(decoded.entries.at(unicode_dir).type == EntryType::directory);
    CHECK(decoded.entries.at(unicode_file).type == EntryType::file);
    // Persistent metadata remains byte-preserving. The macOS FUSE adapter owns
    // the platform presentation rule and decomposes only names returned to macFUSE.
    const std::string cafe_name = "Caf\xc3\xa9 del Mar";
#if defined(__APPLE__)
    CHECK(macos_fuse_decomposed_name(cafe_name) == "Cafe\xcc\x81 del Mar");
    CHECK(macos_fuse_decomposed_name("Na Buachaill\xc3\xad lainn.mp3") ==
          "Na Buachailli\xcc\x81 lainn.mp3");
    CHECK(macos_fuse_composed_name("Cafe\xcc\x81 del Mar") == cafe_name);
    CHECK(macos_fuse_composed_name("Na Buachailli\xcc\x81 lainn.mp3") ==
          "Na Buachaill\xc3\xad lainn.mp3");
#else
    CHECK(macos_fuse_decomposed_name(cafe_name) == cafe_name);
#endif
    REQUIRE(decoded.garbage.size() == 1);
    CHECK(decoded.garbage.front().id == garbage_id);
    CHECK(decoded.garbage.front().retired_at_ns == 123456789);
    CHECK(decoded.garbage.front().retirement_id == retirement_id);

    auto delta_target = decoded;
    delta_target.mutation_sequences[a] = 8;
    delta_target.entries.at("/movie.mkv").size = 456;
    FsEntry extra;
    extra.type = EntryType::file;
    extra.size = 999;
    delta_target.entries["/extra.mkv"] = extra;
    auto catalogue_id = object_id(pattern(1024));
    delta_target.catalogue_root = catalogue_id;
    auto garbage_id_2 = object_id(pattern(2048));
    delta_target.garbage.push_back({garbage_id_2, 987654321, random_node_id()});
    auto compact = metadata_delta(decoded, delta_target);
    REQUIRE(compact.has_value());
    auto encoded_delta = encode_metadata_delta(*compact);
    auto decoded_delta = decode_metadata_delta(encoded_delta);
    auto reconstructed = apply_metadata_delta(decoded, decoded_delta);
    CHECK(encode_snapshot(reconstructed) == encode_snapshot(delta_target));
    CHECK(encoded_delta.size() < encode_snapshot(delta_target).size());

    // DLT2 represents tombstone replacement and pruning directly. This is the
    // ordinary 0.10.x path used to stamp legacy records and bound garbage metadata.
    auto garbage_compacted = delta_target;
    garbage_compacted.garbage.erase(garbage_compacted.garbage.begin());
    garbage_compacted.garbage.front().retired_at_ns += 1;
    garbage_compacted.garbage.front().retirement_id = random_node_id();
    auto garbage_delta = metadata_delta(delta_target, garbage_compacted);
    REQUIRE(garbage_delta.has_value());
    CHECK(garbage_delta->erase_garbage.size() == 1);
    CHECK(garbage_delta->upsert_garbage.size() == 1);
    auto garbage_delta_roundtrip = decode_metadata_delta(encode_metadata_delta(*garbage_delta));
    CHECK(encode_snapshot(apply_metadata_delta(delta_target, garbage_delta_roundtrip)) ==
          encode_snapshot(garbage_compacted));

    // 0.9.4 SM7 snapshots remain valid on disk. Their tombstones intentionally
    // decode as legacy (no retirement time/id) and are stamped by 0.10.x GC.
    Writer old_v7;
    const std::array<uint8_t, 8> old_v7_magic{'D', 'H', 'T', 'M', 'E', 'T', 'A', '7'};
    old_v7.raw(old_v7_magic);
    old_v7.u32(1);
    old_v7.fixed(a.bytes);
    old_v7.u32(1);
    old_v7.u64(4ULL * 1024 * 1024);
    old_v7.u32(1);
    old_v7.fixed(a.bytes);
    old_v7.u64(7);
    old_v7.u32(1);
    old_v7.string("/");
    old_v7.u8(static_cast<uint8_t>(EntryType::directory));
    old_v7.u32(0755);
    old_v7.u32(0);
    old_v7.u32(0);
    old_v7.u64(0);
    old_v7.i64(0);
    old_v7.i64(0);
    old_v7.u64(1);
    old_v7.u32(0);
    old_v7.u8(0);
    old_v7.u32(1);
    old_v7.fixed(garbage_id.bytes);
    auto upgraded_v7 = decode_snapshot(old_v7.data());
    REQUIRE(upgraded_v7.garbage.size() == 1);
    CHECK(upgraded_v7.garbage.front().id == garbage_id);
    CHECK(upgraded_v7.garbage.front().retired_at_ns == 0);
    CHECK(upgraded_v7.garbage.front().retirement_id == NodeId{});

    // DLT1 is accepted only as a persisted-journal format compatibility path.
    // New encoders always emit DLT2; old 0.9.x journal records still replay.
    Writer old_delta;
    const std::array<uint8_t, 8> old_delta_magic{'D', 'H', 'T', 'M', 'D', 'L', 'T', '1'};
    old_delta.raw(old_delta_magic);
    old_delta.u32(0);
    old_delta.u32(0);
    old_delta.u32(0);
    old_delta.u8(static_cast<uint8_t>(CatalogueDelta::unchanged));
    old_delta.u32(1);
    auto legacy_delta_id = object_id(pattern(3072));
    old_delta.fixed(legacy_delta_id.bytes);
    auto decoded_old_delta = decode_metadata_delta(old_delta.data());
    REQUIRE(decoded_old_delta.upsert_garbage.size() == 1);
    CHECK(decoded_old_delta.upsert_garbage.front().id == legacy_delta_id);
    CHECK(decoded_old_delta.upsert_garbage.front().retired_at_ns == 0);
    CHECK(decoded_old_delta.upsert_garbage.front().retirement_id == NodeId{});

    // Storage compatibility includes the authenticated delta journal, not just
    // accepting old payloads in isolation. DLT1 successor hashes were computed
    // over SM7 bytes, so replay must reconstruct that exact historical encoding.
    auto legacy_journal_path = t.path() / "legacy-journal-node";
    MetadataReplica legacy_journal(legacy_journal_path, keys.storage);
    MetadataRecord legacy_seed;
    legacy_seed.generation = 17;
    legacy_seed.previous = object_id(pattern(211));
    legacy_seed.payload = old_v7.data();
    legacy_seed.hash = metadata_hash(legacy_seed.generation, legacy_seed.previous,
                                     legacy_seed.payload);
    REQUIRE(legacy_journal.seed(legacy_seed));
    REQUIRE(legacy_journal.remember_current_committed(legacy_seed.generation,
                                                       legacy_seed.hash));
    MetadataRecord legacy_successor;
    REQUIRE(legacy_journal.cas_delta(legacy_seed.generation, legacy_seed.hash,
                                     old_delta.data(), &legacy_successor));
    REQUIRE(legacy_journal.remember_current_committed(legacy_successor.generation,
                                                       legacy_successor.hash));
    CHECK(std::equal(legacy_successor.payload.begin(), legacy_successor.payload.begin() + 8,
                     old_v7_magic.begin()));
    {
        MetadataReplica replayed_legacy(legacy_journal_path, keys.storage);
        CHECK(replayed_legacy.current().hash == legacy_successor.hash);
        CHECK(replayed_legacy.committed().hash == legacy_successor.hash);
        auto replayed_snapshot = decode_snapshot(replayed_legacy.current().payload);
        REQUIRE(replayed_snapshot.garbage.size() == 2);
        CHECK(replayed_snapshot.garbage.back().id == legacy_delta_id);
        CHECK(replayed_snapshot.garbage.back().retired_at_ns == 0);
    }

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
    CHECK(replica.current_identity().generation == current.generation);
    CHECK(replica.current_identity().hash == current.hash);
    CHECK(replica.committed_identity().generation == replica.committed().generation);
    CHECK(replica.committed_identity().hash == replica.committed().hash);
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
    CHECK(std::filesystem::exists(replica_path / "metadata" / "checkpoint.meta"));
    CHECK(std::filesystem::exists(replica_path / "metadata" / "journal.log"));

    // Ordinary 0.9 mutation is a compact delta proposal. It survives restart
    // through the encrypted journal and does not require a full snapshot file
    // rewrite for either prepare or commit.
    auto before_delta = decode_snapshot(reopened.current().payload);
    auto after_delta = before_delta;
    after_delta.entries.at("/movie.mkv").size = 456;
    after_delta.mutation_sequences[a] = 8;
    auto delta = metadata_delta(before_delta, after_delta);
    REQUIRE(delta.has_value());
    auto delta_bytes = encode_metadata_delta(*delta);
    const auto journal_before_delta =
        std::filesystem::file_size(replica_path / "metadata" / "journal.log");
    MetadataRecord delta_next;
    REQUIRE(reopened.cas_delta(reopened.current().generation, reopened.current().hash,
                               delta_bytes, &delta_next));
    CHECK(reopened.committed().hash == next.hash);
    REQUIRE(reopened.remember_current_committed(delta_next.generation, delta_next.hash));
    auto journal_size = std::filesystem::file_size(replica_path / "metadata" / "journal.log");
    const auto journal_growth = journal_size - journal_before_delta;
    // The journal contains two authenticated frames (prepare + commit), so for
    // deliberately tiny snapshots the fixed nonce/tag/framing overhead can be
    // larger than the snapshot itself. What matters is that growth tracks the
    // compact delta plus bounded framing, rather than embedding the full
    // successor snapshot in the ordinary mutation path.
    CHECK(delta_bytes.size() < delta_next.payload.size());
    CHECK(journal_growth >= delta_bytes.size());
    CHECK(journal_growth < delta_bytes.size() + 512);

    MetadataReplica reopened_again(replica_path, keys.storage);
    CHECK(reopened_again.current().hash == delta_next.hash);
    CHECK(reopened_again.committed().hash == delta_next.hash);
    CHECK(decode_snapshot(reopened_again.current().payload).entries.at("/movie.mkv").size == 456);

    // Accepted-but-uncommitted state remains current after restart but does not
    // become a recovery witness. An interrupted trailing journal append is
    // discarded without losing the last complete proposal.
    auto uncommitted_path = t.path() / "uncommitted-node";
    MetadataRecord uncommitted_next;
    Hash256 uncommitted_base_hash;
    {
        MetadataReplica uncommitted(uncommitted_path, keys.storage);
        auto base = uncommitted.current();
        uncommitted_base_hash = base.hash;
        auto before = decode_snapshot(base.payload);
        auto after = before;
        after.entries["/pending"] = extra;
        auto d = metadata_delta(before, after);
        REQUIRE(d.has_value());
        auto dbytes = encode_metadata_delta(*d);
        REQUIRE(uncommitted.cas_delta(base.generation, base.hash, dbytes, &uncommitted_next));
        CHECK(uncommitted.committed().hash == uncommitted_base_hash);
    }
    {
        std::ofstream tail(uncommitted_path / "metadata" / "journal.log",
                           std::ios::binary | std::ios::app);
        tail.write("bad", 3);
    }
    {
        MetadataReplica recovered(uncommitted_path, keys.storage);
        CHECK(recovered.current().hash == uncommitted_next.hash);
        CHECK(recovered.committed().hash == uncommitted_base_hash);
    }

    // Periodic compaction bounds replay. 64 committed mutations produce 128
    // prepare+commit journal records; the threshold checkpoints and truncates
    // them rather than allowing an unbounded replay log.
    auto compact_path = t.path() / "compact-node";
    {
        MetadataReplica compacted(compact_path, keys.storage);
        for (size_t i = 0; i < 65; ++i) {
            auto base = compacted.current();
            auto before = decode_snapshot(base.payload);
            auto after = before;
            after.entries["/"].mtime_ns = static_cast<int64_t>(i + 1);
            auto d = metadata_delta(before, after);
            REQUIRE(d.has_value());
            auto dbytes = encode_metadata_delta(*d);
            MetadataRecord proposal;
            REQUIRE(compacted.cas_delta(base.generation, base.hash, dbytes, &proposal));
            REQUIRE(compacted.remember_current_committed(proposal.generation, proposal.hash));
        }
        CHECK(std::filesystem::file_size(compact_path / "metadata" / "journal.log") > 4096);
        compacted.compact();
    }
    CHECK(std::filesystem::file_size(compact_path / "metadata" / "journal.log") < 4096);
    MetadataReplica compacted_again(compact_path, keys.storage);
    CHECK(compacted_again.current().generation == 66);
    CHECK(compacted_again.current().hash == compacted_again.committed().hash);

}

void test_metadata_identity_rpc() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c1 = config_for(t.path() / "identity-1", keyfile, free_port());
    auto c2 = config_for(t.path() / "identity-2", keyfile, free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] { return n1.membership().active().size() >= 2; }, 5s));

    auto peers = n1.membership().active();
    auto found = std::find_if(peers.begin(), peers.end(),
                              [&](const NodeInfo& peer) { return peer.id == n2.node_id(); });
    REQUIRE(found != peers.end());
    auto reply = n1.call(*found, MessageType::get_metadata_identity, {},
                         FrameType::speculative);
    REQUIRE(reply.message.type == MessageType::metadata_identity_reply);
    CHECK(reply.message.payload.size() == sizeof(uint64_t) + 32);
    Reader reader(reply.message.payload);
    MetadataIdentity observed;
    observed.generation = reader.u64();
    observed.hash.bytes = reader.fixed<32>();
    reader.finish();
    CHECK(observed == n2.metadata_replica().current_identity());

    n2.stop();
    n1.stop();
}

void test_config() {
    CHECK(Config{}.log_level == LogLevel::info);
    CHECK(Config{}.ffmpeg_log_level == FfmpegLogLevel::error);
    CHECK(parse_log_level("all") == LogLevel::all);
    CHECK(parse_log_level("DEBUG") == LogLevel::debug);
    CHECK(parse_log_level("Info") == LogLevel::info);
    CHECK(parse_log_level("warning") == LogLevel::warn);
    CHECK(parse_log_level("ERROR") == LogLevel::error);
    CHECK(parse_ffmpeg_log_level("quiet") == FfmpegLogLevel::quiet);
    CHECK(parse_ffmpeg_log_level("WARN") == FfmpegLogLevel::warning);
    CHECK(parse_ffmpeg_log_level("Verbose") == FfmpegLogLevel::verbose);
    CHECK(parse_ffmpeg_log_level("DEBUG") == FfmpegLogLevel::debug);
    CHECK(parse_ffmpeg_log_level("trace") == FfmpegLogLevel::trace);

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
    auto capture = std::make_shared<CapturingLogger>(LogLevel::info);
    Log::set_logger(capture);
    Log::debug("macha debug must remain filtered");
    Log::emit(LogLevel::debug, "ffmpeg: admitted debug must reach the sink");
    CHECK(capture->records.size() == 1);
    if (!capture->records.empty()) {
        CHECK(capture->records.front().first == LogLevel::debug);
        CHECK(capture->records.front().second == "ffmpeg: admitted debug must reach the sink");
    }
    capture->records.clear();
    configure_ffmpeg_logging(FfmpegLogLevel::debug);
    av_log(nullptr, AV_LOG_DEBUG, "independent FFmpeg debug\n");
    av_log(nullptr, AV_LOG_TRACE, "filtered FFmpeg trace\n");
    CHECK(capture->records.size() == 1);
    if (!capture->records.empty()) {
        CHECK(capture->records.front().first == LogLevel::debug);
        CHECK(capture->records.front().second.find("independent FFmpeg debug") != std::string::npos);
    }
    configure_ffmpeg_logging(FfmpegLogLevel::error);
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::warn));
    CHECK(!Log::enabled(LogLevel::all));
    CHECK(!Log::enabled(LogLevel::debug));
    CHECK(!Log::enabled(LogLevel::info));
    CHECK(Log::enabled(LogLevel::warn));
    CHECK(Log::enabled(LogLevel::error));
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
    MaintenanceConfig maintenance_policy;
    CHECK(maintenance_policy.interval == 1000ms);
    CHECK(maintenance_policy.idle_bandwidth_fraction == 0.10);
    CHECK(maintenance_policy.cpu_target == 0.10);
    CHECK(maintenance_policy.scrub_fraction == 0.02);
    CHECK(maintenance_policy.scrub_interval == std::chrono::hours(24 * 30));
    CHECK(maintenance_policy.no_progress_backoff == 300000ms);
    FuseConfig fuse_defaults;
    CHECK(fuse_defaults.entry_timeout == 1000ms);
    CHECK(fuse_defaults.attr_timeout == 1000ms);
    CHECK(fuse_defaults.negative_timeout == 500ms);
    CHECK(maintenance_background_interval(maintenance_policy) == 30000ms);
    maintenance_policy.no_progress_backoff = 2000ms;
    CHECK(maintenance_background_interval(maintenance_policy) == 5000ms);
    maintenance_policy.no_progress_backoff = 45000ms;
    CHECK(maintenance_background_interval(maintenance_policy) == 30000ms);
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
            << "ffmpeg_log_level: DEBUG\n"
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
            << "  root_uid: 501\n"
            << "  root_gid: 20\n"
            << "  root_mode: '0750'\n"
            << "fuse:\n"
            << "  allow_other: true\n"
            << "  entry_timeout_ms: 375\n"
            << "  attr_timeout_ms: 225\n"
            << "  negative_timeout_ms: 75\n"
            << "  absolute_request_timeout_ms: 14000\n"
            << "  request_workers: 18\n"
            << "  max_pending_requests: 2048\n"
            << "  commit_workers: 4\n"
            << "  foreground_commit_workers: 2\n"
            << "  publication_quiet_ms: 425\n"
            << "  max_pending_operations: 1024\n"
            << "  hydration_priority: 2500\n"
            << "  read_ahead_extents: 4\n"
            << "  hint_lifetime_ms: 4500\n"
            << "  write_through_cache: false\n"
            << "  refresh_interval_ms: 750\n" // legacy 0.14.4 key: accepted and ignored
            << "  fail_closed_mountpoint: true\n"
            << "  watchdog_interval_ms: 650\n"
            << "  timeouts:\n"
            << "    lookup_ms: 900\n"
            << "    namespace_ms: 2200\n"
            << "    read_ms: 8000\n"
            << "    write_ms: 3200\n"
            << "    sync_ms: 4100\n"
            << "    lifecycle_ms: 1700\n"
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
            << "  min_write_replicas: 2\n"
            << "  write_stall_ms: 1750\n"
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
            << "  scrub_interval_ms: 7776000000\n"
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
            << "    rescan_debounce_ms: 12000\n"
            << "    rescan_max_delay_ms: 45000\n"
            << "    max_provider_requests_per_scan: 48\n"
            << "    provider_batch_delay_ms: 15000\n"
            << "    max_artwork_bytes: 6M\n"
            << "    providers:\n"
            << "      movies:\n"
            << "        enabled: true\n"
            << "        roots: [/Movies]\n"
            << "        tmdb:\n"
            << "          enabled: true\n"
            << "          token_file: " << (t.path() / "tmdb.token").string() << "\n"
            << "          language: en-GB\n"
            << "          image_size: w500\n"
            << "      tv:\n"
            << "        enabled: true\n"
            << "        roots: [/TV]\n"
            << "        tmdb:\n"
            << "          enabled: true\n"
            << "          token_file: " << (t.path() / "tmdb.token").string() << "\n"
            << "          language: en-GB\n"
            << "          image_size: w500\n"
            << "      music:\n"
            << "        enabled: true\n"
            << "        roots: [/Music]\n"
            << "        musicbrainz:\n"
            << "          enabled: true\n"
            << "          contact: https://example.test/macha\n"
            << "          cover_size: '500'\n"
            << "        discogs:\n"
            << "          enabled: true\n"
            << "          token_file: " << (t.path() / "discogs.token").string() << "\n"
            << "ingest:\n"
            << "  enabled: true\n"
            << "  staging_path: " << (t.path() / "ingest").string() << "\n"
            << "  staging_limit: 12G\n"
            << "  source_roots: [" << (t.path() / "import").string() << "]\n"
            << "  cleanup:\n"
            << "    delete_owned_source_on_clear: false\n"
            << "    delete_external_source_on_clear: true\n"
            << "    delete_owned_source_on_cancel: false\n"
            << "torrent:\n"
            << "  enabled: true\n"
            << "  search:\n"
            << "    providers:\n"
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
    CHECK(yc.ffmpeg_log_level == FfmpegLogLevel::debug);
    CHECK(yc.fuse.allow_other);
    CHECK(yc.fuse.entry_timeout == 375ms);
    CHECK(yc.fuse.attr_timeout == 225ms);
    CHECK(yc.fuse.negative_timeout == 75ms);
    CHECK(yc.fuse.absolute_request_timeout == 14000ms);
    CHECK(yc.fuse.request_workers == 18);
    CHECK(yc.fuse.max_pending_requests == 2048);
    CHECK(yc.fuse.commit_workers == 4);
    CHECK(yc.fuse.foreground_commit_workers == 2);
    CHECK(yc.fuse.publication_quiet == 425ms);
    CHECK(yc.fuse.max_pending_operations == 1024);
    CHECK(yc.fuse.hydration_priority == 2500);
    CHECK(yc.fuse.read_ahead_extents == 4);
    CHECK(yc.fuse.hint_lifetime == 4500ms);
    CHECK(!yc.fuse.write_through_cache);
    CHECK(yc.fuse.fail_closed_mountpoint);
    CHECK(yc.fuse.watchdog_interval == 650ms);
    CHECK(yc.fuse.timeouts.lookup == 900ms);
    CHECK(yc.fuse.timeouts.namespace_mutation == 2200ms);
    CHECK(yc.fuse.timeouts.read == 8000ms);
    CHECK(yc.fuse.timeouts.write == 3200ms);
    CHECK(yc.fuse.timeouts.sync == 4100ms);
    CHECK(yc.fuse.timeouts.lifecycle == 1700ms);
    CHECK(yc.filesystem.root_uid == 501);
    CHECK(yc.filesystem.root_gid == 20);
    CHECK(yc.filesystem.root_mode == 0750);
    CHECK(yc.bootstrap.size() == 2);
    CHECK(yc.port == 7440);
    CHECK(yc.max_frame_size == 192ULL * 1024);
    CHECK(yc.control_stall_notice == 4100ms);
    CHECK(yc.data_stall_notice == 88000ms);
    CHECK(yc.min_write_replicas == 2);
    CHECK(yc.write_stall == 1750ms);
    CHECK(yc.maintenance.interval == 250ms);
    CHECK(yc.maintenance.garbage_grace == 1234ms);
    CHECK(yc.maintenance.busy_bandwidth_fraction == 0.03);
    CHECK(yc.maintenance.scrub_interval == std::chrono::hours(24 * 90));
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
    CHECK(yc.catalogue.scanner.rescan_debounce == 12000ms);
    CHECK(yc.catalogue.scanner.rescan_max_delay == 45000ms);
    CHECK(yc.catalogue.scanner.max_provider_requests_per_scan == 48);
    CHECK(yc.catalogue.scanner.provider_batch_delay == 15000ms);
    CHECK(yc.catalogue.scanner.movies.roots == std::vector<std::string>{"/Movies"});
    CHECK(yc.catalogue.scanner.tv.roots == std::vector<std::string>{"/TV"});
    CHECK(yc.catalogue.scanner.music.roots == std::vector<std::string>{"/Music"});
    CHECK(yc.catalogue.scanner.max_artwork_bytes == 6ULL * 1024 * 1024);
    REQUIRE(yc.catalogue.scanner.movies.tmdb.token_file.has_value());
    CHECK(*yc.catalogue.scanner.movies.tmdb.token_file == t.path() / "tmdb.token");
    CHECK(yc.catalogue.scanner.movies.tmdb.language == "en-GB");
    CHECK(yc.catalogue.scanner.movies.tmdb.image_size == "w500");
    REQUIRE(yc.catalogue.scanner.tv.tmdb.token_file.has_value());
    CHECK(*yc.catalogue.scanner.tv.tmdb.token_file == t.path() / "tmdb.token");
    CHECK(yc.catalogue.scanner.music.musicbrainz.contact == "https://example.test/macha");
    CHECK(yc.catalogue.scanner.music.musicbrainz.cover_size == "500");
    CHECK(yc.catalogue.scanner.music.discogs.enabled);
    REQUIRE(yc.catalogue.scanner.music.discogs.token_file.has_value());
    CHECK(*yc.catalogue.scanner.music.discogs.token_file == t.path() / "discogs.token");
    CHECK(yc.ingest.enabled);
    CHECK(yc.ingest.staging_path == t.path() / "ingest");
    CHECK(yc.ingest.staging_limit == 12ULL * 1024 * 1024 * 1024);
    CHECK(!yc.ingest.delete_owned_source_on_clear);
    CHECK(yc.ingest.delete_external_source_on_clear);
    CHECK(!yc.ingest.delete_owned_source_on_cancel);
    CHECK(yc.torrent.enabled);
    CHECK(yc.torrent.search_providers.empty());
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
                                            "--min-write-replicas", "3",
                                            "--write-stall", "1600",
                                            "--read-ahead", "4", "--failure-domain", "site-a",
                                            "--connect-timeout", "1700",
                                            "--max-frame-size", "320K",
                                            "--control-stall-notice", "4200",
                                            "--data-stall-notice", "90000",
                                            "--metadata-cache", "125",
                                            "--log-level", "DEBUG",
                                            "--ffmpeg-log-level", "WARNING"};
    std::vector<char*> override_argv;
    for (auto& arg : override_args)
        override_argv.push_back(arg.data());
    auto overridden = parse_config(static_cast<int>(override_argv.size()), override_argv.data());
    CHECK(overridden.port == 8123);
    CHECK(overridden.replication == 5);
    CHECK(overridden.min_write_replicas == 3);
    CHECK(overridden.write_stall == 1600ms);
    CHECK(overridden.read_ahead_extents == 4);
    CHECK(overridden.failure_domain == "site-a");
    CHECK(overridden.connect_timeout == 1700ms);
    CHECK(overridden.max_frame_size == 320ULL * 1024);
    CHECK(overridden.control_stall_notice == 4200ms);
    CHECK(overridden.data_stall_notice == 90000ms);
    CHECK(overridden.metadata_cache == 125ms);
    CHECK(overridden.log_level == LogLevel::debug);
    CHECK(overridden.ffmpeg_log_level == FfmpegLogLevel::warning);

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

void test_rpc_v13_frame_priority_and_variable_length() {
    CHECK(frame_type_priority(FrameType::control) < frame_type_priority(FrameType::foreground));
    CHECK(frame_type_priority(FrameType::foreground) < frame_type_priority(FrameType::read_ahead));
    CHECK(frame_type_priority(FrameType::read_ahead) <
          frame_type_priority(FrameType::speculative));
    CHECK(default_frame_type(MessageType::ping) == FrameType::control);
    CHECK(default_frame_type(MessageType::get_object) == FrameType::foreground);
    CHECK(default_frame_type(MessageType::get_metadata_object) == FrameType::speculative);
    CHECK(std::string(message_type_name(MessageType::commit_metadata)) == "commit_metadata");
    CHECK(std::string(message_type_name(MessageType::cas_metadata_delta)) ==
          "cas_metadata_delta");

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

    // Content-addressed metadata objects are potentially large and therefore
    // run at speculative worker priority, but they deliberately stay on the
    // CONTROL TCP session. Catalogue bootstrap must not require a DATA lane.
    auto metadata_object_reply =
        client.call(endpoint, MessageType::put_metadata_object, Bytes{0x43, 0x41, 0x54},
                    FrameType::speculative, 2s);
    CHECK(metadata_object_reply.message.type == MessageType::ok);
    CHECK(client.stats().canonical_connections == 1);

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

void test_rpc_v13_persistence_and_multiplexing() {
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

void test_rpc_v13_bidirectional_and_deduplication() {
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

void test_put_spills_stalled_owners_and_commits_degraded_floor() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto local_port = free_port();
    auto slow1_port = free_port();
    auto slow2_port = free_port();

    auto config = config_for(t.path() / "local-degraded", keyfile, local_port);
    config.replication = 3;
    config.metadata_replication = 1;
    config.min_write_replicas = 1;
    config.write_stall = 100ms;
    config.heartbeat = 10s;
    config.dead_after = 5s;

    Service service(config, keys);
    service.start();

    auto slow_info = [](uint16_t port, const char* domain) {
        NodeInfo info;
        info.id = random_node_id();
        info.host = "127.0.0.1";
        info.port = port;
        info.failure_domain = domain;
        info.capacity = 1024ULL * 1024 * 1024;
        info.seen_unix_ms = unix_ms();
        return info;
    };
    auto slow1 = slow_info(slow1_port, "slow-site-1");
    auto slow2 = slow_info(slow2_port, "slow-site-2");

    auto delayed_put = [](const NodeInfo&, FrameType, const RpcMessage& request) {
        if (request.type == MessageType::put_object)
            std::this_thread::sleep_for(1500ms);
        return RpcMessage{MessageType::ok, {}};
    };
    RpcServer slow1_server("127.0.0.1", slow1_port, keys, slow1, delayed_put,
                           [](const NodeInfo&) {});
    RpcServer slow2_server("127.0.0.1", slow2_port, keys, slow2, delayed_put,
                           [](const NodeInfo&) {});
    slow1_server.start();
    slow2_server.start();

    service.node().membership().observe(slow1, true);
    service.node().membership().observe(slow2, true);
    REQUIRE(service.node().membership().active().size() == 3);

    // The local copy succeeds immediately, but the normal R=3 quorum requires
    // two replicas. Both remote owners accept their RPCs and then make no
    // progress. The write must spill/degrade at the explicit floor rather than
    // wait for the 5s membership expiry (or for the delayed replies).
    DistributedStore store(service.node());
    auto data = pattern(128 * 1024);
    auto id = object_id(data);
    auto started = Clock::now();
    CHECK(store.put(id, data));
    auto elapsed = Clock::now() - started;
    CHECK(elapsed >= 75ms);
    CHECK(elapsed < 1s);
    CHECK(service.node().local_store().has(id));

    slow2_server.stop();
    slow1_server.stop();
    service.stop();
}

void test_put_falls_back_after_remote_launch_failure() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto local_port = free_port();
    auto dead_port = free_port();

    auto config = config_for(t.path() / "local", keyfile, local_port);
    config.replication = 1;
    config.metadata_replication = 1;
    config.connect_timeout = 100ms;
    config.heartbeat = 30s;
    config.dead_after = 60s;

    Service service(config, keys);
    service.start();

    NodeInfo unreachable;
    unreachable.id = random_node_id();
    unreachable.host = "127.0.0.1";
    unreachable.port = dead_port; // free_port() closes the listener: connect must fail.
    unreachable.failure_domain = "unreachable-site";
    unreachable.capacity = config.storage_backends.front().limit;
    unreachable.seen_unix_ms = unix_ms();
    service.node().membership().observe(unreachable, true);
    REQUIRE(service.node().membership().active().size() == 2);

    Bytes data;
    std::vector<NodeInfo> ranked;
    for (uint32_t salt = 0; salt < 4096; ++salt) {
        data = pattern(256 * 1024 + salt);
        auto id = object_id(data);
        ranked = capacity_placement_nodes(id.bytes, service.node().membership().active(), 1);
        if (ranked.size() == 2 && ranked[0].id == unreachable.id &&
            ranked[1].id == service.node().node_id())
            break;
        ranked.clear();
    }
    REQUIRE(ranked.size() == 2);

    // Before 0.9.4, a synchronous call_async() failure incremented a dead
    // "completed" counter but was not treated as a failed replica. With no
    // pending RPC, the quorum loop then slept forever instead of trying the
    // deterministic fallback owner. Keep a cancellation watchdog so this
    // regression fails boundedly rather than hanging the entire test binary.
    std::atomic_bool cancelled{false};
    std::jthread watchdog([&](std::stop_token stop) {
        const auto deadline = Clock::now() + 2s;
        while (!stop.stop_requested() && Clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        if (!stop.stop_requested())
            cancelled.store(true, std::memory_order_relaxed);
    });

    DistributedStore store(service.node());
    auto id = object_id(data);
    const bool stored = store.put(id, data, &cancelled);
    watchdog.request_stop();

    CHECK(stored);
    CHECK(!cancelled.load(std::memory_order_relaxed));
    CHECK(service.node().local_store().has(id));
    service.stop();
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


void test_bootstrap_joiner_requires_complete_checkpoint_survey() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    const auto unreachable_port = free_port();
    auto config = config_for(t.path() / "checkpoint-survey", keyfile, free_port(),
                             {{"127.0.0.1", unreachable_port}});
    config.replication = 1;
    config.metadata_replication = 1;
    config.dead_after = 10s;

    NodeRuntime node(config, keys);
    node.start();

    // Keep an unreachable peer in active membership and choose its identity so
    // metadata HRW would select this fresh node as the sole genesis voter. This
    // deterministically exercises the dangerous pre-0.14.2 path: both metadata
    // RPC surveys fail, yet a replication-1 joiner could previously form an
    // empty generation-2 namespace on itself.
    static constexpr char label[] = "macha/metadata-placement/v1";
    const auto placement_key =
        sha256({reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1});
    NodeInfo phantom;
    phantom.host = "127.0.0.1";
    phantom.failure_domain = "unreachable-bootstrap";
    phantom.port = unreachable_port;
    phantom.capacity = 512ULL * 1024 * 1024;
    phantom.seen_unix_ms = unix_ms();

    const auto self = node.membership().self();
    bool selected_self = false;
    for (uint32_t candidate = 1; candidate < 100000 && !selected_self; ++candidate) {
        phantom.id = {};
        phantom.id.bytes[0] = static_cast<uint8_t>(candidate >> 24U);
        phantom.id.bytes[1] = static_cast<uint8_t>(candidate >> 16U);
        phantom.id.bytes[2] = static_cast<uint8_t>(candidate >> 8U);
        phantom.id.bytes[3] = static_cast<uint8_t>(candidate);
        if (phantom.id == self.id)
            continue;
        auto ranked = rendezvous_nodes(placement_key.bytes, {self, phantom}, 1);
        selected_self = !ranked.empty() && ranked.front().id == self.id;
    }
    REQUIRE(selected_self);
    node.membership().observe(phantom, true);

    MetadataManager metadata(node);
    bool rejected = false;
    try {
        (void)metadata.snapshot_view();
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find("bootstrap checkpoint survey") !=
                   std::string::npos;
    }
    CHECK(rejected);
    CHECK(node.metadata_replica().current().generation <= 1);
    CHECK(node.metadata_replica().committed().generation <= 1);

    node.stop();
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

void test_fresh_and_resumed_write_exactness() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "write-exactness", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;

    Service service(config, keys);
    service.start();

    const auto read_exact = [&](const std::string& path, size_t size) {
        auto reader = service.filesystem().open_read(path);
        Bytes output(size);
        size_t offset = 0;
        while (offset < output.size()) {
            const auto n = reader->read(
                offset, {output.data() + offset, std::min<size_t>(131071, output.size() - offset)});
            REQUIRE(n > 0);
            offset += n;
        }
        return output;
    };

    // Fresh rsync-style sequential write: varied FUSE-sized chunks cross many
    // extent boundaries and the final extent is deliberately partial.
    auto fresh = pattern(5 * config.extent_size + 123457);
    service.filesystem().create_file("/fresh.bin", 0644, getuid(), getgid());
    auto fresh_writer = service.filesystem().open_write("/fresh.bin", true);
    size_t offset = 0;
    while (offset < fresh.size()) {
        const auto n = std::min<size_t>(65537, fresh.size() - offset);
        REQUIRE(fresh_writer->write(offset, {fresh.data() + offset, n}) == n);
        offset += n;
    }
    fresh_writer->commit();
    CHECK(read_exact("/fresh.bin", fresh.size()) == fresh);

    // --append/--append-verify style resume: an existing committed prefix is
    // reopened without truncation and writing resumes exactly at EOF.  This is
    // the case that can otherwise retain a bad prefix or corrupt rematerialised
    // data without being noticed until rsync's final verification pass.
    auto resumed = pattern(6 * config.extent_size + 654321);
    const size_t prefix = 2 * config.extent_size + 77777;
    service.filesystem().create_file("/resumed.bin", 0644, getuid(), getgid());
    auto prefix_writer = service.filesystem().open_write("/resumed.bin", true);
    REQUIRE(prefix_writer->write(0, {resumed.data(), prefix}) == prefix);
    prefix_writer->commit();
    prefix_writer.reset();

    const auto prefix_entry = service.filesystem().getattr("/resumed.bin");
    REQUIRE(prefix_entry.extents.size() == 3);
    const auto first_full = prefix_entry.extents[0];
    const auto second_full = prefix_entry.extents[1];

    auto resumed_writer = service.filesystem().open_write("/resumed.bin", false);
    offset = prefix;
    while (offset < resumed.size()) {
        const auto n = std::min<size_t>(98317, resumed.size() - offset);
        REQUIRE(resumed_writer->write(offset, {resumed.data() + offset, n}) == n);
        offset += n;
    }
    resumed_writer->commit();
    const auto resume_diag = resumed_writer->diagnostics();
    CHECK(resume_diag.sequential);
    CHECK(!resume_diag.temp_open);
    CHECK(resume_diag.append_tail_fetches == 1);
    CHECK(resume_diag.materialize_source_reads == 0);
    CHECK(resume_diag.rebuild_reused_extents == 0);
    CHECK(resume_diag.rebuild_put_extents == 0);

    const auto resumed_entry = service.filesystem().getattr("/resumed.bin");
    REQUIRE(resumed_entry.extents.size() >= 2);
    CHECK(resumed_entry.extents[0] == first_full);
    CHECK(resumed_entry.extents[1] == second_full);
    CHECK(read_exact("/resumed.bin", resumed.size()) == resumed);

    // Extent-aligned resume is even cheaper: no old object is fetched at all.
    auto aligned = pattern(5 * config.extent_size + 333);
    service.filesystem().create_file("/aligned.bin", 0644, getuid(), getgid());
    auto aligned_prefix = service.filesystem().open_write("/aligned.bin", true);
    REQUIRE(aligned_prefix->write(0, {aligned.data(), 3 * config.extent_size}) ==
            3 * config.extent_size);
    aligned_prefix->commit();
    aligned_prefix.reset();
    const auto aligned_before = service.filesystem().getattr("/aligned.bin");
    REQUIRE(aligned_before.extents.size() == 3);

    auto aligned_writer = service.filesystem().open_write("/aligned.bin", false);
    offset = 3 * config.extent_size;
    while (offset < aligned.size()) {
        const auto n = std::min<size_t>(77777, aligned.size() - offset);
        REQUIRE(aligned_writer->write(offset, {aligned.data() + offset, n}) == n);
        offset += n;
    }
    aligned_writer->commit();
    const auto aligned_diag = aligned_writer->diagnostics();
    CHECK(aligned_diag.sequential);
    CHECK(!aligned_diag.temp_open);
    CHECK(aligned_diag.append_tail_fetches == 0);
    CHECK(aligned_diag.materialize_source_reads == 0);
    CHECK(aligned_diag.rebuild_put_extents == 0);
    const auto aligned_after = service.filesystem().getattr("/aligned.bin");
    REQUIRE(aligned_after.extents.size() >= aligned_before.extents.size());
    for (size_t i = 0; i < aligned_before.extents.size(); ++i)
        CHECK(aligned_after.extents[i] == aligned_before.extents[i]);
    CHECK(read_exact("/aligned.bin", aligned.size()) == aligned);

    // A FUSE flush does not close the handle.  Appending again after a commit
    // must lazily reopen only the newly committed partial tail, not materialise
    // the complete file.
    auto more = pattern(777);
    const auto aligned_old_size = aligned.size();
    aligned.insert(aligned.end(), more.begin(), more.end());
    REQUIRE(aligned_writer->write(aligned_old_size, more) == more.size());
    aligned_writer->commit();
    const auto aligned_again_diag = aligned_writer->diagnostics();
    CHECK(!aligned_again_diag.temp_open);
    CHECK(aligned_again_diag.append_tail_fetches == 1);
    CHECK(aligned_again_diag.materialize_source_reads == 0);
    CHECK(aligned_again_diag.rebuild_put_extents == 0);
    CHECK(read_exact("/aligned.bin", aligned.size()) == aligned);

    // Arbitrary overwrite still uses staging, but unchanged extents are reused
    // rather than re-put. Change one byte in a four-extent file and assert only
    // the touched extent is newly stored at rebuild time.
    auto random_write = pattern(4 * config.extent_size);
    service.filesystem().create_file("/random.bin", 0644, getuid(), getgid());
    auto random_seed = service.filesystem().open_write("/random.bin", true);
    REQUIRE(random_seed->write(0, random_write) == random_write.size());
    random_seed->commit();
    random_seed.reset();
    auto random_writer = service.filesystem().open_write("/random.bin", false);
    const uint64_t changed_offset = config.extent_size + 1234;
    const uint8_t changed = static_cast<uint8_t>(random_write[changed_offset] ^ 0x5a);
    random_write[changed_offset] = changed;
    REQUIRE(random_writer->write(changed_offset, {&changed, 1}) == 1);
    random_writer->commit();
    const auto random_diag = random_writer->diagnostics();
    CHECK(random_diag.temp_open);
    CHECK(random_diag.materialize_source_reads == 4);
    CHECK(random_diag.rebuild_reused_extents == 3);
    CHECK(random_diag.rebuild_put_extents == 1);
    CHECK(read_exact("/random.bin", random_write.size()) == random_write);

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


void test_fuse_frontend_ordering_merging_and_cache() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "fuse-ordering", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;
    config.cache.path = t.path() / "cache";
    config.cache.max_blocks = 64;
    config.fuse.commit_workers = 2;
    config.fuse.read_ahead_extents = 2;
    config.fuse.write_through_cache = true;

    Service service(config, keys);
    service.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->create("/.rsync.tmp", 0600, getuid(), getgid(), true, true, false);
        const auto inode = handle.inode;
        REQUIRE(inode != 0);

        Bytes expected(3 * config.extent_size + 8192, 0);
        auto first = pattern(config.extent_size + 32768);
        REQUIRE(frontend->write(inode, 0, first) == first.size());
        std::copy(first.begin(), first.end(), expected.begin());

        // Adjacent and overlapping writes are retained in exact byte order but
        // expose one coalesced dirty range to the frontend scheduler.
        auto adjacent = pattern(config.extent_size);
        REQUIRE(frontend->write(inode, first.size(), adjacent) == adjacent.size());
        std::copy(adjacent.begin(), adjacent.end(), expected.begin() + static_cast<ptrdiff_t>(first.size()));
        auto patch = pattern(131072);
        const uint64_t patch_offset = config.extent_size - 65536;
        for (auto& byte : patch) byte ^= 0xa5;
        REQUIRE(frontend->write(inode, patch_offset, patch) == patch.size());
        std::copy(patch.begin(), patch.end(), expected.begin() + static_cast<ptrdiff_t>(patch_offset));

        auto ranges = frontend->dirty_ranges(inode);
        REQUIRE(ranges.size() == 1);
        CHECK(ranges.front().offset == 0);
        CHECK(ranges.front().length == first.size() + adjacent.size());

        // Queue publication more than once. It is legal for the first commit to
        // complete very quickly on a one-node test cluster, but pending work may
        // never be double-counted and no duplicate bytes may result.
        frontend->flush(inode);
        frontend->flush(inode);
        auto during = frontend->status();
        CHECK(during.pending_data <= 1);
        CHECK(during.active_data <= config.fuse.commit_workers);

        // Rename twice while retaining the same open file description, then
        // continue writing through that inode. No path lookup participates in
        // the subsequent write/close sequence.
        frontend->rename("/.rsync.tmp", "/.stage.tmp");
        REQUIRE(frontend->inode_for_path("/.stage.tmp") == inode);
        CHECK(frontend->path_for_inode(inode) == "/.stage.tmp");
        frontend->rename("/.stage.tmp", "/movie.bin");
        REQUIRE(frontend->inode_for_path("/movie.bin") == inode);
        CHECK(!frontend->inode_for_path("/.rsync.tmp").has_value());
        CHECK(!frontend->inode_for_path("/.stage.tmp").has_value());

        auto tail = pattern(8192);
        for (auto& byte : tail) byte ^= 0x3c;
        const uint64_t tail_offset = 3 * config.extent_size;
        REQUIRE(frontend->write(inode, tail_offset, tail) == tail.size());
        std::copy(tail.begin(), tail.end(), expected.begin() + static_cast<ptrdiff_t>(tail_offset));
        frontend->release(inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        CHECK(frontend->status().pending_data == 0);
        auto entry = service.filesystem().getattr("/movie.bin");
        CHECK(entry.size == expected.size());
        auto reader = service.filesystem().open_read("/movie.bin");
        Bytes actual(expected.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == expected);

        // FUSE write-back publication uses the ordinary extent writer and, when
        // requested, also promotes each immutable extent into Macha's existing
        // persistent block cache rather than maintaining a second FUSE cache.
        REQUIRE(!entry.extents.empty());
        for (const auto& extent : entry.extents)
            if (!extent.hole)
                CHECK(service.node().block_cache().has(extent.id));
    }
    service.stop();
}

void test_fuse_read_only_release_does_not_publish_writer_data() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "fuse-read-release", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.fuse.commit_workers = 1;

    Service service(config, keys);
    service.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto writer = frontend->create("/growing.bin", 0600, getuid(), getgid(), true, true, false);
        auto bytes = pattern(256 * 1024);
        REQUIRE(frontend->write(writer.inode, 0, bytes) == bytes.size());

        // A second process such as rsync --append-verify may open the file for
        // basis reads while the writer still has dirty local data. Closing that
        // reader must not turn into an implicit writer flush/publication.
        auto reader = frontend->open("/growing.bin", true, false, false, false);
        CHECK(reader.inode == writer.inode);
        frontend->release(reader.inode, false);
        REQUIRE(frontend->wait_for_idle(2s));

        auto backend_before_writer_close = service.filesystem().getattr("/growing.bin");
        CHECK(backend_before_writer_close.size == 0);
        REQUIRE(frontend->dirty_ranges(writer.inode).size() == 1);

        frontend->release(writer.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        auto backend_after_writer_close = service.filesystem().getattr("/growing.bin");
        CHECK(backend_after_writer_close.size == bytes.size());

        auto stored = service.filesystem().open_read("/growing.bin");
        Bytes actual(bytes.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = stored->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == bytes);
    }
    service.stop();
}

void test_fuse_frontend_unlink_and_rename_over_open_inode_ordering() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "fuse-replace", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.fuse.commit_workers = 2;

    Service service(config, keys);
    service.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

        // A dirty inode that is unlinked before release must never recreate its
        // old pathname when the data-publication worker eventually sees it.
        auto doomed = frontend->create("/doomed.bin", 0600, getuid(), getgid(), true, true, false);
        auto doomed_data = pattern(65536);
        REQUIRE(frontend->write(doomed.inode, 0, doomed_data) == doomed_data.size());
        frontend->unlink("/doomed.bin");
        frontend->release(doomed.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(!frontend->inode_for_path("/doomed.bin").has_value());
        bool missing = false;
        try { (void)service.filesystem().getattr("/doomed.bin"); }
        catch (const FsError& e) { missing = e.code() == ENOENT; }
        CHECK(missing);

        // More subtle: POSIX rename may replace a destination which still has
        // an open descriptor. The displaced inode remains a valid open identity,
        // but it no longer owns that pathname. Releasing dirty data through the
        // old descriptor must not overwrite/resurrect the new destination.
        auto destination = frontend->create("/target.bin", 0600, getuid(), getgid(), true, true, false);
        auto old_bytes = pattern(32768);
        REQUIRE(frontend->write(destination.inode, 0, old_bytes) == old_bytes.size());
        frontend->release(destination.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        auto old_open = frontend->open("/target.bin", true, true, false, false);
        std::array<uint8_t, 8> stale{{'S','T','A','L','E','!','!','!'}};
        REQUIRE(frontend->write(old_open.inode, 0, stale) == stale.size());

        auto source = frontend->create("/replacement.bin", 0600, getuid(), getgid(), true, true, false);
        auto replacement = pattern(98304);
        for (auto& byte : replacement) byte ^= 0x7d;
        REQUIRE(frontend->write(source.inode, 0, replacement) == replacement.size());
        frontend->flush(source.inode);
        frontend->rename("/replacement.bin", "/target.bin");
        REQUIRE(frontend->inode_for_path("/target.bin") == source.inode);
        frontend->release(source.inode, true);
        frontend->release(old_open.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        CHECK(frontend->path_for_inode(old_open.inode).empty());
        auto final = service.filesystem().getattr("/target.bin");
        CHECK(final.size == replacement.size());
        auto reader = service.filesystem().open_read("/target.bin");
        Bytes actual(replacement.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == replacement);
    }
    service.stop();
}

void test_fuse_frontend_read_overlay_truncate_and_hydration_hints() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "fuse-read", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.read_ahead_extents = 2;
    config.fuse.hydration_priority = 2718;

    Service service(config, keys);
    service.start();
    auto committed = pattern(4 * config.extent_size + 4096);
    service.filesystem().create_file("/read.bin", 0644, getuid(), getgid());
    auto seed = service.filesystem().open_write("/read.bin", true);
    REQUIRE(seed->write(0, committed) == committed.size());
    seed->commit();
    seed.reset();
    auto base_entry = service.filesystem().getattr("/read.bin");
    REQUIRE(base_entry.extents.size() >= 5);

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->open("/read.bin", true, true, false, false);

        auto patch = pattern(16384);
        for (auto& byte : patch) byte ^= 0x91;
        const uint64_t patch_offset = 4096;
        REQUIRE(frontend->write(handle.inode, patch_offset, patch) == patch.size());
        Bytes view(32768);
        REQUIRE(frontend->read(handle.inode, 0, view) == view.size());
        auto expected_view = Bytes(committed.begin(), committed.begin() + static_cast<ptrdiff_t>(view.size()));
        std::copy(patch.begin(), patch.end(), expected_view.begin() + static_cast<ptrdiff_t>(patch_offset));
        CHECK(view == expected_view);

        // A committed-range read emits one high-priority FUSE run into the
        // ordinary hydration scheduler. Re-reading the same range replaces the
        // inode's hint rather than duplicating work, and object IDs are unique.
        // This must be tested while the committed extent is still inside the
        // inode's logical EOF.
        Bytes demand(4096);
        REQUIRE(frontend->read(handle.inode, config.extent_size + 1024, demand) == demand.size());
        REQUIRE(frontend->read(handle.inode, config.extent_size + 1024, demand) == demand.size());
        auto hints = frontend->hints();
        REQUIRE(hints.size() == 1);
        CHECK(hints.front().run_id == "fuse:" + std::to_string(handle.inode));
        CHECK(hints.front().priority == config.fuse.hydration_priority);
        CHECK(hints.front().frame_type == FrameType::read_ahead);
        REQUIRE(hints.front().objects.size() == 3);
        CHECK(hints.front().objects[0] == base_entry.extents[1].id);
        CHECK(hints.front().objects[1] == base_entry.extents[2].id);
        CHECK(hints.front().objects[2] == base_entry.extents[3].id);
        std::set<ObjectId> unique(hints.front().objects.begin(), hints.front().objects.end());
        CHECK(unique.size() == hints.front().objects.size());

        // Shrink then extend before publication. Bytes from the old committed
        // suffix must not reappear; the extended region is logically zero until
        // a later write overlays it.
        frontend->truncate(handle.inode, 32768);
        frontend->truncate(handle.inode, 65536);
        Bytes extended(32768, 0xff);
        REQUIRE(frontend->read(handle.inode, 32768, extended) == extended.size());
        CHECK(std::all_of(extended.begin(), extended.end(), [](uint8_t b) { return b == 0; }));
        Bytes beyond_eof(4096, 0xff);
        CHECK(frontend->read(handle.inode, config.extent_size + 1024, beyond_eof) == 0);
        std::array<uint8_t, 6> marker{{'M','A','C','H','A','!'}};
        REQUIRE(frontend->write(handle.inode, 40000, marker) == marker.size());
        Bytes marker_view(64, 0xff);
        REQUIRE(frontend->read(handle.inode, 39984, marker_view) == marker_view.size());
        CHECK(std::equal(marker.begin(), marker.end(), marker_view.begin() + 16));

        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        auto final = service.filesystem().getattr("/read.bin");
        CHECK(final.size == 65536);
        auto reader = service.filesystem().open_read("/read.bin");
        Bytes final_bytes(65536);
        size_t offset = 0;
        while (offset < final_bytes.size()) {
            auto n = reader->read(offset, {final_bytes.data() + offset, final_bytes.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(std::equal(patch.begin(), patch.end(), final_bytes.begin() + static_cast<ptrdiff_t>(patch_offset)));
        CHECK(std::equal(marker.begin(), marker.end(), final_bytes.begin() + 40000));
        CHECK(std::all_of(final_bytes.begin() + 32768, final_bytes.begin() + 40000,
                          [](uint8_t b) { return b == 0; }));
    }
    service.stop();
}

void test_fuse_frontend_namespace_refresh_is_demand_driven() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(t.path() / "fuse-demand-refresh", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.fuse.commit_workers = 1;

    Service service(config, keys);
    service.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

        bool missing = false;
        try { (void)frontend->getattr("/external"); }
        catch (const FsError& e) { missing = e.code() == ENOENT; }
        CHECK(missing);

        // Mutate the namespace outside the FUSE frontend. There is no refresh
        // timer: the next namespace-facing FUSE request observes the metadata
        // generation advance and adopts MetadataManager's shared decoded view.
        service.filesystem().mkdir("/external", 0755, getuid(), getgid());
        auto external = frontend->getattr("/external");
        CHECK(external.type == EntryType::directory);

        service.filesystem().create_file("/external/media.bin", 0644, getuid(), getgid());
        auto entries = frontend->readdir("/external");
        CHECK(std::any_of(entries.begin(), entries.end(), [](const auto& item) {
            return item.first == "media.bin";
        }));

        service.filesystem().unlink("/external/media.bin");
        missing = false;
        try { (void)frontend->getattr("/external/media.bin"); }
        catch (const FsError& e) { missing = e.code() == ENOENT; }
        CHECK(missing);
    }
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

    std::atomic_uint playback_changes{};
    tracker.set_change_callback([&] { ++playback_changes; });
    auto notified_session = tracker.open("/notified.mkv", synthetic);
    tracker.progress(notified_session, 2);
    tracker.close(notified_session);
    CHECK(playback_changes.load() == 3);
    tracker.set_change_callback({});

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
    };
    auto provider = std::make_shared<StaticHints>(std::vector<HydrationHint>{
        {"current", {a, b}, 1000, "read_ahead"},
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
    auto concurrent_provider = std::make_shared<StaticHints>(
        std::vector<HydrationHint>{{"parallel", {parallel0, parallel1, parallel2}, 500, "current_file"}});
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

    // With no work the production hydrator must block, not sample providers at
    // hydration.interval. A producer notification wakes it immediately when a
    // new hint becomes available.
    const auto event_object = make_remote(69);
    class NotifyingHints final : public HydrationHintProvider {
        mutable std::mutex mutex_;
        std::vector<HydrationHint> hints_;
        std::function<void()> wake_;
      public:
        std::atomic_uint calls{};
        std::string_view name() const override { return "notifying-test"; }
        std::vector<HydrationHint> hints() override {
            ++calls;
            std::lock_guard lock(mutex_);
            return hints_;
        }
        void set_wake_callback(std::function<void()> callback) override {
            std::lock_guard lock(mutex_);
            wake_ = std::move(callback);
        }
        void publish(std::vector<HydrationHint> hints) {
            std::function<void()> wake;
            {
                std::lock_guard lock(mutex_);
                hints_ = std::move(hints);
                wake = wake_;
            }
            if (wake) wake();
        }
    };
    auto notifying_provider = std::make_shared<NotifyingHints>();
    HydrationConfig event_config;
    event_config.interval = 5ms;
    event_config.max_inflight = 1;
    CacheHydrator event_hydrator(target, event_config);
    event_hydrator.add_provider(notifying_provider);
    event_hydrator.start();
    std::this_thread::sleep_for(75ms);
    CHECK(notifying_provider->calls.load() <= 2);
    notifying_provider->publish({{"event", {event_object}, 1000, "event-test"}});
    REQUIRE(wait_until([&] { return n2.block_cache().has(event_object); }, 3s));
    event_hydrator.stop();

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

    struct MovieRegression { const char* path; const char* title; int year; const char* edition{}; };
    for (const auto& regression : std::array{
             MovieRegression{"/Movies/01 Men In Black 1 - Will Smith 1997 Eng Ita Multi-Subs 1080p [H264-mp4].mp4", "Men In Black 1", 1997},
             MovieRegression{"/Movies/02 Men In Black 2 - Will Smith 2002 Eng Ita Multi-Subs 1080p [H264-mp4].mp4", "Men In Black 2", 2002},
             MovieRegression{"/Movies/03 Men In Black 3 - Will Smith 2012 Eng Ita Multi-Subs 1080p [H264-mp4].mp4", "Men In Black 3", 2012},
             MovieRegression{"/Movies/12.Monkeys.1995.1080p.BluRay.x264.AAC5.1.mp4", "12 Monkeys", 1995},
             MovieRegression{"/Movies/1994.Pulp.Fiction.1920x816.BDRip.x264.DTS-HD.MA.mkv", "Pulp Fiction", 1994},
             MovieRegression{"/Movies/Apollo.13.1995.Remastered.1080p.BluRay.DDP.5.1.H.265-EDGE2020.mkv", "Apollo 13", 1995, "Remastered"},
             MovieRegression{"/Movies/Bo.Burnham.Inside.2021.1080p.NF.WEBRip.DDP.5.1.H.265-EDGE2020.mkv", "Bo Burnham Inside", 2021},
             MovieRegression{"/Movies/Corpse.Bride.2005.1080p.BluRay.DDP.5.1.H.265-EDGE2020.mkv", "Corpse Bride", 2005},
             MovieRegression{"/Movies/Leon.the.Professional.Extended.1994.BrRip.x264.YIFY.mp4", "Leon the Professional", 1994, "Extended"},
             MovieRegression{"/Movies/Requiem.For.A.Dream.DIRECTORS.CUT.2000.1080p.BrRip.x264.YIFY.mp4", "Requiem For A Dream", 2000, "DIRECTORS CUT"},
             MovieRegression{"/Movies/Rebel.Moon.Part.One.Directors.Cut.1080p.NF.WEBRip.AAC5.1.10bits.x265-Rapta.mkv", "Rebel Moon Part One", 0, "Directors Cut"},
             MovieRegression{"/Movies/2003.Kill.Bill-.Volume.1.1920x802.BDRip.x264.DTS-HD.MA.mkv", "Kill Bill Volume 1", 2003},
             MovieRegression{"/Movies/Soldier - Sci-fi 1998 Eng Rus Comm Multi Subs 720p [H264-mp4].mp4", "Soldier", 1998},
         }) {
        auto parsed = probe_media_path(regression.path, fake);
        REQUIRE(parsed.has_value());
        CHECK(parsed->kind == MediaProbeKind::movie);
        CHECK(parsed->title == regression.title);
        if (regression.year) CHECK(parsed->year == regression.year);
        else CHECK(!parsed->year.has_value());
        if (regression.edition) CHECK(parsed->edition == std::optional<std::string>{regression.edition});
        else CHECK(!parsed->edition.has_value());
    }

    auto apollo_candidates = probe_media_candidates(
        "/Movies/Apollo.13.1995.Remastered.1080p.BluRay.DDP.5.1.H.265-EDGE2020.mkv", fake);
    REQUIRE(apollo_candidates.size() >= 2);
    CHECK(apollo_candidates.front().generator == "movie-semantic");
    CHECK(apollo_candidates.front().score > apollo_candidates.back().score);
    CHECK(!apollo_candidates.front().evidence.empty());

    auto compact_candidates = probe_media_candidates(
        "/Movies/japhson-romeoandjuliet.mkv", fake);
    auto compact = std::find_if(compact_candidates.begin(), compact_candidates.end(),
                                [](const auto& candidate) {
                                    return candidate.generator == "movie-compact-title";
                                });
    REQUIRE(compact != compact_candidates.end());
    CHECK(compact->probe.title == "romeo and juliet");

    struct EpisodeRegression {
        const char* path;
        const char* series;
        int year;
        int season;
        int episode;
        const char* title;
    };
    for (const auto& regression : std::array{
             EpisodeRegression{"/TV/Big.Mistakes.S01E08.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Big Mistakes", 0, 1, 8, ""},
             EpisodeRegression{"/TV/Stranger.Things.S05E01.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Stranger Things", 0, 5, 1, ""},
             EpisodeRegression{"/TV/Stranger.Things.S05E03.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Stranger Things", 0, 5, 3, ""},
             EpisodeRegression{"/TV/Stranger.Things.S05E04.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Stranger Things", 0, 5, 4, ""},
             EpisodeRegression{"/TV/Stranger.Things.S05E05.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Stranger Things", 0, 5, 5, ""},
             EpisodeRegression{"/TV/Stranger.Things.S05E06.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Stranger Things", 0, 5, 6, ""},
             EpisodeRegression{"/TV/Stranger.Things.S05E08.1080p.HEVC.x265-MeGusta[EZTVx.to].mkv", "Stranger Things", 0, 5, 8, ""},
             EpisodeRegression{"/TV/Battlestar Galactica (2003) Season 1-4 S01-S04 (1080p BluRay x265 HEVC 10bit AAC 5.1 RZeroX)/Season 2/Battlestar Galactica (2003) - S02E01 - Scattered (1080p BluRay x265 RZeroX).mkv", "Battlestar Galactica", 2003, 2, 1, "Scattered"},
             EpisodeRegression{"/TV/Ballykissangel (1996)/Season 2/Ballykissangel - S02E09 - As Happy as a Turkey on Boxing Day.mkv", "Ballykissangel", 1996, 2, 9, "As Happy as a Turkey on Boxing Day"},
             EpisodeRegression{"/TV/Allo Allo 1984 Season 1 to 3 Complete DVDRip x264 [i_c]/Allo Allo 1984 Season 1/01 - Allo Allo S1e00 - The British Are Coming [Pilot].mkv", "Allo Allo", 0, 1, 0, "The British Are Coming [Pilot]"},
             EpisodeRegression{"/TV/Test Show S01E01 - Ordinary Episode [rartv].mkv", "Test Show", 0, 1, 1, "Ordinary Episode"},
             EpisodeRegression{"/TV/Black Books (2000)/Black Books (2000) - S01E01 - Cooking the Books (576p DVD x265 Ghost).mkv", "Black Books", 2000, 1, 1, "Cooking the Books"},
             EpisodeRegression{"/TV/Black Books (2000)/S01E02.mkv", "Black Books", 2000, 1, 2, ""},
             EpisodeRegression{"/TV/Blackadder.1982.S01-S04.1080p.BluRay.EAC3.2.0.x265-iVy/S01E01.mkv", "Blackadder", 1982, 1, 1, ""},
             EpisodeRegression{"/TV/Black.Jesus.S01.1080p.AMZN.WEBRip.DDP5.1.x264-Cinefeel[rartv]/S01E01.mkv", "Black Jesus", 0, 1, 1, ""},
             EpisodeRegression{"/TV/Black.Jesus.S02.1080p.WEB-DL.DD5.1.H.264-BTN[rartv]/S02E01.mkv", "Black Jesus", 0, 2, 1, ""},
             EpisodeRegression{"/TV/Black.Jesus.S02.1080p.WEB-DL.DD5.1.H.264-BTN[rartv]/Black.Jesus.S02E05.Tasty.Tudi.s.1080p.WEB-DL.DD5.1.H.264-BTN.mkv", "Black Jesus", 0, 2, 5, "Tasty Tudi's"},
             EpisodeRegression{"/TV/Black.Jesus.S02.1080p.WEB-DL.DD5.1.H.264-BTN[rartv]/Black.Jesus.S02E07.Thy.Neighbor.s.Strife.1080p.WEB-DL.DD5.1.H.264-BTN.mkv", "Black Jesus", 0, 2, 7, "Thy Neighbor's Strife"},
         }) {
        auto parsed = probe_media_path(regression.path, fake);
        REQUIRE(parsed.has_value());
        CHECK(parsed->kind == MediaProbeKind::episode);
        CHECK(parsed->series == regression.series);
        CHECK(parsed->season == regression.season);
        CHECK(parsed->episode == regression.episode);
        CHECK(parsed->title == regression.title);
        if (regression.year) CHECK(parsed->year == regression.year);
        else CHECK(!parsed->year.has_value());
    }

    auto multi_episode = probe_media_path(
        "/TV/Battlestar Galactica (2003)/Season 4/Battlestar Galactica (2003) - S04E19-E20 - Daybreak (1080p BluRay x265 RZeroX).mkv", fake);
    REQUIRE(multi_episode.has_value());
    CHECK(multi_episode->kind == MediaProbeKind::episode);
    CHECK(multi_episode->series == "Battlestar Galactica");
    CHECK(multi_episode->year == 2003);
    CHECK(multi_episode->season == 4);
    CHECK(multi_episode->episode == 19);
    CHECK(multi_episode->episode_end == 20);
    CHECK(multi_episode->title == "Daybreak");

    auto special_directory = probe_media_path(
        "/TV/Battlestar Galactica (2003)/Specials/Battlestar Galactica (2003) - S00E23 - The Resistance (1) (480p BluRay x265 RZeroX).mkv", fake);
    REQUIRE(special_directory.has_value());
    CHECK(special_directory->series == "Battlestar Galactica");
    CHECK(special_directory->year == 2003);
    CHECK(special_directory->season == 0);
    CHECK(special_directory->episode == 23);
    CHECK(special_directory->title == "The Resistance (1)");

    const auto battlestar_path =
        "/TV/Battlestar Galactica (2003) Season 1-4 S01-S04 (1080p BluRay x265 HEVC 10bit AAC 5.1 RZeroX)/Season 2/Battlestar Galactica (2003) - S02E01 - Scattered (1080p BluRay x265 RZeroX).mkv";
    auto battlestar_candidates = probe_media_candidates(battlestar_path, fake, "/TV");
    auto battlestar_yearless = std::find_if(
        battlestar_candidates.begin(), battlestar_candidates.end(), [](const auto& candidate) {
            return candidate.generator == "episode-filename-yearless";
        });
    REQUIRE(battlestar_yearless != battlestar_candidates.end());
    CHECK(battlestar_yearless->probe.series == "Battlestar Galactica");
    CHECK(!battlestar_yearless->probe.year.has_value());
    CHECK(battlestar_yearless->probe.season == 2);
    CHECK(battlestar_yearless->probe.episode == 1);

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

    auto discography_track = probe_media_path(
        "/Music/A Tribe Called Quest Discography @ 320 (8 Albums)(RAP)(by dragan09)/1990 - Peoples Instinctive Travels And The Path/16 Can I Kick It_ (Extended Bollerho.mp3", fake);
    REQUIRE(discography_track.has_value());
    CHECK(discography_track->artist == "A Tribe Called Quest");
    CHECK(discography_track->album == "Peoples Instinctive Travels And The Path");
    CHECK(discography_track->year == 1990);
    CHECK(discography_track->track == 16);

    MediaProbe tagged_music;
    tagged_music.kind = MediaProbeKind::track;
    tagged_music.path = "/Music/Various Artists/Collected/04 - Teardrop.flac";
    tagged_music.media_id = "macha:test-tagged-track";
    tagged_music.title = "Teardrop";
    tagged_music.album = "Collected";
    tagged_music.album_artist = "Various Artists";
    tagged_music.track_artist = "Massive Attack";
    tagged_music.artist = tagged_music.album_artist;
    auto tagged_candidates = probe_media_candidates(
        MediaProbeContext{"/Music", tagged_music.path, fake, &tagged_music});
    auto embedded_candidate = std::find_if(tagged_candidates.begin(), tagged_candidates.end(),
                                           [](const auto& candidate) {
                                               return candidate.generator == "music-embedded-tags";
                                           });
    auto recording_candidate = std::find_if(tagged_candidates.begin(), tagged_candidates.end(),
                                            [](const auto& candidate) {
                                                return candidate.generator == "music-recording-tags";
                                            });
    auto merged_candidate = std::find_if(tagged_candidates.begin(), tagged_candidates.end(),
                                         [](const auto& candidate) {
                                             return candidate.generator == "music-tags-plus-path";
                                         });
    REQUIRE(embedded_candidate != tagged_candidates.end());
    REQUIRE(recording_candidate != tagged_candidates.end());
    REQUIRE(merged_candidate != tagged_candidates.end());
    CHECK(embedded_candidate->probe.artist == "Various Artists");
    CHECK(embedded_candidate->probe.lookup_strategy ==
          MediaProbeLookupStrategy::music_release_first);
    CHECK(recording_candidate->probe.artist == "Massive Attack");
    CHECK(recording_candidate->probe.album == "Collected");
    CHECK(recording_candidate->probe.lookup_strategy ==
          MediaProbeLookupStrategy::music_recording_first);
    CHECK(merged_candidate->probe.track == 4);

    auto untagged_music_candidates = probe_media_candidates(
        "/Music/Pink Floyd/The Dark Side of the Moon/01 - Speak to Me.flac", fake, "/Music");
    auto structured_recording = std::find_if(
        untagged_music_candidates.begin(), untagged_music_candidates.end(), [](const auto& candidate) {
            return candidate.generator == "music-structured-recording";
        });
    REQUIRE(structured_recording != untagged_music_candidates.end());
    CHECK(structured_recording->probe.lookup_strategy ==
          MediaProbeLookupStrategy::music_recording_first);

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

    // A one-year TV premiere difference is evidence, not a hard rejection.
    // Release folders frequently use a pilot/miniseries/production year.
    FakeHttpClient adjacent_year_http;
    adjacent_year_http.add("query=Adjacent%20Year%20Show&language=en-GB", 200, "application/json",
                           R"({"results":[{"id":1500,"name":"Adjacent Year Show","first_air_date":"2004-01-01"}]})");
    adjacent_year_http.add("/tv/1500/season/1", 200, "application/json",
                           R"({"id":1501,"name":"Season 1","episodes":[{"id":1502,"episode_number":1,"name":"Pilot"}]})");
    TmdbProvider adjacent_year_tmdb(adjacent_year_http, tmdb_config);
    MediaProbe adjacent_year_probe;
    adjacent_year_probe.kind = MediaProbeKind::episode;
    adjacent_year_probe.series = "Adjacent Year Show";
    adjacent_year_probe.year = 2003;
    adjacent_year_probe.season = 1;
    adjacent_year_probe.episode = 1;
    adjacent_year_probe.title = "Pilot";
    adjacent_year_probe.media_id = "macha:adjacent-year";
    CHECK(adjacent_year_tmdb.lookup(adjacent_year_probe).has_value());

    // Positive show/season results are cached as the actual JSON objects, not
    // merely as truthy values. A second episode lookup must therefore remain
    // usable without issuing another provider request.
    const auto tv_requests = tmdb_http.requests();
    auto tv_cached = tmdb.lookup(tv_probe);
    REQUIRE(tv_cached.has_value());
    CHECK(tv_cached->items.size() == 3);
    CHECK(tmdb_http.requests() == tv_requests);

    // A year in a release name can identify a miniseries or special rather than
    // TMDB's canonical ongoing series. A missing season is a semantic mismatch:
    // cache that negative season result and let the scanner try the yearless
    // episode candidate without repeatedly spending one request per episode.
    FakeHttpClient battlestar_http;
    battlestar_http.add("query=Battlestar%20Galactica&language=en-GB", 200, "application/json",
                        R"({"results":[{"id":1972,"name":"Battlestar Galactica","first_air_date":"2004-10-18"},{"id":101,"name":"Battlestar Galactica","first_air_date":"2003-12-08"}]})");
    battlestar_http.add("/tv/101/season/2", 404, "application/json", R"({})");
    battlestar_http.add("/tv/1972/season/2", 200, "application/json",
                        R"({"id":202,"name":"Season 2","episodes":[{"id":203,"episode_number":1,"name":"Scattered"},{"id":204,"episode_number":2,"name":"Valley of Darkness"}]})");
    TmdbProvider battlestar_tmdb(battlestar_http, tmdb_config);
    MediaProbe battlestar_probe;
    battlestar_probe.kind = MediaProbeKind::episode;
    battlestar_probe.series = "Battlestar Galactica";
    battlestar_probe.year = 2003;
    battlestar_probe.season = 2;
    battlestar_probe.episode = 1;
    battlestar_probe.title = "Scattered";
    battlestar_probe.media_id = "macha:test-bsg";
    CHECK(!battlestar_tmdb.lookup(battlestar_probe).has_value());
    CHECK(battlestar_http.requests() == 2);
    battlestar_probe.episode = 2;
    battlestar_probe.title = "Valley of Darkness";
    CHECK(!battlestar_tmdb.lookup(battlestar_probe).has_value());
    CHECK(battlestar_http.requests() == 2);
    battlestar_probe.year.reset();
    battlestar_probe.episode = 1;
    battlestar_probe.title = "Scattered";
    auto battlestar_match = battlestar_tmdb.lookup(battlestar_probe);
    REQUIRE(battlestar_match.has_value());
    CHECK(battlestar_match->items.back().title == "Scattered");
    CHECK(battlestar_http.requests() == 4);

    battlestar_probe.title = "Definitely Not Scattered";
    CHECK(!battlestar_tmdb.lookup(battlestar_probe).has_value());
    CHECK(battlestar_http.requests() == 4);

    // Strong series/year/season/episode identity must not be vetoed by small
    // filename-title differences. TMDB's title is canonical; release titles are
    // secondary evidence once the numbered identity is strong.
    FakeHttpClient title_variation_http;
    title_variation_http.add("query=Blue%20Lights&language=en-GB", 200, "application/json",
                             R"({"results":[{"id":2000,"name":"Blue Lights","first_air_date":"2023-03-27"}]})");
    title_variation_http.add("/tv/2000/season/1", 200, "application/json",
                             R"({"id":2001,"name":"Season 1","episodes":[{"id":2006,"episode_number":6,"name":"Love the One You're With"}]})");
    TmdbProvider title_variation_tmdb(title_variation_http, tmdb_config);
    MediaProbe title_variation_probe;
    title_variation_probe.kind = MediaProbeKind::episode;
    title_variation_probe.series = "Blue Lights";
    title_variation_probe.year = 2023;
    title_variation_probe.season = 1;
    title_variation_probe.episode = 6;
    title_variation_probe.title = "Love the One You Are With";
    title_variation_probe.media_id = "macha:blue-lights-s01e06";
    auto title_variation_match = title_variation_tmdb.lookup(title_variation_probe);
    REQUIRE(title_variation_match.has_value());
    CHECK(title_variation_match->items.back().title == "Love the One You're With");

    // Dots replacing apostrophes in release names are a punctuation artefact,
    // not evidence that an otherwise matching episode is different.
    FakeHttpClient possessive_http;
    possessive_http.add("query=Black%20Jesus&language=en-GB", 200, "application/json",
                        R"({"results":[{"id":2100,"name":"Black Jesus","first_air_date":"2014-08-07"}]})");
    possessive_http.add("/tv/2100/season/2", 200, "application/json",
                        R"({"id":2101,"name":"Season 2","episodes":[{"id":2105,"episode_number":5,"name":"Tasty Tudi's"}]})");
    TmdbProvider possessive_tmdb(possessive_http, tmdb_config);
    MediaProbe possessive_probe;
    possessive_probe.kind = MediaProbeKind::episode;
    possessive_probe.series = "Black Jesus";
    possessive_probe.season = 2;
    possessive_probe.episode = 5;
    possessive_probe.title = "Tasty Tudi's";
    possessive_probe.media_id = "macha:black-jesus-s02e05";
    auto possessive_match = possessive_tmdb.lookup(possessive_probe);
    REQUIRE(possessive_match.has_value());
    CHECK(possessive_match->items.back().title == "Tasty Tudi's");

    // Specials are especially prone to numbering differences between metadata
    // ordering schemes. Keep the already-resolved show/season, but allow a very
    // strong title to remap the local special number to TMDB's canonical one.
    FakeHttpClient special_remap_http;
    special_remap_http.add("query=Battlestar%20Galactica&language=en-GB", 200, "application/json",
                           R"({"results":[{"id":1972,"name":"Battlestar Galactica","first_air_date":"2004-10-18"}]})");
    special_remap_http.add("/tv/1972/season/0", 200, "application/json",
                           R"json({"id":2200,"name":"Specials","episodes":[{"id":2202,"episode_number":2,"name":"The Resistance (1)"},{"id":2223,"episode_number":23,"name":"Unrelated Special"}]})json");
    TmdbProvider special_remap_tmdb(special_remap_http, tmdb_config);
    MediaProbe special_remap_probe;
    special_remap_probe.kind = MediaProbeKind::episode;
    special_remap_probe.series = "Battlestar Galactica";
    special_remap_probe.season = 0;
    special_remap_probe.episode = 23;
    special_remap_probe.title = "The Resistance (1)";
    special_remap_probe.media_id = "macha:bsg-resistance-1";
    auto special_remap_match = special_remap_tmdb.lookup(special_remap_probe);
    REQUIRE(special_remap_match.has_value());
    CHECK(special_remap_match->items.back().episode_number == 2);
    CHECK(special_remap_match->items.back().title == "The Resistance (1)");

    // Some legacy Specials layouts contain a programme that TMDB models as a
    // separate one-season TV entity. Exact-year identity plus missing season 0
    // is enough to try the corresponding season-1 episode.
    FakeHttpClient miniseries_http;
    miniseries_http.add("query=Battlestar%20Galactica&language=en-GB", 200, "application/json",
                        R"({"results":[{"id":101,"name":"Battlestar Galactica","first_air_date":"2003-12-08"}]})");
    miniseries_http.add("/tv/101/season/0", 404, "application/json", R"({})");
    miniseries_http.add("/tv/101/season/1", 200, "application/json",
                        R"({"id":2300,"name":"Miniseries","episodes":[{"id":2301,"episode_number":1,"name":"Part 1"},{"id":2302,"episode_number":2,"name":"Part 2"}]})");
    TmdbProvider miniseries_tmdb(miniseries_http, tmdb_config);
    MediaProbe miniseries_probe;
    miniseries_probe.kind = MediaProbeKind::episode;
    miniseries_probe.series = "Battlestar Galactica";
    miniseries_probe.year = 2003;
    miniseries_probe.season = 0;
    miniseries_probe.episode = 1;
    miniseries_probe.title = "Battlestar Galactica The Miniseries (1)";
    miniseries_probe.media_id = "macha:bsg-miniseries-1";
    auto miniseries_match = miniseries_tmdb.lookup(miniseries_probe);
    REQUIRE(miniseries_match.has_value());
    CHECK(miniseries_match->items[1].season_number == 1);
    CHECK(miniseries_match->items.back().episode_number == 1);

    // If a legacy special is a standalone TMDB movie rather than an episode,
    // recover it through the movie catalogue path instead of dropping it.
    FakeHttpClient standalone_special_http;
    standalone_special_http.add("query=Battlestar%20Galactica&language=en-GB", 200, "application/json",
                                R"({"results":[{"id":101,"name":"Battlestar Galactica","first_air_date":"2003-12-08"}]})");
    standalone_special_http.add("/tv/101/season/0", 404, "application/json", R"({})");
    standalone_special_http.add("/tv/101/season/1", 200, "application/json",
                                R"({"id":2400,"name":"Miniseries","episodes":[{"id":2401,"episode_number":1,"name":"Part 1"},{"id":2402,"episode_number":2,"name":"Part 2"}]})");
    standalone_special_http.add("query=Battlestar%20Galactica%20The%20Plan&language=en-GB", 200, "application/json",
                                R"({"results":[{"id":2403,"title":"Battlestar Galactica: The Plan","release_date":"2009-10-27"}]})");
    standalone_special_http.add("/movie/2403", 200, "application/json",
                                R"({"id":2403,"title":"Battlestar Galactica: The Plan","release_date":"2009-10-27"})");
    TmdbProvider standalone_special_tmdb(standalone_special_http, tmdb_config);
    MediaProbe standalone_special_probe;
    standalone_special_probe.kind = MediaProbeKind::episode;
    standalone_special_probe.series = "Battlestar Galactica";
    standalone_special_probe.year = 2003;
    standalone_special_probe.season = 0;
    standalone_special_probe.episode = 22;
    standalone_special_probe.title = "The Plan";
    standalone_special_probe.media_id = "macha:bsg-the-plan";
    auto standalone_special_match = standalone_special_tmdb.lookup(standalone_special_probe);
    REQUIRE(standalone_special_match.has_value());
    REQUIRE(standalone_special_match->items.size() == 1);
    CHECK(standalone_special_match->items.front().kind == CatalogueKind::movie);
    CHECK(standalone_special_match->items.front().title == "Battlestar Galactica: The Plan");
    CHECK(standalone_special_match->items.front().media_ids ==
          std::vector<std::string>{"macha:bsg-the-plan"});

    // Multi-episode files retain one media object while emitting every TMDB
    // episode identity covered by the filename range.
    FakeHttpClient range_http;
    range_http.add("query=Range%20Show&language=en-GB", 200, "application/json",
                   R"({"results":[{"id":2500,"name":"Range Show","first_air_date":"2020-01-01"}]})");
    range_http.add("/tv/2500/season/4", 200, "application/json",
                   R"({"id":2501,"name":"Season 4","episodes":[{"id":2519,"episode_number":19,"name":"Part One"},{"id":2520,"episode_number":20,"name":"Part Two"}]})");
    TmdbProvider range_tmdb(range_http, tmdb_config);
    MediaProbe range_probe;
    range_probe.kind = MediaProbeKind::episode;
    range_probe.series = "Range Show";
    range_probe.year = 2020;
    range_probe.season = 4;
    range_probe.episode = 19;
    range_probe.episode_end = 20;
    range_probe.title = "Combined Finale";
    range_probe.media_id = "macha:range-show-finale";
    auto range_match = range_tmdb.lookup(range_probe);
    REQUIRE(range_match.has_value());
    REQUIRE(range_match->items.size() == 4);
    CHECK(range_match->items[2].episode_number == 19);
    CHECK(range_match->items[3].episode_number == 20);
    CHECK(range_match->items[2].media_ids == std::vector<std::string>{"macha:range-show-finale"});
    CHECK(range_match->items[3].media_ids == std::vector<std::string>{"macha:range-show-finale"});

    // Provider title scoring must tolerate common number spelling differences
    // between release filenames and canonical provider titles. The year remains
    // part of the score, so this does not turn matching into a first-result win.
    FakeHttpClient movie_http;
    movie_http.add("query=Men%20In%20Black%202&language=en-GB&primary_release_year=2002",
                   200, "application/json",
                   R"({"results":[{"id":1001,"title":"Men in Black II","release_date":"2002-07-03"}]})");
    movie_http.add("/movie/1001", 200, "application/json",
                   R"({"id":1001,"title":"Men in Black II","release_date":"2002-07-03"})");
    movie_http.add("query=12%20Monkeys&language=en-GB&primary_release_year=1995",
                   200, "application/json",
                   R"({"results":[{"id":1002,"title":"Twelve Monkeys","release_date":"1995-12-29"}]})");
    movie_http.add("/movie/1002", 200, "application/json",
                   R"({"id":1002,"title":"Twelve Monkeys","release_date":"1995-12-29"})");
    movie_http.add("query=A%20Knights%20Tale&language=en-GB&primary_release_year=2001",
                   200, "application/json",
                   R"({"results":[{"id":1003,"title":"A Knight's Tale","release_date":"2001-05-11"}]})");
    movie_http.add("/movie/1003", 200, "application/json",
                   R"({"id":1003,"title":"A Knight's Tale","release_date":"2001-05-11"})");
    movie_http.add("query=Kill%20Bill%20Volume%201&language=en-GB&primary_release_year=2003",
                   200, "application/json",
                   R"({"results":[{"id":1004,"title":"Kill Bill: Vol. 1","release_date":"2003-10-10"}]})");
    movie_http.add("/movie/1004", 200, "application/json",
                   R"({"id":1004,"title":"Kill Bill: Vol. 1","release_date":"2003-10-10"})");
    movie_http.add("query=Shrek%204&language=en-GB&primary_release_year=2010",
                   200, "application/json",
                   R"({"results":[{"id":1005,"title":"Shrek Forever After","release_date":"2010-05-16"}]})");
    movie_http.add("/movie/1005", 200, "application/json",
                   R"({"id":1005,"title":"Shrek Forever After","release_date":"2010-05-16"})");
    movie_http.add("query=Shichinin%20no%20samurai&language=en-GB&primary_release_year=1954",
                   200, "application/json",
                   R"({"results":[{"id":1006,"title":"Seven Samurai","release_date":"1954-04-26"}]})");
    movie_http.add("/movie/1006", 200, "application/json",
                   R"({"id":1006,"title":"Seven Samurai","release_date":"1954-04-26"})");
    movie_http.add("query=Rebel%20Moon%20Part%20One&language=en-GB",
                   200, "application/json",
                   R"({"results":[{"id":1007,"title":"Rebel Moon - Part One: A Child of Fire","release_date":"2023-12-15"}]})");
    movie_http.add("/movie/1007", 200, "application/json",
                   R"({"id":1007,"title":"Rebel Moon - Part One: A Child of Fire","release_date":"2023-12-15"})");
    TmdbProvider movie_tmdb(movie_http, tmdb_config);

    MediaProbe mib_probe;
    mib_probe.kind = MediaProbeKind::movie;
    mib_probe.title = "Men In Black 2";
    mib_probe.year = 2002;
    mib_probe.media_id = "macha:test-mib2";
    auto mib_match = movie_tmdb.lookup(mib_probe);
    REQUIRE(mib_match.has_value());
    REQUIRE(mib_match->items.size() == 1);
    CHECK(mib_match->items.front().title == "Men in Black II");
    CHECK(mib_match->items.front().media_ids == std::vector<std::string>{"macha:test-mib2"});

    MediaProbe monkeys_probe;
    monkeys_probe.kind = MediaProbeKind::movie;
    monkeys_probe.title = "12 Monkeys";
    monkeys_probe.year = 1995;
    monkeys_probe.media_id = "macha:test-12-monkeys";
    auto monkeys_match = movie_tmdb.lookup(monkeys_probe);
    REQUIRE(monkeys_match.has_value());
    REQUIRE(monkeys_match->items.size() == 1);
    CHECK(monkeys_match->items.front().title == "Twelve Monkeys");
    CHECK(monkeys_match->items.front().media_ids == std::vector<std::string>{"macha:test-12-monkeys"});

    auto punctuation_probe = mib_probe;
    punctuation_probe.title = "A Knights Tale";
    punctuation_probe.year = 2001;
    punctuation_probe.media_id = "macha:test-knights";
    auto punctuation_match = movie_tmdb.lookup(punctuation_probe);
    REQUIRE(punctuation_match.has_value());
    CHECK(punctuation_match->items.front().title == "A Knight's Tale");

    auto volume_probe = mib_probe;
    volume_probe.title = "Kill Bill Volume 1";
    volume_probe.year = 2003;
    volume_probe.media_id = "macha:test-kill-bill";
    auto volume_match = movie_tmdb.lookup(volume_probe);
    REQUIRE(volume_match.has_value());
    CHECK(volume_match->items.front().title == "Kill Bill: Vol. 1");

    auto franchise_probe = mib_probe;
    franchise_probe.title = "Shrek 4";
    franchise_probe.year = 2010;
    franchise_probe.media_id = "macha:test-shrek4";
    auto franchise_match = movie_tmdb.lookup(franchise_probe);
    REQUIRE(franchise_match.has_value());
    CHECK(franchise_match->items.front().title == "Shrek Forever After");

    auto alias_probe = mib_probe;
    alias_probe.title = "Shichinin no samurai";
    alias_probe.year = 1954;
    alias_probe.media_id = "macha:test-seven-samurai";
    auto alias_match = movie_tmdb.lookup(alias_probe);
    REQUIRE(alias_match.has_value());
    CHECK(alias_match->items.front().title == "Seven Samurai");

    auto expanded_title_probe = mib_probe;
    expanded_title_probe.title = "Rebel Moon Part One";
    expanded_title_probe.year.reset();
    expanded_title_probe.media_id = "macha:test-rebel-moon";
    auto expanded_title_match = movie_tmdb.lookup(expanded_title_probe);
    REQUIRE(expanded_title_match.has_value());
    CHECK(expanded_title_match->items.front().title == "Rebel Moon - Part One: A Child of Fire");

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

    // If tags/filename give artist+title but no trustworthy album, use a
    // recording search rather than inventing a release from directory names.
    FakeHttpClient mb_recording_http;
    mb_recording_http.add("/ws/2/recording?", 200, "application/json",
        R"JSON({"recordings":[{"id":"rec-2","title":"The First Time (Raven Remix)","score":100,"artist-credit":[{"name":"Scooter","artist":{"id":"artist-2","name":"Scooter"}}]}]})JSON");
    mb_recording_http.add("/ws/2/recording/rec-2", 200, "application/json",
        R"JSON({"id":"rec-2","title":"The First Time (Raven Remix)","artist-credit":[{"name":"Scooter","artist":{"id":"artist-2","name":"Scooter"}}],"releases":[{"id":"rel-2","title":"The First Time"}]})JSON");
    mb_recording_http.add("/ws/2/release/rel-2", 200, "application/json",
        R"JSON({"id":"rel-2","title":"The First Time","date":"1995-05-01","artist-credit":[{"name":"Scooter","artist":{"id":"artist-2","name":"Scooter"}}],"release-group":{"id":"rg-2"},"media":[{"position":1,"tracks":[{"position":1,"title":"The First Time (Raven Remix)","recording":{"id":"rec-2","title":"The First Time (Raven Remix)"}}]}]})JSON");
    MusicBrainzProvider mb_recording(mb_recording_http, mb_config);
    MediaProbe recording_probe;
    recording_probe.kind = MediaProbeKind::track;
    recording_probe.artist = "Scooter";
    recording_probe.title = "The First Time (Raven Remix)";
    recording_probe.media_id = "macha:test-recording-fallback";
    auto recording_match = mb_recording.lookup(recording_probe);
    REQUIRE(recording_match.has_value());
    CHECK(recording_match->items.size() == 3);
    CHECK(recording_match->items[1].title == "The First Time");
    CHECK(recording_match->items[2].external_ids.at("musicbrainz") == "rec-2");

    FakeHttpClient mb_compilation_http;
    mb_compilation_http.add("/ws/2/recording?", 200, "application/json",
        R"JSON({"recordings":[{"id":"rec-3","title":"Teardrop","score":100,"artist-credit":[{"name":"Massive Attack","artist":{"id":"artist-3","name":"Massive Attack"}}]}]})JSON");
    mb_compilation_http.add("/ws/2/recording/rec-3", 200, "application/json",
        R"JSON({"id":"rec-3","title":"Teardrop","artist-credit":[{"name":"Massive Attack","artist":{"id":"artist-3","name":"Massive Attack"}}],"releases":[{"id":"rel-other","title":"Mezzanine"},{"id":"rel-collected","title":"Collected"}]})JSON");
    mb_compilation_http.add("/ws/2/release/rel-collected", 200, "application/json",
        R"JSON({"id":"rel-collected","title":"Collected","date":"2006-03-27","artist-credit":[{"name":"Various Artists","artist":{"id":"artist-va","name":"Various Artists"}}],"release-group":{"id":"rg-collected"},"media":[{"position":1,"tracks":[{"position":4,"title":"Teardrop","recording":{"id":"rec-3","title":"Teardrop"}}]}]})JSON");
    MusicBrainzProvider mb_compilation(mb_compilation_http, mb_config);
    MediaProbe compilation_probe;
    compilation_probe.kind = MediaProbeKind::track;
    compilation_probe.artist = "Massive Attack";
    compilation_probe.album = "Collected";
    compilation_probe.title = "Teardrop";
    compilation_probe.track = 4;
    compilation_probe.lookup_strategy = MediaProbeLookupStrategy::music_recording_first;
    compilation_probe.media_id = "macha:test-compilation";
    auto compilation_match = mb_compilation.lookup(compilation_probe);
    REQUIRE(compilation_match.has_value());
    CHECK(compilation_match->items[0].title == "Various Artists");
    CHECK(compilation_match->items[1].title == "Collected");
    CHECK(compilation_match->items[2].title == "Teardrop");

    // Semantic provider misses are process-lifetime negative cache entries.
    // Only a successful provider response saying "no match" is suppressed on
    // later tracks/scans; transient failures use the provider circuit instead.
    FakeHttpClient mb_miss_http;
    mb_miss_http.add("/ws/2/release?", 200, "application/json", R"({"releases":[]})");
    MusicBrainzProvider mb_miss(mb_miss_http, mb_config);
    MediaProbe missing_track = music_probe;
    missing_track.album = "Definitely Missing Album";
    missing_track.title = "Track One";
    CHECK(!mb_miss.lookup(missing_track).has_value());
    CHECK(mb_miss_http.requests() == 1);
    for (int track_number = 2; track_number <= 128; ++track_number) {
        missing_track.title = "Track " + std::to_string(track_number);
        missing_track.track = track_number;
        CHECK(!mb_miss.lookup(missing_track).has_value());
    }
    CHECK(mb_miss_http.requests() == 1);

    FakeHttpClient mb_error_http;
    mb_error_http.add("/ws/2/release?", 503, "application/json", R"({})");
    MusicBrainzProvider mb_error(mb_error_http, mb_config);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            (void)mb_error.lookup(music_probe);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
    CHECK(mb_error_http.requests() == 1);

    // A transient MusicBrainz outage must not burn every music hypothesis.
    // The circuit opens after one 503 and Discogs receives the same candidate.
    TempDir discogs_temp;
    auto discogs_token = discogs_temp.path() / "discogs.token";
    {
        std::ofstream out(discogs_token);
        out << "discogs-test-token\n";
    }
    FakeHttpClient fallback_music_http;
    fallback_music_http.add("musicbrainz.org/ws/2", 503, "application/json", R"({})");
    fallback_music_http.add("api.discogs.com/database/search", 200, "application/json",
                            R"({"results":[{"id":500,"type":"release","title":"Clannad - Crann Ull","year":1980}]})");
    fallback_music_http.add("api.discogs.com/releases/500", 200, "application/json",
                            R"({"id":500,"title":"Crann Ull","year":1980,"master_id":600,"artists":[{"id":700,"name":"Clannad"}],"tracklist":[{"position":"7","type_":"track","title":"Gathering Mushrooms"}],"images":[{"type":"primary","uri":"https://img.discogs.example/500.jpg"}]})");
    CatalogueMusicProviderConfig fallback_music_config;
    fallback_music_config.roots = {"/Music"};
    fallback_music_config.musicbrainz.enabled = true;
    fallback_music_config.discogs.enabled = true;
    fallback_music_config.discogs.token_file = discogs_token;
    MusicScanProvider fallback_music(fallback_music_http, fallback_music_config);
    MediaProbe discogs_probe;
    discogs_probe.kind = MediaProbeKind::track;
    discogs_probe.path = "/Music/Clannad/1980 - Crann Ull/07.Clannad - Gathering Mushrooms.mp3";
    discogs_probe.media_id = "macha:discogs-fallback";
    discogs_probe.artist = "Clannad";
    discogs_probe.album = "Crann Ull";
    discogs_probe.title = "Gathering Mushrooms";
    discogs_probe.year = 1980;
    discogs_probe.track = 7;
    discogs_probe.lookup_strategy = MediaProbeLookupStrategy::music_recording_first;
    auto discogs_match = fallback_music.lookup(discogs_probe);
    REQUIRE(discogs_match.has_value());
    CHECK(discogs_match->items.size() == 3);
    CHECK(discogs_match->items[0].id == "discogs:artist:700");
    CHECK(discogs_match->items[1].id == "discogs:album:master:600");
    CHECK(discogs_match->items[2].id == "discogs:track:500:7");
    CHECK(discogs_match->items[2].title == "Gathering Mushrooms");
    CHECK(fallback_music_http.requests_containing("musicbrainz.org/ws/2") == 1);
    CHECK(fallback_music_http.requests_containing("api.discogs.com/database/search") == 1);
    CHECK(fallback_music_http.requests_containing("api.discogs.com/releases/500") == 1);

    // The MusicBrainz circuit is still open, while Discogs' successful search
    // and release detail are cached.
    auto discogs_cached = fallback_music.lookup(discogs_probe);
    REQUIRE(discogs_cached.has_value());
    CHECK(fallback_music_http.requests_containing("musicbrainz.org/ws/2") == 1);
    CHECK(fallback_music_http.requests_containing("api.discogs.com/database/search") == 1);
    CHECK(fallback_music_http.requests_containing("api.discogs.com/releases/500") == 1);

    FakeHttpClient tmdb_miss_http;
    tmdb_miss_http.add("/search/movie", 200, "application/json", R"({"results":[]})");
    TmdbProvider tmdb_miss(tmdb_miss_http, tmdb_config);
    MediaProbe missing_movie;
    missing_movie.kind = MediaProbeKind::movie;
    missing_movie.title = "Definitely Missing Movie";
    missing_movie.year = 2026;
    CHECK(!tmdb_miss.lookup(missing_movie).has_value());
    CHECK(tmdb_miss_http.requests() == 1);
    CHECK(!tmdb_miss.lookup(missing_movie).has_value());
    CHECK(tmdb_miss_http.requests() == 1);

    // Transport/provider failures are deliberately not negative-cached: the
    // next scan gets another chance after a transient outage.
    FakeHttpClient tmdb_error_http;
    tmdb_error_http.add("/search/movie", 503, "application/json", R"({})");
    TmdbProvider tmdb_error(tmdb_error_http, tmdb_config);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            (void)tmdb_error.lookup(missing_movie);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
    CHECK(tmdb_error_http.requests() == 2);

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

    // Music scanning is tag-first and provider-root scoped. Deliberately put a
    // tagged MP3 under misleading collection/grouping directories: embedded
    // metadata must win, while untagged filename fallback must not manufacture
    // an artist/album from those same directories.
    service.filesystem().mkdir("/Music", 0755, getuid(), getgid());
    const std::string collection = "/Music/Scooter Full Discography (Albums & Singles 1994-2011)";
    service.filesystem().mkdir(collection, 0755, getuid(), getgid());
    const std::string singles = collection + "/Singles";
    service.filesystem().mkdir(singles, 0755, getuid(), getgid());
    const std::string tagged_album = singles + "/14 - [1996] I'm Raving The Remixes CDM";
    service.filesystem().mkdir(tagged_album, 0755, getuid(), getgid());
    const std::string tagged_path = tagged_album + "/01 - Completely Wrong.mp3";

    auto fixture_bytes = [](const char* name) {
        auto path = std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "tests" / "fixtures" / name;
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return Bytes(bytes.begin(), bytes.end());
    };
    auto write_fixture = [&](const std::string& path, const Bytes& bytes) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto writer = service.filesystem().open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    };

    const auto tagged_bytes = fixture_bytes("tagged.mp3");
    const auto untagged_bytes = fixture_bytes("untagged.mp3");
    const Bytes embedded_cover{
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xde,
        0x00, 0x00, 0x00, 0x0c, 'I', 'D', 'A', 'T', 0x78, 0x9c,
        0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00,
        0xc9, 0xfe, 0x92, 0xef, 0x00, 0x00, 0x00, 0x00, 'I', 'E',
        'N', 'D', 0xae, 0x42, 0x60, 0x82};
    auto with_apic = [](const Bytes& input, const Bytes& cover) {
        if (input.size() < 10 || std::string_view(reinterpret_cast<const char*>(input.data()), 3) != "ID3")
            throw std::runtime_error("tagged MP3 fixture has no ID3 header");
        auto decode_synchsafe = [](const uint8_t* p) -> size_t {
            return (static_cast<size_t>(p[0] & 0x7f) << 21) |
                   (static_cast<size_t>(p[1] & 0x7f) << 14) |
                   (static_cast<size_t>(p[2] & 0x7f) << 7) |
                   static_cast<size_t>(p[3] & 0x7f);
        };
        auto encode_synchsafe = [](size_t n) {
            return std::array<uint8_t, 4>{
                static_cast<uint8_t>((n >> 21) & 0x7f),
                static_cast<uint8_t>((n >> 14) & 0x7f),
                static_cast<uint8_t>((n >> 7) & 0x7f),
                static_cast<uint8_t>(n & 0x7f)};
        };
        const auto tag_size = decode_synchsafe(input.data() + 6);
        if (10 + tag_size > input.size()) throw std::runtime_error("invalid ID3 fixture size");
        Bytes payload{0x03};
        const std::string mime = "image/png";
        payload.insert(payload.end(), mime.begin(), mime.end());
        payload.push_back(0);
        payload.push_back(0x03); // front cover
        payload.push_back(0);   // empty UTF-8 description
        payload.insert(payload.end(), cover.begin(), cover.end());
        Bytes frame{'A', 'P', 'I', 'C'};
        auto frame_size = encode_synchsafe(payload.size());
        frame.insert(frame.end(), frame_size.begin(), frame_size.end());
        frame.push_back(0);
        frame.push_back(0);
        frame.insert(frame.end(), payload.begin(), payload.end());

        Bytes result;
        result.reserve(input.size() + frame.size());
        result.insert(result.end(), input.begin(), input.begin() + 6);
        auto new_tag_size = encode_synchsafe(tag_size + frame.size());
        result.insert(result.end(), new_tag_size.begin(), new_tag_size.end());
        result.insert(result.end(), input.begin() + 10, input.begin() + 10 + tag_size);
        result.insert(result.end(), frame.begin(), frame.end());
        result.insert(result.end(), input.begin() + 10 + tag_size, input.end());
        return result;
    };
    const auto tagged_with_cover = with_apic(tagged_bytes, embedded_cover);
    write_fixture(tagged_path, tagged_with_cover);

    FakeHttpClient music_probe_http;
    CatalogueMusicProviderConfig music_source_config;
    music_source_config.roots = {"/Music"};
    music_source_config.musicbrainz.enabled = false;
    MusicScanProvider music_source(music_probe_http, music_source_config);
    auto tagged_entry = service.filesystem().getattr(tagged_path);
    auto tagged_file = music_source.probe_file(service.filesystem(), "/Music", tagged_path, tagged_entry);
    REQUIRE(!tagged_file.candidates.empty());
    REQUIRE(tagged_file.artwork.size() == 1);
    CHECK(tagged_file.artwork.front().role == "cover");
    CHECK(tagged_file.artwork.front().mime_type == "image/png");
    CHECK(tagged_file.artwork.front().bytes == embedded_cover);
    auto tagged_probe = std::optional<MediaProbe>{tagged_file.candidates.front().probe};
    REQUIRE(tagged_probe.has_value());
    CHECK(tagged_probe->artist == "Scooter");
    CHECK(tagged_probe->album == "I'm Raving The Remixes");
    CHECK(tagged_probe->title == "I'm Raving (Progressive Remix)");
    CHECK(tagged_probe->track == 1);
    CHECK(tagged_probe->disc == 1);
    CHECK(tagged_probe->year == 1996);
    CHECK(tagged_probe->musicbrainz_recording_id == std::optional<std::string>{"rec-tagged-1"});
    CHECK(tagged_probe->musicbrainz_release_id == std::optional<std::string>{"rel-tagged-1"});
    CHECK(tagged_probe->musicbrainz_artist_id == std::optional<std::string>{"artist-tagged-1"});

    // Embedded APIC artwork and provider artwork are independent catalogue
    // candidates. Neither should suppress or replace the other merely because
    // both have the semantic role "cover".
    auto music_art_http = std::make_unique<FakeHttpClient>();
    music_art_http->add("/ws/2/release/rel-tagged-1", 200, "application/json",
        R"JSON({"id":"rel-tagged-1","title":"I'm Raving The Remixes","date":"1996-01-01","artist-credit":[{"name":"Scooter","artist":{"id":"artist-tagged-1","name":"Scooter"}}],"release-group":{"id":"rg-tagged-1"},"media":[{"position":1,"tracks":[{"position":1,"title":"I'm Raving (Progressive Remix)","recording":{"id":"rec-tagged-1","title":"I'm Raving (Progressive Remix)"}}]}]})JSON");
    music_art_http->add("coverartarchive.org/release/rel-tagged-1", 200, "application/json",
        R"JSON({"images":[{"front":true,"image":"https://provider.example/cover.jpg","thumbnails":{"500":"https://provider.example/cover.jpg"}}]})JSON");
    const Bytes provider_cover{0xff, 0xd8, 0xff, 0xe0, 0x01, 0x02, 0x03, 0x04};
    music_art_http->add_bytes("provider.example/cover.jpg", 200, "image/jpeg", provider_cover);
    CatalogueScannerConfig music_art_config;
    music_art_config.enabled = true;
    music_art_config.movies.enabled = false;
    music_art_config.tv.enabled = false;
    music_art_config.music.enabled = true;
    music_art_config.music.roots = {tagged_album};
    music_art_config.music.musicbrainz.enabled = true;
    music_art_config.music.musicbrainz.contact = "https://example.test/macha";
    music_art_config.music.discogs.enabled = false;
    music_art_config.max_provider_requests_per_scan = 8;
    CatalogueScanner music_art_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                       music_art_config, std::move(music_art_http));
    CHECK(music_art_scanner.scan_once() == 1);
    auto music_album = service.catalogue().get("musicbrainz:album:rg-tagged-1");
    REQUIRE(music_album.has_value());
    REQUIRE(music_album->artwork.size() == 2);
    CHECK(std::count_if(music_album->artwork.begin(), music_album->artwork.end(),
                        [](const auto& art) { return art.role == "cover"; }) == 2);
    const auto embedded_id = object_id(embedded_cover);
    const auto provider_id = object_id(provider_cover);
    CHECK(std::any_of(music_album->artwork.begin(), music_album->artwork.end(),
                      [&](const auto& art) { return art.id == embedded_id; }));
    CHECK(std::any_of(music_album->artwork.begin(), music_album->artwork.end(),
                      [&](const auto& art) { return art.id == provider_id; }));
    CHECK(service.node().local_store().has(embedded_id));
    CHECK(service.node().local_store().has(provider_id));

    const std::string loose_path = singles + "/Scooter - The First Time (Raven Remix).mp3";
    write_fixture(loose_path, untagged_bytes);
    auto loose_entry = service.filesystem().getattr(loose_path);
    auto loose_probe = music_source.probe(service.filesystem(), "/Music", loose_path, loose_entry);
    REQUIRE(loose_probe.has_value());
    CHECK(loose_probe->artist == "Scooter");
    CHECK(loose_probe->album.empty());
    CHECK(loose_probe->title == "The First Time (Raven Remix)");

    const std::string nested_album = singles + "/13 - [1996] I'm Raving CDM";
    service.filesystem().mkdir(nested_album, 0755, getuid(), getgid());
    const std::string nested_path = nested_album + "/01 - I'm Raving.mp3";
    write_fixture(nested_path, untagged_bytes);
    auto nested_entry = service.filesystem().getattr(nested_path);
    auto nested_probe = music_source.probe(service.filesystem(), "/Music", nested_path, nested_entry);
    REQUIRE(nested_probe.has_value());
    CHECK(nested_probe->artist.empty());
    CHECK(nested_probe->album.empty());
    CHECK(nested_probe->track == 1);
    CHECK(nested_probe->title == "I'm Raving");

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
    // A missing configured root makes the pass partial: discoveries from
    // available roots are still ingested, but absence cannot prune existing
    // scanner-owned bindings until every root is traversable.
    scanner_config.movies.roots = {"/Movies", "/Missing"};
    scanner_config.movies.tmdb.token_file = scanner_token;
    scanner_config.tv.enabled = false;
    scanner_config.music.enabled = false;
    CatalogueScanner scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                             scanner_config, std::move(fake_http));
    const auto namespace_before_scan = service.filesystem().namespace_signature();
    CHECK(scanner.scan_once() == 1);
    CHECK(service.filesystem().namespace_signature() == namespace_before_scan);
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

    // Manual metadata editing is authoritative. A later scanner discovery for
    // another local copy may add media bindings, but must not silently overwrite
    // the user's descriptive changes.
    auto manual = *service.catalogue().get("tmdb:movie:335984");
    manual.title = "Blade Runner Custom";
    manual.sort_title = manual.title;
    manual.synopsis = "A manually edited synopsis.";
    manual.external_ids["macha_metadata_locked"] = "1";
    auto manually_saved = service.catalogue().upsert(std::move(manual), revision);
    revision = manually_saved.revision;

    // A second file resolving to the same title adds another binding without
    // losing the already-bound media identity.
    const std::string alternate = "/Movies/Blade.Runner.2049.2017.Remux.mkv";
    service.filesystem().create_file(alternate, 0644, getuid(), getgid());
    auto alternate_bytes = pattern(32769);
    auto alternate_writer = service.filesystem().open_write(alternate, true);
    REQUIRE(alternate_writer->write(0, alternate_bytes) == alternate_bytes.size());
    alternate_writer->commit();
    CHECK(service.filesystem().namespace_signature() != namespace_before_scan);
    auto alternate_id = file_media_id(service.filesystem().getattr(alternate));
    CHECK(alternate_id != media_id);
    CHECK(scanner.scan_once() == 1);
    auto twice = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(twice.has_value());
    CHECK(twice->title == "Blade Runner Custom");
    CHECK(twice->synopsis == "A manually edited synopsis.");
    CHECK(twice->year == std::optional<int32_t>{2017});
    CHECK(twice->artwork.size() == 2);
    CHECK(twice->external_ids.at("tmdb") == "335984");
    CHECK(twice->external_ids.at("macha_metadata_locked") == "1");
    CHECK(std::find(twice->media_ids.begin(), twice->media_ids.end(), media_id) != twice->media_ids.end());
    CHECK(std::find(twice->media_ids.begin(), twice->media_ids.end(), alternate_id) != twice->media_ids.end());

    // Clear Metadata removes the catalogue entity rather than saving an empty
    // matched item. Both underlying files therefore become unbound and the next
    // scanner pass performs provider matching again from scratch.
    CHECK(service.catalogue().clear_metadata("tmdb:movie:335984", twice->revision) == 1);
    CHECK(!service.catalogue().get("tmdb:movie:335984").has_value());
    CHECK(scanner.scan_once() == 2);
    auto rematched = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(rematched.has_value());
    CHECK(rematched->title == "Blade Runner 2049");
    CHECK(rematched->synopsis == "A blade runner uncovers a long-buried secret.");
    CHECK(rematched->external_ids.at("tmdb") == "335984");
    CHECK(!rematched->external_ids.contains("macha_metadata_locked"));
    CHECK(rematched->artwork.size() == 2);
    CHECK(std::find(rematched->media_ids.begin(), rematched->media_ids.end(), media_id) != rematched->media_ids.end());
    CHECK(std::find(rematched->media_ids.begin(), rematched->media_ids.end(), alternate_id) != rematched->media_ids.end());

    // Deletion reconciles duplicate bindings one at a time and removes the
    // scanner-owned item only after the final copy goes.
    service.filesystem().unlink("/Movies/Blade.Runner.2049.2017.1080p.mkv");
    std::this_thread::sleep_for(config.metadata_cache + 50ms);
    CHECK(scanner.scan_once() == 0);
    auto partial = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(partial.has_value());
    CHECK(std::find(partial->media_ids.begin(), partial->media_ids.end(), media_id) != partial->media_ids.end());
    CHECK(std::find(partial->media_ids.begin(), partial->media_ids.end(), alternate_id) != partial->media_ids.end());

    // Once the previously unavailable root exists, the scan is complete and
    // destructive reconciliation may safely remove the vanished first binding.
    service.filesystem().mkdir("/Missing", 0755, getuid(), getgid());
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

    // Shutdown must not wait for a complete catalogue scan. request_stop()
    // propagates into the HTTP client so an in-flight provider request is
    // interrupted, and scan_once(stop) abandons the partial pass without a
    // reconciliation commit.
    const std::string shutdown_path = "/Movies/Shutdown.Test.2020.mkv";
    service.filesystem().create_file(shutdown_path, 0644, getuid(), getgid());
    auto shutdown_writer = service.filesystem().open_write(shutdown_path, true);
    auto shutdown_bytes = pattern(32769);
    REQUIRE(shutdown_writer->write(0, shutdown_bytes) == shutdown_bytes.size());
    shutdown_writer->commit();

    auto blocking_http = std::make_unique<BlockingHttpClient>();
    auto* blocking_http_ptr = blocking_http.get();
    auto cancel_config = scanner_config;
    cancel_config.movies.roots = {"/Movies"};
    CatalogueScanner cancel_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                    cancel_config, std::move(blocking_http));
    cancel_scanner.start();
    REQUIRE(wait_until([&] { return blocking_http_ptr->entered(); }, 1s));
    const auto stop_started = Clock::now();
    cancel_scanner.stop();
    CHECK(blocking_http_ptr->stopped());
    CHECK(Clock::now() - stop_started < 1s);

    // Online metadata enrichment is bounded by actual provider HTTP requests,
    // not by the number of files. Completed discoveries commit normally and a
    // later pass resumes with already-bound media skipped.
    service.filesystem().mkdir("/Budget", 0755, getuid(), getgid());
    const std::array<std::pair<const char*, uint8_t>, 3> budget_files{{
        {"/Budget/Budget.One.2020.mkv", 1},
        {"/Budget/Budget.Two.2021.mkv", 2},
        {"/Budget/Budget.Three.2022.mkv", 3},
    }};
    std::set<std::string> budget_media_ids;
    for (const auto& [path, marker] : budget_files) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto w = service.filesystem().open_write(path, true);
        REQUIRE(w->write(0, Bytes{marker, 2, 3, 4}) == 4);
        w->commit();
        budget_media_ids.insert(file_media_id(service.filesystem().getattr(path)));
    }
    REQUIRE(budget_media_ids.size() == budget_files.size());
    auto budget_http = std::make_unique<FakeHttpClient>();
    auto* budget_http_ptr = budget_http.get();
    budget_http->add("query=Budget%20One", 200, "application/json",
                     R"({"results":[{"id":2001,"title":"Budget One","release_date":"2020-01-01"}]})");
    budget_http->add("/movie/2001", 200, "application/json",
                     R"({"id":2001,"title":"Budget One","release_date":"2020-01-01"})");
    budget_http->add("query=Budget%20Two", 200, "application/json",
                     R"({"results":[{"id":2002,"title":"Budget Two","release_date":"2021-01-01"}]})");
    budget_http->add("/movie/2002", 200, "application/json",
                     R"({"id":2002,"title":"Budget Two","release_date":"2021-01-01"})");
    budget_http->add("query=Budget%20Three", 200, "application/json",
                     R"({"results":[{"id":2003,"title":"Budget Three","release_date":"2022-01-01"}]})");
    budget_http->add("/movie/2003", 200, "application/json",
                     R"({"id":2003,"title":"Budget Three","release_date":"2022-01-01"})");

    auto budget_config = scanner_config;
    budget_config.movies.roots = {"/Budget"};
    budget_config.max_provider_requests_per_scan = 4;
    budget_config.provider_batch_delay = 1000ms;
    CatalogueScanner budget_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                    budget_config, std::move(budget_http));
    CHECK(budget_scanner.scan_once() == 2);
    CHECK(budget_http_ptr->requests() == 4);
    size_t first_batch_items = 0;
    for (const auto* id : {"tmdb:movie:2001", "tmdb:movie:2002", "tmdb:movie:2003"})
        if (service.catalogue().get(id).has_value()) ++first_batch_items;
    CHECK(first_batch_items == 2);
    CHECK(budget_scanner.scan_once() == 1);
    CHECK(budget_http_ptr->requests() == 6);
    CHECK(service.catalogue().get("tmdb:movie:2001").has_value());
    CHECK(service.catalogue().get("tmdb:movie:2002").has_value());
    CHECK(service.catalogue().get("tmdb:movie:2003").has_value());

    // Provider request budgeting is fair across media domains. A long run of
    // movie misses must not consume the whole batch before TV and Music get a
    // lookup opportunity.
    service.filesystem().mkdir("/FairMovies", 0755, getuid(), getgid());
    for (int i = 1; i <= 4; ++i) {
        const auto path = "/FairMovies/Fair.Movie." + std::to_string(i) + ".mkv";
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto w = service.filesystem().open_write(path, true);
        REQUIRE(w->write(0, Bytes{static_cast<uint8_t>(i), 2, 3, 4}) == 4);
        w->commit();
    }
    service.filesystem().mkdir("/FairTV", 0755, getuid(), getgid());
    const std::string fair_tv = "/FairTV/Fair.Show.S01E01.mkv";
    service.filesystem().create_file(fair_tv, 0644, getuid(), getgid());
    auto fair_tv_writer = service.filesystem().open_write(fair_tv, true);
    REQUIRE(fair_tv_writer->write(0, Bytes{9, 8, 7, 6}) == 4);
    fair_tv_writer->commit();
    service.filesystem().mkdir("/FairMusic", 0755, getuid(), getgid());
    service.filesystem().mkdir("/FairMusic/Fair Artist", 0755, getuid(), getgid());
    service.filesystem().mkdir("/FairMusic/Fair Artist/Fair Album", 0755, getuid(), getgid());
    const std::string fair_music = "/FairMusic/Fair Artist/Fair Album/01 - Fair Track.mp3";
    write_fixture(fair_music, untagged_bytes);

    auto fair_http = std::make_unique<FakeHttpClient>();
    auto* fair_http_ptr = fair_http.get();
    fair_http->add("/search/movie", 200, "application/json", R"({"results":[]})");
    fair_http->add("/search/tv", 200, "application/json", R"({"results":[]})");
    fair_http->add("/ws/2/release?", 200, "application/json", R"({"releases":[]})");
    auto fair_config = scanner_config;
    fair_config.movies.roots = {"/FairMovies"};
    fair_config.tv.enabled = true;
    fair_config.tv.roots = {"/FairTV"};
    fair_config.tv.tmdb.token_file = scanner_token;
    fair_config.music.enabled = true;
    fair_config.music.roots = {"/FairMusic"};
    fair_config.music.musicbrainz.enabled = true;
    fair_config.max_provider_requests_per_scan = 3;
    CatalogueScanner fair_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                  fair_config, std::move(fair_http));
    CHECK(fair_scanner.scan_once() == 0);
    CHECK(fair_http_ptr->requests() == 3);
    CHECK(fair_http_ptr->requests_containing("/search/movie") == 1);
    CHECK(fair_http_ptr->requests_containing("/search/tv") == 1);
    CHECK(fair_http_ptr->requests_containing("/ws/2/release?") == 1);

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


void test_catalogue_hint_queue_persistence_coalescing_and_priority() {
    TempDir temp;
    const auto state = temp.path() / "hint-state";

    std::string no_match_id;
    {
        CatalogueHintQueue hints(state);
        no_match_id = hints.submit("/Movies/Unknown.mkv", "scanner", "macha:rev-a",
                                   CatalogueHintPriority::periodic_scan);
        CHECK(hints.submit("//Movies//Unknown.mkv", "scanner", "macha:rev-a",
                           CatalogueHintPriority::periodic_scan) == no_match_id);
        REQUIRE(hints.list().size() == 1);
        auto claimed = hints.claim_next();
        REQUIRE(claimed.has_value());
        CHECK(claimed->id == no_match_id);
        auto processing_summary = hints.summary();
        CHECK(processing_summary.total == 1);
        CHECK(processing_summary.pending == 1);
        CHECK(processing_summary.queued == 0);
        CHECK(processing_summary.processing == 1);
        CHECK(processing_summary.deferred == 0);
        hints.mark_no_match(no_match_id, "movies", "macha:rev-a", "no provider match");
        auto terminal_summary = hints.summary();
        CHECK(terminal_summary.pending == 0);
        CHECK(terminal_summary.no_match == 1);

        // An unchanged namespace observation must reuse the terminal negative
        // result rather than reopening provider work merely because its source
        // or scan pass is different.
        CHECK(hints.submit("/Movies/Unknown.mkv", "namespace", "macha:rev-a",
                           CatalogueHintPriority::namespace_mutation) == no_match_id);
        auto unchanged = hints.get(no_match_id);
        REQUIRE(unchanged.has_value());
        CHECK(unchanged->state == CatalogueHintState::no_match);

        // Replacing bytes at the same path changes the stable media id and
        // therefore reopens the coalesced work item at the stronger priority.
        hints.submit("/Movies/Unknown.mkv", "namespace", "macha:rev-b",
                     CatalogueHintPriority::namespace_mutation);
        auto changed = hints.get(no_match_id);
        REQUIRE(changed.has_value());
        CHECK(changed->state == CatalogueHintState::queued);
        CHECK(changed->priority == CatalogueHintPriority::namespace_mutation);
        auto replacement = hints.claim_next();
        REQUIRE(replacement.has_value());
        hints.mark_no_match(replacement->id, "movies", "macha:rev-b", "still unmatched");

        // Manual work always reopens terminal state; an ingest occurrence then
        // raises the same canonical-path item to the highest current priority.
        hints.submit("/Movies/Unknown.mkv", "manual", "manual:1",
                     CatalogueHintPriority::manual_rescan);
        hints.submit("/Movies/Unknown.mkv", "ingest", "job-1",
                     CatalogueHintPriority::ingest);
        auto coalesced = hints.get(no_match_id);
        REQUIRE(coalesced.has_value());
        CHECK(coalesced->state == CatalogueHintState::queued);
        CHECK(coalesced->priority == CatalogueHintPriority::ingest);
        CHECK(hints.summary("ingest", "job-1").pending == 1);
        CHECK(hints.erase_origin("ingest", "job-1") == 1);
        auto lowered = hints.get(no_match_id);
        REQUIRE(lowered.has_value());
        CHECK(lowered->priority == CatalogueHintPriority::manual_rescan);
        hints.submit("/Movies/Unknown.mkv", "ingest", "job-1", CatalogueHintPriority::ingest);
        auto in_flight = hints.claim_next();
        REQUIRE(in_flight.has_value());
        CHECK(in_flight->id == no_match_id);
    }

    // Claim ownership is deliberately not persisted: if the daemon exits while
    // a hint is in flight, the durable queued/deferred state provides at-least-once
    // replay without a full hints.json rewrite merely to record `processing`.
    {
        CatalogueHintQueue hints(state);
        auto recovered = hints.get(no_match_id);
        REQUIRE(recovered.has_value());
        CHECK(recovered->state == CatalogueHintState::queued);
    }

    // Candidate fallback progress is queue state, not provider-process state.
    // Persist it so a restart cannot repeatedly retry the first hypothesis and
    // defeat fair scheduling.
    const auto cursor_state = temp.path() / "cursor-state";
    std::string cursor_id;
    {
        CatalogueHintQueue cursor(cursor_state);
        cursor_id = cursor.submit("/Music/Artist/Album/01 - Track.mp3", "scanner",
                                  "macha:cursor", CatalogueHintPriority::periodic_scan);
        auto claimed = cursor.claim_next();
        REQUIRE(claimed.has_value());
        cursor.advance_candidate(cursor_id, 2);
        auto advanced = cursor.get(cursor_id);
        REQUIRE(advanced.has_value());
        CHECK(advanced->candidate_cursor == 2);
    }
    {
        CatalogueHintQueue cursor(cursor_state);
        auto recovered = cursor.get(cursor_id);
        REQUIRE(recovered.has_value());
        CHECK(recovered->state == CatalogueHintState::queued);
        CHECK(recovered->candidate_cursor == 2);
    }

    // Repeated per-item failures become a persisted terminal dead letter rather
    // than remaining runnable forever. The failure counter is distinct from
    // scheduling attempts and survives restart for API/operator inspection.
    const auto failure_state = temp.path() / "failure-state";
    std::string failure_id;
    {
        CatalogueHintQueue failure_queue(failure_state);
        failure_id = failure_queue.submit("/Movies/Broken.mkv", "scanner", "macha:broken",
                                          CatalogueHintPriority::periodic_scan);
        REQUIRE(failure_queue.claim_next().has_value());
        CHECK(!failure_queue.record_failure(failure_id, "first failure", 0, 2));
        auto once = failure_queue.get(failure_id);
        REQUIRE(once.has_value());
        CHECK(once->state == CatalogueHintState::deferred);
        CHECK(once->failures == 1);
        REQUIRE(failure_queue.claim_next().has_value());
        CHECK(failure_queue.record_failure(failure_id, "second failure", 0, 2));
        auto dead = failure_queue.get(failure_id);
        REQUIRE(dead.has_value());
        CHECK(dead->state == CatalogueHintState::failed);
        CHECK(dead->failures == 2);
        CHECK(dead->error == "second failure");
        CHECK(!failure_queue.claim_next().has_value());
    }
    {
        CatalogueHintQueue failure_queue(failure_state);
        auto dead = failure_queue.get(failure_id);
        REQUIRE(dead.has_value());
        CHECK(dead->state == CatalogueHintState::failed);
        CHECK(dead->failures == 2);
        CHECK(!failure_queue.claim_next().has_value());
    }

    // Equal-priority work is fair across top-level catalogue roots rather than
    // allowing a large Movies backlog to starve TV or Music indefinitely.
    const auto fairness_state = temp.path() / "fairness-state";
    CatalogueHintQueue fair(fairness_state);
    fair.submit("/Movies/A.mkv", "scanner", "macha:a", 10);
    fair.submit("/Movies/B.mkv", "scanner", "macha:b", 10);
    fair.submit("/TV/Show/S01E01.mkv", "scanner", "macha:c", 10);
    auto first = fair.claim_next();
    auto second = fair.claim_next();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->path.starts_with("/Movies/"));
    CHECK(second->path.starts_with("/TV/"));

    // An idle consumer blocks on the queue revision instead of polling the
    // complete persisted hint map. A new submission wakes it immediately.
    const auto wake_state = temp.path() / "wake-state";
    CatalogueHintQueue wake(wake_state);
    const auto revision = wake.revision();
    CHECK(!wake.wait_for_change({}, revision, 5ms));
    std::jthread producer([&] {
        std::this_thread::sleep_for(20ms);
        wake.submit("/Movies/Wake.mkv", "ingest", "wake-job",
                    CatalogueHintPriority::ingest);
    });
    CHECK(wake.wait_for_change({}, revision, 500ms));
    auto ready_delay = wake.next_ready_delay();
    REQUIRE(ready_delay.has_value());
    CHECK(*ready_delay == 0ms);
}

void test_ingest_catalogue_feedback_and_external_clear_cleanup() {
    TempDir temp;
    auto keyfile = temp.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto config = config_for(temp.path() / "node", keyfile, free_port());
    config.replication = 1;
    config.metadata_replication = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.scanner.movies.enabled = true;
    config.catalogue.scanner.movies.roots = {"/Movies"};
    config.catalogue.scanner.tv.enabled = false;
    config.catalogue.scanner.music.enabled = false;
    config.ingest.enabled = false; // use the explicit manager below

    Service service(config, keys);
    service.start();

    const auto source_root = temp.path() / "external-import";
    std::filesystem::create_directories(source_root);
    const auto media = source_root / "Queue Test Movie 2024.mkv";
    const auto unrelated = source_root / "do-not-delete.txt";
    {
        std::ofstream out(media, std::ios::binary);
        auto bytes = pattern(512 * 1024);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::ofstream out(unrelated);
        out << "external source material not selected for ingest\n";
    }

    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = temp.path() / "staging";
    ingest_config.source_roots = {source_root};
    ingest_config.copy_chunk_bytes = 64 * 1024;
    ingest_config.checkpoint_bytes = 256 * 1024;
    ingest_config.delete_external_source_on_clear = true;
    IngestManager ingest(service.node(), service.filesystem(), service.catalogue_hints(),
                         ingest_config);
    ingest.start();
    const auto job_id = ingest.submit_path(source_root);

    REQUIRE(wait_until([&] {
        auto job = ingest.job(job_id);
        return job && job->state == IngestJobState::cataloguing;
    }, 10s));
    auto summary = service.catalogue_hints().summary("ingest", job_id);
    REQUIRE(summary.total == 1);
    REQUIRE(summary.pending == 1);
    auto hint = service.catalogue_hints().claim_next();
    REQUIRE(hint.has_value());
    CHECK(hint->origins.size() == 1);
    service.catalogue_hints().mark_catalogued(
        hint->id, "movies", "macha:test-ingest-media", {"test:movie:queue"}, "synthetic match");

    REQUIRE(wait_until([&] {
        auto job = ingest.job(job_id);
        return job && job->state == IngestJobState::completed;
    }, 5s));
    auto completed = ingest.job(job_id);
    REQUIRE(completed.has_value());
    CHECK(completed->catalogue_total == 1);
    CHECK(completed->catalogue_pending == 0);
    CHECK(completed->catalogue_catalogued == 1);
    CHECK(completed->catalogue_no_match == 0);
    CHECK(completed->catalogue_failed == 0);

    REQUIRE(ingest.clear(job_id));
    CHECK(!ingest.job(job_id).has_value());
    CHECK(!std::filesystem::exists(media));
    CHECK(std::filesystem::exists(unrelated));
    CHECK(std::filesystem::exists(source_root));
    CHECK(service.catalogue_hints().summary("ingest", job_id).total == 0);

    ingest.stop();
    service.stop();
}

void test_catalogue_warm_read_defers_remote_refresh() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c1 = config_for(t.path() / "catalogue-live-1", keyfile, free_port());
    auto c2 = config_for(t.path() / "catalogue-live-2", keyfile, free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;
    c1.metadata_cache = c2.metadata_cache = 100ms;

    NodeRuntime n1(c1, keys);
    DistributedStore store1(n1);
    MetadataManager metadata1(n1);
    CatalogueManager catalogue1(n1, store1, metadata1);

    NodeRuntime n2(c2, keys);
    DistributedStore store2(n2);
    MetadataManager metadata2(n2);
    CatalogueManager catalogue2(n2, store2, metadata2);

    // Form the initial namespace on the bootstrap-less founder before starting
    // the joiner. A configured joiner is intentionally forbidden from inventing
    // genesis while its bootstrap peer has not yet entered active membership.
    n1.start();

    CatalogueItem first;
    first.id = "test:movie:remote-first";
    first.kind = CatalogueKind::movie;
    first.title = "Remote First";
    first = catalogue1.upsert(first);

    n2.start();

    // Cold-load node two from node one's committed catalogue. There is no Service
    // here, so no catalogue maintenance thread can refresh it behind the test.
    REQUIRE(wait_until([&] {
        try {
            auto item = catalogue2.get(first.id);
            return item && item->title == first.title;
        } catch (...) {
            return false;
        }
    }, 5s));
    const auto before = catalogue2.status();
    REQUIRE(before.ready);

    CatalogueItem second;
    second.id = "test:movie:remote-second";
    second.kind = CatalogueKind::movie;
    second.title = "Remote Second";
    second = catalogue1.upsert(second);
    const auto writer_status = catalogue1.status();

    // Membership/metadata propagation tells node two that a newer generation
    // exists. The catalogue itself is deliberately still the old cached root.
    REQUIRE(wait_until([&] {
        return n2.known_metadata_generation() >= writer_status.metadata_generation;
    }, 5s));
    const auto stale = catalogue2.status();
    CHECK(stale.metadata_generation == before.metadata_generation);
    CHECK(stale.known_metadata_generation >= writer_status.metadata_generation);
    CHECK(stale.known_metadata_generation > stale.metadata_generation);

    // A remote generation notice must not turn ordinary kernel metadata traffic
    // into quorum reads. FUSE may adopt a newer snapshot only after some control-
    // plane owner has already decoded it locally. Repeated getattr therefore
    // leaves MetadataManager's available generation unchanged.
    FileSystem fs2(n2, store2, metadata2);
    FuseConfig fuse_config;
    fuse_config.commit_workers = 1;
    auto frontend = std::make_shared<FuseFrontend>(fs2, fuse_config);
    const auto available_before_fuse = metadata2.available_snapshot_view();
    REQUIRE(available_before_fuse.has_value());
    const auto namespace_revision_before = metadata2.available_namespace_revision();
    for (int i = 0; i < 64; ++i) CHECK(frontend->getattr("/").type == EntryType::directory);
    const auto available_after_fuse = metadata2.available_snapshot_view();
    REQUIRE(available_after_fuse.has_value());
    CHECK(available_after_fuse->generation == available_before_fuse->generation);

    // Warm reads must remain memory-only even when a newer generation is known.
    // The serving API may briefly return the previous coherent snapshot while its
    // background/control-plane worker converges; it must not perform quorum I/O
    // on the request thread. refresh_needed() is the hand-off to that worker.
    CHECK(catalogue2.refresh_needed());
    auto still_cached = catalogue2.get(first.id);
    REQUIRE(still_cached.has_value());
    CHECK(still_cached->title == first.title);
    CHECK(!catalogue2.get(second.id).has_value());
    const auto after_read = catalogue2.status();
    CHECK(after_read.metadata_generation == stale.metadata_generation);
    CHECK(after_read.known_metadata_generation >= writer_status.metadata_generation);

    // Simulate the Service control-plane pass. It must converge the immutable root
    // and atomically publish the replacement snapshot for subsequent API reads.
    catalogue2.repair_once();
    auto refreshed = catalogue2.get(second.id);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->title == second.title);
    const auto after = catalogue2.status();
    CHECK(after.metadata_generation >= writer_status.metadata_generation);
    CHECK(after.metadata_generation == after.known_metadata_generation);
    CHECK(!catalogue2.refresh_needed());
    CHECK(frontend->getattr("/").type == EntryType::directory);
    auto available_after_repair = metadata2.available_snapshot_view();
    REQUIRE(available_after_repair.has_value());
    CHECK(available_after_repair->generation >= writer_status.metadata_generation);
    // This metadata change only moved the catalogue root, so it must not force
    // FUSE to rebuild its namespace graph.
    CHECK(metadata2.available_namespace_revision() == namespace_revision_before);
    frontend->stop();

    CatalogueHintQueue catalogue2_hints(t.path() / "catalogue2-hints");
    CatalogueApi api(catalogue2, catalogue2_hints);
    auto status_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/status",
                                       .query = {},
                                       .headers = {},
                                       .body = {}});
    REQUIRE(status_response.status == 200);
    auto status_json = Json::parse(std::string(status_response.body.begin(),
                                               status_response.body.end()));
    REQUIRE(status_json.find("metadata_generation") != nullptr);
    REQUIRE(status_json.find("known_metadata_generation") != nullptr);
    CHECK(status_json.find("metadata_generation")->asInt64() ==
          status_json.find("known_metadata_generation")->asInt64());

    n2.stop();
    n1.stop();
}

void test_metadata_decoded_cache_ttl_recovers_missed_notice() {
    TempDir t;
    auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c1 = config_for(t.path() / "metadata-ttl-1", keyfile, free_port());
    auto c2 = config_for(t.path() / "metadata-ttl-2", keyfile, free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 1;
    c1.metadata_cache = c2.metadata_cache = 100ms;
    // Keep ordinary heartbeat propagation outside this test window. We install a
    // valid newer voter record directly to simulate a generation notice that was
    // missed by node two; TTL validation must still discover it from quorum.
    c1.heartbeat = c2.heartbeat = 5s;
    c1.dead_after = c2.dead_after = 20s;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    MetadataManager metadata1(n1);
    MetadataManager metadata2(n2);

    // Establish genesis on the founder first. The joiner may legitimately reject
    // metadata reads with "waiting for bootstrap peer" during the brief interval
    // between start() and membership convergence, so retry its initial read rather
    // than turning that expected bootstrap state into an unhandled test failure.
    n1.start();
    const auto initial1 = metadata1.snapshot_view();
    n2.start();

    std::optional<MetadataSnapshotView> initial2;
    REQUIRE(wait_until([&] {
        try {
            auto view = metadata2.snapshot_view();
            if (view.generation != initial1.generation)
                return false;
            initial2 = std::move(view);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }, 5s));
    REQUIRE(initial2.has_value());

    auto base = n1.metadata_replica().current();
    auto changed = decode_snapshot(base.payload);
    auto root = changed.entries.find("/");
    REQUIRE(root != changed.entries.end());
    ++root->second.version;

    MetadataRecord next;
    next.generation = base.generation + 1;
    next.previous = base.hash;
    next.payload = encode_snapshot(changed);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);
    REQUIRE(n1.metadata_replica().seed(next));

    // With no generation notice, the decoded view is legitimately reused until
    // the configured metadata TTL expires.
    CHECK(n2.known_metadata_generation() < next.generation);
    CHECK(metadata2.snapshot_view().generation == initial2->generation);
    std::this_thread::sleep_for(c2.metadata_cache + 50ms);
    REQUIRE(n2.known_metadata_generation() < next.generation);

    // Expiry must force a real metadata read, discover the newer voter record and
    // replace the decoded snapshot. Before 0.10.4 cached_snapshot_view() ignored
    // cache_until_ and this remained stale indefinitely without a notice.
    auto refreshed = metadata2.snapshot_view();
    CHECK(refreshed.generation == next.generation);
    CHECK(n2.known_metadata_generation() >= next.generation);

    n2.stop();
    n1.stop();
}

void test_catalogue_root_ready_without_local_artwork() {
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

    CatalogueSnapshot snapshot;
    CatalogueItem item;
    item.id = "test:movie:artwork-missing";
    item.kind = CatalogueKind::movie;
    item.title = "Catalogue Still Loads";
    item.revision = 1;
    item.updated_ns = wall_time_ns();
    CatalogueArtwork artwork;
    artwork.role = "poster";
    artwork.mime_type = "image/jpeg";
    artwork.id = object_id(Bytes{0x01, 0x02, 0x03, 0x04});
    item.artwork.push_back(artwork);
    snapshot.items.emplace(item.id, item);

    auto encoded = encode_catalogue(snapshot);
    auto root = object_id(encoded);
    REQUIRE(node.local_store().put(root, encoded));
    REQUIRE(!node.local_store().has(artwork.id));

    metadata.mutate([&](MetadataSnapshot& state) { state.catalogue_root = root; });
    catalogue.repair_once();

    auto status = catalogue.status();
    CHECK(status.ready);
    CHECK(status.items == 1);
    CHECK(status.artwork_objects == 1);
    CHECK(status.local_artwork_objects == 0);
    auto loaded = catalogue.get(item.id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->title == item.title);

    node.stop();
}

void test_macos_unicode_namespace_aliases() {
#if defined(__APPLE__)
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

    filesystem.mkdir("/Music", 0755, getuid(), getgid());

    // Simulate namespace keys written by a previous version/client in D form.
    // The runtime alias index must resolve NFC callbacks to the exact persisted
    // spelling instead of rewriting the metadata representation.
    const std::string nfd_dir = "/Music/Cafe\xcc\x81 del Mar";
    const std::string nfd_file =
        nfd_dir + "/01.Clannad - Na Buachailli\xcc\x81 lainn.mp3";
    const std::string nfc_dir = "/Music/Caf\xc3\xa9 del Mar";
    const std::string nfc_file =
        nfc_dir + "/01.Clannad - Na Buachaill\xc3\xad lainn.mp3";

    metadata.mutate([&](MetadataSnapshot& snapshot) {
        FsEntry dir;
        dir.type = EntryType::directory;
        dir.mode = 0755;
        dir.uid = getuid();
        dir.gid = getgid();
        dir.ctime_ns = dir.mtime_ns = wall_time_ns();
        snapshot.entries[nfd_dir] = dir;

        FsEntry file;
        file.type = EntryType::file;
        file.mode = 0644;
        file.uid = getuid();
        file.gid = getgid();
        file.ctime_ns = file.mtime_ns = wall_time_ns();
        snapshot.entries[nfd_file] = file;
    });

    CHECK(filesystem.getattr(nfc_dir).type == EntryType::directory);
    CHECK(filesystem.getattr(nfc_file).type == EntryType::file);
    auto listed = filesystem.readdir(nfc_dir);
    REQUIRE(listed.size() == 1);
    CHECK(listed.front().first == "01.Clannad - Na Buachailli\xcc\x81 lainn.mp3");

    // A new NFC leaf under an old NFD parent must retain the exact stored parent
    // spelling so require_parent() sees a real namespace key.
    const std::string new_nfc = nfc_dir + "/Macha Caf\xc3\xa9 Test.mp3";
    filesystem.create_file(new_nfc, 0644, getuid(), getgid());
    const auto persisted = metadata.snapshot();
    CHECK(persisted.entries.contains(nfd_dir + "/Macha Caf\xc3\xa9 Test.mp3"));
    CHECK(filesystem.getattr(new_nfc).type == EntryType::file);

    node.stop();
#endif
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

    // Replacing the poster retires the old object in committed metadata. With
    // a zero grace period in this test, reachability GC must prune that retirement
    // and delete the unreachable physical object on every connected node while
    // preserving the replacement.
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

    CatalogueApi api(s3.catalogue(), s3.catalogue_hints());
    auto status_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/status",
                                       .query = {},
                                       .headers = {},
                                       .body = {}});
    CHECK(status_response.status == 200);
    std::string status_body(status_response.body.begin(), status_response.body.end());
    CHECK(status_body.find("\"ready\":true") != std::string::npos);
    auto status_json = Json::parse(status_body);
    REQUIRE(status_json.find("server_version") != nullptr);
    CHECK(status_json.find("server_version")->asString() == kServerVersion);
    auto search_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/search",
                                       .query = {{"q", "pilot"}},
                                       .headers = {},
                                       .body = {}});
    CHECK(search_response.status == 200);
    std::string search_body(search_response.body.begin(), search_response.body.end());
    CHECK(search_body.find("episode:test:1:1") != std::string::npos);

    // Clear Metadata is an atomic catalogue reset. Clearing a hierarchy parent
    // also removes descendants so leaf media bindings cannot keep the old match
    // alive and block a fresh scanner/provider lookup.
    auto clear_response = api.handle({.method = "DELETE",
                                      .path = "/api/v1/catalogue/items/show%3Atest/metadata",
                                      .query = {},
                                      .headers = {{"if-match", "\"rev-" + std::to_string(show.revision + 1) + "\""}},
                                      .body = {}});
    // The poster replacement did not mutate the copy of `show`; use the current
    // revision if the optimistic request raced a catalogue refresh.
    if (clear_response.status == 409) {
        auto current_show = s3.catalogue().get(show.id);
        REQUIRE(current_show.has_value());
        clear_response = api.handle({.method = "DELETE",
                                     .path = "/api/v1/catalogue/items/show%3Atest/metadata",
                                     .query = {},
                                     .headers = {{"if-match", "\"rev-" + std::to_string(current_show->revision) + "\""}},
                                     .body = {}});
    }
    CHECK(clear_response.status == 204);
    CHECK(!s3.catalogue().get(show.id).has_value());
    CHECK(!s3.catalogue().get(episode.id).has_value());

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

        // Removing a committed file records an explicit retirement. Reachability
        // GC uses the same live inventory for both tombstone pruning and orphan sweep.
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
        CHECK(std::find_if(maintenance.garbage.begin(), maintenance.garbage.end(),
                           [&](const GarbageRef& garbage) { return garbage.id == deleted_id; }) !=
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

void test_attached_picture_audio_direct_play() {
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
    service.filesystem().create_file("/media/cover.mp3", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/cover.mp3", true);
    auto bytes = pattern(4096);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/cover.mp3"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::make_unique<AttachedPictureAudioEngine>());
    playback.start();

    Json::Array containers{Json("mp3")};
    Json::Array video_codecs{Json("h264")};
    Json::Array audio_codecs{Json("mp3"), Json("aac")};
    Json::Object capabilities{{"containers", Json(std::move(containers))},
                              {"video_codecs", Json(std::move(video_codecs))},
                              {"audio_codecs", Json(std::move(audio_codecs))},
                              {"hls_fmp4", true}};
    Json::Object root{{"media_id", media_id}, {"capabilities", Json(std::move(capabilities))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/playback/sessions";
    request.body.assign(text.begin(), text.end());
    auto response = playback.handle(request);
    REQUIRE(response.status == 201);
    auto body = Json::parse(std::string(response.body.begin(), response.body.end()));
    CHECK(body.find("mode")->asString() == "direct");
    auto selection = body.find("selection");
    REQUIRE(selection && selection->isObject());
    CHECK(selection->find("video_stream")->asInt64() == -1);
    CHECK(selection->find("audio_stream")->asInt64() == 1);

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
    CHECK(playback_status_json.find("server_version")->asString() == kServerVersion);

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
    REQUIRE(created_json.find("stream") != nullptr);
    CHECK(created_json.find("stream")->find("subtitle_url")->isNull());
    REQUIRE(created_json.find("source") != nullptr);
    CHECK(created_json.find("source")->find("format")->asString() == "mov,mp4,m4a,3gp,3g2,mj2");
    CHECK(created_json.find("source")->find("bitrate")->asUInt64() == 4'000'000);
    REQUIRE(created_json.find("source")->find("streams")->isArray());
    CHECK(created_json.find("source")->find("streams")->asArray().size() == 4);
    CHECK(created_json.find("source")->find("streams")->asArray()[0].find("bitrate")->asUInt64() == 3'700'000);
    CHECK(created_json.find("source")->find("streams")->asArray()[1].find("bitrate")->asUInt64() == 192'000);
    REQUIRE(created_json.find("output") != nullptr);
    CHECK(created_json.find("output")->find("video")->find("transform")->asString() == "copy");
    CHECK(created_json.find("output")->find("video")->find("bitrate")->asUInt64() == 3'700'000);
    CHECK(created_json.find("output")->find("audio")->find("transform")->asString() == "copy");
    CHECK(created_json.find("output")->find("audio")->find("bitrate")->asUInt64() == 192'000);
    REQUIRE(created_json.find("preferences") != nullptr);
    CHECK(created_json.find("preferences")->find("mode")->asString() == "auto");
    REQUIRE(created_json.find("options") != nullptr);
    auto options = created_json.find("options");
    REQUIRE(options->find("audio_streams") != nullptr);
    REQUIRE(options->find("audio_streams")->isArray());
    REQUIRE(!options->find("audio_streams")->asArray().empty());
    CHECK(options->find("audio_streams")->asArray().front().isObject());
    REQUIRE(options->find("subtitle_streams") != nullptr);
    REQUIRE(options->find("subtitle_streams")->isArray());
    REQUIRE(options->find("subtitle_streams")->asArray().size() == 1);
    CHECK(options->find("subtitle_streams")->asArray().front().find("index")->asInt64() == 2);
    REQUIRE(options->find("media_ids") != nullptr);
    CHECK(options->find("media_ids")->asArray().size() == 1);
    REQUIRE(options->find("quality_heights") != nullptr);
    CHECK(!options->find("quality_heights")->asArray().empty());
    auto session_id = created_json.find("session_id")->asString();
    auto direct_url = created_json.find("stream")->find("url")->asString();

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

    // A subtitle-only PATCH must not rebuild or seek the A/V generation.
    // The subtitle resource changes independently and gets a stream-specific
    // URL so browser caches cannot return the previously-selected track.
    const auto probes_before_subtitle = fake_engine_ptr->probes();
    const auto prepares_before_subtitle = fake_engine_ptr->vod_prepares();
    Json::Object subtitle_only_preferences{{"subtitle_stream", 2}};
    Json::Object subtitle_only_root{{"preferences", Json(std::move(subtitle_only_preferences))}};
    auto subtitle_only_text = Json(std::move(subtitle_only_root)).dump();
    HttpRequest subtitle_only;
    subtitle_only.method = "PATCH";
    subtitle_only.path = "/api/v1/playback/sessions/" + session_id;
    subtitle_only.body.assign(subtitle_only_text.begin(), subtitle_only_text.end());
    auto subtitle_only_response = playback.handle(subtitle_only);
    REQUIRE(subtitle_only_response.status == 200);
    auto subtitle_only_json = Json::parse(std::string(subtitle_only_response.body.begin(),
                                                      subtitle_only_response.body.end()));
    CHECK(subtitle_only_json.find("stream")->find("url")->asString() == direct_url);
    CHECK(subtitle_only_json.find("selection")->find("subtitle_stream")->asInt64() == 2);
    CHECK(subtitle_only_json.find("stream")->find("subtitle_url")->asString().find("/1/subtitle-2/manifest.json") != std::string::npos);
    CHECK(fake_engine_ptr->probes() == probes_before_subtitle);
    CHECK(fake_engine_ptr->vod_prepares() == prepares_before_subtitle);

    const auto subtitle_segments_before_manifest = fake_engine_ptr->subtitle_segments();
    HttpRequest selected_subtitle_manifest;
    selected_subtitle_manifest.method = "GET";
    selected_subtitle_manifest.path = subtitle_only_json.find("stream")->find("subtitle_url")->asString();
    auto selected_subtitle_manifest_response = playback.handle(selected_subtitle_manifest);
    REQUIRE(selected_subtitle_manifest_response.status == 200);
    CHECK(selected_subtitle_manifest_response.content_type.starts_with("application/json"));
    CHECK(fake_engine_ptr->subtitle_segments() == subtitle_segments_before_manifest);
    REQUIRE(selected_subtitle_manifest_response.stream != nullptr);
    Bytes selected_subtitle_manifest_bytes(static_cast<size_t>(selected_subtitle_manifest_response.content_length()));
    REQUIRE(selected_subtitle_manifest_response.stream->read(0, selected_subtitle_manifest_bytes) ==
            selected_subtitle_manifest_bytes.size());
    auto selected_subtitle_manifest_json = Json::parse(std::string(
        selected_subtitle_manifest_bytes.begin(), selected_subtitle_manifest_bytes.end()));
    CHECK(selected_subtitle_manifest_json.find("format")->asString() == "macha-webvtt-segments");
    REQUIRE(selected_subtitle_manifest_json.find("segment_durations_ms")->asArray().size() == 15);

    auto selected_subtitle_base = selected_subtitle_manifest.path.substr(0, selected_subtitle_manifest.path.rfind('/'));
    HttpRequest selected_subtitle_segment;
    selected_subtitle_segment.method = "GET";
    selected_subtitle_segment.path = selected_subtitle_base + "/segment-0.vtt";
    auto selected_subtitle_segment_response = playback.handle(selected_subtitle_segment);
    REQUIRE(selected_subtitle_segment_response.status == 200);
    CHECK(selected_subtitle_segment_response.content_type.starts_with("text/vtt"));
    CHECK(fake_engine_ptr->subtitle_segments() == subtitle_segments_before_manifest + 1);
    auto selected_subtitle_segment_again = playback.handle(selected_subtitle_segment);
    REQUIRE(selected_subtitle_segment_again.status == 200);
    CHECK(fake_engine_ptr->subtitle_segments() == subtitle_segments_before_manifest + 1);

    Json::Object subtitle_off_preferences{{"subtitle_stream", Json(nullptr)},
                                          {"subtitle_language", ""}};
    Json::Object subtitle_off_root{{"preferences", Json(std::move(subtitle_off_preferences))}};
    auto subtitle_off_text = Json(std::move(subtitle_off_root)).dump();
    HttpRequest subtitle_off;
    subtitle_off.method = "PATCH";
    subtitle_off.path = "/api/v1/playback/sessions/" + session_id;
    subtitle_off.body.assign(subtitle_off_text.begin(), subtitle_off_text.end());
    auto subtitle_off_response = playback.handle(subtitle_off);
    REQUIRE(subtitle_off_response.status == 200);
    auto subtitle_off_json = Json::parse(std::string(subtitle_off_response.body.begin(),
                                                     subtitle_off_response.body.end()));
    CHECK(subtitle_off_json.find("stream")->find("url")->asString() == direct_url);
    CHECK(subtitle_off_json.find("selection")->find("subtitle_stream")->asInt64() == -1);
    CHECK(subtitle_off_json.find("stream")->find("subtitle_url")->isNull());

    Json::Object bitmap_subtitle_preferences{{"subtitle_stream", 3}};
    Json::Object bitmap_subtitle_root{{"preferences", Json(std::move(bitmap_subtitle_preferences))}};
    auto bitmap_subtitle_text = Json(std::move(bitmap_subtitle_root)).dump();
    HttpRequest bitmap_subtitle;
    bitmap_subtitle.method = "PATCH";
    bitmap_subtitle.path = "/api/v1/playback/sessions/" + session_id;
    bitmap_subtitle.body.assign(bitmap_subtitle_text.begin(), bitmap_subtitle_text.end());
    CHECK(playback.handle(bitmap_subtitle).status == 400);

    Json::Object bad_track_preferences{{"audio_stream", 99}};
    Json::Object bad_track_root{{"preferences", Json(std::move(bad_track_preferences))}};
    auto bad_track_text = Json(std::move(bad_track_root)).dump();
    HttpRequest bad_track;
    bad_track.method = "PATCH";
    bad_track.path = "/api/v1/playback/sessions/" + session_id;
    bad_track.body.assign(bad_track_text.begin(), bad_track_text.end());
    CHECK(playback.handle(bad_track).status == 400);

    // Quality is a real session preference, not a client-only label. Requesting
    // 720p must rebuild the session at 720p and advertise only modes compatible
    // with that quality constraint.
    Json::Object quality_preferences{{"mode", "auto"}, {"max_height", 720}};
    Json::Object quality_root{{"preferences", Json(std::move(quality_preferences))}};
    auto quality_text = Json(std::move(quality_root)).dump();
    HttpRequest quality;
    quality.method = "PATCH";
    quality.path = "/api/v1/playback/sessions/" + session_id;
    quality.body.assign(quality_text.begin(), quality_text.end());
    auto quality_response = playback.handle(quality);
    REQUIRE(quality_response.status == 200);
    auto quality_json = Json::parse(std::string(quality_response.body.begin(), quality_response.body.end()));
    CHECK(quality_json.find("mode")->asString() == "transcode");
    CHECK(quality_json.find("preferences")->find("mode")->asString() == "auto");
    CHECK(quality_json.find("preferences")->find("max_height")->asInt64() == 720);
    CHECK(quality_json.find("output")->find("video")->find("codec")->asString() == "h264");
    CHECK(quality_json.find("output")->find("video")->find("height")->asInt64() == 720);
    auto quality_modes = quality_json.find("options")->find("modes")->asArray();
    CHECK(std::none_of(quality_modes.begin(), quality_modes.end(), [](const Json& mode) {
        return mode.asString() == "direct" || mode.asString() == "remux";
    }));

    Json::Object restore_preferences{{"mode", "auto"}, {"max_height", Json(nullptr)},
                                     {"max_bitrate", Json(nullptr)}};
    Json::Object restore_root{{"preferences", Json(std::move(restore_preferences))}};
    auto restore_text = Json(std::move(restore_root)).dump();
    HttpRequest restore;
    restore.method = "PATCH";
    restore.path = "/api/v1/playback/sessions/" + session_id;
    restore.body.assign(restore_text.begin(), restore_text.end());
    auto restore_response = playback.handle(restore);
    REQUIRE(restore_response.status == 200);
    auto restore_json = Json::parse(std::string(restore_response.body.begin(), restore_response.body.end()));
    CHECK(restore_json.find("mode")->asString() == "direct");
    CHECK(restore_json.find("preferences")->find("max_height")->isNull());

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
    CHECK(patched_json.find("preferences")->find("mode")->asString() == "transcode");
    CHECK(patched_json.find("output")->find("video")->find("transform")->asString() == "transcode");
    CHECK(patched_json.find("output")->find("video")->find("codec")->asString() == "h264");
    CHECK(patched_json.find("output")->find("audio")->find("transform")->asString() == "transcode");
    CHECK(patched_json.find("output")->find("audio")->find("bitrate")->asUInt64() == 192000);
    auto hls_url = patched_json.find("stream")->find("url")->asString();
    auto subtitle_url = patched_json.find("stream")->find("subtitle_url")->asString();

    HttpRequest playlist;
    playlist.method = "GET";
    playlist.path = hls_url;
    auto playlist_response = playback.handle(playlist);
    REQUIRE(playlist_response.status == 200);
    REQUIRE(playlist_response.stream != nullptr);
    Bytes playlist_bytes(static_cast<size_t>(playlist_response.content_length()));
    REQUIRE(playlist_response.stream->read(0, playlist_bytes) == playlist_bytes.size());
    CHECK(std::string(playlist_bytes.begin(), playlist_bytes.end()).find("#EXTM3U") != std::string::npos);

    HttpRequest subtitle_manifest_request;
    subtitle_manifest_request.method = "GET";
    subtitle_manifest_request.path = subtitle_url;
    auto subtitle_manifest_response = playback.handle(subtitle_manifest_request);
    REQUIRE(subtitle_manifest_response.status == 200);
    CHECK(subtitle_manifest_response.content_type.starts_with("application/json"));
    REQUIRE(subtitle_manifest_response.stream != nullptr);
    Bytes subtitle_manifest_bytes(static_cast<size_t>(subtitle_manifest_response.content_length()));
    REQUIRE(subtitle_manifest_response.stream->read(0, subtitle_manifest_bytes) == subtitle_manifest_bytes.size());
    auto subtitle_manifest_json = Json::parse(std::string(subtitle_manifest_bytes.begin(),
                                                          subtitle_manifest_bytes.end()));
    REQUIRE(!subtitle_manifest_json.find("segment_durations_ms")->asArray().empty());
    auto subtitle_base = subtitle_url.substr(0, subtitle_url.rfind('/'));
    HttpRequest subtitle_segment;
    subtitle_segment.method = "GET";
    subtitle_segment.path = subtitle_base + "/segment-0.vtt";
    auto subtitle_segment_response = playback.handle(subtitle_segment);
    REQUIRE(subtitle_segment_response.status == 200);
    CHECK(subtitle_segment_response.content_type.starts_with("text/vtt"));

    // The same in-place subtitle path must preserve a live transformed HLS
    // generation as well; no replacement MediaEngineSession is started.
    const auto plans_before_transformed_subtitle_off = fake_engine_ptr->started_plans().size();
    Json::Object transformed_subtitle_off_preferences{{"subtitle_stream", Json(nullptr)},
                                                      {"subtitle_language", ""}};
    Json::Object transformed_subtitle_off_root{{"preferences", Json(std::move(transformed_subtitle_off_preferences))}};
    auto transformed_subtitle_off_text = Json(std::move(transformed_subtitle_off_root)).dump();
    HttpRequest transformed_subtitle_off;
    transformed_subtitle_off.method = "PATCH";
    transformed_subtitle_off.path = "/api/v1/playback/sessions/" + session_id;
    transformed_subtitle_off.body.assign(transformed_subtitle_off_text.begin(), transformed_subtitle_off_text.end());
    auto transformed_subtitle_off_response = playback.handle(transformed_subtitle_off);
    REQUIRE(transformed_subtitle_off_response.status == 200);
    auto transformed_subtitle_off_json = Json::parse(std::string(transformed_subtitle_off_response.body.begin(),
                                                                 transformed_subtitle_off_response.body.end()));
    CHECK(transformed_subtitle_off_json.find("stream")->find("url")->asString() == hls_url);
    CHECK(transformed_subtitle_off_json.find("stream")->find("subtitle_url")->isNull());
    CHECK(fake_engine_ptr->started_plans().size() == plans_before_transformed_subtitle_off);

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
    auto path_stream_url = path_json.find("stream")->find("url")->asString();

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

    // Session creation must wake an otherwise indefinitely-blocked cleanup
    // worker. A short idle timeout catches the condition_variable_any mistake
    // where notify_all() was paired with a predicate that could never become
    // true and therefore silently swallowed the notification.
    streaming.session_idle = 50ms;
    playback.reconfigure(streaming);
    auto expiring = playback.handle(create);
    REQUIRE(expiring.status == 201);
    REQUIRE(wait_until([&] {
        HttpRequest status_request;
        status_request.method = "GET";
        status_request.path = "/api/v1/playback/status";
        auto response = playback.handle(status_request);
        if (response.status != 200) return false;
        auto body = Json::parse(std::string(response.body.begin(), response.body.end()));
        return body.find("sessions") && body.find("sessions")->asUInt64() == 0;
    }, 1s));

    playback.stop();
    service.stop();
}

} // namespace


void test_subtitle_text_normalisation() {
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello") == "Hello");
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello, world") == "Hello, world");
    CHECK(plain_ass_subtitle_text("Dialogue: 0,0,Default,,0,0,0,,{\\i1}Hello{\\i0}\\Nworld") ==
          "Hello\nworld");
}

int main() {
    try {
        RUN_TEST(test_codec_and_crypto);
        RUN_TEST(test_local_store);
        RUN_TEST(test_storage_pool_and_persistent_cache);
        RUN_TEST(test_metadata_codec_and_replica);
        RUN_TEST(test_metadata_identity_rpc);
        RUN_TEST(test_config);
        RUN_TEST(test_placement);
        RUN_TEST(test_capacity_placement);
        RUN_TEST(test_async_rpc_move_ownership);
        RUN_TEST(test_rpc_v13_frame_priority_and_variable_length);
        RUN_TEST(test_repair_step_is_bounded_and_yields);
        RUN_TEST(test_rpc_v13_persistence_and_multiplexing);
        RUN_TEST(test_rpc_v13_bidirectional_and_deduplication);
        RUN_TEST(test_mutual_bootstrap_prunes_cross_dial);
        RUN_TEST(test_rpc_v7_handshake_is_rejected);
        RUN_TEST(test_rpc_slow_control_does_not_abort_data);
        RUN_TEST(test_rpc_health_and_control_not_starved_by_data);
        RUN_TEST(test_early_replication_quorum);
        RUN_TEST(test_put_spills_stalled_owners_and_commits_degraded_floor);
        RUN_TEST(test_put_falls_back_after_remote_launch_failure);
        RUN_TEST(test_joiner_cannot_form_genesis);
        RUN_TEST(test_bootstrap_joiner_requires_complete_checkpoint_survey);
        RUN_TEST(test_two_node_mutual_bootstrap_metadata_quorum);
        RUN_TEST(test_replication_policy_change_on_restart);
        RUN_TEST(test_genesis_root_configuration);
        RUN_TEST(test_open_write_metadata_merge);
        RUN_TEST(test_fresh_and_resumed_write_exactness);
        RUN_TEST(test_active_write_size_visibility);
        RUN_TEST(test_open_write_survives_rename);
        RUN_TEST(test_fuse_frontend_ordering_merging_and_cache);
        RUN_TEST(test_fuse_read_only_release_does_not_publish_writer_data);
        RUN_TEST(test_fuse_frontend_unlink_and_rename_over_open_inode_ordering);
        RUN_TEST(test_fuse_frontend_read_overlay_truncate_and_hydration_hints);
        RUN_TEST(test_fuse_frontend_namespace_refresh_is_demand_driven);
        RUN_TEST(test_full_replica_fallback);
        RUN_TEST(test_replacement_node_recovers_namespace_and_replication);
        RUN_TEST(test_hydration_scheduler_and_prediction);
        RUN_TEST(test_replica_selector);
        RUN_TEST(test_cache_hydrator_fetches_to_persistent_cache);
        RUN_TEST(test_media_probe_and_online_catalogue_scanner);
        RUN_TEST(test_catalogue_cache_ignores_unrelated_metadata_generation);
        RUN_TEST(test_catalogue_hint_queue_persistence_coalescing_and_priority);
        RUN_TEST(test_ingest_catalogue_feedback_and_external_clear_cleanup);
        RUN_TEST(test_catalogue_warm_read_defers_remote_refresh);
        RUN_TEST(test_metadata_decoded_cache_ttl_recovers_missed_notice);
        RUN_TEST(test_catalogue_root_ready_without_local_artwork);
        RUN_TEST(test_macos_unicode_namespace_aliases);
        RUN_TEST(test_media_index_cache_survives_namespace_churn);
        RUN_TEST(test_catalogue_sync_search_and_artwork_gc);
        RUN_TEST(test_media_segment_store_backpressure_and_spill);
        RUN_TEST(test_media_vod_index_planning_rejects_partial_indexes);
        RUN_TEST(test_reseek_hls_vod_reuses_prepared_random_access_state);
        RUN_TEST(test_media_timestamp_repair);
        RUN_TEST(test_subtitle_text_normalisation);
        RUN_TEST(test_http_server_serves_streams_concurrently);
        RUN_TEST(test_playback_probe_failure_is_stage_specific);
        RUN_TEST(test_attached_picture_audio_direct_play);
        RUN_TEST(test_concurrent_transcode_admission_is_reserved);
        RUN_TEST(test_playback_sessions_and_streaming_http_bodies);
        RUN_TEST(test_three_node_cluster);
    } catch (const std::exception& e) {
        std::cerr << "Unhandled test exception: " << e.what() << '\n';
        return 2;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}

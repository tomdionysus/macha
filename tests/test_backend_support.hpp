// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "cluster.hpp"
#include "distributed_store.hpp"
#include "metadata_manager.hpp"
#include "codec.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "diagnostics.hpp"
#include "durable_file.hpp"
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
#include "test_framework.hpp"
#include "test_support.hpp"
#include "torrent.hpp"
#include "replica_selector.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <latch>
#include <map>
#include <mutex>
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

namespace macha::test_support {

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

class ConcurrentCapturingLogger final : public Logger {
    LogLevel level_;
    mutable std::mutex mutex_;
    std::vector<std::pair<LogLevel, std::string>> records_;

  public:
    explicit ConcurrentCapturingLogger(LogLevel level) : level_(level) {}

    bool enabled(LogLevel level) const noexcept override {
        return level_ == LogLevel::all ||
               static_cast<unsigned char>(level) >= static_cast<unsigned char>(level_);
    }

    void log(LogLevel level, const std::string& message) override {
        std::lock_guard lock(mutex_);
        records_.emplace_back(level, message);
    }

    std::vector<std::pair<LogLevel, std::string>> records() const {
        std::lock_guard lock(mutex_);
        return records_;
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

class FakeMediaEngine : public MediaEngine {
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
        result.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920, 1080, 0, 0, 8, true, false, 3'700'000, false, 0, ""});
        result.streams.push_back(MediaStreamInfo{1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0, 2, 48000, 0, true, false, 192'000, false, 0, ""});
        result.streams.push_back(MediaStreamInfo{2, MediaStreamType::subtitle, "subrip", "", "eng", 0, 0, 0, 0, 0, false, false, 0, false, 0, ""});
        result.streams.push_back(MediaStreamInfo{3, MediaStreamType::subtitle, "hdmv_pgs_subtitle", "", "eng", 0, 0, 0, 0, 0, false, false, 0, false, 0, ""});
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

class BlockingHttpBody final : public HttpBodySource {
    std::atomic_bool& entered_;
    TestGate& gate_;
    uint64_t size_;
    std::atomic_bool first_read_{true};
  public:
    BlockingHttpBody(std::atomic_bool& entered, TestGate& gate, uint64_t size)
        : entered_(entered), gate_(gate), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_) return 0;
        if (first_read_.exchange(false)) {
            entered_ = true;
            gate_.enter_and_wait();
        }
        auto n = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        std::fill_n(destination.data(), n, static_cast<uint8_t>('s'));
        return n;
    }
};

inline std::string raw_http_get(uint16_t port, std::string_view path,
                                const std::map<std::string, std::string>& headers = {}) {
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
    auto request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    for (const auto& [key, value] : headers)
        request += key + ": " + value + "\r\n";
    request += "Connection: close\r\n\r\n";
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

// Every non-exempt API route now requires a live session bearer token (see
// SessionApi / HttpServer's SessionAuthenticator). Mint one directly against
// the node under test rather than through HTTP, so callers exercising an
// unrelated route don't also need to drive session creation by hand.
inline std::map<std::string, std::string> bearer_header(Service& service) {
    auto minted = service.node().sessions().create({"anonymous"});
    REQUIRE(minted.has_value());
    return {{"Authorization", "Bearer " + minted->bearer_token}};
}

// Raw request/response helpers for keep-alive scenarios, where raw_http_get's
// send-Connection-close-and-read-to-EOF shape does not apply: the caller keeps
// the socket open and drives it request by request.
struct RawHttpResponse {
    int status{};
    std::map<std::string, std::string> headers; // lowercased keys
    std::string body;
};

inline void raw_http_send(int fd, std::string_view request) {
    size_t sent = 0;
    while (sent < request.size()) {
        auto n = send(fd, request.data() + sent, request.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("http test send failed");
        sent += static_cast<size_t>(n);
    }
}

inline RawHttpResponse raw_http_read_response(int fd) {
    std::string input;
    std::array<char, 8192> buffer{};
    size_t header_end;
    while ((header_end = input.find("\r\n\r\n")) == std::string::npos) {
        auto n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("http test response truncated before headers");
        input.append(buffer.data(), static_cast<size_t>(n));
    }

    RawHttpResponse response;
    std::istringstream head(input.substr(0, header_end));
    std::string status_line;
    std::getline(head, status_line);
    {
        std::istringstream status_stream(status_line);
        std::string http_version;
        status_stream >> http_version >> response.status;
    }
    std::string line;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        response.headers[key] = value;
    }

    size_t content_length = 0;
    if (auto found = response.headers.find("content-length"); found != response.headers.end())
        content_length = static_cast<size_t>(std::stoull(found->second));
    response.body = input.substr(header_end + 4);
    while (response.body.size() < content_length) {
        auto n = recv(fd, buffer.data(), std::min(buffer.size(), content_length - response.body.size()), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("http test response body truncated");
        response.body.append(buffer.data(), static_cast<size_t>(n));
    }
    return response;
}

inline RawHttpResponse raw_http_exchange(int fd, std::string_view request) {
    raw_http_send(fd, request);
    return raw_http_read_response(fd);
}

} // namespace macha::test_support

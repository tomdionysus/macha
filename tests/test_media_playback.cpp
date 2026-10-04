// SPDX-License-Identifier: GPL-3.0-or-later
#include "stepped_time.hpp"
#include "test_backend_support.hpp"
#include "media/media_containers.hpp"
#include "playback/segment_holds.hpp"
#include "catalogue/media_information.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

const SessionIdentity anonymous{.id = "", .roles = {"anonymous"}};

Json body_of(const HttpResponse& response) {
    return Json::parse(std::string(response.body.begin(), response.body.end()));
}

std::string session_id_of(const HttpResponse& response) {
    return body_of(response).find("session_id")->asString();
}

// The body's `idempotency` field on a keyed create, or empty when absent.
std::string idempotency_of(const HttpResponse& response) {
    const auto body = body_of(response);
    const auto* field = body.find("idempotency");
    return field ? field->asString() : std::string{};
}

// Playback serves payloads as range-capable streams, so reading one means draining it.
std::string response_text(const HttpResponse& response) {
    if (!response.stream) return std::string(response.body.begin(), response.body.end());
    Bytes bytes(static_cast<size_t>(response.stream->size()));
    size_t filled = 0;
    while (filled < bytes.size()) {
        const auto n = response.stream->read(filled, std::span(bytes).subspan(filled));
        if (n == 0) break;
        filled += n;
    }
    bytes.resize(filled);
    return std::string(bytes.begin(), bytes.end());
}

HttpRequest playback_request(std::string method, std::string path, const Json::Object& body = {},
                             const SessionIdentity& who = anonymous) {
    HttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.session = who;
    if (!body.empty()) {
        const auto text = Json(body).dump();
        request.body.assign(text.begin(), text.end());
    }
    return request;
}

std::string segment_name(int index, const char* suffix = ".m4s") {
    std::ostringstream name;
    name << "segment-" << std::setfill('0') << std::setw(6) << index << suffix;
    return name.str();
}

// One node's filesystem and catalogue: what PlaybackManager,
// MediaInformationService and CatalogueApi are built on. Each test gives
// every file distinct bytes, so managers sharing the node never share a
// media id or its cached profile.
class PlaybackNode {
    TestNode node_{"playback"};
    std::unique_ptr<CatalogueManager> catalogue_;
    std::unique_ptr<CatalogueHintQueue> hints_;
    unsigned managers_{};

  public:
    PlaybackNode() {
        node_.start();
        auto& bare = node_.node();
        catalogue_ = std::make_unique<CatalogueManager>(bare, bare.local_state(), bare.metadata_server(),
                                                        node_.store(), node_.metadata(), bare.ledger());
        hints_ = std::make_unique<CatalogueHintQueue>(node_.path() / "catalogue-hints");
        filesystem().mkdir("/media", 0755, getuid(), getgid());
    }

    FileSystem& filesystem() { return node_.filesystem(); }
    CatalogueManager& catalogue() { return *catalogue_; }
    CatalogueHintQueue& hints() { return *hints_; }
    const std::filesystem::path& path() const { return node_.path(); }

    // Writes `bytes` at `path`, replacing what is there, and returns its media id.
    std::string write(const std::string& path, const Bytes& bytes) {
        try {
            (void)filesystem().getattr(path);
        } catch (const FsError&) {
            filesystem().create_file(path, 0644, getuid(), getgid());
        }
        auto writer = filesystem().open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
        return file_media_id(filesystem().getattr(path));
    }

    // Streaming enabled, with a spool directory no other manager uses.
    StreamingConfig streaming() {
        StreamingConfig config;
        config.enabled = true;
        config.temp_path = path() / ("playback-" + std::to_string(++managers_));
        return config;
    }

    std::unique_ptr<PlaybackManager> playback(
        StreamingConfig streaming, std::shared_ptr<MediaEngine> engine,
        std::function<size_t(const std::vector<std::string>&)> request_media_profiles = {},
        MediaInformationService* information = nullptr, CatalogueApiConfig api = {},
        const TimeSource& time = steady_time_source()) {
        auto manager = std::make_unique<PlaybackManager>(
            filesystem(), node_.resources().transcode_rates, node_.resources().memory, catalogue(), api,
            std::move(streaming), std::move(engine), std::move(request_media_profiles), information,
            PlaybackManager::MediaFacts{}, time);
        manager->start();
        return manager;
    }

    std::unique_ptr<MediaInformationService> information(
        std::shared_ptr<MediaEngine> engine,
        MediaInformationService::ProfilePublisher publisher = {}) {
        return std::make_unique<MediaInformationService>(filesystem(), catalogue(), std::move(engine),
                                                         path() / ("media-info-" + std::to_string(++managers_)),
                                                         std::move(publisher));
    }
};

// Creates a session for `media_id` with `preferences`, plus any other body
// fields in `extra`.
HttpResponse create_session(PlaybackManager& playback, const std::string& media_id,
                            Json::Object preferences, Json::Object extra = {},
                            const SessionIdentity& who = anonymous,
                            std::map<std::string, std::string, std::less<>> query = {}) {
    extra["media_id"] = media_id;
    extra["preferences"] = Json(std::move(preferences));
    auto request = playback_request("POST", "/api/v1/playback/sessions", extra, who);
    request.query = std::move(query);
    return playback.handle(request);
}

HttpResponse session_call(PlaybackManager& playback, const std::string& method, const std::string& id,
                          const Json::Object& body = {}, const SessionIdentity& who = anonymous) {
    return playback.handle(playback_request(method, "/api/v1/playback/sessions/" + id, body, who));
}

Json playback_status(PlaybackManager& playback) {
    const auto response = playback.handle(playback_request("GET", "/api/v1/playback/status"));
    REQUIRE(response.status == 200);
    return body_of(response);
}

// FakeMediaEngine with its store reachable, so a test can watch what a refused
// request did to the producer, and publish a fragment while a request is held.
class ObservableHlsMediaEngine final : public FakeMediaEngine {
    mutable std::mutex store_mutex_;
    std::shared_ptr<MediaSegmentStore> store_;

  public:
    std::unique_ptr<MediaEngineSession> start_hls(const MediaSource&, const HlsVodPlan& vod_plan,
                                                  std::chrono::milliseconds segment_duration,
                                                  size_t max_ahead_segments, uint64_t memory_limit,
                                                  const std::filesystem::path& spill) override {
        auto store = std::make_shared<MediaSegmentStore>(max_ahead_segments, memory_limit, spill,
                                                         segment_duration,
                                                         vod_plan.segment_durations);
        REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
        REQUIRE(store->publish_segment(Bytes{'s', 'e', 'g', '0'}, 4.0));
        {
            std::lock_guard lock(store_mutex_);
            store_ = store;
        }
        return std::make_unique<FakeMediaEngineSession>(std::move(store));
    }

    std::shared_ptr<MediaSegmentStore> store() const {
        std::lock_guard lock(store_mutex_);
        return store_;
    }
};

// A probe that blocks on a gate (and may then fail), counting how many ran.
class CoalescingProbeMediaEngine final : public MediaEngine {
    TestGate& gate_;
    bool fail_{};
    std::atomic_uint probes_{};
  public:
    explicit CoalescingProbeMediaEngine(TestGate& gate, bool fail = false)
        : gate_(gate), fail_(fail) {}
    MediaEngineStatus status() const override {
        return {true, "fake", "coalescing-probe", true, true};
    }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds = {}) override {
        ++probes_;
        gate_.enter_and_wait();
        if (fail_) throw std::runtime_error("synthetic coalesced probe failure");
        MediaProbeResult result;
        result.format = "mov,mp4,m4a,3gp,3g2,mj2";
        result.duration_seconds = 60.0;
        result.bitrate = 4'000'000;
        result.streams.push_back(MediaStreamInfo{
            0, MediaStreamType::video, "h264", "High", "", 1920, 1080,
            0, 0, 8, true, false, 3'700'000, false});
        result.streams.push_back(MediaStreamInfo{
            1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0,
            2, 48000, 0, true, false, 192'000, false});
        return result;
    }
    HlsVodPlan prepare_hls_vod(const MediaSource&, const PlaybackPlan&, double,
                               std::chrono::milliseconds, bool,
                               std::chrono::milliseconds = {}) override {
        throw std::runtime_error("direct test must not prepare HLS");
    }
    std::unique_ptr<MediaEngineSession> start_hls(
        const MediaSource&, const HlsVodPlan&, std::chrono::milliseconds, size_t, uint64_t,
        const std::filesystem::path&) override {
        throw std::runtime_error("direct test must not start HLS");
    }
    std::string extract_webvtt_segment(
        const MediaSource&, int, std::chrono::milliseconds, std::chrono::milliseconds,
        std::chrono::milliseconds) override {
        throw std::runtime_error("direct test must not extract subtitles");
    }
    unsigned probes() const { return probes_.load(); }
};

// Like FakeMediaEngine, but extract_webvtt_segment blocks on a gate so a test
// can hold a session's subtitle_cache mutex while exercising other operations.
class GatedSubtitleMediaEngine final : public MediaEngine {
    TestGate& gate_;
  public:
    explicit GatedSubtitleMediaEngine(TestGate& gate) : gate_(gate) {}
    MediaEngineStatus status() const override {
        return {true, "fake", "gated-subtitle", true, true};
    }
    MediaProbeResult probe(const MediaSource&, std::chrono::milliseconds = {}) override {
        MediaProbeResult result;
        result.format = "mov,mp4,m4a,3gp,3g2,mj2";
        result.duration_seconds = 60.0;
        result.bitrate = 4'000'000;
        result.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920, 1080, 0, 0, 8, true, false, 3'700'000});
        result.streams.push_back(MediaStreamInfo{1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0, 2, 48000, 0, true, false, 192'000});
        result.streams.push_back(MediaStreamInfo{2, MediaStreamType::subtitle, "subrip", "", "eng", 0, 0, 0, 0, 0, false, false});
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
        REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
        REQUIRE(store->publish_segment(Bytes{'s', 'e', 'g'}, 4.0));
        return std::make_unique<FakeMediaEngineSession>(std::move(store));
    }
    std::string extract_webvtt_segment(const MediaSource&, int, std::chrono::milliseconds,
                                       std::chrono::milliseconds, std::chrono::milliseconds) override {
        gate_.enter_and_wait();
        return "WEBVTT\n\n00:00.000 --> 00:01.000\nsubtitle\n";
    }
};

// Records the order media is probed in; the first probe may wait for its
// cancellation instead of completing.
class PriorityMediaInformationEngine final : public MediaEngine {
    bool cancel_first_{};
    mutable std::mutex mutex_;
    std::vector<std::string> order_;
    std::atomic_uint starts_{};
    std::atomic_uint completions_{};
    std::atomic_uint cancellations_{};
  public:
    explicit PriorityMediaInformationEngine(bool cancel_first = false)
        : cancel_first_(cancel_first) {}
    MediaEngineStatus status() const override {
        return {true, "fake", "media-information-priority", true, true};
    }
    MediaProbeResult probe(const MediaSource& source,
                           std::chrono::milliseconds = {}) override {
        const auto attempt = ++starts_;
        {
            std::lock_guard lock(mutex_);
            order_.push_back(source.media_id);
        }
        if (cancel_first_ && attempt == 1) {
            while (!source.cancelled || !source.cancelled->load())
                std::this_thread::sleep_for(1ms);
            ++cancellations_;
            throw std::runtime_error("synthetic speculative cancellation");
        }
        ++completions_;
        MediaProbeResult result;
        result.format = "mov,mp4,m4a,3gp,3g2,mj2";
        result.duration_seconds = 60.0;
        result.bitrate = 4'000'000;
        result.streams.push_back(MediaStreamInfo{
            0, MediaStreamType::video, "h264", "High", "", 1920, 1080,
            0, 0, 8, true, false, 3'700'000, false});
        result.streams.push_back(MediaStreamInfo{
            1, MediaStreamType::audio, "aac", "LC", "eng", 0, 0,
            2, 48000, 0, true, false, 192'000, false});
        return result;
    }
    HlsVodPlan prepare_hls_vod(const MediaSource&, const PlaybackPlan&, double,
                               std::chrono::milliseconds, bool,
                               std::chrono::milliseconds = {}) override {
        throw std::runtime_error("media information test does not prepare VOD");
    }
    std::unique_ptr<MediaEngineSession> start_hls(
        const MediaSource&, const HlsVodPlan&, std::chrono::milliseconds, size_t, uint64_t,
        const std::filesystem::path&) override {
        throw std::runtime_error("media information test does not start HLS");
    }
    std::string extract_webvtt_segment(
        const MediaSource&, int, std::chrono::milliseconds, std::chrono::milliseconds,
        std::chrono::milliseconds) override {
        throw std::runtime_error("media information test does not extract subtitles");
    }
    unsigned starts() const { return starts_.load(); }
    unsigned completions() const { return completions_.load(); }
    unsigned cancellations() const { return cancellations_.load(); }
    std::vector<std::string> order() const {
        std::lock_guard lock(mutex_);
        return order_;
    }
};

// HEVC Main 10, PQ transfer (Dolby Vision profile 8), E-AC3 audio, in Matroska.
class HdrFakeMediaEngine final : public FakeMediaEngine {
  public:
    MediaProbeResult probe(const MediaSource& source, std::chrono::milliseconds timeout = {}) override {
        auto result = FakeMediaEngine::probe(source, timeout);
        result.format = "matroska,webm";
        result.streams.clear();
        MediaStreamInfo video;
        video.index = 0;
        video.type = MediaStreamType::video;
        video.codec = "hevc";
        video.profile = "Main 10";
        video.width = 1920;
        video.height = 802;
        video.bit_depth = 10;
        video.level = 153;
        video.color_transfer = "smpte2084";
        video.default_stream = true;
        MediaStreamInfo audio;
        audio.index = 1;
        audio.type = MediaStreamType::audio;
        audio.codec = "eac3";
        audio.channels = 6;
        audio.sample_rate = 48000;
        audio.default_stream = true;
        result.streams = {video, audio};
        return result;
    }
};

// English and French audio beside the fake engine's one video and two English
// subtitles, so every choice is a real one.
class TwoAudioFakeMediaEngine final : public FakeMediaEngine {
  public:
    MediaProbeResult probe(const MediaSource& source, std::chrono::milliseconds timeout = {}) override {
        auto result = FakeMediaEngine::probe(source, timeout);
        result.streams.push_back(MediaStreamInfo{4, MediaStreamType::audio, "ac3", "", "fra", 0, 0, 6, 48000, 0,
                                                 false, false, 384'000, false, 0, ""});
        return result;
    }
};

// A remux engine with a keyframe every 10 s, so an off-keyframe seek lands
// before the request (FakeMediaEngine's empty index makes every offset zero).
class KeyframedRemuxMediaEngine final : public FakeMediaEngine {
  public:
    HlsVodPlan prepare_hls_vod(const MediaSource& source, const PlaybackPlan& plan,
                               double duration_seconds,
                               std::chrono::milliseconds segment_duration,
                               bool allow_video_transcode_fallback,
                               std::chrono::milliseconds timeout = {}) override {
        auto vod = FakeMediaEngine::prepare_hls_vod(source, plan, duration_seconds,
                                                    segment_duration,
                                                    allow_video_transcode_fallback, timeout);
        for (double seconds = 0.0; seconds < duration_seconds; seconds += 10.0)
            vod.video_random_access_points.push_back(seconds);
        const auto requested_ms =
            media_vod::clamp_seek_ms(plan.seek.count(), duration_seconds);
        // Only a stream copy is bound to a sync sample; a transcode starts on the frame asked for.
        if (plan.video != MediaTransform::copy) {
            vod.playback.seek = std::chrono::milliseconds(requested_ms);
            vod.playback.seek_offset = {};
            vod.playback.seek_requested = std::chrono::milliseconds(requested_ms);
            return vod;
        }
        auto indexed = media_vod::indexed_plan(vod.video_random_access_points, duration_seconds,
                                               requested_ms, vod.seek_segment_seconds);
        REQUIRE(indexed.has_value());
        vod.playback.seek = std::chrono::milliseconds(indexed->seek_ms);
        vod.playback.seek_offset = std::chrono::milliseconds(indexed->seek_offset_ms);
        vod.playback.seek_requested = std::chrono::milliseconds(indexed->seek_requested_ms);
        vod.segment_durations = std::move(indexed->segment_durations);
        return vod;
    }
};

// A pipeline whose first fragment and start progress the test controls, for
// `start=async`; shared by engine and test so neither holds a dangling session.
struct ProgressingState {
    std::atomic_bool running{true};
    std::shared_ptr<MediaSegmentStore> segments;
    MediaStartProgress progress;
};

class ProgressingSession final : public MediaEngineSession {
    std::shared_ptr<ProgressingState> state_;

  public:
    explicit ProgressingSession(std::shared_ptr<ProgressingState> state) : state_(std::move(state)) {}
    bool running() const override { return state_->running.load(); }
    std::optional<int> exit_code() const override {
        return state_->running.load() ? std::optional<int>{} : std::optional<int>{0};
    }
    std::string diagnostics() const override { return {}; }
    std::shared_ptr<MediaSegmentStore> segments() const override { return state_->segments; }
    void note_segment_requested(uint64_t index) override { state_->segments->note_requested(index); }
    void stop() override {
        state_->running.store(false);
        state_->segments->cancel();
    }
    const MediaStartProgress* start_progress() const override { return &state_->progress; }
};

class ProgressingMediaEngine final : public MediaEngine {
    mutable std::mutex mutex_;
    std::shared_ptr<ProgressingState> last_;
    std::atomic_uint starts_{};

  public:
    MediaEngineStatus status() const override { return {true, "fake", "progressing", true, true}; }
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
        vod.segment_durations.push_back(2.0);
        double left = duration_seconds - 2.0;
        while (left > segment + 0.001) { vod.segment_durations.push_back(segment); left -= segment; }
        vod.segment_durations.push_back(std::max(0.001, left));
        return vod;
    }
    std::unique_ptr<MediaEngineSession> start_hls(const MediaSource&, const HlsVodPlan& vod_plan,
                                                  std::chrono::milliseconds segment_duration,
                                                  size_t max_ahead_segments, uint64_t memory_limit,
                                                  const std::filesystem::path& spill_directory) override {
        auto state = std::make_shared<ProgressingState>();
        state->segments = std::make_shared<MediaSegmentStore>(max_ahead_segments, memory_limit, spill_directory,
                                                              segment_duration, vod_plan.segment_durations);
        {
            std::lock_guard lock(mutex_);
            last_ = state;
        }
        ++starts_;
        return std::make_unique<ProgressingSession>(std::move(state));
    }
    std::string extract_webvtt_segment(const MediaSource&, int, std::chrono::milliseconds,
                                       std::chrono::milliseconds, std::chrono::milliseconds) override { return {}; }
    unsigned starts() const { return starts_.load(); }
    std::shared_ptr<ProgressingState> last() const {
        std::lock_guard lock(mutex_);
        return last_;
    }
    void advance(int64_t output_ms) {
        auto state = last();
        REQUIRE(state != nullptr);
        state->progress.source_bytes_read.fetch_add(4096);
        state->progress.output_media_us.store(output_ms * 1000);
        state->progress.moved();
    }
    void release() {
        auto state = last();
        REQUIRE(state != nullptr);
        REQUIRE(state->segments->publish_init(Bytes{'i', 'n', 'i', 't'}));
        REQUIRE(state->segments->publish_segment(Bytes{'s', 'e', 'g'}, 2.0));
    }
};

} // namespace

MACHA_FAST_TEST("media_playback", test_segment_store_serves_its_plan_as_a_closed_playlist) {
    // The playlist is the plan, which exists before any media: complete and
    // closed on the first fetch, byte-identical afterwards, named for its
    // container, and withheld once the generation breaks.
    TempDir t;
    const std::vector<double> plan{2.0, 4.0, 4.0, 3.5};
    auto store = std::make_shared<MediaSegmentStore>(8, 8 * 1024, t.path() / "spill", 4000ms, plan);
    const auto state = store->snapshot();
    REQUIRE(state.segment_count == 0);
    REQUIRE(!state.init_ready);
    CHECK(state.planned_segments == 4);

    const auto first = store->playlist();
    REQUIRE(!first.empty());
    CHECK(first.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos);
    CHECK(first.find("#EXT-X-PLAYLIST-TYPE:EVENT") == std::string::npos);
    CHECK(first.find("#EXT-X-ENDLIST") != std::string::npos);
    CHECK(first.find("#EXT-X-MAP:URI=\"init.mp4\"") != std::string::npos);
    // Every planned entry is advertised, produced or not.
    for (int i = 0; i < 4; ++i) CHECK(first.find(segment_name(i)) != std::string::npos);
    CHECK(first.find(segment_name(4)) == std::string::npos);
    // EXTINF is the planned length; TARGETDURATION is the longest planned entry,
    // rounded up.
    CHECK(first.find("#EXTINF:2.000,") != std::string::npos);
    CHECK(first.find("#EXTINF:3.500,") != std::string::npos);
    CHECK(first.find("#EXT-X-TARGETDURATION:4\n") != std::string::npos);

    // Immutable through production, even when a fragment is longer than planned.
    REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
    CHECK(store->playlist() == first);
    REQUIRE(store->publish_segment(Bytes(64, 0x10), 2.0));
    CHECK(store->playlist() == first);
    REQUIRE(store->publish_segment(Bytes(64, 0x11), 6.0));
    CHECK(store->playlist() == first);
    REQUIRE(store->publish_segment(Bytes(64, 0x12), 4.0));
    REQUIRE(store->publish_segment(Bytes(64, 0x13), 3.5));
    store->finish();
    CHECK(store->snapshot().error.empty());
    CHECK(store->playlist() == first);
    auto init = store->object("init.mp4");
    REQUIRE(init.has_value());
    CHECK(std::string(init->begin(), init->end()) == "init");
    for (int i = 0; i < 4; ++i) {
        auto segment = store->object(segment_name(i));
        REQUIRE(segment.has_value());
        CHECK((*segment)[0] == static_cast<uint8_t>(0x10 + i));
    }

    // A broken generation withholds the playlist rather than serve a promise
    // it cannot keep.
    auto broken = std::make_shared<MediaSegmentStore>(8, 8 * 1024, t.path() / "spill-broken",
                                                      4000ms, plan);
    REQUIRE(!broken->playlist().empty());
    broken->fail("generation broke");
    CHECK(broken->playlist().empty());

    // MPEG-TS: the same closed plan with version 3 and .ts names, no init
    // fragment (the first fragment makes it ready), and an init request that
    // is a genuine miss rather than held.
    auto ts = std::make_shared<MediaSegmentStore>(4, 64 * 1024, t.path() / "spill-ts", 4000ms,
                                                  std::vector<double>{2.0, 4.0, 4.0},
                                                  MediaContainer::mpegts);
    CHECK(ts->container() == MediaContainer::mpegts);
    const auto planned_ts = ts->playlist();
    CHECK(planned_ts.find("#EXT-X-VERSION:3") != std::string::npos);
    CHECK(planned_ts.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos);
    CHECK(planned_ts.find(segment_name(2, ".ts")) != std::string::npos);
    CHECK(planned_ts.find("#EXT-X-ENDLIST") != std::string::npos);
    CHECK(planned_ts.find("#EXT-X-MAP") == std::string::npos);
    CHECK(planned_ts.find(".m4s") == std::string::npos);
    const auto ts_init_asked = std::chrono::steady_clock::now();
    CHECK(!ts->wait_object("init.mp4", 5s).has_value());
    CHECK(std::chrono::steady_clock::now() - ts_init_asked < 1s);
    REQUIRE(ts->publish_segment(Bytes(188 * 3, 0x47), 2.0));
    REQUIRE(ts->wait_ready(10ms));
    CHECK(ts->playlist() == planned_ts);
    REQUIRE(ts->object(segment_name(0, ".ts")).has_value());
    CHECK(ts->object(segment_name(0, ".ts"))->size() == 188 * 3);
    // Both spellings map to index 0.
    CHECK(ts->object(segment_name(0)).has_value());
    REQUIRE(ts->publish_segment(Bytes(188, 0x47), 4.0));
    REQUIRE(ts->publish_segment(Bytes(188, 0x47), 4.0));
    ts->finish();
    CHECK(ts->playlist() == planned_ts);
}

MACHA_TEST("media_playback", test_segment_store_holds_requests_until_published_superseded_or_ended) {
    TempDir t;
    // Back-pressure: the producer may run max_ahead fragments past the highest
    // request and then parks; older fragments spill so residency stays bounded,
    // and the store's memory is leased from the ledger.
    {
        RetainedMemoryLedger retained(8 * 1024, 1024, 4 * 1024, 1024);
        auto store = std::make_shared<MediaSegmentStore>(2, 2 * 1024, t.path() / "spill", 4000ms,
                                                         std::vector<double>{4.0, 4.0, 4.0, 4.0});
        REQUIRE(store->attach_memory_ledger(retained));
        CHECK(retained.stats().owner_bytes[static_cast<size_t>(MemoryOwner::playback_segment)] ==
              2 * 1024);
        REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
        REQUIRE(store->publish_segment(Bytes(1024, 0x10), 4.0));
        REQUIRE(store->publish_segment(Bytes(1024, 0x11), 4.0));
        REQUIRE(store->publish_segment(Bytes(1024, 0x12), 4.0));
        CHECK(store->snapshot().producer_parked);

        std::atomic_bool fourth_published{};
        std::jthread producer([&] {
            fourth_published.store(store->publish_segment(Bytes(1024, 0x13), 4.0));
        });
        std::this_thread::sleep_for(50ms);
        CHECK(!fourth_published.load());
        store->note_requested(3);
        auto waited = store->wait_object(segment_name(3), 1s);
        producer.join();
        CHECK(fourth_published.load());
        REQUIRE(waited.has_value());
        CHECK(waited->size() == 1024);
        CHECK((*waited)[0] == 0x13);

        store->finish();
        REQUIRE(store->wait_ready(10ms));
        const auto state = store->snapshot();
        CHECK(state.init_ready);
        CHECK(state.finished);
        CHECK(state.segment_count == 4);
        CHECK(state.highest_requested == 3);
        CHECK(state.resident_bytes <= 2 * 1024 + 1024 + 4);
        CHECK(state.spill_bytes >= 1024);
        CHECK(state.descriptor_bytes >= state.segment_count);
        CHECK(!state.producer_parked);
        for (int i = 0; i < 4; ++i) {
            auto segment = store->object(segment_name(i));
            REQUIRE(segment.has_value());
            CHECK(segment->size() == 1024);
            CHECK((*segment)[0] == static_cast<uint8_t>(0x10 + i));
        }
        store.reset();
        CHECK(retained.stats().used_bytes == 0);
    }

    // Superseding wakes a request held on an unproduced fragment, and is
    // reversible: a failed replacement leaves the active store's long-poll
    // intact.
    {
        auto store = std::make_shared<MediaSegmentStore>(8, 8 * 1024, t.path() / "spill-superseded", 4000ms,
                                                         std::vector<double>{4.0, 4.0, 4.0, 4.0, 4.0});
        REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
        REQUIRE(store->publish_segment(Bytes(16, 0x10), 4.0));
        std::atomic_bool returned{};
        std::optional<Bytes> result;
        std::jthread waiter([&] {
            store->note_requested(4);
            result = store->wait_object(segment_name(4), {});
            returned.store(true);
        });
        std::this_thread::sleep_for(50ms);
        CHECK(!returned.load());
        store->mark_superseded(true);
        waiter.join();
        CHECK(!result.has_value());
        CHECK(store->snapshot().error.empty());
        CHECK(!store->snapshot().finished);

        store->mark_superseded(false);
        std::atomic_bool second_returned{};
        std::jthread second([&] {
            second_returned.store(store->wait_object(segment_name(4), {}).has_value());
        });
        std::this_thread::sleep_for(50ms);
        CHECK(!second_returned.load());
        for (uint8_t fill = 0x11; fill <= 0x14; ++fill)
            REQUIRE(store->publish_segment(Bytes(16, fill), 4.0));
        second.join();
        CHECK(second_returned.load());
    }

    // init.mp4 may be asked for before the muxer has written it: held like a
    // fragment, released by its publication or by the generation ending.
    {
        auto store = std::make_shared<MediaSegmentStore>(8, 8 * 1024, t.path() / "spill-init", 4000ms,
                                                         std::vector<double>{4.0, 4.0});
        CHECK(!store->object("init.mp4").has_value());
        std::atomic_bool returned{};
        std::optional<Bytes> init;
        std::jthread waiter([&] {
            init = store->wait_object("init.mp4", {});
            returned.store(true);
        });
        std::this_thread::sleep_for(50ms);
        CHECK(!returned.load());
        REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
        waiter.join();
        REQUIRE(init.has_value());
        CHECK(std::string(init->begin(), init->end()) == "init");

        auto broken = std::make_shared<MediaSegmentStore>(8, 8 * 1024, t.path() / "spill-init-broken",
                                                          4000ms, std::vector<double>{4.0});
        std::jthread breaker([&] { broken->fail("generation broke"); });
        CHECK(!broken->wait_object("init.mp4", {}).has_value());
        breaker.join();

        // An unknown object is an immediate miss, held by nothing.
        const auto asked = std::chrono::steady_clock::now();
        CHECK(!store->wait_object("nonsense.bin", 5s).has_value());
        CHECK(std::chrono::steady_clock::now() - asked < 1s);
    }
}

MACHA_FAST_TEST("media_playback", test_production_rate_excludes_time_parked_on_demand) {
    // producing_ms counts encode time only, so the rate is what the node could
    // sustain: time parked at the look-ahead limit is excluded.
    TempDir t;
    SteppedTime time;
    MediaSegmentStore store(2, 64ULL * 1024 * 1024, t.path() / "spill", 4000ms, {}, MediaContainer::fmp4, time);
    REQUIRE(store.publish_init(Bytes{'i', 'n', 'i', 't'}));

    // Three fragments of 20 ms encode work each are inside the look-ahead.
    for (int i = 0; i < 3; ++i) {
        time.advance(20ms);
        REQUIRE(store.publish_segment(Bytes{'s', 'e', 'g'}, 4.0));
    }
    // Nothing has been requested, so the fourth parks: index 3 is past
    // highest_requested (0) + max_ahead (2).
    CHECK(store.snapshot().producer_parked);
    time.advance(20ms);
    const auto reads = time.reads();
    std::thread producer([&] { store.publish_segment(Bytes{'s', 'e', 'g'}, 4.0); });
    // The producer has taken its entry time and is at the gate.
    REQUIRE(wait_until([&] { return time.reads() == reads + 1; }, 5s));
    time.advance(2000ms);
    store.note_requested(3);
    producer.join();

    const auto state = store.snapshot();
    CHECK(state.segment_count == 4);
    CHECK(state.produced_media_ms == 16'000);
    // Four 20 ms fragments; the two seconds parked are not in it.
    CHECK(state.producing_ms == 80);
    CHECK(state.produced_age_ms == 0);
    CHECK(!state.producer_parked);
}

MACHA_FAST_TEST("media_playback", test_segment_hold_arbiter_admits_within_limits_and_refuses_beyond_them) {
    // A hold is an explicitly admitted resource, so the limits are testable
    // without any thread blocking.
    SegmentHoldArbiter arbiter(2, 3);
    auto why = SegmentHoldArbiter::Refusal::budget_exhausted;

    // One in flight plus one prefetch, per session.
    auto first = arbiter.try_acquire("session-a", &why);
    auto second = arbiter.try_acquire("session-a", &why);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(arbiter.outstanding("session-a") == 2);

    // A third from the same session is refused for the session limit, not the node's.
    auto third = arbiter.try_acquire("session-a", &why);
    CHECK(!third.has_value());
    CHECK(why == SegmentHoldArbiter::Refusal::session_limit);

    // Another session is unaffected by the first one's limit.
    auto other = arbiter.try_acquire("session-b", &why);
    REQUIRE(other.has_value());
    CHECK(arbiter.outstanding() == 3);

    // The global budget binds across sessions: session-b is under its own limit
    // and still refused.
    auto beyond = arbiter.try_acquire("session-b", &why);
    CHECK(!beyond.has_value());
    CHECK(why == SegmentHoldArbiter::Refusal::budget_exhausted);

    // Releasing returns capacity to the node, not just to the session.
    first->reset();
    CHECK(arbiter.outstanding() == 2);
    CHECK(arbiter.outstanding("session-a") == 1);
    auto after_release = arbiter.try_acquire("session-b", &why);
    CHECK(after_release.has_value());

    // Release is idempotent, and a moved-from hold releases nothing twice.
    first->reset();
    CHECK(arbiter.outstanding() == 3);
    {
        auto moved = std::move(*second);
        second.reset();
        CHECK(arbiter.outstanding() == 3);
    }
    // Scope exit released the moved-to hold exactly once.
    CHECK(arbiter.outstanding() == 2);
    CHECK(arbiter.outstanding("session-a") == 0);
}

MACHA_FAST_TEST("media_playback", test_vod_plans_follow_the_keyframe_index) {
    CHECK(media_vod::requires_seek_index_materialisation("matroska,webm"));
    CHECK(media_vod::requires_seek_index_materialisation("webm"));
    CHECK(!media_vod::requires_seek_index_materialisation("mov,mp4,m4a,3gp,3g2,mj2"));

    std::vector<double> complete;
    for (double seconds = 0.0; seconds < 120.0; seconds += 2.0) complete.push_back(seconds);
    auto full = media_vod::indexed_plan(complete, 120.0, 0, 4.0);
    REQUIRE(full.has_value());
    CHECK(std::abs(full->actual_seek_seconds) < 0.0005);
    CHECK(full->segment_durations.size() == 30);
    for (const auto duration : full->segment_durations) CHECK(duration <= 4.001);

    // avformat_find_stream_info() can leave a Matroska index holding only the
    // keyframes seen while probing; that is not a complete index.
    const std::vector<double> partial{0.0, 2.0};
    CHECK(!media_vod::indexed_plan(partial, 120.0, 0, 4.0).has_value());

    // A partially populated index must also be rejected when it contains
    // enough early entries to produce several apparently sensible fragments.
    const std::vector<double> partial_with_several_starts{0.0, 4.0, 8.0, 12.0, 16.0};
    CHECK(!media_vod::indexed_plan(partial_with_several_starts, 120.0, 0, 4.0).has_value());

    // Sparse but complete GOPs can still be remuxed; a fragment is as long as its GOP.
    std::vector<double> sparse_complete;
    for (double seconds = 0.0; seconds < 60.0; seconds += 10.0)
        sparse_complete.push_back(seconds);
    CHECK(media_vod::indexed_plan(sparse_complete, 60.0, 0, 4.0).has_value());

    // Scene-cut encodes leave keyframe gaps well past 3x the target; a 40 s
    // fragment is a long fragment, not an unusable index.
    std::vector<double> scene_cut{0.0, 4.0, 44.0, 48.0, 52.0, 90.0, 94.0, 118.0};
    auto scene_cut_plan = media_vod::indexed_plan(scene_cut, 120.0, 0, 4.0);
    REQUIRE(scene_cut_plan.has_value());
    CHECK(std::abs(scene_cut_plan->longest_segment_seconds - 40.0) < 0.0005);
    // A gap a viewer would wait minutes to seek across is unusable.
    const std::vector<double> huge_gap{0.0, 4.0, 110.0, 114.0, 118.0};
    CHECK(!media_vod::indexed_plan(huge_gap, 120.0, 0, 4.0).has_value());

    // One fragment is legitimate for genuinely short media.
    const std::vector<double> short_index{0.0};
    auto short_plan = media_vod::indexed_plan(short_index, 6.0, 0, 4.0);
    REQUIRE(short_plan.has_value());
    CHECK(short_plan->segment_durations.size() == 1);
    CHECK(std::abs(short_plan->segment_durations.front() - 6.0) < 0.0005);

    // A seek starts at the last keyframe at or before the request, never after,
    // and reports the remainder as an offset.
    auto seeked = media_vod::indexed_plan(complete, 120.0, 61'000, 4.0);
    REQUIRE(seeked.has_value());
    CHECK(std::abs(seeked->actual_seek_seconds - 60.0) < 0.0005);
    CHECK(seeked->seek_ms == 60'000);
    CHECK(seeked->seek_offset_ms == 1'000);
    CHECK(seeked->seek_requested_ms == 61'000);
    CHECK(seeked->seek_ms + seeked->seek_offset_ms == seeked->seek_requested_ms);

    // A request on a keyframe has offset zero, so clients can make aligned seeks.
    auto aligned = media_vod::indexed_plan(complete, 120.0, 62'000, 4.0);
    REQUIRE(aligned.has_value());
    CHECK(aligned->seek_ms == 62'000);
    CHECK(aligned->seek_offset_ms == 0);

    // A keyframe a fraction of a millisecond after the request is not a
    // candidate: it rounds UP to 61'001 ms (rounding down would land
    // avformat_seek_file's backward search one keyframe early).
    const std::vector<double> fractional{0.0, 30.0, 61.0004, 90.0};
    auto fractional_plan = media_vod::indexed_plan(fractional, 120.0, 61'000, 4.0);
    REQUIRE(fractional_plan.has_value());
    CHECK(fractional_plan->seek_ms == 30'000);
    CHECK(fractional_plan->seek_offset_ms == 31'000);

    // Out of range clamps to [0, duration - 1 ms]; the invariant holds against
    // the clamped request, so the clamp is visible.
    CHECK(media_vod::clamp_seek_ms(500'000, 120.0) == 119'999);
    CHECK(media_vod::clamp_seek_ms(-5, 120.0) == 0);

    // No indexed keyframe at or before the request: baseline zero, the offset
    // carries the whole request, and the mode is kept (a stream's first sample
    // is always a sync sample).
    const std::vector<double> late_index{40.0, 44.0, 48.0};
    auto unnamed_start = media_vod::indexed_plan(late_index, 60.0, 20'000, 4.0);
    REQUIRE(unnamed_start.has_value());
    CHECK(unnamed_start->seek_ms == 0);
    CHECK(unnamed_start->seek_offset_ms == 20'000);
    CHECK(unnamed_start->seek_requested_ms == 20'000);

    // The Cues behind a plan; the gaps bound the true GOP from above.
    const auto density = media_vod::index_density(complete, 120.0);
    CHECK(density.entries == 60);
    CHECK(std::abs(density.longest_gap_seconds - 2.0) < 0.0005);
    CHECK(std::abs(density.median_gap_seconds - 2.0) < 0.0005);

    // A prepared plan re-seeks without the engine. A PATCH seek and a create
    // seek agree: baseline at the keyframe at or before the request, the
    // remainder published as an offset.
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
    CHECK(remux_seek->playback.seek == 60s);
    CHECK(remux_seek->playback.seek_offset == 1s);
    CHECK(remux_seek->playback.seek_requested == 61s);
    CHECK(remux_seek->playback.seek + remux_seek->playback.seek_offset ==
          remux_seek->playback.seek_requested);
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
    CHECK(transcode_seek->playback.seek_offset == 0s);
    CHECK(transcode_seek->playback.seek_requested == 61s);
    REQUIRE(transcode_seek->segment_durations.size() >= 2);
    // A seek's first fragment is the 2 s start-up fragment; the rest keep the target.
    CHECK(std::abs(transcode_seek->segment_durations.front() - 2.0) < 0.0005);
    CHECK(std::abs(transcode_seek->segment_durations[1] - 4.0) < 0.0005);

    // A transcode with a known keyframe index does not snap: the encoder starts
    // exactly where it was told. The keyframe at 62.5274 s, not a whole number
    // of milliseconds, must not attract the seek.
    HlsVodPlan transcode_with_keyframes = transcode;
    transcode_with_keyframes.source_duration_seconds = 7200.0;
    transcode_with_keyframes.video_random_access_points = {0.0, 30.0, 60.0, 62.5274, 6000.0};
    auto unsnapped_seek = reseek_hls_vod(transcode_with_keyframes, 61s);
    REQUIRE(unsnapped_seek.has_value());
    CHECK(unsnapped_seek->playback.seek == 61s);
    CHECK(unsnapped_seek->playback.seek_offset == 0s);
    REQUIRE(!unsnapped_seek->segment_durations.empty());
    CHECK(std::abs(unsnapped_seek->segment_durations.front() - 2.0) < 0.0005);
    // Seeking past the last known keyframe is not a special case.
    auto past_last_keyframe = reseek_hls_vod(transcode_with_keyframes, 6500s);
    REQUIRE(past_last_keyframe.has_value());
    CHECK(past_last_keyframe->playback.seek == 6500s);

    // A decline names the precondition that failed.
    HlsVodPlan unavailable;
    std::string reason;
    CHECK(!reseek_hls_vod(unavailable, 10s, &reason).has_value());
    CHECK(reason == "plan-not-reusable");
}

MACHA_FAST_TEST("media_playback", test_media_vocabulary_names_containers_codecs_and_subtitles) {
    // Every file the catalogue admits is one playback can name.
    for (std::string_view ext : {".mkv", ".mp4", ".m4v", ".avi", ".mov", ".wmv", ".mpg",
                                 ".mpeg", ".ts", ".m2ts", ".webm"}) {
        CHECK(video_extension(ext));
        CHECK(!audio_extension(ext));
        CHECK(!container_for_extension(std::string("film") + std::string(ext)).empty());
    }
    for (std::string_view ext : {".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".wav",
                                 ".aiff", ".wma", ".mka", ".oga"}) {
        CHECK(audio_extension(ext));
        CHECK(!video_extension(ext));
        CHECK(!container_for_extension(std::string("song") + std::string(ext)).empty());
    }
    // A name that states a codec rather than a container: admitted as audio,
    // and left for the probe to name.
    CHECK(audio_extension(".alac"));
    CHECK(container_for_extension("song.alac").empty());
    CHECK(!video_extension(".srt"));
    CHECK(!audio_extension(".jpg"));

    // The probed format wins over the name; the name only picks a member of a
    // family the format names more than once.
    CHECK(container_for_format("matroska,webm", "film.mkv") == "matroska");
    CHECK(container_for_format("matroska,webm", "film.webm") == "webm");
    CHECK(container_for_format("matroska,webm", "film.mp4") == "matroska");
    CHECK(container_for_format("mov,mp4,m4a,3gp,3g2,mj2", "film.mp4") == "mp4");
    CHECK(container_for_format("avi", "film.avi") == "avi");
    CHECK(container_for_format("mpegts", "film.ts") == "mpegts");
    // Nothing probed: the name stands in; unknown to both tables: libav's own
    // name, never empty.
    CHECK(container_for_format("", "film.mkv") == "matroska");
    CHECK(container_for_format("nut", "film.nut") == "nut");
    CHECK(container_for_format("", "film.qqq").empty());

    // A source's Content-Type is what the probe found in it: the container,
    // and whether there is a picture. A Matroska file named .mp4 is served as
    // Matroska; sound alone in Matroska or MP4 is served as audio.
    CHECK(direct_mime(container_for_format("matroska,webm", "film.mp4"), true) == "video/x-matroska");
    CHECK(direct_mime("matroska", false) == "audio/x-matroska");
    CHECK(direct_mime(container_for_format("mov,mp4,m4a,3gp,3g2,mj2", "song.mp4"), false) == "audio/mp4");
    CHECK(direct_mime("avi", true) == "video/x-msvideo");
    CHECK(direct_mime("avi", false) == "video/x-msvideo");
    CHECK(direct_mime("mp3", false) == "audio/mpeg");
    CHECK(direct_mime("nut", true) == "application/octet-stream");
    CHECK(segment_mime("index.m3u8") == "application/vnd.apple.mpegurl");
    CHECK(segment_mime("seg7.m4s") == "video/mp4");
    CHECK(segment_mime("seg7.ts") == "video/mp2t");

    // The two containers do not carry the same codecs, and neither answers a
    // question about the other kind of stream.
    CHECK(fmp4_video_copy_supported("hevc"));
    CHECK(fmp4_video_copy_supported("HEVC"));
    CHECK(fmp4_video_copy_supported("av1"));
    CHECK(!fmp4_video_copy_supported("mpeg2video"));
    CHECK(!fmp4_video_copy_supported("aac"));
    CHECK(mpegts_video_copy_supported("mpeg2video"));
    CHECK(!mpegts_video_copy_supported("av1"));
    CHECK(fmp4_audio_copy_supported("eac3"));
    CHECK(fmp4_audio_copy_supported("opus"));
    CHECK(!fmp4_audio_copy_supported("mp3"));
    CHECK(mpegts_audio_copy_supported("mp3"));
    CHECK(mpegts_audio_copy_supported("eac3"));
    CHECK(!mpegts_audio_copy_supported("opus"));
    CHECK(!mpegts_audio_copy_supported("h264"));

    CHECK(webvtt_subtitle_codec_supported("subrip"));
    CHECK(webvtt_subtitle_codec_supported("mov_text"));
    CHECK(!webvtt_subtitle_codec_supported("dvd_subtitle"));
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello") == "Hello");
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello, world") == "Hello, world");
    CHECK(plain_ass_subtitle_text("Dialogue: 0,0,Default,,0,0,0,,{\\i1}Hello{\\i0}\\Nworld") ==
          "Hello\nworld");

    // HLS CODECS describe the fragments: the source's for a copy, the
    // libx264/AAC output for a transcode.
    MediaStreamInfo hevc10;
    hevc10.codec = "hevc";
    hevc10.profile = "Main 10";
    hevc10.bit_depth = 10;
    hevc10.level = 153;
    hevc10.width = 1920;
    hevc10.height = 802;
    MediaStreamInfo eac3;
    eac3.codec = "eac3";
    MediaStreamInfo h264_high;
    h264_high.codec = "h264";
    h264_high.profile = "High";
    h264_high.level = 41;
    CHECK(hls_codec_string("hevc", &hevc10, false) == "hvc1.2.4.L153.B0");
    CHECK(hls_codec_string("h264", &h264_high, false) == "avc1.640029");
    CHECK(hls_codec_string("eac3", &eac3, false) == "ec-3");
    CHECK(hls_codec_string("ac3", nullptr, false) == "ac-3");
    CHECK(hls_codec_string("aac", nullptr, false) == "mp4a.40.2");
    CHECK(hls_codec_string("h264", &hevc10, true) == "avc1.640029");
    CHECK(hls_codec_string("aac", &eac3, true) == "mp4a.40.2");

    PlaybackPlan remux;
    remux.video = MediaTransform::copy;
    remux.audio = MediaTransform::copy;
    remux.video_codec = "hevc";
    remux.audio_codec = "eac3";
    CHECK(hls_variant_stream_inf(remux, &hevc10, &eac3, 10'887'601) ==
          "#EXT-X-STREAM-INF:BANDWIDTH=10887601,CODECS=\"hvc1.2.4.L153.B0,ec-3\",RESOLUTION=1920x802");
    PlaybackPlan transcode;
    transcode.video = MediaTransform::transcode;
    transcode.audio = MediaTransform::transcode;
    transcode.video_codec = "h264";
    transcode.audio_codec = "aac";
    transcode.target_height = 720;
    CHECK(hls_variant_stream_inf(transcode, &hevc10, &eac3, 10'887'601) ==
          "#EXT-X-STREAM-INF:BANDWIDTH=5000000,CODECS=\"avc1.640029,mp4a.40.2\",RESOLUTION=1724x720");
}

MACHA_FAST_TEST("media_playback", test_media_timestamp_repair) {
    MediaTimestampRepairState state;

    MediaPacketTimestamps first{-69952, -69952, 40};
    normalize_media_timestamps(state, first);
    CHECK(first.pts == -69952);
    CHECK(first.dts == -69952);
    CHECK(state.repair_count() == 0);

    // Two packets with equal DTS after a seek/rescale: the second advances, and
    // the same correction applies to later source timestamps.
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

MACHA_FAST_TEST("media_playback", test_older_video_profiles_are_stale_and_regenerate) {
    CatalogueSnapshot::MediaProfile profile;
    profile.probe.format = "matroska,webm";
    profile.probe.duration_seconds = 6660.0;
    MediaStreamInfo video;
    video.index = 0;
    video.type = MediaStreamType::video;
    video.codec = "hevc";
    video.profile = "Main 10";
    MediaStreamInfo audio;
    audio.index = 1;
    audio.type = MediaStreamType::audio;
    audio.codec = "eac3";
    profile.probe.streams = {video, audio};

    // Fresh profiles carry the depth/transfer signalling negotiation needs.
    CHECK(profile.schema_version == catalogue_media_profile_schema);
    CHECK(valid_catalogue_media_profile("macha:abc", profile));
    // A profile from an older schema lacks it, so it is regenerated.
    for (uint32_t older = 1; older < catalogue_media_profile_schema; ++older) {
        profile.schema_version = older;
        CHECK(!valid_catalogue_media_profile("macha:abc", profile));
    }
    profile.schema_version = 1;
    // Unless it has no video stream.
    profile.probe.streams = {audio};
    CHECK(valid_catalogue_media_profile("macha:abc", profile));
}

// Integrated: the concurrency is HttpServer's worker pool over real sockets.
MACHA_TEST("media_playback", test_http_server_serves_streams_concurrently) {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = 0;
    config.workers = 2;
    config.max_connections = 8;
    config.stream_chunk_bytes = 16 * 1024;
    std::atomic_bool entered{};
    TestGate slow_body_gate;
    HttpServer server(config, [&](const HttpRequest& request) {
        if (request.path == "/slow") {
            HttpResponse response;
            response.content_type = "application/octet-stream";
            response.stream = std::make_shared<BlockingHttpBody>(entered, slow_body_gate, 128 * 1024);
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
    REQUIRE(slow_body_gate.wait_for_entries(1));
    auto started = Clock::now();
    auto fast = raw_http_get(server.bound_port(), "/fast");
    auto elapsed = Clock::now() - started;
    CHECK(fast.find("200 OK") != std::string::npos);
    CHECK(fast.ends_with("ok"));
    CHECK(elapsed < 300ms);
    slow_body_gate.open();
    slow.join();
    CHECK(slow_response.find("200 OK") != std::string::npos);
    CHECK(slow_response.size() >= 128 * 1024);
    server.stop();
}

MACHA_TEST("media_playback", test_playback_performs_the_instruction_it_is_given) {
    // Negotiation is the client's: playback is by media_id, performs exactly
    // the mode and per-stream transforms it is told, refuses what it cannot
    // perform or would have to choose, and reports what it served. One manager
    // per engine, all on one node.
    PlaybackNode node;

    // A title is not playable as such: only a file is. Stream, language and
    // container left open where there are several are refused with the
    // candidates; a language the media lacks is never answered with another.
    {
        const auto media_id = node.write("/media/two.mkv", pattern(128 * 1024 + 17, 1));
        auto streaming = node.streaming();
        streaming.startup_timeout = 2s;
        streaming.max_video_transcodes = 4;
        auto playback = node.playback(streaming, std::make_shared<TwoAudioFakeMediaEngine>());
        const auto create = [&](Json::Object root, int expect) {
            auto response = playback->handle(playback_request("POST", "/api/v1/playback/sessions", root));
            REQUIRE(response.status == expect);
            auto parsed = body_of(response);
            if (const auto* id = parsed.find("session_id"))
                playback->handle(playback_request("DELETE", "/api/v1/playback/sessions/" + id->asString()));
            return parsed;
        };
        const auto instruct = [&](Json::Object preferences, int expect = 201) {
            return create(Json::Object{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}}, expect);
        };
        const auto refusal = [](const Json& body, std::string code, std::string choice) {
            CHECK(body.find("status")->asString() == code);
            CHECK(body.find("error")->find("code")->asString() == code);
            CHECK(body.find("error")->find("choice")->asString() == choice);
            return body.find("error")->find("choices")->asArray();
        };

        auto by_item = create(Json::Object{{"item_id", "tmdb:movie:603"}, {"media_id", media_id},
                                           {"preferences", Json(Json::Object{{"mode", "direct"}})}},
                              400);
        CHECK(by_item.find("error")->find("code")->asString() == "item_id_not_accepted");
        auto nothing = create(Json::Object{{"preferences", Json(Json::Object{{"mode", "direct"}})}}, 400);
        CHECK(nothing.find("error")->find("code")->asString() == "media_id_required");

        auto open = refusal(instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}}, 400),
                            "choice_required", "audio_stream");
        REQUIRE(open.size() == 2);
        CHECK(open[0].asInt64() == 1);
        CHECK(open[1].asInt64() == 4);
        auto french = instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"audio_stream", 4}});
        CHECK(french.find("output")->find("audio")->find("source_stream")->asInt64() == 4);
        auto english = instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"audio_language", "eng"}});
        CHECK(english.find("output")->find("audio")->find("source_stream")->asInt64() == 1);
        auto german = refusal(instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"},
                                                    {"audio_language", "deu"}},
                                       400),
                              "choice_not_available", "audio_stream");
        CHECK(german.size() == 2);
        auto bogus = refusal(instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"audio_stream", 9}}, 400),
                             "choice_not_available", "audio_stream");
        CHECK(bogus.size() == 2);
        // Two English subtitles: a language that matches both picks neither.
        auto subtitles = refusal(instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"audio_stream", 1},
                                                       {"subtitle_language", "eng"}},
                                          400),
                                 "choice_required", "subtitle_stream");
        CHECK(subtitles.size() == 2);
        // Direct serves the file untouched and the player picks tracks: nothing is refused.
        auto direct = instruct(Json::Object{{"mode", "direct"}});
        CHECK(direct.find("mode")->asString() == "direct");
        CHECK(direct.find("output")->find("audio") == nullptr);
        // The session still reports every mode the media supports.
        CHECK(direct.find("options")->find("modes")->asArray().size() == 3);

        // Copy support is a fact about each stream, not about whichever came first.
        auto facts = playback_request("GET", "/api/v1/playback/media");
        facts.query["media_id"] = media_id;
        auto response = playback->handle(facts);
        REQUIRE(response.status == 200);
        const auto parsed = body_of(response);
        const auto& media = parsed.find("media")->asArray().front();
        CHECK(media.find("operations")->find("copy_into_fmp4") == nullptr);
        size_t audio_with_facts = 0;
        for (const auto& stream : media.find("streams")->asArray())
            if (stream.find("type")->asString() == "audio" && stream.find("copy_into")) ++audio_with_facts;
        CHECK(audio_with_facts == 2);
    }

    // HDR Matroska: direct is served over byte ranges as Matroska; transcode,
    // remux and the per-stream mixtures are performed as named; a mode
    // naming a transform it is not doing is refused, never reinterpreted; and
    // an update naming a mode restates the whole transform.
    {
        const auto media_id = node.write("/media/dv.mkv", pattern(128 * 1024 + 17, 2));
        auto streaming = node.streaming();
        streaming.startup_timeout = 2s;
        streaming.max_video_transcodes = 4;
        auto playback = node.playback(streaming, std::make_shared<HdrFakeMediaEngine>());
        const auto instruct = [&](Json::Object preferences, int expect = 201) {
            auto response = create_session(*playback, media_id, std::move(preferences));
            REQUIRE(response.status == expect);
            auto parsed = body_of(response);
            // Release the slot so the permutation walk does not hit the session limit.
            if (const auto* id = parsed.find("session_id"))
                session_call(*playback, "DELETE", id->asString());
            return parsed;
        };

        auto direct_session = instruct(Json::Object{{"mode", "direct"}});
        CHECK(direct_session.find("mode")->asString() == "direct");
        CHECK(direct_session.find("stream")->find("url")->asString().ends_with("/direct"));
        CHECK(direct_session.find("stream")->find("mime_type")->asString() == "video/x-matroska");
        CHECK(direct_session.find("source")->find("format")->asString() == "matroska,webm");
        CHECK(direct_session.find("output")->find("container")->asString() == "matroska");

        auto transcoded = instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}});
        CHECK(transcoded.find("mode")->asString() == "transcode");
        CHECK(transcoded.find("output")->find("video")->find("codec")->asString() == "h264");
        CHECK(transcoded.find("output")->find("video")->find("color_transfer")->asString() == "bt709");
        CHECK(transcoded.find("output")->find("audio")->find("codec")->asString() == "aac");
        // The source facts are reported whatever was asked for.
        auto source_streams = transcoded.find("source")->find("streams")->asArray();
        REQUIRE(!source_streams.empty());
        CHECK(source_streams.front().find("color_transfer")->asString() == "smpte2084");
        CHECK(source_streams.front().find("bit_depth")->asInt64() == 10);

        // Remux copies both, 10-bit PQ HEVC and E-AC-3 included.
        auto remuxed = instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}});
        CHECK(remuxed.find("mode")->asString() == "remux");
        CHECK(remuxed.find("output")->find("video")->find("codec")->asString() == "hevc");
        CHECK(remuxed.find("output")->find("video")->find("color_transfer")->asString() == "smpte2084");
        CHECK(remuxed.find("output")->find("audio")->find("codec")->asString() == "eac3");

        // Copy the video, re-encode the audio: a transcode, as the request says.
        auto mixed = instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"video", "copy"}});
        CHECK(mixed.find("mode")->asString() == "transcode");
        CHECK(mixed.find("output")->find("video")->find("transform")->asString() == "copy");
        CHECK(mixed.find("output")->find("audio")->find("transform")->asString() == "transcode");
        // A codec change is not a downmix: the 5.1 source stays 5.1 through AAC.
        CHECK(mixed.find("output")->find("audio")->find("channels")->asUInt64() == 6);
        CHECK(transcoded.find("output")->find("audio")->find("channels")->asUInt64() == 6);
        auto video_only = instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"audio", "copy"}});
        CHECK(video_only.find("output")->find("video")->find("transform")->asString() == "transcode");
        CHECK(video_only.find("output")->find("audio")->find("transform")->asString() == "copy");

        // The segment container is instructed too, and the session reports the one served.
        auto ts = instruct(Json::Object{{"mode", "transcode"}, {"container", "mpegts"}});
        CHECK(ts.find("output")->find("format")->asString() == "mpegts");
        CHECK(ts.find("output")->find("container")->asString() == "mpegts");
        auto ts_copy = instruct(Json::Object{{"mode", "remux"}, {"container", "mpegts"}});
        CHECK(ts_copy.find("output")->find("container")->asString() == "mpegts");
        CHECK(ts_copy.find("output")->find("video")->find("transform")->asString() == "copy");
        CHECK(ts_copy.find("output")->find("audio")->find("transform")->asString() == "copy");
        auto fmp4 = instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}});
        CHECK(fmp4.find("output")->find("container")->asString() == "fmp4");
        // No default container: an HLS instruction without one is refused with both listed.
        auto unnamed = instruct(Json::Object{{"mode", "remux"}}, 400);
        CHECK(unnamed.find("error")->find("code")->asString() == "choice_required");
        CHECK(unnamed.find("error")->find("choice")->asString() == "container");
        CHECK(unnamed.find("error")->find("choices")->asArray().size() == 2);

        // A mode is required, and a copy cannot also be a quality change.
        instruct(Json::Object{{"max_height", 720}}, 400);
        instruct(Json::Object{{"mode", "auto"}}, 400);
        instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"video", "copy"}, {"max_height", 720}}, 400);
        // direct and remux copy every stream; transcode re-encodes at least one.
        instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"audio", "transcode"}}, 400);
        instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"video", "transcode"}}, 400);
        instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"max_height", 720}}, 400);
        instruct(Json::Object{{"mode", "direct"}, {"audio", "transcode"}}, 400);
        instruct(Json::Object{{"mode", "direct"}, {"video", "transcode"}}, 400);
        instruct(Json::Object{{"mode", "direct"}, {"max_height", 720}}, 400);
        instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"video", "copy"}, {"audio", "copy"}}, 400);
        // The legal permutations are accepted.
        instruct(Json::Object{{"mode", "direct"}, {"video", "copy"}, {"audio", "copy"}});
        instruct(Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"video", "copy"}, {"audio", "copy"}});
        instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"video", "transcode"}, {"audio", "transcode"}});
        instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"video", "copy"}, {"audio", "transcode"}});
        instruct(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"video", "transcode"}, {"audio", "copy"}});

        // `mode` is shorthand for the whole transform, so an update naming it is
        // not judged against per-stream instructions from the mode it replaces.
        const auto update = [&](const std::string& id, Json::Object preferences, int expect = 200) {
            auto response = session_call(*playback, "PATCH", id,
                                         Json::Object{{"preferences", Json(std::move(preferences))}});
            REQUIRE(response.status == expect);
            return body_of(response);
        };
        auto created = create_session(*playback, media_id,
                                      Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"video", "copy"}});
        REQUIRE(created.status == 201);
        auto id = session_id_of(created);
        auto to_direct = update(id, Json::Object{{"mode", "direct"}});
        CHECK(to_direct.find("mode")->asString() == "direct");
        CHECK(to_direct.find("preferences")->find("video")->isNull());
        CHECK(to_direct.find("preferences")->find("audio")->isNull());
        id = to_direct.find("session_id")->asString();
        auto to_remux = update(id, Json::Object{{"mode", "remux"}, {"container", "fmp4"}});
        CHECK(to_remux.find("mode")->asString() == "remux");
        CHECK(to_remux.find("output")->find("video")->find("transform")->asString() == "copy");
        CHECK(to_remux.find("output")->find("audio")->find("transform")->asString() == "copy");
        id = to_remux.find("session_id")->asString();
        // An update that names both sets both.
        auto restated = update(id, Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"audio", "copy"}});
        CHECK(restated.find("mode")->asString() == "transcode");
        CHECK(restated.find("output")->find("video")->find("transform")->asString() == "transcode");
        CHECK(restated.find("output")->find("audio")->find("transform")->asString() == "copy");
        session_call(*playback, "DELETE", restated.find("session_id")->asString());
        // A quality instruction also belongs to its mode, so it does not outlive a transcode.
        auto capped = create_session(*playback, media_id,
                                     Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"max_height", 720}});
        REQUIRE(capped.status == 201);
        CHECK(body_of(capped).find("preferences")->find("max_height")->asInt64() == 720);
        auto uncapped = update(session_id_of(capped), Json::Object{{"mode", "direct"}});
        CHECK(uncapped.find("mode")->asString() == "direct");
        CHECK(uncapped.find("preferences")->find("max_height")->isNull());
        session_call(*playback, "DELETE", uncapped.find("session_id")->asString());
    }

    // A cover picture is not a video stream: an MP3 with one direct-plays its audio.
    {
        const auto media_id = node.write("/media/cover.mp3", pattern(4096, 3));
        auto playback = node.playback(node.streaming(), std::make_shared<AttachedPictureAudioEngine>());
        Json::Object capabilities{{"containers", Json(Json::Array{Json("mp3")})},
                                  {"video_codecs", Json(Json::Array{Json("h264")})},
                                  {"audio_codecs", Json(Json::Array{Json("mp3"), Json("aac")})},
                                  {"hls_fmp4", true}};
        auto response = create_session(*playback, media_id, Json::Object{{"mode", "direct"}},
                                       Json::Object{{"capabilities", Json(std::move(capabilities))}});
        REQUIRE(response.status == 201);
        const auto body = body_of(response);
        CHECK(body.find("mode")->asString() == "direct");
        const auto* selection = body.find("selection");
        REQUIRE(selection && selection->isObject());
        CHECK(selection->find("video_stream")->asInt64() == -1);
        CHECK(selection->find("audio_stream")->asInt64() == 1);
    }

    // A seek never starts after the position asked for: seek_ms +
    // seek_offset_ms equals the request exactly, with a non-negative offset.
    {
        const auto media_id = node.write("/media/keyframed.mkv", pattern(64 * 1024, 4));
        auto streaming = node.streaming();
        streaming.startup_timeout = 2s;
        auto playback = node.playback(streaming, std::make_shared<KeyframedRemuxMediaEngine>());
        const auto honoured = [](const Json& payload) {
            REQUIRE(payload.find("seek_ms") != nullptr);
            REQUIRE(payload.find("seek_offset_ms") != nullptr);
            REQUIRE(payload.find("seek_requested_ms") != nullptr);
            const auto seek = payload.find("seek_ms")->asInt64();
            const auto offset = payload.find("seek_offset_ms")->asInt64();
            const auto requested = payload.find("seek_requested_ms")->asInt64();
            CHECK(seek + offset == requested);
            CHECK(offset >= 0);
            return requested;
        };
        const auto create = [&](const char* mode, int64_t seek_ms) {
            auto response = create_session(*playback, media_id, Json::Object{{"mode", mode}, {"container", "fmp4"}},
                                           Json::Object{{"seek_ms", seek_ms}});
            REQUIRE(response.status == 201);
            return body_of(response);
        };
        // Remux: baseline at the last keyframe at or before 23 s, remainder published.
        auto remux = create("remux", 23'000);
        CHECK(remux.find("mode")->asString() == "remux");
        CHECK(honoured(remux) == 23'000);
        CHECK(remux.find("seek_ms")->asInt64() == 20'000);
        CHECK(remux.find("seek_offset_ms")->asInt64() == 3'000);
        // A seek-only PATCH follows the same rule as a create seek, and the
        // mode is never substituted.
        auto patched = session_call(*playback, "PATCH", remux.find("session_id")->asString(),
                                    Json::Object{{"seek_ms", 35'000}});
        REQUIRE(patched.status == 200);
        const auto patched_body = body_of(patched);
        CHECK(honoured(patched_body) == 35'000);
        CHECK(patched_body.find("seek_ms")->asInt64() == 30'000);
        CHECK(patched_body.find("seek_offset_ms")->asInt64() == 5'000);
        CHECK(patched_body.find("mode")->asString() == "remux");
        auto aligned = create("remux", 30'000);
        CHECK(honoured(aligned) == 30'000);
        CHECK(aligned.find("seek_offset_ms")->asInt64() == 0);
        // Transcode is frame-accurate: the offset is always zero.
        auto transcode = create("transcode", 23'000);
        CHECK(transcode.find("mode")->asString() == "transcode");
        CHECK(honoured(transcode) == 23'000);
        CHECK(transcode.find("seek_ms")->asInt64() == 23'000);
        CHECK(transcode.find("seek_offset_ms")->asInt64() == 0);
        // Direct has no generation; the client byte-ranges the source.
        auto direct = create("direct", 23'000);
        CHECK(direct.find("mode")->asString() == "direct");
        CHECK(honoured(direct) == 23'000);
        CHECK(direct.find("seek_offset_ms")->asInt64() == 0);
    }

    // A session reports how far past its last requested fragment media is
    // produced, from this node's max_ahead_segments and segment duration; direct
    // play has no frontier.
    {
        const auto media_id = node.write("/media/look-ahead.mkv", pattern(64 * 1024, 5));
        auto streaming = node.streaming();
        streaming.startup_timeout = 2s;
        // Not the defaults, so the assertion cannot pass by coincidence.
        streaming.max_ahead_segments = 3;
        streaming.segment_duration = 2000ms;
        auto playback = node.playback(streaming, std::make_shared<ObservableHlsMediaEngine>());
        auto transformed = create_session(*playback, media_id, Json::Object{{"mode", "remux"}, {"container", "fmp4"}});
        REQUIRE(transformed.status == 201);
        const auto transformed_body = body_of(transformed);
        const auto* look_ahead = transformed_body.find("stream")->find("look_ahead_ms");
        REQUIRE(look_ahead != nullptr);
        CHECK(look_ahead->asInt64() == 6000);
        auto direct = create_session(*playback, media_id, Json::Object{{"mode", "direct"}, {"container", "fmp4"}});
        REQUIRE(direct.status == 201);
        const auto direct_body = body_of(direct);
        REQUIRE(direct_body.find("stream")->find("look_ahead_ms") != nullptr);
        CHECK(direct_body.find("stream")->find("look_ahead_ms")->isNull());
        // Production figures sit beside look_ahead_ms, and are absent for direct play.
        const auto* production = transformed_body.find("stream")->find("production");
        REQUIRE(production != nullptr);
        REQUIRE(production->find("produced_ms") != nullptr);
        REQUIRE(production->find("producing_ms") != nullptr);
        REQUIRE(production->find("produced_age_ms") != nullptr);
        REQUIRE(production->find("producer_parked") != nullptr);
        CHECK(direct_body.find("stream")->find("production") == nullptr);
    }

    // A URL generation below the current one was replaced (gone, not to be
    // retried, scoped to the request); one above it is an ordinary not-found.
    {
        const auto media_id = node.write("/media/superseded.mp4", pattern(65549, 6));
        auto streaming = node.streaming();
        streaming.session_idle = 5min;
        auto playback = node.playback(streaming, std::make_shared<FakeMediaEngine>());
        auto created = create_session(*playback, media_id, Json::Object{{"mode", "transcode"}, {"container", "fmp4"}});
        REQUIRE(created.status == 201);
        const auto created_body = body_of(created);
        const auto first_url = created_body.find("stream")->find("url")->asString();
        const auto first_generation = created_body.find("generation")->asUInt64();
        auto patched = session_call(*playback, "PATCH", created_body.find("session_id")->asString(),
                                    Json::Object{{"seek_ms", 5'000}});
        REQUIRE(patched.status == 200);
        const auto patched_body = body_of(patched);
        const auto second_generation = patched_body.find("generation")->asUInt64();
        REQUIRE(second_generation > first_generation);

        auto stale = playback->handle(playback_request("GET", first_url));
        REQUIRE(stale.status == 410);
        const auto stale_body = body_of(stale);
        const auto* stale_error = stale_body.find("error");
        REQUIRE(stale_error != nullptr);
        CHECK(stale_error->find("code")->asString() == "generation_superseded");
        CHECK(stale_error->find("scope")->asString() == "request");
        CHECK(stale_error->find("node_healthy")->asBool());
        CHECK(stale_error->find("alternative_may_succeed")->asBool());

        auto future_url = patched_body.find("stream")->find("url")->asString();
        const auto marker = "/" + std::to_string(second_generation) + "/";
        const auto at = future_url.find(marker);
        REQUIRE(at != std::string::npos);
        future_url.replace(at, marker.size(), "/" + std::to_string(second_generation + 99) + "/");
        auto future = playback->handle(playback_request("GET", future_url));
        REQUIRE(future.status == 404);
        CHECK(body_of(future).find("error")->find("code")->asString() == "not_found");
    }

    // A session's own life: its report, direct byte ranges over a snapshot of
    // the file, seek-only and subtitle-only updates that reuse the prepared plan,
    // quality and mode changes, and capabilities that are advisory.
    {
        const auto bytes = pattern(512 * 1024 + 37, 7);
        const auto media_id = node.write("/media/test.mp4", bytes);
        CatalogueApiConfig api;
        api.stream_chunk_bytes = 64 * 1024;
        auto streaming = node.streaming();
        streaming.max_sessions = 4;
        streaming.max_video_transcodes = 1;
        streaming.max_audio_transcodes = 1;
        streaming.startup_timeout = 2s;
        auto engine = std::make_shared<FakeMediaEngine>();
        auto playback = node.playback(streaming, engine, {}, nullptr, api);

        const auto status = playback_status(*playback);
        REQUIRE(status.find("server_version") != nullptr);
        CHECK(status.find("server_version")->asString() == kServerVersion);
        CHECK(status.find("probe_cache_entries")->asUInt64() <=
              status.find("probe_cache_limit_entries")->asUInt64());
        CHECK(status.find("probe_cache_bytes")->asUInt64() <=
              status.find("probe_cache_limit_bytes")->asUInt64());
        for (const auto* field : {"subtitle_cache_entries", "subtitle_cache_bytes",
                                  "segment_store_resident_bytes", "segment_store_spill_bytes",
                                  "segment_store_descriptor_bytes", "segment_store_segments",
                                  "segment_store_planned_segments", "heap_reclaim_pending",
                                  "heap_reclaim_requests", "heap_reclaim_runs", "heap_reclaim_successes"})
            CHECK(status.find(field) != nullptr);

        // A transformed stream can begin at its resume point in the initial POST.
        auto initial_seek = create_session(*playback, media_id, Json::Object{{"mode", "remux"}, {"container", "fmp4"}},
                                           Json::Object{{"seek_ms", 23000}});
        REQUIRE(initial_seek.status == 201);
        const auto initial_seek_body = body_of(initial_seek);
        CHECK(initial_seek_body.find("mode")->asString() == "remux");
        CHECK(initial_seek_body.find("seek_ms")->asInt64() == 23000);
        auto plans = engine->started_plans();
        REQUIRE(plans.size() == 1);
        CHECK(plans.back().seek == 23s);
        const auto seek_id = initial_seek_body.find("session_id")->asString();

        // A seek-only PATCH, even one restating unchanged preferences, reuses the
        // prepared VOD plan without re-probing.
        const auto probes_before_seek = engine->probes();
        const auto prepares_before_seek = engine->vod_prepares();
        auto fast_seek = session_call(*playback, "PATCH", seek_id, Json::Object{{"seek_ms", 35000}});
        REQUIRE(fast_seek.status == 200);
        CHECK(body_of(fast_seek).find("seek_ms")->asInt64() == 35000);
        auto redundant_seek = session_call(
            *playback, "PATCH", seek_id,
            Json::Object{{"seek_ms", 47000},
                         {"preferences", Json(Json::Object{{"mode", "remux"}, {"container", "fmp4"}})}});
        REQUIRE(redundant_seek.status == 200);
        CHECK(body_of(redundant_seek).find("seek_ms")->asInt64() == 47000);
        CHECK(engine->probes() == probes_before_seek);
        CHECK(engine->vod_prepares() == prepares_before_seek);
        plans = engine->started_plans();
        REQUIRE(plans.size() == 3);
        CHECK(plans[1].seek == 35s);
        CHECK(plans[2].seek == 47s);
        CHECK(session_call(*playback, "DELETE", seek_id).status == 204);

        // Reopening the same media with the same transformed plan reuses the
        // probe and the prepared plan; only the pipeline generation is fresh.
        const auto probes_before_reopen = engine->probes();
        const auto prepares_before_reopen = engine->vod_prepares();
        auto reopened = create_session(*playback, media_id, Json::Object{{"mode", "remux"}, {"container", "fmp4"}},
                                       Json::Object{{"seek_ms", 23000}});
        REQUIRE(reopened.status == 201);
        CHECK(engine->probes() == probes_before_reopen);
        CHECK(engine->vod_prepares() == prepares_before_reopen);
        CHECK(session_call(*playback, "DELETE", session_id_of(reopened)).status == 204);

        // Direct: the source, described, its streams offered, served by range,
        // with no HLS plan prepared or started.
        const auto plans_before_direct = engine->started_plans().size();
        auto created = create_session(*playback, media_id, Json::Object{{"mode", "direct"}});
        REQUIRE(created.status == 201);
        const auto created_body = body_of(created);
        CHECK(created_body.find("mode")->asString() == "direct");
        CHECK(created_body.find("stream")->find("subtitle_url")->isNull());
        CHECK(created_body.find("source")->find("format")->asString() == "mov,mp4,m4a,3gp,3g2,mj2");
        CHECK(created_body.find("source")->find("bitrate")->asUInt64() == 4'000'000);
        const auto& source_streams = created_body.find("source")->find("streams")->asArray();
        REQUIRE(source_streams.size() == 4);
        CHECK(source_streams[0].find("bitrate")->asUInt64() == 3'700'000);
        CHECK(source_streams[1].find("bitrate")->asUInt64() == 192'000);
        CHECK(created_body.find("output")->find("video")->find("transform")->asString() == "copy");
        CHECK(created_body.find("output")->find("video")->find("bitrate")->asUInt64() == 3'700'000);
        CHECK(created_body.find("output")->find("audio")->find("transform")->asString() == "copy");
        CHECK(created_body.find("output")->find("audio")->find("bitrate")->asUInt64() == 192'000);
        CHECK(created_body.find("preferences")->find("mode")->asString() == "direct");
        const auto* options = created_body.find("options");
        REQUIRE(options != nullptr);
        REQUIRE(!options->find("audio_streams")->asArray().empty());
        CHECK(options->find("audio_streams")->asArray().front().isObject());
        REQUIRE(options->find("subtitle_streams")->asArray().size() == 1);
        CHECK(options->find("subtitle_streams")->asArray().front().find("index")->asInt64() == 2);
        CHECK(!options->find("quality_heights")->asArray().empty());
        CHECK(std::any_of(options->find("modes")->asArray().begin(), options->find("modes")->asArray().end(),
                          [](const Json& mode) { return mode.asString() == "direct"; }));
        CHECK(engine->vod_prepares() == prepares_before_reopen);
        CHECK(engine->started_plans().size() == plans_before_direct);
        const auto session_id = created_body.find("session_id")->asString();
        const auto direct_url = created_body.find("stream")->find("url")->asString();
        CHECK(direct_url.ends_with("/direct"));
        auto direct = playback_request("GET", direct_url);
        direct.headers["range"] = "bytes=100-1099";
        auto direct_response = playback->handle(direct);
        REQUIRE(direct_response.status == 206);
        REQUIRE(direct_response.stream != nullptr);
        CHECK(direct_response.content_length() == 1000);
        Bytes direct_bytes(1000);
        REQUIRE(direct_response.stream->read(0, direct_bytes) == direct_bytes.size());
        CHECK(std::equal(direct_bytes.begin(), direct_bytes.end(), bytes.begin() + 100));

        // A quality instruction is a re-encode, so it is refused with direct, not ignored.
        CHECK(create_session(*playback, media_id,
                             Json::Object{{"mode", "direct"}, {"max_height", 1},
                                          {"max_bitrate", static_cast<uint64_t>(1)}})
                  .status == 400);

        // A subtitle-only PATCH does not rebuild or seek the A/V generation; the
        // subtitle gets a stream-specific URL so caches cannot serve the old track.
        const auto probes_before_subtitle = engine->probes();
        const auto prepares_before_subtitle = engine->vod_prepares();
        auto subtitle_only = session_call(*playback, "PATCH", session_id,
                                          Json::Object{{"preferences", Json(Json::Object{{"subtitle_stream", 2}})}});
        REQUIRE(subtitle_only.status == 200);
        const auto subtitle_only_body = body_of(subtitle_only);
        CHECK(subtitle_only_body.find("stream")->find("url")->asString() == direct_url);
        CHECK(subtitle_only_body.find("selection")->find("subtitle_stream")->asInt64() == 2);
        const auto selected_subtitle_url = subtitle_only_body.find("stream")->find("subtitle_url")->asString();
        CHECK(selected_subtitle_url.find("/1/subtitle-2/manifest.json") != std::string::npos);
        CHECK(engine->probes() == probes_before_subtitle);
        CHECK(engine->vod_prepares() == prepares_before_subtitle);

        // The manifest describes segments without extracting any; a segment is
        // extracted once and then cached.
        const auto subtitle_segments_before = engine->subtitle_segments();
        auto manifest = playback->handle(playback_request("GET", selected_subtitle_url));
        REQUIRE(manifest.status == 200);
        CHECK(manifest.content_type.starts_with("application/json"));
        CHECK(engine->subtitle_segments() == subtitle_segments_before);
        const auto manifest_body = Json::parse(response_text(manifest));
        CHECK(manifest_body.find("format")->asString() == "macha-webvtt-segments");
        REQUIRE(manifest_body.find("segment_durations_ms")->asArray().size() == 15);
        const auto subtitle_segment_path =
            selected_subtitle_url.substr(0, selected_subtitle_url.rfind('/')) + "/segment-0.vtt";
        auto segment = playback->handle(playback_request("GET", subtitle_segment_path));
        REQUIRE(segment.status == 200);
        CHECK(segment.content_type.starts_with("text/vtt"));
        CHECK(engine->subtitle_segments() == subtitle_segments_before + 1);
        REQUIRE(playback->handle(playback_request("GET", subtitle_segment_path)).status == 200);
        CHECK(engine->subtitle_segments() == subtitle_segments_before + 1);

        // Subtitles off; a bitmap subtitle or a missing track is refused.
        const Json::Object subtitles_off{{"subtitle_stream", Json(nullptr)}, {"subtitle_language", ""}};
        auto subtitle_off = session_call(*playback, "PATCH", session_id,
                                         Json::Object{{"preferences", Json(subtitles_off)}});
        REQUIRE(subtitle_off.status == 200);
        const auto subtitle_off_body = body_of(subtitle_off);
        CHECK(subtitle_off_body.find("stream")->find("url")->asString() == direct_url);
        CHECK(subtitle_off_body.find("selection")->find("subtitle_stream")->asInt64() == -1);
        CHECK(subtitle_off_body.find("stream")->find("subtitle_url")->isNull());
        CHECK(session_call(*playback, "PATCH", session_id,
                           Json::Object{{"preferences", Json(Json::Object{{"subtitle_stream", 3}})}})
                  .status == 400);
        CHECK(session_call(*playback, "PATCH", session_id,
                           Json::Object{{"preferences", Json(Json::Object{{"audio_stream", 99}})}})
                  .status == 400);

        // Requesting 720p rebuilds the session at 720p, every mode still offered.
        auto quality = session_call(
            *playback, "PATCH", session_id,
            Json::Object{{"preferences",
                          Json(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"max_height", 720}})}});
        REQUIRE(quality.status == 200);
        const auto quality_body = body_of(quality);
        CHECK(quality_body.find("mode")->asString() == "transcode");
        CHECK(quality_body.find("preferences")->find("mode")->asString() == "transcode");
        CHECK(quality_body.find("preferences")->find("max_height")->asInt64() == 720);
        CHECK(quality_body.find("output")->find("video")->find("codec")->asString() == "h264");
        CHECK(quality_body.find("output")->find("video")->find("height")->asInt64() == 720);
        for (const auto* mode : {"direct", "remux", "transcode"}) {
            const auto& modes = quality_body.find("options")->find("modes")->asArray();
            CHECK(std::any_of(modes.begin(), modes.end(), [&](const Json& m) { return m.asString() == mode; }));
        }
        auto restore = session_call(
            *playback, "PATCH", session_id,
            Json::Object{{"preferences", Json(Json::Object{{"mode", "direct"}, {"max_height", Json(nullptr)},
                                                           {"max_bitrate", Json(nullptr)}})}});
        REQUIRE(restore.status == 200);
        CHECK(body_of(restore).find("mode")->asString() == "direct");
        CHECK(body_of(restore).find("preferences")->find("max_height")->isNull());

        // Capabilities are advisory: the instruction is performed, not refused.
        // The session above released its slot on leaving transcode, so this is admitted.
        Json::Object unsupported_caps{{"containers", Json::Array{}},
                                      {"video_codecs", Json::Array{Json("vp9")}},
                                      {"audio_codecs", Json::Array{Json("opus")}},
                                      {"hls_fmp4", true}};
        auto unsupported = create_session(*playback, media_id, Json::Object{{"mode", "transcode"}, {"container", "fmp4"}},
                                          Json::Object{{"capabilities", Json(std::move(unsupported_caps))}});
        REQUIRE(unsupported.status == 201);
        REQUIRE(session_call(*playback, "DELETE", session_id_of(unsupported)).status == 204);

        // Into transcode with a seek and a subtitle: AAC keeps the stereo layout.
        auto patched = session_call(
            *playback, "PATCH", session_id,
            Json::Object{{"preferences",
                          Json(Json::Object{{"mode", "transcode"}, {"container", "fmp4"}, {"subtitle_stream", 2}})},
                         {"seek_ms", 12000}});
        REQUIRE(patched.status == 200);
        const auto patched_body = body_of(patched);
        CHECK(patched_body.find("mode")->asString() == "transcode");
        CHECK(patched_body.find("output")->find("video")->find("transform")->asString() == "transcode");
        CHECK(patched_body.find("output")->find("video")->find("codec")->asString() == "h264");
        CHECK(patched_body.find("output")->find("audio")->find("transform")->asString() == "transcode");
        CHECK(patched_body.find("output")->find("audio")->find("channels")->asUInt64() == 2);
        CHECK(patched_body.find("output")->find("audio")->find("bitrate")->asUInt64() == 2 * 64000);
        const auto hls_url = patched_body.find("stream")->find("url")->asString();
        const auto subtitle_url = patched_body.find("stream")->find("subtitle_url")->asString();
        CHECK(response_text(playback->handle(playback_request("GET", hls_url))).find("#EXTM3U") !=
              std::string::npos);
        auto transcoded_manifest = playback->handle(playback_request("GET", subtitle_url));
        REQUIRE(transcoded_manifest.status == 200);
        REQUIRE(!Json::parse(response_text(transcoded_manifest)).find("segment_durations_ms")->asArray().empty());
        auto transcoded_segment = playback->handle(
            playback_request("GET", subtitle_url.substr(0, subtitle_url.rfind('/')) + "/segment-0.vtt"));
        REQUIRE(transcoded_segment.status == 200);
        CHECK(transcoded_segment.content_type.starts_with("text/vtt"));

        // The in-place subtitle path also preserves a live transformed generation.
        const auto plans_before_subtitle_off = engine->started_plans().size();
        auto transformed_off = session_call(*playback, "PATCH", session_id,
                                            Json::Object{{"preferences", Json(subtitles_off)}});
        REQUIRE(transformed_off.status == 200);
        CHECK(body_of(transformed_off).find("stream")->find("url")->asString() == hls_url);
        CHECK(body_of(transformed_off).find("stream")->find("subtitle_url")->isNull());
        CHECK(engine->started_plans().size() == plans_before_subtitle_off);

        // The one transcode slot is held.
        CHECK(create_session(*playback, media_id, Json::Object{{"mode", "transcode"}, {"container", "fmp4"}})
                  .status == 429);
        CHECK(session_call(*playback, "DELETE", session_id).status == 204);

        // A playback lease is a snapshot: replacing the file does not change the
        // bytes of a running direct stream named by path.
        auto by_path = create_session(*playback, "path:/media/test.mp4", Json::Object{{"mode", "direct"}});
        REQUIRE(by_path.status == 201);
        auto replacement = bytes;
        for (auto& byte : replacement) byte ^= 0x5a;
        (void)node.write("/media/test.mp4", replacement);
        auto pinned = playback_request("GET", body_of(by_path).find("stream")->find("url")->asString());
        pinned.headers["range"] = "bytes=0-255";
        auto pinned_response = playback->handle(pinned);
        REQUIRE(pinned_response.status == 206);
        Bytes pinned_bytes(256);
        REQUIRE(pinned_response.stream->read(0, pinned_bytes) == pinned_bytes.size());
        CHECK(std::equal(pinned_bytes.begin(), pinned_bytes.end(), bytes.begin()));
        CHECK(session_call(*playback, "DELETE", session_id_of(by_path)).status == 204);

        // A remux session advertises direct and honours an explicit switch back to it.
        auto remux = create_session(*playback, media_id, Json::Object{{"mode", "remux"}, {"container", "fmp4"}});
        REQUIRE(remux.status == 201);
        const auto remux_body = body_of(remux);
        const auto& remux_modes = remux_body.find("options")->find("modes")->asArray();
        CHECK(std::any_of(remux_modes.begin(), remux_modes.end(),
                          [](const Json& mode) { return mode.asString() == "direct"; }));
        auto switched = session_call(*playback, "PATCH", session_id_of(remux),
                                     Json::Object{{"preferences", Json(Json::Object{{"mode", "direct"}})}});
        REQUIRE(switched.status == 200);
        CHECK(body_of(switched).find("mode")->asString() == "direct");
        CHECK(body_of(switched).find("preferences")->find("mode")->asString() == "direct");
        CHECK(body_of(switched).find("stream")->find("url")->asString().ends_with("/direct"));
        CHECK(session_call(*playback, "DELETE", body_of(switched).find("session_id")->asString()).status == 204);
    }
}

MACHA_TEST("media_playback", test_playback_admission_bounds_slots_sessions_and_accounts) {
    // Each create is its own session and its own entitlement; transcode slots
    // are reserved at admission, bound per node and per account, released by
    // leaving transcode or closing; and one account never sees, touches or
    // collides with another's sessions.
    PlaybackNode node;
    const auto transcode = Json::Object{{"mode", "transcode"}, {"container", "fmp4"}};
    const auto remux = Json::Object{{"mode", "remux"}, {"container", "fmp4"}};

    // A second transcode is refused while the first is still starting: the
    // slot is reserved before the pipeline exists.
    {
        const auto media_id = node.write("/media/reserved.mkv", pattern(64 * 1024, 11));
        auto streaming = node.streaming();
        streaming.max_sessions = 4;
        streaming.max_video_transcodes = 1;
        streaming.max_audio_transcodes = 1;
        streaming.startup_timeout = 2s;
        auto engine = std::make_shared<BlockingMediaEngine>();
        auto playback = node.playback(streaming, engine);
        HttpResponse first;
        std::jthread starting([&] { first = create_session(*playback, media_id, transcode); });
        REQUIRE(wait_until([&] { return engine->starts() == 1; }, 2s));
        CHECK(create_session(*playback, media_id, transcode).status == 429);
        CHECK(engine->starts() == 1);
        engine->release();
        starting.join();
        REQUIRE(first.status == 201);
        CHECK(session_call(*playback, "DELETE", session_id_of(first)).status == 204);
    }

    // One node-wide transcode slot: a second create on the same bearer is a
    // second session, not a replacement; direct needs no encoder; a PATCH out
    // of transcode or a close through the signed stream URL releases the slot.
    {
        const auto media_id = node.write("/media/slot.mp4", pattern(64 * 1024, 12));
        auto streaming = node.streaming();
        streaming.max_sessions = 4;
        streaming.max_video_transcodes = 1;
        streaming.max_audio_transcodes = 1;
        streaming.startup_timeout = 2s;
        auto playback = node.playback(streaming, std::make_shared<FakeMediaEngine>());
        const auto create = [&](const Json::Object& preferences, const std::string& viewer,
                                const std::string& attempt) {
            return create_session(*playback, media_id, preferences, {},
                                  SessionIdentity{.id = viewer, .roles = {"anonymous"}},
                                  {{"idempotency_key", attempt}});
        };
        const auto as = [](const std::string& viewer) { return SessionIdentity{.id = viewer, .roles = {"anonymous"}}; };

        auto first = create(transcode, "ui-player-1", "attempt-1");
        REQUIRE(first.status == 201);
        const auto first_id = session_id_of(first);
        CHECK(playback_status(*playback).find("video_transcodes")->asUInt64() == 1);
        CHECK(playback_status(*playback).find("sessions")->asUInt64() == 1);
        auto second = create(Json::Object{{"mode", "direct"}, {"container", "fmp4"}}, "ui-player-1", "attempt-2");
        REQUIRE(second.status == 201);
        const auto second_id = session_id_of(second);
        CHECK(second_id != first_id);
        CHECK(playback_status(*playback).find("sessions")->asUInt64() == 2);
        CHECK(playback_status(*playback).find("video_transcodes")->asUInt64() == 1);
        for (const auto& id : {first_id, second_id})
            CHECK(session_call(*playback, "GET", id, {}, as("ui-player-1")).status == 200);
        // Bound node-wide: neither another viewer nor the holder gets a second slot.
        CHECK(create(transcode, "ui-player-2", "other-attempt").status == 429);
        CHECK(create(transcode, "ui-player-1", "attempt-3").status == 429);
        REQUIRE(session_call(*playback, "DELETE", second_id, {}, as("ui-player-1")).status == 204);

        // Leaving transcode by PATCH frees the slot.
        auto patched = session_call(*playback, "PATCH", first_id,
                                    Json::Object{{"preferences", Json(Json::Object{{"mode", "direct"}})}},
                                    as("ui-player-1"));
        REQUIRE(patched.status == 200);
        CHECK(playback_status(*playback).find("video_transcodes")->asUInt64() == 0);
        auto other = create(transcode, "ui-player-2", "other-attempt-2");
        REQUIRE(other.status == 201);
        REQUIRE(session_call(*playback, "DELETE", body_of(patched).find("session_id")->asString(), {},
                             as("ui-player-1")).status == 204);

        // A page unloading cannot finish a preflighted DELETE, so the session is
        // closed through its signed stream URL with no bearer (a CORS simple request).
        const auto other_body = body_of(other);
        const auto id = other_body.find("session_id")->asString();
        const auto url = other_body.find("stream")->find("url")->asString();
        const std::string prefix = "/api/v1/playback/sessions/" + id + "/stream/";
        REQUIRE(url.starts_with(prefix));
        const auto token = url.substr(prefix.size(), url.find('/', prefix.size()) - prefix.size());
        REQUIRE(!token.empty());
        const auto close = [&](const std::string& method, const std::string& with_token) {
            HttpRequest request;
            request.method = method;
            request.path = prefix + with_token + "/close";
            CHECK(playback->capability_request(request));
            return playback->handle(request);
        };
        CHECK(close("GET", token).status == 405);
        std::string wrong = token;
        wrong.back() = wrong.back() == '0' ? '1' : '0';
        CHECK(close("POST", wrong).status == 404);
        CHECK(playback_status(*playback).find("sessions")->asUInt64() == 1);
        CHECK(close("POST", token).status == 204);
        CHECK(playback_status(*playback).find("sessions")->asUInt64() == 0);
        CHECK(playback_status(*playback).find("video_transcodes")->asUInt64() == 0);
        CHECK(create(transcode, "ui-player-3", "after-close").status == 201);
        // Idempotent: the session is already gone, which is what was asked for.
        CHECK(close("POST", token).status == 204);
    }

    // Accounts: a listing of one's own sessions with the cap stated, owner
    // checks answering 404, ownership kept across a replacement, a session cap
    // and a transcode cap with their own codes, and idempotency keys scoped to
    // their account.
    {
        const auto media_id = node.write("/media/accounts.mp4", pattern(64 * 1024, 13));
        const auto viewer = [](std::string_view user) {
            return SessionIdentity{.id = std::string(user) + "-auth", .roles = {"media_viewer"},
                                   .user_id = std::string(user)};
        };
        const auto alice = viewer("alice");
        const auto bob = viewer("bob");
        const auto manager = [&](size_t per_account) {
            auto streaming = node.streaming();
            streaming.startup_timeout = 2s;
            streaming.max_sessions = 16;
            streaming.max_sessions_per_account = per_account;
            streaming.max_video_transcodes = 4;
            streaming.max_audio_transcodes = 4;
            return node.playback(streaming, std::make_shared<FakeMediaEngine>());
        };
        const auto list = [](PlaybackManager& playback, const SessionIdentity& who) {
            auto response = playback.handle(playback_request("GET", "/api/v1/playback/sessions", {}, who));
            REQUIRE(response.status == 200);
            return body_of(response);
        };

        {
            auto playback = manager(8);
            auto first = create_session(*playback, media_id, remux, {}, alice);
            REQUIRE(first.status == 201);
            auto second = create_session(*playback, media_id, remux, {}, alice);
            REQUIRE(second.status == 201);
            auto theirs = create_session(*playback, media_id, remux, {}, bob);
            REQUIRE(theirs.status == 201);
            const auto listed = list(*playback, alice);
            std::set<std::string> ids;
            for (const auto& entry : listed.find("items")->asArray())
                ids.insert(entry.find("session_id")->asString());
            CHECK((ids == std::set<std::string>{session_id_of(first), session_id_of(second)}));
            CHECK(listed.find("account")->find("sessions")->asUInt64() == 2);
            CHECK(listed.find("account")->find("max_sessions")->asUInt64() == 8);
            CHECK(list(*playback, bob).find("items")->asArray().size() == 1);

            // Another account gets 404, not 403, so it cannot learn the id exists.
            const auto id = session_id_of(first);
            CHECK(session_call(*playback, "GET", id, {}, bob).status == 404);
            CHECK(session_call(*playback, "PATCH", id, Json::Object{{"seek_ms", 1000}}, bob).status == 404);
            CHECK(session_call(*playback, "DELETE", id, {}, bob).status == 404);
            CHECK(session_call(*playback, "GET", id, {}, alice).status == 200);
            CHECK(session_call(*playback, "DELETE", id, {}, alice).status == 204);

            // A replaced session keeps its account: reachable, listed and counted once.
            auto switched = session_call(*playback, "PATCH", session_id_of(second),
                                         Json::Object{{"preferences", Json(transcode)}}, alice);
            REQUIRE(switched.status == 200);
            const auto replaced = body_of(switched).find("session_id")->asString();
            CHECK(session_call(*playback, "GET", replaced, {}, alice).status == 200);
            const auto after = list(*playback, alice);
            CHECK(after.find("items")->asArray().size() == 1);
            CHECK(after.find("account")->find("sessions")->asUInt64() == 1);
            CHECK(session_call(*playback, "GET", replaced, {}, bob).status == 404);

            // Keys are client-chosen and predictable, so one account cannot turn
            // another's retry into a 409; within one account it replays.
            auto squatted = create_session(*playback, media_id, remux, {}, bob, {{"idempotency_key", "retry-1"}});
            REQUIRE(squatted.status == 201);
            auto mine = create_session(*playback, media_id, remux, {}, alice, {{"idempotency_key", "retry-1"}});
            REQUIRE(mine.status == 201);
            CHECK(session_id_of(mine) != session_id_of(squatted));
            auto replayed = create_session(*playback, media_id, remux, {}, alice, {{"idempotency_key", "retry-1"}});
            REQUIRE(replayed.status == 201);
            CHECK(session_id_of(replayed) == session_id_of(mine));
        }
        {
            // The account session cap is the same on every node: scope request.
            auto playback = manager(2);
            REQUIRE(create_session(*playback, media_id, remux, {}, alice).status == 201);
            REQUIRE(create_session(*playback, media_id, remux, {}, alice).status == 201);
            auto refused = create_session(*playback, media_id, remux, {}, alice);
            REQUIRE(refused.status == 429);
            const auto refusal = body_of(refused);
            const auto* error = refusal.find("error");
            CHECK(error->find("code")->asString() == "account_session_limit");
            CHECK(error->find("scope")->asString() == "request");
            CHECK(error->find("node_healthy")->asBool() == true);
            CHECK(error->find("sessions")->asUInt64() == 2);
            CHECK(error->find("max_sessions")->asUInt64() == 2);
            CHECK(create_session(*playback, media_id, remux, {}, bob).status == 201);
            const auto listed = list(*playback, alice);
            const auto id = listed.find("items")->asArray().front().find("session_id")->asString();
            REQUIRE(session_call(*playback, "DELETE", id, {}, alice).status == 204);
            CHECK(create_session(*playback, media_id, remux, {}, alice).status == 201);
        }
        {
            // Entitlements are per session, so a per-account bound stops one
            // account taking every transcode slot: the node allows four, one account two.
            auto playback = manager(32);
            const auto first = create_session(*playback, media_id, transcode, {}, alice);
            REQUIRE(first.status == 201);
            REQUIRE(create_session(*playback, media_id, transcode, {}, alice).status == 201);
            auto refused = create_session(*playback, media_id, transcode, {}, alice);
            REQUIRE(refused.status == 429);
            const auto refusal = body_of(refused);
            const auto* error = refusal.find("error");
            CHECK(error->find("code")->asString() == "account_transcode_limit");
            CHECK(error->find("scope")->asString() == "request");
            CHECK(error->find("node_healthy")->asBool() == true);
            CHECK(error->find("alternative_may_succeed")->asBool() == true);
            CHECK(error->find("transcodes")->asUInt64() == 2);
            CHECK(error->find("max_transcodes")->asUInt64() == 2);
            CHECK(create_session(*playback, media_id, remux, {}, alice).status == 201);
            CHECK(create_session(*playback, media_id, transcode, {}, bob).status == 201);
            const auto listed = list(*playback, alice);
            CHECK(listed.find("account")->find("transcodes")->asUInt64() == 2);
            CHECK(listed.find("account")->find("max_transcodes")->asUInt64() == 2);
            REQUIRE(session_call(*playback, "DELETE", session_id_of(first), {}, alice).status == 204);
            CHECK(create_session(*playback, media_id, transcode, {}, alice).status == 201);
        }
    }
}

MACHA_TEST("media_playback", test_playback_probes_once_and_replays_idempotent_creates) {
    // The probe is stage-specific and node-healthy when it fails; a profile is
    // immutable per media identity and survives a cold manager; concurrent
    // misses and keyed retries share one probe and one session; a failed
    // creation releases its joiners and reservation; and the profile queue
    // never gates admission.
    PlaybackNode node;
    const auto direct = Json::Object{{"mode", "direct"}};
    const auto keyed = [](std::string key) {
        return std::map<std::string, std::string, std::less<>>{{"idempotency_key", std::move(key)}};
    };

    // One error envelope: error.code is the discriminator, error.message for a
    // human, every other detail beside them.
    {
        const auto media_id = node.write("/media/probe-fails.mp4", pattern(64 * 1024, 21));
        auto streaming = node.streaming();
        streaming.probe_timeout = 2s;
        auto playback = node.playback(streaming, std::make_shared<FailingProbeMediaEngine>());
        auto request = playback_request("POST", "/api/v1/playback/sessions", Json::Object{{"media_id", media_id}});
        auto response = playback->handle(request);
        REQUIRE(response.status == 503);
        const auto body = body_of(response);
        const auto* error = body.find("error");
        REQUIRE(error != nullptr);
        CHECK(error->find("code")->asString() == "playback_probe_failed");
        CHECK(!error->find("message")->asString().empty());
        CHECK(error->find("stage")->asString() == "probe");
        CHECK(!error->find("trace")->asString().empty());
        // A probe failure is one title's problem; the node is fit for every other.
        CHECK(error->find("node_healthy")->asBool());
    }

    // A distinct manager with an empty process-local cache answers from the
    // catalogue's immutable profile without probing; replacing the file changes
    // its identity, so the old profile cannot hit it.
    {
        const auto first_media_id = node.write("/media/profile.mp4", pattern(128 * 1024 + 37, 22));
        Json first_body;
        {
            auto engine = std::make_shared<FakeMediaEngine>();
            auto playback = node.playback(node.streaming(), engine);
            auto created = create_session(*playback, first_media_id, direct, {}, anonymous, keyed(first_media_id));
            REQUIRE(created.status == 201);
            CHECK(engine->probes() == 1);
            CHECK(idempotency_of(created) == "created");
            first_body = body_of(created);
        }
        REQUIRE(node.catalogue().media_profile(first_media_id).has_value());

        // The profile endpoint serves it immutably with the size this node finds;
        // a size it cannot find is null and the answer is not cached.
        auto media_size = [&](const std::string& media_id) -> std::optional<uint64_t> {
            auto found = node.filesystem().find_media(media_id);
            if (!found) return std::nullopt;
            return found->second.size;
        };
        CatalogueApi catalogue_api(node.catalogue(), node.hints(), {}, {}, {}, std::chrono::hours(24 * 30),
                                   media_size);
        auto profile_request = playback_request("GET", "/api/v1/catalogue/media/" + first_media_id + "/profile");
        auto response = catalogue_api.handle(profile_request);
        REQUIRE(response.status == 200);
        CHECK(response.headers.at("Cache-Control").find("immutable") != std::string::npos);
        const auto profile = body_of(response);
        CHECK(profile.find("media_id")->asString() == first_media_id);
        CHECK(profile.find("size")->asUInt64() == node.filesystem().find_media(first_media_id)->second.size);
        CHECK(profile.find("format")->asString() == "mov,mp4,m4a,3gp,3g2,mj2");
        CHECK(profile.find("duration_ms")->asUInt64() == 60'000);
        REQUIRE(profile.find("streams")->asArray().size() == 4);
        CatalogueApi sizeless_api(node.catalogue(), node.hints());
        auto sizeless = sizeless_api.handle(profile_request);
        REQUIRE(sizeless.status == 200);
        CHECK(sizeless.headers.at("Cache-Control") == "private, no-cache");
        CHECK(body_of(sizeless).find("size")->isNull());

        {
            auto engine = std::make_shared<FakeMediaEngine>();
            auto playback = node.playback(node.streaming(), engine);
            auto created = create_session(*playback, first_media_id, direct, {}, anonymous, keyed(first_media_id));
            REQUIRE(created.status == 201);
            CHECK(engine->probes() == 0);
            const auto cached_body = body_of(created);
            CHECK(cached_body.find("session_id")->asString() == first_body.find("session_id")->asString());
            CHECK(cached_body.find("generation")->dump() == first_body.find("generation")->dump());
            for (const auto* field : {"mode", "media_id", "duration_ms", "preferences",
                                      "selection", "source", "output", "options"}) {
                REQUIRE(first_body.find(field) != nullptr);
                REQUIRE(cached_body.find(field) != nullptr);
                CHECK(first_body.find(field)->dump() == cached_body.find(field)->dump());
            }
        }
        auto replacement = pattern(128 * 1024 + 41, 22);
        for (auto& byte : replacement) byte ^= 0x5a;
        const auto second_media_id = node.write("/media/profile.mp4", replacement);
        REQUIRE(second_media_id != first_media_id);
        auto engine = std::make_shared<FakeMediaEngine>();
        auto playback = node.playback(node.streaming(), engine);
        REQUIRE(create_session(*playback, second_media_id, direct, {}, anonymous, keyed(second_media_id)).status == 201);
        CHECK(engine->probes() == 1);
    }

    // Two concurrent keyed creates share one probe and one session; a replay
    // answers it; the same key with a different body is a conflict until the
    // session is deleted.
    {
        const auto media_id = node.write("/media/coalesce.mp4", pattern(64 * 1024 + 19, 23));
        TestGate gate;
        auto engine = std::make_shared<CoalescingProbeMediaEngine>(gate);
        auto streaming = node.streaming();
        streaming.probe_timeout = 2s;
        auto playback = node.playback(streaming, engine);
        HttpResponse first, second;
        std::jthread a([&] { first = create_session(*playback, media_id, direct, {}, anonymous, keyed("coalesced-create-1")); });
        REQUIRE(gate.wait_for_entries(1));
        std::jthread b([&] { second = create_session(*playback, media_id, direct, {}, anonymous, keyed("coalesced-create-1")); });
        std::this_thread::sleep_for(50ms);
        CHECK(gate.entered() == 1);
        gate.open();
        a.join();
        b.join();
        CHECK(first.status == 201);
        CHECK(second.status == 201);
        CHECK(engine->probes() == 1);
        CHECK(body_of(first).find("session_id")->dump() == body_of(second).find("session_id")->dump());
        CHECK(body_of(first).find("generation")->dump() == body_of(second).find("generation")->dump());
        CHECK(idempotency_of(first) == "created");
        CHECK(idempotency_of(second) == "replayed");

        auto replay = create_session(*playback, media_id, direct, {}, anonymous, keyed("coalesced-create-1"));
        REQUIRE(replay.status == 201);
        CHECK(idempotency_of(replay) == "replayed");
        CHECK(session_id_of(replay) == session_id_of(first));
        CHECK(engine->probes() == 1);

        auto conflict = create_session(*playback, media_id, direct, Json::Object{{"seek_ms", 1000}}, anonymous,
                                       keyed("coalesced-create-1"));
        REQUIRE(conflict.status == 409);
        CHECK(std::string(conflict.body.begin(), conflict.body.end()).find("idempotency_conflict") !=
              std::string::npos);
        CHECK(session_call(*playback, "DELETE", session_id_of(first)).status == 204);
        auto reused = create_session(*playback, media_id, direct, Json::Object{{"seek_ms", 1000}}, anonymous,
                                     keyed("coalesced-create-1"));
        REQUIRE(reused.status == 201);
        CHECK(idempotency_of(reused) == "created");
        CHECK(engine->probes() == 1);
        // Publication of the probed profile is asynchronous.
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 2s));
    }

    // A failed keyed creation releases its joiners and its reservation: with
    // max_sessions 1, reaching a second probe proves both were released.
    {
        const auto media_id = node.write("/media/failing.mp4", pattern(64 * 1024 + 7, 24));
        TestGate gate;
        auto engine = std::make_shared<CoalescingProbeMediaEngine>(gate, true);
        auto streaming = node.streaming();
        streaming.probe_timeout = 2s;
        streaming.max_sessions = 1;
        auto playback = node.playback(streaming, engine);
        HttpResponse first, second;
        std::jthread a([&] { first = create_session(*playback, media_id, direct, {}, anonymous, keyed("failing-create-1")); });
        REQUIRE(gate.wait_for_entries(1));
        std::jthread b([&] { second = create_session(*playback, media_id, direct, {}, anonymous, keyed("failing-create-1")); });
        std::this_thread::sleep_for(30ms);
        gate.open();
        a.join();
        b.join();
        CHECK(first.status == 503);
        CHECK(second.status == 503);
        CHECK(engine->probes() == 1);
        CHECK(create_session(*playback, media_id, direct, {}, anonymous, keyed("failing-create-1")).status == 503);
        CHECK(engine->probes() == 2);
    }

    // The profile endpoint's pending answer (202) does not gate negotiation:
    // the create produces the profile through the same shared flight, and
    // publication stays asynchronous and outside admission.
    {
        const auto media_id = node.write("/media/pending.mp4", pattern(32 * 1024 + 3, 25));
        auto engine = std::make_shared<FakeMediaEngine>();
        auto information = node.information(engine);
        std::atomic_uint queue_requests{};
        auto queue = [&](const std::vector<std::string>& media_ids) {
            ++queue_requests;
            return information->request(media_ids, MediaInformationPriority::requested, "media-information-api");
        };
        CatalogueApi catalogue_api(node.catalogue(), node.hints(), {}, queue);
        auto pending = catalogue_api.handle(playback_request("GET", "/api/v1/catalogue/media/" + media_id + "/profile"));
        REQUIRE(pending.status == 202);
        CHECK(pending.headers.at("Retry-After") == "1");
        auto playback = node.playback(node.streaming(), engine, queue, information.get());
        auto admitted = create_session(*playback, media_id, direct, {}, anonymous, keyed("pending-profile-create"));
        REQUIRE(admitted.status == 201);
        CHECK(idempotency_of(admitted) == "created");
        CHECK(engine->probes() == 1);
        CHECK(queue_requests.load() == 1);
        CHECK(!node.catalogue().media_profile(media_id).has_value());
        information->start();
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 2s));
        playback->stop();
        information->stop();
    }

    // A profile queue that cannot take the job lets playback use its bounded
    // media-engine fallback, without asking it.
    {
        const auto media_id = node.write("/media/fallback.mp4", pattern(32 * 1024 + 5, 26));
        std::atomic_uint queue_attempts{};
        auto unavailable = [&](const std::vector<std::string>&) {
            ++queue_attempts;
            return size_t{0};
        };
        auto engine = std::make_shared<FakeMediaEngine>();
        auto playback = node.playback(node.streaming(), engine, unavailable);
        auto admitted = create_session(*playback, media_id, direct, {}, anonymous, keyed("unavailable-profile-fallback"));
        REQUIRE(admitted.status == 201);
        CHECK(idempotency_of(admitted) == "created");
        CHECK(queue_attempts.load() == 0);
        CHECK(engine->probes() == 1);
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 2s));
    }

    // A profile job that went pending then failed: the create falls back to the
    // engine once and a retry replays rather than probing again.
    {
        const auto media_id = node.write("/media/retry.mp4", pattern(32 * 1024 + 7, 27));
        std::atomic_uint requests{};
        auto pending_then_failed = [&](const std::vector<std::string>&) {
            return ++requests == 1 ? size_t{1} : size_t{0};
        };
        CatalogueApi catalogue_api(node.catalogue(), node.hints(), {}, pending_then_failed);
        REQUIRE(catalogue_api.handle(playback_request("GET", "/api/v1/catalogue/media/" + media_id + "/profile"))
                    .status == 202);
        auto engine = std::make_shared<FakeMediaEngine>();
        auto playback = node.playback(node.streaming(), engine, pending_then_failed);
        auto admitted = create_session(*playback, media_id, direct, {}, anonymous, keyed("failed-profile-retry"));
        REQUIRE(admitted.status == 201);
        CHECK(idempotency_of(admitted) == "created");
        CHECK(engine->probes() == 1);
        CHECK(requests.load() == 1);
        auto replayed = create_session(*playback, media_id, direct, {}, anonymous, keyed("failed-profile-retry"));
        REQUIRE(replayed.status == 201);
        CHECK(idempotency_of(replayed) == "replayed");
        CHECK(engine->probes() == 1);
        CHECK(body_of(replayed).find("session_id")->dump() == body_of(admitted).find("session_id")->dump());
        CHECK(body_of(replayed).find("generation")->dump() == body_of(admitted).find("generation")->dump());
    }
}

MACHA_TEST("media_playback", test_media_information_schedules_scans_and_publishes_profiles) {
    // MediaInformationService runs requested work before background work,
    // shares one scan between foreground requests, lets playback take over a
    // speculative scan, prunes a profile only with the last live copy, and
    // retries publication without rescanning.
    PlaybackNode node;

    {
        const auto low_a = node.write("/media/low-a.mp4", pattern(32769, 31));
        const auto low_b = node.write("/media/low-b.mp4", pattern(32771, 31));
        const auto requested = node.write("/media/requested.mp4", pattern(32773, 31));
        auto engine = std::make_shared<PriorityMediaInformationEngine>();
        auto information = node.information(engine);
        REQUIRE(information->request_path("/media/low-a.mp4", MediaInformationPriority::background));
        REQUIRE(information->request_path("/media/low-b.mp4", MediaInformationPriority::background));
        REQUIRE(information->request_path("/media/requested.mp4", MediaInformationPriority::requested,
                                          "media-information-request"));
        information->start();
        REQUIRE(wait_until([&] {
            return node.catalogue().media_profile(low_a).has_value() &&
                   node.catalogue().media_profile(low_b).has_value() &&
                   node.catalogue().media_profile(requested).has_value();
        }, 5s));
        const auto order = engine->order();
        REQUIRE(order.size() == 3);
        CHECK(order.front() == requested);
        information->stop();
    }

    {
        const std::string path = "/media/single-flight.mp4";
        const auto media_id = node.write(path, pattern(65539, 32));
        const auto entry = node.filesystem().getattr(path);
        TestGate gate;
        auto engine = std::make_shared<CoalescingProbeMediaEngine>(gate);
        auto information = node.information(engine);
        information->start();
        std::optional<MediaProbeResult> first, second;
        std::jthread a([&] { first = information->resolve_playback(media_id, path, entry, Clock::now() + 2s); });
        REQUIRE(gate.wait_for_entries(1));
        std::jthread b([&] { second = information->resolve_playback(media_id, path, entry, Clock::now() + 2s); });
        std::this_thread::sleep_for(30ms);
        CHECK(engine->probes() == 1);
        gate.open();
        a.join();
        b.join();
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);
        CHECK(engine->probes() == 1);
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 2s));
        information->stop();
    }

    {
        const std::string path = "/media/takeover.mp4";
        const auto media_id = node.write(path, pattern(65541, 33));
        const auto entry = node.filesystem().getattr(path);
        auto engine = std::make_shared<PriorityMediaInformationEngine>(true);
        auto information = node.information(engine);
        REQUIRE(information->request_path(path));
        information->start();
        REQUIRE(wait_until([&] { return engine->starts() == 1; }, 2s));
        auto resolved = information->resolve_playback(media_id, path, entry, Clock::now() + 2s);
        CHECK(!resolved.streams.empty());
        CHECK(engine->starts() == 2);
        CHECK(engine->cancellations() == 1);
        CHECK(engine->completions() == 1);
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 2s));
        information->stop();
    }

    {
        const auto bytes = pattern(65543, 34);
        const auto media_id = node.write("/media/copy-a.mp4", bytes);
        REQUIRE(node.write("/media/copy-b.mp4", bytes) == media_id);
        auto engine = std::make_shared<PriorityMediaInformationEngine>();
        auto information = node.information(engine);
        REQUIRE(information->request_path("/media/copy-a.mp4"));
        information->start();
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 2s));
        node.filesystem().unlink("/media/copy-a.mp4");
        information->request_prune();
        std::this_thread::sleep_for(100ms);
        CHECK(node.catalogue().media_profile(media_id).has_value());
        node.filesystem().unlink("/media/copy-b.mp4");
        information->request_prune();
        REQUIRE(wait_until([&] { return !node.catalogue().media_profile(media_id).has_value(); }, 2s));
        information->stop();
    }

    {
        const std::string path = "/media/retry-publication.mp4";
        const auto media_id = node.write(path, pattern(65547, 35));
        auto engine = std::make_shared<PriorityMediaInformationEngine>();
        std::atomic_uint publication_attempts{};
        auto information = node.information(engine, [&](std::string id, MediaProbeResult profile) {
            if (++publication_attempts == 1) throw std::runtime_error("synthetic catalogue conflict");
            node.catalogue().put_media_profile(id, std::move(profile));
        });
        REQUIRE(information->request_path(path));
        information->start();
        REQUIRE(wait_until([&] { return node.catalogue().media_profile(media_id).has_value(); }, 3s));
        CHECK(publication_attempts.load() == 2);
        CHECK(engine->starts() == 1);
        CHECK(engine->completions() == 1);
        information->stop();
    }
}

namespace {
// A fragment request run as HttpServer runs it, one step at a time: a held
// request comes back deferred with its hold, and the test decides whether it
// resumes woken or at its deadline.
class FragmentRequest {
    PlaybackManager& playback_;
    HttpRequest request_;
    HttpResponse response_;

  public:
    FragmentRequest(PlaybackManager& playback, std::string path)
        : playback_(playback), request_(playback_request("GET", std::move(path))),
          response_(playback_.handle(request_)) {}

    const HttpResponse& response() const { return response_; }
    bool held() const { return response_.defer.has_value(); }
    Clock::time_point deadline() const { return response_.defer->deadline; }
    // True once the store has fired the deferral's waker.
    bool woken() const {
        auto fired = std::make_shared<std::atomic_bool>();
        response_.defer->waker->arm([fired] { fired->store(true); });
        return fired->load();
    }
    // Re-runs the request as the server does when woken or once its deadline
    // has passed; the hold goes with the deferral unless it defers again.
    const HttpResponse& resume(bool deadline_passed) {
        REQUIRE(held());
        auto deferral = std::move(*response_.defer);
        request_.resumed = true;
        request_.resumed_state = std::move(deferral.state);
        request_.resume_deadline = deadline_passed ? Clock::now() - 1ms : deferral.deadline;
        response_ = playback_.handle(request_);
        request_.resumed_state.reset();
        return response_;
    }
};
} // namespace

MACHA_TEST("media_playback", test_playback_holds_admitted_fragment_requests_and_refuses_the_rest_at_once) {
    // A fragment request is served if produced, held if admitted (one in
    // flight plus one prefetch per session, inside the hold window), and
    // otherwise refused at once as a retryable 500 that never advances the
    // producer. A held request costs no thread, is woken by publication, and
    // answers retryably at its deadline. Control traffic answers while holds
    // and subtitle extractions are outstanding.
    PlaybackNode node;
    const auto remux = Json::Object{{"mode", "remux"}, {"container", "fmp4"}};
    const auto stream_base = [](const HttpResponse& created) {
        const auto url = body_of(created).find("stream")->find("url")->asString();
        return url.substr(0, url.rfind('/') + 1);
    };
    const auto not_ready = [](const HttpResponse& response, std::string_view reason) {
        CHECK(!response.defer.has_value());
        CHECK(response.status == 500);
        const auto text = std::string(response.body.begin(), response.body.end());
        CHECK(text.find("segment_not_ready") != std::string::npos);
        CHECK(text.find(reason) != std::string::npos);
        CHECK(response.headers.at("Retry-After") == "1");
    };

    {
        const auto media_id = node.write("/media/window.mkv", pattern(64 * 1024, 41));
        auto streaming = node.streaming();
        streaming.startup_timeout = 2s;
        streaming.segment_timeout = 400ms;
        streaming.segment_hold_window = 8;
        auto engine = std::make_shared<ObservableHlsMediaEngine>();
        auto playback = node.playback(streaming, engine);
        auto created = create_session(*playback, media_id, remux);
        REQUIRE(created.status == 201);
        const auto base = stream_base(created);
        auto store = engine->store();
        REQUIRE(store != nullptr);
        REQUIRE(store->snapshot().segment_count == 1);
        REQUIRE(store->snapshot().planned_segments == 15);

        // Produced already: served with no admission at all.
        CHECK(FragmentRequest(*playback, base + segment_name(0)).response().status == 200);

        // Beyond the window (frontier one fragment, window eight): refused at
        // once, and the producer is not told.
        FragmentRequest refused(*playback, base + segment_name(9));
        not_ready(refused.response(), "beyond_hold_window");
        CHECK(refused.response().headers.at("Cache-Control") == "no-store");
        CHECK(store->snapshot().highest_requested == 0);
        // Past the end of the plan is a genuine miss: 404, not "come back later".
        CHECK(FragmentRequest(*playback, base + segment_name(99)).response().status == 404);
        CHECK(store->snapshot().highest_requested == 0);

        // Inside the window: held until the segment timeout, admitted (so the
        // frontier moves), and served when the fragment arrives.
        const auto asked = Clock::now();
        FragmentRequest held(*playback, base + segment_name(1));
        REQUIRE(held.held());
        CHECK(held.deadline() >= asked + streaming.segment_timeout);
        CHECK(held.deadline() <= Clock::now() + streaming.segment_timeout);
        CHECK(store->snapshot().highest_requested == 1);
        CHECK(!held.woken());
        // Fragment 0 is four bytes, so eleven bytes identifies fragment 1.
        REQUIRE(store->publish_segment(Bytes(11, 0x31), 4.0));
        CHECK(held.woken());
        const auto& served = held.resume(false);
        CHECK(served.status == 200);
        CHECK(served.content_length() == 11);

        // A held request that reaches its deadline answers retryably.
        FragmentRequest timed_out(*playback, base + segment_name(2));
        REQUIRE(timed_out.held());
        not_ready(timed_out.resume(true), "hold_timed_out");
    }

    {
        // A deeply prefetching client gets its share of holds and no more.
        const auto media_id = node.write("/media/prefetch.mkv", pattern(64 * 1024, 42));
        auto streaming = node.streaming();
        streaming.startup_timeout = 2s;
        streaming.max_session_holds = 2;
        streaming.max_concurrent_holds = 8;
        streaming.segment_hold_window = 8;
        auto engine = std::make_shared<ObservableHlsMediaEngine>();
        auto playback = node.playback(streaming, engine);
        auto created = create_session(*playback, media_id, remux);
        REQUIRE(created.status == 201);
        const auto base = stream_base(created);
        // The playlist is complete and closed before a second fragment exists.
        FragmentRequest playlist(*playback, base + "media.m3u8");
        REQUIRE(playlist.response().status == 200);
        const auto text = response_text(playlist.response());
        CHECK(text.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos);
        CHECK(text.find("#EXT-X-ENDLIST") != std::string::npos);
        CHECK(text.find(segment_name(14)) != std::string::npos);

        // Two requests for unproduced fragments are the session's whole share;
        // the third is refused immediately, naming the limit it met.
        FragmentRequest first(*playback, base + segment_name(1));
        FragmentRequest second(*playback, base + segment_name(2));
        REQUIRE(first.held());
        REQUIRE(second.held());
        not_ready(FragmentRequest(*playback, base + segment_name(3)).response(), "session_hold_limit");
        // Control traffic answers while both holds are outstanding (law 1).
        CHECK(playback_status(*playback).find("sessions")->asUInt64() == 1);

        // Both reach their deadline and answer retryably, not as missing, and
        // their release returns the session's share.
        not_ready(first.resume(true), "hold_timed_out");
        not_ready(second.resume(true), "hold_timed_out");
        auto store = engine->store();
        REQUIRE(store != nullptr);
        FragmentRequest again(*playback, base + segment_name(1));
        REQUIRE(again.held());
        REQUIRE(store->publish_segment(Bytes(11, 0x31), 4.0));
        const auto& served = again.resume(false);
        CHECK(served.status == 200);
        CHECK(served.content_length() == 11);
    }

    {
        // A slow subtitle extraction holds only its session's subtitle cache:
        // status reads it with try_lock, and an unrelated create is not stuck
        // behind it.
        const auto media_id = node.write("/media/subtitled.mp4", pattern(65549, 43));
        TestGate gate;
        auto playback = node.playback(node.streaming(), std::make_shared<GatedSubtitleMediaEngine>(gate));
        const auto preferences = Json::Object{{"mode", "remux"}, {"container", "fmp4"}, {"subtitle_stream", 2}};
        auto created = create_session(*playback, media_id, preferences);
        REQUIRE(created.status == 201);
        const auto subtitle_url = body_of(created).find("stream")->find("subtitle_url")->asString();
        HttpResponse segment_response;
        std::jthread segment_request([&] {
            segment_response = playback->handle(
                playback_request("GET", subtitle_url.substr(0, subtitle_url.rfind('/')) + "/segment-0.vtt"));
        });
        REQUIRE(gate.wait_for_entries(1));

        const auto status = playback_status(*playback);
        CHECK(status.find("sessions")->asUInt64() == 1);
        CHECK(status.find("subtitle_cache_entries")->asUInt64() == 0);
        CHECK(create_session(*playback, media_id, preferences).status == 201);

        gate.open();
        segment_request.join();
        REQUIRE(segment_response.status == 200);
        CHECK(segment_response.content_type.starts_with("text/vtt"));
    }
}

MACHA_TEST("media_playback", test_playback_idle_clocks_release_what_is_not_used) {
    // Each idle clock releases only what it governs: an abandoned pipeline is
    // reclaimed while its session keeps the entitlement; an entitlement lapses
    // without stream fetches although session polls keep the session; a session
    // never streamed from is reclaimed on its short clock, one stream fetch
    // moving it to the long one; and creating a session wakes the otherwise
    // blocked cleanup worker. Time is stepped, so no margin depends on the host.
    PlaybackNode node;
    const auto transcode = Json::Object{{"mode", "transcode"}, {"container", "fmp4"}};
    SteppedTime time;
    // Runs one cleanup pass at the current time: reconfiguring wakes the
    // worker, whose pass begins by reading the time and decides under the lock
    // every later request takes.
    const auto cleanup_pass = [&](PlaybackManager& playback, const StreamingConfig& streaming) {
        const auto reads = time.reads();
        playback.reconfigure(streaming);
        REQUIRE(wait_until([&] { return time.reads() > reads; }, 5s));
    };
    const auto count = [](PlaybackManager& playback, const char* field) {
        return playback_status(playback).find(field)->asUInt64();
    };

    {
        const auto media_id = node.write("/media/abandoned.mp4", pattern(65549, 51));
        auto streaming = node.streaming();
        streaming.max_video_transcodes = 1;
        streaming.video_decoder_threads = 3;
        streaming.pipeline_idle = 500ms;
        streaming.session_idle = 5min;
        auto playback = node.playback(streaming, std::make_shared<FakeMediaEngine>(), {}, nullptr, {}, time);
        auto first = create_session(*playback, media_id, transcode);
        REQUIRE(first.status == 201);
        auto stale_stream = body_of(first).find("stream")->find("url")->asString();
        const auto current_stream = stale_stream;
        const auto generation = stale_stream.find("/1/master.m3u8");
        REQUIRE(generation != std::string::npos);
        stale_stream.replace(generation, std::string("/1/master.m3u8").size(), "/0/master.m3u8");
        CHECK(count(*playback, "sessions") == 1);
        CHECK(count(*playback, "video_transcodes") == 1);
        CHECK(count(*playback, "video_decoder_threads") == 3);

        // Current-generation traffic renews the pipeline lease: 800 ms on, the
        // last fetch is 400 ms old and the pipeline still runs.
        time.advance(400ms);
        CHECK(playback->handle(playback_request("GET", current_stream)).status == 200);
        time.advance(400ms);
        cleanup_pass(*playback, streaming);
        CHECK(count(*playback, "running_video_transcode_pipelines") == 1);
        CHECK(count(*playback, "idle_pipelines_reclaimed") == 0);

        // Requests for an obsolete generation are gone and renew nothing.
        CHECK(playback->handle(playback_request("GET", stale_stream)).status == 410);
        time.advance(100ms);
        cleanup_pass(*playback, streaming);
        // The entitlement is on its own clock, transcode_entitlement_idle, left
        // at its default here; the heap is reclaimed once no pipeline runs.
        REQUIRE(wait_until([&] {
            const auto status = playback_status(*playback);
            return status.find("sessions")->asUInt64() == 1 &&
                   status.find("video_transcodes")->asUInt64() == 1 &&
                   status.find("running_video_transcode_pipelines")->asUInt64() == 0 &&
                   !status.find("heap_reclaim_pending")->asBool() &&
                   status.find("heap_reclaim_requests")->asUInt64() >= 1 &&
                   status.find("idle_pipelines_reclaimed")->asUInt64() == 1 &&
                   status.find("heap_reclaim_runs")->asUInt64() >= 1;
        }, 5s));
        // Reclaiming the pipeline does not surrender the entitlement, so a
        // resume or seek after an idle pipeline is not refused; a create is,
        // node-scoped since another node may serve it.
        auto second = create_session(*playback, media_id, transcode);
        REQUIRE(second.status == 429);
        const auto second_body = body_of(second);
        const auto* error = second_body.find("error");
        REQUIRE(error != nullptr);
        CHECK(error->find("code")->asString() == "resource_limit");
        CHECK(error->find("scope")->asString() == "node");
        CHECK(error->find("node_healthy")->asBool());
        CHECK(error->find("alternative_may_succeed")->asBool());
        REQUIRE(session_call(*playback, "DELETE", session_id_of(first)).status == 204);
        REQUIRE(create_session(*playback, media_id, transcode).status == 201);
    }

    {
        const auto media_id = node.write("/media/keepalive.mp4", pattern(65549, 52));
        auto streaming = node.streaming();
        streaming.max_video_transcodes = 1;
        streaming.pipeline_idle = 250ms;
        // Clamped into [pipeline_idle, session_idle].
        streaming.transcode_entitlement_idle = 600ms;
        streaming.session_idle = 5min;
        auto playback = node.playback(streaming, std::make_shared<FakeMediaEngine>(), {}, nullptr, {}, time);
        auto created = create_session(*playback, media_id, transcode);
        REQUIRE(created.status == 201);
        const auto session_id = session_id_of(created);
        REQUIRE(count(*playback, "video_transcodes") == 1);
        // Polling the session is not stream activity: just short of the window
        // the slot is held, at the window it is released, and the session
        // itself survives.
        time.advance(599ms);
        CHECK(session_call(*playback, "GET", session_id).status == 200);
        cleanup_pass(*playback, streaming);
        CHECK(count(*playback, "video_transcodes") == 1);
        time.advance(1ms);
        CHECK(session_call(*playback, "GET", session_id).status == 200);
        cleanup_pass(*playback, streaming);
        CHECK(count(*playback, "video_transcodes") == 0);
        CHECK(session_call(*playback, "GET", session_id).status == 200);

        // A fresh session kept warm by a playlist fetch every 100 ms holds its
        // pipeline and its entitlement through 1500 ms, a window and a half
        // twice over.
        auto second = create_session(*playback, media_id, transcode);
        REQUIRE(second.status == 201);
        const auto fetch = playback_request("GET", body_of(second).find("stream")->find("url")->asString());
        REQUIRE(count(*playback, "video_transcodes") == 1);
        for (int i = 0; i < 15; ++i) {
            time.advance(100ms);
            CHECK(playback->handle(fetch).status == 200);
            cleanup_pass(*playback, streaming);
        }
        CHECK(count(*playback, "video_transcodes") == 1);
    }

    {
        const auto media_id = node.write("/media/never-watched.mp4", pattern(65549, 53));
        auto streaming = node.streaming();
        streaming.max_video_transcodes = 1;
        // The clock under test; the others stay long, so only this one can fire.
        streaming.session_unused_idle = 150ms;
        streaming.session_idle = 5min;
        streaming.pipeline_idle = 5min;
        auto playback = node.playback(streaming, std::make_shared<FakeMediaEngine>(), {}, nullptr, {}, time);
        // A session created and never used again (app killed, DELETE never sent).
        auto abandoned = create_session(*playback, media_id, transcode);
        REQUIRE(abandoned.status == 201);
        CHECK(count(*playback, "video_transcodes") == 1);
        time.advance(149ms);
        cleanup_pass(*playback, streaming);
        CHECK(count(*playback, "sessions") == 1);
        time.advance(1ms);
        cleanup_pass(*playback, streaming);
        CHECK(count(*playback, "sessions") == 0);
        CHECK(count(*playback, "video_transcodes") == 0);
        CHECK(count(*playback, "unused_sessions_reclaimed") == 1);
        CHECK(count(*playback, "session_unused_idle_ms") == 150);
        // The slot is genuinely released, not merely reported free.
        auto admitted = create_session(*playback, media_id, transcode);
        REQUIRE(admitted.status == 201);
        CHECK(session_id_of(admitted) != session_id_of(abandoned));
        // One stream fetch moves a session onto the long clock.
        REQUIRE(playback->handle(playback_request("GET", body_of(admitted).find("stream")->find("url")->asString()))
                    .status == 200);
        time.advance(400ms);
        cleanup_pass(*playback, streaming);
        CHECK(count(*playback, "sessions") == 1);
        CHECK(count(*playback, "video_transcodes") == 1);
        CHECK(count(*playback, "unused_sessions_reclaimed") == 1);
    }

    {
        // With no session the worker waits on nothing; creating one must wake
        // it to learn the new expiry, or the session below would never expire.
        const auto media_id = node.write("/media/expiring.mp4", pattern(64 * 1024, 54));
        auto streaming = node.streaming();
        streaming.session_idle = 50ms;
        auto playback = node.playback(streaming, std::make_shared<FakeMediaEngine>(), {}, nullptr, {}, time);
        REQUIRE(create_session(*playback, media_id, Json::Object{{"mode", "direct"}}).status == 201);
        time.advance(50ms);
        REQUIRE(wait_until([&] { return count(*playback, "sessions") == 0; }, 5s));
    }
}

MACHA_TEST("media_playback", test_an_async_start_answers_at_once_and_reports_its_progress) {
    // start=async answers 202 with a pending generation the client polls or
    // long-polls; progress outlives the elapsed budget, a stall fails it and
    // frees its slot at once, a delete stops it, and an update keeps the
    // playing generation until the replacement is ready or abandoned. Direct
    // play and keyed replays never start a second pipeline.
    PlaybackNode node;
    const auto media_id = node.write("/media/async.mkv", pattern(64 * 1024, 61));
    const SessionIdentity viewer{.id = "viewer", .roles = {"media_viewer"}};
    // Idle clocks and a failed start's retention run on stepped time; the start
    // monitor's progress and stall detection are real.
    SteppedTime time;
    constexpr auto failed_retention = 1000ms;
    struct Async {
        std::shared_ptr<ProgressingMediaEngine> engine = std::make_shared<ProgressingMediaEngine>();
        std::unique_ptr<PlaybackManager> playback;
    };
    const auto async_manager = [&](std::chrono::milliseconds no_progress) {
        Async out;
        auto streaming = node.streaming();
        streaming.max_sessions = 4;
        streaming.max_video_transcodes = 1;
        streaming.max_audio_transcodes = 1;
        streaming.startup_timeout = 1s;
        streaming.startup_no_progress = no_progress;
        streaming.start_wait_max = 5s;
        streaming.start_failed_retention = failed_retention;
        out.playback = node.playback(streaming, out.engine, {}, nullptr, {}, time);
        return out;
    };
    const auto create = [&](Async& f, const std::string& mode, const std::string& key = {}) {
        Json::Object preferences{{"mode", mode}};
        if (mode != "direct") preferences["container"] = std::string("fmp4");
        std::map<std::string, std::string, std::less<>> query{{"start", "async"}};
        if (!key.empty()) query["idempotency_key"] = key;
        return create_session(*f.playback, media_id, preferences, {}, viewer, std::move(query));
    };
    const auto call = [&](Async& f, const std::string& method, const std::string& id,
                          std::map<std::string, std::string, std::less<>> query = {},
                          const Json::Object& body = {}) {
        auto request = playback_request(method, "/api/v1/playback/sessions/" + id, body, viewer);
        request.query = std::move(query);
        return f.playback->handle(request);
    };
    const auto stage = [&](Async& f, const std::string& id) {
        auto response = call(f, "GET", id);
        if (response.status != 200) return "status-" + std::to_string(response.status);
        return body_of(response).find("start")->find("stage")->asString();
    };
    const auto pending_stage = [&](Async& f, const std::string& id) {
        const auto json = body_of(call(f, "GET", id));
        const auto* pending = json.find("pending");
        return pending ? pending->find("start")->find("stage")->asString() : std::string("none");
    };
    // A ready session: created, released, swapped in.
    const auto ready_session = [&](Async& f) {
        auto created = create(f, "transcode");
        REQUIRE(created.status == 202);
        const auto id = session_id_of(created);
        REQUIRE(wait_until([&] { return f.engine->starts() == 1; }, 2s));
        f.engine->release();
        REQUIRE(wait_until([&] { return stage(f, id) == "ready"; }, 2s));
        return id;
    };

    {
        // Still going past startup_timeout (1 s), because it keeps progressing.
        auto f = async_manager(1000ms);
        auto created = create(f, "transcode");
        REQUIRE(created.status == 202);
        const auto json = body_of(created);
        CHECK(json.find("status")->asString() == "playback_starting");
        const auto id = json.find("session_id")->asString();
        CHECK(json.find("stream")->find("url")->isNull());
        CHECK(json.find("stream")->find("close_url")->asString().ends_with("/close"));
        CHECK(json.find("mode")->asString() == "transcode");
        REQUIRE(wait_until([&] { return f.engine->starts() == 1; }, 2s));
        for (int ms = 250; ms <= 1250; ms += 250) {
            f.engine->advance(ms / 2);
            std::this_thread::sleep_for(250ms);
        }
        CHECK(stage(f, id) == "encoding");
        const auto polled = body_of(call(f, "GET", id));
        const auto* start = polled.find("start");
        REQUIRE(start != nullptr);
        CHECK(start->find("output_media_ms")->asInt64() == 625);
        CHECK(start->find("first_fragment_ms")->asInt64() == 2000);
        CHECK(start->find("source_bytes_read")->asUInt64() > 0);
        CHECK(start->find("preroll_total_ms") == nullptr);

        // A long-poll on the current sequence parks; a PATCH is refused while pending.
        const auto seq = std::to_string(start->find("progress_seq")->asUInt64());
        CHECK(call(f, "GET", id, {{"after", seq}, {"wait_ms", "60000"}}).defer.has_value());
        auto patch = call(f, "PATCH", id, {}, Json::Object{{"seek_ms", 1000}});
        CHECK(patch.status == 409);
        CHECK(body_of(patch).find("error")->find("code")->asString() == "playback_starting");

        f.engine->release();
        REQUIRE(wait_until([&] { return stage(f, id) == "ready"; }, 2s));
        CHECK(body_of(call(f, "GET", id)).find("stream")->find("url")->asString().ends_with("/master.m3u8"));
        CHECK(call(f, "DELETE", id).status == 204);

        // Deleting a pending start stops it and frees the slot.
        auto pending = create(f, "transcode");
        REQUIRE(pending.status == 202);
        REQUIRE(wait_until([&] { return f.engine->starts() == 2; }, 2s));
        CHECK(call(f, "DELETE", session_id_of(pending)).status == 204);
        REQUIRE(wait_until([&] { return !f.engine->last()->running.load(); }, 2s));
        auto next = create(f, "transcode");
        CHECK(next.status == 202);
        CHECK(call(f, "DELETE", session_id_of(next)).status == 204);

        // Direct play has no pipeline: it answers as a blocking create would.
        auto direct = create(f, "direct");
        REQUIRE(direct.status == 201);
        CHECK(body_of(direct).find("start") == nullptr);
        CHECK(call(f, "DELETE", session_id_of(direct)).status == 204);
        // A retried keyed create while pending answers the same pending session.
        const auto starts_before = f.engine->starts();
        auto keyed = create(f, "transcode", "retry-1");
        REQUIRE(keyed.status == 202);
        auto again = create(f, "transcode", "retry-1");
        REQUIRE(again.status == 202);
        CHECK(idempotency_of(again) == "replayed");
        CHECK(session_id_of(again) == session_id_of(keyed));
        CHECK(f.engine->starts() <= starts_before + 1);
        CHECK(call(f, "DELETE", session_id_of(keyed)).status == 204);
    }

    {
        // An async update keeps the playing generation until its replacement is
        // ready; abandoning the pending update leaves it playing and frees the
        // replacement's reservation; deleting the session takes both.
        auto f = async_manager(1000ms);
        const auto id = ready_session(f);
        const auto first = f.engine->last();
        auto patched = call(f, "PATCH", id, {{"start", "async"}}, Json::Object{{"seek_ms", 30000}});
        REQUIRE(patched.status == 202);
        const auto json = body_of(patched);
        CHECK(json.find("status")->asString() == "playback_starting");
        CHECK(json.find("generation")->asUInt64() == 1);
        CHECK(json.find("stream")->find("url")->asString().ends_with("/1/master.m3u8"));
        REQUIRE(json.find("pending") != nullptr);
        REQUIRE(wait_until([&] { return f.engine->starts() == 2; }, 2s));
        CHECK(first->running.load());
        f.engine->advance(500);
        REQUIRE(wait_until([&] { return pending_stage(f, id) == "encoding"; }, 2s));
        f.engine->release();
        REQUIRE(wait_until([&] { return pending_stage(f, id) == "none"; }, 2s));
        const auto swapped = body_of(call(f, "GET", id));
        CHECK(swapped.find("generation")->asUInt64() == 2);
        CHECK(swapped.find("seek_ms")->asUInt64() == 30000);
        CHECK(!first->running.load());

        const auto playing = f.engine->last();
        REQUIRE(call(f, "PATCH", id, {{"start", "async"}}, Json::Object{{"seek_ms", 20000}}).status == 202);
        REQUIRE(wait_until([&] { return f.engine->starts() == 3; }, 2s));
        const auto abandoned = f.engine->last();
        CHECK(call(f, "DELETE", id + "/pending").status == 204);
        CHECK(!abandoned->running.load());
        CHECK(playing->running.load());
        const auto after = body_of(call(f, "GET", id));
        CHECK(after.find("pending") == nullptr);
        CHECK(after.find("generation")->asUInt64() == 2);
        CHECK(call(f, "PATCH", id, {{"start", "async"}}, Json::Object{{"seek_ms", 40000}}).status == 202);
        REQUIRE(wait_until([&] { return f.engine->starts() == 4; }, 2s));
        const auto replacement = f.engine->last();
        CHECK(call(f, "DELETE", id).status == 204);
        CHECK(!replacement->running.load());
        CHECK(!playing->running.load());
    }

    {
        // A start that stops progressing fails with its stage and frees the
        // only slot at once; the failure is kept for its retention. A stalled
        // update fails under `pending` and the generation plays on.
        auto f = async_manager(600ms);
        auto created = create(f, "transcode");
        REQUIRE(created.status == 202);
        const auto id = session_id_of(created);
        REQUIRE(wait_until([&] { return stage(f, id) == "failed"; }, 3s));
        const auto failed = body_of(call(f, "GET", id));
        const auto* error = failed.find("start")->find("error");
        REQUIRE(error != nullptr);
        CHECK(error->find("code")->asString() == "playback_pipeline_start_failed");
        CHECK(error->find("start_stage")->asString() == "encoding");
        auto next = create(f, "transcode");
        CHECK(next.status == 202);
        // Its start reaches the engine before it is deleted: the starts
        // counted below include it.
        REQUIRE(wait_until([&] { return f.engine->starts() == 2; }, 2s));
        CHECK(call(f, "DELETE", session_id_of(next)).status == 204);
        // The failure is readable for its retention and then gone.
        CHECK(stage(f, id) == "failed");
        time.advance(failed_retention);
        CHECK(call(f, "GET", id).status == 404);

        const auto playing_id = [&] {
            auto playing = create(f, "transcode");
            REQUIRE(playing.status == 202);
            const auto playing_session = session_id_of(playing);
            REQUIRE(wait_until([&] { return f.engine->starts() == 3; }, 2s));
            f.engine->release();
            REQUIRE(wait_until([&] { return stage(f, playing_session) == "ready"; }, 2s));
            return playing_session;
        }();
        const auto playing = f.engine->last();
        REQUIRE(call(f, "PATCH", playing_id, {{"start", "async"}}, Json::Object{{"seek_ms", 20000}}).status == 202);
        REQUIRE(wait_until([&] { return pending_stage(f, playing_id) == "failed"; }, 3s));
        const auto json = body_of(call(f, "GET", playing_id));
        CHECK(json.find("pending")->find("start")->find("error")->find("code")->asString() ==
              "playback_pipeline_start_failed");
        CHECK(json.find("generation")->asUInt64() == 1);
        CHECK(playing->running.load());
        CHECK(call(f, "DELETE", playing_id).status == 204);
    }
}


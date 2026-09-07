// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "media_information.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

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

// Mirrors FakeMediaEngine for probe/HLS start, but extract_webvtt_segment
// blocks on a gate so a test can hold a session's subtitle_cache mutex open
// for as long as needed while exercising other playback operations.
class GatedSubtitleMediaEngine final : public MediaEngine {
    TestGate& gate_;
    mutable std::mutex mutex_;
    std::vector<PlaybackPlan> started_plans_;
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
        {
            std::lock_guard lock(mutex_);
            started_plans_.push_back(vod_plan.playback);
        }
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

MACHA_TEST("media_playback", test_media_segment_store_backpressure_and_spill) {
    TempDir t;
    RetainedMemoryLedger retained(8 * 1024, 1024, 4 * 1024, 1024);
    auto store = std::make_shared<MediaSegmentStore>(2, 2 * 1024, t.path() / "spill", 4000ms,
                                                     std::vector<double>{4.0, 4.0, 4.0, 4.0});
    REQUIRE(store->attach_memory_ledger(retained));
    CHECK(retained.stats().owner_bytes[static_cast<size_t>(MemoryOwner::playback_segment)] ==
          2 * 1024);
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
    CHECK(state.resident_bytes <= 2 * 1024 + 1024 + 4);
    CHECK(state.spill_bytes >= 1024);
    CHECK(state.descriptor_bytes >= state.segment_count);
    CHECK(state.planned_segments == 4);

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
    store.reset();
    CHECK(retained.stats().used_bytes == 0);
}

MACHA_TEST("media_playback", test_media_segment_store_supersede_wakes_stale_waiter_reversibly) {
    // Regression for a seek/generation-replacement stall: a request blocked
    // in wait_object() on a not-yet-produced segment of a superseded
    // generation must wake promptly (rather than only once the replacement
    // pipeline's own startup completes), and marking superseded is
    // reversible so a replacement attempt that fails leaves the still-active
    // store's normal long-poll behaviour intact.
    TempDir t;
    auto store = std::make_shared<MediaSegmentStore>(8, 8 * 1024, t.path() / "spill", 4000ms,
                                                      std::vector<double>{4.0, 4.0, 4.0, 4.0, 4.0});
    REQUIRE(store->publish_init(Bytes{'i', 'n', 'i', 't'}));
    REQUIRE(store->publish_segment(Bytes(16, 0x10), 4.0));

    std::atomic_bool first_wait_returned{};
    std::optional<Bytes> first_wait_result;
    std::jthread first_waiter([&] {
        store->note_requested(4);
        first_wait_result = store->wait_object("segment-000004.m4s", {});
        first_wait_returned.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK(!first_wait_returned.load());

    store->mark_superseded(true);
    for (int i = 0; i < 100 && !first_wait_returned.load(); ++i) std::this_thread::sleep_for(10ms);
    first_waiter.join();
    CHECK(first_wait_returned.load());
    CHECK(!first_wait_result.has_value());
    auto superseded_state = store->snapshot();
    CHECK(superseded_state.error.empty());
    CHECK(!superseded_state.finished);

    // A replacement that later fails clears superseded, restoring normal
    // long-poll blocking for the still-active generation.
    store->mark_superseded(false);
    std::atomic_bool second_wait_returned{};
    std::jthread second_waiter([&] {
        second_wait_returned.store(store->wait_object("segment-000004.m4s", {}).has_value());
    });
    std::this_thread::sleep_for(50ms);
    CHECK(!second_wait_returned.load());

    REQUIRE(store->publish_segment(Bytes(16, 0x11), 4.0));
    REQUIRE(store->publish_segment(Bytes(16, 0x12), 4.0));
    REQUIRE(store->publish_segment(Bytes(16, 0x13), 4.0));
    REQUIRE(store->publish_segment(Bytes(16, 0x14), 4.0));
    second_waiter.join();
    CHECK(second_wait_returned.load());
}

MACHA_TEST("media_playback", test_http_server_serves_streams_concurrently) {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = 0;
    config.workers = 2;
    config.max_queued_connections = 8;
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
    REQUIRE(wait_until([&] { return entered.load(); }, 1s));
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

MACHA_FAST_TEST("media_playback", test_media_vod_index_planning_rejects_partial_indexes) {
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

    // Regression: avformat_find_stream_info() can leave a Matroska
    // AVStream index containing only keyframes encountered during probing. The
    // old planner accepted that as complete and advertised the entire
    // unindexed tail as one fragment, e.g. segments=1 for a full movie.
    const std::vector<double> partial{0.0, 2.0};
    CHECK(!media_vod::indexed_plan(partial, 120.0, 0.0, 4.0).has_value());

    // A partially populated index must also be rejected when it contains
    // enough early entries to produce several apparently sensible fragments.
    const std::vector<double> partial_with_several_starts{0.0, 4.0, 8.0, 12.0, 16.0};
    CHECK(!media_vod::indexed_plan(partial_with_several_starts, 120.0, 0.0, 4.0).has_value());

    // Sparse but complete GOPs can still be remuxed: a fragment is as long as
    // the source GOP makes it.
    std::vector<double> sparse_complete;
    for (double seconds = 0.0; seconds < 60.0; seconds += 10.0)
        sparse_complete.push_back(seconds);
    CHECK(media_vod::indexed_plan(sparse_complete, 60.0, 0.0, 4.0).has_value());

    // Scene-cut encodes (x264/x265 defaults) leave keyframe gaps well past
    // 3x the target fragment. Until 0.32.11 one such gap anywhere sent the
    // whole file to a software transcode; a 40 s fragment is a long fragment,
    // not an unusable index.
    std::vector<double> scene_cut{0.0, 4.0, 44.0, 48.0, 52.0, 90.0, 94.0, 118.0};
    auto scene_cut_plan = media_vod::indexed_plan(scene_cut, 120.0, 0.0, 4.0);
    REQUIRE(scene_cut_plan.has_value());
    CHECK(std::abs(scene_cut_plan->longest_segment_seconds - 40.0) < 0.0005);
    // ... while a gap a viewer would wait minutes to seek across still is.
    const std::vector<double> huge_gap{0.0, 4.0, 110.0, 114.0, 118.0};
    CHECK(!media_vod::indexed_plan(huge_gap, 120.0, 0.0, 4.0).has_value());

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

MACHA_FAST_TEST("media_playback", test_hls_codec_strings_describe_the_fragments) {
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
    // The libx264/AAC transcode output is described, not the source.
    CHECK(hls_codec_string("h264", &hevc10, true) == "avc1.640029");
    CHECK(hls_codec_string("aac", &eac3, true) == "mp4a.40.2");

    PlaybackPlan remux;
    remux.video = MediaTransform::copy;
    remux.audio = MediaTransform::copy;
    remux.video_codec = "hevc";
    remux.audio_codec = "eac3";
    const auto remux_inf = hls_variant_stream_inf(remux, &hevc10, &eac3, 10'887'601);
    CHECK(remux_inf == "#EXT-X-STREAM-INF:BANDWIDTH=10887601,CODECS=\"hvc1.2.4.L153.B0,ec-3\",RESOLUTION=1920x802");

    PlaybackPlan transcode;
    transcode.video = MediaTransform::transcode;
    transcode.audio = MediaTransform::transcode;
    transcode.video_codec = "h264";
    transcode.audio_codec = "aac";
    transcode.target_height = 720;
    const auto transcode_inf = hls_variant_stream_inf(transcode, &hevc10, &eac3, 10'887'601);
    CHECK(transcode_inf == "#EXT-X-STREAM-INF:BANDWIDTH=5000000,CODECS=\"avc1.640029,mp4a.40.2\",RESOLUTION=1724x720");
}

MACHA_TEST("media_playback", test_reseek_hls_vod_reuses_prepared_random_access_state) {
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
    REQUIRE(transcode_seek->segment_durations.size() >= 2);
    // A seek's first fragment is the short start-up fragment (2 s), so the
    // generation answers after 2 s of encoding; the rest keep the target.
    CHECK(std::abs(transcode_seek->segment_durations.front() - 2.0) < 0.0005);
    CHECK(std::abs(transcode_seek->segment_durations[1] - 4.0) < 0.0005);

    // Regression: a transcode plan whose keyframe index is known (e.g.
    // captured during the initial VOD plan) should snap forward to the
    // nearest keyframe instead of staying frame-accurate -- avoiding the
    // decode-then-discard cost of landing mid-GOP -- even when a sparse gap
    // elsewhere in the file would make indexed_plan's whole-file
    // segment-density check reject the plan outright. That check is
    // remux-only: transcode lays down its own GOP structure via
    // fixed_vod_durations regardless of source keyframes.
    // The keyframe timestamp below (62.5274s) is deliberately not a round
    // number of milliseconds: llround(62527.4) rounds DOWN to 62527ms, which
    // reconstructs to microseconds *before* the real keyframe's PTS and
    // makes avformat_seek_file's AVSEEK_FLAG_BACKWARD search land one
    // keyframe early -- a real regression caught live (see CHANGELOG). Must
    // round up (ceil) to 62528ms instead, guaranteeing the reconstructed
    // target is never before the keyframe it names.
    HlsVodPlan transcode_with_keyframes;
    transcode_with_keyframes.playback.mode = PlaybackMode::transcode;
    transcode_with_keyframes.playback.video = MediaTransform::transcode;
    transcode_with_keyframes.source_duration_seconds = 7200.0;
    transcode_with_keyframes.seek_segment_seconds = 4.0;
    transcode_with_keyframes.reusable_seek = true;
    transcode_with_keyframes.video_random_access_points = {0.0, 30.0, 60.0, 62.5274, 6000.0};

    auto snapped_seek = reseek_hls_vod(transcode_with_keyframes, 61s);
    REQUIRE(snapped_seek.has_value());
    CHECK(snapped_seek->playback.seek == 62528ms);
    REQUIRE(!snapped_seek->segment_durations.empty());
    CHECK(std::abs(snapped_seek->segment_durations.front() - 2.0) < 0.0005);

    // Seeking past the last known keyframe falls back to the unsnapped
    // position rather than failing.
    auto past_last_keyframe = reseek_hls_vod(transcode_with_keyframes, 6500s);
    REQUIRE(past_last_keyframe.has_value());
    CHECK(past_last_keyframe->playback.seek == 6500s);

    HlsVodPlan unavailable;
    CHECK(!reseek_hls_vod(unavailable, 10s).has_value());
}

MACHA_FAST_TEST("media_playback", test_media_timestamp_repair) {
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

MACHA_TEST("media_playback", test_playback_probe_failure_is_stage_specific) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
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
    request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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

MACHA_TEST("media_playback", test_immutable_media_profile_survives_cold_playback_manager) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();

    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/profile.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/profile.mp4", true);
    auto bytes = pattern(128 * 1024 + 37);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto first_media_id = file_media_id(service.filesystem().getattr("/media/profile.mp4"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";

    auto request_for = [](const std::string& media_id) {
        Json::Object preferences{{"mode", "direct"}};
        Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
        auto text = Json(std::move(root)).dump();
        HttpRequest request;
        request.method = "POST";
        request.path = "/api/v1/playback/sessions";
        request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
        request.query["idempotency_key"] = media_id;
        request.body.assign(text.begin(), text.end());
        return request;
    };

    Json first_body;
    std::string first_session_id;
    {
        auto engine = std::make_unique<FakeMediaEngine>();
        auto* observed = engine.get();
        PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                                 std::move(engine));
        playback.start();
        auto created = playback.handle(request_for(first_media_id));
        REQUIRE(created.status == 201);
        CHECK(observed->probes() == 1);
        first_body = Json::parse(std::string(created.body.begin(), created.body.end()));
        first_session_id = first_body.find("session_id")->asString();
        CHECK(created.headers.at("X-Macha-Idempotency") == "created");
        playback.stop();
    }
    REQUIRE(service.catalogue().media_profile(first_media_id).has_value());
    {
        CatalogueApi catalogue_api(service.catalogue(), service.catalogue_hints());
        HttpRequest profile_request;
        profile_request.method = "GET";
        profile_request.path = "/api/v1/catalogue/media/" + first_media_id + "/profile";
        auto response = catalogue_api.handle(profile_request);
        REQUIRE(response.status == 200);
        CHECK(response.headers.at("Cache-Control").find("immutable") != std::string::npos);
        auto profile = Json::parse(std::string(response.body.begin(), response.body.end()));
        CHECK(profile.find("media_id")->asString() == first_media_id);
        CHECK(profile.find("format")->asString() == "mov,mp4,m4a,3gp,3g2,mj2");
        CHECK(profile.find("duration_ms")->asUInt64() == 60'000);
        REQUIRE(profile.find("streams")->asArray().size() == 4);
    }

    // A distinct manager has an empty process-local cache. It must construct
    // the identical response semantics without invoking its media engine's
    // probe path (and therefore without opening the media source for probing).
    {
        auto engine = std::make_unique<FakeMediaEngine>();
        auto* observed = engine.get();
        PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                                 std::move(engine));
        playback.start();
        auto created = playback.handle(request_for(first_media_id));
        REQUIRE(created.status == 201);
        CHECK(observed->probes() == 0);
        auto cached_body = Json::parse(std::string(created.body.begin(), created.body.end()));
        CHECK(cached_body.find("session_id")->asString() == first_session_id);
        CHECK(cached_body.find("generation")->dump() ==
              first_body.find("generation")->dump());
        for (const auto* field : {"mode", "media_id", "duration_ms", "preferences",
                                  "selection", "source", "output", "options"}) {
            REQUIRE(first_body.find(field) != nullptr);
            REQUIRE(cached_body.find(field) != nullptr);
            CHECK(first_body.find(field)->dump() == cached_body.find(field)->dump());
        }
        playback.stop();
    }

    // Replacing the path changes its immutable extent-manifest identity. The
    // old profile remains valid for the old object but cannot hit the new one.
    auto replacement = pattern(128 * 1024 + 41);
    for (auto& byte : replacement) byte ^= 0x5a;
    auto replacement_writer = service.filesystem().open_write("/media/profile.mp4", true);
    REQUIRE(replacement_writer->write(0, replacement) == replacement.size());
    replacement_writer->commit();
    const auto second_media_id = file_media_id(service.filesystem().getattr("/media/profile.mp4"));
    REQUIRE(second_media_id != first_media_id);
    {
        auto engine = std::make_unique<FakeMediaEngine>();
        auto* observed = engine.get();
        PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                                 std::move(engine));
        playback.start();
        auto created = playback.handle(request_for(second_media_id));
        REQUIRE(created.status == 201);
        CHECK(observed->probes() == 1);
        playback.stop();
    }

    service.stop();
}

MACHA_TEST("media_playback", test_concurrent_immutable_profile_misses_coalesce) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/coalesce.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/coalesce.mp4", true);
    auto bytes = pattern(64 * 1024 + 19);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/coalesce.mp4"));

    TestGate gate;
    auto engine = std::make_unique<CoalescingProbeMediaEngine>(gate);
    auto* observed = engine.get();
    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.probe_timeout = 2s;
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::move(engine));
    playback.start();

    Json::Object preferences{{"mode", "direct"}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/playback/sessions";
    request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    request.query["idempotency_key"] = "coalesced-create-1";
    request.body.assign(text.begin(), text.end());
    HttpResponse first, second;
    std::jthread a([&] { first = playback.handle(request); });
    REQUIRE(gate.wait_for_entries(1));
    std::jthread b([&] { second = playback.handle(request); });
    std::this_thread::sleep_for(50ms);
    CHECK(gate.entered() == 1);
    CHECK(observed->probes() == 1);
    gate.open();
    a.join();
    b.join();
    CHECK(first.status == 201);
    CHECK(second.status == 201);
    CHECK(observed->probes() == 1);
    auto first_json = Json::parse(std::string(first.body.begin(), first.body.end()));
    auto second_json = Json::parse(std::string(second.body.begin(), second.body.end()));
    CHECK(first_json.find("session_id")->dump() == second_json.find("session_id")->dump());
    CHECK(first_json.find("generation")->dump() == second_json.find("generation")->dump());
    CHECK(first.headers.at("X-Macha-Idempotency") == "created");
    CHECK(second.headers.at("X-Macha-Idempotency") == "replayed");

    auto replay = playback.handle(request);
    REQUIRE(replay.status == 201);
    CHECK(replay.headers.at("X-Macha-Idempotency") == "replayed");
    auto replay_json = Json::parse(std::string(replay.body.begin(), replay.body.end()));
    CHECK(replay_json.find("session_id")->dump() == first_json.find("session_id")->dump());
    CHECK(observed->probes() == 1);

    Json::Object changed_preferences{{"mode", "direct"}};
    Json::Object changed_root{{"media_id", media_id},
                              {"seek_ms", 1000},
                              {"preferences", Json(std::move(changed_preferences))}};
    auto changed_text = Json(std::move(changed_root)).dump();
    auto conflicting = request;
    conflicting.body.assign(changed_text.begin(), changed_text.end());
    auto conflict = playback.handle(conflicting);
    REQUIRE(conflict.status == 409);
    const std::string conflict_body(conflict.body.begin(), conflict.body.end());
    CHECK(conflict_body.find("idempotency_conflict") != std::string::npos);

    HttpRequest remove;
    remove.method = "DELETE";
    remove.path = "/api/v1/playback/sessions/" + first_json.find("session_id")->asString();
    CHECK(playback.handle(remove).status == 204);
    auto reused_after_delete = playback.handle(conflicting);
    REQUIRE(reused_after_delete.status == 201);
    CHECK(reused_after_delete.headers.at("X-Macha-Idempotency") == "created");
    CHECK(observed->probes() == 1);
    REQUIRE(wait_until([&] { return service.catalogue().media_profile(media_id).has_value(); }, 2s));

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_failed_idempotent_creation_releases_joiners_and_reservations) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/failing.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/failing.mp4", true);
    auto bytes = pattern(64 * 1024 + 7);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/failing.mp4"));

    TestGate gate;
    auto engine = std::make_unique<CoalescingProbeMediaEngine>(gate, true);
    auto* observed = engine.get();
    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback-failure";
    streaming.probe_timeout = 2s;
    streaming.max_sessions = 1;
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::move(engine));
    playback.start();

    Json::Object preferences{{"mode", "direct"}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/playback/sessions";
    request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    request.query["idempotency_key"] = "failing-create-1";
    request.body.assign(text.begin(), text.end());

    HttpResponse first, second;
    std::jthread a([&] { first = playback.handle(request); });
    REQUIRE(gate.wait_for_entries(1));
    std::jthread b([&] { second = playback.handle(request); });
    std::this_thread::sleep_for(30ms);
    CHECK(observed->probes() == 1);
    gate.open();
    a.join();
    b.join();
    CHECK(first.status == 503);
    CHECK(second.status == 503);
    CHECK(observed->probes() == 1);

    // The failed association and its sole pending-session reservation must be
    // gone. With max_sessions=1, reaching a second probe proves both releases.
    auto retry = playback.handle(request);
    CHECK(retry.status == 503);
    CHECK(observed->probes() == 2);

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_profile_endpoint_pending_does_not_gate_session_negotiation) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/pending.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/pending.mp4", true);
    auto bytes = pattern(32 * 1024 + 3);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/pending.mp4"));

    auto engine = std::make_shared<FakeMediaEngine>();
    MediaInformationService information(service.filesystem(), service.catalogue(), engine,
                                        c.state_path);
    std::atomic_uint queue_requests{};
    auto queue = [&](const std::vector<std::string>& media_ids) {
        ++queue_requests;
        return information.request(media_ids, MediaInformationPriority::requested,
                                   "media-information-api");
    };

    CatalogueApi catalogue_api(service.catalogue(), service.catalogue_hints(), {}, queue);
    HttpRequest profile_request;
    profile_request.method = "GET";
    profile_request.path = "/api/v1/catalogue/media/" + media_id + "/profile";
    auto profile_pending = catalogue_api.handle(profile_request);
    REQUIRE(profile_pending.status == 202);
    CHECK(profile_pending.headers.at("Retry-After") == "1");

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback-pending";
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             engine, queue, &information);
    playback.start();
    Json::Object preferences{{"mode", "direct"}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    create.query["idempotency_key"] = "pending-profile-create";
    create.body.assign(text.begin(), text.end());
    auto admitted = playback.handle(create);
    REQUIRE(admitted.status == 201);
    CHECK(admitted.headers.at("X-Macha-Idempotency") == "created");
    CHECK(engine->probes() == 1);
    CHECK(queue_requests.load() == 1);

    // Session negotiation produced the profile through the same shared flight,
    // but publication remains asynchronous and outside admission.
    CHECK(!service.catalogue().media_profile(media_id).has_value());
    information.start();
    REQUIRE(wait_until([&] { return service.catalogue().media_profile(media_id).has_value(); }, 2s));

    playback.stop();
    information.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_unavailable_profile_queue_uses_media_engine_fallback) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/fallback.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/fallback.mp4", true);
    auto bytes = pattern(32 * 1024 + 5);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/fallback.mp4"));

    std::atomic_uint queue_attempts{};
    auto unavailable = [&](const std::vector<std::string>&) {
        ++queue_attempts;
        return size_t{0};
    };
    auto engine = std::make_shared<FakeMediaEngine>();
    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback-fallback";
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             engine, unavailable);
    playback.start();

    Json::Object preferences{{"mode", "direct"}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    create.query["idempotency_key"] = "unavailable-profile-fallback";
    create.body.assign(text.begin(), text.end());

    auto admitted = playback.handle(create);
    REQUIRE(admitted.status == 201);
    CHECK(admitted.headers.at("X-Macha-Idempotency") == "created");
    CHECK(queue_attempts.load() == 0);
    CHECK(engine->probes() == 1);
    REQUIRE(wait_until([&] { return service.catalogue().media_profile(media_id).has_value(); }, 2s));

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_failed_profile_job_retry_falls_back_and_replays_once) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/retry.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/retry.mp4", true);
    auto bytes = pattern(32 * 1024 + 7);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/retry.mp4"));

    std::atomic_uint requests{};
    auto pending_then_failed = [&](const std::vector<std::string>&) {
        return ++requests == 1 ? size_t{1} : size_t{0};
    };
    CatalogueApi catalogue_api(service.catalogue(), service.catalogue_hints(), {},
                               pending_then_failed);
    HttpRequest profile_request;
    profile_request.method = "GET";
    profile_request.path = "/api/v1/catalogue/media/" + media_id + "/profile";
    auto pending = catalogue_api.handle(profile_request);
    REQUIRE(pending.status == 202);
    auto engine = std::make_shared<FakeMediaEngine>();
    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback-retry";
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             engine, pending_then_failed);
    playback.start();

    Json::Object preferences{{"mode", "direct"}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    create.query["idempotency_key"] = "failed-profile-retry";
    create.body.assign(text.begin(), text.end());

    auto admitted = playback.handle(create);
    REQUIRE(admitted.status == 201);
    CHECK(admitted.headers.at("X-Macha-Idempotency") == "created");
    CHECK(engine->probes() == 1);
    CHECK(requests.load() == 1);
    auto admitted_body = Json::parse(std::string(admitted.body.begin(), admitted.body.end()));

    auto replayed = playback.handle(create);
    REQUIRE(replayed.status == 201);
    CHECK(replayed.headers.at("X-Macha-Idempotency") == "replayed");
    CHECK(engine->probes() == 1);
    auto replayed_body = Json::parse(std::string(replayed.body.begin(), replayed.body.end()));
    CHECK(replayed_body.find("session_id")->dump() == admitted_body.find("session_id")->dump());
    CHECK(replayed_body.find("generation")->dump() == admitted_body.find("generation")->dump());

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_media_information_hints_reorder_by_priority) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());

    auto create_media = [&](std::string path, size_t size) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto writer = service.filesystem().open_write(path, true);
        auto bytes = pattern(size);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
        return file_media_id(service.filesystem().getattr(path));
    };
    const auto low_a = create_media("/media/low-a.mp4", 32769);
    const auto low_b = create_media("/media/low-b.mp4", 32771);
    const auto requested = create_media("/media/requested.mp4", 32773);

    auto engine = std::make_shared<PriorityMediaInformationEngine>();
    MediaInformationService information(service.filesystem(), service.catalogue(), engine,
                                        t.path() / "media-info");
    REQUIRE(information.request_path("/media/low-a.mp4",
                                     MediaInformationPriority::background));
    REQUIRE(information.request_path("/media/low-b.mp4",
                                     MediaInformationPriority::background));
    REQUIRE(information.request_path("/media/requested.mp4",
                                     MediaInformationPriority::requested,
                                     "media-information-request"));
    information.start();

    REQUIRE(wait_until([&] { return engine->completions() == 3; }, 5s));
    auto order = engine->order();
    REQUIRE(order.size() == 3);
    CHECK(order.front() == requested);
    REQUIRE(wait_until([&] {
        return service.catalogue().media_profile(low_a).has_value() &&
               service.catalogue().media_profile(low_b).has_value() &&
               service.catalogue().media_profile(requested).has_value();
    }, 5s));

    information.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_media_information_foreground_requests_share_one_scan) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    const std::string path = "/media/single-flight.mp4";
    service.filesystem().create_file(path, 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(path, true);
    auto bytes = pattern(65539);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto entry = service.filesystem().getattr(path);
    auto media_id = file_media_id(entry);

    TestGate gate;
    auto engine = std::make_shared<CoalescingProbeMediaEngine>(gate);
    MediaInformationService information(service.filesystem(), service.catalogue(), engine,
                                        t.path() / "media-info");
    information.start();
    std::optional<MediaProbeResult> first, second;
    std::jthread a([&] {
        first = information.resolve_playback(media_id, path, entry, Clock::now() + 2s);
    });
    REQUIRE(gate.wait_for_entries(1));
    std::jthread b([&] {
        second = information.resolve_playback(media_id, path, entry, Clock::now() + 2s);
    });
    std::this_thread::sleep_for(30ms);
    CHECK(engine->probes() == 1);
    gate.open();
    a.join();
    b.join();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
    CHECK(engine->probes() == 1);
    REQUIRE(wait_until([&] { return service.catalogue().media_profile(media_id).has_value(); }, 2s));

    information.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_media_information_playback_takes_over_speculative_scan) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    const std::string path = "/media/takeover.mp4";
    service.filesystem().create_file(path, 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(path, true);
    auto bytes = pattern(65541);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto entry = service.filesystem().getattr(path);
    auto media_id = file_media_id(entry);

    auto engine = std::make_shared<PriorityMediaInformationEngine>(true);
    MediaInformationService information(service.filesystem(), service.catalogue(), engine,
                                        t.path() / "media-info");
    REQUIRE(information.request_path(path));
    information.start();
    REQUIRE(wait_until([&] { return engine->starts() == 1; }, 1s));

    auto resolved = information.resolve_playback(media_id, path, entry, Clock::now() + 2s);
    CHECK(!resolved.streams.empty());
    CHECK(engine->starts() == 2);
    CHECK(engine->cancellations() == 1);
    CHECK(engine->completions() == 1);
    REQUIRE(wait_until([&] { return service.catalogue().media_profile(media_id).has_value(); }, 2s));

    information.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_media_information_profile_pruning_tracks_last_live_copy) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    auto bytes = pattern(65543);
    for (const auto* path : {"/media/copy-a.mp4", "/media/copy-b.mp4"}) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto writer = service.filesystem().open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    }
    auto first = service.filesystem().getattr("/media/copy-a.mp4");
    auto second = service.filesystem().getattr("/media/copy-b.mp4");
    const auto media_id = file_media_id(first);
    REQUIRE(file_media_id(second) == media_id);

    auto engine = std::make_shared<PriorityMediaInformationEngine>();
    MediaInformationService information(service.filesystem(), service.catalogue(), engine,
                                        t.path() / "media-info");
    REQUIRE(information.request_path("/media/copy-a.mp4"));
    information.start();
    REQUIRE(wait_until([&] { return service.catalogue().media_profile(media_id).has_value(); }, 2s));

    service.filesystem().unlink("/media/copy-a.mp4");
    information.request_prune();
    std::this_thread::sleep_for(100ms);
    CHECK(service.catalogue().media_profile(media_id).has_value());

    service.filesystem().unlink("/media/copy-b.mp4");
    information.request_prune();
    REQUIRE(wait_until([&] { return !service.catalogue().media_profile(media_id).has_value(); }, 2s));

    information.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_media_information_retries_profile_publication_without_rescanning) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    const std::string path = "/media/retry-publication.mp4";
    service.filesystem().create_file(path, 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(path, true);
    auto bytes = pattern(65547);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto media_id = file_media_id(service.filesystem().getattr(path));

    auto engine = std::make_shared<PriorityMediaInformationEngine>();
    std::atomic_uint publication_attempts{};
    MediaInformationService information(
        service.filesystem(), service.catalogue(), engine, t.path() / "media-info",
        [&](std::string id, MediaProbeResult profile) {
            if (++publication_attempts == 1)
                throw std::runtime_error("synthetic catalogue conflict");
            service.catalogue().put_media_profile(id, std::move(profile));
        });
    REQUIRE(information.request_path(path));
    information.start();

    REQUIRE(wait_until([&] {
        return service.catalogue().media_profile(media_id).has_value();
    }, 3s));
    CHECK(publication_attempts.load() == 2);
    CHECK(engine->starts() == 1);
    CHECK(engine->completions() == 1);

    information.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_abandoned_transcode_pipeline_is_reclaimed_before_session) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/abandoned.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/abandoned.mp4", true);
    auto bytes = pattern(65549);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto media_id = file_media_id(service.filesystem().getattr("/media/abandoned.mp4"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.max_video_transcodes = 1;
    streaming.video_decoder_threads = 3;
    streaming.pipeline_idle = 50ms;
    streaming.session_idle = 5min;
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::make_unique<FakeMediaEngine>());
    playback.start();

    Json::Object preferences{{"mode", "transcode"}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    create.body.assign(text.begin(), text.end());
    auto first = playback.handle(create);
    REQUIRE(first.status == 201);
    auto first_json = Json::parse(std::string(first.body.begin(), first.body.end()));
    auto stale_stream = first_json.find("stream")->find("url")->asString();
    const auto current_stream = stale_stream;
    auto generation = stale_stream.find("/1/master.m3u8");
    REQUIRE(generation != std::string::npos);
    stale_stream.replace(generation, std::string("/1/master.m3u8").size(),
                         "/0/master.m3u8");

    auto playback_status = [&] {
        HttpRequest request;
        request.method = "GET";
        request.path = "/api/v1/playback/status";
        auto response = playback.handle(request);
        REQUIRE(response.status == 200);
        return Json::parse(std::string(response.body.begin(), response.body.end()));
    };
    auto active = playback_status();
    CHECK(active.find("sessions")->asUInt64() == 1);
    CHECK(active.find("video_transcodes")->asUInt64() == 1);
    CHECK(active.find("video_decoder_threads")->asUInt64() == 3);

    // Valid current-generation traffic renews the physical pipeline lease.
    for (int i = 0; i < 4; ++i) {
        HttpRequest current;
        current.method = "GET";
        current.path = current_stream;
        CHECK(playback.handle(current).status == 200);
        std::this_thread::sleep_for(20ms);
    }
    CHECK(playback_status().find("video_transcodes")->asUInt64() == 1);

    // A client retrying an obsolete generation receives a precise 404, but
    // those invalid requests must not keep an abandoned encoder leased.
    for (int i = 0; i < 4; ++i) {
        HttpRequest stale;
        stale.method = "GET";
        stale.path = stale_stream;
        CHECK(playback.handle(stale).status == 404);
        std::this_thread::sleep_for(20ms);
    }

    REQUIRE(wait_until([&] {
        auto status = playback_status();
        return status.find("sessions")->asUInt64() == 1 &&
               status.find("video_transcodes")->asUInt64() == 1 &&
               status.find("running_video_transcode_pipelines")->asUInt64() == 0 &&
               status.find("idle_pipelines_reclaimed")->asUInt64() == 1 &&
               !status.find("heap_reclaim_pending")->asBool() &&
               status.find("heap_reclaim_requests")->asUInt64() >= 1 &&
               status.find("heap_reclaim_runs")->asUInt64() >= 1;
    }, 1s));

    // Physical reclamation does not surrender the persistent viewer's logical
    // entitlement: otherwise an ordinary resume/seek could be rejected after
    // another viewer slipped into the transient idle gap.
    auto second = playback.handle(create);
    REQUIRE(second.status == 429);

    HttpRequest remove;
    remove.method = "DELETE";
    remove.path = "/api/v1/playback/sessions/" +
                  first_json.find("session_id")->asString();
    REQUIRE(playback.handle(remove).status == 204);
    second = playback.handle(create);
    REQUIRE(second.status == 201);

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_status_does_not_block_on_a_contended_subtitle_cache) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/subtitled.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/subtitled.mp4", true);
    auto bytes = pattern(65549);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto media_id = file_media_id(service.filesystem().getattr("/media/subtitled.mp4"));

    TestGate gate;
    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::make_unique<GatedSubtitleMediaEngine>(gate));
    playback.start();

    Json::Object preferences{{"mode", "remux"}, {"subtitle_stream", 2}};
    Json::Object root{{"media_id", media_id}, {"preferences", Json(std::move(preferences))}};
    auto text = Json(std::move(root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    create.body.assign(text.begin(), text.end());
    auto created = playback.handle(create);
    REQUIRE(created.status == 201);
    auto created_json = Json::parse(std::string(created.body.begin(), created.body.end()));
    const auto subtitle_url = created_json.find("stream")->find("subtitle_url")->asString();
    const auto subtitle_base = subtitle_url.substr(0, subtitle_url.rfind('/'));

    // Hold this session's subtitle_cache mutex open for the whole test by
    // blocking inside extract_webvtt_segment, exactly as a slow real
    // extraction would. Before the fix this alone was enough to stall
    // status() (and therefore create/patch/delete/cleanup, which all take
    // the same global session mutex) for as long as the gate stayed shut.
    HttpResponse segment_response;
    std::jthread segment_request([&] {
        HttpRequest segment;
        segment.method = "GET";
        segment.path = subtitle_base + "/segment-0.vtt";
        segment_response = playback.handle(segment);
    });
    REQUIRE(gate.wait_for_entries(1));

    HttpRequest status_request;
    status_request.method = "GET";
    status_request.path = "/api/v1/playback/status";
    const auto status_started = Clock::now();
    auto status_response = playback.handle(status_request);
    const auto status_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - status_started);
    REQUIRE(status_response.status == 200);
    // status() must not block on the subtitle_cache mutex the gated request
    // is still holding: the per-session read is try_lock, so a busy session
    // just contributes nothing to this snapshot instead of stalling status()
    // (and, via the global mutex, every other playback operation) for as
    // long as the gate stays shut.
    CHECK(status_elapsed < 500ms);
    auto status_json = Json::parse(std::string(status_response.body.begin(), status_response.body.end()));
    CHECK(status_json.find("sessions")->asUInt64() == 1);
    CHECK(status_json.find("subtitle_cache_entries")->asUInt64() == 0);

    // A second, unrelated session-mutating call must also not be stuck
    // behind the global mutex while the gate is held.
    HttpRequest second_create;
    second_create.method = "POST";
    second_create.path = "/api/v1/playback/sessions";
    second_create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    second_create.body.assign(text.begin(), text.end());
    const auto second_started = Clock::now();
    auto second_created = playback.handle(second_create);
    const auto second_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - second_started);
    REQUIRE(second_created.status == 201);
    CHECK(second_elapsed < 500ms);

    gate.open();
    segment_request.join();
    REQUIRE(segment_response.status == 200);
    CHECK(segment_response.content_type.starts_with("text/vtt"));

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_attached_picture_audio_direct_play) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
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
    request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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

namespace {
// The Ratatouille case: HEVC Main 10, PQ transfer (Dolby Vision profile 8),
// E-AC3 audio, in Matroska.
class HdrFakeMediaEngine final : public FakeMediaEngine {
  public:
    MediaProbeResult probe(const MediaSource& source, std::chrono::milliseconds timeout = {}) override {
        auto result = FakeMediaEngine::probe(source, timeout);
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
} // namespace

MACHA_FAST_TEST("media_playback", test_schema1_video_profiles_are_stale_for_negotiation) {
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
    CHECK(profile.schema_version == 2);
    CHECK(valid_catalogue_media_profile("macha:abc", profile));
    // A profile stored before 0.32.12 does not, so it is regenerated: a
    // stale one let a Dolby Vision title be copied to a client that could
    // not decode it.
    profile.schema_version = 1;
    CHECK(!valid_catalogue_media_profile("macha:abc", profile));
    // ... unless nothing in it is a video stream.
    profile.probe.streams = {audio};
    CHECK(valid_catalogue_media_profile("macha:abc", profile));
}

MACHA_TEST("media_playback", test_auto_transcodes_hdr_10bit_unless_the_client_opts_in) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();
    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/dv.mkv", 0644, getuid(), getgid());
    auto bytes = pattern(128 * 1024 + 17);
    auto writer = service.filesystem().open_write("/media/dv.mkv", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/dv.mkv"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.startup_timeout = 2s;
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::make_unique<HdrFakeMediaEngine>());
    playback.start();

    const auto create = [&](Json::Object caps) {
        Json::Object root{{"media_id", media_id}, {"capabilities", Json(std::move(caps))},
                          {"preferences", Json(Json::Object{{"mode", "auto"}})}};
        auto text = Json(std::move(root)).dump();
        HttpRequest request;
        request.method = "POST";
        request.path = "/api/v1/playback/sessions";
        request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
        request.body.assign(text.begin(), text.end());
        auto response = playback.handle(request);
        REQUIRE(response.status == 201);
        return Json::parse(std::string(response.body.begin(), response.body.end()));
    };
    const auto hevc_caps = [] {
        return Json::Object{{"containers", Json::Array{Json("mp4"), Json("mkv")}},
                            {"video_codecs", Json::Array{Json("h264"), Json("hevc")}},
                            {"audio_codecs", Json::Array{Json("aac"), Json("eac3")}},
                            {"hls_fmp4", true}};
    };

    // "hevc" alone is a decoder claim; the 10-bit PQ samples are transcoded
    // (and not offered direct) until the client says its pipeline takes them.
    auto conservative = create(hevc_caps());
    CHECK(conservative.find("mode")->asString() == "transcode");
    auto master = playback.handle([&] {
        HttpRequest r;
        r.method = "GET";
        r.path = conservative.find("stream")->find("url")->asString();
        r.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
        return r;
    }());
    REQUIRE(master.status == 200);
    REQUIRE(master.stream != nullptr);
    Bytes master_bytes(static_cast<size_t>(master.content_length()));
    REQUIRE(master.stream->read(0, master_bytes) == master_bytes.size());
    const std::string master_text(master_bytes.begin(), master_bytes.end());
    // The master playlist declares the H.264 transcode and the AAC the
    // E-AC-3 is transcoded to (only AAC is copied for now).
    if (master_text.find("#EXT-X-STREAM-INF:") == std::string::npos)
        std::fprintf(stderr, "master playlist body:\n%s\n", master_text.c_str());
    CHECK(master_text.find("#EXT-X-STREAM-INF:") != std::string::npos);
    CHECK(master_text.find("CODECS=\"avc1.640029,mp4a.40.2\"") != std::string::npos);
    CHECK(master_text.find("\nmedia.m3u8\n") != std::string::npos);
    CHECK(conservative.find("output")->find("video")->find("transform")->asString() == "transcode");
    CHECK(conservative.find("output")->find("video")->find("color_transfer")->asString() == "bt709");

    // A client that presents PQ at 10 bits gets the video as it is; the
    // session is still labelled transcode for the audio alone.
    auto opted_in = hevc_caps();
    opted_in["video_bit_depth"] = 10;
    opted_in["hdr"] = Json::Array{Json("smpte2084")};
    auto capable = create(std::move(opted_in));
    CHECK(capable.find("output")->find("video")->find("transform")->asString() == "copy");
    CHECK(capable.find("output")->find("video")->find("color_transfer")->asString() == "smpte2084");
    auto streams = capable.find("source")->find("streams")->asArray();
    REQUIRE(!streams.empty());
    CHECK(streams.front().find("color_transfer")->asString() == "smpte2084");
    CHECK(streams.front().find("level")->asInt64() == 153);
}

MACHA_TEST("media_playback", test_forced_direct_bypasses_client_capabilities) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();

    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/test.mp4", 0644, getuid(), getgid());
    auto bytes = pattern(128 * 1024 + 17);
    auto writer = service.filesystem().open_write("/media/test.mp4", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto media_id = file_media_id(service.filesystem().getattr("/media/test.mp4"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.startup_timeout = 2s;
    auto fake_engine = std::make_unique<FakeMediaEngine>();
    auto* fake_engine_ptr = fake_engine.get();
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::move(fake_engine));
    playback.start();

    // Direct is a byte-stream override. Deliberately claim that the client can
    // decode none of the source container/codecs, cannot consume HLS, and has
    // absurd resolution/bitrate limits. None of those negotiation constraints
    // may reject an explicit Direct request.
    Json::Object incompatible_caps{{"containers", Json::Array{Json("webm")}},
                                   {"video_codecs", Json::Array{Json("vp9")}},
                                   {"audio_codecs", Json::Array{Json("opus")}},
                                   {"hls_fmp4", false},
                                   {"max_width", 1},
                                   {"max_height", 1}};
    Json::Object direct_prefs{{"mode", "direct"},
                              {"max_height", 1},
                              {"max_bitrate", static_cast<uint64_t>(1)}};
    Json::Object direct_root{{"media_id", media_id},
                             {"capabilities", Json(std::move(incompatible_caps))},
                             {"preferences", Json(std::move(direct_prefs))}};
    auto direct_text = Json(std::move(direct_root)).dump();
    HttpRequest direct_create;
    direct_create.method = "POST";
    direct_create.path = "/api/v1/playback/sessions";
    direct_create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    direct_create.body.assign(direct_text.begin(), direct_text.end());
    auto direct_created = playback.handle(direct_create);
    REQUIRE(direct_created.status == 201);
    auto direct_json = Json::parse(std::string(direct_created.body.begin(), direct_created.body.end()));
    CHECK(direct_json.find("mode")->asString() == "direct");
    CHECK(direct_json.find("stream")->find("url")->asString().ends_with("/direct"));
    auto direct_modes = direct_json.find("options")->find("modes")->asArray();
    CHECK(std::any_of(direct_modes.begin(), direct_modes.end(), [](const Json& mode) {
        return mode.asString() == "direct";
    }));
    CHECK(fake_engine_ptr->vod_prepares() == 0);
    CHECK(fake_engine_ptr->started_plans().empty());

    HttpRequest direct_range;
    direct_range.method = "GET";
    direct_range.path = direct_json.find("stream")->find("url")->asString();
    direct_range.headers["range"] = "bytes=123-1122";
    auto direct_range_response = playback.handle(direct_range);
    REQUIRE(direct_range_response.status == 206);
    REQUIRE(direct_range_response.stream != nullptr);
    CHECK(direct_range_response.content_length() == 1000);
    Bytes direct_bytes(1000);
    REQUIRE(direct_range_response.stream->read(0, direct_bytes) == direct_bytes.size());
    CHECK(std::equal(direct_bytes.begin(), direct_bytes.end(), bytes.begin() + 123));

    HttpRequest remove_direct;
    remove_direct.method = "DELETE";
    remove_direct.path = "/api/v1/playback/sessions/" + direct_json.find("session_id")->asString();
    CHECK(playback.handle(remove_direct).status == 204);

    // Auto still negotiates normally. Make MP4 direct-play incompatible while
    // keeping fMP4 remux compatible: the session must start as remux, advertise
    // Direct unconditionally, and honour an explicit switch to Direct.
    Json::Object remux_caps{{"containers", Json::Array{Json("webm")}},
                            {"video_codecs", Json::Array{Json("h264")}},
                            {"audio_codecs", Json::Array{Json("aac")}},
                            {"hls_fmp4", true}};
    Json::Object remux_root{{"media_id", media_id},
                            {"capabilities", Json(std::move(remux_caps))}};
    auto remux_text = Json(std::move(remux_root)).dump();
    HttpRequest remux_create;
    remux_create.method = "POST";
    remux_create.path = "/api/v1/playback/sessions";
    remux_create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
    remux_create.body.assign(remux_text.begin(), remux_text.end());
    auto remux_created = playback.handle(remux_create);
    REQUIRE(remux_created.status == 201);
    auto remux_json = Json::parse(std::string(remux_created.body.begin(), remux_created.body.end()));
    CHECK(remux_json.find("mode")->asString() == "remux");
    auto remux_modes = remux_json.find("options")->find("modes")->asArray();
    CHECK(std::any_of(remux_modes.begin(), remux_modes.end(), [](const Json& mode) {
        return mode.asString() == "direct";
    }));

    Json::Object switch_preferences{{"mode", "direct"}};
    Json::Object switch_root{{"preferences", Json(std::move(switch_preferences))}};
    auto switch_text = Json(std::move(switch_root)).dump();
    HttpRequest switch_direct;
    switch_direct.method = "PATCH";
    switch_direct.path = "/api/v1/playback/sessions/" + remux_json.find("session_id")->asString();
    switch_direct.body.assign(switch_text.begin(), switch_text.end());
    auto switched = playback.handle(switch_direct);
    REQUIRE(switched.status == 200);
    auto switched_json = Json::parse(std::string(switched.body.begin(), switched.body.end()));
    CHECK(switched_json.find("mode")->asString() == "direct");
    CHECK(switched_json.find("preferences")->find("mode")->asString() == "direct");
    CHECK(switched_json.find("stream")->find("url")->asString().ends_with("/direct"));

    HttpRequest remove_switched;
    remove_switched.method = "DELETE";
    remove_switched.path = "/api/v1/playback/sessions/" + switched_json.find("session_id")->asString();
    CHECK(playback.handle(remove_switched).status == 204);

    playback.stop();
    service.stop();
}

MACHA_TEST("media_playback", test_concurrent_transcode_admission_is_reserved) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
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
        request.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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

MACHA_TEST("media_playback", test_logical_viewer_keeps_one_transcode_entitlement_across_replacements) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto c = config_for(t.path() / "node", keyfile, free_port());
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
    c.catalogue.api.enabled = false;
    Service service(c, keys);
    service.start();

    service.filesystem().mkdir("/media", 0755, getuid(), getgid());
    service.filesystem().create_file("/media/logical.mp4", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/media/logical.mp4", true);
    auto bytes = pattern(64 * 1024);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto media_id =
        file_media_id(service.filesystem().getattr("/media/logical.mp4"));

    CatalogueApiConfig api;
    StreamingConfig streaming;
    streaming.enabled = true;
    streaming.temp_path = t.path() / "playback";
    streaming.max_sessions = 4;
    streaming.max_video_transcodes = 1;
    streaming.max_audio_transcodes = 1;
    streaming.startup_timeout = 2s;
    PlaybackManager playback(service.filesystem(), service.catalogue(), api, streaming,
                             std::make_unique<FakeMediaEngine>());
    playback.start();

    auto create = [&](std::string mode, std::string viewer, std::string attempt,
                      std::optional<int64_t> seek_ms = {}) {
        Json::Object preferences{{"mode", std::move(mode)}};
        Json::Object root{{"media_id", media_id},
                          {"preferences", Json(std::move(preferences))}};
        if (seek_ms) root["seek_ms"] = *seek_ms;
        auto text = Json(std::move(root)).dump();
        HttpRequest request;
        request.method = "POST";
        request.path = "/api/v1/playback/sessions";
        request.session = SessionIdentity{.id = viewer, .roles = {"anonymous"}};
        request.query["idempotency_key"] = std::move(attempt);
        request.body.assign(text.begin(), text.end());
        return playback.handle(request);
    };
    auto status = [&] {
        HttpRequest request;
        request.method = "GET";
        request.path = "/api/v1/playback/status";
        auto response = playback.handle(request);
        REQUIRE(response.status == 200);
        return Json::parse(std::string(response.body.begin(), response.body.end()));
    };

    auto first = create("transcode", "ui-player-1", "logical-attempt-1");
    REQUIRE(first.status == 201);
    auto first_json = Json::parse(std::string(first.body.begin(), first.body.end()));
    const auto session_id = first_json.find("session_id")->asString();
    CHECK(status().find("video_transcodes")->asUInt64() == 1);

    HttpResponse concurrent_direct, concurrent_remux;
    std::jthread replace_a([&] {
        concurrent_direct =
            create("direct", "ui-player-1", "logical-concurrent-direct");
    });
    std::jthread replace_b([&] {
        concurrent_remux =
            create("remux", "ui-player-1", "logical-concurrent-remux");
    });
    replace_a.join();
    replace_b.join();
    REQUIRE(concurrent_direct.status == 201);
    REQUIRE(concurrent_remux.status == 201);
    for (const auto* response : {&concurrent_direct, &concurrent_remux}) {
        auto body = Json::parse(std::string(response->body.begin(), response->body.end()));
        CHECK(body.find("session_id")->asString() == session_id);
    }
    CHECK(status().find("sessions")->asUInt64() == 1);
    CHECK(status().find("video_transcodes")->asUInt64() == 1);

    // A replacement POST is a new request attempt in the same persistent UI
    // session. It keeps the server session identity and entitlement even while
    // the selected representation temporarily requires no encoder.
    auto direct = create("direct", "ui-player-1", "logical-attempt-2");
    REQUIRE(direct.status == 201);
    auto direct_json = Json::parse(std::string(direct.body.begin(), direct.body.end()));
    CHECK(direct_json.find("session_id")->asString() == session_id);
    CHECK(direct_json.find("mode")->asString() == "direct");
    auto direct_status = status();
    CHECK(direct_status.find("sessions")->asUInt64() == 1);
    CHECK(direct_status.find("video_transcodes")->asUInt64() == 1);

    // Another logical viewer cannot steal the retained slot during that Direct
    // interval, but the original viewer can switch back and seek repeatedly.
    CHECK(create("transcode", "ui-player-2", "other-attempt").status == 429);
    auto remux = create("remux", "ui-player-1", "logical-attempt-3");
    REQUIRE(remux.status == 201);
    auto remux_json = Json::parse(std::string(remux.body.begin(), remux.body.end()));
    CHECK(remux_json.find("session_id")->asString() == session_id);
    CHECK(remux_json.find("mode")->asString() == "remux");
    CHECK(status().find("video_transcodes")->asUInt64() == 1);

    auto transcoded = create("transcode", "ui-player-1", "logical-attempt-4", 10'000);
    REQUIRE(transcoded.status == 201);
    auto transcoded_json =
        Json::parse(std::string(transcoded.body.begin(), transcoded.body.end()));
    CHECK(transcoded_json.find("session_id")->asString() == session_id);
    CHECK(status().find("video_transcodes")->asUInt64() == 1);

    for (int64_t seek_ms : {20'000, 30'000, 40'000}) {
        Json::Object patch_root{{"seek_ms", seek_ms}};
        auto text = Json(std::move(patch_root)).dump();
        HttpRequest patch;
        patch.method = "PATCH";
        patch.path = "/api/v1/playback/sessions/" + session_id;
        patch.body.assign(text.begin(), text.end());
        auto response = playback.handle(patch);
        REQUIRE(response.status == 200);
        CHECK(status().find("video_transcodes")->asUInt64() == 1);
    }

    HttpRequest remove;
    remove.method = "DELETE";
    remove.path = "/api/v1/playback/sessions/" + session_id;
    REQUIRE(playback.handle(remove).status == 204);
    CHECK(status().find("video_transcodes")->asUInt64() == 0);
    CHECK(create("transcode", "ui-player-2", "other-attempt-2").status == 201);

    playback.stop();
    service.stop();
}

MACHA_HEAVY_TEST("media_playback", test_playback_sessions_and_streaming_http_bodies) {
    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto port = free_port();
    auto c = config_for(t.path() / "node", keyfile, port);
    c.replication = 1;
    c.metadata_min_write_replicas = 1;
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
    REQUIRE(playback_status_json.find("probe_cache_entries") != nullptr);
    REQUIRE(playback_status_json.find("probe_cache_bytes") != nullptr);
    CHECK(playback_status_json.find("probe_cache_entries")->asUInt64() <=
          playback_status_json.find("probe_cache_limit_entries")->asUInt64());
    CHECK(playback_status_json.find("probe_cache_bytes")->asUInt64() <=
          playback_status_json.find("probe_cache_limit_bytes")->asUInt64());
    REQUIRE(playback_status_json.find("subtitle_cache_entries") != nullptr);
    REQUIRE(playback_status_json.find("subtitle_cache_bytes") != nullptr);
    REQUIRE(playback_status_json.find("segment_store_resident_bytes") != nullptr);
    REQUIRE(playback_status_json.find("segment_store_spill_bytes") != nullptr);
    REQUIRE(playback_status_json.find("segment_store_descriptor_bytes") != nullptr);
    REQUIRE(playback_status_json.find("segment_store_segments") != nullptr);
    REQUIRE(playback_status_json.find("segment_store_planned_segments") != nullptr);
    REQUIRE(playback_status_json.find("heap_reclaim_pending") != nullptr);
    REQUIRE(playback_status_json.find("heap_reclaim_requests") != nullptr);
    REQUIRE(playback_status_json.find("heap_reclaim_runs") != nullptr);
    REQUIRE(playback_status_json.find("heap_reclaim_successes") != nullptr);

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
    initial_seek.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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
    // Re-probing/re-planning here makes cached
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

    // The web client may include its current preferences in every PATCH.  If
    // those preferences are unchanged, the request is still semantically a
    // seek-only update and must retain the reusable random-access plan.
    Json::Object redundant_seek_preferences{{"mode", "remux"}};
    Json::Object redundant_seek_root{{"seek_ms", 47000},
                                     {"preferences", Json(std::move(redundant_seek_preferences))}};
    auto redundant_seek_text = Json(std::move(redundant_seek_root)).dump();
    HttpRequest redundant_seek;
    redundant_seek.method = "PATCH";
    redundant_seek.path = fast_seek.path;
    redundant_seek.body.assign(redundant_seek_text.begin(), redundant_seek_text.end());
    auto redundant_seek_response = playback.handle(redundant_seek);
    REQUIRE(redundant_seek_response.status == 200);
    auto redundant_seek_json = Json::parse(std::string(redundant_seek_response.body.begin(),
                                                       redundant_seek_response.body.end()));
    CHECK(redundant_seek_json.find("seek_ms")->asInt64() == 47000);
    CHECK(fake_engine_ptr->probes() == probes_before_seek);
    CHECK(fake_engine_ptr->vod_prepares() == prepares_before_seek);
    plans = fake_engine_ptr->started_plans();
    REQUIRE(plans.size() == 3);
    CHECK(plans.back().seek == 47s);

    HttpRequest remove_initial_seek;
    remove_initial_seek.method = "DELETE";
    remove_initial_seek.path = "/api/v1/playback/sessions/" + initial_seek_json.find("session_id")->asString();
    CHECK(playback.handle(remove_initial_seek).status == 204);

    // Reopening the same immutable media with the same transformed plan should
    // reuse both the probe and prepared VOD/random-access plan. The first
    // fragment still belongs to a fresh pipeline generation, but source/index
    // inspection is not repeated merely because the previous session ended.
    const auto probes_before_reopen = fake_engine_ptr->probes();
    const auto prepares_before_reopen = fake_engine_ptr->vod_prepares();
    auto reopened_seek = playback.handle(initial_seek);
    REQUIRE(reopened_seek.status == 201);
    CHECK(fake_engine_ptr->probes() == probes_before_reopen);
    CHECK(fake_engine_ptr->vod_prepares() == prepares_before_reopen);
    auto reopened_seek_json = Json::parse(std::string(reopened_seek.body.begin(),
                                                      reopened_seek.body.end()));
    HttpRequest remove_reopened_seek;
    remove_reopened_seek.method = "DELETE";
    remove_reopened_seek.path = "/api/v1/playback/sessions/" +
                                reopened_seek_json.find("session_id")->asString();
    CHECK(playback.handle(remove_reopened_seek).status == 204);

    Json::Object create_root{{"media_id", media_id}};
    auto create_text = Json(std::move(create_root)).dump();
    HttpRequest create;
    create.method = "POST";
    create.path = "/api/v1/playback/sessions";
    create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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
    // 720p must rebuild the negotiated Auto session at 720p. Remux is not a
    // valid quality-preserving choice here, while Direct remains exposed as the
    // explicit byte-stream override and deliberately ignores quality constraints.
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
    CHECK(std::any_of(quality_modes.begin(), quality_modes.end(), [](const Json& mode) {
        return mode.asString() == "direct";
    }));
    CHECK(std::none_of(quality_modes.begin(), quality_modes.end(), [](const Json& mode) {
        return mode.asString() == "remux";
    }));
    CHECK(std::any_of(quality_modes.begin(), quality_modes.end(), [](const Json& mode) {
        return mode.asString() == "transcode";
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
    unsupported.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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
    second.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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
    path_create.session = SessionIdentity{.id = "", .roles = {"anonymous"}};
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

MACHA_FAST_TEST("media_playback", test_subtitle_text_normalisation) {
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello") == "Hello");
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello, world") == "Hello, world");
    CHECK(plain_ass_subtitle_text("Dialogue: 0,0,Default,,0,0,0,,{\\i1}Hello{\\i0}\\Nworld") ==
          "Hello\nworld");
}

} // namespace

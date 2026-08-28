// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_TEST("media_playback", test_media_segment_store_backpressure_and_spill) {
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
    REQUIRE(!transcode_seek->segment_durations.empty());
    CHECK(std::abs(transcode_seek->segment_durations.front() - 4.0) < 0.0005);

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

MACHA_FAST_TEST("media_playback", test_subtitle_text_normalisation) {
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello") == "Hello");
    CHECK(plain_ass_subtitle_text("0,0,Default,,0,0,0,,Hello, world") == "Hello, world");
    CHECK(plain_ass_subtitle_text("Dialogue: 0,0,Default,,0,0,0,,{\\i1}Hello{\\i0}\\Nworld") ==
          "Hello\nworld");
}

} // namespace

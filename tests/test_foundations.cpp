// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "fuse_mountpoint.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_FAST_TEST("foundations", test_codec_and_crypto) {
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

MACHA_FAST_TEST("foundations", test_durable_replace_file_matrix) {
    TempDir t;
    const auto path = t.path() / "nested" / "state.json";
    const std::array<std::string, 4> contents{{
        "",
        "one",
        std::string(4096, 'x'),
        std::string("contains\nnewlines\nand\0binary", 28),
    }};

    for (const auto& expected : contents) {
        durable_replace_file(path, expected);
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::string actual((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        CHECK(actual == expected);

        size_t temporaries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
            if (entry.path().filename().string().starts_with("state.json.tmp.")) ++temporaries;
        }
        CHECK(temporaries == 0);
    }
}

MACHA_FAST_TEST("foundations", test_torrent_uri_safety_matrix) {
    struct UrlCase {
        std::string_view value;
        bool accepted;
    };
    constexpr std::array<UrlCase, 10> urls{{
        {"http://tracker.example/file.torrent", true},
        {"https://tracker.example/file.torrent?token=x", true},
        {"http://127.0.0.1:8080/file.torrent", true},
        {"https://[::1]/file.torrent", true},
        {"ftp://tracker.example/file.torrent", false},
        {"file:///tmp/file.torrent", false},
        {"magnet:?xt=urn:btih:abc", false},
        {"/relative/file.torrent", false},
        {"https://", false},
        {"HTTP://tracker.example/file.torrent", false},
    }};
    for (const auto& test_case : urls)
        CHECK(safe_torrent_fetch_url(test_case.value) == test_case.accepted);

    struct MagnetCase {
        std::string_view value;
        bool accepted;
    };
    constexpr std::array<MagnetCase, 8> magnets{{
        {"magnet:?xt=urn:btih:0123456789abcdef", true},
        {"magnet:?xt=urn:btmh:1220123456789abcdef", true},
        {"magnet:?dn=name-only", false},
        {"magnet:?xt=urn:sha1:0123456789abcdef", false},
        {"MAGNET:?xt=urn:btih:0123456789abcdef", false},
        {"http://example.test/file.torrent", false},
        {"", false},
        {"magnet:?xt=", false},
    }};
    for (const auto& test_case : magnets)
        CHECK(sanitize_magnet_uri(test_case.value).has_value() == test_case.accepted);

    auto sanitized = sanitize_magnet_uri(
        "magnet:?xt=urn%3Abtih%3A0123456789abcdef"
        "&dn=%20Example%01%20Name%20"
        "&tr=https%3A%2F%2Ftracker.example%2Fannounce"
        "&tr=udp%3A%2F%2Ftracker.example%3A80%2Fannounce"
        "&tr=file%3A%2F%2F%2Fetc%2Fpasswd"
        "&xs=https%3A%2F%2Funtrusted.example%2Fpayload");
    REQUIRE(sanitized.has_value());
    CHECK(sanitized->starts_with("magnet:?xt=urn%3Abtih%3A0123456789abcdef"));
    CHECK(sanitized->find("dn=Example%20Name") != std::string::npos);
    CHECK(sanitized->find("tracker.example") != std::string::npos);
    CHECK(sanitized->find("file%3A") == std::string::npos);
    CHECK(sanitized->find("xs=") == std::string::npos);

    constexpr std::array<TorrentJobState, 12> states{{
        TorrentJobState::queued,
        TorrentJobState::metadata,
        TorrentJobState::downloading,
        TorrentJobState::verifying,
        TorrentJobState::downloaded,
        TorrentJobState::importing,
        TorrentJobState::cataloguing,
        TorrentJobState::paused,
        TorrentJobState::blocked,
        TorrentJobState::completed,
        TorrentJobState::cancelled,
        TorrentJobState::failed,
    }};
    for (const auto state : states) {
        const auto encoded = torrent_job_state_name(state);
        const auto decoded = parse_torrent_job_state(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == state);
    }
    CHECK(!parse_torrent_job_state("unknown").has_value());

    constexpr std::array<IngestJobState, 9> ingest_states{{
        IngestJobState::queued,
        IngestJobState::scanning,
        IngestJobState::importing,
        IngestJobState::cataloguing,
        IngestJobState::paused,
        IngestJobState::blocked,
        IngestJobState::completed,
        IngestJobState::cancelled,
        IngestJobState::failed,
    }};
    for (const auto state : ingest_states) {
        const auto encoded = ingest_job_state_name(state);
        const auto decoded = parse_ingest_job_state(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == state);
    }
    CHECK(!parse_ingest_job_state("unknown").has_value());

    constexpr std::array<CatalogueHintState, 6> hint_states{{
        CatalogueHintState::queued,
        CatalogueHintState::processing,
        CatalogueHintState::deferred,
        CatalogueHintState::catalogued,
        CatalogueHintState::no_match,
        CatalogueHintState::failed,
    }};
    for (const auto state : hint_states) {
        const auto encoded = catalogue_hint_state_name(state);
        const auto decoded = parse_catalogue_hint_state(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == state);
    }
    CHECK(!parse_catalogue_hint_state("unknown").has_value());
}

MACHA_TEST("foundations", test_thread_cpu_reporter_debug_escalation) {
    auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::debug);
    Log::set_logger(capture);

    ThreadCpuReporter reporter("macha-test-hot", 25ms, true);
    const auto busy_until = Clock::now() + 100ms;
    while (Clock::now() < busy_until)
        reporter.tick();

    auto records = capture->records();
    const auto high = std::find_if(records.begin(), records.end(), [](const auto& record) {
        return record.first == LogLevel::debug &&
               record.second.find("DIAG high thread CPU name=macha-test-hot") != std::string::npos;
    });
    CHECK(high != records.end());

    std::this_thread::sleep_for(50ms);
    reporter.tick();
    records = capture->records();
    const auto recovered = std::find_if(records.begin(), records.end(), [](const auto& record) {
        return record.first == LogLevel::debug &&
               record.second.find("DIAG thread CPU recovered name=macha-test-hot") != std::string::npos;
    });
    CHECK(recovered != records.end());

    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
}

MACHA_FAST_TEST("foundations", test_config) {
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
    CHECK(fuse_defaults.recovery_commit_workers == 2);
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
    auto fuse_spool = t.path() / "fuse-spool";
    auto fuse_journal = t.path() / "fuse-journal" / "operations.log";
    {
        std::ofstream out(yaml);
        out << "state_path: " << state.string() << "\n"
            << "key_file: " << keyfile << "\n"
            << "log_level: WARN\n"
            << "ffmpeg_log_level: DEBUG\n"
            << "storage:\n"
            << "  data:\n"
            << "    backends:\n"
            << "      - path: " << disk1.string() << "\n"
            << "        limit: 10T\n"
            << "        reserve_free: 2G\n"
            << "      - path: " << disk2.string() << "\n"
            << "        limit: 10T\n"
            << "        reserve_free: 2G\n"
            << "    packing:\n"
            << "      threshold: 512K\n"
            << "      target_size: 32M\n"
            << "  metadata:\n"
            << "    path: " << (state / "control").string() << "\n"
            << "    limit: 2G\n"
            << "    packing:\n"
            << "      threshold: 256K\n"
            << "      target_size: 16M\n"
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
            << "  unmount_if_mounted: true\n"
            << "  spool_path: " << fuse_spool.string() << "\n"
            << "  operation_journal_path: " << fuse_journal.string() << "\n"
            << "  entry_timeout_ms: 375\n"
            << "  attr_timeout_ms: 225\n"
            << "  negative_timeout_ms: 75\n"
            << "  absolute_request_timeout_ms: 14000\n"
            << "  request_workers: 18\n"
            << "  max_pending_requests: 2048\n"
            << "  commit_workers: 4\n"
            << "  recovery_commit_workers: 3\n"
            << "  foreground_commit_workers: 2\n"
            << "  publication_quiet_ms: 425\n"
            << "  max_pending_operations: 1024\n"
            << "  hydration_priority: 2500\n"
            << "  read_ahead_extents: 4\n"
            << "  hint_lifetime_ms: 4500\n"
            << "  write_through_cache: false\n"
            << "  refresh_interval_ms: 750\n" // legacy key: accepted and ignored
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
            << "    client_io_timeout_ms: 45000\n"
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
    CHECK(yc.storage_backends[0].reserve_free == 2ULL * 1024 * 1024 * 1024);
    CHECK(yc.storage_packing.threshold == 512ULL * 1024);
    CHECK(yc.storage_packing.target_size == 32ULL * 1024 * 1024);
    CHECK(yc.metadata_store.path == state / "control");
    CHECK(yc.metadata_store.limit == 2ULL * 1024 * 1024 * 1024);
    CHECK(yc.metadata_store.packing.threshold == 256ULL * 1024);
    CHECK(yc.metadata_store.packing.target_size == 16ULL * 1024 * 1024);
    CHECK(yc.cache.path == cache_dir);
    CHECK(yc.cache.max_blocks == 4096);
    CHECK(yc.log_level == LogLevel::warn);
    CHECK(yc.ffmpeg_log_level == FfmpegLogLevel::debug);
    CHECK(yc.fuse.allow_other);
    CHECK(yc.fuse.unmount_if_mounted);
    REQUIRE(yc.fuse.spool_path.has_value());
    CHECK(*yc.fuse.spool_path == fuse_spool);
    REQUIRE(yc.fuse.operation_journal_path.has_value());
    CHECK(*yc.fuse.operation_journal_path == fuse_journal);
    CHECK(yc.fuse.entry_timeout == 375ms);
    CHECK(yc.fuse.attr_timeout == 225ms);
    CHECK(yc.fuse.negative_timeout == 75ms);
    CHECK(yc.fuse.absolute_request_timeout == 14000ms);
    CHECK(yc.fuse.request_workers == 18);
    CHECK(yc.fuse.max_pending_requests == 2048);
    CHECK(yc.fuse.commit_workers == 4);
    CHECK(yc.fuse.recovery_commit_workers == 3);
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
    CHECK(yc.catalogue.api.client_io_timeout == 45000ms);
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

    for (size_t i = 0; i < 4; ++i) {
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
        else if (i == 2)
            out << "network:\n  data_timeout_ms: 1000\n";
        // i == 3 is deliberately only the pre-0.18 storage sequence. The
        // fresh storage contract rejects it without relying on another
        // obsolete key to trigger the failure.
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

MACHA_FAST_TEST("foundations", test_placement) {
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

MACHA_FAST_TEST("foundations", test_capacity_placement) {
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

} // namespace

MACHA_TEST("foundations", test_fuse_mountpoint_preflight_refuses_unrelated_filesystem) {
#if defined(__linux__) || defined(__APPLE__)
    TempDir temp;
    FuseConfig config;
    config.unmount_if_mounted = true;

    CHECK(probe_macha_mountpoint(temp.path().string()).state == MountTableState::missing);
    prepare_fuse_mountpoint(temp.path(), config);

    CHECK(probe_macha_mountpoint("/").state == MountTableState::other);
    bool refused = false;
    try {
        prepare_fuse_mountpoint("/", config);
    } catch (const std::exception& e) {
        refused = std::string(e.what()).find("non-Macha") != std::string::npos;
    }
    CHECK(refused);
#endif
}

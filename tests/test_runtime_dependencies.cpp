// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

extern "C" {
#include <libavutil/log.h>
}

using namespace macha::test_support;

MACHA_FAST_TEST("runtime_dependencies", test_ffmpeg_log_bridge) {
    auto capture = std::make_shared<CapturingLogger>(LogLevel::info);
    Log::set_logger(capture);
    configure_ffmpeg_logging(FfmpegLogLevel::debug);
    av_log(nullptr, AV_LOG_DEBUG, "runtime dependency FFmpeg debug\n");
    av_log(nullptr, AV_LOG_TRACE, "filtered runtime dependency FFmpeg trace\n");
    CHECK(capture->records.size() == 1);
    if (!capture->records.empty()) {
        CHECK(capture->records.front().first == LogLevel::debug);
        CHECK(capture->records.front().second.find("runtime dependency FFmpeg debug") != std::string::npos);
    }
    configure_ffmpeg_logging(FfmpegLogLevel::error);
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
}

MACHA_FAST_TEST("runtime_dependencies", test_yaml_config) {
    TempDir t;
#ifdef MACHA_HAVE_LIBTORRENT
    constexpr const char* torrent_enabled = "true";
#else
    constexpr const char* torrent_enabled = "false";
#endif
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
            << "  viewer_weight: 91\n"
            << "  loader_weight: 9\n"
            << "  publication_quantum_bytes: 32M\n"
            << "  publication_inflight_bytes: 128M\n"
            << "  publication_pipeline_bytes: 32M\n"
            << "  max_pending_operations: 1024\n"
            << "  namespace_batch_operations: 128\n"
            << "  namespace_batch_bytes: 192K\n"
            << "  max_spool_bytes: 7G\n"
            << "  spool_reserve_free: 768M\n"
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
            << "  upnp:\n"
            << "    enabled: true\n"
            << "    external_port: 17440\n"
            << "    discovery_timeout_ms: 1800\n"
            << "    lease_seconds: 3600\n"
            << "  external_ip:\n"
            << "    enabled: true\n"
            << "    timeout_ms: 2400\n"
            << "  connectivity_check:\n"
            << "    enabled: true\n"
            << "    timeout_ms: 2600\n"
            << "dht:\n"
            << "  replicas: 3\n"
            << "  metadata_replicas: 3\n"
            << "  min_write_replicas: 2\n"
            << "  write_stall_ms: 1750\n"
            << "  extent_size: 16M\n"
            << "  data_inflight_bytes: 96M\n"
            << "  data_viewer_reserve_bytes: 24M\n"
            << "  read_ahead: 5\n"
            << "  metadata_materialization_cache_bytes: 96M\n"
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
            << "  enabled: " << torrent_enabled << "\n"
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
            << "  pipeline_idle_ms: 45000\n"
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
    CHECK(yc.metadata_materialization_cache_bytes == 96ULL * 1024 * 1024);
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
    CHECK(yc.fuse.viewer_weight == 91);
    CHECK(yc.fuse.loader_weight == 9);
    CHECK(yc.fuse.publication_quantum_bytes == 32ULL * 1024 * 1024);
    CHECK(yc.fuse.publication_inflight_bytes == 128ULL * 1024 * 1024);
    CHECK(yc.fuse.publication_pipeline_bytes == 32ULL * 1024 * 1024);
    CHECK(yc.fuse.max_pending_operations == 1024);
    CHECK(yc.fuse.namespace_batch_operations == 128);
    CHECK(yc.fuse.namespace_batch_bytes == 192 * 1024);
    CHECK(yc.fuse.max_spool_bytes == 7ULL * 1024 * 1024 * 1024);
    CHECK(yc.fuse.spool_reserve_free == 768ULL * 1024 * 1024);
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
    CHECK(yc.upnp.enabled);
    CHECK(yc.upnp.external_port == 17440);
    CHECK(yc.upnp.discovery_timeout == 1800ms);
    CHECK(yc.upnp.lease_seconds == 3600);
    CHECK(yc.external_ip.enabled);
    CHECK(yc.external_ip.timeout == 2400ms);
    CHECK(yc.connectivity_check.enabled);
    CHECK(yc.connectivity_check.timeout == 2600ms);
    CHECK(yc.metadata_min_write_replicas == 2); // legacy metadata_replicas: 3 -> old 2-vote floor
    CHECK(yc.min_write_replicas == 2);
    CHECK(yc.write_stall == 1750ms);
    CHECK(yc.data_inflight_bytes == 96ULL * 1024 * 1024);
    CHECK(yc.data_viewer_reserve_bytes == 24ULL * 1024 * 1024);
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
    CHECK(yc.torrent.enabled == (std::string_view(torrent_enabled) == "true"));
    CHECK(yc.torrent.search_providers.empty());
    CHECK(yc.streaming.enabled);
    REQUIRE(yc.streaming.temp_path.has_value());
    CHECK(*yc.streaming.temp_path == t.path() / "streams");
    CHECK(yc.streaming.max_sessions == 9);
    CHECK(yc.streaming.max_video_transcodes == 2);
    CHECK(yc.streaming.max_audio_transcodes == 5);
    CHECK(yc.streaming.session_idle == 60000ms);
    CHECK(yc.streaming.pipeline_idle == 45000ms);
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

}

MACHA_HEAVY_TEST("runtime_dependencies", test_embedded_music_metadata_and_artwork) {
    TempDir t;
    auto key = t.path() / "cluster.key";
    write_key(key);
    auto config = config_for(t.path() / "disk", key, free_port());
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.metadata_cache = 20ms;
    auto keys = load_cluster_keys(key);
    Service service(config, keys);
    service.start();

    service.filesystem().mkdir("/Music", 0755, getuid(), getgid());
    const std::string collection = "/Music/Scooter Full Discography (Albums & Singles 1994-2011)";
    service.filesystem().mkdir(collection, 0755, getuid(), getgid());
    const std::string singles = collection + "/Singles";
    service.filesystem().mkdir(singles, 0755, getuid(), getgid());
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

    FakeHttpClient music_probe_http;
    CatalogueMusicProviderConfig music_source_config;
    music_source_config.roots = {"/Music"};
    music_source_config.musicbrainz.enabled = false;
    MusicScanProvider music_source(music_probe_http, music_source_config);

    const std::string tagged_album = singles + "/14 - [1996] I'm Raving The Remixes CDM";
    service.filesystem().mkdir(tagged_album, 0755, getuid(), getgid());
    const std::string tagged_path = tagged_album + "/01 - Completely Wrong.mp3";

    const auto tagged_bytes = fixture_bytes("tagged.mp3");
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


}

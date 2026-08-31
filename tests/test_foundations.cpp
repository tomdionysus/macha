// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_mountpoint.hpp"
#include "miniupnpc_compat.hpp"
#include "test_backend_support.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_FAST_TEST("foundations", test_spool_retirement_rate_is_aggregate) {
    SpoolRetirementRateEstimator rate;
    const SpoolRetirementRateEstimator::TimePoint start{};
    rate.start(start);

    auto first = rate.retire(100, start + 10s);
    REQUIRE(first.has_value());
    CHECK(first->bytes == 100);
    CHECK(first->elapsed == 10s);
    CHECK(static_cast<uint64_t>(first->bytes_per_second) == 10);

    // A concurrent completion shares the same wall-clock denominator. The old
    // per-file EMA would still report 10 B/s here; aggregate retirement is 20.
    auto concurrent = rate.retire(100, start + 10s);
    REQUIRE(concurrent.has_value());
    CHECK(concurrent->bytes == 200);
    CHECK(concurrent->elapsed == 10s);
    CHECK(static_cast<uint64_t>(concurrent->bytes_per_second) == 20);

    auto later = rate.retire(200, start + 20s);
    REQUIRE(later.has_value());
    CHECK(later->bytes == 400);
    CHECK(static_cast<uint64_t>(later->bytes_per_second) == 20);

    rate.reset();
    CHECK(!rate.retire(50, start + 30s).has_value());
    auto restarted = rate.retire(50, start + 31s);
    REQUIRE(restarted.has_value());
    CHECK(restarted->bytes == 100);
    CHECK(static_cast<uint64_t>(restarted->bytes_per_second) == 100);
}

MACHA_FAST_TEST("foundations", test_weighted_loader_service_is_work_conserving_and_non_starving) {
    WeightedLoaderService service(95, 5, 25ms);
    const WeightedLoaderService::TimePoint start{};

    REQUIRE(service.can_start(start, true));
    service.started(start, true);
    CHECK(!service.should_yield(start + 24ms, true));
    CHECK(service.should_yield(start + 25ms, true));
    const auto cooldown = service.finished(start + 25ms, true);
    CHECK(cooldown == 475ms);
    CHECK(!service.can_start(start + 499ms, true));
    CHECK(service.can_start(start + 500ms, true));

    // Viewer absence immediately lends the entire resource to the loader,
    // regardless of an outstanding contended cooldown.
    CHECK(service.can_start(start + 100ms, false));

    // A viewer arriving during an unrestricted loader quantum causes a bounded
    // yield, then a finite proportional cooldown rather than indefinite arrest.
    service.started(start + 1s, false);
    CHECK(service.should_yield(start + 1010ms, true));
    CHECK(service.finished(start + 1020ms, true) == 190ms);
    CHECK(!service.can_start(start + 1209ms, true));
    CHECK(service.can_start(start + 1210ms, true));
}

MACHA_FAST_TEST("foundations", test_miniupnpc_igd_status_compatibility) {
    using namespace miniupnpc_compat;

    CHECK(usable(17, connected_igd));
    CHECK(!private_wan(17, private_wan_igd));
    CHECK(!usable(17, private_wan_igd));

    CHECK(usable(18, connected_igd));
    CHECK(!private_wan(18, connected_igd));
    CHECK(usable(18, private_wan_igd));
    CHECK(private_wan(18, private_wan_igd));
    CHECK(!usable(18, 3));
    CHECK(!usable(18, 4));
}

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
    advertised.metadata_write_replicas_required = 2;
    Writer node_writer;
    encode_node_info(node_writer, advertised);
    Reader node_reader(node_writer.data());
    auto decoded_node = decode_node_info(node_reader);
    node_reader.finish();
    CHECK(decoded_node.id == advertised.id);
    CHECK(decoded_node.host == advertised.host);
    CHECK(decoded_node.failure_domain == advertised.failure_domain);
    CHECK(decoded_node.metadata_generation == 42);
    CHECK(decoded_node.metadata_write_replicas_required == 2);

    NodeTelemetry telemetry;
    telemetry.node_id = advertised.id;
    telemetry.boot_id = random_node_id();
    telemetry.sequence = 9;
    telemetry.observed_unix_ms = 123456789;
    telemetry.version = "0.18.2";
    telemetry.host = advertised.host;
    telemetry.failure_domain = advertised.failure_domain;
    telemetry.port = advertised.port;
    telemetry.storage_capacity = advertised.capacity;
    telemetry.storage_used = advertised.used;
    telemetry.cache_capacity = 1024;
    telemetry.cache_used = 256;
    telemetry.metadata_generation = advertised.metadata_generation;
    telemetry.uptime_ms = 60000;
    telemetry.rss_bytes = 4096;
    telemetry.process_cpu_milli_percent = 1250;
    telemetry.load1_milli = 375;
    telemetry.storage_backends_online = 2;
    telemetry.peers_known = 3;
    telemetry.peers_active = 2;
    telemetry.rpc_connections_reused = 7;
    CHECK(decode_node_telemetry(encode_node_telemetry(telemetry)) == telemetry);
    auto telemetry_set = decode_telemetry_set(encode_telemetry_set({telemetry}));
    REQUIRE(telemetry_set.size() == 1);
    CHECK(telemetry_set.front() == telemetry);

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
    CHECK(
        aes_gcm_open(keys.auth, empty_sealed.nonce, empty_sealed.tag, empty_sealed.ciphertext, aad)
            .empty());

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

MACHA_FAST_TEST("foundations", test_membership_identity_reset_tombstone) {
    NodeInfo self;
    self.id = random_node_id();
    self.host = "self.example";
    self.port = 57401;
    Membership membership(self, 30s);

    NodeInfo stale;
    stale.id = random_node_id();
    stale.host = "10.44.1.50";
    stale.port = 57401;
    stale.seen_unix_ms = unix_ms();
    membership.observe(stale, true);
    CHECK(membership.all().size() == 2);

    IdentityAssociationReset reset;
    reset.host = stale.host;
    reset.port = stale.port;
    reset.stale_node_id = stale.id;
    reset.epoch = 1;
    reset.reset_unix_ms = unix_ms();
    reset.reset_by = self.id;
    REQUIRE(membership.apply_identity_reset(reset));
    CHECK(membership.all().size() == 1);

    // Gossip cannot resurrect the pre-reset endpoint->NodeId association.
    membership.observe(stale, false);
    CHECK(membership.all().size() == 1);

    // A tombstone is a freshness boundary, not a permanent NodeId ban. A
    // directly authenticated post-reset observation can establish the same
    // association again when that really is the node at the endpoint.
    stale.seen_unix_ms = reset.reset_unix_ms + 1;
    membership.observe(stale, true);
    REQUIRE(membership.all().size() == 2);

    // A newer reset clears that fresh association again.
    auto reset2 = reset;
    reset2.epoch = 2;
    reset2.reset_unix_ms += 2;
    REQUIRE(membership.apply_identity_reset(reset2));
    CHECK(membership.all().size() == 1);

    // The endpoint itself is not blacklisted: a freshly authenticated
    // replacement NodeId is valid.
    auto replacement = stale;
    replacement.id = random_node_id();
    replacement.seen_unix_ms = reset2.reset_unix_ms + 1;
    membership.observe(replacement, true);
    REQUIRE(membership.all().size() == 2);
    CHECK(membership.all().back().id == replacement.id);

    // Older/equal reset epochs cannot roll the tombstone backwards.
    CHECK(!membership.apply_identity_reset(reset));
    CHECK(!membership.apply_identity_reset(reset2));
}

MACHA_FAST_TEST("foundations", test_membership_identity_reset_is_durable_without_metadata) {
    TempDir t;
    const auto roster = t.path() / "membership" / "known-nodes.bin";

    NodeInfo self;
    self.id = random_node_id();
    self.host = "127.0.0.1";
    self.port = 57401;

    NodeInfo stale;
    stale.id = random_node_id();
    stale.host = "10.44.1.50";
    stale.port = 7437;
    stale.seen_unix_ms = unix_ms();

    IdentityAssociationReset reset;
    reset.host = stale.host;
    reset.port = stale.port;
    reset.stale_node_id = stale.id;
    reset.epoch = 1;
    reset.reset_unix_ms = stale.seen_unix_ms + 1;
    reset.reset_by = self.id;
    reset.reason = "metadata unavailable recovery";

    {
        Membership membership(self, 30s, roster);
        membership.observe(stale, true);
        REQUIRE(membership.all().size() == 2);
        REQUIRE(membership.apply_identity_reset(reset));
        CHECK(membership.all().size() == 1);
    }

    {
        Membership recovered(self, 30s, roster);
        REQUIRE(recovered.identity_resets().size() == 1);
        CHECK(recovered.identity_resets().front() == reset);
        CHECK(recovered.all().size() == 1);

        // Restart cannot restore the invalidated roster entry, and old gossip
        // remains fenced without requiring a readable metadata snapshot.
        recovered.observe(stale, false);
        CHECK(recovered.all().size() == 1);

        stale.seen_unix_ms = reset.reset_unix_ms + 1;
        recovered.observe(stale, true);
        REQUIRE(recovered.all().size() == 2);
    }

    // A genuinely fresh, directly authenticated association survives another
    // restart; the persisted observation time distinguishes it from stale data.
    Membership recovered_fresh(self, 30s, roster);
    const auto fresh_members = recovered_fresh.all();
    REQUIRE(fresh_members.size() == 2);
    CHECK(std::any_of(fresh_members.begin(), fresh_members.end(),
                      [&](const NodeInfo& node) { return node.id == stale.id; }));
}

MACHA_FAST_TEST("foundations", test_membership_persists_gc_fence_and_requires_direct_reachability) {
    TempDir t;
    const auto roster = t.path() / "membership" / "known-nodes.bin";

    NodeInfo self;
    self.id = random_node_id();
    self.host = "127.0.0.1";
    self.port = 57401;

    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "127.0.0.2";
    peer.port = 57402;
    peer.seen_unix_ms = unix_ms();

    {
        Membership membership(self, 40ms, roster);
        CHECK(membership.all_known_reachable());

        // Gossip may teach us that a node exists, but it is deliberately not
        // proof that the node is reachable for destructive GC.
        membership.observe(peer, false);
        REQUIRE(membership.all().size() == 2);
        CHECK(!membership.all_known_reachable());

        membership.observe(peer, true);
        CHECK(membership.all_known_reachable());
        REQUIRE(std::filesystem::exists(roster));
    }

    // A recovering isolated node must remember the peer before it has had a
    // chance to rediscover the cluster. Persisted members therefore restart as
    // GC fences until this process directly authenticates them again.
    {
        Membership recovered(self, 40ms, roster);
        REQUIRE(recovered.all().size() == 2);
        CHECK(!recovered.all_known_reachable());

        peer.seen_unix_ms = unix_ms();
        recovered.observe(peer, true);
        CHECK(recovered.all_known_reachable());
        std::this_thread::sleep_for(60ms);
        CHECK(!recovered.all_known_reachable());
    }
}

MACHA_FAST_TEST("foundations", test_telemetry_identity_reset_freshness_boundary) {
    const auto self = random_node_id();
    TelemetryStore store(self);

    NodeTelemetry peer;
    peer.node_id = random_node_id();
    peer.boot_id = random_node_id();
    peer.sequence = 1;
    peer.observed_unix_ms = unix_ms();
    peer.host = "10.44.1.50";
    peer.port = 57401;
    store.observe(peer, true);
    REQUIRE(store.all().size() == 1);

    IdentityAssociationReset reset;
    reset.host = peer.host;
    reset.port = peer.port;
    reset.stale_node_id = peer.node_id;
    reset.epoch = 1;
    reset.reset_unix_ms = peer.observed_unix_ms + 1;
    reset.reset_by = self;
    store.apply_identity_reset(reset);
    CHECK(store.all().empty());

    // Pre-reset gossip remains suppressed.
    peer.sequence = 2;
    store.observe(peer, false);
    CHECK(store.all().empty());

    // Fresh direct telemetry can establish the association again.
    peer.sequence = 3;
    peer.observed_unix_ms = reset.reset_unix_ms + 1;
    store.observe(peer, true);
    REQUIRE(store.all().size() == 1);
    CHECK(store.all().front().sequence == 3);
}

MACHA_FAST_TEST("foundations", test_membership_ip_identity_reset_without_node_id) {
    NodeInfo self;
    self.id = random_node_id();
    self.host = "self.example";
    self.port = 57401;
    Membership membership(self, 30s);

    const auto before_reset = unix_ms();
    NodeInfo first;
    first.id = random_node_id();
    first.host = "10.44.1.50";
    first.port = 57401;
    first.seen_unix_ms = before_reset;
    NodeInfo second = first;
    second.id = random_node_id();
    second.port = 57402;
    NodeInfo other = first;
    other.id = random_node_id();
    other.host = "10.44.1.51";
    membership.observe(first, true);
    membership.observe(second, true);
    membership.observe(other, true);
    REQUIRE(membership.all().size() == 4);

    IdentityAssociationReset reset;
    reset.host = "10.44.1.50";
    reset.port = 0;           // every endpoint on this IP
    reset.stale_node_id = {}; // NodeId unknown
    reset.epoch = 1;
    reset.reset_unix_ms = before_reset + 1;
    reset.reset_by = self.id;
    REQUIRE(membership.apply_identity_reset(reset));

    auto after = membership.all();
    CHECK(after.size() == 2);
    CHECK(std::any_of(after.begin(), after.end(),
                      [&](const NodeInfo& node) { return node.id == other.id; }));

    // Gossip containing a pre-reset observation cannot reintroduce either old
    // association, even when the administrator did not know their NodeIds.
    membership.observe(first, false);
    membership.observe(second, false);
    CHECK(membership.all().size() == 2);

    // The IP is not blacklisted. Fresh authentication may establish a new
    // identity, and subsequent gossip carrying a post-reset observation is valid.
    auto replacement = first;
    replacement.id = random_node_id();
    replacement.seen_unix_ms = reset.reset_unix_ms + 1;
    membership.observe(replacement, true);
    REQUIRE(membership.all().size() == 3);
    membership.observe(replacement, false);
    CHECK(membership.all().size() == 3);
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
            if (entry.path().filename().string().starts_with("state.json.tmp."))
                ++temporaries;
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

    auto sanitized = sanitize_magnet_uri("magnet:?xt=urn%3Abtih%3A0123456789abcdef"
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

    // The final high-CPU reporting interval can contain a short unreported
    // busy tail before the sleep starts. Give the reporter more than one idle
    // interval so the assertion does not depend on exactly where that final
    // sampling boundary landed.
    bool recovered = false;
    for (int attempt = 0; attempt < 4 && !recovered; ++attempt) {
        std::this_thread::sleep_for(35ms);
        reporter.tick();
        records = capture->records();
        recovered = std::any_of(records.begin(), records.end(), [](const auto& record) {
            return record.first == LogLevel::debug &&
                   record.second.find("DIAG thread CPU recovered name=macha-test-hot") !=
                       std::string::npos;
        });
    }
    CHECK(recovered);

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

    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::warn));
    CHECK(!Log::enabled(LogLevel::all));
    CHECK(!Log::enabled(LogLevel::debug));
    CHECK(!Log::enabled(LogLevel::info));
    CHECK(Log::enabled(LogLevel::warn));
    CHECK(Log::enabled(LogLevel::error));
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
    CHECK(!Config{}.upnp.enabled);
    CHECK(Config{}.upnp.external_port == 0);
    CHECK(Config{}.upnp.discovery_timeout == 2000ms);
    CHECK(!Config{}.external_ip.enabled);
    CHECK(!Config{}.connectivity_check.enabled);
    MaintenanceConfig maintenance_policy;
    CHECK(maintenance_policy.interval == 1000ms);
    CHECK(maintenance_policy.idle_bandwidth_fraction == 0.10);
    CHECK(maintenance_policy.cpu_target == 0.10);
    CHECK(maintenance_policy.scrub_fraction == 0.02);
    CHECK(maintenance_policy.scrub_interval == std::chrono::hours(24 * 30));
    CHECK(maintenance_policy.no_progress_backoff == 300000ms);
    FuseConfig fuse_defaults;
    CHECK(fuse_defaults.recovery_commit_workers == 2);
    CHECK(fuse_defaults.publication_quantum_bytes == 32ULL * 1024 * 1024);
    CHECK(fuse_defaults.publication_inflight_bytes == 256ULL * 1024 * 1024);
    CHECK(fuse_defaults.publication_pipeline_bytes == 0);
    CHECK(fuse_defaults.viewer_weight == 95);
    CHECK(fuse_defaults.loader_weight == 5);
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

    std::vector<NodeInfo> asymmetric{make_node(1, 10 * TiB), make_node(2, 10 * TiB),
                                     make_node(3, 8 * GiB)};

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
    auto at_end =
        capacity_placement_nodes(shard_id(std::numeric_limits<uint32_t>::max()), asymmetric, 2);
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

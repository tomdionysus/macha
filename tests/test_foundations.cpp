// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/public_connectivity.hpp"
#include "coverage.hpp"
#include "fuse/fuse_mountpoint.hpp"
#include "macha_version.hpp"
#include "retained_memory.hpp"
#include "supervised.hpp"
#include "test_backend_support.hpp"
#include "write_behind.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

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

    // A concurrent completion shares the same wall-clock denominator, so the
    // aggregate rate doubles.
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

    // With no viewer the loader may start despite an outstanding cooldown.
    CHECK(service.can_start(start + 100ms, false));

    // A viewer arriving mid-quantum forces a yield, then a finite proportional
    // cooldown.
    service.started(start + 1s, false);
    CHECK(service.should_yield(start + 1010ms, true));
    CHECK(service.finished(start + 1020ms, true) == 190ms);
    CHECK(!service.can_start(start + 1209ms, true));
    CHECK(service.can_start(start + 1210ms, true));

    // Cold writer setup is active but not charged as service: a slow setup
    // would otherwise spend the slice and be multiplied by the cooldown ratio.
    WeightedLoaderService cold_service(95, 5, 25ms);
    cold_service.started(start, true, false);
    cold_service.service_started(start + 10s, true);
    CHECK(!cold_service.should_yield(start + 10024ms, true));
    CHECK(cold_service.should_yield(start + 10025ms, true));
    CHECK(cold_service.finished(start + 10025ms, true) == 475ms);
}

MACHA_TEST("foundations", test_data_resource_arbiter_background_effort_ceiling) {
    // Byte capacity would admit four loader leases; the effort ceiling
    // admits two at a time and never counts viewers.
    DataResourceArbiter resources(64, 8, 2);
    DataWorkContext loader(FrameType::loader, 1);
    auto first = resources.try_acquire(loader, 1);
    auto second = resources.try_acquire(loader, 1);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(!resources.try_acquire(loader, 1));
    CHECK(!resources.try_acquire(DataWorkContext(FrameType::speculative, 1), 1));
    auto viewer = resources.try_acquire(DataWorkContext(FrameType::foreground, 1), 1);
    CHECK(viewer);
    auto stats = resources.stats();
    CHECK(stats.background_limit == 2);
    CHECK(stats.background_active == 2);
    CHECK(stats.peak_background_active == 2);
    first.reset();
    auto third = resources.try_acquire(loader, 1);
    CHECK(third);
    CHECK(resources.stats().background_active == 2);
}

MACHA_TEST("foundations", test_data_resource_arbiter_reserves_viewer_headroom) {
    DataResourceArbiter resources(4, 1);
    CHECK(!resources.acquire(DataWorkContext(FrameType::loader), 4));
    CHECK(!resources.acquire(DataWorkContext(FrameType::foreground), 5));
    auto loader_context = DataWorkContext(FrameType::loader, 1);
    auto first = resources.acquire(loader_context, 1);
    auto second = resources.acquire(loader_context, 1);
    auto third = resources.acquire(loader_context, 1);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    auto blocked_loader =
        std::async(std::launch::async, [&] { return resources.acquire(loader_context, 1); });
    CHECK(blocked_loader.wait_for(scaled(20ms)) == std::future_status::timeout);
    auto blocked_speculative = std::async(std::launch::async, [&] {
        return resources.acquire(DataWorkContext(FrameType::speculative, 1), 1);
    });
    CHECK(blocked_speculative.wait_for(scaled(20ms)) == std::future_status::timeout);

    // Lower-class saturation cannot consume the reserved byte; a late viewer
    // starts at once without cancelling loader work in flight.
    auto viewer = resources.acquire(DataWorkContext(FrameType::foreground, 1), 1);
    REQUIRE(viewer.has_value());
    CHECK(blocked_loader.wait_for(scaled(20ms)) == std::future_status::timeout);
    viewer.reset();

    // A deadline ends the wait via the condition variable, not a poll.
    auto expired = resources.acquire(
        DataWorkContext(FrameType::loader, 1, DataWorkContext::Clock::now() + 20ms), 1);
    CHECK(!expired.has_value());

    first.reset();
    REQUIRE(blocked_loader.wait_for(scaled(1s)) == std::future_status::ready);
    auto admitted_loader = blocked_loader.get();
    REQUIRE(admitted_loader.has_value());
    CHECK(blocked_speculative.wait_for(scaled(20ms)) == std::future_status::timeout);
    second.reset();
    REQUIRE(blocked_speculative.wait_for(scaled(1s)) == std::future_status::ready);
    auto admitted_speculative = blocked_speculative.get();
    REQUIRE(admitted_speculative.has_value());

    const auto stats = resources.stats();
    CHECK(stats.capacity_bytes == 4);
    CHECK(stats.viewer_reserve_bytes == 1);
    CHECK(stats.peak_used_bytes == 4);
    CHECK(stats.viewer_admissions == 1);
    CHECK(stats.loader_admissions == 4);
    CHECK(stats.loader_waits == 2);
    CHECK(stats.speculative_admissions == 1);
    CHECK(stats.speculative_waits == 1);
    CHECK(stats.cancelled_waits == 1);
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

    TempDir t;
    auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    auto plain = pattern(65537);
    auto sealed = aes_gcm_seal(keys.storage, plain, keys.cluster_id);
    auto opened =
        aes_gcm_open(keys.storage, sealed.nonce, sealed.tag, sealed.ciphertext, keys.cluster_id);
    CHECK(opened == plain);

    // An empty plaintext with AAD seals to an empty ciphertext, not one sized
    // by the AAD.
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

    // The last high-CPU interval may hold an unreported busy tail; allow more
    // than one idle interval so the result does not depend on sample boundaries.
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
    CHECK(Config{}.runtime.glibc_arena_max == 4);
    CHECK(Config{}.streaming.video_decoder_threads == 2);
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
    // Background work yields to everyone else's CPU, not its own: 0.2 s of
    // process CPU a second, half of it the maintenance thread's, is a load of
    // 0.1, within a 0.1 target.
    CHECK(background_cpu_scale(0.2, 0.1, 1.0, 0.1) == 1.0);
    CHECK(background_cpu_scale(0.2, 0.0, 1.0, 0.1) == 0.5);
    CHECK(background_cpu_scale(0.5, 0.1, 1.0, 0.1) == 0.25);
    CHECK(background_cpu_scale(0.1, 0.3, 1.0, 0.1) == 1.0);
    CHECK(background_cpu_scale(5.0, 0.0, 0.0, 0.1) == 1.0);
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

    // Three nodes, R=2: the 8 GiB node owns only its proportional share, so
    // logical capacity is ~10 TiB, not 8 GiB.
    CHECK(placement_logical_capacity(asymmetric, 2) == 10 * TiB + 4 * GiB);

    // Two nodes, R=2: every byte needs both, so the small node caps it.
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

    // The 8 GiB node's quota is 3,354,133 of 2^32 shards; with these node IDs
    // its interval is the tail of the shard space, so the boundary is exact.
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

    // R=1 is weighted rendezvous: a new node may take shards, but unchanged
    // owners never trade with each other. Sampled deterministically over 2^32.
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

    // Failure-domain diversity outranks raw capacity: one replica must be in
    // site-b.
    std::vector<NodeInfo> domains{make_node(1, 10 * TiB, "site-a"),
                                  make_node(2, 10 * TiB, "site-a"),
                                  make_node(3, 8 * GiB, "site-b")};
    CHECK(placement_logical_capacity(domains, 2) == 8 * GiB);
    std::array<uint8_t, 32> key{};
    auto diverse = capacity_placement_nodes(key, domains, 2);
    REQUIRE(diverse.size() == 3);
    CHECK(diverse[0].failure_domain != diverse[1].failure_domain);
}

MACHA_FAST_TEST("foundations", test_retained_memory_ledger_preserves_priority_headroom) {
    RetainedMemoryLedger ledger(100, 10, 30, 10, 5);
    auto speculative = ledger.try_acquire(WorkClass::speculative, MemoryOwner::cache, 50);
    REQUIRE(speculative.has_value());
    CHECK(!ledger.try_acquire(WorkClass::speculative, MemoryOwner::cache, 1).has_value());

    // Speculative use cannot consume the loader floor; viewer and control
    // reserves stay usable at full lower load.
    auto loader = ledger.try_acquire(WorkClass::loader, MemoryOwner::publication, 10);
    auto viewer = ledger.try_acquire(WorkClass::foreground, MemoryOwner::playback_segment, 30);
    auto control = ledger.try_acquire(WorkClass::control, MemoryOwner::rpc_frame, 10);
    REQUIRE(loader.has_value());
    REQUIRE(viewer.has_value());
    REQUIRE(control.has_value());
    const auto full = ledger.stats();
    CHECK(full.used_bytes == 100);
    CHECK(full.peak_used_bytes == 100);
    CHECK(full.owner_bytes[static_cast<size_t>(MemoryOwner::cache)] == 50);

    control.reset();
    viewer.reset();
    loader.reset();
    speculative.reset();
    CHECK(ledger.stats().used_bytes == 0);
}

MACHA_FAST_TEST("foundations", test_retained_memory_ledger_never_starves_rpc_reassembly) {
    // Publication holds its bytes until a peer confirms the write, and the
    // confirmation must be reassembled into this same ledger. A full
    // durable-lower budget must therefore never block reassembly.
    RetainedMemoryLedger ledger(100, 10, 30, 10, 5);

    auto publication = ledger.try_acquire(WorkClass::loader, MemoryOwner::publication, 60);
    REQUIRE(publication.has_value());

    // The durable-lower budget is full: ordinary lower work is refused.
    CHECK(!ledger.try_acquire(WorkClass::loader, MemoryOwner::publication, 1).has_value());
    CHECK(!ledger.try_acquire(WorkClass::speculative, MemoryOwner::cache, 1).has_value());

    // Reassembly is still admitted: it is what releases publication's bytes.
    auto reassembly = ledger.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 10);
    REQUIRE(reassembly.has_value());

    // The exemption covers the durable-lower budget only: 60 + 10 held of 90
    // non-control capacity, so 30 more is refused, as is exceeding the total.
    CHECK(!ledger.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 30).has_value());
    CHECK(!ledger.try_acquire(WorkClass::control, MemoryOwner::rpc_frame, 40).has_value());

    reassembly.reset();
    publication.reset();
    CHECK(ledger.stats().used_bytes == 0);
    CHECK(ledger.try_acquire(WorkClass::loader, MemoryOwner::publication, 60).has_value());
}

MACHA_FAST_TEST("foundations", test_retained_memory_reassembly_reserve_is_bounded_not_absolute) {
    // A 5-byte reassembly reserve, so the bound is testable.
    RetainedMemoryLedger ledger(100, 10, 30, 10, 5);

    auto publication = ledger.try_acquire(WorkClass::loader, MemoryOwner::publication, 60);
    REQUIRE(publication.has_value());

    // A queued lower-class waiter must not shut reassembly out of the reserve.
    std::atomic_bool started{false};
    std::atomic_bool finished{false};
    std::jthread waiter([&] {
        started.store(true);
        std::atomic_bool cancel{false};
        auto blocked = ledger.acquire(WorkClass::loader, MemoryOwner::publication, 40,
                                      RetainedMemoryLedger::Clock::now() + 2s, &cancel);
        finished.store(true);
        CHECK(!blocked.has_value());
    });
    while (!started.load())
        std::this_thread::yield();
    for (int i = 0; i < 200 && ledger.stats().waits[2] == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(ledger.stats().waits[2] > 0);

    // A queued viewer always outranks reassembly: the reserve sits below the
    // control/viewer waiter gate.
    std::atomic_bool viewer_cancel{false};
    std::atomic_bool viewer_started{false};
    std::jthread viewer([&] {
        viewer_started.store(true);
        (void)ledger.acquire(WorkClass::foreground, MemoryOwner::playback_segment, 40,
                             RetainedMemoryLedger::Clock::now() + 1s, &viewer_cancel);
    });
    while (!viewer_started.load())
        std::this_thread::yield();
    for (int i = 0; i < 200 && ledger.stats().waits[0] + ledger.stats().waits[3] == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(!ledger.try_acquire(WorkClass::speculative, MemoryOwner::rpc_frame, 1).has_value());
    viewer_cancel.store(true);
    viewer.join();

    {
        // Inside the reserve, reassembly is admitted despite the lower waiter.
        // Scoped so the destructor releases the lease: an explicit reset()
        // here trips a GCC -Wmaybe-uninitialized false positive.
        auto inside = ledger.try_acquire(WorkClass::speculative, MemoryOwner::rpc_frame, 4);
        REQUIRE(inside.has_value());

        // Beyond the reserve it defers; unbounded priority would starve the
        // node's own publication.
        CHECK(!ledger.try_acquire(WorkClass::speculative, MemoryOwner::rpc_frame, 4).has_value());
        // The reserve is for reassembly only; other speculative work defers.
        CHECK(!ledger.try_acquire(WorkClass::speculative, MemoryOwner::cache, 1).has_value());
    }
    waiter.join();
    CHECK(finished.load());

    publication.reset();
    CHECK(ledger.stats().used_bytes == 0);
}

MACHA_FAST_TEST("foundations", test_retained_memory_ledger_sheds_borrowed_cache_for_viewer) {
    RetainedMemoryLedger ledger(100, 10, 30, 10, 5);
    std::optional<RetainedMemoryLedger::Lease> borrowed;
    borrowed = ledger.try_acquire(WorkClass::loader, MemoryOwner::cache, 90, true,
                                  [&] { borrowed.reset(); });
    REQUIRE(borrowed.has_value());
    CHECK(ledger.stats().reclaimable_bytes == 90);

    auto viewer = ledger.acquire(WorkClass::foreground, MemoryOwner::playback_segment, 30,
                                 RetainedMemoryLedger::Clock::now() + 1s);
    REQUIRE(viewer.has_value());
    CHECK(!borrowed.has_value());
    const auto after = ledger.stats();
    CHECK(after.used_bytes == 30);
    CHECK(after.reclaimable_bytes == 0);
    CHECK(after.shed_requests == 1);
    viewer.reset();
    CHECK(ledger.stats().used_bytes == 0);
}

MACHA_FAST_TEST("foundations", test_retained_memory_ledger_restores_durable_overcommit) {
    RetainedMemoryLedger ledger(100, 10, 30, 10, 5);
    auto restored = ledger.restore(WorkClass::loader, MemoryOwner::fuse_operation, 120);
    const auto overcommitted = ledger.stats();
    CHECK(overcommitted.used_bytes == 120);
    CHECK(overcommitted.restored_bytes == 120);
    CHECK(!ledger.try_acquire(WorkClass::foreground, MemoryOwner::playback_segment, 1).has_value());

    restored.reset();
    auto viewer = ledger.try_acquire(WorkClass::foreground, MemoryOwner::playback_segment, 30);
    REQUIRE(viewer.has_value());
    CHECK(ledger.stats().used_bytes == 30);
}

} // namespace

MACHA_TEST("foundations", test_every_subsystem_thread_is_run_supervised) {
    // An exception escaping a thread body calls std::terminate(); run_supervised
    // (src/supervised.hpp) guards that boundary. Every thread construction site
    // under src/ must name run_supervised within the next two lines: a jthread
    // or thread built on a lambda, and a thread pool's emplace_back of one.
    const std::regex site_pattern(
        R"(std::jthread\(\[|std::thread\(\[|emplace_back\([^)]*stop_token|(workers|threads)_?\.emplace_back\(\[)");
    const std::filesystem::path src_dir = std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "src";
    std::vector<std::string> unguarded;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
        if (entry.path().extension() != ".cpp")
            continue;
        // The guard's own implementation is exempt: it does not call itself.
        if (entry.path().filename() == "supervised.cpp")
            continue;

        std::ifstream file(entry.path());
        REQUIRE(file.is_open());
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);)
            lines.push_back(std::move(line));

        for (size_t i = 0; i < lines.size(); ++i) {
            if (!std::regex_search(lines[i], site_pattern))
                continue;
            bool guarded = false;
            for (size_t j = i; j < lines.size() && j < i + 3; ++j) {
                if (lines[j].find("run_supervised") != std::string::npos) {
                    guarded = true;
                    break;
                }
            }
            if (!guarded)
                unguarded.push_back(entry.path().filename().string() + ":" + std::to_string(i + 1) +
                                    ": " + lines[i]);
        }
    }

    if (!unguarded.empty()) {
        std::cerr << unguarded.size() << " thread construction site(s) bypass run_supervised:\n";
        for (const auto& site : unguarded)
            std::cerr << "  " << site << "\n";
    }
    REQUIRE(unguarded.empty());
}

namespace {
std::optional<SupervisedThreadStatus> supervised_status_of(std::string_view name) {
    for (auto& status : supervised_thread_statuses())
        if (status.name == name)
            return status;
    return std::nullopt;
}
} // namespace

MACHA_FAST_TEST("foundations", test_supervised_restart_delay_doubles_to_its_ceiling) {
    CHECK(supervised_restart_delay(1) == 1s);
    CHECK(supervised_restart_delay(2) == 2s);
    CHECK(supervised_restart_delay(3) == 4s);
    CHECK(supervised_restart_delay(6) == 32s);
    CHECK(supervised_restart_delay(7) == 60s);
    CHECK(supervised_restart_delay(1000) == 60s);
}

MACHA_TEST("foundations", test_a_supervised_loop_that_throws_runs_again) {
    // A faulting service loop is run again, and the fault is recorded.
    std::atomic<int> runs{0};
    std::jthread thread([&](std::stop_token stop) {
        run_supervised_loop("test-loop-restarts", stop, [&] {
            if (++runs == 1)
                throw std::runtime_error("first run fails");
        });
    });
    REQUIRE(wait_until([&] { return runs.load() == 2; }, 5s));
    thread.join();
    CHECK(runs.load() == 2); // a normal return ends it; it is not rerun.
    const auto status = supervised_status_of("test-loop-restarts");
    REQUIRE(status.has_value());
    CHECK(status->faults == 1);
    CHECK(status->last_fault_code == "exception");
    CHECK(status->last_fault == "first run fails");
    CHECK(status->last_fault_unix_ms > 0);
    CHECK(status->running == 0);
    CHECK(status->restarting == 0);
}

MACHA_TEST("foundations", test_a_supervised_loop_in_backoff_stops_when_asked) {
    std::atomic<int> runs{0};
    std::jthread thread([&](std::stop_token stop) {
        run_supervised_loop("test-loop-backoff-stop", stop, [&] {
            ++runs;
            throw 7; // not a std::exception
        });
    });
    REQUIRE(wait_until(
        [&] {
            const auto status = supervised_status_of("test-loop-backoff-stop");
            return status && status->restarting == 1;
        },
        5s));
    const auto asked = std::chrono::steady_clock::now();
    thread.request_stop();
    thread.join();
    // Well inside the 1 s backoff: the wait is woken by the stop, not waited out.
    CHECK(std::chrono::steady_clock::now() - asked < 900ms);
    CHECK(runs.load() == 1);
    const auto status = supervised_status_of("test-loop-backoff-stop");
    REQUIRE(status.has_value());
    CHECK(status->last_fault_code == "unknown_exception");
    CHECK(status->restarting == 0);
}

MACHA_TEST("foundations", test_a_supervised_task_that_throws_ends_on_record) {
    std::atomic<int> runs{0};
    std::thread thread([&] {
        run_supervised_once("test-once-ends", [&] {
            ++runs;
            throw std::runtime_error("connection lost");
        });
    });
    thread.join();
    CHECK(runs.load() == 1);
    const auto status = supervised_status_of("test-once-ends");
    REQUIRE(status.has_value());
    CHECK(status->faults == 1);
    CHECK(status->last_fault == "connection lost");
    CHECK(status->running == 0);
}

MACHA_TEST("foundations", test_an_escalating_thread_hands_its_fault_to_the_sink) {
    std::atomic<int> runs{0};
    std::string reason;
    std::thread thread([&] {
        run_supervised_escalating(
            "test-escalates",
            [&] {
                ++runs;
                throw std::runtime_error("invalid torrent handle used");
            },
            [&](std::string why) { reason = std::move(why); });
    });
    thread.join();
    CHECK(runs.load() == 1);
    CHECK(reason.find("test-escalates") != std::string::npos);
    CHECK(reason.find("invalid torrent handle used") != std::string::npos);
    const auto status = supervised_status_of("test-escalates");
    REQUIRE(status.has_value());
    CHECK(status->faults == 1);
}

namespace {
Config minimal_valid_config() {
    Config config;
    config.state_path = "/tmp/does-not-need-to-exist-for-this-test";
    config.key_file = "/tmp/does-not-need-to-exist-for-this-test.key";
    config.storage_backends.push_back(
        {.path = "/tmp/does-not-need-to-exist-for-this-test/data", .limit = 1});
    return config;
}
} // namespace

MACHA_TEST("foundations", test_normalize_config_defaults_plugin_path_to_the_build_default) {
    // The default is the compile-time MACHA_PLUGIN_INSTALL_DIR, not derived
    // from the executable's location (bindir).
    auto normalized = normalize_config(minimal_valid_config());
    if (kDefaultPluginDir.empty()) {
        CHECK(!normalized.plugin_path.has_value());
    } else {
        REQUIRE(normalized.plugin_path.has_value());
        CHECK(*normalized.plugin_path == std::filesystem::path(kDefaultPluginDir));
    }
}

MACHA_TEST("foundations", test_publication_open_writer_bound_fits_the_loader_reserve) {
    // Every writer open at once must fit its worst case (one extent buffer plus
    // its pipeline) inside the loader reserve, or writers deadlock each other.
    auto config = minimal_valid_config();
    config.extent_size = 4 * 1024 * 1024;
    config.fuse.commit_workers = 8;
    config.runtime.loader_memory_reserve_bytes = 64ULL * 1024 * 1024;
    auto normalized = normalize_config(config);
    const auto per_writer =
        static_cast<uint64_t>(normalized.extent_size) + normalized.fuse.publication_pipeline_bytes;
    CHECK(per_writer == 12ULL * 1024 * 1024); // 4M buffer + two 4M pipeline extents
    // 64M / 12M is 5, but never fewer than one writer per commit worker.
    CHECK(normalized.fuse.publication_max_open_writers == 8);

    // With enough reserve the ledger, not the worker count, sets the bound.
    config.runtime.retained_memory_bytes = 2048ULL * 1024 * 1024;
    config.runtime.loader_memory_reserve_bytes = 600ULL * 1024 * 1024;
    CHECK(normalize_config(config).fuse.publication_max_open_writers == 50);

    // An explicit operator value is never overridden.
    config.fuse.publication_max_open_writers = 3;
    CHECK(normalize_config(config).fuse.publication_max_open_writers == 3);
}

MACHA_TEST("foundations", test_torrent_binds_the_advertised_address_not_libtorrent_enumeration) {
    // libtorrent's own device enumeration can miss the live link; the
    // advertised address is correct whichever device carries it.
    TorrentConfig config;
    config.listen_port = 6881;

    CHECK(torrent_listen_interfaces(config, "10.44.1.51") == "10.44.1.51:6881");

    // A literal IPv6 advertise must be bracketed or libtorrent cannot parse
    // the port off it.
    CHECK(torrent_listen_interfaces(config, "fd0e:9c96:30a3::bb4") == "[fd0e:9c96:30a3::bb4]:6881");

    // No usable advertised address: bind every interface.
    CHECK(torrent_listen_interfaces(config, "") == "0.0.0.0:6881,[::]:6881");
    CHECK(torrent_listen_interfaces(config, "0.0.0.0") == "0.0.0.0:6881,[::]:6881");

    config.listen_port = 51413;
    CHECK(torrent_listen_interfaces(config, "10.44.1.51") == "10.44.1.51:51413");
    CHECK(torrent_listen_interfaces(config, "") == "0.0.0.0:51413,[::]:51413");

    // An explicit operator value, including a device name, always wins.
    config.listen_interfaces = "wlan0:6881";
    CHECK(torrent_listen_interfaces(config, "10.44.1.51") == "wlan0:6881");
    CHECK(torrent_listen_interfaces(config, "") == "wlan0:6881");

    // A hostname advertise binds every interface: listen_interfaces takes an
    // IP literal or device name, so a hostname would silently bind nothing,
    // and it may resolve to a NATed address the node does not hold.
    config.listen_interfaces.clear();
    config.listen_port = 6881;
    for (const auto* name : {"inverbeg.macha.network", "macnessa.macha.network", "localhost",
                             "node-1", "example.com."}) {
        CHECK(torrent_listen_interfaces(config, name) == "0.0.0.0:6881,[::]:6881");
    }

    // An advertised address still gets a specific binding.
    CHECK(torrent_listen_interfaces(config, "192.168.1.50") == "192.168.1.50:6881");
    CHECK(torrent_listen_interfaces(config, "::1") == "[::1]:6881");

    // Near-misses that must not be mistaken for addresses.
    CHECK(torrent_listen_interfaces(config, "10.44.1") == "0.0.0.0:6881,[::]:6881");
    CHECK(torrent_listen_interfaces(config, "10.44.1.256") == "0.0.0.0:6881,[::]:6881");
    CHECK(torrent_listen_interfaces(config, "10.44.1.51.") == "0.0.0.0:6881,[::]:6881");

    // The operator override still wins.
    config.listen_interfaces = "0.0.0.0:6881,[::]:6881";
    CHECK(torrent_listen_interfaces(config, "inverbeg.macha.network") == "0.0.0.0:6881,[::]:6881");
}

MACHA_TEST("foundations", test_retry_budget_is_reachable_once_backoff_reaches_its_ceiling) {
    // Once backoff caps, a window holds at most failure_window/max_backoff
    // attempts (30 min / 30 s = 60 < 100), so the density rule alone never
    // parks; max_failing_duration must. failed() takes `now`: simulated time.
    using Clock = RetryState::Clock;
    const auto start = Clock::now();
    RetryPolicy shipped{100, 30min, 250ms, 30s};
    shipped.max_failing_duration = {}; // no duration backstop

    RetryState never_parks;
    bool parked = false;
    for (int i = 1; i <= 400 && !parked; ++i) // 400 x 30 s ~ 3.3 simulated hours
        parked = !never_parks.failed(shipped, start + i * 30s).has_value();
    CHECK(!parked);
    CHECK(never_parks.failures_in_window() <= 61); // the arithmetic ceiling
    CHECK(never_parks.consecutive_failures() == 400);

    // With the backstop the same policy parks, exactly once it is exceeded.
    RetryPolicy fixed = shipped;
    fixed.max_failing_duration = 1h;
    RetryState bounded;
    std::optional<int> parked_at;
    for (int i = 1; i <= 400 && !parked_at; ++i) {
        if (!bounded.failed(fixed, start + i * 30s))
            parked_at = i;
    }
    // The run is measured from the first failure, so at attempt i it has been
    // failing for (i-1) x 30 s. It parks on the first attempt strictly past 1 h.
    REQUIRE(parked_at.has_value());
    CHECK(*parked_at == 122);
    CHECK((*parked_at - 1) * 30s > 1h);
    CHECK((*parked_at - 2) * 30s <= 1h);

    // The backstop measures the current unbroken run, so an item that keeps
    // recovering never parks on age alone.
    RetryState flapping;
    for (int i = 1; i <= 400; ++i) {
        REQUIRE(flapping.failed(fixed, start + i * 30s).has_value());
        flapping.succeeded();
    }
    CHECK(flapping.consecutive_failures() == 0);

    // Dense flapping still parks on the density rule.
    RetryPolicy dense{5, 10min, 1ms, 10ms};
    dense.max_failing_duration = 1h;
    RetryState flapping_hard;
    std::optional<int> dense_parked_at;
    for (int i = 1; i <= 20 && !dense_parked_at; ++i) {
        if (!flapping_hard.failed(dense, start + i * 1s))
            dense_parked_at = i;
        else
            flapping_hard.succeeded();
    }
    REQUIRE(dense_parked_at.has_value());
    CHECK(*dense_parked_at == 6); // more than 5 in the window
}

MACHA_TEST("foundations", test_normalize_config_preserves_an_explicit_plugin_path) {
    auto config = minimal_valid_config();
    config.plugin_path = "/opt/macha/plugins";
    auto normalized = normalize_config(config);
    REQUIRE(normalized.plugin_path.has_value());
    CHECK(*normalized.plugin_path == std::filesystem::path("/opt/macha/plugins"));
}

MACHA_TEST("foundations", test_fuse_mountpoint_preflight_counts_stray_entries) {
    TempDir temp;
    const auto covered = temp.path() / "mnt";
    // Missing directory: created, empty, nothing stray.
    CHECK(guard_covered_mountpoint(covered, false).stray_entries == 0);
    CHECK(std::filesystem::is_directory(covered));
    // Entries in the host directory, which the mount would hide, are counted.
    std::ofstream(covered / "stray.mkv") << "not in macha";
    std::filesystem::create_directory(covered / "Movies");
    CHECK(guard_covered_mountpoint(covered, false).stray_entries == 2);
}

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

// Nodes that accept no inbound connections, and edge nodes.

MACHA_FAST_TEST("foundations", test_configuration_tristates_and_hosting_rules) {
    CHECK(parse_tristate("true", "x") == Tristate::yes);
    CHECK(parse_tristate("FALSE", "x") == Tristate::no);
    CHECK(parse_tristate("auto", "x") == Tristate::automatic);
    CHECK(parse_tristate("yes", "x") == Tristate::yes);
    CHECK(parse_tristate("off", "x") == Tristate::no);
    bool rejected = false;
    try {
        (void)parse_tristate("maybe", "network.inbound_capable");
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("network.inbound_capable") != std::string::npos;
    }
    CHECK(rejected);
    CHECK(tristate_name(Tristate::automatic) == "auto");

    TempDir t;
    const auto keyfile = t.path() / "key";
    write_key(keyfile);
    auto base = config_for(t.path() / "node", keyfile, 57431);
    CHECK(base.inbound_capable == Tristate::automatic);
    CHECK(base.hosts_extents == Tristate::automatic);
    CHECK(configuration_warnings(base).empty());

    // storage.data is optional unless the node insists on hosting.
    auto edge = base;
    edge.storage_backends.clear();
    (void)normalize_config(edge); // auto with no backends: resolves to not hosting
    edge.hosts_extents = Tristate::no;
    (void)normalize_config(edge);
    auto insists = base;
    insists.storage_backends.clear();
    insists.hosts_extents = Tristate::yes;
    bool refused = false;
    try {
        (void)normalize_config(insists);
    } catch (const std::runtime_error& error) {
        refused =
            std::string(error.what()).find("storage.hosts_extents is true") != std::string::npos;
    }
    CHECK(refused);

    // Legal shapes worth a warning: backends that will only drain, extents
    // nobody can dial for, and NAT machinery a non-dialable node cannot use.
    auto draining = base;
    draining.hosts_extents = Tristate::no;
    auto warnings = configuration_warnings(draining);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("will drain") != std::string::npos);

    auto hidden_host = base;
    hidden_host.inbound_capable = Tristate::no;
    hidden_host.hosts_extents = Tristate::yes;
    hidden_host.upnp.enabled = true;
    warnings = configuration_warnings(hidden_host);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find("inbound-capable peers only") != std::string::npos);
    CHECK(warnings[1].find("network.advertise, network.upnp") != std::string::npos);
}

MACHA_FAST_TEST("foundations", test_transcode_rates_keep_a_median_per_class_and_survive_a_restart) {
    TempDir t;
    const auto path = t.path() / "playback" / "transcode-rates.json";
    CHECK(TranscodeRateBook::height_class(534) == 576);
    CHECK(TranscodeRateBook::height_class(1080) == 1080);
    CHECK(TranscodeRateBook::height_class(1600) == 2160);
    CHECK(TranscodeRateBook::height_class(4320) == 4320);
    {
        TranscodeRateBook book(path);
        CHECK(book.summary().empty());
        for (const double rate : {0.30, 0.35, 0.33})
            book.record("video", "hevc", 10, 2160, rate, 1);
        book.record("audio", "truehd", 0, 0, 12.5, 3);
        // Only the last kept_observations count.
        for (size_t i = 0; i < TranscodeRateBook::kept_observations + 4; ++i)
            book.record("video", "h264", 8, 1080, 4.0, 2);
    }
    // A restart keeps what the hardware has shown.
    TranscodeRateBook reloaded(path);
    const auto rates = reloaded.summary();
    REQUIRE(rates.size() == 3);
    const auto find = [&](const std::string& codec) {
        return *std::find_if(rates.begin(), rates.end(),
                             [&](const auto& rate) { return rate.codec == codec; });
    };
    CHECK(find("hevc").rate_milli == 330);
    CHECK(find("hevc").observations == 3);
    CHECK(find("hevc").height_class == 2160);
    CHECK(find("truehd").kind == "audio");
    CHECK(find("truehd").rate_milli == 12500);
    CHECK(find("truehd").concurrent == 3);
    CHECK(find("h264").observations == TranscodeRateBook::kept_observations);
}

// A forked case's profile name: every %p becomes the child's pid and tag,
// the rest of the pattern untouched.
MACHA_FAST_TEST("foundations", test_coverage_child_profile_name) {
    CHECK(coverage::child_profile_name("/p/%p.profraw", 42, "") == "/p/42.profraw");
    CHECK(coverage::child_profile_name("/p/%p.profraw", 42, "-core") == "/p/42-core.profraw");
    CHECK(coverage::child_profile_name("%p/%p", 7, "x") == "7x/7x");
    CHECK(coverage::child_profile_name("/p/%m.profraw", 7, "x") == "/p/%m.profraw");
    CHECK(coverage::child_profile_name("", 7, "x").empty());
}

MACHA_FAST_TEST("foundations", test_message_routes_answer_only_while_bound) {
    MessageRoutes routes;
    const NodeInfo peer;
    const RpcMessage request{MessageType::get_ingest_jobs, Bytes{1, 2, 3}};
    const auto unbound = rpc_refusal(routes.dispatch(peer, FrameType::control, request));
    REQUIRE(unbound);
    CHECK(unbound->status == 501);
    CHECK(unbound->code == "not_implemented");
    CHECK(!rpc_try_later(*unbound));

    routes.bind(MessageType::get_ingest_jobs,
                [](const NodeInfo&, FrameType, const RpcMessage& message) {
                    return RpcMessage{MessageType::ingest_jobs_reply,
                                      Bytes(message.payload.rbegin(), message.payload.rend())};
                });
    const auto reply = routes.dispatch(peer, FrameType::control, request);
    CHECK(reply.type == MessageType::ingest_jobs_reply);
    CHECK(reply.payload == Bytes({3, 2, 1}));
    // Each type is its own binding.
    CHECK(routes.dispatch(peer, FrameType::control, RpcMessage{MessageType::get_torrent_jobs, {}})
              .type == MessageType::error);

    // A handler that throws answers an error carrying the reason.
    routes.bind(MessageType::get_ingest_jobs,
                [](const NodeInfo&, FrameType, const RpcMessage&) -> RpcMessage {
                    throw std::runtime_error("no jobs here");
                });
    const auto failed = routes.dispatch(peer, FrameType::control, request);
    REQUIRE(failed.type == MessageType::error);
    // The reason comes first, so a reader that takes only the text reads it.
    Reader reader(failed.payload);
    CHECK(reader.string(4096) == "no jobs here");
    const auto thrown = rpc_refusal(failed);
    REQUIRE(thrown);
    CHECK(thrown->status == 500);
    CHECK(thrown->code == "internal_error");
    CHECK(thrown->message == "no jobs here");
    CHECK(rpc_try_later(*thrown));

    routes.unbind(MessageType::get_ingest_jobs);
    CHECK(routes.dispatch(peer, FrameType::control, request).type == MessageType::error);
}

MACHA_FAST_TEST("foundations", test_rpc_refusals_say_whether_asking_again_may_help) {
    CHECK(!rpc_refusal(RpcMessage{MessageType::ok, {}}));

    const auto gone = rpc_refusal(error_reply(404, "not_found", "object not found"));
    REQUIRE(gone);
    CHECK(
        (gone->status == 404 && gone->code == "not_found" && gone->message == "object not found"));
    CHECK(!rpc_try_later(*gone));
    for (const uint16_t status : {429, 500, 502, 503, 504})
        CHECK(rpc_try_later(*rpc_refusal(error_reply(status, "x", "y"))));
    for (const uint16_t status : {400, 409, 410, 413, 501, 507})
        CHECK(!rpc_try_later(*rpc_refusal(error_reply(status, "x", "y"))));

    // A refusal naming no status (an older peer's text, or the transport's
    // empty refusal) may change.
    Writer text_only;
    text_only.string("object not found");
    const auto older = rpc_refusal(RpcMessage{MessageType::error, text_only.take()});
    REQUIRE(older);
    CHECK((older->status == 0 && older->message == "object not found"));
    CHECK(rpc_try_later(*older));
    const auto empty = rpc_refusal(RpcMessage{MessageType::error, {}});
    REQUIRE(empty);
    CHECK(rpc_try_later(*empty));
}

MACHA_FAST_TEST("foundations", test_node_events_count_each_kind) {
    NodeEvents events;
    CHECK(events.total() == 0);
    events.notify(NodeEvent::metadata);
    events.notify(NodeEvent::metadata);
    events.notify(NodeEvent::storage);
    CHECK(events.count(NodeEvent::metadata) == 2);
    CHECK(events.count(NodeEvent::storage) == 1);
    CHECK(events.count(NodeEvent::topology) == 0);
    CHECK(events.total() == 3);
}

namespace {

MACHA_FAST_TEST("foundations", test_write_behind_acts_once_per_window) {
    WriteBehind behind(8);
    CHECK(!behind.window_filled(3));
    CHECK(!behind.window_filled(4));
    CHECK(behind.window_filled(1)); // 8: a window, which starts again
    CHECK(!behind.window_filled(7));
    CHECK(behind.window_filled(20)); // one large write fills one window
    CHECK(!behind.window_filled(0));
}

MACHA_FAST_TEST("foundations", test_write_behind_leaves_what_was_written_intact) {
    TempDir dir;
    const auto path = dir.path() / "staged";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    REQUIRE(fd >= 0);
    WriteBehind behind(64 * 1024);
    Bytes chunk(10000);
    uint64_t written = 0;
    for (int round = 0; round < 40; ++round) {
        std::fill(chunk.begin(), chunk.end(), static_cast<uint8_t>(round));
        REQUIRE(::pwrite(fd, chunk.data(), chunk.size(), static_cast<off_t>(written)) ==
                static_cast<ssize_t>(chunk.size()));
        written += chunk.size();
        behind.wrote(fd, chunk.size());
    }
    start_writeback(fd);
    Bytes back(written);
    REQUIRE(::pread(fd, back.data(), back.size(), 0) == static_cast<ssize_t>(back.size()));
    ::close(fd);
    for (uint64_t i = 0; i < written; ++i)
        if (back[i] != static_cast<uint8_t>(i / chunk.size())) {
            CHECK(false);
            break;
        }
}

} // namespace

namespace {} // namespace

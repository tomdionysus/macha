// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_mountpoint.hpp"
#include "macha_version.hpp"
#include "miniupnpc_compat.hpp"
#include "test_backend_support.hpp"
#include "retained_memory.hpp"

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

    // Cold writer setup is admitted and tracked as active, but it is not
    // charged as loader service. Otherwise a slow disk/network setup would
    // consume the slice before producing a byte and its latency would then be
    // multiplied by the viewer:loader cooldown ratio.
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

    auto blocked_loader = std::async(std::launch::async, [&] {
        return resources.acquire(loader_context, 1);
    });
    CHECK(blocked_loader.wait_for(20ms) == std::future_status::timeout);
    auto blocked_speculative = std::async(std::launch::async, [&] {
        return resources.acquire(DataWorkContext(FrameType::speculative, 1), 1);
    });
    CHECK(blocked_speculative.wait_for(20ms) == std::future_status::timeout);

    // Lower-class saturation cannot consume the reserved byte. A viewer which
    // arrives later starts immediately without cancelling the bounded loader
    // work already in flight.
    auto viewer = resources.acquire(DataWorkContext(FrameType::foreground, 1), 1);
    REQUIRE(viewer.has_value());
    CHECK(blocked_loader.wait_for(20ms) == std::future_status::timeout);
    viewer.reset();

    // A bounded deadline terminates through the condition-variable deadline;
    // there is no periodic admission poll.
    auto expired = resources.acquire(
        DataWorkContext(FrameType::loader, 1, DataWorkContext::Clock::now() + 20ms), 1);
    CHECK(!expired.has_value());

    first.reset();
    REQUIRE(blocked_loader.wait_for(1s) == std::future_status::ready);
    auto admitted_loader = blocked_loader.get();
    REQUIRE(admitted_loader.has_value());
    CHECK(blocked_speculative.wait_for(20ms) == std::future_status::timeout);
    second.reset();
    REQUIRE(blocked_speculative.wait_for(1s) == std::future_status::ready);
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
    telemetry.api_endpoint = "https://10.44.1.50:7438";
    telemetry.cpu_cores = 8;
    telemetry.memory_total_bytes = 64ULL * 1024 * 1024 * 1024;
    CHECK(decode_node_telemetry(encode_node_telemetry(telemetry)) == telemetry);
    auto telemetry_set = decode_telemetry_set(encode_telemetry_set({telemetry}));
    REQUIRE(telemetry_set.size() == 1);
    CHECK(telemetry_set.front() == telemetry);

    telemetry.phase = NodePhase::recovering;
    CHECK(decode_node_telemetry(encode_node_telemetry(telemetry)) == telemetry);
    auto recovering_set = decode_telemetry_set(encode_telemetry_set({telemetry}));
    REQUIRE(recovering_set.size() == 1);
    CHECK(recovering_set.front().phase == NodePhase::recovering);

    auto full = encode_node_telemetry(telemetry);
    // memory_total_bytes is the newest trailing field: one u64. cpu_cores
    // precedes it as a u32.
    const size_t memory_bytes_bytes = 8;
    const size_t cpu_cores_bytes = 4 + memory_bytes_bytes;
    // The API endpoint precedes it: a string length prefix + content.
    const size_t api_fields_bytes = 4 + telemetry.api_endpoint.size();
    REQUIRE(full.size() > cpu_cores_bytes + api_fields_bytes + 1);

    // A record encoded before physical memory existed ends right after
    // cpu_cores, and must report none rather than fail.
    auto pre_memory = full;
    pre_memory.resize(pre_memory.size() - memory_bytes_bytes);
    auto legacy_no_memory = decode_node_telemetry(pre_memory);
    CHECK(legacy_no_memory.memory_total_bytes == 0);
    CHECK(legacy_no_memory.cpu_cores == telemetry.cpu_cores);
    CHECK(legacy_no_memory.api_endpoint == telemetry.api_endpoint);

    // A record encoded before cpu_cores existed ends right after the API
    // endpoint. It
    // must decode as "not reported" -- zero, meaning no opinion -- rather than
    // fail. Every node is briefly in this position during a rolling upgrade,
    // which is exactly when a peer's core count would otherwise be read from
    // whatever bytes happened to follow.
    auto pre_cores = full;
    pre_cores.resize(pre_cores.size() - cpu_cores_bytes);
    auto legacy_no_cores = decode_node_telemetry(pre_cores);
    CHECK(legacy_no_cores.cpu_cores == 0);
    CHECK(legacy_no_cores.memory_total_bytes == 0);
    CHECK(legacy_no_cores.api_endpoint == telemetry.api_endpoint);
    CHECK(legacy_no_cores.sequence == telemetry.sequence);

    // A record encoded before the API endpoint existed simply ends earlier,
    // right after phase. It must decode as "not reported" rather than fail or
    // silently pick up truncated bytes as an endpoint.
    auto pre_api = full;
    pre_api.resize(pre_api.size() - cpu_cores_bytes - api_fields_bytes);
    auto legacy_no_api = decode_node_telemetry(pre_api);
    CHECK(legacy_no_api.phase == NodePhase::recovering);
    CHECK(legacy_no_api.api_endpoint.empty());
    CHECK(legacy_no_api.cpu_cores == 0);
    CHECK(legacy_no_api.memory_total_bytes == 0);
    CHECK(legacy_no_api.sequence == telemetry.sequence);

    // A record encoded before phase (and so also before the API endpoint)
    // existed ends one byte earlier still. It must decode as "ready"
    // (NodeTelemetry's default) rather than fail or silently pick a
    // different phase.
    auto pre_phase = full;
    pre_phase.resize(pre_phase.size() - cpu_cores_bytes - api_fields_bytes - 1);
    auto legacy = decode_node_telemetry(pre_phase);
    CHECK(legacy.phase == NodePhase::ready);
    CHECK(legacy.api_endpoint.empty());
    CHECK(legacy.cpu_cores == 0);
    CHECK(legacy.memory_total_bytes == 0);
    CHECK(legacy.sequence == telemetry.sequence);
    CHECK(legacy.rpc_connections_reused == telemetry.rpc_connections_reused);

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

MACHA_FAST_TEST("foundations", test_retained_memory_ledger_preserves_priority_headroom) {
    RetainedMemoryLedger ledger(100, 10, 30, 10);
    auto speculative = ledger.try_acquire(MemoryClass::speculative, MemoryOwner::cache, 50);
    REQUIRE(speculative.has_value());
    CHECK(!ledger.try_acquire(MemoryClass::speculative, MemoryOwner::cache, 1).has_value());

    // Speculative ownership cannot consume the loader floor. Viewer and
    // control reservations remain independently usable at full lower load.
    auto loader = ledger.try_acquire(MemoryClass::loader, MemoryOwner::publication, 10);
    auto viewer = ledger.try_acquire(MemoryClass::viewer, MemoryOwner::playback_segment, 30);
    auto control = ledger.try_acquire(MemoryClass::control, MemoryOwner::rpc_frame, 10);
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
    // The es-1 livelock of 2026-09-08, in miniature. Publication holds its
    // retained bytes until a peer confirms the write, and that confirmation
    // arrives as an RPC message which must first be reassembled into this same
    // ledger. While reassembly was charged against the durable-lower budget,
    // publication could fill that budget and then be unable to confirm
    // anything -- reassembly refused, MessageAssembler throwing, the peer
    // channel dropping, so nothing was released and the node could not drain
    // itself even across a restart.
    RetainedMemoryLedger ledger(100, 10, 30, 10);

    // Fill the durable-lower budget exactly, as a publication backlog does.
    auto publication = ledger.try_acquire(MemoryClass::loader, MemoryOwner::publication, 60);
    REQUIRE(publication.has_value());

    // Durable lower-priority work is now correctly refused: this is the state
    // the node was wedged in, and it must stay refused or the test proves
    // nothing.
    CHECK(!ledger.try_acquire(MemoryClass::loader, MemoryOwner::publication, 1).has_value());
    CHECK(!ledger.try_acquire(MemoryClass::speculative, MemoryOwner::cache, 1).has_value());

    // But reassembly still gets in, because it is the path that releases the
    // very bytes publication is holding.
    auto reassembly = ledger.try_acquire(MemoryClass::loader, MemoryOwner::rpc_frame, 10);
    REQUIRE(reassembly.has_value());

    // The exemption is from the durable-lower budget only, not from the
    // ledger. Control and viewer reserves still bound it: 60 + 10 held, and
    // non-control capacity is 90, so 30 more must not be admitted.
    CHECK(!ledger.try_acquire(MemoryClass::loader, MemoryOwner::rpc_frame, 30).has_value());
    // Nor may it exceed total capacity.
    CHECK(!ledger.try_acquire(MemoryClass::control, MemoryOwner::rpc_frame, 40).has_value());

    // Confirmation completes, publication releases, and the budget is usable
    // again -- the loop closes instead of wedging.
    reassembly.reset();
    publication.reset();
    CHECK(ledger.stats().used_bytes == 0);
    CHECK(ledger.try_acquire(MemoryClass::loader, MemoryOwner::publication, 60).has_value());
}

MACHA_FAST_TEST("foundations", test_retained_memory_reassembly_reserve_is_bounded_not_absolute) {
    // Reserve of 5 bytes inside a 100-byte ledger, so the bound is testable.
    RetainedMemoryLedger ledger(100, 10, 30, 10, 5);

    // Publication holds the durable-lower budget, as a wedged node's does.
    auto publication = ledger.try_acquire(MemoryClass::loader, MemoryOwner::publication, 60);
    REQUIRE(publication.has_value());

    // A permanently queued waiter is the normal state of that node, and it is
    // the case the 2026-09-09 first attempt missed: the exemption sat below
    // the waiter gates, so every data-lane frame was refused before it was
    // reached and the node re-entered the identical livelock.
    std::atomic_bool started{false};
    std::atomic_bool finished{false};
    std::jthread waiter([&] {
        started.store(true);
        std::atomic_bool cancel{false};
        auto blocked = ledger.acquire(MemoryClass::loader, MemoryOwner::publication, 40,
                                      RetainedMemoryLedger::Clock::now() + 2s, &cancel);
        finished.store(true);
        CHECK(!blocked.has_value());
    });
    while (!started.load()) std::this_thread::yield();
    for (int i = 0; i < 200 && ledger.stats().waits[2] == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(ledger.stats().waits[2] > 0);

    // Governing law 1 first: a queued VIEWER outranks reassembly, always. The
    // reserve sits below the control/viewer waiter gate precisely so that a
    // frame can never be admitted ahead of playback, and a viewer cannot be
    // the party publication is deadlocked against in any case -- the viewer
    // reserve is headroom loader and speculative work can never consume.
    std::atomic_bool viewer_cancel{false};
    std::atomic_bool viewer_started{false};
    std::jthread viewer([&] {
        viewer_started.store(true);
        (void)ledger.acquire(MemoryClass::viewer, MemoryOwner::playback_segment, 40,
                             RetainedMemoryLedger::Clock::now() + 1s, &viewer_cancel);
    });
    while (!viewer_started.load()) std::this_thread::yield();
    for (int i = 0; i < 200 && ledger.stats().waits[0] + ledger.stats().waits[3] == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(!ledger.try_acquire(MemoryClass::speculative, MemoryOwner::rpc_frame, 1).has_value());
    viewer_cancel.store(true);
    viewer.join();

    {
        // Inside the reserve, reassembly is admitted despite that waiter: this
        // is the confirmation path, and refusing it is what wedges the ledger.
        // Scoped so the lease is released by its destructor -- an explicit
        // reset() here trips a -Wmaybe-uninitialized false positive on GCC.
        auto inside = ledger.try_acquire(MemoryClass::speculative, MemoryOwner::rpc_frame, 4);
        REQUIRE(inside.has_value());

        // Beyond the reserve it defers like anything else. Absolute priority
        // here simply inverts the deadlock -- a node receiving from two peers
        // starves its own publication -- so the guarantee must be bounded.
        CHECK(!ledger.try_acquire(MemoryClass::speculative, MemoryOwner::rpc_frame, 4)
                   .has_value());
        // And an ordinary speculative admission is still deferred, so the
        // reserve is specific to reassembly rather than a hole in the waiter
        // discipline.
        CHECK(!ledger.try_acquire(MemoryClass::speculative, MemoryOwner::cache, 1).has_value());
    }
    waiter.join();
    CHECK(finished.load());

    // Once the frame is reassembled the confirmation completes, publication
    // releases, and the ledger is usable again.
    publication.reset();
    CHECK(ledger.stats().used_bytes == 0);
}

MACHA_FAST_TEST("foundations", test_retained_memory_ledger_sheds_borrowed_cache_for_viewer) {
    RetainedMemoryLedger ledger(100, 10, 30, 10);
    std::optional<RetainedMemoryLedger::Lease> borrowed;
    borrowed = ledger.try_acquire(MemoryClass::loader, MemoryOwner::cache, 90, true,
                                  [&] { borrowed.reset(); });
    REQUIRE(borrowed.has_value());
    CHECK(ledger.stats().reclaimable_bytes == 90);

    auto viewer = ledger.acquire(MemoryClass::viewer, MemoryOwner::playback_segment, 30,
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
    RetainedMemoryLedger ledger(100, 10, 30, 10);
    auto restored = ledger.restore(MemoryClass::loader, MemoryOwner::fuse_operation, 120);
    const auto overcommitted = ledger.stats();
    CHECK(overcommitted.used_bytes == 120);
    CHECK(overcommitted.restored_bytes == 120);
    CHECK(!ledger.try_acquire(MemoryClass::viewer, MemoryOwner::playback_segment, 1).has_value());

    restored.reset();
    auto viewer = ledger.try_acquire(MemoryClass::viewer, MemoryOwner::playback_segment, 30);
    REQUIRE(viewer.has_value());
    CHECK(ledger.stats().used_bytes == 30);
}

} // namespace

MACHA_TEST("foundations", test_every_subsystem_thread_is_run_supervised) {
    // An exception escaping a std::jthread/std::thread lambda does not reach
    // any caller's try/catch -- it calls std::terminate() and aborts the
    // whole process. run_supervised() is the one place that boundary is
    // guarded (see src/supervised.hpp). This scans every src/*.cpp for a raw
    // thread/worker-pool construction site and fails if run_supervised does
    // not appear within the next couple of lines, so a new thread can't be
    // added later that silently bypasses the guard.
    const std::regex site_pattern(R"(std::jthread\(\[|emplace_back\([^)]*stop_token)");
    const std::filesystem::path src_dir = std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "src";
    std::vector<std::string> unguarded;

    for (const auto& entry : std::filesystem::directory_iterator(src_dir)) {
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
                unguarded.push_back(entry.path().filename().string() + ":" +
                                    std::to_string(i + 1) + ": " + lines[i]);
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
Config minimal_valid_config() {
    Config config;
    config.state_path = "/tmp/does-not-need-to-exist-for-this-test";
    config.key_file = "/tmp/does-not-need-to-exist-for-this-test.key";
    config.storage_backends.push_back({.path = "/tmp/does-not-need-to-exist-for-this-test/data",
                                       .limit = 1});
    return config;
}
} // namespace

MACHA_TEST("foundations", test_normalize_config_defaults_plugin_path_to_the_build_default) {
    // The default is a compile-time constant (this build's private plugin
    // directory under CMAKE_INSTALL_PREFIX, see MACHA_PLUGIN_INSTALL_DIR in
    // CMakeLists.txt) -- deliberately not derived from the running
    // executable's own location, which is bindir, not a place private
    // libraries/plugins belong.
    auto normalized = normalize_config(minimal_valid_config());
    if (kDefaultPluginDir.empty()) {
        CHECK(!normalized.plugin_path.has_value());
    } else {
        REQUIRE(normalized.plugin_path.has_value());
        CHECK(*normalized.plugin_path == std::filesystem::path(kDefaultPluginDir));
    }
}

MACHA_TEST("foundations", test_publication_open_writer_bound_fits_the_loader_reserve) {
    // The bound exists so every writer that may be open at once can hold its
    // worst case -- one filling extent buffer plus its pipeline -- inside the
    // loader's guaranteed share of the retained-memory ledger. Unbounded, the
    // number of writers holding partial state is the width of the publication
    // backlog, and they deadlock against each other (es-1, 2026-09-09).
    auto config = minimal_valid_config();
    config.extent_size = 4 * 1024 * 1024;
    config.fuse.commit_workers = 8;
    config.runtime.loader_memory_reserve_bytes = 64ULL * 1024 * 1024;
    auto normalized = normalize_config(config);
    const auto per_writer =
        static_cast<uint64_t>(normalized.extent_size) + normalized.fuse.publication_pipeline_bytes;
    CHECK(per_writer == 12ULL * 1024 * 1024); // 4M buffer + two 4M pipeline extents
    // 64M / 12M is 5, but never fewer than one writer per commit worker: a
    // worker with no admissible inode is worse than a slightly overcommitted
    // reserve, and the no-progress deadline still bounds the wait.
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
    // libtorrent's default listen_interfaces is expanded by its own device
    // enumeration. On these nodes that binds eth0 and loopback and never
    // wlan0, so gbni-2 -- whose eth0 is NO-CARRIER and whose only live link is
    // wireless -- ran a session bound to 127.0.0.1 and ::1 alone. Two magnets
    // sat in `metadata` for hours with 0 peers, an empty error field, and one
    // "plugin loaded" line in the journal. The advertised address is correct
    // whichever device carries it.
    TorrentConfig config;
    config.listen_port = 6881;

    CHECK(torrent_listen_interfaces(config, "10.44.1.51") == "10.44.1.51:6881");

    // A literal IPv6 advertise must be bracketed or libtorrent cannot parse
    // the port off it.
    CHECK(torrent_listen_interfaces(config, "fd0e:9c96:30a3::bb4") ==
          "[fd0e:9c96:30a3::bb4]:6881");

    // No usable advertised address: fall back to libtorrent's own default
    // rather than inventing a binding.
    CHECK(torrent_listen_interfaces(config, "") == "0.0.0.0:6881,[::]:6881");
    CHECK(torrent_listen_interfaces(config, "0.0.0.0") == "0.0.0.0:6881,[::]:6881");

    // The port is honoured in every branch.
    config.listen_port = 51413;
    CHECK(torrent_listen_interfaces(config, "10.44.1.51") == "10.44.1.51:51413");
    CHECK(torrent_listen_interfaces(config, "") == "0.0.0.0:51413,[::]:51413");

    // An explicit operator value always wins, including a device name, which
    // is the escape hatch when the advertised address is not what should carry
    // peer traffic.
    config.listen_interfaces = "wlan0:6881";
    CHECK(torrent_listen_interfaces(config, "10.44.1.51") == "wlan0:6881");
    CHECK(torrent_listen_interfaces(config, "") == "wlan0:6881");

    // A DNS-name advertise must NOT be handed to libtorrent. listen_interfaces
    // takes an IP literal or a device name, so a hostname matches no device
    // and binds nothing at all -- silently. Worse, the name usually resolves
    // to a public address the node does not hold, because it is behind NAT.
    //
    // Observed live on 2026-09-12: all three nodes moved to public DNS
    // advertise values, every one of them bound nothing on 6881, and torrents
    // sat in dl-metadata for ever with no error anywhere. Binding every
    // interface is the only honest answer.
    config.listen_interfaces.clear();
    config.listen_port = 6881;
    for (const auto* name : {"inverbeg.macha.network", "macnessa.macha.network",
                             "localhost", "node-1", "example.com."}) {
        CHECK(torrent_listen_interfaces(config, name) == "0.0.0.0:6881,[::]:6881");
    }

    // Addresses are still used, so a node that advertises one keeps the
    // specific binding 0.37.2 introduced for it.
    CHECK(torrent_listen_interfaces(config, "192.168.1.50") == "192.168.1.50:6881");
    CHECK(torrent_listen_interfaces(config, "::1") == "[::1]:6881");

    // Near-misses that must not be mistaken for addresses.
    CHECK(torrent_listen_interfaces(config, "10.44.1") == "0.0.0.0:6881,[::]:6881");
    CHECK(torrent_listen_interfaces(config, "10.44.1.256") == "0.0.0.0:6881,[::]:6881");
    CHECK(torrent_listen_interfaces(config, "10.44.1.51.") == "0.0.0.0:6881,[::]:6881");

    // And the operator override still wins over all of it.
    config.listen_interfaces = "0.0.0.0:6881,[::]:6881";
    CHECK(torrent_listen_interfaces(config, "inverbeg.macha.network") ==
          "0.0.0.0:6881,[::]:6881");
}

MACHA_TEST("foundations", test_retry_budget_is_reachable_once_backoff_reaches_its_ceiling) {
    // The density rule ("more than N failures inside the window") cannot fire
    // once backoff caps: a window only ever holds failure_window/max_backoff
    // attempts. The shipped publication policy was 30 min / 30 s = 60 possible
    // attempts against a threshold of 100, so a permanently failing file
    // retried forever. On 2026-09-10 gbni-1 did exactly that -- 68 consecutive
    // failures of one inode, every aggregate reading healthy, parked = 0.
    // RetryState::failed() takes `now`, so this drives simulated time and needs
    // no sleeps.
    using Clock = RetryState::Clock;
    const auto start = Clock::now();
    RetryPolicy shipped{100, 30min, 250ms, 30s};
    shipped.max_failing_duration = {}; // the pre-fix behaviour

    RetryState never_parks;
    bool parked = false;
    for (int i = 1; i <= 400 && !parked; ++i) // 400 x 30 s ~ 3.3 simulated hours
        parked = !never_parks.failed(shipped, start + i * 30s).has_value();
    CHECK(!parked);
    CHECK(never_parks.failures_in_window() <= 61); // the arithmetic ceiling
    CHECK(never_parks.consecutive_failures() == 400);

    // With the duration backstop the same policy parks, and only once the
    // backstop is genuinely exceeded -- not before.
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

    // The backstop measures the current unbroken run, not the whole history:
    // an item that keeps recovering must never park on age alone.
    RetryState flapping;
    for (int i = 1; i <= 400; ++i) {
        REQUIRE(flapping.failed(fixed, start + i * 30s).has_value());
        flapping.succeeded();
    }
    CHECK(flapping.consecutive_failures() == 0);

    // ...but genuine flapping still parks on density, which is what that rule
    // is for: 101 failures inside the window, sparse enough runs that the
    // backstop never applies.
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
    // Files written to the host directory while no mount covered it are
    // reported, so an operator learns that the mount is hiding them.
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

MACHA_TEST("foundations", test_main_owns_signals_and_never_runs_the_mount_itself) {
    // Until 0.41.0 main() ran FUSE on its own thread when a mount was
    // configured, which made three things true at once: FUSE's constructor
    // threw into main()'s outermost catch and exited the whole process, the
    // mount loop's return value WAS the process exit status, and libfuse's
    // signal handlers -- not this loop -- owned SIGINT/SIGTERM/SIGHUP, so
    // SIGHUP configuration reload was silently unavailable on exactly the
    // nodes that mount. FUSE is a supervised subsystem now
    // (TODO/2026-09-14-fuse-supervised-subsystem-plan.md); this keeps main()
    // from growing the branch back.
    const std::filesystem::path main_cpp =
        std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "src" / "main.cpp";
    std::ifstream file(main_cpp);
    REQUIRE(file.is_open());
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

    // No mount loop on the main thread, by any route.
    CHECK(source.find("run_fuse") == std::string::npos);
    CHECK(source.find("fuse_loop") == std::string::npos);
    CHECK(source.find("FuseFrontend") == std::string::npos);

    // The signal mask is installed unconditionally, before Service exists, so
    // every thread it starts inherits it -- libfuse's own workers included.
    const auto mask = source.find("pthread_sigmask");
    REQUIRE(mask != std::string::npos);
    const auto service_constructed = source.find("macha::Service service");
    REQUIRE(service_constructed != std::string::npos);
    CHECK(mask < service_constructed);

    // Nothing between the top of main() and the mask may make blocking
    // conditional on a mount path again.
    const auto guarded = source.find("if (!config.fuse.mount_path)");
    CHECK(guarded == std::string::npos);

    // And the sigwait loop is what the process waits in.
    CHECK(source.find("sigwait(&service_signals") != std::string::npos);
}

// 0.42.0: nodes that accept no inbound connections, and edge nodes.

MACHA_FAST_TEST("foundations", test_node_info_flags_travel_on_the_wire_and_in_the_roster) {
    NodeInfo node;
    node.id = random_node_id();
    node.host = "198.51.100.7";
    node.port = 7437;
    node.failure_domain = "cgnat-site";
    node.seen_unix_ms = unix_ms();
    CHECK(node_inbound_capable(node)); // the pre-0.42 default: dialable, hosting
    CHECK(node_hosts_extents(node));

    node.flags = node_flags_for(false, true);
    Writer writer;
    encode_node_info(writer, node);
    Reader reader(writer.data());
    const auto decoded = decode_node_info(reader);
    reader.finish();
    CHECK(!node_inbound_capable(decoded));
    CHECK(node_hosts_extents(decoded));
    CHECK(decoded.flags == node.flags);

    // The roster persists the flags (v3), so a restarting node knows which
    // peers it must not dial before it has heard from anyone.
    TempDir t;
    const auto roster = t.path() / "membership" / "known-nodes.bin";
    NodeInfo self;
    self.id = random_node_id();
    self.host = "127.0.0.1";
    self.port = 57411;
    {
        Membership membership(self, 30s, roster);
        membership.observe(node, true);
        CHECK(!membership.inbound_capable(node.id));
        CHECK(membership.hosts_extents(node.id));
        CHECK(membership.inbound_capable(self.id));
        // Unknown ids read as the default: capable and hosting.
        CHECK(membership.inbound_capable(random_node_id()));
    }
    {
        Membership recovered(self, 30s, roster);
        REQUIRE(recovered.all().size() == 2);
        CHECK(!recovered.inbound_capable(node.id));
        CHECK(recovered.hosts_extents(node.id));
        // A flag change is an association change: it re-persists.
        auto flipped = node;
        flipped.flags = node_flags_for(false, false);
        flipped.seen_unix_ms = unix_ms() + 1;
        recovered.observe(flipped, false);
        CHECK(!recovered.hosts_extents(node.id));
    }
    {
        Membership again(self, 30s, roster);
        CHECK(!again.hosts_extents(node.id));
    }

    // set_flags reports change and shows up in self().
    Membership membership(self, 30s, {});
    CHECK(membership.set_flags(false, false));
    CHECK(!membership.set_flags(false, false));
    CHECK(!node_inbound_capable(membership.self()));
    CHECK(!node_hosts_extents(membership.self()));
    CHECK(!membership.inbound_capable(self.id));
}

MACHA_FAST_TEST("foundations", test_gc_fence_excludes_pairs_that_can_never_dial_each_other) {
    NodeInfo self;
    self.id = random_node_id();
    self.host = "127.0.0.1";
    self.port = 57421;
    self.flags = node_flags_for(false, false);

    NodeInfo incapable;
    incapable.id = random_node_id();
    incapable.host = "192.0.2.9";
    incapable.port = 7437;
    incapable.flags = node_flags_for(false, true);
    incapable.seen_unix_ms = unix_ms();

    NodeInfo capable;
    capable.id = random_node_id();
    capable.host = "127.0.0.2";
    capable.port = 7437;
    capable.seen_unix_ms = unix_ms();

    // Two nodes that both accept no inbound connections can never
    // authenticate each other directly: not a fault, not a fence.
    Membership membership(self, 40ms, {});
    membership.observe(incapable, false);
    CHECK(membership.all_known_reachable());

    // A capable peer known only from gossip still fences, exactly as before.
    membership.observe(capable, false);
    CHECK(!membership.all_known_reachable());
    membership.observe(capable, true);
    CHECK(membership.all_known_reachable());

    // From a capable node's point of view an incapable peer is an ordinary
    // fence: it can be authenticated directly over the session it opened.
    NodeInfo capable_self = self;
    capable_self.flags = node_flags_for(true, true);
    Membership from_capable(capable_self, 40ms, {});
    from_capable.observe(incapable, false);
    CHECK(!from_capable.all_known_reachable());
    from_capable.observe(incapable, true);
    CHECK(from_capable.all_known_reachable());
}

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
        refused = std::string(error.what()).find("storage.hosts_extents is true") !=
                  std::string::npos;
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

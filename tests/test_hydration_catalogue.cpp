// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent/torrent_extent_journal.hpp"
#include "stepped_time.hpp"
#include "test_backend_support.hpp"
#include "api/acquisition_api.hpp"
#include "api/paging.hpp"
#include "subsystem/subsystem_abi.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "supervised.hpp"
#include "torrent/torrent_coordinator.hpp"

#ifdef MACHA_TEST_TORRENT_PLUGIN
#include <dlfcn.h>
#endif

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

#ifdef MACHA_TEST_TORRENT_PLUGIN
// Loads the real libmacha-torrent module the way SubsystemSupervisor does --
// dlopen, entry symbol, build-identity check -- and owns both the handle and
// the Subsystem so they are torn down in the right order (the instance must
// be gone before the library is unmapped).
class LoadedTorrentPlugin {
    void* handle_{};
    std::unique_ptr<Subsystem> subsystem_;

  public:
    explicit LoadedTorrentPlugin(const SubsystemContext& context) {
        handle_ = ::dlopen(MACHA_TEST_TORRENT_PLUGIN, RTLD_NOW | RTLD_LOCAL);
        REQUIRE(handle_ != nullptr);
        auto* symbol = ::dlsym(handle_, kSubsystemEntrySymbol);
        REQUIRE(symbol != nullptr);
        const auto* entry = reinterpret_cast<SubsystemEntryFunction>(symbol)();
        REQUIRE(entry != nullptr);
        REQUIRE(entry->build_identity == kBuildIdentity);
        subsystem_ = entry->create(context);
        REQUIRE(subsystem_ != nullptr);
    }

    ~LoadedTorrentPlugin() {
        subsystem_.reset();
        if (handle_) ::dlclose(handle_);
    }

    LoadedTorrentPlugin(const LoadedTorrentPlugin&) = delete;
    LoadedTorrentPlugin& operator=(const LoadedTorrentPlugin&) = delete;

    Subsystem& subsystem() { return *subsystem_; }
};
#endif

// The real query parser lives inside http.cpp's anonymous namespace; this is
// a small test-local equivalent for splitting a signed artwork URL's query
// string. Values here are always hex digits or decimal numbers, so no
// percent-decoding is needed.
std::map<std::string, std::string, std::less<>> parse_test_query(std::string_view query) {
    std::map<std::string, std::string, std::less<>> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        auto part = query.substr(pos, amp == std::string_view::npos ? query.size() - pos : amp - pos);
        auto eq = part.find('=');
        out[std::string(part.substr(0, eq))] =
            eq == std::string_view::npos ? "" : std::string(part.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return out;
}

// One node's filesystem, catalogue and catalogue hint queue, built as Service
// builds them: what CatalogueScanner, CatalogueApi, IngestManager and the
// catalogue predictors take. `guard` runs before each of the node's metadata
// commits is published, on the committing thread.
class CatalogueNode {
    TestNode node_;
    std::unique_ptr<CatalogueManager> catalogue_;
    std::unique_ptr<CatalogueHintQueue> hints_;

  public:
    explicit CatalogueNode(std::string_view name, const std::function<void(Config&)>& configure = {},
                           std::function<void(const MetadataPublicationContext&)> guard = {})
        : node_(name) {
        if (configure) configure(node_.config());
        if (guard) node_.set_publication_guard(std::move(guard));
        node_.start();
        auto& bare = node_.node();
        catalogue_ = std::make_unique<CatalogueManager>(bare, bare.local_state(), bare.metadata_server(),
                                                        node_.store(), node_.metadata(), bare.ledger());
        hints_ = std::make_unique<CatalogueHintQueue>(node_.config().state_path);
    }

    BareNode& node() { return node_.node(); }
    const Config& config() const { return node_.config(); }
    const std::filesystem::path& path() const { return node_.path(); }
    FileSystem& filesystem() { return node_.filesystem(); }
    MetadataManager& metadata() { return node_.metadata(); }
    DistributedStore& store() { return node_.store(); }
    CatalogueManager& catalogue() { return *catalogue_; }
    CatalogueHintQueue& hints() { return *hints_; }

    void mkdir(const std::string& path) { filesystem().mkdir(path, 0755, getuid(), getgid()); }

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

    CatalogueItem upsert(std::string id, CatalogueKind kind, std::string title,
                         std::optional<std::string> parent = {}) {
        CatalogueItem item;
        item.id = std::move(id);
        item.kind = kind;
        item.title = std::move(title);
        item.parent_id = std::move(parent);
        return catalogue().upsert(item);
    }
};

std::filesystem::path write_token(const std::filesystem::path& path, std::string_view token) {
    std::ofstream out(path);
    out << token << '\n';
    return path;
}

Json api_body(const HttpResponse& response) {
    return Json::parse(std::string(response.body.begin(), response.body.end()));
}

HttpRequest api_request(std::string method, std::string path, Bytes body = {},
                        std::map<std::string, std::string, std::less<>> headers = {}) {
    return {.method = std::move(method), .path = std::move(path), .query = {},
            .headers = std::move(headers), .body = std::move(body), .session = {}};
}

std::filesystem::path write_host_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

MACHA_FAST_TEST("hydration_catalogue", test_hydration_scheduler_orders_runs_and_follows_playback) {
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
}

MACHA_FAST_TEST("hydration_catalogue", test_catalogue_three_way_merge) {
    CatalogueSnapshot base;
    CatalogueItem a;
    a.id = "movie:a";
    a.kind = CatalogueKind::movie;
    a.title = "A";
    base.items.emplace(a.id, a);

    auto left = base;
    CatalogueItem b;
    b.id = "movie:b";
    b.kind = CatalogueKind::movie;
    b.title = "B";
    left.items.emplace(b.id, b);

    auto right = base;
    CatalogueItem c;
    c.id = "movie:c";
    c.kind = CatalogueKind::movie;
    c.title = "C";
    right.items.emplace(c.id, c);

    auto merged = merge_catalogue_snapshots(base, left, right);
    REQUIRE(merged.has_value());
    CHECK(merged->items.size() == 3);
    CHECK(merged->items.contains("movie:b"));
    CHECK(merged->items.contains("movie:c"));

    auto conflicting_left = base;
    auto conflicting_right = base;
    conflicting_left.items.at("movie:a").title = "Left";
    conflicting_right.items.at("movie:a").title = "Right";
    CHECK(!merge_catalogue_snapshots(base, conflicting_left, conflicting_right).has_value());

    CatalogueSnapshot structural_base;
    CatalogueItem parent;
    parent.id = "show:p";
    parent.kind = CatalogueKind::show;
    parent.title = "Parent";
    structural_base.items.emplace(parent.id, parent);
    auto delete_parent = structural_base;
    delete_parent.items.erase(parent.id);
    auto add_child = structural_base;
    CatalogueItem child;
    child.id = "episode:p:1";
    child.kind = CatalogueKind::episode;
    child.title = "Child";
    child.parent_id = parent.id;
    add_child.items.emplace(child.id, child);
    CHECK(!merge_catalogue_snapshots(structural_base, delete_parent, add_child).has_value());
}

MACHA_FAST_TEST("hydration_catalogue", test_replica_selector) {
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

MACHA_TEST("hydration_catalogue", test_cache_hydrator_fetches_to_persistent_cache) {
    TempDir temp;
    auto keyfile = temp.path() / "key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(temp.path() / "n1", keyfile, p1);
    auto c2 = config_for(temp.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c2.cache.path = temp.path() / "cache2";
    c2.cache.max_blocks = 32;

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));
    // Membership reachability is intentionally orthogonal to local backend
    // readiness. This test exercises cache hydration, not early lifecycle, so
    // wait for the DATA/cache planes before publishing test objects.
    REQUIRE(n1.wait_local_state_ready(10s));
    REQUIRE(n2.wait_local_state_ready(10s));

    DistributedStore source(n1, n1.local_state(), n1.resources.activity, n1.resources.data, n1.resources.memory, n1.resources.events);
    DistributedStore target(n2, n2.local_state(), n2.resources.activity, n2.resources.data, n2.resources.memory, n2.resources.events);
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
    REQUIRE(wait_until([&] { return concurrent.status().executor_completed >= 3; }, 1s));
    const auto live_executor = concurrent.status();
    CHECK(live_executor.executor_workers == concurrent_config.max_inflight);
    CHECK(live_executor.executor_submitted == 3);
    CHECK(live_executor.executor_completed == 3);
    CHECK(live_executor.executor_queued == 0);
    CHECK(live_executor.executor_peak_queued <= concurrent_config.max_inflight);
    concurrent.stop();
    auto concurrent_status = concurrent.status();
    CHECK(concurrent_status.peak_in_flight == 2);
    CHECK(concurrent_status.in_flight == 0);
    CHECK(concurrent_status.executor_workers == 0);
    CHECK(concurrent_status.executor_queued == 0);
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
    std::this_thread::sleep_for(25ms);
    CHECK(notifying_provider->calls.load() <= 2);
    notifying_provider->publish({{"event", {event_object}, 1000, "event-test"}});
    REQUIRE(wait_until([&] { return n2.block_cache().has(event_object); }, 3s));
    event_hydrator.stop();

    // A service stop can race a provider which is already inside hints(). The
    // scheduler must treat the resulting late executor submission as ordinary
    // cancellation: it must not throw out of the jthread entry point, abort the
    // process, or leave a promise owned by a stopped fetch queue.
    const auto shutdown_object = make_remote(70);
    class BlockingHints final : public HydrationHintProvider {
        std::mutex mutex_;
        std::condition_variable cv_;
        bool entered_{};
        bool released_{};
        ObjectId object_;
      public:
        explicit BlockingHints(ObjectId object) : object_(object) {}
        std::string_view name() const override { return "blocking-stop-test"; }
        std::vector<HydrationHint> hints() override {
            std::unique_lock lock(mutex_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [&] { return released_; });
            return {{"shutdown", {object_}, 1000, "shutdown-race"}};
        }
        bool wait_until_entered() {
            std::unique_lock lock(mutex_);
            return cv_.wait_for(lock, 2s, [&] { return entered_; });
        }
        void release() {
            std::lock_guard lock(mutex_);
            released_ = true;
            cv_.notify_all();
        }
    };
    auto blocking_provider = std::make_shared<BlockingHints>(shutdown_object);
    HydrationConfig shutdown_config;
    shutdown_config.max_inflight = 1;
    CacheHydrator shutdown_hydrator(target, shutdown_config);
    shutdown_hydrator.add_provider(blocking_provider);
    shutdown_hydrator.start();
    REQUIRE(blocking_provider->wait_until_entered());
    shutdown_hydrator.request_stop();
    blocking_provider->release();
    shutdown_hydrator.stop();
    const auto shutdown_status = shutdown_hydrator.status();
    CHECK(shutdown_status.in_flight == 0);
    CHECK(shutdown_status.executor_workers == 0);
    CHECK(shutdown_status.executor_queued == 0);
    CHECK(shutdown_status.executor_submitted == 0);
    CHECK(shutdown_status.executor_rejected == 1);
    CHECK(!n2.block_cache().has(shutdown_object));

    n2.stop();
    n1.stop();
}

MACHA_FAST_TEST("hydration_catalogue", test_media_probe_and_metadata_providers) {
    CHECK(catalogue_media_profile_frame_type() == FrameType::speculative);
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

    CatalogueSnapshot::MediaProfile detailed_profile;
    detailed_profile.probe.format = "mov,mp4,m4a,3gp,3g2,mj2";
    detailed_profile.probe.duration_seconds = 7265.125;
    detailed_profile.probe.bitrate = 5'123'456;
    detailed_profile.probe.streams.push_back(MediaStreamInfo{
        0, MediaStreamType::video, "hevc", "Main 10", "und", 3840, 2160,
        0, 0, 10, true, false, 4'700'000, false});
    detailed_profile.probe.streams.push_back(MediaStreamInfo{
        2, MediaStreamType::audio, "eac3", "", "eng", 0, 0,
        6, 48000, 24, true, false, 384'000, false});
    detailed_profile.probe.streams.push_back(MediaStreamInfo{
        7, MediaStreamType::subtitle, "hdmv_pgs_subtitle", "", "fra", 0, 0,
        0, 0, 0, false, true, 0, false});
    const std::string detailed_media_id = "macha:" + std::string(64, 'a');
    codec_snapshot.media_profiles.emplace(detailed_media_id, detailed_profile);
    codec_roundtrip = decode_catalogue(encode_catalogue(codec_snapshot));
    REQUIRE(codec_roundtrip.media_profiles.contains(detailed_media_id));
    CHECK(codec_roundtrip.media_profiles.at(detailed_media_id) == detailed_profile);
    CHECK(valid_catalogue_media_profile(
        detailed_media_id, codec_roundtrip.media_profiles.at(detailed_media_id)));

    // Unknown/incomplete semantic data remains decodable so one bad cached
    // profile cannot poison the complete catalogue snapshot. Consumers treat
    // it as a miss and use their bounded probe path.
    auto incomplete = detailed_profile;
    incomplete.complete = false;
    incomplete.probe.format.clear();
    codec_snapshot.media_profiles[detailed_media_id] = incomplete;
    codec_roundtrip = decode_catalogue(encode_catalogue(codec_snapshot));
    REQUIRE(codec_roundtrip.media_profiles.contains(detailed_media_id));
    CHECK(!valid_catalogue_media_profile(
        detailed_media_id, codec_roundtrip.media_profiles.at(detailed_media_id)));

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
             // A number the calendar has not reached is title text, not a
             // release year.
             MovieRegression{"/Movies/Blade Runner 2049.HDRip.XviD.AC3-EVO.avi", "Blade Runner 2049", 0},
             MovieRegression{"/Movies/Death Race 2050 1080p BluRay x265.mkv", "Death Race 2050", 0},
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
    // Requests to MusicBrainz are spaced by their gate's interval, a second
    // unless the gate is built with another. Each provider here has its own
    // gate, as the gate also carries the circuit.
    const auto unpaced = [] { return std::make_shared<MusicBrainzGate>(0ms); };
    MusicBrainzProvider mb(mb_http, mb_config, std::make_shared<MusicBrainzGate>(25ms));
    MediaProbe music_probe;
    music_probe.kind = MediaProbeKind::track;
    music_probe.artist = "Pink Floyd";
    music_probe.album = "The Dark Side of the Moon";
    music_probe.title = "Speak to Me";
    music_probe.track = 1;
    music_probe.media_id = "macha:test-track";
    const auto mb_started = Clock::now();
    auto mb_match = mb.lookup(music_probe);
    CHECK(Clock::now() - mb_started >= 25ms);
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
    MusicBrainzProvider mb_recording(mb_recording_http, mb_config, unpaced());
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
    MusicBrainzProvider mb_compilation(mb_compilation_http, mb_config, unpaced());
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
    MusicBrainzProvider mb_miss(mb_miss_http, mb_config, unpaced());
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
    MusicBrainzProvider mb_error(mb_error_http, mb_config, unpaced());
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
    // MusicBrainz backs off after one 503 and Discogs receives the same candidate.
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

    // Provider validity caches are optional acceleration, not process-lifetime
    // ownership of every title ever inspected.
    for (size_t i = 0; i < 300; ++i) {
        auto distinct = missing_movie;
        distinct.title = "Missing cache ownership " + std::to_string(i);
        CHECK(!tmdb_miss.lookup(distinct).has_value());
    }
    CHECK(tmdb_miss.cache_entries() <= 256);
    CHECK(tmdb_miss.cache_bytes() <= 8ULL * 1024 * 1024);

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
}

// A MusicBrainz release's tracks: every medium's tracks in the release's own
// order, under the track's title rather than the recording's, fetched through
// the gate the node's MusicBrainz providers share.
MACHA_FAST_TEST("hydration_catalogue", test_musicbrainz_release_tracks) {
    FakeHttpClient http;
    http.add("/ws/2/release/rel-2cd", 200, "application/json",
             R"JSON({"id":"rel-2cd","title":"Two Discs","media":[
                 {"position":1,"tracks":[
                     {"position":1,"number":"A1","title":"Opening","length":215000,
                      "recording":{"id":"rec-a","title":"Opening (album version)","length":216000}},
                     {"position":2,"title":"Untimed","length":null,
                      "recording":{"id":"rec-b","title":"Untimed","length":null}}]},
                 {"position":2,"tracks":[
                     {"position":1,"title":"Second Disc","length":null,
                      "recording":{"id":"rec-c","title":"Second Disc","length":90500}}]}]})JSON");
    http.add("/ws/2/release/rel-empty", 200, "application/json", R"({"id":"rel-empty"})");
    CatalogueMusicBrainzConfig config;
    config.contact = "https://example.test/macha";
    auto gate = std::make_shared<MusicBrainzGate>(0ms);
    MusicBrainzProvider mb(http, config, gate);
    {
        Lock lock(gate->mutex);
        CHECK(gate->last_request == std::chrono::steady_clock::time_point{});
    }

    const auto tracks = mb.release_tracks("rel-2cd");
    REQUIRE(tracks.size() == 3);
    struct Expected {
        int32_t disc;
        int32_t track;
        const char* title;
        std::optional<int64_t> length_ms;
        const char* recording_id;
    };
    const std::vector<Expected> expected{
        {1, 1, "Opening", 215000, "rec-a"},
        {1, 2, "Untimed", std::nullopt, "rec-b"},
        // A track without a length of its own has its recording's.
        {2, 1, "Second Disc", 90500, "rec-c"},
    };
    for (size_t i = 0; i < expected.size(); ++i) {
        std::cerr << "row: " << expected[i].title << "\n";
        CHECK(tracks[i].disc_number == expected[i].disc);
        CHECK(tracks[i].track_number == expected[i].track);
        CHECK(tracks[i].title == expected[i].title);
        CHECK(tracks[i].length_ms == expected[i].length_ms);
        CHECK(tracks[i].recording_id == expected[i].recording_id);
    }
    CHECK(http.requests_containing("/ws/2/release/rel-2cd?inc=recordings") == 1);
    {
        Lock lock(gate->mutex);
        CHECK(gate->last_request != std::chrono::steady_clock::time_point{});
    }
    // The release is cached, and a release without media has no tracks.
    CHECK(mb.release_tracks("rel-2cd").size() == 3);
    CHECK(mb.release_tracks("rel-empty").empty());
    CHECK(http.requests() == 2);

    // The gate's circuit, opened by another provider's failure, refuses this
    // provider's next request before it reaches MusicBrainz.
    FakeHttpClient down;
    down.add("/ws/2/release/", 503, "application/json", "{}");
    MusicBrainzProvider failing(down, config, gate);
    const auto throws = [](MusicBrainzProvider& provider, std::string_view id) {
        try {
            (void)provider.release_tracks(id);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    CHECK(throws(failing, "rel-down"));
    CHECK(down.requests() == 1);
    CHECK(throws(mb, "rel-unfetched"));
    CHECK(http.requests() == 2);
}

MACHA_TEST("hydration_catalogue", test_catalogue_scanner_matches_binds_and_reconciles_a_library) {
    // CatalogueScanner end to end over one node's namespace and catalogue,
    // with providers answered by a fake HTTP client: match and bind, stay
    // idempotent, respect manual edits, rematch after Clear Metadata, reconcile
    // deletions, stop mid-request, and budget provider requests fairly.
    CatalogueNode node("catalogue-scanner-library");

    // Music scanning is tag-first and provider-root scoped. Deliberately put a
    // tagged MP3 under misleading collection/grouping directories: embedded
    // metadata must win, while untagged filename fallback must not manufacture
    // an artist/album from those same directories.
    node.filesystem().mkdir("/Music", 0755, getuid(), getgid());
    const std::string collection = "/Music/Scooter Full Discography (Albums & Singles 1994-2011)";
    node.filesystem().mkdir(collection, 0755, getuid(), getgid());
    const std::string singles = collection + "/Singles";
    node.filesystem().mkdir(singles, 0755, getuid(), getgid());
    auto fixture_bytes = [](const char* name) {
        auto path = std::filesystem::path(MACHA_TEST_SOURCE_DIR) / "tests" / "fixtures" / name;
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return Bytes(bytes.begin(), bytes.end());
    };
    auto write_fixture = [&](const std::string& path, const Bytes& bytes) {
        node.filesystem().create_file(path, 0644, getuid(), getgid());
        auto writer = node.filesystem().open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    };

    const auto untagged_bytes = fixture_bytes("untagged.mp3");

    FakeHttpClient music_probe_http;
    CatalogueMusicProviderConfig music_source_config;
    music_source_config.roots = {"/Music"};
    music_source_config.musicbrainz.enabled = false;
    MusicScanProvider music_source(music_probe_http, music_source_config);

    const std::string loose_path = singles + "/Scooter - The First Time (Raven Remix).mp3";
    write_fixture(loose_path, untagged_bytes);
    auto loose_entry = node.filesystem().getattr(loose_path);
    auto loose_probe = music_source.probe(node.filesystem(), "/Music", loose_path, loose_entry);
    REQUIRE(loose_probe.has_value());
    CHECK(loose_probe->artist == "Scooter");
    CHECK(loose_probe->album.empty());
    CHECK(loose_probe->title == "The First Time (Raven Remix)");

    const std::string nested_album = singles + "/13 - [1996] I'm Raving CDM";
    node.filesystem().mkdir(nested_album, 0755, getuid(), getgid());
    const std::string nested_path = nested_album + "/01 - I'm Raving.mp3";
    write_fixture(nested_path, untagged_bytes);
    auto nested_entry = node.filesystem().getattr(nested_path);
    auto nested_probe = music_source.probe(node.filesystem(), "/Music", nested_path, nested_entry);
    REQUIRE(nested_probe.has_value());
    CHECK(nested_probe->artist.empty());
    CHECK(nested_probe->album.empty());
    CHECK(nested_probe->track == 1);
    CHECK(nested_probe->title == "I'm Raving");

    node.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
    node.filesystem().create_file("/Movies/Blade.Runner.2049.2017.1080p.mkv", 0644,
                                     getuid(), getgid());
    auto bytes = pattern(32768);
    auto writer = node.filesystem().open_write(
        "/Movies/Blade.Runner.2049.2017.1080p.mkv", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto entry = node.filesystem().getattr(
        "/Movies/Blade.Runner.2049.2017.1080p.mkv");
    auto media_id = file_media_id(entry);

    auto scanner_token = node.path() / "scanner-tmdb.token";
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
    auto profile_engine = std::make_shared<FakeMediaEngine>();
    CatalogueScanner scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(), node.hints(),
                             scanner_config, std::move(fake_http), 5s, profile_engine);
    const auto namespace_before_scan = node.filesystem().namespace_signature();
    CHECK(scanner.scan_once() == 1);
    CHECK(node.filesystem().namespace_signature() == namespace_before_scan);
    auto catalogued = node.catalogue().get("tmdb:movie:335984");
    REQUIRE(catalogued.has_value());
    CHECK(catalogued->title == "Blade Runner 2049");
    CHECK(catalogued->external_ids.at("tmdb_collection") == "422837");
    CHECK(catalogued->external_ids.at("macha_scanner") == "1");
    CHECK(catalogued->media_ids == std::vector<std::string>{media_id});
    REQUIRE(node.catalogue().media_profile(media_id).has_value());
    CHECK(profile_engine->probes() == 1);
    CHECK(catalogued->artwork.size() == 2);
    for (const auto& art : catalogued->artwork)
        CHECK(node.node().local_store().has(art.id));
    auto revision = catalogued->revision;
    CHECK(scanner.scan_once() == 0);
    REQUIRE(node.catalogue().get("tmdb:movie:335984").has_value());
    CHECK(node.catalogue().get("tmdb:movie:335984")->revision == revision);

    // Manual metadata editing is authoritative. A later scanner discovery for
    // another local copy may add media bindings, but must not silently overwrite
    // the user's descriptive changes.
    auto manual = *node.catalogue().get("tmdb:movie:335984");
    manual.title = "Blade Runner Custom";
    manual.sort_title = manual.title;
    manual.synopsis = "A manually edited synopsis.";
    manual.external_ids["macha_metadata_locked"] = "1";
    auto manually_saved = node.catalogue().upsert(std::move(manual), revision);
    revision = manually_saved.revision;

    // A second file resolving to the same title adds another binding without
    // losing the already-bound media identity.
    const std::string alternate = "/Movies/Blade.Runner.2049.2017.Remux.mkv";
    node.filesystem().create_file(alternate, 0644, getuid(), getgid());
    auto alternate_bytes = pattern(32769);
    auto alternate_writer = node.filesystem().open_write(alternate, true);
    REQUIRE(alternate_writer->write(0, alternate_bytes) == alternate_bytes.size());
    alternate_writer->commit();
    CHECK(node.filesystem().namespace_signature() != namespace_before_scan);
    auto alternate_id = file_media_id(node.filesystem().getattr(alternate));
    CHECK(alternate_id != media_id);
    CHECK(scanner.scan_once() == 1);
    auto twice = node.catalogue().get("tmdb:movie:335984");
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
    // matched item, and returns the exact media identities that became
    // unbound, so only those files go to the unmatched list.
    auto cleared = node.catalogue().clear_metadata_with_media(
        "tmdb:movie:335984", twice->revision);
    CHECK(cleared.removed_items == 1);
    CHECK(cleared.media_ids.size() == 2);
    CHECK(std::find(cleared.media_ids.begin(), cleared.media_ids.end(), media_id) !=
          cleared.media_ids.end());
    CHECK(std::find(cleared.media_ids.begin(), cleared.media_ids.end(), alternate_id) !=
          cleared.media_ids.end());
    CHECK(!node.catalogue().get("tmdb:movie:335984").has_value());

    // The scanner's match again, holding both files, for what follows.
    auto restored = *twice;
    restored.external_ids.erase("macha_metadata_locked");
    restored.external_ids["macha_scanner"] = "1";
    (void)node.catalogue().upsert(restored);

    // Deletion reconciles duplicate bindings one at a time and removes the
    // scanner-owned item only after the final copy goes.
    node.filesystem().unlink("/Movies/Blade.Runner.2049.2017.1080p.mkv");
    CHECK(scanner.scan_once() == 0);
    auto partial = node.catalogue().get("tmdb:movie:335984");
    REQUIRE(partial.has_value());
    CHECK(std::find(partial->media_ids.begin(), partial->media_ids.end(), media_id) != partial->media_ids.end());
    CHECK(std::find(partial->media_ids.begin(), partial->media_ids.end(), alternate_id) != partial->media_ids.end());

    // Once the previously unavailable root exists, the scan is complete and
    // destructive reconciliation may safely remove the vanished first binding.
    node.filesystem().mkdir("/Missing", 0755, getuid(), getgid());
    CHECK(scanner.scan_once() == 0);
    auto remaining = node.catalogue().get("tmdb:movie:335984");
    REQUIRE(remaining.has_value());
    CHECK(remaining->media_ids == std::vector<std::string>{alternate_id});

    node.filesystem().unlink(alternate);
    CHECK(node.filesystem().readdir("/Movies").empty());
    CHECK(scanner.scan_once() == 0);
    CHECK(!node.catalogue().get("tmdb:movie:335984").has_value());

    // Shutdown must not wait for a complete catalogue scan. request_stop()
    // propagates into the HTTP client so an in-flight provider request is
    // interrupted, and scan_once(stop) abandons the partial pass without a
    // reconciliation commit.
    const std::string shutdown_path = "/Movies/Shutdown.Test.2020.mkv";
    node.filesystem().create_file(shutdown_path, 0644, getuid(), getgid());
    auto shutdown_writer = node.filesystem().open_write(shutdown_path, true);
    auto shutdown_bytes = pattern(32769);
    REQUIRE(shutdown_writer->write(0, shutdown_bytes) == shutdown_bytes.size());
    shutdown_writer->commit();

    auto blocking_http = std::make_unique<BlockingHttpClient>();
    auto* blocking_http_ptr = blocking_http.get();
    auto cancel_config = scanner_config;
    cancel_config.movies.roots = {"/Movies"};
    CatalogueScanner cancel_scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(), node.hints(),
                                    cancel_config, std::move(blocking_http));
    // Scanner startup is idle on an already-operated library, so request the
    // pass whose in-flight provider request this test exercises.
    cancel_scanner.request_rescan();
    cancel_scanner.start();
    REQUIRE(wait_until([&] { return blocking_http_ptr->entered(); }, 1s));
    const auto stop_started = Clock::now();
    cancel_scanner.stop();
    CHECK(blocking_http_ptr->stopped());
    CHECK(Clock::now() - stop_started < 1s);

    // Online metadata enrichment is bounded by actual provider HTTP requests,
    // not by the number of files. Completed discoveries commit normally and a
    // later pass resumes with already-bound media skipped.
    node.filesystem().mkdir("/Budget", 0755, getuid(), getgid());
    const std::array<std::pair<const char*, uint8_t>, 3> budget_files{{
        {"/Budget/Budget.One.2020.mkv", 1},
        {"/Budget/Budget.Two.2021.mkv", 2},
        {"/Budget/Budget.Three.2022.mkv", 3},
    }};
    std::set<std::string> budget_media_ids;
    for (const auto& [path, marker] : budget_files) {
        node.filesystem().create_file(path, 0644, getuid(), getgid());
        auto w = node.filesystem().open_write(path, true);
        REQUIRE(w->write(0, Bytes{marker, 2, 3, 4}) == 4);
        w->commit();
        budget_media_ids.insert(file_media_id(node.filesystem().getattr(path)));
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
    CatalogueScanner budget_scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(), node.hints(),
                                    budget_config, std::move(budget_http));
    CHECK(budget_scanner.scan_once() == 2);
    CHECK(budget_http_ptr->requests() == 4);
    size_t first_batch_items = 0;
    for (const auto* id : {"tmdb:movie:2001", "tmdb:movie:2002", "tmdb:movie:2003"})
        if (node.catalogue().get(id).has_value()) ++first_batch_items;
    CHECK(first_batch_items == 2);
    CHECK(budget_scanner.scan_once() == 1);
    CHECK(budget_http_ptr->requests() == 6);
    CHECK(node.catalogue().get("tmdb:movie:2001").has_value());
    CHECK(node.catalogue().get("tmdb:movie:2002").has_value());
    CHECK(node.catalogue().get("tmdb:movie:2003").has_value());

    // Provider request budgeting is fair across media domains. A long run of
    // movie misses must not consume the whole batch before TV and Music get a
    // lookup opportunity.
    node.filesystem().mkdir("/FairMovies", 0755, getuid(), getgid());
    for (int i = 1; i <= 4; ++i) {
        const auto path = "/FairMovies/Fair.Movie." + std::to_string(i) + ".mkv";
        node.filesystem().create_file(path, 0644, getuid(), getgid());
        auto w = node.filesystem().open_write(path, true);
        REQUIRE(w->write(0, Bytes{static_cast<uint8_t>(i), 2, 3, 4}) == 4);
        w->commit();
    }
    node.filesystem().mkdir("/FairTV", 0755, getuid(), getgid());
    const std::string fair_tv = "/FairTV/Fair.Show.S01E01.mkv";
    node.filesystem().create_file(fair_tv, 0644, getuid(), getgid());
    auto fair_tv_writer = node.filesystem().open_write(fair_tv, true);
    REQUIRE(fair_tv_writer->write(0, Bytes{9, 8, 7, 6}) == 4);
    fair_tv_writer->commit();
    node.filesystem().mkdir("/FairMusic", 0755, getuid(), getgid());
    node.filesystem().mkdir("/FairMusic/Fair Artist", 0755, getuid(), getgid());
    node.filesystem().mkdir("/FairMusic/Fair Artist/Fair Album", 0755, getuid(), getgid());
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
    CatalogueScanner fair_scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(), node.hints(),
                                  fair_config, std::move(fair_http));
    CHECK(fair_scanner.scan_once() == 0);
    CHECK(fair_http_ptr->requests() == 3);
    CHECK(fair_http_ptr->requests_containing("/search/movie") == 1);
    CHECK(fair_http_ptr->requests_containing("/search/tv") == 1);
    CHECK(fair_http_ptr->requests_containing("/ws/2/release?") == 1);
}

MACHA_TEST("hydration_catalogue", test_catalogue_scanner_admits_only_what_is_ready_to_catalogue) {
    // CatalogueScanner over one node's namespace, each behaviour under its own
    // root: a restart does not rescan an unchanged namespace; zero-length files
    // wait for committed content; a terminal profile job is not requeued; paths
    // carrying an ignore term are skipped; and a hint newer than its batch's
    // snapshot is deferred, not failed.
    CatalogueNode node("catalogue-scanner");
    const auto token = write_token(node.path() / "tmdb.token", "test-token");
    const auto scanner_for = [&](const std::string& root) {
        CatalogueScannerConfig config;
        config.enabled = true;
        config.movies.roots = {root};
        config.movies.tmdb.token_file = token;
        config.tv.enabled = false;
        config.music.enabled = false;
        return config;
    };

    // Hint state on disk and no scanner.state: the elected coordinator seeds
    // scanner.state from the current namespace and waits for its interval,
    // rather than walking the root at process start.
    {
        node.mkdir("/Restart");
        (void)node.write("/Restart/Unbound.2026.mkv", pattern(4096));
        std::filesystem::create_directories(node.config().state_path / "catalogue");
        {
            std::ofstream out(node.config().state_path / "catalogue" / "hints.json");
            out << R"({"version":2,"hints":[]})";
        }
        auto config = scanner_for("/Restart");
        config.interval = 1h;
        config.rescan_debounce = 50ms;
        config.rescan_max_delay = 1s;
        auto http = std::make_unique<FakeHttpClient>();
        auto* requests = http.get();
        CatalogueScanner scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(),
                                 node.hints(), config, std::move(http));
        scanner.start();
        REQUIRE(wait_until([&] {
            return std::filesystem::exists(node.config().state_path / "catalogue" / "scanner.state");
        }, 2s));
        // Past the rescan debounce: a walk would have happened by now.
        std::this_thread::sleep_for(80ms);
        scanner.stop();
        CHECK(node.hints().summary().total == 0);
        CHECK(requests->requests() == 0);
    }

    // Discovery must not manufacture one shared identity for every zero-length
    // shell, queue provider work, or contact TMDB; a hint racing publication
    // stays deferred; committed content reopens the path and matches at once.
    {
        node.mkdir("/Transient");
        const std::string path = "/Transient/Transient.Movie.2026.mkv";
        node.filesystem().create_file(path, 0644, getuid(), getgid());
        const auto empty_entry = node.filesystem().getattr(path);
        REQUIRE(empty_entry.size == 0);
        const auto empty_media_id = file_media_id(empty_entry);
        auto http = std::make_unique<FakeHttpClient>();
        auto* requests = http.get();
        http->add("/search/movie", 200, "application/json",
                  R"({"results":[{"id":4242,"title":"Transient Movie","release_date":"2026-01-01"}]})");
        http->add("/movie/4242", 200, "application/json",
                  R"({"id":4242,"title":"Transient Movie","release_date":"2026-01-01"})");
        CatalogueScanner scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(),
                                 node.hints(), scanner_for("/Transient"), std::move(http));
        CHECK(scanner.scan_once() == 0);
        CHECK(node.hints().summary().total == 0);
        CHECK(requests->requests() == 0);

        const auto hint_id = node.hints().submit(path, "namespace", empty_media_id,
                                                 CatalogueHintPriority::namespace_mutation);
        CHECK(scanner.scan_once() == 0);
        auto waiting = node.hints().get(hint_id);
        REQUIRE(waiting.has_value());
        CHECK(waiting->state == CatalogueHintState::deferred);
        CHECK(waiting->result.empty());
        CHECK(waiting->error == "namespace media file has no committed content yet");
        CHECK(requests->requests() == 0);

        const auto committed_media_id = node.write(path, pattern(32768));
        CHECK(committed_media_id != empty_media_id);
        CHECK(scanner.scan_once() == 1);
        auto item = node.catalogue().get("tmdb:movie:4242");
        REQUIRE(item.has_value());
        CHECK(item->media_ids == std::vector<std::string>{committed_media_id});
        CHECK(requests->requests() == 2);
    }

    // A retry of a failed profile job observes the terminal result instead of
    // reopening it; playback reads zero as leave to use its bounded fallback.
    {
        node.mkdir("/Profiles");
        const auto media_id = node.write("/Profiles/Profile.Failure.2026.mp4", pattern(32771));
        CatalogueScanner scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(),
                                 node.hints(), scanner_for("/Profiles"), std::make_unique<FakeHttpClient>(), 5s,
                                 std::make_shared<FakeMediaEngine>());
        CHECK(scanner.request_media_profiles({media_id}) == 1);
        std::string queued_id;
        for (const auto& hint : node.hints().list())
            if (hint.state == CatalogueHintState::queued) queued_id = hint.id;
        REQUIRE(!queued_id.empty());
        node.hints().fail(queued_id, "synthetic_failure", "synthetic profile failure");
        CHECK(scanner.request_media_profiles({media_id}) == 0);
        auto terminal = node.hints().get(queued_id);
        REQUIRE(terminal.has_value());
        CHECK(terminal->state == CatalogueHintState::failed);
        CHECK(!node.catalogue().media_profile(media_id).has_value());
    }

    // Release samples sit beside the film they sample and name the same title.
    // The default ignore_terms keeps them out; with no terms all three bind.
    {
        node.mkdir("/Samples");
        node.mkdir("/Samples/Sample");
        const auto film = node.write("/Samples/Blade.Runner.2049.2017.1080p.mkv", pattern(32768, 1));
        (void)node.write("/Samples/Sample/Blade.Runner.2049.2017.1080p.mkv", pattern(32768, 2));
        (void)node.write("/Samples/Blade.Runner.2049.2017.1080p-sample.mkv", pattern(32768, 3));
        const auto blade_runner = [] {
            auto http = std::make_unique<FakeHttpClient>();
            http->add("/search/movie", 200, "application/json",
                      R"({"results":[{"id":335984,"title":"Blade Runner 2049","release_date":"2017-10-04"}]})");
            http->add("/movie/335984", 200, "application/json",
                      R"({"id":335984,"title":"Blade Runner 2049","release_date":"2017-10-04"})");
            return http;
        };
        const auto config = scanner_for("/Samples");
        CatalogueScanner scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(),
                                 node.hints(), config, blade_runner(), 5s, std::make_shared<FakeMediaEngine>());
        CHECK(scanner.scan_once() == 1);
        const auto item = node.catalogue().get("tmdb:movie:335984");
        REQUIRE(item.has_value());
        CHECK(item->media_ids == std::vector<std::string>{film});

        auto everything_config = config;
        everything_config.ignore_terms.clear();
        CatalogueScanner everything(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(),
                                    node.hints(), everything_config, blade_runner(), 5s,
                                    std::make_shared<FakeMediaEngine>());
        (void)everything.scan_once();
        const auto all = node.catalogue().get("tmdb:movie:335984");
        REQUIRE(all.has_value());
        CHECK(all->media_ids.size() == 3);
    }

    // A hint created after its batch's namespace snapshot is deferred to the
    // next batch; absent from a snapshot taken after the hint, it is gone.
    {
        node.mkdir("/Late");
        CatalogueScanner scanner(node.node(), node.node().metadata_server(), node.filesystem(), node.catalogue(),
                                 node.hints(), scanner_for("/Late"), std::make_unique<FakeHttpClient>());
        const auto claim = [&](const std::string& id) {
            // Earlier blocks left settled hints; claim until this one comes up.
            for (auto claimed = node.hints().claim_next(); claimed; claimed = node.hints().claim_next())
                if (claimed->id == id) return *claimed;
            throw std::runtime_error("hint was never claimable: " + id);
        };
        const auto snapshot = node.metadata().snapshot(); // no such file in it
        const auto taken = unix_ms();
        DistributedStore::DurabilityBatch batch;
        const auto late_id = node.hints().submit("/Late/Late (2026)/Late.mkv", "ingest", "job",
                                                 CatalogueHintPriority::ingest);
        const auto late = claim(late_id);
        REQUIRE(late.created_unix_ms >= taken);
        CHECK(!scanner.prepare_hint(late, {}, snapshot, taken, batch).has_value());
        CHECK(node.hints().get(late_id)->state == CatalogueHintState::deferred);
        CHECK(node.hints().get(late_id)->error_code == "path_not_yet_visible");

        const auto gone_id = node.hints().submit("/Late/Gone (2026)/Gone.mkv", "ingest", "job",
                                                 CatalogueHintPriority::ingest);
        const auto gone = claim(gone_id);
        const auto later = node.metadata().snapshot();
        CHECK(!scanner.prepare_hint(gone, {}, later, unix_ms() + 1, batch).has_value());
        CHECK(node.hints().get(gone_id)->state == CatalogueHintState::failed);
        CHECK(node.hints().get(gone_id)->error_code == "path_missing");
    }
}

MACHA_TEST("hydration_catalogue", test_catalogue_non_coordinator_idle_does_not_spin) {
    TempDir temp;
    auto keyfile = temp.path() / "cluster.key";
    write_key(keyfile);
    auto keys = load_cluster_keys(keyfile);

    auto c1 = config_for(temp.path() / "catalogue-idle-1", keyfile, free_port());
    auto c2 = config_for(temp.path() / "catalogue-idle-2", keyfile, free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }, 5s));

    Service* non_coordinator = s1.node().node_id() > s2.node().node_id() ? &s1 : &s2;
    CatalogueScannerConfig scanner_config;
    scanner_config.enabled = true;
    scanner_config.interval = 1h;
    scanner_config.movies.enabled = false;
    scanner_config.tv.enabled = false;
    scanner_config.music.enabled = false;

    auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::all);
    Log::set_logger(capture);
    CatalogueScanner scanner(non_coordinator->node(), non_coordinator->metadata_server(),
                             non_coordinator->filesystem(),
                             non_coordinator->catalogue(), non_coordinator->catalogue_hints(),
                             scanner_config, std::make_unique<FakeHttpClient>(), 1s);
    scanner.start();
    std::this_thread::sleep_for(1200ms);
    scanner.stop();
    const auto records = capture->records();
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));

    uint64_t max_iterations = 0;
    bool saw_report = false;
    for (const auto& [level, message] : records) {
        if (level != LogLevel::all ||
            message.find("DIAG thread name=macha-catalogue") == std::string::npos)
            continue;
        const auto marker = message.find(" iterations=");
        REQUIRE(marker != std::string::npos);
        max_iterations = std::max(
            max_iterations,
            static_cast<uint64_t>(std::stoull(message.substr(marker + 12))));
        saw_report = true;
    }
    CHECK(saw_report);
    // The idle non-coordinator has only the one-second membership/coordinator
    // observation cadence. A stale coordinator-only deadline must never turn
    // this into a zero-timeout polling loop.
    CHECK(max_iterations <= 20);

    s2.stop();
    s1.stop();
}

MACHA_FAST_TEST("hydration_catalogue", test_catalogue_hint_queue_follows_machadfs_rename_and_delete) {
    TempDir temp;
    const auto state = temp.path() / "manage-hints";

    CatalogueHintQueue hints(state);
    const auto original_id = hints.submit("/Movies/A/unknown.mkv", "scanner", "macha:stable",
                                          CatalogueHintPriority::periodic_scan);
    REQUIRE(hints.claim_next().has_value());
    hints.mark_no_match(original_id, "movies", "macha:stable", "no provider match");

    CHECK(hints.rename_prefix("/Movies/A", "/Movies/B") == 1);
    auto moved = hints.list();
    REQUIRE(moved.size() == 1);
    CHECK(moved.front().path == "/Movies/B/unknown.mkv");
    CHECK(moved.front().media_id == "macha:stable");
    CHECK(moved.front().state == CatalogueHintState::no_match);
    CHECK(moved.front().id != original_id);

    const auto processing_id = hints.submit("/Movies/B/in-flight.mkv", "scanner", "macha:flight",
                                            CatalogueHintPriority::periodic_scan);
    auto claimed = hints.claim_next();
    REQUIRE(claimed.has_value());
    CHECK(claimed->id == processing_id);
    CHECK(hints.rename_prefix("/Movies/B/in-flight.mkv", "/Movies/C/in-flight.mkv") == 1);
    CHECK(!hints.get(processing_id).has_value());
    auto after_processing_move = hints.list();
    auto requeued = std::find_if(after_processing_move.begin(), after_processing_move.end(), [](const auto& hint) {
        return hint.path == "/Movies/C/in-flight.mkv";
    });
    REQUIRE(requeued != after_processing_move.end());
    CHECK(requeued->state == CatalogueHintState::queued);

    CHECK(hints.erase_prefix("/Movies/B") == 1);
    auto after_erase = hints.list();
    CHECK(std::none_of(after_erase.begin(), after_erase.end(), [](const auto& hint) {
        return hint.path == "/Movies/B/unknown.mkv";
    }));
}

MACHA_FAST_TEST("hydration_catalogue", test_catalogue_hint_queue_persistence_coalescing_and_priority) {
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

    // Terminal worker progress is intentionally coalesced rather than forcing a
    // complete hints.json rewrite per item. Destruction is a durability boundary:
    // the final dirty terminal state must still survive a clean shutdown.
    const auto terminal_state = temp.path() / "terminal-state";
    std::string terminal_id;
    {
        CatalogueHintQueue terminal_queue(terminal_state);
        terminal_id = terminal_queue.submit("/Movies/Terminal.mkv", "scanner", "macha:terminal",
                                            CatalogueHintPriority::periodic_scan);
        REQUIRE(terminal_queue.claim_next().has_value());
        terminal_queue.mark_no_match(terminal_id, "movies", "macha:terminal", "no match");
        CHECK(terminal_queue.summary().pending == 0);
    }
    {
        CatalogueHintQueue terminal_queue(terminal_state);
        auto recovered = terminal_queue.get(terminal_id);
        REQUIRE(recovered.has_value());
        CHECK(recovered->state == CatalogueHintState::no_match);
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
        CHECK(!failure_queue.record_failure(failure_id, "synthetic_failure", "first failure", 0, 2));
        auto once = failure_queue.get(failure_id);
        REQUIRE(once.has_value());
        CHECK(once->state == CatalogueHintState::deferred);
        CHECK(once->failures == 1);
        REQUIRE(failure_queue.claim_next().has_value());
        CHECK(failure_queue.record_failure(failure_id, "synthetic_failure", "second failure", 0, 2));
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

    // Infrastructure unavailability is scheduling state, not evidence that the
    // media/provider match is bad. Deferring work must therefore preserve the
    // semantic failure count exactly, even when the hint already has one real
    // failure recorded.
    const auto infrastructure_state = temp.path() / "infrastructure-state";
    {
        CatalogueHintQueue queue(infrastructure_state);
        const auto id = queue.submit("/Movies/Retry.mkv", "scanner", "macha:retry",
                                     CatalogueHintPriority::periodic_scan);
        REQUIRE(queue.claim_next().has_value());
        CHECK(!queue.record_failure(id, "provider_error", "provider parse failure", 0, 5));
        auto failed_once = queue.get(id);
        REQUIRE(failed_once.has_value());
        CHECK(failed_once->failures == 1);
        REQUIRE(queue.claim_next().has_value());
        queue.defer(id, "catalogue_unavailable", "metadata durability temporarily unavailable", unix_ms() + 1000);
        auto deferred = queue.get(id);
        REQUIRE(deferred.has_value());
        CHECK(deferred->state == CatalogueHintState::deferred);
        CHECK(deferred->failures == 1);
        CHECK(deferred->error == "metadata durability temporarily unavailable");
        // The code is primary, and it survives a restart of the queue.
        CHECK(deferred->error_code == "catalogue_unavailable");
        CHECK(failed_once->error_code == "provider_error");
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

MACHA_FAST_TEST("hydration_catalogue", test_a_torrent_jobs_publication_travels_the_wire_and_reaches_the_api) {
    // A downloaded torrent waits while its extents are published into the
    // store; the job says how far that has got, and why it is waiting.
    TorrentJob job;
    job.id = "t1";
    job.state = TorrentJobState::downloaded;
    job.bytes_total = 2'607'096'508;
    job.bytes_completed = job.bytes_total;
    job.publication = TorrentJob::Publication{246, 624, 1'031'798'784, 2'607'096'508, 4200};
    job.waiting_reason = "extent_publication";
    job.swarm = TorrentJob::Swarm{12, std::nullopt, 0.75};
    const auto wire = parse_torrent_job_wire(torrent_job_wire_json(job));
    REQUIRE(wire.publication.has_value());
    CHECK(wire.publication->published_extents == 246);
    CHECK(wire.publication->extents == 624);
    CHECK(wire.publication->published_bytes == 1'031'798'784);
    CHECK(wire.publication->bytes == 2'607'096'508);
    CHECK(wire.publication->progress_age_ms == 4200);
    CHECK(wire.waiting_reason == "extent_publication");
    const auto api = torrent_job_api_json(wire);
    CHECK(api.find("publication")->find("published_extents")->asUInt64() == 246);
    CHECK(api.find("waiting_reason")->asString() == "extent_publication");
    // The swarm, with what no tracker reported left null.
    CHECK(api.find("swarm")->find("seeds")->asUInt64() == 12);
    CHECK(api.find("swarm")->find("peers")->isNull());
    CHECK(api.find("swarm")->find("availability")->asNumber() == 0.75);
    // Never persisted: a restart reports it afresh from the backend.
    const auto stored = parse_torrent_job(torrent_job_json(job));
    CHECK(!stored.publication.has_value());
    CHECK(stored.waiting_reason.empty());
    CHECK(!stored.swarm.has_value());
    // None reported: null, never zero.
    TorrentJob quiet;
    quiet.id = "t2";
    const auto none = torrent_job_api_json(parse_torrent_job_wire(torrent_job_wire_json(quiet)));
    CHECK(none.find("publication")->isNull());
    CHECK(none.find("waiting_reason")->isNull());
    CHECK(none.find("swarm")->isNull());
}

namespace {

// The TorrentService a coordinator drives, as a table of jobs: adopt, pause,
// resume, cancel and clear move a job's state as the plugin's manager does,
// with no engine behind them.
class TableTorrentService final : public TorrentService {
    mutable std::mutex mutex_;
    std::map<std::string, TorrentJob, std::less<>> jobs_;

    bool set_state(std::string_view id, TorrentJobState state) {
        std::lock_guard lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) return false;
        found->second.state = state;
        return true;
    }

  public:
    void put(TorrentJob job) {
        std::lock_guard lock(mutex_);
        jobs_[job.id] = std::move(job);
    }

    bool enabled() const noexcept override { return true; }
    void reconfigure(TorrentConfig) override {}
    std::string add(std::string) override { throw std::runtime_error("adds go through the coordinator"); }
    std::string add_search_result(std::string) override {
        throw std::runtime_error("adds go through the coordinator");
    }
    std::vector<TorrentJob> jobs() const override {
        std::lock_guard lock(mutex_);
        std::vector<TorrentJob> out;
        for (const auto& [_, job] : jobs_) out.push_back(job);
        return out;
    }
    std::optional<TorrentJob> job(std::string_view id) const override {
        std::lock_guard lock(mutex_);
        const auto found = jobs_.find(id);
        return found == jobs_.end() ? std::nullopt : std::optional<TorrentJob>{found->second};
    }
    bool pause(std::string_view id) override { return set_state(id, TorrentJobState::paused); }
    bool resume(std::string_view id) override { return set_state(id, TorrentJobState::downloading); }
    bool retry(std::string_view) override { return false; }
    bool cancel(std::string_view id) override { return set_state(id, TorrentJobState::cancelled); }
    bool clear(std::string_view id) override {
        std::lock_guard lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) return false;
        jobs_.erase(found);
        return true;
    }
    Placement place(std::string_view, bool) override { return {}; }
    Offer offer() const override { return {true, {}, 8, 0}; }
    Resolved resolve(std::string_view uri, bool) override {
        return {std::string(uri), magnet_info_hash(uri), magnet_display_name(uri)};
    }
    std::string adopt(std::string_view id, std::string_view magnet, bool held) override {
        TorrentJob job;
        job.id = std::string(id);
        job.source_uri = std::string(magnet);
        job.info_hash = magnet_info_hash(magnet);
        job.name = magnet_display_name(magnet);
        job.state = held ? TorrentJobState::paused : TorrentJobState::downloading;
        std::lock_guard lock(mutex_);
        jobs_.try_emplace(job.id, std::move(job));
        return std::string(id);
    }
};

} // namespace

MACHA_TEST("hydration_catalogue", test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner) {
    // TorrentCoordinator and the acquisition API on one node. With nothing
    // providing torrents the node answers from the cluster view and takes adds
    // that wait for a capable node. With a torrent service published it claims
    // requests, drives the job from the operator's intent, removes a completed
    // one after its delay, takes over a lapsed claim, and lets a superseded one go.
    TestNode fixture("torrent-coordinator");
    fixture.prepare();
    fixture.start();
    const auto self = fixture.node().node_id();

    CatalogueHintQueue hints(fixture.config().state_path / "catalogue-hints");
    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    IngestManager ingest(fixture.node(), fixture.filesystem(), hints, ingest_config);
    TorrentConfig torrent_config;
    torrent_config.enabled = true; // configured on, but nothing provides it yet.
    TorrentSearchManager search(torrent_config);
    SubsystemRegistry registry;
    ClusterJobView cluster_jobs(fixture.node(), ingest, registry);
    TorrentCoordinator coordinator(fixture.node(), fixture.metadata(), registry, cluster_jobs,
                                   fixture.config().state_path);
    AcquisitionApi acquisition(ingest, registry, search, cluster_jobs, coordinator);
    const auto call = [&](std::string method, std::string path, std::string body = {}) {
        HttpRequest request;
        request.method = std::move(method);
        request.path = std::move(path);
        request.body = Bytes(body.begin(), body.end());
        return acquisition.handle(request);
    };

    // No plugin: honest answers rather than assuming the engine is there.
    {
        const auto status = call("GET", "/api/v1/torrents/status");
        REQUIRE(status.status == 200);
        CHECK(api_body(status).find("build_available")->asBool() == false);
        CHECK(api_body(status).find("enabled")->asBool() == false);
        const auto listed = call("GET", "/api/v1/torrents/jobs");
        REQUIRE(listed.status == 200);
        CHECK(api_body(listed).find("jobs")->asArray().empty());
        CHECK(api_body(listed).find("refresh_interval_ms")->asUInt64() == 5000);
        CHECK(api_body(listed).find("sources")->asArray().empty()); // no torrents here, no peers
        const auto nodes = call("GET", "/api/v1/torrents/nodes");
        REQUIRE(nodes.status == 200);
        CHECK(api_body(nodes).find("nodes")->asArray().empty()); // not torrent-capable
        CHECK(api_body(nodes).find("default_remove_after_ms")->isNull());

        // Torrents belong to the cluster: a node that cannot run one still
        // takes the add, and it waits for a node that can.
        const auto added = call("POST", "/api/v1/torrents/jobs",
                                R"({"magnet":"magnet:?xt=urn:btih:1234567890123456789012345678901234567890&dn=Waiting"})");
        REQUIRE(added.status == 202);
        const auto body = api_body(added);
        CHECK(body.find("node_id")->isNull());
        CHECK(body.find("info_hash")->asString() == "1234567890123456789012345678901234567890");
        const auto& job = *body.find("job");
        CHECK(job.find("phase")->asString() == "awaiting_node");
        CHECK(job.find("state")->asString() == "awaiting_node");
        CHECK(job.find("name")->asString() == "Waiting");
        CHECK(job.find("node_id")->isNull());
        CHECK(job.find("desired_blocked_reason")->asString() == "no_capable_node");
        CHECK(job.find("bytes_total")->isNull()); // no live view of an unclaimed job
        const auto id = body.find("id")->asString();

        // One request holds a torrent: a second add is refused naming it.
        const auto again = call("POST", "/api/v1/torrents/jobs",
                                R"({"magnet":"magnet:?xt=urn:btih:1234567890123456789012345678901234567890"})");
        CHECK(again.status == 409);
        const auto refusal = api_body(again);
        CHECK(refusal.find("status")->asString() == "torrent_already_added");
        CHECK(refusal.find("error")->find("code")->asString() == "torrent_already_added");
        CHECK(!refusal.find("error")->find("message")->asString().empty());
        CHECK(refusal.find("id")->asString() == id);
        CHECK(refusal.find("node_id")->isNull());

        CHECK(api_body(call("GET", "/api/v1/torrents/jobs")).find("jobs")->asArray().size() == 1);
        const auto paused = call("POST", "/api/v1/torrents/jobs/" + id + "/pause");
        CHECK(paused.status == 202);
        CHECK(api_body(paused).find("desired")->asString() == "paused");
        const auto patched = call("PATCH", "/api/v1/torrents/jobs/" + id, R"({"remove_after_ms":3600000})");
        CHECK(patched.status == 200);
        CHECK(api_body(patched).find("remove_after_ms")->asUInt64() == 3600000);
        CHECK(call("PATCH", "/api/v1/torrents/jobs/" + id, R"({"remove_after_ms":86400001})").status == 400);
        CHECK(call("POST", "/api/v1/torrents/jobs/" + id + "/clear").status == 202);
        CHECK(api_body(call("GET", "/api/v1/torrents/jobs")).find("jobs")->asArray().empty());

        // Added already paused, in the same write: no pause to race.
        const auto held = call("POST", "/api/v1/torrents/jobs",
                               R"({"magnet":"magnet:?xt=urn:btih:abcdefabcdefabcdefabcdefabcdefabcdefabcd&dn=Held","paused":true})");
        REQUIRE(held.status == 202);
        CHECK(api_body(held).find("job")->find("desired")->asString() == "paused");
        const auto held_id = api_body(held).find("id")->asString();
        const auto resumed = call("POST", "/api/v1/torrents/jobs/" + held_id + "/resume");
        CHECK(resumed.status == 202);
        CHECK(api_body(resumed).find("desired")->asString() == "active");
        CHECK(call("POST", "/api/v1/torrents/jobs",
                   R"({"magnet":"magnet:?xt=urn:btih:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","paused":"yes"})").status == 400);
        CHECK(call("POST", "/api/v1/torrents/jobs/" + held_id + "/clear").status == 202);

        // A pin to a node that cannot run torrents is refused.
        const auto pinned = call("POST", "/api/v1/torrents/jobs",
                                 R"({"magnet":"magnet:?xt=urn:btih:2234567890123456789012345678901234567890","node_id":")" +
                                     to_string(self) + R"("})");
        CHECK(pinned.status == 409);
        CHECK(api_body(pinned).find("error")->find("reason")->asString() == "node_not_torrent_capable");
        CHECK(call("GET", "/api/v1/torrents/jobs/whatever").status == 404);
        CHECK(call("POST", "/api/v1/torrents/jobs/whatever/pause").status == 404);
        const auto ingests = call("GET", "/api/v1/ingest/jobs");
        REQUIRE(ingests.status == 200);
        const auto sources = api_body(ingests).find("sources")->asArray();
        REQUIRE(sources.size() == 1);
        CHECK(sources.front().find("local")->asBool());
        CHECK(sources.front().find("reachable")->asBool());
        // Search is core's own Torznab client, so it stays available.
        CHECK(call("GET", "/api/v1/torrents/search").status == 400); // missing q, not 503.
    }

    // A torrent service is published: this node is now the one capable node.
    auto torrents = std::make_shared<TableTorrentService>();
    registry.publish_torrent(torrents);

    // An add is a request in metadata; the capable node claims it, runs it
    // under the request's id, applies the operator's intent to it, and lets it
    // go when it is cleared.
    {
        const auto added = coordinator.add("magnet:?xt=urn:btih:8888888888888888888888888888888888888888&dn=Owned",
                                           false, std::nullopt, std::nullopt);
        REQUIRE(added.status == 202);
        const auto id = added.request->id;
        CHECK(added.request->phase == TorrentPhase::awaiting_node);
        CHECK(!added.request->remove_after_ms); // the cluster default is off

        coordinator.pass_now();
        auto r = coordinator.request(id);
        REQUIRE(r.has_value());
        REQUIRE(r->claim.has_value());
        CHECK(r->claim->node_id == self);
        CHECK(r->claim->epoch == 1);
        CHECK(r->phase == TorrentPhase::downloading);
        REQUIRE(torrents->job(id).has_value()); // run under the request's id

        CHECK(coordinator.act(id, "pause").status == 202);
        coordinator.pass_now();
        CHECK(torrents->job(id)->state == TorrentJobState::paused);
        CHECK(coordinator.desired_applied(*coordinator.request(id), torrents->job(id)));
        CHECK(coordinator.act(id, "resume").status == 202);
        coordinator.pass_now();
        CHECK(torrents->job(id)->state != TorrentJobState::paused);

        CHECK(coordinator.act(id, "clear").status == 409); // still running
        CHECK(coordinator.act(id, "cancel").status == 202);
        coordinator.pass_now(); // stops it locally
        coordinator.pass_now(); // and records it
        CHECK(coordinator.request(id)->phase == TorrentPhase::cancelled);
        CHECK(coordinator.act(id, "clear").status == 202);
        coordinator.pass_now();
        CHECK(!coordinator.request(id).has_value());
        CHECK(!torrents->job(id).has_value());
    }

    // A local job no request holds becomes a request this node has claimed.
    // Removal after completion is off unless asked; with a delay of 0 the
    // owner removes the job and the request at its next pass.
    {
        TorrentJob done;
        done.id = "done-job";
        done.name = "Finished";
        done.info_hash = std::string(40, '9');
        done.source_uri = "magnet:?xt=urn:btih:" + done.info_hash;
        done.state = TorrentJobState::completed;
        done.created_unix_ms = 1;
        done.updated_unix_ms = 2;
        torrents->put(done);
        coordinator.pass_now();
        auto r = coordinator.request("done-job");
        REQUIRE(r.has_value());
        CHECK(r->phase == TorrentPhase::completed);
        CHECK(r->claim->node_id == self);
        coordinator.pass_now();
        CHECK(coordinator.request("done-job").has_value()); // no delay set: kept
        REQUIRE(coordinator.patch("done-job", std::optional<uint64_t>{0}, std::nullopt).status == 200);
        coordinator.pass_now();
        CHECK(!coordinator.request("done-job").has_value());
        CHECK(!torrents->job("done-job").has_value());
    }

    // A claim holds while its node is a member; past the lease another node
    // takes it at the next epoch. A node that finds its own claim superseded
    // stops and deletes its copy.
    {
        TorrentCoordinator impatient(fixture.node(), fixture.metadata(), registry, cluster_jobs,
                                     fixture.config().state_path, std::chrono::milliseconds(0));
        const auto gone = random_node_id(); // never a member
        TorrentRequest lapsed;
        lapsed.id = std::string(32, 'e');
        lapsed.info_hash = std::string(40, 'a');
        lapsed.source = "magnet:?xt=urn:btih:" + lapsed.info_hash;
        lapsed.created_unix_ms = 1;
        lapsed.claim = TorrentClaim{gone, 1, 1};
        lapsed.phase = TorrentPhase::downloading;
        lapsed.phase_epoch = 1;
        fixture.metadata().mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            snapshot.torrent_requests[lapsed.id] = lapsed;
            delta.upsert_torrent_requests[lapsed.id] = lapsed;
        });

        // The default lease is ten minutes: an absent owner keeps its claim.
        coordinator.pass_now();
        coordinator.pass_now();
        CHECK(coordinator.request(lapsed.id)->claim->node_id == gone);
        CHECK(!torrents->job(lapsed.id).has_value());

        impatient.pass_now(); // notes the owner absent
        impatient.pass_now(); // lease (0 ms) elapsed: taken over
        auto r = impatient.request(lapsed.id);
        REQUIRE(r.has_value());
        CHECK(r->claim->node_id == self);
        CHECK(r->claim->epoch == 2);
        CHECK(r->phase_epoch == 2);
        REQUIRE(torrents->job(lapsed.id).has_value());

        // A pin holds while the node it names is known, and no longer once
        // that node is forgotten: then any node may take the request.
        NodeInfo known_peer;
        known_peer.id = random_node_id();
        known_peer.host = "127.0.0.9";
        known_peer.port = 57499;
        known_peer.seen_unix_ms = unix_ms();
        fixture.node().membership().observe(known_peer, true);
        const auto pinned_to = [&](char tag, const NodeId& node) {
            TorrentRequest pinned;
            pinned.id = std::string(32, tag);
            pinned.info_hash = std::string(40, tag);
            pinned.source = "magnet:?xt=urn:btih:" + pinned.info_hash;
            pinned.created_unix_ms = 2;
            pinned.phase = TorrentPhase::awaiting_node;
            pinned.pinned_node_id = node;
            fixture.metadata().mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
                snapshot.torrent_requests[pinned.id] = pinned;
                delta.upsert_torrent_requests[pinned.id] = pinned;
            });
            return pinned.id;
        };
        const auto held = pinned_to('b', known_peer.id);
        const auto freed = pinned_to('c', gone);
        impatient.pass_now();
        impatient.pass_now();
        CHECK(!impatient.request(held)->claim.has_value());
        REQUIRE(impatient.request(freed)->claim.has_value());
        CHECK(impatient.request(freed)->claim->node_id == self);

        // Another node's newer claim wins; this node lets its copy go.
        auto superseding = *r;
        superseding.claim = TorrentClaim{gone, 3, unix_ms()};
        superseding.phase_epoch = 3;
        superseding.progress_unix_ms = unix_ms() + 1;
        fixture.metadata().mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
            snapshot.torrent_requests[lapsed.id] = superseding;
            delta.upsert_torrent_requests[lapsed.id] = superseding;
        });
        coordinator.pass_now(); // no takeover, only the let-go
        CHECK(!torrents->job(lapsed.id).has_value());
        CHECK(coordinator.request(lapsed.id)->claim->node_id == gone);
    }
    registry.withdraw_torrent(torrents.get());
}

// Only meaningful in a build that produced the plugin: without libtorrent
// there is no download engine to load, and the capability is absent by
// design rather than differently compiled.
#ifdef MACHA_TEST_TORRENT_PLUGIN
MACHA_FAST_TEST("hydration_catalogue", test_a_torrent_job_is_saved_only_when_its_record_changes) {
    // The manager's tick compares each job before and after: nothing changed
    // is no write; moving transfer counters alone are saved on an interval;
    // anything else in the persisted record is saved at once.
    TorrentJob job;
    job.id = "job";
    job.name = "Film";
    job.state = TorrentJobState::downloading;
    job.bytes_total = 1000;
    job.bytes_completed = 100;
    job.updated_unix_ms = 1;

    auto same = job;
    same.updated_unix_ms = 2;       // the timestamp is not the record
    same.download_rate = 5000;      // nor are the live counters
    same.peers = 12;
    same.eta_seconds = 60;
    CHECK(torrent_job_change(job, same) == TorrentJobChange::none);

    auto progressed = job;
    progressed.bytes_completed = 200;
    CHECK(torrent_job_change(job, progressed) == TorrentJobChange::progress);
    auto uploaded = job;
    uploaded.uploaded_total = 10;
    CHECK(torrent_job_change(job, uploaded) == TorrentJobChange::progress);

    auto state = progressed;
    state.state = TorrentJobState::downloaded;
    CHECK(torrent_job_change(job, state) == TorrentJobChange::record);
    auto failed = job;
    failed.error_code = "torrent_error";
    CHECK(torrent_job_change(job, failed) == TorrentJobChange::record);
    auto catalogued = job;
    catalogued.catalogue_catalogued = 1;
    CHECK(torrent_job_change(job, catalogued) == TorrentJobChange::record);
}

namespace {
uint64_t torrent_thread_faults() {
    for (const auto& status : supervised_thread_statuses())
        if (status.name == "torrent") return status.faults;
    return 0;
}

size_t torrent_thread_running() {
    for (const auto& status : supervised_thread_statuses())
        if (status.name == "torrent") return status.running;
    return 0;
}

Json::Object recorded_torrent(const std::filesystem::path& staging, const std::string& id,
                              const std::string& hash, const std::string& state, uint64_t created = 1) {
    const auto payload = staging / "torrents" / id;
    std::filesystem::create_directories(payload);
    Json::Object job;
    job["id"] = id;
    job["name"] = id;
    job["source_uri"] = "magnet:?xt=urn:btih:" + hash;
    job["info_hash"] = hash;
    job["save_path"] = payload.string();
    job["state"] = state;
    job["created_unix_ms"] = created;
    job["updated_unix_ms"] = created + 1;
    job["error"] = "";
    return job;
}

// A failed torrent job with the failed ingest it handed its payload to.
void record_failed_import(Json::Array& torrents, Json::Array& ingests, const std::filesystem::path& staging,
                          const std::string& torrent_id, const std::string& ingest_id,
                          const std::string& hash) {
    auto torrent = recorded_torrent(staging, torrent_id, hash, "failed");
    torrent["bytes_total"] = static_cast<uint64_t>(1234);
    torrent["bytes_completed"] = static_cast<uint64_t>(1234);
    torrent["ingest_job_id"] = ingest_id;
    torrent["error"] = "ingest failed: metadata quorum unavailable";
    torrents.emplace_back(std::move(torrent));

    Json::Object ingest;
    ingest["id"] = ingest_id;
    ingest["source_type"] = "torrent";
    ingest["source_ref"] = torrent_id;
    ingest["display_name"] = torrent_id;
    ingest["source_path"] = (staging / "torrents" / torrent_id).string();
    ingest["source_owned"] = true;
    ingest["delete_source_on_clear"] = true;
    ingest["state"] = "failed";
    ingest["bytes_total"] = static_cast<uint64_t>(1234);
    ingest["bytes_completed"] = static_cast<uint64_t>(1234);
    ingest["files_total"] = static_cast<uint64_t>(1);
    ingest["files_completed"] = static_cast<uint64_t>(1);
    ingest["created_unix_ms"] = static_cast<uint64_t>(1);
    ingest["updated_unix_ms"] = static_cast<uint64_t>(2);
    ingest["error"] = "metadata quorum unavailable";
    ingest["files"] = Json::Array{};
    ingests.emplace_back(std::move(ingest));
}

void write_jobs_file(const std::filesystem::path& path, uint64_t version, Json::Array jobs) {
    std::filesystem::create_directories(path.parent_path());
    Json::Object root;
    root["version"] = version;
    root["jobs"] = std::move(jobs);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << Json(std::move(root)).dump();
    REQUIRE(out.good());
}
} // namespace

// Integrated: the real libmacha-torrent module, loaded as SubsystemSupervisor
// loads it, restoring jobs.json into a libtorrent session.
MACHA_TEST("hydration_catalogue", test_the_torrent_plugin_restores_its_jobs_and_runs_them_through_the_engine) {
    const std::string duplicated(40, '5');
    TestNode fixture("torrent-plugin");
    fixture.prepare();
    const auto state_path = fixture.config().state_path;
    const auto staging_path = fixture.path() / "staging";
    {
        Json::Array torrents;
        Json::Array ingests;
        // Saved without an info_hash: backfilled from its magnet on load.
        auto done = recorded_torrent(staging_path, "torrent-done", std::string(40, '3'), "completed");
        done.erase("info_hash");
        torrents.emplace_back(std::move(done));
        record_failed_import(torrents, ingests, staging_path, "torrent-retry", "ingest-retry", std::string(40, '1'));
        record_failed_import(torrents, ingests, staging_path, "torrent-follow", "ingest-follow", std::string(40, 'f'));
        torrents.emplace_back(recorded_torrent(staging_path, "torrent-pause", std::string(40, '2'), "queued"));
        // Two jobs recorded for one torrent, the later one listed first.
        torrents.emplace_back(recorded_torrent(staging_path, "dup-second", duplicated, "queued", 20));
        torrents.emplace_back(recorded_torrent(staging_path, "dup-first", duplicated, "queued", 10));
        write_jobs_file(state_path / "torrent" / "jobs.json", 1, std::move(torrents));
        write_jobs_file(state_path / "ingest" / "jobs.json", 2, std::move(ingests));
    }
    fixture.start();
    const auto self = fixture.node().node_id();

    CatalogueHintQueue hints(state_path / "catalogue-hints");
    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = staging_path;
    IngestManager ingest(fixture.node(), fixture.filesystem(), hints, ingest_config);
    TorrentConfig torrent_config;
    torrent_config.enabled = true;
    torrent_config.dht = false;
    torrent_config.pex = false;
    torrent_config.lsd = false;
    // The Subsystem is driven directly, not through a SubsystemSupervisor,
    // which would start the polling worker at once: the first steps act on
    // state restored from jobs.json before it runs.
    Config plugin_config = fixture.node().config();
    plugin_config.torrent = torrent_config;
    plugin_config.state_path = state_path;
    SubsystemRegistry registry;
    SubsystemContext context;
    context.config = &plugin_config;
    context.node = &fixture.node();
    context.data_resources = &fixture.node().resources.data;
    context.retained_memory = &fixture.node().resources.memory;
    context.routes = &fixture.node().routes;
    context.local_state = &fixture.node().local_state();
    context.ingest = &ingest;
    context.registry = &registry;
    // The manager's periodic passes run on stepped time.
    SteppedTime time;
    context.time = &time;
    LoadedTorrentPlugin plugin(context);
    struct StopPlugin {
        LoadedTorrentPlugin& plugin;
        ~StopPlugin() { plugin.subsystem().stop(); }
    } stop_plugin{plugin};
    const auto torrents_owner = registry.torrent();
    REQUIRE(torrents_owner);
    auto& torrents = *torrents_owner;
    TorrentSearchManager search(torrent_config);
    ClusterJobView cluster_jobs(fixture.node(), ingest, registry);
    TorrentCoordinator coordinator(fixture.node(), fixture.metadata(), registry, cluster_jobs, state_path);
    AcquisitionApi acquisition(ingest, registry, search, cluster_jobs, coordinator);
    const auto post = [&](const std::string& path) {
        HttpRequest request;
        request.method = "POST";
        request.path = path;
        return acquisition.handle(request);
    };

    REQUIRE(torrents.job("torrent-done").has_value());
    CHECK(torrents.job("torrent-done")->info_hash == std::string(40, '3'));
    REQUIRE(ingest.job("ingest-retry").has_value());
    CHECK(ingest.job("ingest-retry")->state == IngestJobState::failed);

    // An explicit pause is the operator's intent, recorded before the worker runs.
    REQUIRE(torrents.pause("torrent-pause"));
    CHECK(torrents.job("torrent-pause")->state == TorrentJobState::paused);

    // The restored jobs become cluster requests this node has already
    // claimed, on the coordinator's first pass. Retrying the failed one
    // through the API requeues its ingest and the torrent mirrors it.
    coordinator.pass_now();
    REQUIRE(coordinator.request("torrent-retry").has_value());
    CHECK(coordinator.request("torrent-retry")->phase == TorrentPhase::failed);
    CHECK(coordinator.request("torrent-retry")->claim->node_id == self);
    const auto retried = post("/api/v1/torrents/jobs/torrent-retry/retry");
    REQUIRE(retried.status == 202);
    CHECK(api_body(retried).find("state")->asString() == "importing");
    CHECK(!torrents.retry("torrent-retry"));
    const auto retried_ingest = ingest.job("ingest-retry");
    const auto retried_torrent = torrents.job("torrent-retry");
    REQUIRE(retried_ingest.has_value());
    REQUIRE(retried_torrent.has_value());
    CHECK(retried_ingest->state == IngestJobState::queued);
    CHECK(retried_ingest->error.empty());
    CHECK(retried_torrent->state == TorrentJobState::importing);
    CHECK(retried_torrent->error.empty());
    REQUIRE(retried_torrent->ingest_job_id.has_value());
    CHECK(*retried_torrent->ingest_job_id == "ingest-retry");

    const auto faults_before = torrent_thread_faults();
    plugin.subsystem().start();

    // Two jobs recorded for one torrent restore as one: the later fails with
    // duplicate_torrent and holds nothing.
    const auto second = torrents.job("dup-second");
    REQUIRE(second.has_value());
    CHECK(second->state == TorrentJobState::failed);
    CHECK(second->error_code == "duplicate_torrent");
    CHECK(second->error.find("dup-first") != std::string::npos);
    REQUIRE(torrents.job("dup-first").has_value());
    CHECK(torrents.job("dup-first")->state != TorrentJobState::failed);

    // A request claimed here runs in the engine under the request's id and
    // follows the operator's intent.
    {
        const auto added = coordinator.add("magnet:?xt=urn:btih:8888888888888888888888888888888888888888&dn=Owned",
                                           false, std::nullopt, std::nullopt);
        REQUIRE(added.status == 202);
        const auto id = added.request->id;
        coordinator.pass_now();
        REQUIRE(torrents.job(id).has_value());
        CHECK(coordinator.request(id)->phase == TorrentPhase::downloading);
        CHECK(coordinator.act(id, "pause").status == 202);
        coordinator.pass_now();
        CHECK(torrents.job(id)->state == TorrentJobState::paused);
        CHECK(coordinator.desired_applied(*coordinator.request(id), torrents.job(id)));
        CHECK(coordinator.act(id, "resume").status == 202);
        coordinator.pass_now();
        CHECK(torrents.job(id)->state != TorrentJobState::paused);
        CHECK(coordinator.act(id, "cancel").status == 202);
        coordinator.pass_now();
        coordinator.pass_now();
        CHECK(coordinator.request(id)->phase == TorrentPhase::cancelled);
        CHECK(coordinator.act(id, "clear").status == 202);
        coordinator.pass_now();
        CHECK(!torrents.job(id).has_value());
    }

    // A search result may be a provider's .torrent URL; a plain placement
    // requires a magnet.
    const std::string url = "https://127.0.0.1:1/result.torrent";
    const auto as_magnet = torrents.place(url, false);
    CHECK(!as_magnet.placed);
    CHECK(as_magnet.error.find("requires a magnet") != std::string::npos);
    const auto as_result = torrents.place(url, true);
    CHECK(!as_result.placed);
    CHECK(as_result.error.find("requires a magnet") == std::string::npos);

    // libtorrent keys a torrent by its info hash, so one job holds it: a second
    // add is refused with the holder's id, whatever the holder's state, until
    // it is cleared and nothing of it is left in the session.
    {
        const std::string magnet = "magnet:?xt=urn:btih:4444444444444444444444444444444444444444&dn=Twice";
        const auto first = torrents.place(magnet, false);
        REQUIRE(first.placed);
        const auto again = torrents.place(magnet, false);
        CHECK(!again.placed);
        CHECK(again.reason == "torrent_already_added");
        CHECK(again.job_id == first.job_id);
        CHECK(again.node_id == self);
        size_t holding = 0;
        for (const auto& job : torrents.jobs())
            if (job.info_hash == "4444444444444444444444444444444444444444") ++holding;
        CHECK(holding == 1);
        REQUIRE(torrents.cancel(first.job_id));
        CHECK(torrents.place(magnet, false).reason == "torrent_already_added");
        REQUIRE(torrents.clear(first.job_id));
        const auto replaced = torrents.place(magnet, false);
        CHECK(replaced.placed);
        CHECK(replaced.job_id != first.job_id);
    }

    // Cancelling the job that holds the duplicated torrent must not fault the
    // worker's next held-pieces pass, ten seconds on: step past it and let two
    // more worker iterations begin, so one whole pass has run since the cancel.
    REQUIRE(torrents.cancel("dup-first"));
    const auto reads = time.reads();
    time.advance(11s);
    REQUIRE(wait_until([&] { return time.reads() >= reads + 4; }, 10s));
    CHECK(torrent_thread_faults() == faults_before);
    for (const auto& job : torrents.jobs()) CHECK(job.error_code != "torrent_fault");
    // The worker is still inside its loop, and a new job is still taken.
    CHECK(torrent_thread_running() == 1);
    const auto fresh = torrents.place("magnet:?xt=urn:btih:6666666666666666666666666666666666666666&dn=After", false);
    REQUIRE(fresh.placed);
    CHECK(torrents.job(fresh.job_id).has_value());
    // The worker has sampled the restored handles by now: the explicit pause
    // stays authoritative whatever libtorrent reports, and a failed torrent
    // whose ingest failed too is not brought back by itself.
    CHECK(torrents.job("torrent-pause")->state == TorrentJobState::paused);
    CHECK(torrents.job("torrent-follow")->state == TorrentJobState::failed);

    // Resumed through the ingest's own route, the ingest is queued (its workers
    // are not running here) and the torrent follows it back, failure cleared.
    REQUIRE(post("/api/v1/ingest/jobs/ingest-follow/resume").status == 200);
    REQUIRE(wait_until([&] {
        return torrents.job("torrent-follow")->state == TorrentJobState::importing;
    }, 5s));
    CHECK(torrents.job("torrent-follow")->error.empty());
    CHECK(torrents.job("torrent-follow")->error_code.empty());
}
#endif // MACHA_TEST_TORRENT_PLUGIN

namespace {

// Builds `count` single-file import roots under `base` and returns them; file
// `i` is "Concurrent Movie <i> 2024.mkv".
std::vector<std::filesystem::path> make_import_roots(const std::filesystem::path& base,
                                                     size_t count, size_t file_bytes) {
    std::vector<std::filesystem::path> roots;
    for (size_t i = 0; i < count; ++i) {
        auto root = base / ("concurrent-import-" + std::to_string(i));
        (void)write_host_file(root / ("Concurrent Movie " + std::to_string(i) + " 2024.mkv"),
                              pattern(file_bytes));
        roots.push_back(root);
    }
    return roots;
}

// True once every listed job has copied all of its planned files. Deliberately
// checks copy progress rather than the terminal state, so the assertion does
// not depend on how the catalogue is configured for the fixture.
bool all_imports_copied(const IngestManager& ingest, const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        auto job = ingest.job(id);
        if (!job || job->files_total == 0 || job->files_completed != job->files_total)
            return false;
    }
    return true;
}

// A source root holding one file whose bytes are `bytes`, and an extent
// journal beside it recording the extents a torrent's disk backend would
// have published. When `store_them` is false the journal names objects that
// were never stored.
struct PublishedSource {
    std::filesystem::path root;
    std::string name;
    std::vector<ObjectId> ids;
};

PublishedSource make_published_source(const std::filesystem::path& root, uint64_t extent_size, FileSystem& fs,
                                      const Bytes& bytes, bool store_them) {
    PublishedSource out;
    out.root = root;
    out.name = "Published Movie 2024.mkv";
    (void)write_host_file(out.root / out.name, bytes);
    TorrentExtentJournal journal(out.root);
    for (uint64_t offset = 0; offset < bytes.size(); offset += extent_size) {
        const auto length = std::min<uint64_t>(extent_size, bytes.size() - offset);
        const std::span<const uint8_t> slice(bytes.data() + offset, static_cast<size_t>(length));
        ObjectId id;
        if (store_them) {
            id = fs.store().put(slice, FrameType::loader);
        } else {
            // A different object of the same length, never stored.
            Bytes other(slice.begin(), slice.end());
            other[0] ^= 0xff;
            id = object_id(other);
        }
        journal.append(out.name, bytes.size(), {offset, length, id});
        out.ids.push_back(id);
    }
    return out;
}

Bytes read_whole(FileSystem& fs, const std::string& path, uint64_t size) {
    Bytes out(static_cast<size_t>(size));
    auto reader = fs.open_read(path);
    size_t done = 0;
    while (done < out.size()) {
        const auto got = reader->read(done, std::span<uint8_t>(out.data() + done, out.size() - done));
        if (!got) break;
        done += got;
    }
    out.resize(done);
    return out;
}

bool absent(FileSystem& fs, const std::string& path) {
    try {
        (void)fs.getattr(path);
        return false;
    } catch (const FsError& error) {
        return error.code() == ENOENT;
    }
}

// Holds chosen metadata commits before they are published, on the committing
// thread: a commit naming a path that contains a held name blocks until
// release_all(), after `skip` earlier such commits have passed. With it a test
// stops an ingest worker at a known point in a copy.
class CommitGate {
    struct Hold {
        std::string name;
        size_t skip{};
        // Only a path ending in the name: a file's final name, not its partial.
        bool ending{};
    };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Hold> holds_;
    size_t held_{};

  public:
    void hold(std::string name, size_t skip = 0) {
        std::lock_guard lock(mutex_);
        holds_.push_back({std::move(name), skip});
    }

    void hold_ending(std::string name) {
        std::lock_guard lock(mutex_);
        holds_.push_back({std::move(name), 0, true});
    }

    void release_all() {
        std::lock_guard lock(mutex_);
        holds_.clear();
        cv_.notify_all();
    }

    size_t held() {
        std::lock_guard lock(mutex_);
        return held_;
    }

    void operator()(const MetadataPublicationContext& context) {
        if (!context.delta) return;
        std::vector<std::string_view> paths;
        for (const auto& [path, _] : context.delta->upsert_entries) paths.push_back(path);
        for (const auto& [path, _] : context.delta->append_entries) paths.push_back(path);
        std::unique_lock lock(mutex_);
        bool block = false;
        for (auto& hold : holds_) {
            const bool named = std::any_of(paths.begin(), paths.end(), [&](std::string_view path) {
                return hold.ending ? path.ends_with(hold.name)
                                   : path.find(hold.name) != std::string_view::npos;
            });
            if (!named) continue;
            if (hold.skip) --hold.skip;
            else block = true;
        }
        if (!block) return;
        ++held_;
        cv_.wait(lock, [&] { return holds_.empty(); });
        --held_;
    }
};

} // namespace

MACHA_TEST("hydration_catalogue", test_ingest_plans_destinations_and_imports_files) {
    // IngestManager against one node's filesystem and hint queue: catalogue
    // feedback completes a job and clearing it removes only what was imported;
    // two same-named files of one job get distinct destinations; a torrent's
    // published extents are committed without copying, exactly as journalled;
    // destinations fold case onto existing folders.
    CatalogueNode node("ingest");
    auto& fs = node.filesystem();
    node.mkdir("/Movies");
    node.mkdir("/Movies/The Martian (2015)");
    fs.create_file("/Movies/The Martian (2015)/The.Martian.2015.EXTENDED.1080p.mkv", 0644, getuid(), getgid());

    const auto external = node.path() / "external-import";
    const auto media = write_host_file(external / "Queue Test Movie 2024.mkv", pattern(512 * 1024));
    const auto unrelated = external / "do-not-delete.txt";
    {
        std::ofstream out(unrelated);
        out << "external source material not selected for ingest\n";
    }
    const auto rome = node.path() / "rome";
    const auto first_menu = pattern(64 * 1024 + 11, 3);
    const auto second_menu = pattern(96 * 1024 + 7, 4);
    (void)write_host_file(rome / "Season 1/Extras" / "Menu Art.mkv", first_menu);
    (void)write_host_file(rome / "Season 2/Extras" / "Menu Art.mkv", second_menu);
    const auto published_bytes = pattern(2 * node.config().extent_size + 1000, 5);
    const auto published = make_published_source(node.path() / "published-import", node.config().extent_size, fs,
                                                 published_bytes, true);
    const auto martian = node.path() / "martian";
    (void)write_host_file(martian / "the.martian.2015.extended.720p.bluray.x264-nezu.mkv", pattern(64 * 1024 + 5, 5));
    (void)write_host_file(martian / "the.martian.2015.extended.1080p.mkv", pattern(64 * 1024 + 6, 6));

    IngestConfig config;
    config.enabled = true;
    config.staging_path = node.path() / "staging";
    config.source_roots = {external, rome, published.root, martian};
    config.copy_chunk_bytes = 64 * 1024;
    config.checkpoint_bytes = 256 * 1024;
    config.delete_external_source_on_clear = true;
    IngestManager ingest(node.node(), fs, node.hints(), config);
    ingest.start();

    // Copied, the job waits on the catalogue; the hint's outcome completes it.
    {
        const auto id = ingest.submit_path(external);
        REQUIRE(wait_until([&] {
            auto job = ingest.job(id);
            return job && job->state == IngestJobState::cataloguing;
        }, 10s));
        const auto summary = node.hints().summary("ingest", id);
        REQUIRE(summary.total == 1);
        REQUIRE(summary.pending == 1);
        auto hint = node.hints().claim_next();
        REQUIRE(hint.has_value());
        CHECK(hint->origins.size() == 1);
        node.hints().mark_catalogued(hint->id, "movies", "macha:test-ingest-media", {"test:movie:queue"},
                                     "synthetic match");
        REQUIRE(wait_until([&] {
            auto job = ingest.job(id);
            return job && job->state == IngestJobState::completed;
        }, 5s));
        const auto completed = ingest.job(id);
        REQUIRE(completed.has_value());
        CHECK(completed->catalogue_total == 1);
        CHECK(completed->catalogue_pending == 0);
        CHECK(completed->catalogue_catalogued == 1);
        CHECK(completed->catalogue_no_match == 0);
        CHECK(completed->catalogue_failed == 0);

        // Clearing deletes the imported source file and nothing beside it.
        REQUIRE(ingest.clear(id));
        CHECK(!ingest.job(id).has_value());
        CHECK(!std::filesystem::exists(media));
        CHECK(std::filesystem::exists(unrelated));
        CHECK(std::filesystem::exists(external));
        CHECK(node.hints().summary("ingest", id).total == 0);
    }

    const auto rome_id = ingest.submit_path(rome);
    const auto published_id = ingest.submit_path(published.root);
    const auto martian_id = ingest.submit_path(martian);
    REQUIRE(wait_until([&] {
        return all_imports_copied(ingest, {rome_id, published_id, martian_id});
    }, 60s));

    {
        // Two same-named files, neither yet in the filesystem.
        const auto job = ingest.job(rome_id);
        REQUIRE(job.has_value());
        REQUIRE(job->files.size() == 2);
        CHECK(job->files[0].destination_path != job->files[1].destination_path);
        for (const auto& file : job->files) {
            const auto& bytes = file.size == first_menu.size() ? first_menu : second_menu;
            CHECK(read_whole(fs, file.destination_path, bytes.size()) == bytes);
        }
    }
    {
        // The committed manifest is exactly the journal's; the journal itself
        // is not media and is not imported.
        const auto job = ingest.job(published_id);
        REQUIRE(job.has_value());
        REQUIRE(job->files.size() == 1);
        const auto destination = job->files.front().destination_path;
        const auto entry = fs.getattr(destination);
        CHECK(entry.size == published_bytes.size());
        REQUIRE(entry.extents.size() == published.ids.size());
        for (size_t i = 0; i < published.ids.size(); ++i) CHECK(entry.extents[i].id == published.ids[i]);
        CHECK(read_whole(fs, destination, published_bytes.size()) == published_bytes);
    }
    {
        // An existing folder is reused as spelt, and a file whose name differs
        // from one already there only by case is a collision.
        const auto job = ingest.job(martian_id);
        REQUIRE(job.has_value());
        REQUIRE(job->files.size() == 2);
        for (const auto& file : job->files) {
            CHECK(file.destination_path.starts_with("/Movies/The Martian (2015)/"));
            if (file.source_path.ends_with("1080p.mkv"))
                CHECK(file.destination_path != "/Movies/The Martian (2015)/the.martian.2015.extended.1080p.mkv");
        }
        size_t martian_folders = 0;
        for (const auto& [name, entry] : fs.readdir("/Movies"))
            if (entry.type == EntryType::directory && name.find("artian") != std::string::npos) ++martian_folders;
        CHECK(martian_folders == 1);
    }
    ingest.stop();
}

MACHA_TEST("hydration_catalogue", test_a_pause_or_cancel_accepted_during_the_final_rename_stands) {
    // The worker is past its last control check, inside the commit that
    // renames its finished copy into place, when the job is paused or
    // cancelled: the answer given must hold once the worker comes out.
    for (const bool cancel : {true, false}) {
        CommitGate gate;
        CatalogueNode node(cancel ? "ingest-late-cancel" : "ingest-late-pause", {},
                           [&](const MetadataPublicationContext& context) { gate(context); });
        const auto roots = make_import_roots(node.path(), 1, 256 * 1024);
        gate.hold_ending("Concurrent Movie 0 2024.mkv");

        IngestConfig config;
        config.enabled = true;
        config.staging_path = node.path() / "staging";
        config.source_roots = roots;
        config.max_concurrent_jobs = 1;
        IngestManager ingest(node.node(), node.filesystem(), node.hints(), config);
        struct ReleaseGate {
            CommitGate& gate;
            ~ReleaseGate() { gate.release_all(); }
        } release_gate{gate};
        const auto id = ingest.submit_path(roots.front());
        ingest.start();
        REQUIRE(wait_until([&] { return gate.held() == 1; }, 30s));
        REQUIRE(ingest.job(id)->state == IngestJobState::importing);

        REQUIRE(cancel ? ingest.cancel(id) : ingest.pause(id));
        const auto asked = cancel ? IngestJobState::cancelled : IngestJobState::paused;
        CHECK(ingest.job(id)->state == asked);
        gate.release_all();
        REQUIRE(wait_until([&] { return ingest.active_jobs() == 0; }, 30s));

        const auto job = ingest.job(id);
        REQUIRE(job.has_value());
        CHECK(job->state == asked);
        // The copy that finished is on record under that state.
        CHECK(job->files_completed == 1);
        if (!cancel) {
            REQUIRE(ingest.resume(id));
            REQUIRE(wait_until(
                [&] {
                    const auto resumed = ingest.job(id);
                    return resumed && (resumed->state == IngestJobState::cataloguing ||
                                       resumed->state == IngestJobState::completed);
                },
                30s));
        }
    }
}

MACHA_TEST("hydration_catalogue", test_ingest_job_control_acts_on_workers_held_mid_copy) {
    // Six jobs, three workers, every copy held at its first checkpoint commit:
    // exactly the bound is claimed; pause, cancel and clear then act on jobs a
    // worker holds; and once released the paused job stays paused without
    // stalling the rest, the cancelled job's partial goes, the cleared job
    // never comes back, and resume completes the paused copy.
    CommitGate gate;
    CatalogueNode node("ingest-control", {}, [&](const MetadataPublicationContext& context) { gate(context); });
    constexpr size_t job_count = 6;
    constexpr size_t bound = 3;
    constexpr size_t file_bytes = 1024 * 1024;
    const auto roots = make_import_roots(node.path(), job_count, file_bytes);
    // The first commit naming a file creates its partial; the second is its
    // first checkpoint, with three more to come.
    for (size_t i = 0; i < job_count; ++i) gate.hold("Concurrent Movie " + std::to_string(i) + " 2024.mkv", 1);

    IngestConfig config;
    config.enabled = true;
    config.staging_path = node.path() / "staging";
    config.source_roots = roots;
    config.copy_chunk_bytes = 64 * 1024;
    config.checkpoint_bytes = 256 * 1024;
    config.max_concurrent_jobs = bound;
    IngestManager ingest(node.node(), node.filesystem(), node.hints(), config);
    // Declared after the manager: a failed REQUIRE releases the held workers
    // before the manager joins them.
    struct ReleaseGate {
        CommitGate& gate;
        ~ReleaseGate() { gate.release_all(); }
    } release_gate{gate};
    // Submitted before start(), so every worker wakes to claimable work.
    std::vector<std::string> ids;
    for (const auto& root : roots) ids.push_back(ingest.submit_path(root));
    CHECK(ingest.active_jobs() == 0);
    ingest.start();

    const auto importing = [&] {
        std::vector<IngestJob> out;
        for (const auto& id : ids)
            if (auto job = ingest.job(id); job && job->state == IngestJobState::importing) out.push_back(*job);
        return out;
    };
    REQUIRE(wait_until([&] { return importing().size() >= bound && gate.held() > 0; }, 30s));
    CHECK(importing().size() == bound);
    // The bound is the contract: never more claimed at once than configured.
    CHECK(ingest.active_jobs() == bound);
    CHECK(ingest.peak_active_jobs() == bound);
    const auto claimed = importing();
    const auto& paused = claimed[0];
    const auto& cancelled = claimed[1];
    const auto& cleared = claimed[2];
    std::vector<std::string> untouched;
    for (const auto& id : ids)
        if (id != paused.id && id != cancelled.id && id != cleared.id) untouched.push_back(id);
    REQUIRE(untouched.size() == job_count - bound);
    for (const auto& id : untouched) CHECK(ingest.job(id)->state == IngestJobState::queued);

    // Each takes effect in the job's record at once, with its worker still inside the copy.
    REQUIRE(ingest.pause(paused.id));
    REQUIRE(ingest.cancel(cancelled.id));
    REQUIRE(ingest.cancel(cleared.id));
    REQUIRE(ingest.clear(cleared.id));
    CHECK(ingest.job(paused.id)->state == IngestJobState::paused);
    CHECK(ingest.job(cancelled.id)->state == IngestJobState::cancelled);
    CHECK(!ingest.job(cleared.id).has_value());

    gate.release_all();
    // The workers let go at their next control check and take the queued jobs.
    REQUIRE(wait_until([&] { return all_imports_copied(ingest, untouched); }, 60s));
    REQUIRE(wait_until([&] { return ingest.active_jobs() == 0; }, 10s));
    CHECK(ingest.peak_active_jobs() == bound);

    // No worker picked the paused job back up.
    const auto held = ingest.job(paused.id);
    REQUIRE(held.has_value());
    CHECK(held->state == IngestJobState::paused);
    CHECK(held->files_completed == 0);
    // A cancel stands, and its worker removed the partial.
    CHECK(ingest.job(cancelled.id)->state == IngestJobState::cancelled);
    CHECK(absent(node.filesystem(), cancelled.files.front().temporary_path));
    // A cleared job is never re-inserted by the worker that held it, which
    // does the cleanup clear() owed.
    CHECK(!ingest.job(cleared.id).has_value());
    CHECK(ingest.jobs().size() == job_count - 1);
    CHECK(absent(node.filesystem(), cleared.files.front().temporary_path));

    // Resume queues the paused job again and it completes its copy.
    REQUIRE(ingest.resume(paused.id));
    REQUIRE(wait_until([&] { return all_imports_copied(ingest, {paused.id}); }, 60s));
    CHECK(read_whole(node.filesystem(), ingest.job(paused.id)->files.front().destination_path, file_bytes) ==
          pattern(file_bytes));
    // clear() is the API's delete: terminal jobs go, and stay gone.
    REQUIRE(ingest.clear(cancelled.id));
    CHECK(!ingest.job(cancelled.id).has_value());

    ingest.stop();
    CHECK(ingest.active_jobs() == 0);
    CHECK(!ingest.job(cleared.id).has_value());
}

// Integrated: the refusal is Service's claims barrier on the metadata commit,
// and the ingest's fallback answers it.
MACHA_TEST("hydration_catalogue", test_ingest_copies_when_published_extents_are_missing) {
    // The journal is a claim, not proof. A manifest naming objects the store
    // does not hold must never be committed: the ingest falls back to copying,
    // and the file holds the real bytes.
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false;
    auto& service = fixture.start();

    const auto bytes = pattern(2 * fixture.config().extent_size + 1000);
    const auto source = make_published_source(fixture.path() / "unpublished-import", fixture.config().extent_size,
                                              service.filesystem(), bytes, false);
    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    ingest_config.source_roots = {source.root};
    IngestManager ingest(service.node(), service.filesystem(), service.catalogue_hints(), ingest_config);
    const auto id = ingest.submit_path(source.root);
    ingest.start();
    REQUIRE(wait_until([&] { return all_imports_copied(ingest, {id}); }, 60s));

    const auto job = ingest.job(id);
    REQUIRE(job.has_value());
    const auto destination = job->files.front().destination_path;
    const auto entry = service.filesystem().getattr(destination);
    CHECK(entry.size == bytes.size());
    for (const auto& extent : entry.extents)
        for (const auto& bogus : source.ids) CHECK(extent.id != bogus);
    CHECK(read_whole(service.filesystem(), destination, bytes.size()) == bytes);
    ingest.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_warm_read_defers_remote_refresh) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto c1 = config_for(cluster.path() / "catalogue-live-1", cluster.keyfile(), free_port());
    auto c2 = config_for(cluster.path() / "catalogue-live-2", cluster.keyfile(), free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.metadata_cache = c2.metadata_cache = 30ms;

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);

    // Form the initial namespace on the bootstrap-less founder before starting
    // the joiner. A configured joiner is intentionally forbidden from inventing
    // genesis while its bootstrap peer has not yet entered active membership.
    n1.start();
    REQUIRE(n1.wait_local_state_ready(10s));
    DistributedStore store1(n1, n1.local_state(), n1.resources.activity, n1.resources.data, n1.resources.memory, n1.resources.events);
    MetadataManager metadata1(n1, n1.local_state(), n1.metadata_server());
    CatalogueManager catalogue1(n1, n1.local_state(), n1.metadata_server(), store1, metadata1, n1.ledger());

    CatalogueItem first;
    first.id = "test:movie:remote-first";
    first.kind = CatalogueKind::movie;
    first.title = "Remote First";
    first = catalogue1.upsert(first);

    n2.start();
    REQUIRE(n2.wait_local_state_ready(10s));
    DistributedStore store2(n2, n2.local_state(), n2.resources.activity, n2.resources.data, n2.resources.memory, n2.resources.events);
    MetadataManager metadata2(n2, n2.local_state(), n2.metadata_server());
    CatalogueManager catalogue2(n2, n2.local_state(), n2.metadata_server(), store2, metadata2, n2.ledger());

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
    FileSystem fs2(n2.config(), n2.node_id(), n2.membership(), n2.local_state(), n2.metadata_server(), store2, metadata2, n2.resources.memory);
    FuseConfig fuse_config;
    fuse_config.commit_workers = 1;
    auto frontend = make_fuse_frontend(fs2, n2.resources.memory, fuse_config);
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
    metadata2.repair_once();
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

    CatalogueHintQueue catalogue2_hints(cluster.path() / "catalogue2-hints");
    CatalogueApi api(catalogue2, catalogue2_hints);
    auto status_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/status",
                                       .query = {},
                                       .headers = {},
                                       .body = {}, .session = {}});
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

MACHA_TEST("hydration_catalogue", test_metadata_decoded_cache_ttl_recovers_missed_notice) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto c1 = config_for(cluster.path() / "metadata-ttl-1", cluster.keyfile(), free_port());
    auto c2 = config_for(cluster.path() / "metadata-ttl-2", cluster.keyfile(), free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    // Keep ordinary heartbeat propagation outside this test window. We install a
    // valid newer committed metadata head directly to simulate a generation notice
    // that was missed by node two; TTL validation must still discover it from replicas.
    c1.heartbeat = c2.heartbeat = 5s;
    c1.dead_after = c2.dead_after = 20s;

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);

    // Establish genesis on the founder first. The joiner may legitimately reject
    // metadata reads with "waiting for bootstrap peer" during the brief interval
    // between start() and membership convergence, so retry its initial read rather
    // than turning that expected bootstrap state into an unhandled test failure.
    n1.start();
    REQUIRE(n1.wait_local_state_ready(10s));
    MetadataManager metadata1(n1, n1.local_state(), n1.metadata_server());
    const auto initial1 = metadata1.snapshot_view();
    n2.start();
    REQUIRE(n2.wait_local_state_ready(10s));
    // Node two's decoded cache lives by a clock the test steps.
    SteppedTime time;
    MetadataManager metadata2(n2, n2.local_state(), n2.metadata_server(), nullptr, {}, time);

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
    REQUIRE(n1.metadata_replica().store_commit(next));
    REQUIRE(n1.metadata_replica().accept_commit(
        {next.generation, next.hash, 1, {n1.node_id()}}));

    // With no generation notice, the decoded view is legitimately reused until
    // the configured metadata TTL expires.
    CHECK(n2.known_metadata_generation() < next.generation);
    CHECK(metadata2.snapshot_view().generation == initial2->generation);
    time.advance(c2.metadata_cache - 1ms);
    CHECK(metadata2.snapshot_view().generation == initial2->generation);
    time.advance(1ms);
    REQUIRE(n2.known_metadata_generation() < next.generation);

    // Expiry must force a real metadata read, discover the newer committed head
    // and replace the decoded snapshot, even with no generation notice.
    auto refreshed = metadata2.snapshot_view();
    CHECK(refreshed.generation == next.generation);
    CHECK(n2.known_metadata_generation() >= next.generation);

    n2.stop();
    n1.stop();
}

MACHA_FAST_TEST("hydration_catalogue", test_catalogue_effective_music_artwork_resolution) {
    auto art = [](std::string_view seed) {
        return CatalogueArtwork{"cover", object_id(Bytes(seed.begin(), seed.end())), "image/jpeg"};
    };

    const auto old_art = art("old");
    const auto newest_a_art = art("newest-a");
    const auto newest_b_art = art("newest-b");
    const auto unknown_art = art("unknown");
    const auto track_art = art("track");
    const auto compilation_art = art("compilation");
    const auto explicit_artist_art = art("artist");

    CatalogueSnapshot snapshot;

    CatalogueItem artist;
    artist.id = "artist:test";
    artist.kind = CatalogueKind::artist;
    artist.title = "Test Artist";
    snapshot.items.emplace(artist.id, artist);

    CatalogueItem various;
    various.id = "artist:various";
    various.kind = CatalogueKind::artist;
    various.title = "Various Artists";
    snapshot.items.emplace(various.id, various);

    CatalogueItem old_album;
    old_album.id = "album:old";
    old_album.kind = CatalogueKind::album;
    old_album.title = "Old";
    old_album.parent_id = artist.id;
    old_album.year = 2020;
    old_album.artwork = {old_art};
    snapshot.items.emplace(old_album.id, old_album);

    CatalogueItem newest_b;
    newest_b.id = "album:2024-b";
    newest_b.kind = CatalogueKind::album;
    newest_b.title = "Newest B";
    newest_b.parent_id = artist.id;
    newest_b.year = 2024;
    newest_b.artwork = {newest_b_art};
    snapshot.items.emplace(newest_b.id, newest_b);

    CatalogueItem newest_a;
    newest_a.id = "album:2024-a";
    newest_a.kind = CatalogueKind::album;
    newest_a.title = "Newest A";
    newest_a.parent_id = artist.id;
    newest_a.year = 2024;
    newest_a.artwork = {newest_a_art};
    snapshot.items.emplace(newest_a.id, newest_a);

    CatalogueItem newer_bare;
    newer_bare.id = "album:2025-bare";
    newer_bare.kind = CatalogueKind::album;
    newer_bare.title = "Newer But Bare";
    newer_bare.parent_id = artist.id;
    newer_bare.year = 2025;
    snapshot.items.emplace(newer_bare.id, newer_bare);

    CatalogueItem unknown_album;
    unknown_album.id = "album:000-unknown";
    unknown_album.kind = CatalogueKind::album;
    unknown_album.title = "Unknown Date";
    unknown_album.parent_id = artist.id;
    unknown_album.artwork = {unknown_art};
    snapshot.items.emplace(unknown_album.id, unknown_album);

    CatalogueItem track;
    track.id = "track:old:1";
    track.kind = CatalogueKind::track;
    track.title = "Inherited Track";
    track.parent_id = old_album.id;
    snapshot.items.emplace(track.id, track);

    CatalogueItem explicit_track = track;
    explicit_track.id = "track:old:2";
    explicit_track.title = "Explicit Track";
    explicit_track.artwork = {track_art};
    snapshot.items.emplace(explicit_track.id, explicit_track);

    CatalogueItem bare_track = track;
    bare_track.id = "track:bare:1";
    bare_track.title = "Bare Track";
    bare_track.parent_id = newer_bare.id;
    snapshot.items.emplace(bare_track.id, bare_track);

    CatalogueItem compilation;
    compilation.id = "album:compilation";
    compilation.kind = CatalogueKind::album;
    compilation.title = "Compilation";
    compilation.parent_id = various.id;
    compilation.year = 2026;
    compilation.artwork = {compilation_art};
    snapshot.items.emplace(compilation.id, compilation);

    CHECK(effective_catalogue_artwork(snapshot, artist) ==
          std::vector<CatalogueArtwork>{newest_a_art});
    CHECK(effective_catalogue_artwork(snapshot, track) == std::vector<CatalogueArtwork>{old_art});
    CHECK(effective_catalogue_artwork(snapshot, explicit_track) ==
          std::vector<CatalogueArtwork>{track_art});
    CHECK(effective_catalogue_artwork(snapshot, bare_track).empty());
    CHECK(effective_catalogue_artwork(snapshot, newer_bare).empty());

    CatalogueItem explicit_artist = artist;
    explicit_artist.artwork = {explicit_artist_art};
    CHECK(effective_catalogue_artwork(snapshot, explicit_artist) ==
          std::vector<CatalogueArtwork>{explicit_artist_art});

    // Compilation artwork does not leak across track-artist relationships: only
    // albums whose release/album artist is this direct catalogue parent qualify.
    CHECK(effective_catalogue_artwork(snapshot, artist) !=
          std::vector<CatalogueArtwork>{compilation_art});
    CHECK(artist.artwork.empty());
    CHECK(track.artwork.empty());
}

MACHA_FAST_TEST("hydration_catalogue", test_media_indexes_round_trip_and_older_shards_still_read) {
    CatalogueSnapshot snapshot;
    CatalogueItem item;
    item.id = "codec:index";
    item.kind = CatalogueKind::movie;
    item.title = "Index";
    item.revision = 1;
    snapshot.items.emplace(item.id, item);
    const std::string media_id = "macha:" + std::string(64, 'b');
    const ObjectId index = object_id(Bytes{1, 2, 3});
    snapshot.media_indexes.emplace(media_id, index);
    const auto roundtrip = decode_catalogue(encode_catalogue(snapshot));
    REQUIRE(roundtrip.media_indexes.contains(media_id));
    CHECK(roundtrip.media_indexes.at(media_id) == index);

    // A shard written before media indexes existed (MCAT0021: no index
    // section) is data already on disk and must still read.
    snapshot.media_indexes.clear();
    auto older = encode_catalogue(snapshot);
    REQUIRE(older.size() > 12);
    older[7] = '1';                            // MCAT0022 -> MCAT0021
    older.resize(older.size() - 4);            // no index count
    const auto read_back = decode_catalogue(older);
    CHECK(read_back.items.contains(item.id));
    CHECK(read_back.media_indexes.empty());
}

MACHA_TEST("hydration_catalogue", test_catalogue_on_one_node_predicts_caches_and_protects_its_control_objects) {
    // CatalogueManager, MachaDFS and the catalogue predictor on one node:
    // prediction resolves against real file manifests; the media-id index and
    // the decoded catalogue survive unrelated namespace and metadata churn;
    // macOS NFC callbacks find NFD-persisted names; a cold load needs no local
    // artwork; CONTROL GC spares a future root's staging; and a catalogue whose
    // CONTROL manifest vanished fails repair closed while serving its snapshot.
    CatalogueNode node("catalogue-node");

    // Prediction advances within a season, crosses into the next season, and
    // advances a movie collection; every predicted run begins at extent zero.
    {
        node.mkdir("/TV");
        node.mkdir("/Movies");
        const auto make_file = [&](const std::string& path, uint8_t value) {
            (void)node.write(path, Bytes(2 * 1024 * 1024 + 12345, value));
            auto entry = node.filesystem().getattr(path);
            REQUIRE(entry.extents.size() >= 3);
            return entry;
        };
        const auto ep1_file = make_file("/TV/s01e01.mkv", 31);
        const auto ep2_file = make_file("/TV/s01e02.mkv", 32);
        const auto ep3_file = make_file("/TV/s02e01.mkv", 33);
        const auto movie1_file = make_file("/Movies/one.mkv", 41);
        const auto movie2_file = make_file("/Movies/two.mkv", 42);

        const auto show = node.upsert("show:test", CatalogueKind::show, "Test Show");
        const auto add = [&](CatalogueItem item) { return node.catalogue().upsert(item); };
        CatalogueItem season1;
        season1.id = "season:test:1";
        season1.kind = CatalogueKind::season;
        season1.title = "Season 1";
        season1.parent_id = show.id;
        season1.season_number = 1;
        season1 = add(season1);
        auto season2 = season1;
        season2.id = "season:test:2";
        season2.title = "Season 2";
        season2.season_number = 2;
        season2 = add(season2);
        const auto episode = [&](std::string id, std::string title, const CatalogueItem& season, int number,
                                 const FsEntry& file) {
            CatalogueItem item;
            item.id = std::move(id);
            item.kind = CatalogueKind::episode;
            item.title = std::move(title);
            item.parent_id = season.id;
            item.season_number = season.season_number;
            item.episode_number = number;
            item.media_ids = {file_media_id(file)};
            return add(item);
        };
        (void)episode("episode:test:1:1", "One", season1, 1, ep1_file);
        (void)episode("episode:test:1:2", "Two", season1, 2, ep2_file);
        (void)episode("episode:test:2:1", "Three", season2, 1, ep3_file);
        // Movies bind by plain and by prefixed path as well as by media id.
        CatalogueItem movie1;
        movie1.id = "movie:test:1";
        movie1.kind = CatalogueKind::movie;
        movie1.title = "First Film";
        movie1.year = 2001;
        movie1.external_ids["collection"] = "test-films";
        movie1.media_ids = {"/Movies/one.mkv"};
        (void)add(movie1);
        auto movie2 = movie1;
        movie2.id = "movie:test:2";
        movie2.title = "Second Film";
        movie2.year = 2003;
        movie2.media_ids = {"path:/Movies/two.mkv"};
        (void)add(movie2);

        HydrationConfig prediction_config;
        prediction_config.catalogue_lookahead = 1;
        PlaybackTracker tracker;
        CatalogueSequenceHintProvider predictor(tracker, node.filesystem(), node.catalogue(), prediction_config);
        const auto check_prediction = [&](const std::string& path, const FsEntry& current,
                                          const FsEntry& expected, const char* reason) {
            auto active = tracker.open(path, current);
            tracker.progress(active, 0);
            auto predicted = predictor.hints();
            REQUIRE(predicted.size() == 1);
            CHECK(predicted[0].reason == reason);
            REQUIRE(!predicted[0].objects.empty());
            CHECK(predicted[0].objects.front() == expected.extents.front().id);
            CHECK(predicted[0].objects.size() == expected.extents.size());
            tracker.close(active);
        };
        check_prediction("/TV/s01e01.mkv", ep1_file, ep2_file, "next_episode");
        check_prediction("/TV/s01e02.mkv", ep2_file, ep3_file, "next_episode");
        check_prediction("/Movies/one.mkv", movie1_file, movie2_file, "next_movie");
    }

    // An already resolved, content-addressed media id survives unrelated
    // namespace churn; a new id is a miss that rebuilds, after which both resolve.
    {
        node.mkdir("/media");
        const auto a_id = node.write("/media/a.mkv", pattern(32 * 1024 + 17));
        auto first = node.filesystem().find_media(a_id);
        REQUIRE(first.has_value());
        CHECK(first->first == "/media/a.mkv");
        node.mkdir("/noise");
        auto cached = node.filesystem().find_media(a_id);
        REQUIRE(cached.has_value());
        CHECK(cached->first == "/media/a.mkv");
        CHECK(file_media_id(cached->second) == a_id);
        const auto b_id = node.write("/media/b.mkv", pattern(48 * 1024 + 29));
        CHECK(b_id != a_id);
        auto second = node.filesystem().find_media(b_id);
        REQUIRE(second.has_value());
        CHECK(second->first == "/media/b.mkv");
        REQUIRE(node.filesystem().find_media(a_id).has_value());

        // The index follows renames and removals, and content held at two
        // paths still resolves while either remains.
        node.filesystem().rename("/media/b.mkv", "/media/b-renamed.mkv");
        auto renamed = node.filesystem().find_media(b_id);
        REQUIRE(renamed.has_value());
        CHECK(renamed->first == "/media/b-renamed.mkv");
        const auto twin_id = node.write("/media/a-copy.mkv", pattern(32 * 1024 + 17));
        CHECK(twin_id == a_id);
        REQUIRE(node.filesystem().find_media(a_id).has_value());
        node.filesystem().unlink("/media/a.mkv");
        auto survivor = node.filesystem().find_media(a_id);
        REQUIRE(survivor.has_value());
        CHECK(survivor->first == "/media/a-copy.mkv");
        node.filesystem().unlink("/media/a-copy.mkv");
        CHECK(!node.filesystem().find_media(a_id).has_value());
        CHECK(node.filesystem().find_media(b_id).has_value());
    }

#if defined(__APPLE__)
    // The runtime alias index resolves NFC callbacks to the exact persisted NFD
    // spelling instead of rewriting the metadata representation, and a new NFC
    // leaf under an NFD parent keeps the stored parent spelling.
    {
        node.mkdir("/Music");
        const std::string nfd_dir = "/Music/Cafe\xcc\x81 del Mar";
        const std::string nfd_file = nfd_dir + "/01.Clannad - Na Buachailli\xcc\x81 lainn.mp3";
        const std::string nfc_dir = "/Music/Caf\xc3\xa9 del Mar";
        const std::string nfc_file = nfc_dir + "/01.Clannad - Na Buachaill\xc3\xad lainn.mp3";
        node.metadata().mutate([&](MetadataSnapshot& snapshot) {
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
        CHECK(node.filesystem().getattr(nfc_dir).type == EntryType::directory);
        CHECK(node.filesystem().getattr(nfc_file).type == EntryType::file);
        auto listed = node.filesystem().readdir(nfc_dir);
        REQUIRE(listed.size() == 1);
        CHECK(listed.front().first == "01.Clannad - Na Buachailli\xcc\x81 lainn.mp3");
        const std::string new_nfc = nfc_dir + "/Macha Caf\xc3\xa9 Test.mp3";
        node.filesystem().create_file(new_nfc, 0644, getuid(), getgid());
        CHECK(node.metadata().snapshot().entries.contains(nfd_dir + "/Macha Caf\xc3\xa9 Test.mp3"));
        CHECK(node.filesystem().getattr(new_nfc).type == EntryType::file);
    }
#endif

    // A cold load from the sharded CONTROL representation is ready without the
    // referenced artwork DATA held locally.
    {
        auto item = node.upsert("test:movie:artwork-missing", CatalogueKind::movie, "Catalogue Still Loads");
        const auto artwork = node.catalogue().put_artwork(item.id, "poster", "image/jpeg",
                                                          Bytes{0x01, 0x02, 0x03, 0x04}, item.revision);
        REQUIRE(node.node().local_store().remove(artwork.id));
        REQUIRE(!node.node().local_store().has(artwork.id));
        CatalogueManager reloaded(node.node(), node.node().local_state(), node.node().metadata_server(),
                                  node.store(), node.metadata(), node.node().ledger());
        reloaded.repair_once();
        const auto status = reloaded.status();
        CHECK(status.ready);
        CHECK(status.artwork_objects == 1);
        CHECK(status.local_artwork_objects == 0);
        auto loaded = reloaded.get(item.id);
        REQUIRE(loaded.has_value());
        CHECK(loaded->title == item.title);
    }

    // CONTROL publication is data-before-metadata: an object written after the
    // observed catalogue root is kept by GC even with zero grace, and becomes an
    // ordinary orphan once a successor root is committed and observed.
    {
        auto& control = node.node().control_store();
        REQUIRE(wait_until([&] {
            try {
                node.catalogue().repair_once();
                return node.catalogue().status().ready;
            } catch (...) {
                return false;
            }
        }));
        Bytes staged = pattern(4096 + 37);
        staged[0] ^= 0x6d;
        const auto staged_id = object_id(staged);
        REQUIRE(control.put(staged_id, staged));
        std::this_thread::sleep_for(5ms);
        const std::vector<ObjectId> no_live;
        for (int i = 0; i < 4; ++i) (void)node.catalogue().control_gc_step(no_live, 0ms, 64);
        CHECK(control.has(staged_id));
        (void)node.upsert("movie:control-publication-fence", CatalogueKind::movie, "Control Publication Fence");
        std::this_thread::sleep_for(5ms);
        auto maintenance = maintenance_inventory(node.catalogue());
        REQUIRE(maintenance.complete);
        std::vector<ObjectId> live(maintenance.control_live.begin(), maintenance.control_live.end());
        for (int i = 0; i < 4 && control.has(staged_id); ++i)
            (void)node.catalogue().control_gc_step(live, 0ms, 64);
        CHECK(!control.has(staged_id));
    }

    // A warm immutable catalogue stays usable when ordinary metadata moves on
    // without changing catalogue_root, even with its CONTROL manifest gone:
    // definitely_absent answers from the decoded view, and repair fails closed.
    {
        auto committed = node.upsert("test:movie:1", CatalogueKind::movie, "Cached Movie");
        CHECK(committed.title == "Cached Movie");
        const auto status = node.catalogue().status();
        REQUIRE(status.root.has_value());
        REQUIRE(node.node().control_store().remove(*status.root));
        node.metadata().mutate([](MetadataSnapshot& snapshot) {
            auto root = snapshot.entries.find("/");
            REQUIRE(root != snapshot.entries.end());
            ++root->second.version;
            ++root->second.mtime_ns;
        });
        auto cached = node.catalogue().get("test:movie:1");
        REQUIRE(cached.has_value());
        CHECK(cached->title == "Cached Movie");
        CHECK(node.catalogue().definitely_absent("test:movie:missing"));
        bool repair_failed = false;
        try {
            node.catalogue().repair_once();
        } catch (const CatalogueUnavailable&) {
            repair_failed = true;
        }
        CHECK(repair_failed);
        auto after_repair = node.catalogue().get("test:movie:1");
        REQUIRE(after_repair.has_value());
        CHECK(after_repair->title == "Cached Movie");
    }
}

// A catalogue write encodes only the shards it touches, and the catalogue it
// leaves is the one a reader loading the root from scratch finds: every item
// in its own shard, none left behind in a shard it moved out of or was
// removed from.
MACHA_TEST("hydration_catalogue", test_a_catalogue_write_touches_its_own_shards_and_leaves_a_whole_catalogue) {
    CatalogueNode node("catalogue-shards");
    auto& catalogue = node.catalogue();
    const auto item_of = [](int n, const std::string& title) {
        CatalogueItem item;
        item.id = "movie:shard:" + std::to_string(n);
        item.kind = CatalogueKind::movie;
        item.title = title;
        return item;
    };
    const auto agrees = [&] {
        CatalogueManager from_root(node.node(), node.node().local_state(),
                                   node.node().metadata_server(), node.store(),
                                   node.metadata(), node.node().ledger());
        from_root.repair_once();
        const auto loaded = from_root.snapshot();
        const auto held = catalogue.snapshot();
        CHECK(loaded.items == held.items);
        CHECK(loaded.media_profiles == held.media_profiles);
        CHECK(loaded.media_indexes == held.media_indexes);
        return loaded.items == held.items;
    };

    std::vector<CatalogueItem> many;
    for (int i = 0; i < 300; ++i)
        many.push_back(item_of(i, "Title " + std::to_string(i)));
    (void)catalogue.upsert_many(many);
    REQUIRE(agrees());
    for (const int n : {3, 77, 150, 299})
        (void)catalogue.upsert(item_of(n, "Retitled " + std::to_string(n)));
    REQUIRE(agrees());
    for (const int n : {5, 77, 201})
        CHECK(catalogue.erase("movie:shard:" + std::to_string(n)));
    REQUIRE(agrees());
    (void)catalogue.upsert(item_of(1000, "Added"));
    const std::string index = R"({"status":"ok","schema_version":1,"streams":[]})";
    catalogue.put_media_index("macha:" + std::string(64, 'd'), Bytes(index.begin(), index.end()));
    REQUIRE(agrees());
    CHECK(catalogue.snapshot().items.size() == 300 - 3 + 1);
    CHECK(!catalogue.get("movie:shard:77").has_value());
    CHECK(catalogue.get("movie:shard:150")->title == "Retitled 150");
}

MACHA_FAST_TEST("hydration_catalogue", test_a_page_resumes_after_its_cursor_and_never_repeats) {
    const auto json_body_code = [](const HttpResponse& response) {
        const auto body = Json::parse(std::string(response.body.begin(), response.body.end()));
        return body.find("error")->find("code")->asString();
    };
    const auto page_of = [&](std::map<std::string, std::string, std::less<>> query) {
        HttpRequest request;
        for (auto& [name, value] : query) request.query[name] = value;
        PageQuery page;
        const auto bad = read_page_query(request, page);
        return std::pair{page, bad ? std::optional<std::string>(json_body_code(*bad)) : std::nullopt};
    };
    const auto key = [](const std::string& entry) -> std::string_view { return entry; };

    // Without a limit, one page holds everything and says nothing follows.
    std::vector<std::string> keys{"a", "c", "e", "g"};
    auto range = page_range(keys, key, page_of({}).first);
    CHECK(range.begin == 0);
    CHECK(range.end == 4);
    CHECK(!range.next_cursor);

    // Pages of two; an entry inserted behind the cursor is not seen, one
    // ahead of it is, and none is seen twice.
    range = page_range(keys, key, page_of({{"limit", "2"}}).first);
    CHECK(range.begin == 0);
    CHECK(range.end == 2);
    REQUIRE(range.next_cursor.has_value());
    keys = {"a", "b", "c", "d", "e", "g"};
    auto [next, bad] = page_of({{"limit", "2"}, {"cursor", *range.next_cursor}});
    REQUIRE(!bad);
    range = page_range(keys, key, next);
    CHECK(keys[range.begin] == "d");
    CHECK(keys[range.end - 1] == "e");
    REQUIRE(range.next_cursor.has_value());
    range = page_range(keys, key, page_of({{"limit", "2"}, {"cursor", *range.next_cursor}}).first);
    CHECK(keys[range.begin] == "g");
    CHECK(range.end == keys.size());
    CHECK(!range.next_cursor);

    // A cursor whose entry was removed still resumes after its key.
    range = page_range(std::vector<std::string>{"a", "e"}, key,
                       page_of({{"cursor", page_cursor("c")}}).first);
    CHECK(range.begin == 1);

    CHECK(page_of({{"limit", "0"}}).second == "bad_limit");
    CHECK(page_of({{"limit", "1001"}}).second == "bad_limit");
    CHECK(page_of({{"limit", "ten"}}).second == "bad_limit");
    CHECK(page_of({{"cursor", "xyz"}}).second == "bad_cursor");
    CHECK(page_of({{"cursor", "abc"}}).second == "bad_cursor");
    CHECK(page_cursor_key(page_cursor("any key / \xff")) == "any key / \xff");
}

MACHA_TEST("hydration_catalogue", test_catalogue_lists_page_in_key_order) {
    CatalogueNode node("catalogue-paging");
    CatalogueApi api(node.catalogue(), node.hints());
    for (int i = 0; i < 25; ++i)
        (void)node.upsert("movie:" + std::to_string(100 + i), CatalogueKind::movie,
                          "Paging " + std::string(1, static_cast<char>('z' - i)));
    const auto get = [&](std::string path, std::map<std::string, std::string, std::less<>> query) {
        auto request = api_request("GET", std::move(path));
        for (auto& [name, value] : query) request.query[name] = value;
        auto response = api.handle(request);
        REQUIRE(response.status == 200);
        return Json::parse(std::string(response.body.begin(), response.body.end()));
    };
    const auto ids = [](const Json& body) {
        std::vector<std::string> out;
        for (const auto& item : body.find("items")->asArray())
            out.push_back(item.find("id")->asString());
        return out;
    };

    // Whole, in id order, with nothing to follow.
    const auto whole = get("/api/v1/catalogue/items", {});
    const auto all = ids(whole);
    CHECK(all.size() == 25);
    CHECK(std::is_sorted(all.begin(), all.end()));
    CHECK(whole.find("next_cursor")->isNull());

    // Pages of ten give every item once, in the same order.
    std::vector<std::string> paged;
    std::map<std::string, std::string, std::less<>> query{{"limit", "10"}};
    size_t pages = 0;
    for (;;) {
        const auto body = get("/api/v1/catalogue/items", query);
        const auto page = ids(body);
        CHECK(page.size() <= 10);
        paged.insert(paged.end(), page.begin(), page.end());
        ++pages;
        if (body.find("next_cursor")->isNull()) break;
        query["cursor"] = body.find("next_cursor")->asString();
        REQUIRE(pages < 10);
    }
    CHECK(pages == 3);
    CHECK(paged == all);

    // Search pages through its ranking the same way.
    std::vector<std::string> found;
    query = {{"q", "Paging"}, {"limit", "7"}};
    for (pages = 0;; ++pages) {
        REQUIRE(pages < 10);
        const auto body = get("/api/v1/catalogue/search", query);
        const auto page = ids(body);
        found.insert(found.end(), page.begin(), page.end());
        if (body.find("next_cursor")->isNull()) break;
        query["cursor"] = body.find("next_cursor")->asString();
    }
    auto sorted = found;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == all);

    auto bad = api_request("GET", "/api/v1/catalogue/items");
    bad.query["cursor"] = "not-hex";
    CHECK(api.handle(bad).status == 400);
}

MACHA_TEST("hydration_catalogue", test_catalogue_api_edits_searches_and_signs_artwork) {
    // CatalogueApi over one node's catalogue: item edits keep files unless
    // named and validate parents; search filters before its limit; effective
    // artwork is display-only; artwork URLs are signed, stable, capability
    // exempt and refuse tampering or expiry; media indexes are served immutably.
    CatalogueNode node("catalogue-api");
    CatalogueApi api(node.catalogue(), node.hints());
    const auto call = [&](CatalogueApi& on, std::string method, std::string path, std::string body = {},
                          std::map<std::string, std::string, std::less<>> headers = {}) {
        return on.handle(api_request(std::move(method), std::move(path), Bytes(body.begin(), body.end()),
                                     std::move(headers)));
    };

    // A whole PUT that leaves out media_ids keeps the item's files; PATCH
    // changes only what it names; parents are checked; a bad body is a 400;
    // and a hand edit is locked against the scanner unless it says otherwise.
    {
        (void)node.upsert("show:edit-test", CatalogueKind::show, "Edit Show");
        CatalogueItem movie;
        movie.id = "movie:edit-test";
        movie.kind = CatalogueKind::movie;
        movie.title = "Edit Movie";
        movie.media_ids = {"macha:file-a", "macha:file-b"};
        movie = node.catalogue().upsert(movie);
        const auto item_call = [&](std::string method, std::string id, std::string body) {
            return call(api, std::move(method), "/api/v1/catalogue/items/" + id, std::move(body));
        };
        REQUIRE(item_call("PUT", "movie%3Aedit-test", R"({"kind":"movie","title":"Renamed"})").status == 200);
        auto stored = node.catalogue().get(movie.id);
        REQUIRE(stored.has_value());
        CHECK(stored->title == "Renamed");
        CHECK((stored->media_ids == std::vector<std::string>{"macha:file-a", "macha:file-b"}));
        CHECK(stored->external_ids.at("macha_metadata_locked") == "1");
        REQUIRE(item_call("PATCH", "movie%3Aedit-test", R"({"year":1999,"lock":false})").status == 200);
        stored = node.catalogue().get(movie.id);
        CHECK(stored->title == "Renamed");
        CHECK(stored->year == std::optional<int32_t>{1999});
        CHECK(stored->media_ids.size() == 2);
        CHECK(!stored->external_ids.contains("macha_metadata_locked"));
        CHECK(item_call("PATCH", "movie%3Anope", R"({"year":2000})").status == 404);
        auto malformed = item_call("PATCH", "movie%3Aedit-test", "[1,2]");
        CHECK(malformed.status == 400);
        CHECK(api_body(malformed).find("error")->asString() == "bad_item");
        auto missing = item_call("PUT", "season%3Aedit-test",
                                 R"({"kind":"season","title":"S1","parent_id":"show:absent"})");
        CHECK(missing.status == 400);
        CHECK(api_body(missing).find("error")->asString() == "parent_not_found");
        CHECK(api_body(missing).find("parent_id")->asString() == "show:absent");
        auto wrong = item_call("PUT", "episode%3Aedit-test",
                               R"({"kind":"episode","title":"E1","parent_id":"show:edit-test"})");
        CHECK(wrong.status == 400);
        CHECK(api_body(wrong).find("error")->asString() == "bad_parent_kind");
        CHECK(api_body(wrong).find("kind")->asString() == "episode");
        CHECK(api_body(wrong).find("parent_kind")->asString() == "show");
        CHECK(!node.catalogue().get("episode:edit-test").has_value());
        CHECK(item_call("PUT", "season%3Aedit-test",
                        R"({"kind":"season","title":"S1","parent_id":"show:edit-test"})").status == 201);
    }

    // `kind` may repeat, `parent` keeps one item's children, both filter before
    // `limit`, and an unknown kind is a 400.
    {
        for (int i = 0; i < 5; ++i)
            (void)node.upsert("episode:ember-" + std::to_string(i), CatalogueKind::episode,
                              "Ember Episode " + std::to_string(i));
        (void)node.upsert("movie:ember", CatalogueKind::movie, "Ember");
        (void)node.upsert("show:ember", CatalogueKind::show, "Ember Show");
        (void)node.upsert("season:ember-1", CatalogueKind::season, "Ember Season", "show:ember");
        (void)node.upsert("season:other-1", CatalogueKind::season, "Ember Season Elsewhere", "show:other");
        const auto search = [&](std::map<std::string, std::vector<std::string>, std::less<>> all) {
            auto request = api_request("GET", "/api/v1/catalogue/search");
            for (const auto& [key, values] : all) request.query[key] = values.back();
            request.query_all = std::move(all);
            return api.handle(request);
        };
        const auto ids = [](const HttpResponse& response) {
            std::set<std::string> out;
            const auto body = api_body(response);
            for (const auto& item : body.find("items")->asArray()) out.insert(item.find("id")->asString());
            return out;
        };
        auto kinds = search({{"q", {"ember"}}, {"kind", {"movie", "show"}}, {"limit", {"2"}}});
        REQUIRE(kinds.status == 200);
        CHECK((ids(kinds) == std::set<std::string>{"movie:ember", "show:ember"}));
        auto children = search({{"q", {"ember"}}, {"parent", {"show:ember"}}});
        REQUIRE(children.status == 200);
        CHECK((ids(children) == std::set<std::string>{"season:ember-1"}));
        auto unknown = search({{"q", {"ember"}}, {"kind", {"film"}}});
        CHECK(unknown.status == 400);
        CHECK(std::string(unknown.body.begin(), unknown.body.end()).find("bad_kind") != std::string::npos);
    }

    // A track inherits its album's cover as effective_artwork for display; it
    // is never canonical, so a round-tripped GET does not store it.
    {
        const auto artist = node.upsert("artist:api-test", CatalogueKind::artist, "API Artist");
        CatalogueItem album;
        album.id = "album:api-test";
        album.kind = CatalogueKind::album;
        album.title = "API Album";
        album.parent_id = artist.id;
        album.year = 2024;
        album = node.catalogue().upsert(album);
        const auto track = node.upsert("track:api-test", CatalogueKind::track, "API Track", album.id);
        const Bytes cover_bytes{0x10, 0x20, 0x30, 0x40};
        const auto cover = node.catalogue().put_artwork(album.id, "cover", "image/jpeg", cover_bytes, album.revision);
        const auto track_response = call(api, "GET", "/api/v1/catalogue/items/track%3Aapi-test");
        REQUIRE(track_response.status == 200);
        const auto track_json = api_body(track_response);
        REQUIRE(track_json.find("artwork")->isArray());
        CHECK(track_json.find("artwork")->asArray().empty());
        REQUIRE(track_json.find("effective_artwork")->asArray().size() == 1);
        CHECK(track_json.find("effective_artwork")->asArray().front().find("id")->asString() == to_string(cover.id));
        const auto artist_json = api_body(call(api, "GET", "/api/v1/catalogue/items/artist%3Aapi-test"));
        REQUIRE(artist_json.find("effective_artwork")->asArray().size() == 1);
        CHECK(artist_json.find("effective_artwork")->asArray().front().find("id")->asString() == to_string(cover.id));
        auto artwork_response = call(api, "GET", "/api/v1/catalogue/artwork/" + to_string(cover.id));
        REQUIRE(artwork_response.status == 200);
        CHECK(artwork_response.body == cover_bytes);
        auto put = api.handle(api_request("PUT", "/api/v1/catalogue/items/track%3Aapi-test", track_response.body,
                                          {{"if-match", "\"rev-" + std::to_string(track.revision) + "\""}}));
        REQUIRE(put.status == 200);
        auto stored = node.catalogue().get(track.id);
        REQUIRE(stored.has_value());
        CHECK(stored->artwork.empty());
    }

    // Artwork URLs: directly usable and signed, recognised by
    // capability_request() (which HttpServer consults to skip the bearer check)
    // only when signed, byte-identical across reads and routes so browsers can
    // cache them, valid for at least the TTL, and refused when tampered with or
    // expired.
    {
        CatalogueItem album;
        album.id = "album:signed-url-test";
        album.kind = CatalogueKind::album;
        album.title = "Signed URL Album";
        album = node.catalogue().upsert(album);
        const Bytes cover_bytes{0x01, 0x02, 0x03, 0x04};
        (void)node.catalogue().put_artwork(album.id, "cover", "image/png", cover_bytes, album.revision);
        CHECK(Config{}.catalogue.api.artwork_capability_ttl == std::chrono::hours(24 * 30));
        const std::chrono::milliseconds ttl = std::chrono::hours(24);
        CatalogueApi signing(node.catalogue(), node.hints(), {}, {}, ttl);
        const auto artwork_url = [&](CatalogueApi& on) {
            const auto response = call(on, "GET", "/api/v1/catalogue/items/album%3Asigned-url-test");
            REQUIRE(response.status == 200);
            const auto json = api_body(response);
            REQUIRE(json.find("artwork")->asArray().size() == 1);
            return json.find("artwork")->asArray().front().find("url")->asString();
        };
        const auto signed_request = [](const std::string& url) {
            const auto question = url.find('?');
            REQUIRE(question != std::string::npos);
            HttpRequest request;
            request.method = "GET";
            request.path = url.substr(0, question);
            request.query = parse_test_query(url.substr(question + 1));
            return request;
        };

        const auto first = artwork_url(signing);
        std::this_thread::sleep_for(5ms);
        CHECK(artwork_url(signing) == first);
        // The list route signs separately and must produce the same URL.
        const auto listed = api_body(call(signing, "GET", "/api/v1/catalogue/items"));
        bool found = false;
        for (const auto& item : listed.find("items")->asArray()) {
            if (item.find("id")->asString() != album.id) continue;
            found = true;
            CHECK(item.find("artwork")->asArray().front().find("url")->asString() == first);
        }
        CHECK(found);
        // The expiry is rounded up to the bucket after next, so a URL minted just
        // before a boundary still outlives the configured TTL.
        auto request = signed_request(first);
        REQUIRE(request.query.contains("exp"));
        const auto expires = std::stoull(request.query.at("exp"));
        const auto ttl_ms = static_cast<uint64_t>(ttl.count());
        CHECK(expires % ttl_ms == 0);
        const auto now = unix_ms();
        CHECK(expires >= now + ttl_ms);
        CHECK(expires <= now + 2 * ttl_ms);
        CHECK(signing.capability_request(request));
        const auto fetched = signing.handle(request);
        REQUIRE(fetched.status == 200);
        CHECK(fetched.body == cover_bytes);
        // Content-addressed: the id is the entity tag, the cache lifetime is the
        // capability's, and a revalidation is a bodiless 304.
        const auto tag = fetched.headers.at("ETag");
        CHECK(tag == "\"" + request.path.substr(request.path.rfind('/') + 1) + "\"");
        CHECK(fetched.headers.at("Cache-Control") ==
              "public, max-age=" + std::to_string(ttl_ms / 1000) + ", immutable");
        CHECK(fetched.headers.at("Timing-Allow-Origin") == "*");
        auto revalidation = request;
        revalidation.headers["if-none-match"] = tag;
        const auto not_modified = signing.handle(revalidation);
        CHECK(not_modified.status == 304);
        CHECK(not_modified.body.empty());
        CHECK(not_modified.headers.at("ETag") == tag);
        auto stale = request;
        stale.headers["if-none-match"] = "\"not-this-one\"";
        const auto refetched = signing.handle(stale);
        CHECK(refetched.status == 200);
        CHECK(refetched.body == cover_bytes);

        // The default-TTL API's URL is a capability too, and only when signed.
        auto default_request = signed_request(artwork_url(api));
        REQUIRE(default_request.query.contains("sig"));
        CHECK(api.capability_request(default_request));
        const auto default_fetched = api.handle(default_request);
        REQUIRE(default_fetched.status == 200);
        CHECK(default_fetched.headers.at("Cache-Control").find("immutable") != std::string::npos);
        auto with = [&](std::map<std::string, std::string, std::less<>> query) {
            auto tampered = default_request;
            tampered.query = std::move(query);
            return tampered;
        };
        auto tampered_sig = default_request.query;
        tampered_sig["sig"][0] = (tampered_sig["sig"][0] == '0') ? '1' : '0';
        CHECK(!api.capability_request(with(tampered_sig)));
        auto tampered_exp = default_request.query;
        tampered_exp["exp"] = std::to_string(std::stoull(tampered_exp["exp"]) + 1);
        CHECK(!api.capability_request(with(tampered_exp)));
        auto missing_sig = default_request.query;
        missing_sig.erase("sig");
        CHECK(!api.capability_request(with(missing_sig)));
        CHECK(!api.capability_request(with({})));
        // A correctly signed but expired URL is refused: expiry itself is
        // enforced. Minted through the real signing path with a 1 ms TTL.
        CatalogueApi short_lived(node.catalogue(), node.hints(), {}, {}, 1ms);
        const auto expired = signed_request(artwork_url(short_lived));
        std::this_thread::sleep_for(20ms);
        CHECK(!api.capability_request(expired));
    }

    // A media index is referenced DATA (live to GC), served immutably by the
    // keyframes route with its error codes, and goes with its media's profile.
    {
        const std::string media_id = "macha:" + std::string(64, 'c');
        const std::string body = R"({"status":"ok","schema_version":1,"streams":[]})";
        node.catalogue().put_media_index(media_id, Bytes(body.begin(), body.end()));
        auto stored = node.catalogue().media_index(media_id);
        REQUIRE(stored.has_value());
        CHECK(std::string(stored->begin(), stored->end()) == body);
        CHECK(maintenance_inventory(node.catalogue()).live.contains(object_id(Bytes(body.begin(), body.end()))));
        // A catalogue loaded from its root has the index too.
        {
            CatalogueManager from_root(node.node(), node.node().local_state(),
                                       node.node().metadata_server(), node.store(),
                                       node.metadata(), node.node().ledger());
            from_root.repair_once();
            auto loaded = from_root.media_index(media_id);
            REQUIRE(loaded.has_value());
            CHECK(std::string(loaded->begin(), loaded->end()) == body);
        }
        int calls = 0;
        CatalogueApi index_api(node.catalogue(), node.hints(), {}, {}, std::chrono::hours(24 * 30), {},
                               [&](const std::string& id) -> std::optional<Bytes> {
                                   ++calls;
                                   if (id == media_id) return node.catalogue().media_index(id);
                                   if (id == "macha:mpegts") throw KeyframeIndexUnsupported("no byte index");
                                   return std::nullopt;
                               });
        const auto get = [&](const std::string& id) {
            return call(index_api, "GET", "/api/v1/catalogue/media/" + id + "/keyframes");
        };
        auto served = get(media_id);
        REQUIRE(served.status == 200);
        CHECK(std::string(served.body.begin(), served.body.end()) == body);
        CHECK(served.headers.at("Cache-Control").find("immutable") != std::string::npos);
        CHECK(served.headers.at("ETag") == "\"" + media_id + "\"");
        CHECK(get("path:/Movies/x.mkv").status == 400);
        CHECK(get("macha:gone").status == 404);
        auto unsupported = get("macha:mpegts");
        CHECK(unsupported.status == 422);
        CHECK(std::string(unsupported.body.begin(), unsupported.body.end()).find("keyframes_not_supported") !=
              std::string::npos);
        CHECK(calls == 3);
        (void)node.catalogue().prune_media_profiles({});
        CHECK(!node.catalogue().media_index(media_id).has_value());
    }
}

MACHA_HEAVY_TEST("hydration_catalogue", test_catalogue_sync_search_and_artwork_gc) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();
    uint16_t p3 = free_port();

    auto c1 = config_for(cluster.path() / "cat1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "cat2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    auto c3 = config_for(cluster.path() / "cat3", cluster.keyfile(), p3, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = c3.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = c3.metadata_write_copies = 1;
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

    Service s1(c1, keys, test_durability_window);
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

    // A node joining after the catalogue already exists must become
    // browse/search capable without provider access. Artwork is ordinary DATA:
    // with R=1 it is not copied to every joining node, but every node must still
    // be able to read it from the elected/fallback owner.
    Service s2(c2, keys, test_durability_window);
    s2.start();
    REQUIRE(wait_until([&] {
        auto status = s2.catalogue().status();
        return status.ready && status.items == 2 && status.artwork_objects == 1;
    }, 10s));
    REQUIRE(s2.catalogue().get(episode.id).has_value());
    CHECK(s2.catalogue().search("test programme").front().id == show.id);
    // status.artwork_objects counts what this node's CATALOGUE knows about.
    // The bytes behind it are a DATA object the node may still be fetching, so
    // the count going to 1 does not mean artwork() can answer yet. Wait for the
    // fetch itself rather than for the count that precedes it.
    REQUIRE(wait_until([&] {
        try { return s2.catalogue().artwork(first_art.id).has_value(); } catch (...) { return false; }
    }, 10s));
    auto s2_first_art = s2.catalogue().artwork(first_art.id);
    REQUIRE(s2_first_art.has_value());
    CHECK(s2_first_art->bytes == first_art_bytes);

    Service s3(c3, keys, test_durability_window);
    s3.start();
    REQUIRE(wait_until([&] {
        auto status = s3.catalogue().status();
        return status.ready && status.items == 2 && status.artwork_objects == 1;
    }, 10s));
    CHECK(s3.catalogue().list(CatalogueKind::episode).size() == 1);
    REQUIRE(wait_until([&] {
        try { return s3.catalogue().artwork(first_art.id).has_value(); } catch (...) { return false; }
    }, 10s));
    auto s3_first_art = s3.catalogue().artwork(first_art.id);
    REQUIRE(s3_first_art.has_value());
    CHECK(s3_first_art->bytes == first_art_bytes);

    // Catalogue metadata can arrive through the bootstrap node before the
    // joining nodes have learned routes to one another.  R=1 placement and
    // fallback reads are only meaningful against a common membership view, so
    // establish that precondition rather than racing topology dissemination.
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 3 &&
               s2.node().membership().active().size() == 3 &&
               s3.node().membership().active().size() == 3;
    }, 10s));

    // Replacing the poster retires the old DATA object in committed metadata.
    // With a zero grace period, reachability GC must delete the old authoritative
    // copy while preserving exactly the normal R=1 placement semantics for the
    // replacement.
    auto second_art_bytes = pattern(96 * 1024 + 3);
    second_art_bytes[0] ^= 0xa5;
    auto second_art = s2.catalogue().put_artwork(show.id, "poster", "image/jpeg",
                                                 second_art_bytes, show.revision);
    auto has_second_artwork_reference = [&](Service& service) {
        try {
            // Namespace and catalogue share Service's MetadataManager. Force the
            // known metadata generation into that manager, then run catalogue
            // convergence explicitly instead of waiting for background cadence.
            (void)service.filesystem().getattr("/");
            service.catalogue().repair_once();
            auto item = service.catalogue().get(show.id);
            return item && std::any_of(item->artwork.begin(), item->artwork.end(),
                                       [&](const CatalogueArtwork& art) {
                                           return art.id == second_art.id;
                                       });
        } catch (...) {
            return false;
        }
    };
    // `ready` means the node has a usable catalogue, not that it has observed
    // the just-committed generation.  Wait for the replacement reference to
    // converge before testing distributed DATA reads for that ObjectId.
    REQUIRE(wait_until([&] {
        return has_second_artwork_reference(s1) &&
               has_second_artwork_reference(s2) &&
               has_second_artwork_reference(s3);
    }, 10s));
    auto s1_second_art = s1.catalogue().artwork(second_art.id);
    auto s2_second_art = s2.catalogue().artwork(second_art.id);
    auto s3_second_art = s3.catalogue().artwork(second_art.id);
    REQUIRE(s1_second_art.has_value());
    REQUIRE(s2_second_art.has_value());
    REQUIRE(s3_second_art.has_value());
    CHECK(s1_second_art->bytes == second_art_bytes);
    CHECK(s2_second_art->bytes == second_art_bytes);
    CHECK(s3_second_art->bytes == second_art_bytes);
    // All known metadata replicas are online and have converged past the
    // replacement, so the inherited retention claim may now be causally released
    // and ordinary DATA GC must reclaim the old artwork. If a replica were
    // offline, that claim would remain as the physical safety barrier for a
    // potentially unseen accepted branch.
    REQUIRE(wait_until([&] {
        return !s1.local_state().data().has(first_art.id) &&
               !s2.local_state().data().has(first_art.id) &&
               !s3.local_state().data().has(first_art.id);
    }, 10s));

    CatalogueApi api(s3.catalogue(), s3.catalogue_hints());
    CHECK(!s3.catalogue().definitely_absent(show.id));
    CHECK(s3.catalogue().definitely_absent("show:does-not-exist"));
    auto missing_clear = api.handle({.method = "DELETE",
                                     .path = "/api/v1/catalogue/items/show%3Adoes-not-exist/metadata",
                                     .query = {},
                                     .headers = {},
                                     .body = {}, .session = {}});
    CHECK(missing_clear.status == 404);
    auto status_response = api.handle({.method = "GET",
                                       .path = "/api/v1/catalogue/status",
                                       .query = {},
                                       .headers = {},
                                       .body = {}, .session = {}});
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
                                       .body = {}, .session = {}});
    CHECK(search_response.status == 200);
    std::string search_body(search_response.body.begin(), search_response.body.end());
    CHECK(search_body.find("episode:test:1:1") != std::string::npos);

    // Clear Metadata is an atomic catalogue reset. Clearing a hierarchy parent
    // also removes its descendants, so a leaf's binding cannot keep the old
    // match alive.
    const auto clear = [&](uint64_t revision) {
        return s3.manage_api().handle(
            {.method = "DELETE",
             .path = "/api/v1/catalogue/items/show:test/metadata",
             .query = {},
             .headers = {{"if-match", "\"rev-" + std::to_string(revision) + "\""}},
             .body = {}, .session = {}});
    };
    auto clear_response = clear(show.revision + 1);
    // The poster replacement did not mutate the copy of `show`; use the current
    // revision if the optimistic request raced a catalogue refresh.
    if (clear_response.status == 409) {
        auto current_show = s3.catalogue().get(show.id);
        REQUIRE(current_show.has_value());
        clear_response = clear(current_show->revision);
    }
    CHECK(clear_response.status == 204);
    CHECK(!s3.catalogue().get(show.id).has_value());
    CHECK(!s3.catalogue().get(episode.id).has_value());

    s3.stop();
    s2.stop();
    s1.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_uses_final_state_after_coalesced_metadata_burst) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "catalogue-burst-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "catalogue-burst-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->metadata_write_copies = 1;
        config->maintenance.garbage_grace = 0ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
        // This case tests burst coalescing and GC, not failure detection: keep
        // liveness above scheduler jitter so a descheduled process cannot
        // cause a false topology edge, which would fence destructive GC.
        config->dead_after = 5s;
        config->catalogue.scanner.enabled = false;
        config->catalogue.api.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
    }

    TestGate metadata_gate;
    std::atomic_bool gate_metadata{};
    std::atomic_bool gate_once{};
    std::atomic_uint64_t catalogue_repairs{};
    // Convergence counters as each metadata run begins once the gate is set:
    // the first is the gated run, the second the burst's follow-up.
    std::atomic<Service*> observed{nullptr};
    std::mutex run_begins_mutex;
    std::vector<ConvergenceDemandDiagnostics> run_begins;
    Service s1(c1, keys, test_durability_window, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_metadata.load(std::memory_order_acquire)) {
            if (auto* service = observed.load(std::memory_order_acquire)) {
                std::lock_guard lock(run_begins_mutex);
                run_begins.push_back(service->metadata_convergence_diagnostics());
            }
        }
        if (stage == "metadata-repair-begin" &&
            gate_metadata.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel)) {
            metadata_gate.enter_and_wait();
        } else if (stage == "catalogue-repair-begin") {
            catalogue_repairs.fetch_add(1, std::memory_order_relaxed);
        }
    });
    observed.store(&s1, std::memory_order_release);
    Service s2(c2, keys, test_durability_window);
    struct GateOpener {
        TestGate& gate;
        ~GateOpener() { gate.open(); }
    } open_on_exit{metadata_gate};

    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    CatalogueItem item;
    item.id = "movie:coalesced-catalogue";
    item.kind = CatalogueKind::movie;
    item.title = "Initial Catalogue Title";
    item = s2.catalogue().upsert(item);
    auto initial_bytes = pattern(32 * 1024 + 11, 41);
    auto initial_art = s2.catalogue().put_artwork(
        item.id, "poster", "image/jpeg", initial_bytes, item.revision);
    item = *s2.catalogue().get(item.id);

    REQUIRE(wait_until([&] {
        try {
            const auto found = s1.catalogue().get(item.id);
            return found && found->title == "Initial Catalogue Title" &&
                   !s1.catalogue().search("initial catalogue").empty();
        } catch (...) {
            return false;
        }
    }, 10s));
    // The baseline must be the snapshot that satisfied quiescence: a second
    // sample can catch a newly scheduled run and put every delta off by one.
    ConvergenceDemandDiagnostics convergence_before{};
    REQUIRE(wait_until([&] {
        const auto d = s1.metadata_convergence_diagnostics();
        if (d.scheduled || d.runs_scheduled != d.runs_completed)
            return false;
        convergence_before = d;
        return true;
    }, 5s));
    const auto convergence_events = [&] {
        return s1.resources().events.count(NodeEvent::metadata) +
               s1.resources().events.count(NodeEvent::topology);
    };
    const auto events_before = convergence_events();

    const auto repairs_before = catalogue_repairs.load(std::memory_order_acquire);
    gate_metadata.store(true, std::memory_order_release);

    item.title = "Intermediate Catalogue Title";
    item = s2.catalogue().upsert(item, item.revision);
    REQUIRE(metadata_gate.wait_for_entries(1, 5s));

    std::vector<ObjectId> superseded{initial_art.id};
    for (uint8_t index = 1; index <= 4; ++index) {
        item = *s2.catalogue().get(item.id);
        item.title = index == 4 ? "Final Catalogue Title"
                                : "Intermediate Catalogue Title " + std::to_string(index);
        item = s2.catalogue().upsert(item, item.revision);
        auto bytes = pattern(32 * 1024 + index, static_cast<uint8_t>(41 + index));
        auto art = s2.catalogue().put_artwork(
            item.id, "poster", "image/jpeg", bytes, item.revision);
        if (index < 4)
            superseded.push_back(art.id);
        else {
            initial_bytes = std::move(bytes);
            initial_art = art;
        }
    }
    const auto final_generation =
        s2.local_state().replica().committed_generation();
    REQUIRE(wait_until([&] {
        return s1.metadata_server().known_generation() >= final_generation;
    }, 5s));
    // Not "no catalogue repair yet": the gate stops s1's repair pass, not its
    // committed generation, because publish_commit accepts commits on replicas
    // directly, so Service::loop may repair under the gate. Coalescing is
    // asserted at the end over the whole window, gate included.
    const auto gated_repairs = catalogue_repairs.load(std::memory_order_acquire);

    metadata_gate.open();
    const bool final_state_ready = wait_until([&] {
        try {
            const auto status = s1.catalogue().status();
            const auto found = s1.catalogue().get(item.id);
            // Reconciliation may publish a later merge/retention generation
            // after the writer-side generation captured above. A newer cached
            // view is valid only when it also contains the exact final item and
            // artwork state asserted below.
            return status.metadata_generation >= final_generation && found &&
                   found->title == "Final Catalogue Title" &&
                   std::any_of(found->artwork.begin(), found->artwork.end(),
                               [&](const CatalogueArtwork& art) {
                                   return art.id == initial_art.id;
                               });
        } catch (...) {
            return false;
        }
    }, 10s);
    if (!final_state_ready) {
        const auto status = s1.catalogue().status();
        const auto found = s1.catalogue().get(item.id);
        const auto convergence = s1.metadata_convergence_diagnostics();
        const auto available = s1.metadata_manager().available_snapshot_view();
        std::string artwork;
        if (found) {
            for (const auto& candidate : found->artwork) {
                if (!artwork.empty())
                    artwork += ',';
                artwork += to_string(candidate.id);
            }
        }
        throw std::runtime_error(
            "catalogue final state missing: cached_generation=" +
            std::to_string(status.metadata_generation) +
            " known_generation=" + std::to_string(status.known_metadata_generation) +
            " available_generation=" +
            std::to_string(available ? available->generation : 0) +
            " expected_generation=" + std::to_string(final_generation) +
            " title=" + (found ? found->title : std::string("<missing>")) +
            " artwork=" + artwork +
            " expected_artwork=" + to_string(initial_art.id) +
            " convergence_requested=" + std::to_string(convergence.requested_epoch) +
            " convergence_completed=" + std::to_string(convergence.completed_epoch) +
            " convergence_scheduled=" + (convergence.scheduled ? "true" : "false"));
    }

    const auto final_search = s1.catalogue().search("final catalogue");
    REQUIRE(!final_search.empty());
    CHECK(final_search.front().id == item.id);
    CHECK(s1.catalogue().search("intermediate catalogue").empty());
    const auto final_artwork = s1.catalogue().artwork(initial_art.id);
    REQUIRE(final_artwork.has_value());
    CHECK(final_artwork->bytes == initial_bytes);

    // Measured as the burst's follow-up run begins: the gated run has
    // completed and the burst has become that one run, with at most one more
    // scheduled by reconciliation publishing its own accepted metadata. Later
    // metadata (artwork and retention commits replicating) is not this burst.
    // The pure one-follow-up state-machine contract is covered by the
    // dedicated ConvergenceDemand test.
    REQUIRE(wait_until([&] {
        std::lock_guard lock(run_begins_mutex);
        return run_begins.size() >= 2;
    }, 10s));
    ConvergenceDemandDiagnostics follow_up{};
    {
        std::lock_guard lock(run_begins_mutex);
        follow_up = run_begins[1];
    }
    const auto events_delta = convergence_events() - events_before;
    const auto scheduled_delta = follow_up.runs_scheduled - convergence_before.runs_scheduled;
    const auto completed_delta = follow_up.runs_completed - convergence_before.runs_completed;
    if (completed_delta != 1 || scheduled_delta < 2 || scheduled_delta > 3 ||
        scheduled_delta >= events_delta) {
        throw std::runtime_error(
            "unexpected convergence run count for one coalesced burst: before_scheduled=" +
            std::to_string(convergence_before.runs_scheduled) +
            " before_completed=" + std::to_string(convergence_before.runs_completed) +
            " follow_up_scheduled=" + std::to_string(follow_up.runs_scheduled) +
            " follow_up_completed=" + std::to_string(follow_up.runs_completed) +
            " events=" + std::to_string(events_delta) +
            " repairs_before=" + std::to_string(repairs_before) + " repairs_after=" +
            std::to_string(catalogue_repairs.load(std::memory_order_acquire)));
    }
    // Nine catalogue mutations, arriving as ~20 node events, must not become
    // ~20 catalogue repairs. A quarter of the demand events is a
    // generous ceiling on "coalesced" and still far below a per-event storm.
    // Not pinned to exactly one: each convergence run that advances the
    // committed generation correctly dirties the catalogue, and whether the
    // burst lands in the gated run or is split across it and its follow-up is
    // a timing accident.
    const auto repairs_after = catalogue_repairs.load(std::memory_order_acquire);
    const auto repairs_delta = repairs_after - repairs_before;
    if (repairs_delta < 1 || repairs_delta * 4 > events_delta) {
        throw std::runtime_error(
            "unexpected catalogue repair count for one coalesced burst: repairs_before=" +
            std::to_string(repairs_before) + " repairs_after=" + std::to_string(repairs_after) +
            " gated_repairs=" + std::to_string(gated_repairs) +
            " scheduled_delta=" + std::to_string(scheduled_delta) +
            " completed_delta=" + std::to_string(completed_delta) +
            " events=" + std::to_string(events_delta));
    }

    const auto unreclaimed = [&] {
        return std::none_of(superseded.begin(), superseded.end(), [&](const ObjectId& id) {
            return s1.local_state().data().has(id) || s2.local_state().data().has(id);
        });
    };
    const auto gc_started = Clock::now();
    const auto s2_wakeups_at_gc_start = s2.maintenance_wakeups();
    const std::string s2_stage_at_gc_start =
        std::string(s2.maintenance_stage()) + " " + s2.maintenance_sleep_diagnostic();
    if (!wait_until(unreclaimed, 12s)) {
        // A bounded second chance only classifies the failure as slow or
        // stuck; it never turns it into a pass.
        const bool eventually = wait_until(unreclaimed, 15s);
        const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - gc_started).count();
        // Name what is left and on which node.
        std::string remaining;
        for (const auto& id : superseded) {
            const bool on1 = s1.local_state().data().has(id);
            const bool on2 = s2.local_state().data().has(id);
            if (!on1 && !on2)
                continue;
            if (!remaining.empty())
                remaining += ' ';
            remaining += to_string(id).substr(0, 12);
            remaining += on1 && on2 ? "=both" : (on1 ? "=s1" : "=s2");
        }
        const auto status1 = s1.catalogue().status();
        const auto status2 = s2.catalogue().status();
        // The facts that decide whether s2's retention claims can ever be
        // released: a claim is erased only by a release clock that dominates
        // it, and release only runs when destructive GC is enabled.
        std::string claims;
        for (const auto& id : superseded) {
            if (!claims.empty())
                claims += ' ';
            claims += to_string(id).substr(0, 12);
            claims += ":s1=";
            claims += s1.local_state().retention().retained(RetentionClass::data, id) ? "held" : "free";
            claims += ",s2=";
            claims += s2.local_state().retention().retained(RetentionClass::data, id) ? "held" : "free";
            const auto state = s2.local_state().retention().claims(RetentionClass::data, id);
            claims += "(adds:";
            for (const auto& [origin, sequence] : state.adds)
                claims += to_string(origin).substr(0, 6) + "=" + std::to_string(sequence) + ";";
            claims += " removed:";
            for (const auto& [origin, sequence] : state.removed)
                claims += to_string(origin).substr(0, 6) + "=" + std::to_string(sequence) + ";";
            claims += ")";
        }
        const auto view2 = s2.metadata_manager().retention_release_view();
        std::string clock2;
        if (view2) {
            for (const auto& [node, sequence] : view2->snapshot->mutation_sequences) {
                if (!clock2.empty())
                    clock2 += ',';
                clock2 += to_string(node).substr(0, 6) + "=" + std::to_string(sequence);
            }
        }
        const auto cluster2 = s2.metadata_manager().cluster_status();
        throw std::runtime_error(
            "GC_DIAG s2_maintenance_passes_during_wait=" +
            std::to_string(s2.maintenance_wakeups() - s2_wakeups_at_gc_start) +
            " s2_stage_at_gc_start=" + s2_stage_at_gc_start +
            " s2_stage_now=" + s2.maintenance_stage() + " " + s2.maintenance_sleep_diagnostic() +
            " claims=[" + claims + "] s2_release_view=" +
            (view2 ? std::to_string(view2->generation) : std::string("none")) +
            " s2_release_clock=[" + clock2 + "] s2_committed=" +
            std::to_string(s2.local_state().replica().committed_generation()) +
            " s2_known=" + std::to_string(s2.metadata_server().known_generation()) +
            " s2_all_reachable=" + (s2.node().membership().all_known_reachable() ? "yes" : "no") +
            " s2_stable=" + (cluster2.stable ? "yes" : "no") +
            " s2_heads=" + std::to_string(s2.local_state().replica().accepted_heads().size()) +
            " s1_heads=" + std::to_string(s1.local_state().replica().accepted_heads().size()) +
            " s2_self=" + to_string(s2.node().node_id()).substr(0, 6) +
            " s1_self=" + to_string(s1.node().node_id()).substr(0, 6) + " | " +
            
            "superseded catalogue artwork was not reclaimed within 12s: remaining=[" + remaining +
            "] superseded_count=" + std::to_string(superseded.size()) +
            " s1_catalogue_generation=" + std::to_string(status1.metadata_generation) +
            " s1_known=" + std::to_string(status1.known_metadata_generation) +
            " s1_artwork_objects=" + std::to_string(status1.artwork_objects) +
            " s2_catalogue_generation=" + std::to_string(status2.metadata_generation) +
            " s2_artwork_objects=" + std::to_string(status2.artwork_objects) +
            " repairs_delta=" + std::to_string(repairs_delta) +
            " reclaimed_eventually=" + (eventually ? "yes" : "no") +
            " total_ms=" + std::to_string(total_ms));
    }

    s2.stop();
    s1.stop();
}

MACHA_FAST_TEST("hydration_catalogue", test_a_failed_hint_outlives_the_job_that_raised_it) {
    // A settled outcome goes with its job; a failure stays, as the only record
    // that the file was never catalogued.
    TempDir dir;
    CatalogueHintQueue hints(dir.path());
    const auto failed_id = hints.submit("/TV/Show/S01E01.mkv", "ingest", "job-1", CatalogueHintPriority::ingest);
    const auto matched_id = hints.submit("/TV/Show/S01E02.mkv", "ingest", "job-1", CatalogueHintPriority::ingest);
    auto first = hints.claim_next();
    auto second = hints.claim_next();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    hints.fail(failed_id, "catalogue_error", "provider lookup failed");
    hints.mark_no_match(matched_id, "tv", "macha:e02", "no match");
    CHECK(hints.erase_origin("ingest", "job-1") == 2);
    const auto kept = hints.get(failed_id);
    REQUIRE(kept.has_value());
    CHECK(kept->state == CatalogueHintState::failed);
    CHECK(kept->origins.empty());
    CHECK(!hints.get(matched_id).has_value());
}

MACHA_FAST_TEST("hydration_catalogue", test_a_discarded_payload_leaves_at_once_and_is_deleted_later) {
    // A discarded payload is renamed into staging's trash, still counted, and
    // deleted when the trash is emptied.
    TempDir dir;
    IngestConfig config;
    config.staging_path = dir.path() / "staging";
    config.staging_limit = 1ull << 30;
    StagingArea staging(config);
    const auto payload = config.staging_path / "torrents" / "job";
    std::filesystem::create_directories(payload / "Season 1");
    {
        std::ofstream out(payload / "Season 1" / "episode.mkv", std::ios::binary);
        out << std::string(4096, 'x');
    }
    const auto before = staging.status().disk_bytes;
    CHECK(staging.discard(payload));
    CHECK(!std::filesystem::exists(payload));
    CHECK(staging.status().disk_bytes == before); // counted until deleted
    CHECK(staging.empty_trash() == 1);
    CHECK(staging.status().disk_bytes < before);
    CHECK(std::filesystem::is_empty(staging.trash_path()));
    CHECK(!staging.discard(dir.path() / "outside")); // never outside staging
}

} // namespace

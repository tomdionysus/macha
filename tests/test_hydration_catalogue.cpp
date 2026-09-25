// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent_extent_journal.hpp"
#include "test_backend_support.hpp"
#include "acquisition_api.hpp"
#include "subsystem_abi.hpp"
#include "subsystem_registry.hpp"

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

MACHA_TEST("hydration_catalogue", test_hydration_scheduler_and_prediction) {
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

    // Catalogue prediction is resolved against actual Macha file manifests. It
    // advances within a season, crosses into the next season, and advances a
    // movie collection; every predicted run begins at extent zero.
    TestService fixture("store");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    auto& service = fixture.start();
    service.filesystem().mkdir("/TV", 0755, getuid(), getgid());
    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());

    auto make_file = [&](const std::string& path, uint8_t value) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto data = Bytes(2 * 1024 * 1024 + 12345, value);
        auto writer = service.filesystem().open_write(path, true);
        REQUIRE(writer->write(0, data) == data.size());
        writer->commit();
        auto entry = service.filesystem().getattr(path);
        REQUIRE(entry.extents.size() >= 3);
        return entry;
    };

    auto ep1_file = make_file("/TV/s01e01.mkv", 31);
    auto ep2_file = make_file("/TV/s01e02.mkv", 32);
    auto ep3_file = make_file("/TV/s02e01.mkv", 33);
    auto movie1_file = make_file("/Movies/one.mkv", 41);
    auto movie2_file = make_file("/Movies/two.mkv", 42);

    CatalogueItem show;
    show.id = "show:test";
    show.kind = CatalogueKind::show;
    show.title = "Test Show";
    show = service.catalogue().upsert(show);

    CatalogueItem season1;
    season1.id = "season:test:1";
    season1.kind = CatalogueKind::season;
    season1.title = "Season 1";
    season1.parent_id = show.id;
    season1.season_number = 1;
    season1 = service.catalogue().upsert(season1);

    CatalogueItem season2;
    season2.id = "season:test:2";
    season2.kind = CatalogueKind::season;
    season2.title = "Season 2";
    season2.parent_id = show.id;
    season2.season_number = 2;
    season2 = service.catalogue().upsert(season2);

    CatalogueItem ep1;
    ep1.id = "episode:test:1:1";
    ep1.kind = CatalogueKind::episode;
    ep1.title = "One";
    ep1.parent_id = season1.id;
    ep1.season_number = 1;
    ep1.episode_number = 1;
    ep1.media_ids = {file_media_id(ep1_file)};
    ep1 = service.catalogue().upsert(ep1);

    CatalogueItem ep2;
    ep2.id = "episode:test:1:2";
    ep2.kind = CatalogueKind::episode;
    ep2.title = "Two";
    ep2.parent_id = season1.id;
    ep2.season_number = 1;
    ep2.episode_number = 2;
    ep2.media_ids = {file_media_id(ep2_file)};
    ep2 = service.catalogue().upsert(ep2);

    CatalogueItem ep3;
    ep3.id = "episode:test:2:1";
    ep3.kind = CatalogueKind::episode;
    ep3.title = "Three";
    ep3.parent_id = season2.id;
    ep3.season_number = 2;
    ep3.episode_number = 1;
    ep3.media_ids = {file_media_id(ep3_file)};
    ep3 = service.catalogue().upsert(ep3);

    CatalogueItem movie1;
    movie1.id = "movie:test:1";
    movie1.kind = CatalogueKind::movie;
    movie1.title = "First Film";
    movie1.year = 2001;
    movie1.external_ids["collection"] = "test-films";
    movie1.media_ids = {"/Movies/one.mkv"};
    movie1 = service.catalogue().upsert(movie1);

    CatalogueItem movie2;
    movie2.id = "movie:test:2";
    movie2.kind = CatalogueKind::movie;
    movie2.title = "Second Film";
    movie2.year = 2003;
    movie2.external_ids["collection"] = "test-films";
    movie2.media_ids = {"path:/Movies/two.mkv"};
    movie2 = service.catalogue().upsert(movie2);

    HydrationConfig prediction_config;
    prediction_config.catalogue_lookahead = 1;
    PlaybackTracker prediction_tracker;
    CatalogueSequenceHintProvider predictor(prediction_tracker, service.filesystem(),
                                            service.catalogue(), prediction_config);

    auto check_prediction = [&](const std::string& path, const FsEntry& current,
                                const FsEntry& expected, const char* reason) {
        auto active = prediction_tracker.open(path, current);
        prediction_tracker.progress(active, 0);
        auto predicted = predictor.hints();
        REQUIRE(predicted.size() == 1);
        CHECK(predicted[0].reason == reason);
        REQUIRE(!predicted[0].objects.empty());
        CHECK(predicted[0].objects.front() == expected.extents.front().id);
        CHECK(predicted[0].objects.size() == expected.extents.size());
        prediction_tracker.close(active);
    };
    check_prediction("/TV/s01e01.mkv", ep1_file, ep2_file, "next_episode");
    check_prediction("/TV/s01e02.mkv", ep2_file, ep3_file, "next_episode");
    check_prediction("/Movies/one.mkv", movie1_file, movie2_file, "next_movie");
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
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c2.cache.path = temp.path() / "cache2";
    c2.cache.max_blocks = 32;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
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

    DistributedStore source(n1);
    DistributedStore target(n2);
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

MACHA_HEAVY_TEST("hydration_catalogue", test_media_probe_and_online_catalogue_scanner) {
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
             // release year. Found live on 2026-09-09: this file was unmatched
             // because "2049" was read as the year, the search title was
             // truncated to "Blade Runner", and the only candidate TMDB
             // returned -- the 1982 film -- was then rejected on the mismatch.
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
    MusicBrainzProvider mb(mb_http, mb_config);
    MediaProbe music_probe;
    music_probe.kind = MediaProbeKind::track;
    music_probe.artist = "Pink Floyd";
    music_probe.album = "The Dark Side of the Moon";
    music_probe.title = "Speak to Me";
    music_probe.track = 1;
    music_probe.media_id = "macha:test-track";
    auto mb_match = mb.lookup(music_probe);
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
    MusicBrainzProvider mb_recording(mb_recording_http, mb_config);
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
    MusicBrainzProvider mb_compilation(mb_compilation_http, mb_config);
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
    MusicBrainzProvider mb_miss(mb_miss_http, mb_config);
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
    MusicBrainzProvider mb_error(mb_error_http, mb_config);
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
    // The circuit opens after one 503 and Discogs receives the same candidate.
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

    // End-to-end scanner: resolve a real MachaDFS entry, fetch
    // poster/backdrop bytes, commit them with the catalogue, then prove a second
    // scan is idempotent and deletion removes only the scanner-owned item.
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

    // Music scanning is tag-first and provider-root scoped. Deliberately put a
    // tagged MP3 under misleading collection/grouping directories: embedded
    // metadata must win, while untagged filename fallback must not manufacture
    // an artist/album from those same directories.
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

    const auto untagged_bytes = fixture_bytes("untagged.mp3");

    FakeHttpClient music_probe_http;
    CatalogueMusicProviderConfig music_source_config;
    music_source_config.roots = {"/Music"};
    music_source_config.musicbrainz.enabled = false;
    MusicScanProvider music_source(music_probe_http, music_source_config);

    const std::string loose_path = singles + "/Scooter - The First Time (Raven Remix).mp3";
    write_fixture(loose_path, untagged_bytes);
    auto loose_entry = service.filesystem().getattr(loose_path);
    auto loose_probe = music_source.probe(service.filesystem(), "/Music", loose_path, loose_entry);
    REQUIRE(loose_probe.has_value());
    CHECK(loose_probe->artist == "Scooter");
    CHECK(loose_probe->album.empty());
    CHECK(loose_probe->title == "The First Time (Raven Remix)");

    const std::string nested_album = singles + "/13 - [1996] I'm Raving CDM";
    service.filesystem().mkdir(nested_album, 0755, getuid(), getgid());
    const std::string nested_path = nested_album + "/01 - I'm Raving.mp3";
    write_fixture(nested_path, untagged_bytes);
    auto nested_entry = service.filesystem().getattr(nested_path);
    auto nested_probe = music_source.probe(service.filesystem(), "/Music", nested_path, nested_entry);
    REQUIRE(nested_probe.has_value());
    CHECK(nested_probe->artist.empty());
    CHECK(nested_probe->album.empty());
    CHECK(nested_probe->track == 1);
    CHECK(nested_probe->title == "I'm Raving");

    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
    service.filesystem().create_file("/Movies/Blade.Runner.2049.2017.1080p.mkv", 0644,
                                     getuid(), getgid());
    auto bytes = pattern(32768);
    auto writer = service.filesystem().open_write(
        "/Movies/Blade.Runner.2049.2017.1080p.mkv", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    auto entry = service.filesystem().getattr(
        "/Movies/Blade.Runner.2049.2017.1080p.mkv");
    auto media_id = file_media_id(entry);

    auto scanner_token = t.path() / "scanner-tmdb.token";
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
    CatalogueScanner scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                             scanner_config, std::move(fake_http), 5s, profile_engine);
    const auto namespace_before_scan = service.filesystem().namespace_signature();
    CHECK(scanner.scan_once() == 1);
    CHECK(service.filesystem().namespace_signature() == namespace_before_scan);
    auto catalogued = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(catalogued.has_value());
    CHECK(catalogued->title == "Blade Runner 2049");
    CHECK(catalogued->external_ids.at("tmdb_collection") == "422837");
    CHECK(catalogued->external_ids.at("macha_scanner") == "1");
    CHECK(catalogued->media_ids == std::vector<std::string>{media_id});
    REQUIRE(service.catalogue().media_profile(media_id).has_value());
    CHECK(profile_engine->probes() == 1);
    CHECK(catalogued->artwork.size() == 2);
    for (const auto& art : catalogued->artwork)
        CHECK(service.node().local_store().has(art.id));
    auto revision = catalogued->revision;
    CHECK(scanner.scan_once() == 0);
    REQUIRE(service.catalogue().get("tmdb:movie:335984").has_value());
    CHECK(service.catalogue().get("tmdb:movie:335984")->revision == revision);

    // Manual metadata editing is authoritative. A later scanner discovery for
    // another local copy may add media bindings, but must not silently overwrite
    // the user's descriptive changes.
    auto manual = *service.catalogue().get("tmdb:movie:335984");
    manual.title = "Blade Runner Custom";
    manual.sort_title = manual.title;
    manual.synopsis = "A manually edited synopsis.";
    manual.external_ids["macha_metadata_locked"] = "1";
    auto manually_saved = service.catalogue().upsert(std::move(manual), revision);
    revision = manually_saved.revision;

    // A second file resolving to the same title adds another binding without
    // losing the already-bound media identity.
    const std::string alternate = "/Movies/Blade.Runner.2049.2017.Remux.mkv";
    service.filesystem().create_file(alternate, 0644, getuid(), getgid());
    auto alternate_bytes = pattern(32769);
    auto alternate_writer = service.filesystem().open_write(alternate, true);
    REQUIRE(alternate_writer->write(0, alternate_bytes) == alternate_bytes.size());
    alternate_writer->commit();
    CHECK(service.filesystem().namespace_signature() != namespace_before_scan);
    auto alternate_id = file_media_id(service.filesystem().getattr(alternate));
    CHECK(alternate_id != media_id);
    CHECK(scanner.scan_once() == 1);
    auto twice = service.catalogue().get("tmdb:movie:335984");
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
    // matched item. It also returns the exact immutable media identities that
    // became unbound, so recovery can enqueue only those files instead of
    // reopening every terminal/no-match hint in the library.
    auto cleared = service.catalogue().clear_metadata_with_media(
        "tmdb:movie:335984", twice->revision);
    CHECK(cleared.removed_items == 1);
    CHECK(cleared.media_ids.size() == 2);
    CHECK(std::find(cleared.media_ids.begin(), cleared.media_ids.end(), media_id) !=
          cleared.media_ids.end());
    CHECK(std::find(cleared.media_ids.begin(), cleared.media_ids.end(), alternate_id) !=
          cleared.media_ids.end());
    CHECK(!service.catalogue().get("tmdb:movie:335984").has_value());
    CHECK(scanner.request_media_rescan(cleared.media_ids) == 2);
    CHECK(service.catalogue_hints().summary().pending == 2);
    scanner.start();
    REQUIRE(wait_until([&] {
        auto item = service.catalogue().get("tmdb:movie:335984");
        return item && service.catalogue_hints().summary().pending == 0;
    }, 5s));
    scanner.stop();
    CHECK(std::filesystem::exists(service.node().config().state_path /
                                  "catalogue" / "scanner.state"));
    auto rematched = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(rematched.has_value());
    CHECK(rematched->title == "Blade Runner 2049");
    CHECK(rematched->synopsis == "A blade runner uncovers a long-buried secret.");
    CHECK(rematched->external_ids.at("tmdb") == "335984");
    CHECK(!rematched->external_ids.contains("macha_metadata_locked"));
    CHECK(rematched->artwork.size() == 2);
    CHECK(std::find(rematched->media_ids.begin(), rematched->media_ids.end(), media_id) != rematched->media_ids.end());
    CHECK(std::find(rematched->media_ids.begin(), rematched->media_ids.end(), alternate_id) != rematched->media_ids.end());

    // Deletion reconciles duplicate bindings one at a time and removes the
    // scanner-owned item only after the final copy goes.
    service.filesystem().unlink("/Movies/Blade.Runner.2049.2017.1080p.mkv");
    std::this_thread::sleep_for(config.metadata_cache + 20ms);
    CHECK(scanner.scan_once() == 0);
    auto partial = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(partial.has_value());
    CHECK(std::find(partial->media_ids.begin(), partial->media_ids.end(), media_id) != partial->media_ids.end());
    CHECK(std::find(partial->media_ids.begin(), partial->media_ids.end(), alternate_id) != partial->media_ids.end());

    // Once the previously unavailable root exists, the scan is complete and
    // destructive reconciliation may safely remove the vanished first binding.
    service.filesystem().mkdir("/Missing", 0755, getuid(), getgid());
    std::this_thread::sleep_for(config.metadata_cache + 20ms);
    CHECK(scanner.scan_once() == 0);
    auto remaining = service.catalogue().get("tmdb:movie:335984");
    REQUIRE(remaining.has_value());
    CHECK(remaining->media_ids == std::vector<std::string>{alternate_id});

    service.filesystem().unlink(alternate);
    std::this_thread::sleep_for(config.metadata_cache + 20ms);
    CHECK(service.filesystem().readdir("/Movies").empty());
    CHECK(scanner.scan_once() == 0);
    CHECK(!service.catalogue().get("tmdb:movie:335984").has_value());

    // Shutdown must not wait for a complete catalogue scan. request_stop()
    // propagates into the HTTP client so an in-flight provider request is
    // interrupted, and scan_once(stop) abandons the partial pass without a
    // reconciliation commit.
    const std::string shutdown_path = "/Movies/Shutdown.Test.2020.mkv";
    service.filesystem().create_file(shutdown_path, 0644, getuid(), getgid());
    auto shutdown_writer = service.filesystem().open_write(shutdown_path, true);
    auto shutdown_bytes = pattern(32769);
    REQUIRE(shutdown_writer->write(0, shutdown_bytes) == shutdown_bytes.size());
    shutdown_writer->commit();

    auto blocking_http = std::make_unique<BlockingHttpClient>();
    auto* blocking_http_ptr = blocking_http.get();
    auto cancel_config = scanner_config;
    cancel_config.movies.roots = {"/Movies"};
    CatalogueScanner cancel_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                    cancel_config, std::move(blocking_http));
    // Scanner startup is intentionally idle on an already-operated library.
    // Explicitly request the pass whose in-flight provider request this test
    // exercises, rather than depending on the old startup-rescan behaviour.
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
    service.filesystem().mkdir("/Budget", 0755, getuid(), getgid());
    const std::array<std::pair<const char*, uint8_t>, 3> budget_files{{
        {"/Budget/Budget.One.2020.mkv", 1},
        {"/Budget/Budget.Two.2021.mkv", 2},
        {"/Budget/Budget.Three.2022.mkv", 3},
    }};
    std::set<std::string> budget_media_ids;
    for (const auto& [path, marker] : budget_files) {
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto w = service.filesystem().open_write(path, true);
        REQUIRE(w->write(0, Bytes{marker, 2, 3, 4}) == 4);
        w->commit();
        budget_media_ids.insert(file_media_id(service.filesystem().getattr(path)));
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
    CatalogueScanner budget_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                    budget_config, std::move(budget_http));
    CHECK(budget_scanner.scan_once() == 2);
    CHECK(budget_http_ptr->requests() == 4);
    size_t first_batch_items = 0;
    for (const auto* id : {"tmdb:movie:2001", "tmdb:movie:2002", "tmdb:movie:2003"})
        if (service.catalogue().get(id).has_value()) ++first_batch_items;
    CHECK(first_batch_items == 2);
    CHECK(budget_scanner.scan_once() == 1);
    CHECK(budget_http_ptr->requests() == 6);
    CHECK(service.catalogue().get("tmdb:movie:2001").has_value());
    CHECK(service.catalogue().get("tmdb:movie:2002").has_value());
    CHECK(service.catalogue().get("tmdb:movie:2003").has_value());

    // Provider request budgeting is fair across media domains. A long run of
    // movie misses must not consume the whole batch before TV and Music get a
    // lookup opportunity.
    service.filesystem().mkdir("/FairMovies", 0755, getuid(), getgid());
    for (int i = 1; i <= 4; ++i) {
        const auto path = "/FairMovies/Fair.Movie." + std::to_string(i) + ".mkv";
        service.filesystem().create_file(path, 0644, getuid(), getgid());
        auto w = service.filesystem().open_write(path, true);
        REQUIRE(w->write(0, Bytes{static_cast<uint8_t>(i), 2, 3, 4}) == 4);
        w->commit();
    }
    service.filesystem().mkdir("/FairTV", 0755, getuid(), getgid());
    const std::string fair_tv = "/FairTV/Fair.Show.S01E01.mkv";
    service.filesystem().create_file(fair_tv, 0644, getuid(), getgid());
    auto fair_tv_writer = service.filesystem().open_write(fair_tv, true);
    REQUIRE(fair_tv_writer->write(0, Bytes{9, 8, 7, 6}) == 4);
    fair_tv_writer->commit();
    service.filesystem().mkdir("/FairMusic", 0755, getuid(), getgid());
    service.filesystem().mkdir("/FairMusic/Fair Artist", 0755, getuid(), getgid());
    service.filesystem().mkdir("/FairMusic/Fair Artist/Fair Album", 0755, getuid(), getgid());
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
    CatalogueScanner fair_scanner(service.node(), service.filesystem(), service.catalogue(), service.catalogue_hints(),
                                  fair_config, std::move(fair_http));
    CHECK(fair_scanner.scan_once() == 0);
    CHECK(fair_http_ptr->requests() == 3);
    CHECK(fair_http_ptr->requests_containing("/search/movie") == 1);
    CHECK(fair_http_ptr->requests_containing("/search/tv") == 1);
    CHECK(fair_http_ptr->requests_containing("/ws/2/release?") == 1);

    service.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_zero_length_files_wait_for_committed_content) {
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    auto& service = fixture.start();

    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
    const std::string path = "/Movies/Transient.Movie.2026.mkv";
    service.filesystem().create_file(path, 0644, getuid(), getgid());
    const auto empty_entry = service.filesystem().getattr(path);
    REQUIRE(empty_entry.type == EntryType::file);
    REQUIRE(empty_entry.size == 0);
    const auto empty_media_id = file_media_id(empty_entry);

    auto token = fixture.path() / "tmdb.token";
    {
        std::ofstream out(token);
        out << "test-token\n";
    }
    auto fake_http = std::make_unique<FakeHttpClient>();
    auto* fake_http_ptr = fake_http.get();
    fake_http->add("/search/movie", 200, "application/json",
                   R"({"results":[{"id":4242,"title":"Transient Movie","release_date":"2026-01-01"}]})");
    fake_http->add("/movie/4242", 200, "application/json",
                   R"({"id":4242,"title":"Transient Movie","release_date":"2026-01-01"})");

    CatalogueScannerConfig scanner_config;
    scanner_config.enabled = true;
    scanner_config.movies.roots = {"/Movies"};
    scanner_config.movies.tmdb.token_file = token;
    scanner_config.tv.enabled = false;
    scanner_config.music.enabled = false;
    CatalogueScanner scanner(service.node(), service.filesystem(), service.catalogue(),
                             service.catalogue_hints(), scanner_config, std::move(fake_http));

    // Discovery must not manufacture one shared immutable identity for every
    // zero-length namespace shell, queue provider work, or contact TMDB.
    CHECK(scanner.scan_once() == 0);
    CHECK(service.catalogue_hints().summary().total == 0);
    CHECK(fake_http_ptr->requests() == 0);

    // A hint admitted just before the namespace becomes visible can still race
    // with publication. It must remain pending rather than becoming a durable
    // semantic no-match for a file whose content has not committed yet.
    const auto hint_id = service.catalogue_hints().submit(
        path, "namespace", empty_media_id, CatalogueHintPriority::namespace_mutation);
    CHECK(scanner.scan_once() == 0);
    auto waiting = service.catalogue_hints().get(hint_id);
    REQUIRE(waiting.has_value());
    CHECK(waiting->state == CatalogueHintState::deferred);
    CHECK(waiting->result.empty());
    CHECK(waiting->error == "namespace media file has no committed content yet");
    CHECK(fake_http_ptr->requests() == 0);

    // Once the real extent manifest commits, the changed media identity reopens
    // the same path and normal provider matching proceeds immediately.
    auto writer = service.filesystem().open_write(path, true);
    auto bytes = pattern(32768);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto committed_entry = service.filesystem().getattr(path);
    REQUIRE(committed_entry.size == bytes.size());
    const auto committed_media_id = file_media_id(committed_entry);
    CHECK(committed_media_id != empty_media_id);

    CHECK(scanner.scan_once() == 1);
    auto item = service.catalogue().get("tmdb:movie:4242");
    REQUIRE(item.has_value());
    CHECK(item->media_ids == std::vector<std::string>{committed_media_id});
    CHECK(fake_http_ptr->requests() == 2);
}

MACHA_TEST("hydration_catalogue", test_terminal_media_profile_job_is_not_requeued_forever) {
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    auto& service = fixture.start();

    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
    const std::string path = "/Movies/Profile.Failure.2026.mp4";
    service.filesystem().create_file(path, 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(path, true);
    auto bytes = pattern(32771);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto media_id = file_media_id(service.filesystem().getattr(path));

    auto token = fixture.path() / "tmdb.token";
    {
        std::ofstream out(token);
        out << "test-token\n";
    }
    CatalogueScannerConfig scanner_config;
    scanner_config.enabled = true;
    scanner_config.movies.roots = {"/Movies"};
    scanner_config.movies.tmdb.token_file = token;
    scanner_config.tv.enabled = false;
    scanner_config.music.enabled = false;
    auto profile_engine = std::make_shared<FakeMediaEngine>();
    CatalogueScanner scanner(service.node(), service.filesystem(), service.catalogue(),
                             service.catalogue_hints(), scanner_config,
                             std::make_unique<FakeHttpClient>(), 5s, profile_engine);

    CHECK(scanner.request_media_profiles({media_id}) == 1);
    auto hints = service.catalogue_hints().list();
    REQUIRE(hints.size() == 1);
    CHECK(hints.front().state == CatalogueHintState::queued);
    service.catalogue_hints().fail(hints.front().id, "synthetic_failure", "synthetic profile failure");

    // A retry observes the terminal result instead of reopening the same
    // immutable profile job and reporting an endless pending state. Playback
    // interprets zero as permission to use its bounded media-engine fallback.
    CHECK(scanner.request_media_profiles({media_id}) == 0);
    auto terminal = service.catalogue_hints().get(hints.front().id);
    REQUIRE(terminal.has_value());
    CHECK(terminal->state == CatalogueHintState::failed);
    CHECK(!service.catalogue().media_profile(media_id).has_value());

    service.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_cache_ignores_unrelated_metadata_generation) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "node", cluster.keyfile(), free_port());
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    NodeRuntime node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);

    CatalogueItem item;
    item.id = "test:movie:1";
    item.kind = CatalogueKind::movie;
    item.title = "Cached Movie";
    auto committed = catalogue.upsert(item);
    CHECK(committed.title == "Cached Movie");

    auto status = catalogue.status();
    REQUIRE(status.root.has_value());
    REQUIRE(node.control_store().remove(*status.root));

    // Advance ordinary filesystem metadata without changing catalogue_root.
    // The warm immutable catalogue must remain usable even though its local
    // CONTROL manifest has deliberately been made unavailable.
    metadata.mutate([](MetadataSnapshot& snapshot) {
        auto root = snapshot.entries.find("/");
        REQUIRE(root != snapshot.entries.end());
        ++root->second.version;
        ++root->second.mtime_ns;
    });

    auto cached = catalogue.get("test:movie:1");
    REQUIRE(cached.has_value());
    CHECK(cached->title == "Cached Movie");
    // The global metadata generation is newer, but the decoded immutable view
    // proves that catalogue_root did not change. A missing-item mutation can
    // therefore return 404 without entering quorum repair.
    CHECK(catalogue.definitely_absent("test:movie:missing"));

    // CONTROL durability is stronger than API-cache availability. Background
    // convergence must notice that the authoritative manifest is missing and
    // fail closed, while the already-loaded immutable snapshot remains usable.
    bool repair_failed = false;
    try {
        catalogue.repair_once();
    } catch (const CatalogueUnavailable&) {
        repair_failed = true;
    }
    CHECK(repair_failed);
    auto after_repair = catalogue.get("test:movie:1");
    REQUIRE(after_repair.has_value());
    CHECK(after_repair->title == "Cached Movie");

    node.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_scanner_restart_does_not_rescan_unchanged_namespace) {
    TestService fixture("store");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false; // explicit scanner below
    auto& service = fixture.start();

    service.filesystem().mkdir("/Movies", 0755, getuid(), getgid());
    service.filesystem().create_file("/Movies/Unbound.2026.mkv", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/Movies/Unbound.2026.mkv", true);
    auto bytes = pattern(4096);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();

    // Simulate an upgrade from the pre-scanner.state queue implementation. The
    // durable hint-state file remains even when every historical ephemeral hint
    // has been discarded. Restart/coordinator election must seed scanner.state
    // from the current immutable namespace and wait for the normal safety
    // interval; it must not interpret process start as a reason to walk /Movies.
    std::filesystem::create_directories(config.state_path / "catalogue");
    {
        std::ofstream out(config.state_path / "catalogue" / "hints.json");
        out << R"({"version":2,"hints":[]})";
    }

    auto token = fixture.path() / "tmdb.token";
    {
        std::ofstream out(token);
        out << "test-token\n";
    }
    CatalogueScannerConfig scanner_config;
    scanner_config.enabled = true;
    scanner_config.interval = 1h;
    scanner_config.rescan_debounce = 50ms;
    scanner_config.rescan_max_delay = 1s;
    scanner_config.movies.roots = {"/Movies"};
    scanner_config.movies.tmdb.token_file = token;
    scanner_config.tv.enabled = false;
    scanner_config.music.enabled = false;

    auto fake_http = std::make_unique<FakeHttpClient>();
    auto* fake_http_ptr = fake_http.get();
    CatalogueScanner scanner(service.node(), service.filesystem(), service.catalogue(),
                             service.catalogue_hints(), scanner_config, std::move(fake_http));
    scanner.start();
    REQUIRE(wait_until([&] {
        return std::filesystem::exists(config.state_path / "catalogue" / "scanner.state");
    }, 1s));
    std::this_thread::sleep_for(80ms);
    CHECK(service.catalogue_hints().summary().total == 0);
    CHECK(fake_http_ptr->requests() == 0);
    scanner.stop();
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
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;

    Service s1(c1, keys);
    Service s2(c2, keys);
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
    CatalogueScanner scanner(non_coordinator->node(), non_coordinator->filesystem(),
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

// A node with no torrent plugin loaded -- not built, not installed, or
// faulted and between restarts -- must answer honestly rather than assuming
// the engine is there. This needs no plugin at all, which is the point.
MACHA_TEST("hydration_catalogue", test_acquisition_api_without_a_torrent_plugin_reports_it_absent) {
    TestNode fixture("torrent-absent");
    fixture.prepare();
    fixture.start();

    CatalogueHintQueue hints(fixture.config().state_path / "catalogue-hints");
    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    IngestManager ingest(fixture.node(), fixture.filesystem(), hints, ingest_config);

    TorrentConfig torrent_config;
    torrent_config.enabled = true; // configured on, but nothing provides it.
    TorrentSearchManager search(torrent_config);
    SubsystemRegistry registry;
    AcquisitionApi acquisition(ingest, registry, search);

    HttpRequest status_request;
    status_request.method = "GET";
    status_request.path = "/api/v1/torrents/status";
    const auto status = acquisition.handle(status_request);
    REQUIRE(status.status == 200);
    const auto status_body =
        Json::parse(std::string(status.body.begin(), status.body.end()));
    CHECK(status_body.find("build_available")->asBool() == false);
    CHECK(status_body.find("enabled")->asBool() == false);

    // Every endpoint that needs the engine says so, rather than crashing on a
    // null capability or pretending the job list is empty.
    for (const auto& [method, path] :
         std::vector<std::pair<std::string, std::string>>{
             {"GET", "/api/v1/torrents/jobs"},
             {"POST", "/api/v1/torrents/jobs"},
             {"GET", "/api/v1/torrents/jobs/whatever"},
             {"POST", "/api/v1/torrents/jobs/whatever/pause"}}) {
        HttpRequest request;
        request.method = method;
        request.path = path;
        const auto response = acquisition.handle(request);
        CHECK(response.status == 503);
    }

    // Search is core's own Torznab client, so it stays available.
    HttpRequest search_request;
    search_request.method = "GET";
    search_request.path = "/api/v1/torrents/search";
    CHECK(acquisition.handle(search_request).status == 400); // missing q, not 503.
}

// Only meaningful in a build that produced the plugin: without libtorrent
// there is no download engine to load, and the capability is absent by
// design rather than differently compiled.
#ifdef MACHA_TEST_TORRENT_PLUGIN
MACHA_TEST("hydration_catalogue", test_torrent_jobs_carry_their_info_hash_and_search_results_can_be_placed) {
    // 0.58.2. info_hash was persisted and serialised but never set, so every
    // job reported null; a job saved without one is backfilled from its magnet
    // on load. And a search result's URI now reaches add_search_result, which
    // may fetch a provider's .torrent URL; until now every placement went
    // through add(), magnets only, and such a result could never be started.
    TestNode fixture("torrent-identity");
    fixture.prepare();
    const auto state_path = fixture.config().state_path;
    const auto staging_path = fixture.path() / "staging";
    const auto payload = staging_path / "torrents" / "torrent-done";
    std::filesystem::create_directories(payload);
    std::filesystem::create_directories(state_path / "torrent");
    {
        Json::Object done;
        done["id"] = "torrent-done";
        done["name"] = "Finished Movie";
        done["source_uri"] = "magnet:?xt=urn:btih:3333333333333333333333333333333333333333";
        done["save_path"] = payload.string();
        done["state"] = "completed";
        done["created_unix_ms"] = static_cast<uint64_t>(1);
        done["updated_unix_ms"] = static_cast<uint64_t>(2);
        done["error"] = "";
        Json::Array jobs;
        jobs.emplace_back(std::move(done));
        Json::Object root;
        root["version"] = static_cast<uint64_t>(1);
        root["jobs"] = std::move(jobs);
        std::ofstream out(state_path / "torrent" / "jobs.json", std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << Json(std::move(root)).dump();
    }
    fixture.start();

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
    Config plugin_config = fixture.node().config();
    plugin_config.torrent = torrent_config;
    plugin_config.state_path = state_path;
    SubsystemRegistry registry;
    SubsystemContext context;
    context.config = &plugin_config;
    context.node = &fixture.node();
    context.ingest = &ingest;
    context.registry = &registry;
    LoadedTorrentPlugin plugin(context);
    auto torrents_owner = registry.torrent();
    REQUIRE(torrents_owner);
    auto& torrents = *torrents_owner;

    auto done = torrents.job("torrent-done");
    REQUIRE(done.has_value());
    CHECK(done->info_hash == "3333333333333333333333333333333333333333");

    plugin.subsystem().start();
    const std::string url = "https://127.0.0.1:1/result.torrent";
    auto as_magnet = torrents.add_on(NodeId{}, url, false);
    CHECK(!as_magnet.placed);
    CHECK(as_magnet.error.find("requires a magnet") != std::string::npos);
    auto as_result = torrents.add_on(NodeId{}, url, true);
    CHECK(!as_result.placed);
    CHECK(as_result.error.find("requires a magnet") == std::string::npos);
    plugin.subsystem().stop();
}

MACHA_TEST("hydration_catalogue", test_torrent_failed_ingest_retry_and_pause_intent) {
    TestNode fixture("torrent-recovery");
    fixture.prepare();

    const auto state_path = fixture.config().state_path;
    const auto staging_path = fixture.path() / "staging";
    const auto retry_payload = staging_path / "torrents" / "torrent-retry";
    const auto pause_payload = staging_path / "torrents" / "torrent-pause";
    std::filesystem::create_directories(retry_payload);
    std::filesystem::create_directories(pause_payload);
    std::filesystem::create_directories(state_path / "ingest");
    std::filesystem::create_directories(state_path / "torrent");

    {
        Json::Object job;
        job["id"] = "ingest-retry";
        job["source_type"] = "torrent";
        job["source_ref"] = "torrent-retry";
        job["display_name"] = "Retry Movie";
        job["source_path"] = retry_payload.string();
        job["source_owned"] = true;
        job["delete_source_on_clear"] = true;
        job["state"] = "failed";
        job["bytes_total"] = static_cast<uint64_t>(1234);
        job["bytes_completed"] = static_cast<uint64_t>(1234);
        job["files_total"] = static_cast<uint64_t>(1);
        job["files_completed"] = static_cast<uint64_t>(1);
        job["created_unix_ms"] = static_cast<uint64_t>(1);
        job["updated_unix_ms"] = static_cast<uint64_t>(2);
        job["error"] = "metadata quorum unavailable";
        job["files"] = Json::Array{};
        Json::Array jobs;
        jobs.emplace_back(std::move(job));
        Json::Object root;
        root["version"] = static_cast<uint64_t>(2);
        root["jobs"] = std::move(jobs);
        std::ofstream out(state_path / "ingest" / "jobs.json", std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << Json(std::move(root)).dump();
        REQUIRE(out.good());
    }

    {
        Json::Array jobs;
        Json::Object retry;
        retry["id"] = "torrent-retry";
        retry["name"] = "Retry Movie";
        retry["source_uri"] = "magnet:?xt=urn:btih:1111111111111111111111111111111111111111";
        retry["info_hash"] = "1111111111111111111111111111111111111111";
        retry["save_path"] = retry_payload.string();
        retry["state"] = "failed";
        retry["bytes_total"] = static_cast<uint64_t>(1234);
        retry["bytes_completed"] = static_cast<uint64_t>(1234);
        retry["ingest_job_id"] = "ingest-retry";
        retry["created_unix_ms"] = static_cast<uint64_t>(1);
        retry["updated_unix_ms"] = static_cast<uint64_t>(2);
        retry["error"] = "ingest failed: metadata quorum unavailable";
        jobs.emplace_back(std::move(retry));

        Json::Object pause;
        pause["id"] = "torrent-pause";
        pause["name"] = "Pause Movie";
        pause["source_uri"] = "magnet:?xt=urn:btih:2222222222222222222222222222222222222222";
        pause["info_hash"] = "2222222222222222222222222222222222222222";
        pause["save_path"] = pause_payload.string();
        pause["state"] = "queued";
        pause["created_unix_ms"] = static_cast<uint64_t>(1);
        pause["updated_unix_ms"] = static_cast<uint64_t>(2);
        pause["error"] = "";
        jobs.emplace_back(std::move(pause));

        Json::Object root;
        root["version"] = static_cast<uint64_t>(1);
        root["jobs"] = std::move(jobs);
        std::ofstream out(state_path / "torrent" / "jobs.json", std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << Json(std::move(root)).dump();
        REQUIRE(out.good());
    }

    fixture.start();

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

    // The download engine is a plugin as of 0.28.0, so this loads the real
    // libmacha-torrent.so through the real entry symbol rather than linking
    // TorrentManager into the test binary. The Subsystem is driven directly
    // instead of through a SubsystemSupervisor because this case depends on
    // acting on state restored from jobs.json *before* the polling worker
    // starts, which a supervisor (which starts it immediately) would not
    // allow.
    Config plugin_config = fixture.node().config();
    plugin_config.torrent = torrent_config;
    plugin_config.state_path = state_path;
    SubsystemRegistry registry;
    SubsystemContext context;
    context.config = &plugin_config;
    context.node = &fixture.node();
    context.ingest = &ingest;
    context.registry = &registry;
    LoadedTorrentPlugin plugin(context);
    auto torrents_owner = registry.torrent();
    REQUIRE(torrents_owner);
    auto& torrents = *torrents_owner;

    auto failed_ingest = ingest.job("ingest-retry");
    REQUIRE(failed_ingest.has_value());
    CHECK(failed_ingest->state == IngestJobState::failed);

    TorrentSearchManager search(torrent_config);
    AcquisitionApi acquisition(ingest, registry, search);
    HttpRequest retry_request;
    retry_request.method = "POST";
    retry_request.path = "/api/v1/torrents/jobs/torrent-retry/retry";
    const auto retry_response = acquisition.handle(retry_request);
    REQUIRE(retry_response.status == 200);
    const auto retry_body = Json::parse(
        std::string(retry_response.body.begin(), retry_response.body.end()));
    CHECK(retry_body.find("state")->asString() == "importing");
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

    REQUIRE(torrents.pause("torrent-pause"));
    auto paused = torrents.job("torrent-pause");
    REQUIRE(paused.has_value());
    CHECK(paused->state == TorrentJobState::paused);

    // start() restores a live paused handle and the worker immediately
    // samples it. The explicit Macha pause must remain authoritative even if
    // libtorrent still reports its pre-pause state.
    plugin.subsystem().start();
    std::this_thread::sleep_for(750ms);
    paused = torrents.job("torrent-pause");
    REQUIRE(paused.has_value());
    CHECK(paused->state == TorrentJobState::paused);
    plugin.subsystem().stop();
}
#endif // MACHA_TEST_TORRENT_PLUGIN

MACHA_TEST("hydration_catalogue", test_ingest_catalogue_feedback_and_external_clear_cleanup) {
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.scanner.movies.enabled = true;
    config.catalogue.scanner.movies.roots = {"/Movies"};
    config.catalogue.scanner.tv.enabled = false;
    config.catalogue.scanner.music.enabled = false;
    config.ingest.enabled = false; // use the explicit manager below

    auto& service = fixture.start();

    const auto source_root = fixture.path() / "external-import";
    std::filesystem::create_directories(source_root);
    const auto media = source_root / "Queue Test Movie 2024.mkv";
    const auto unrelated = source_root / "do-not-delete.txt";
    {
        std::ofstream out(media, std::ios::binary);
        auto bytes = pattern(512 * 1024);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::ofstream out(unrelated);
        out << "external source material not selected for ingest\n";
    }

    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    ingest_config.source_roots = {source_root};
    ingest_config.copy_chunk_bytes = 64 * 1024;
    ingest_config.checkpoint_bytes = 256 * 1024;
    ingest_config.delete_external_source_on_clear = true;
    IngestManager ingest(service.node(), service.filesystem(), service.catalogue_hints(),
                         ingest_config);
    ingest.start();
    const auto job_id = ingest.submit_path(source_root);

    REQUIRE(wait_until([&] {
        auto job = ingest.job(job_id);
        return job && job->state == IngestJobState::cataloguing;
    }, 10s));
    auto summary = service.catalogue_hints().summary("ingest", job_id);
    REQUIRE(summary.total == 1);
    REQUIRE(summary.pending == 1);
    auto hint = service.catalogue_hints().claim_next();
    REQUIRE(hint.has_value());
    CHECK(hint->origins.size() == 1);
    service.catalogue_hints().mark_catalogued(
        hint->id, "movies", "macha:test-ingest-media", {"test:movie:queue"}, "synthetic match");

    REQUIRE(wait_until([&] {
        auto job = ingest.job(job_id);
        return job && job->state == IngestJobState::completed;
    }, 5s));
    auto completed = ingest.job(job_id);
    REQUIRE(completed.has_value());
    CHECK(completed->catalogue_total == 1);
    CHECK(completed->catalogue_pending == 0);
    CHECK(completed->catalogue_catalogued == 1);
    CHECK(completed->catalogue_no_match == 0);
    CHECK(completed->catalogue_failed == 0);

    REQUIRE(ingest.clear(job_id));
    CHECK(!ingest.job(job_id).has_value());
    CHECK(!std::filesystem::exists(media));
    CHECK(std::filesystem::exists(unrelated));
    CHECK(std::filesystem::exists(source_root));
    CHECK(service.catalogue_hints().summary("ingest", job_id).total == 0);

    ingest.stop();
}

MACHA_TEST("hydration_catalogue", test_cleared_ingest_job_does_not_resurrect_while_worker_finishes) {
    // A job the worker is still inside process_job() for can reach a terminal
    // state (cancel() writes it directly, regardless of whether the worker
    // has noticed yet) and be clear()-ed by a client while the worker is
    // still mid-copy. The worker's own per-iteration control checks used to
    // read state via jobs_[job.id], which default-constructs and re-inserts
    // a fresh "queued" job if clear() already erased it -- silently
    // resurrecting a job the operator just removed. Those checks now use
    // find() and treat "already gone" the same as cancelled.
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false;

    auto& service = fixture.start();

    const auto source_root = fixture.path() / "external-import";
    std::filesystem::create_directories(source_root);
    const auto media = source_root / "Slow Import Movie 2024.mkv";
    {
        std::ofstream out(media, std::ios::binary);
        auto bytes = pattern(32 * 1024 * 1024);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    ingest_config.source_roots = {source_root};
    // Small enough that the copy loop takes many real iterations, giving the
    // test a wide window to cancel+clear while the worker is still inside
    // copy_file()'s per-chunk loop.
    ingest_config.copy_chunk_bytes = 256;
    ingest_config.checkpoint_bytes = 4096;
    IngestManager ingest(service.node(), service.filesystem(), service.catalogue_hints(),
                         ingest_config);
    ingest.start();
    const auto job_id = ingest.submit_path(source_root);

    REQUIRE(wait_until([&] {
        auto job = ingest.job(job_id);
        return job && job->state == IngestJobState::importing;
    }, 10s));

    // cancel() writes the terminal state into the map immediately, without
    // waiting for the worker to notice -- exactly the window that used to
    // let clear() erase the job out from under a still-running worker.
    REQUIRE(ingest.cancel(job_id));
    REQUIRE(ingest.clear(job_id));
    CHECK(!ingest.job(job_id).has_value());

    // Give the worker's still-in-flight process_job() every chance to finish
    // its current chunk, hit one of the per-iteration control checks, and
    // (pre-fix) resurrect the job. It must stay gone.
    std::this_thread::sleep_for(200ms);
    CHECK(!ingest.job(job_id).has_value());
    REQUIRE(wait_until([&] { return !ingest.job(job_id).has_value(); }, 2s));
    CHECK(ingest.jobs().empty());

    ingest.stop();
    CHECK(!ingest.job(job_id).has_value());
}

namespace {

// Builds `count` single-file import roots under `base` and returns them.
std::vector<std::filesystem::path> make_import_roots(const std::filesystem::path& base,
                                                     size_t count, size_t file_bytes) {
    std::vector<std::filesystem::path> roots;
    for (size_t i = 0; i < count; ++i) {
        auto root = base / ("concurrent-import-" + std::to_string(i));
        std::filesystem::create_directories(root);
        std::ofstream out(root / ("Concurrent Movie " + std::to_string(i) + " 2024.mkv"),
                          std::ios::binary);
        auto bytes = pattern(file_bytes);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
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

} // namespace

namespace {

// A source root holding one file whose bytes are `bytes`, and an extent
// journal beside it recording the extents a torrent's disk backend would
// have published. When `store_them` is false the journal names objects that
// were never stored.
struct PublishedSource {
    std::filesystem::path root;
    std::string name;
    std::vector<ObjectId> ids;
};

PublishedSource make_published_source(TestService& fixture, Service& service, const Bytes& bytes,
                                      bool store_them) {
    PublishedSource out;
    out.root = fixture.path() / (store_them ? "published-import" : "unpublished-import");
    out.name = "Published Movie 2024.mkv";
    std::filesystem::create_directories(out.root);
    std::ofstream(out.root / out.name, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto extent_size = fixture.config().extent_size;
    TorrentExtentJournal journal(out.root);
    for (uint64_t offset = 0; offset < bytes.size(); offset += extent_size) {
        const auto length = std::min<uint64_t>(extent_size, bytes.size() - offset);
        const std::span<const uint8_t> slice(bytes.data() + offset, static_cast<size_t>(length));
        ObjectId id;
        if (store_them) {
            id = service.filesystem().store().put(slice, FrameType::loader);
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

} // namespace

MACHA_TEST("hydration_catalogue", test_ingest_commits_published_torrent_extents_without_copying) {
    // Stage 2 of the torrent disk backend: a torrent publishes each extent as
    // its pieces verify and records it in the job's extent journal. The ingest
    // then commits the file by naming those extents -- the committed manifest
    // is exactly the journal's -- instead of copying the bytes a second time.
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false;
    auto& service = fixture.start();

    const auto bytes = pattern(2 * fixture.config().extent_size + 1000);
    const auto source = make_published_source(fixture, service, bytes, true);

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
    // The journal is not media and is not imported.
    REQUIRE(job->files.size() == 1);
    const auto destination = job->files.front().destination_path;
    const auto entry = service.filesystem().getattr(destination);
    CHECK(entry.size == bytes.size());
    REQUIRE(entry.extents.size() == source.ids.size());
    for (size_t i = 0; i < source.ids.size(); ++i) CHECK(entry.extents[i].id == source.ids[i]);
    CHECK(read_whole(service.filesystem(), destination, bytes.size()) == bytes);
    ingest.stop();
}

MACHA_TEST("hydration_catalogue", test_ingest_copies_when_published_extents_are_missing) {
    // The journal is a claim, not proof. A manifest naming objects the store
    // does not hold must never be committed: the ingest falls back to copying,
    // and the file holds the real bytes.
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false;
    auto& service = fixture.start();

    const auto bytes = pattern(2 * fixture.config().extent_size + 1000);
    const auto source = make_published_source(fixture, service, bytes, false);

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

MACHA_TEST("hydration_catalogue", test_ingest_runs_jobs_concurrently_up_to_the_configured_bound) {
    // Ingest was strictly serial: one worker thread taking one job at a time
    // (loop() -> process_job() synchronously), so a single job that could not
    // finish held every other job at "queued". That is how a metadata stall on
    // es-1 (2026-09-10) surfaced -- six torrent imports all reading "queued"
    // with nothing visibly running. The pool is now bounded by
    // ingest.max_concurrent_jobs.
    //
    // peak_active_jobs() is a monotonic high-water mark recorded by the workers
    // at claim time, so this proves real overlap without a poller having to
    // catch the moment -- which is what would have made it load-sensitive.
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false; // use the explicit manager below

    auto& service = fixture.start();

    constexpr size_t job_count = 6;
    constexpr size_t bound = 3;
    const auto roots = make_import_roots(fixture.path(), job_count, 4 * 1024 * 1024);

    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    ingest_config.source_roots = roots;
    // Small chunks so each copy takes many real iterations, widening the
    // window in which jobs genuinely overlap -- but a large checkpoint, so
    // that costs loop iterations rather than a metadata commit per chunk.
    ingest_config.copy_chunk_bytes = 4096;
    ingest_config.checkpoint_bytes = 1024 * 1024;
    ingest_config.max_concurrent_jobs = bound;
    IngestManager ingest(service.node(), service.filesystem(), service.catalogue_hints(),
                         ingest_config);

    // Submit before start() so the pool wakes to an already-full queue: every
    // worker finds claimable work immediately instead of racing the submits.
    std::vector<std::string> ids;
    for (const auto& root : roots) ids.push_back(ingest.submit_path(root));
    CHECK(ingest.active_jobs() == 0);
    ingest.start();

    // Wait for the overlap rather than sampling for it after the fact: on a
    // loaded machine the workers start staggered, and asserting at the end
    // measures whether the last job happened to still be running, not whether
    // the pool is concurrent. The high-water mark is monotonic, so waiting on
    // it is exact -- it only ever reports overlap that genuinely happened.
    REQUIRE(wait_until([&] { return ingest.peak_active_jobs() >= 2; }, 60s));

    REQUIRE(wait_until([&] { return all_imports_copied(ingest, ids); }, 60s));

    // The bound is the contract -- never more claimed at once than configured.
    CHECK(ingest.peak_active_jobs() <= bound);

    ingest.stop();
    CHECK(ingest.active_jobs() == 0);
}

MACHA_TEST("hydration_catalogue", test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool) {
    // The pool made job ownership a set rather than one active_job_id_, and
    // cancel() decides whether cleanup is its own responsibility or the owning
    // worker's by consulting exactly that. Prove the control surface the API
    // exposes -- show/pause/resume/cancel/clear -- still behaves per job while
    // several jobs are in flight, and that pausing one does not stall the rest.
    TestService fixture("node");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.ingest.enabled = false;

    auto& service = fixture.start();

    constexpr size_t job_count = 4;
    const auto roots = make_import_roots(fixture.path(), job_count, 2 * 1024 * 1024);

    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = fixture.path() / "staging";
    ingest_config.source_roots = roots;
    // Many copy iterations per file so there is a wide window to pause one
    // mid-flight, without a metadata commit per chunk.
    ingest_config.copy_chunk_bytes = 4096;
    ingest_config.checkpoint_bytes = 1024 * 1024;
    ingest_config.max_concurrent_jobs = job_count;
    IngestManager ingest(service.node(), service.filesystem(), service.catalogue_hints(),
                         ingest_config);

    std::vector<std::string> ids;
    for (const auto& root : roots) ids.push_back(ingest.submit_path(root));
    ingest.start();

    // Wait until the first job is genuinely mid-copy before touching it.
    const auto& paused_id = ids.front();
    REQUIRE(wait_until([&] {
        auto job = ingest.job(paused_id);
        return job && job->state == IngestJobState::importing;
    }, 30s));

    REQUIRE(ingest.pause(paused_id));
    REQUIRE(wait_until([&] {
        auto job = ingest.job(paused_id);
        return job && job->state == IngestJobState::paused;
    }, 10s));

    // A paused job must stay paused -- no worker in the pool may pick it up.
    std::this_thread::sleep_for(200ms);
    auto held = ingest.job(paused_id);
    REQUIRE(held.has_value());
    CHECK(held->state == IngestJobState::paused);

    // Cancelling a different in-flight job must not disturb the others.
    const auto& cancelled_id = ids.back();
    REQUIRE(ingest.cancel(cancelled_id));
    REQUIRE(wait_until([&] {
        auto job = ingest.job(cancelled_id);
        return job && job->state == IngestJobState::cancelled;
    }, 10s));

    // The remaining untouched jobs still finish while one is paused and one
    // cancelled -- i.e. neither one is holding the queue.
    const std::vector<std::string> untouched(ids.begin() + 1, ids.end() - 1);
    REQUIRE(wait_until([&] { return all_imports_copied(ingest, untouched); }, 60s));

    // Resume puts the paused job back in the queue and it completes its copy.
    REQUIRE(ingest.resume(paused_id));
    REQUIRE(wait_until([&] { return all_imports_copied(ingest, {paused_id}); }, 60s));

    // clear() is the API's delete: terminal jobs go, and stay gone.
    REQUIRE(ingest.clear(cancelled_id));
    CHECK(!ingest.job(cancelled_id).has_value());

    ingest.stop();
    CHECK(ingest.active_jobs() == 0);
}

MACHA_TEST("hydration_catalogue", test_catalogue_warm_read_defers_remote_refresh) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto c1 = config_for(cluster.path() / "catalogue-live-1", cluster.keyfile(), free_port());
    auto c2 = config_for(cluster.path() / "catalogue-live-2", cluster.keyfile(), free_port(),
                         {{"127.0.0.1", c1.port}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.metadata_cache = c2.metadata_cache = 30ms;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);

    // Form the initial namespace on the bootstrap-less founder before starting
    // the joiner. A configured joiner is intentionally forbidden from inventing
    // genesis while its bootstrap peer has not yet entered active membership.
    n1.start();
    REQUIRE(n1.wait_local_state_ready(10s));
    DistributedStore store1(n1);
    MetadataManager metadata1(n1);
    CatalogueManager catalogue1(n1, store1, metadata1);

    CatalogueItem first;
    first.id = "test:movie:remote-first";
    first.kind = CatalogueKind::movie;
    first.title = "Remote First";
    first = catalogue1.upsert(first);

    n2.start();
    REQUIRE(n2.wait_local_state_ready(10s));
    DistributedStore store2(n2);
    MetadataManager metadata2(n2);
    CatalogueManager catalogue2(n2, store2, metadata2);

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
    FileSystem fs2(n2, store2, metadata2);
    FuseConfig fuse_config;
    fuse_config.commit_workers = 1;
    auto frontend = std::make_shared<FuseFrontend>(fs2, fuse_config);
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
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.metadata_cache = c2.metadata_cache = 30ms;
    // Keep ordinary heartbeat propagation outside this test window. We install a
    // valid newer committed metadata head directly to simulate a generation notice
    // that was missed by node two; TTL validation must still discover it from replicas.
    c1.heartbeat = c2.heartbeat = 5s;
    c1.dead_after = c2.dead_after = 20s;

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);

    // Establish genesis on the founder first. The joiner may legitimately reject
    // metadata reads with "waiting for bootstrap peer" during the brief interval
    // between start() and membership convergence, so retry its initial read rather
    // than turning that expected bootstrap state into an unhandled test failure.
    n1.start();
    REQUIRE(n1.wait_local_state_ready(10s));
    MetadataManager metadata1(n1);
    const auto initial1 = metadata1.snapshot_view();
    n2.start();
    REQUIRE(n2.wait_local_state_ready(10s));
    MetadataManager metadata2(n2);

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
    std::this_thread::sleep_for(c2.metadata_cache + 20ms);
    REQUIRE(n2.known_metadata_generation() < next.generation);

    // Expiry must force a real metadata read, discover the newer committed head and
    // replace the decoded snapshot. cached_snapshot_view() must not ignore
    // cache_until_ and this remained stale indefinitely without a notice.
    auto refreshed = metadata2.snapshot_view();
    CHECK(refreshed.generation == next.generation);
    CHECK(n2.known_metadata_generation() >= next.generation);

    n2.stop();
    n1.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_effective_music_artwork_resolution) {
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

MACHA_TEST("hydration_catalogue", test_catalogue_api_effective_artwork_is_display_only) {
    TestService fixture("catalogue-effective-artwork-api");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    auto& service = fixture.start();

    CatalogueItem artist;
    artist.id = "artist:api-test";
    artist.kind = CatalogueKind::artist;
    artist.title = "API Artist";
    artist = service.catalogue().upsert(artist);

    CatalogueItem album;
    album.id = "album:api-test";
    album.kind = CatalogueKind::album;
    album.title = "API Album";
    album.parent_id = artist.id;
    album.year = 2024;
    album = service.catalogue().upsert(album);

    CatalogueItem track;
    track.id = "track:api-test";
    track.kind = CatalogueKind::track;
    track.title = "API Track";
    track.parent_id = album.id;
    track = service.catalogue().upsert(track);

    const Bytes cover_bytes{0x10, 0x20, 0x30, 0x40};
    const auto cover = service.catalogue().put_artwork(album.id, "cover", "image/jpeg",
                                                       cover_bytes, album.revision);

    CatalogueApi api(service.catalogue(), service.catalogue_hints());
    const auto track_response = api.handle({.method = "GET",
                                            .path = "/api/v1/catalogue/items/track%3Aapi-test",
                                            .query = {},
                                            .headers = {},
                                            .body = {}, .session = {}});
    REQUIRE(track_response.status == 200);
    const auto track_json =
        Json::parse(std::string(track_response.body.begin(), track_response.body.end()));
    const auto* canonical = track_json.find("artwork");
    const auto* effective = track_json.find("effective_artwork");
    REQUIRE(canonical && canonical->isArray());
    REQUIRE(effective && effective->isArray());
    CHECK(canonical->asArray().empty());
    REQUIRE(effective->asArray().size() == 1);
    CHECK(effective->asArray().front().find("id")->asString() == to_string(cover.id));

    const auto artist_response = api.handle({.method = "GET",
                                             .path = "/api/v1/catalogue/items/artist%3Aapi-test",
                                             .query = {},
                                             .headers = {},
                                             .body = {}, .session = {}});
    REQUIRE(artist_response.status == 200);
    const auto artist_json =
        Json::parse(std::string(artist_response.body.begin(), artist_response.body.end()));
    const auto* artist_effective = artist_json.find("effective_artwork");
    REQUIRE(artist_effective && artist_effective->isArray());
    REQUIRE(artist_effective->asArray().size() == 1);
    CHECK(artist_effective->asArray().front().find("id")->asString() == to_string(cover.id));

    auto artwork_response = api.handle({.method = "GET",
                                        .path = "/api/v1/catalogue/artwork/" + to_string(cover.id),
                                        .query = {},
                                        .headers = {},
                                        .body = {}, .session = {}});
    REQUIRE(artwork_response.status == 200);
    CHECK(artwork_response.body == cover_bytes);

    // Round-trip the GET representation. `effective_artwork` is intentionally
    // ignored by the mutation parser and must never become canonical metadata.
    auto put_response = api.handle({.method = "PUT",
                                    .path = "/api/v1/catalogue/items/track%3Aapi-test",
                                    .query = {},
                                    .headers = {{"if-match", "\"rev-" +
                                                              std::to_string(track.revision) + "\""}},
                                    .body = track_response.body, .session = {}});
    REQUIRE(put_response.status == 200);
    auto stored = service.catalogue().get(track.id);
    REQUIRE(stored.has_value());
    CHECK(stored->artwork.empty());
}

MACHA_TEST("hydration_catalogue", test_catalogue_artwork_url_is_signed_and_capability_exempt) {
    TestService fixture("catalogue-artwork-signed-url");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    auto& service = fixture.start();

    CatalogueItem album;
    album.id = "album:signed-url-test";
    album.kind = CatalogueKind::album;
    album.title = "Signed URL Album";
    album = service.catalogue().upsert(album);
    const Bytes cover_bytes{0x01, 0x02, 0x03, 0x04};
    const auto cover =
        service.catalogue().put_artwork(album.id, "cover", "image/png", cover_bytes, album.revision);

    CatalogueApi api(service.catalogue(), service.catalogue_hints());
    const auto response = api.handle({.method = "GET",
                                      .path = "/api/v1/catalogue/items/album%3Asigned-url-test",
                                      .query = {},
                                      .headers = {},
                                      .body = {}, .session = {}});
    REQUIRE(response.status == 200);
    const auto json = Json::parse(std::string(response.body.begin(), response.body.end()));
    const auto* artwork = json.find("artwork");
    REQUIRE(artwork && artwork->isArray());
    REQUIRE(artwork->asArray().size() == 1);
    const auto& entry = artwork->asArray().front();
    const auto* url_value = entry.find("url");
    REQUIRE(url_value);
    const auto url = url_value->asString();

    // The URL is directly usable: it carries its own path, id and query.
    const auto question = url.find('?');
    REQUIRE(question != std::string::npos);
    HttpRequest signed_request;
    signed_request.method = "GET";
    signed_request.path = url.substr(0, question);
    signed_request.query = parse_test_query(url.substr(question + 1));

    // capability_request() -- what HttpServer consults to decide whether to
    // skip the ordinary bearer check -- must recognize this exact request.
    CHECK(api.capability_request(signed_request));

    const auto fetched = api.handle(signed_request);
    REQUIRE(fetched.status == 200);
    CHECK(fetched.body == cover_bytes);
    REQUIRE(fetched.headers.contains("Cache-Control"));
    CHECK(fetched.headers.at("Cache-Control").find("immutable") != std::string::npos);

    // A bare, unsigned request to the same path must NOT be granted the
    // exemption -- otherwise artwork would become unconditionally public
    // regardless of a configured bearer token, defeating the point.
    HttpRequest unsigned_request;
    unsigned_request.method = "GET";
    unsigned_request.path = signed_request.path;
    CHECK(!api.capability_request(unsigned_request));
}

MACHA_TEST("hydration_catalogue", test_catalogue_artwork_url_is_stable_so_it_can_be_cached) {
    TestService fixture("catalogue-artwork-stable-url");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    auto& service = fixture.start();

    CatalogueItem album;
    album.id = "album:stable-url-test";
    album.kind = CatalogueKind::album;
    album.title = "Stable URL Album";
    album = service.catalogue().upsert(album);
    const Bytes cover_bytes{0x09, 0x08, 0x07};
    service.catalogue().put_artwork(album.id, "cover", "image/png", cover_bytes, album.revision);

    const std::chrono::milliseconds ttl = std::chrono::hours(24);
    CatalogueApi api(service.catalogue(), service.catalogue_hints(), {}, {}, {}, ttl);
    auto artwork_url = [&] {
        const auto response = api.handle({.method = "GET",
                                          .path = "/api/v1/catalogue/items/album%3Astable-url-test",
                                          .query = {},
                                          .headers = {},
                                          .body = {}, .session = {}});
        REQUIRE(response.status == 200);
        const auto json = Json::parse(std::string(response.body.begin(), response.body.end()));
        return json.find("artwork")->asArray().front().find("url")->asString();
    };

    // The whole point: a browser keys its cache on the full URL, so two reads
    // of the same artwork must produce byte-identical URLs or the 24 hour
    // immutable header on the artwork response can never be consulted. Before
    // 0.40.0 the expiry was minted from the instant of signing, so these
    // differed at millisecond granularity and every poster was re-fetched on
    // every page load.
    const auto first = artwork_url();
    std::this_thread::sleep_for(5ms);
    const auto second = artwork_url();
    CHECK(first == second);

    // Same again through the list route, which signs separately: a client that
    // renders a grid and then an item page must not fetch the poster twice.
    const auto listed = api.handle({.method = "GET",
                                    .path = "/api/v1/catalogue/items",
                                    .query = {},
                                    .headers = {},
                                    .body = {}, .session = {}});
    REQUIRE(listed.status == 200);
    const auto listed_json = Json::parse(std::string(listed.body.begin(), listed.body.end()));
    bool found = false;
    for (const auto& item : listed_json.find("items")->asArray()) {
        if (item.find("id")->asString() != album.id) continue;
        found = true;
        CHECK(item.find("artwork")->asArray().front().find("url")->asString() == first);
    }
    CHECK(found);

    // Stability must not be bought with a short capability. The expiry is
    // rounded up to the bucket after next precisely so that a URL minted just
    // before a boundary still outlives the configured TTL rather than dying
    // in the client's hand.
    const auto question = first.find('?');
    REQUIRE(question != std::string::npos);
    const auto query = parse_test_query(first.substr(question + 1));
    REQUIRE(query.contains("exp"));
    const auto expires = std::stoull(query.at("exp"));
    const auto ttl_ms = static_cast<uint64_t>(ttl.count());
    CHECK(expires % ttl_ms == 0);
    const auto now = unix_ms();
    CHECK(expires >= now + ttl_ms);
    CHECK(expires <= now + 2 * ttl_ms);

    // And it is still a working capability, not merely a stable string.
    HttpRequest signed_request;
    signed_request.method = "GET";
    signed_request.path = first.substr(0, question);
    signed_request.query = query;
    CHECK(api.capability_request(signed_request));
    const auto fetched = api.handle(signed_request);
    REQUIRE(fetched.status == 200);
    CHECK(fetched.body == cover_bytes);

    // Artwork is content-addressed, so its id is its entity tag, and the
    // cache lifetime equals the capability's: a poster is downloaded once per
    // browser per TTL, and a revalidation of one already held is a 304 with
    // no body, answered before the artwork is read at all.
    const auto tag = fetched.headers.find("ETag");
    REQUIRE(tag != fetched.headers.end());
    const auto id_text = signed_request.path.substr(signed_request.path.rfind('/') + 1);
    CHECK(tag->second == "\"" + id_text + "\"");
    CHECK(fetched.headers.at("Cache-Control") ==
          "public, max-age=" + std::to_string(ttl_ms / 1000) + ", immutable");
    // Without it the browser zeroes every cross-origin timing, and a client
    // cannot measure how long a poster took.
    CHECK(fetched.headers.at("Timing-Allow-Origin") == "*");

    auto revalidation = signed_request;
    revalidation.headers["if-none-match"] = tag->second;
    const auto not_modified = api.handle(revalidation);
    CHECK(not_modified.status == 304);
    CHECK(not_modified.body.empty());
    CHECK(not_modified.headers.at("ETag") == tag->second);

    // A different tag is not a match: the bytes come back.
    auto stale = signed_request;
    stale.headers["if-none-match"] = "\"not-this-one\"";
    const auto refetched = api.handle(stale);
    CHECK(refetched.status == 200);
    CHECK(refetched.body == cover_bytes);
}

MACHA_TEST("hydration_catalogue", test_catalogue_artwork_capability_lasts_thirty_days_by_default) {
    // At 24 h every artwork URL changed at UTC midnight and every browser
    // downloaded every poster again the next day (2026-09-24, web client).
    CHECK(Config{}.catalogue.api.artwork_capability_ttl == std::chrono::hours(24 * 30));
}

MACHA_TEST("hydration_catalogue", test_catalogue_artwork_capability_rejects_tampered_or_expired) {
    TestService fixture("catalogue-artwork-tampered-url");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    auto& service = fixture.start();

    CatalogueItem album;
    album.id = "album:tampered-url-test";
    album.kind = CatalogueKind::album;
    album.title = "Tampered URL Album";
    album = service.catalogue().upsert(album);
    const Bytes cover_bytes{0x05, 0x06, 0x07, 0x08};
    const auto cover =
        service.catalogue().put_artwork(album.id, "cover", "image/png", cover_bytes, album.revision);

    CatalogueApi api(service.catalogue(), service.catalogue_hints());
    const auto response = api.handle({.method = "GET",
                                      .path = "/api/v1/catalogue/items/album%3Atampered-url-test",
                                      .query = {},
                                      .headers = {},
                                      .body = {}, .session = {}});
    REQUIRE(response.status == 200);
    const auto json = Json::parse(std::string(response.body.begin(), response.body.end()));
    const auto url = json.find("artwork")->asArray().front().find("url")->asString();
    const auto question = url.find('?');
    REQUIRE(question != std::string::npos);
    const auto path = url.substr(0, question);
    const auto query = parse_test_query(url.substr(question + 1));
    REQUIRE(query.contains("exp"));
    REQUIRE(query.contains("sig"));

    auto request_with = [&](std::map<std::string, std::string, std::less<>> q) {
        HttpRequest request;
        request.method = "GET";
        request.path = path;
        request.query = std::move(q);
        return request;
    };

    // Genuinely valid first, so the rest of this test is meaningful.
    CHECK(api.capability_request(request_with(query)));

    auto tampered_sig = query;
    tampered_sig["sig"][0] = (tampered_sig["sig"][0] == '0') ? '1' : '0';
    CHECK(!api.capability_request(request_with(tampered_sig)));

    auto tampered_exp = query;
    tampered_exp["exp"] = std::to_string(std::stoull(tampered_exp["exp"]) + 1);
    CHECK(!api.capability_request(request_with(tampered_exp)));

    // A *correctly signed* but expired URL must still be rejected -- expiry
    // itself is enforced, not just signature validity. Get a genuinely valid
    // signature for a past expiry by minting one through the real signing
    // path with a near-zero TTL and letting it lapse, rather than
    // hand-duplicating the HMAC construction here.
    CatalogueApi short_lived_api(service.catalogue(), service.catalogue_hints(), {}, {}, {}, 1ms);
    const auto short_lived_response =
        short_lived_api.handle({.method = "GET",
                                .path = "/api/v1/catalogue/items/album%3Atampered-url-test",
                                .query = {},
                                .headers = {},
                                .body = {}, .session = {}});
    REQUIRE(short_lived_response.status == 200);
    const auto short_lived_json = Json::parse(
        std::string(short_lived_response.body.begin(), short_lived_response.body.end()));
    const auto short_lived_url =
        short_lived_json.find("artwork")->asArray().front().find("url")->asString();
    const auto short_lived_question = short_lived_url.find('?');
    REQUIRE(short_lived_question != std::string::npos);
    HttpRequest expired_request;
    expired_request.method = "GET";
    expired_request.path = short_lived_url.substr(0, short_lived_question);
    expired_request.query = parse_test_query(short_lived_url.substr(short_lived_question + 1));
    std::this_thread::sleep_for(20ms);
    CHECK(!api.capability_request(expired_request));

    auto missing_sig = query;
    missing_sig.erase("sig");
    CHECK(!api.capability_request(request_with(missing_sig)));

    HttpRequest bare;
    bare.method = "GET";
    bare.path = path;
    CHECK(!api.capability_request(bare));
}

MACHA_TEST("hydration_catalogue", test_catalogue_root_ready_without_local_artwork) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "node", cluster.keyfile(), free_port());
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    NodeRuntime node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));
    DistributedStore store(node);
    MetadataManager metadata(node);
    CatalogueManager catalogue(node, store, metadata);

    CatalogueItem item;
    item.id = "test:movie:artwork-missing";
    item.kind = CatalogueKind::movie;
    item.title = "Catalogue Still Loads";
    item = catalogue.upsert(item);

    const Bytes artwork_bytes{0x01, 0x02, 0x03, 0x04};
    const auto artwork = catalogue.put_artwork(item.id, "poster", "image/jpeg",
                                               artwork_bytes, item.revision);
    REQUIRE(node.local_store().remove(artwork.id));
    REQUIRE(!node.local_store().has(artwork.id));

    // Force a cold catalogue load from the sharded CONTROL representation. The
    // referenced artwork DATA is deliberately absent locally and must not be a
    // prerequisite for catalogue readiness.
    CatalogueManager reloaded(node, store, metadata);
    reloaded.repair_once();

    auto status = reloaded.status();
    CHECK(status.ready);
    CHECK(status.items == 1);
    CHECK(status.artwork_objects == 1);
    CHECK(status.local_artwork_objects == 0);
    auto loaded = reloaded.get(item.id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->title == item.title);

    node.stop();
}

MACHA_FAST_TEST("hydration_catalogue", test_macos_unicode_namespace_aliases) {
#if defined(__APPLE__)
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "node", cluster.keyfile(), free_port());
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    NodeRuntime node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));
    DistributedStore store(node);
    MetadataManager metadata(node);
    FileSystem filesystem(node, store, metadata);

    filesystem.mkdir("/Music", 0755, getuid(), getgid());

    // Simulate namespace keys written by a previous version/client in D form.
    // The runtime alias index must resolve NFC callbacks to the exact persisted
    // spelling instead of rewriting the metadata representation.
    const std::string nfd_dir = "/Music/Cafe\xcc\x81 del Mar";
    const std::string nfd_file =
        nfd_dir + "/01.Clannad - Na Buachailli\xcc\x81 lainn.mp3";
    const std::string nfc_dir = "/Music/Caf\xc3\xa9 del Mar";
    const std::string nfc_file =
        nfc_dir + "/01.Clannad - Na Buachaill\xc3\xad lainn.mp3";

    metadata.mutate([&](MetadataSnapshot& snapshot) {
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

    CHECK(filesystem.getattr(nfc_dir).type == EntryType::directory);
    CHECK(filesystem.getattr(nfc_file).type == EntryType::file);
    auto listed = filesystem.readdir(nfc_dir);
    REQUIRE(listed.size() == 1);
    CHECK(listed.front().first == "01.Clannad - Na Buachailli\xcc\x81 lainn.mp3");

    // A new NFC leaf under an old NFD parent must retain the exact stored parent
    // spelling so require_parent() sees a real namespace key.
    const std::string new_nfc = nfc_dir + "/Macha Caf\xc3\xa9 Test.mp3";
    filesystem.create_file(new_nfc, 0644, getuid(), getgid());
    const auto persisted = metadata.snapshot();
    CHECK(persisted.entries.contains(nfd_dir + "/Macha Caf\xc3\xa9 Test.mp3"));
    CHECK(filesystem.getattr(new_nfc).type == EntryType::file);

    node.stop();
#endif
}

MACHA_TEST("hydration_catalogue", test_media_index_cache_survives_namespace_churn) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "node", cluster.keyfile(), free_port());
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    NodeRuntime node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));
    DistributedStore store(node);
    MetadataManager metadata(node);
    FileSystem filesystem(node, store, metadata);

    filesystem.mkdir("/media", 0755, getuid(), getgid());
    filesystem.create_file("/media/a.mkv", 0644, getuid(), getgid());
    auto a_bytes = pattern(32 * 1024 + 17);
    auto a_writer = filesystem.open_write("/media/a.mkv", true);
    REQUIRE(a_writer->write(0, a_bytes) == a_bytes.size());
    a_writer->commit();
    auto a_entry = filesystem.getattr("/media/a.mkv");
    auto a_id = file_media_id(a_entry);

    auto first = filesystem.find_media(a_id);
    REQUIRE(first.has_value());
    CHECK(first->first == "/media/a.mkv");

    // Unrelated namespace churn must not invalidate an already resolved,
    // content-addressed media id.
    filesystem.mkdir("/noise", 0755, getuid(), getgid());
    auto cached = filesystem.find_media(a_id);
    REQUIRE(cached.has_value());
    CHECK(cached->first == "/media/a.mkv");
    CHECK(file_media_id(cached->second) == a_id);

    // A genuinely new id is a cache miss and must rebuild against current
    // metadata, after which both the new and old ids remain resolvable.
    filesystem.create_file("/media/b.mkv", 0644, getuid(), getgid());
    auto b_bytes = pattern(48 * 1024 + 29);
    auto b_writer = filesystem.open_write("/media/b.mkv", true);
    REQUIRE(b_writer->write(0, b_bytes) == b_bytes.size());
    b_writer->commit();
    auto b_id = file_media_id(filesystem.getattr("/media/b.mkv"));
    CHECK(b_id != a_id);

    auto second = filesystem.find_media(b_id);
    REQUIRE(second.has_value());
    CHECK(second->first == "/media/b.mkv");
    REQUIRE(filesystem.find_media(a_id).has_value());

    node.stop();
}

MACHA_TEST("hydration_catalogue", test_catalogue_control_gc_protects_future_root_staging) {
    TestService fixture("catalogue-control-publication", ConfigProfile::isolated);
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    auto& service = fixture.start();

    REQUIRE(wait_until([&] {
        try {
            service.catalogue().repair_once();
            return service.catalogue().status().ready;
        } catch (...) {
            return false;
        }
    }));

    // CONTROL publication is data-before-metadata. Simulate a future root/shard
    // arriving on this replica before the root CAS references it. Even with zero
    // configured grace, GC must retain anything written after the currently
    // observed catalogue root.
    Bytes staged = pattern(4096 + 37);
    staged[0] ^= 0x6d;
    const auto staged_id = object_id(staged);
    REQUIRE(service.node().control_store().put(staged_id, staged));
    std::this_thread::sleep_for(5ms);

    const std::vector<ObjectId> no_live;
    for (int i = 0; i < 4; ++i)
        (void)service.catalogue().control_gc_step(no_live, 0ms, 64);
    CHECK(service.node().control_store().has(staged_id));

    // Once a successor catalogue root is committed/observed, an unreferenced
    // object from the previous publication epoch becomes an ordinary orphan.
    CatalogueItem item;
    item.id = "movie:control-publication-fence";
    item.kind = CatalogueKind::movie;
    item.title = "Control Publication Fence";
    (void)service.catalogue().upsert(item);
    std::this_thread::sleep_for(5ms);

    auto maintenance = service.catalogue().maintenance_objects();
    REQUIRE(maintenance.complete);
    std::vector<ObjectId> live(maintenance.control_live.begin(),
                               maintenance.control_live.end());
    for (int i = 0; i < 4 && service.node().control_store().has(staged_id); ++i)
        (void)service.catalogue().control_gc_step(live, 0ms, 64);
    CHECK(!service.node().control_store().has(staged_id));
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
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = c3.metadata_min_write_replicas = 1;
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

    Service s1(c1, keys);
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
    Service s2(c2, keys);
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

    Service s3(c3, keys);
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
        return !s1.node().local_store().has(first_art.id) &&
               !s2.node().local_store().has(first_art.id) &&
               !s3.node().local_store().has(first_art.id);
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
    // also removes descendants so leaf media bindings cannot keep the old match
    // alive and block a fresh scanner/provider lookup.
    auto clear_response = api.handle({.method = "DELETE",
                                      .path = "/api/v1/catalogue/items/show%3Atest/metadata",
                                      .query = {},
                                      .headers = {{"if-match", "\"rev-" + std::to_string(show.revision + 1) + "\""}},
                                      .body = {}, .session = {}});
    // The poster replacement did not mutate the copy of `show`; use the current
    // revision if the optimistic request raced a catalogue refresh.
    if (clear_response.status == 409) {
        auto current_show = s3.catalogue().get(show.id);
        REQUIRE(current_show.has_value());
        clear_response = api.handle({.method = "DELETE",
                                     .path = "/api/v1/catalogue/items/show%3Atest/metadata",
                                     .query = {},
                                     .headers = {{"if-match", "\"rev-" + std::to_string(current_show->revision) + "\""}},
                                     .body = {}, .session = {}});
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
        config->metadata_min_write_replicas = 1;
        config->maintenance.garbage_grace = 0ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
        // This case verifies metadata-burst coalescing and catalogue GC, not
        // failure detection.  The generic 500 ms test deadline can expire when
        // the process is descheduled under the parallel suite, causing a false
        // topology edge (and correctly fencing destructive GC).  Keep liveness
        // comfortably above scheduler jitter while retaining the short test
        // heartbeat and all existing behavioural deadlines.
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
    std::atomic_bool capture_catalogue_repair{};
    std::atomic_bool catalogue_repair_capture_claimed{};
    std::atomic_bool catalogue_repair_captured{};
    std::atomic_uint64_t catalogue_repair_runs_scheduled{};
    std::atomic_uint64_t catalogue_repair_runs_completed{};
    std::atomic_uint64_t catalogue_repair_requested_epoch{};
    std::atomic_uint64_t catalogue_repair_completed_epoch{};
    Service* observed_service = nullptr;
    Service s1(c1, keys, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" &&
            gate_metadata.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel)) {
            metadata_gate.enter_and_wait();
        } else if (stage == "catalogue-repair-begin") {
            catalogue_repairs.fetch_add(1, std::memory_order_relaxed);
            if (observed_service && capture_catalogue_repair.load(std::memory_order_acquire) &&
                !catalogue_repair_capture_claimed.exchange(true, std::memory_order_acq_rel)) {
                const auto convergence =
                    observed_service->metadata_convergence_diagnostics();
                catalogue_repair_runs_scheduled.store(convergence.runs_scheduled,
                                                       std::memory_order_release);
                catalogue_repair_runs_completed.store(convergence.runs_completed,
                                                       std::memory_order_release);
                catalogue_repair_requested_epoch.store(convergence.requested_epoch,
                                                        std::memory_order_release);
                catalogue_repair_completed_epoch.store(convergence.completed_epoch,
                                                        std::memory_order_release);
                catalogue_repair_captured.store(true, std::memory_order_release);
            }
        }
    });
    observed_service = &s1;
    Service s2(c2, keys);
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
    // The baseline has to be the snapshot that actually satisfied quiescence,
    // not a second one taken afterwards: between the predicate returning true
    // and a separate sample, another convergence run can be scheduled, and the
    // baseline then reads runs_scheduled=5 runs_completed=4. Every delta
    // computed from it is off by one, and the end-of-test assertion
    // `completed_delta == scheduled_delta` fails for a run that behaved
    // perfectly (observed 1 in 60, 2026-09-15).
    ConvergenceDemandDiagnostics convergence_before{};
    REQUIRE(wait_until([&] {
        const auto d = s1.metadata_convergence_diagnostics();
        if (d.scheduled || d.runs_scheduled != d.runs_completed)
            return false;
        convergence_before = d;
        return true;
    }, 5s));

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
        s2.node().metadata_replica().committed_generation();
    REQUIRE(wait_until([&] {
        return s1.node().known_metadata_generation() >= final_generation;
    }, 5s));
    // Deliberately NOT "no catalogue repair happened yet". Gating
    // `metadata-repair-begin` stops this node's repair pass; it does not stop
    // its committed generation from advancing, because publish_commit stores
    // and accepts commits on replicas directly. s1 therefore legitimately
    // moves forward under the gate and Service::loop repairs the catalogue
    // for the generation it now has. Measured: `repairs_before=3
    // gated_repairs=4 s1_committed=12 s1_known=13 final_generation=13`
    // (2026-09-15) -- one repair, not a storm. The coalescing claim is
    // asserted at the end of the test over the whole window, gate included,
    // which is where it belongs.
    const auto gated_repairs = catalogue_repairs.load(std::memory_order_acquire);

    // Capture the convergence state at the first catalogue repair AFTER the
    // gate opens: that is the repair which processes the accumulated burst,
    // and the only one the run-count assertions below are about. Latching it
    // from before the burst instead (as this did until 2026-09-15) could
    // claim the capture on a repair for the single pre-burst upsert, giving
    // `repair_scheduled=7 before_scheduled=6 repair_requested=10
    // before_requested=9` -- one run and one demand event, asserted against
    // as though it were the twenty-event burst. Observed 1 in 60.
    capture_catalogue_repair.store(true, std::memory_order_release);
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

    REQUIRE(catalogue_repair_captured.load(std::memory_order_acquire));
    const auto repair_runs_scheduled =
        catalogue_repair_runs_scheduled.load(std::memory_order_acquire);
    const auto repair_runs_completed =
        catalogue_repair_runs_completed.load(std::memory_order_acquire);
    const auto repair_requested_epoch =
        catalogue_repair_requested_epoch.load(std::memory_order_acquire);
    const auto repair_completed_epoch =
        catalogue_repair_completed_epoch.load(std::memory_order_acquire);
    const auto scheduled_delta = repair_runs_scheduled - convergence_before.runs_scheduled;
    const auto completed_delta = repair_runs_completed - convergence_before.runs_completed;
    const auto requested_delta = repair_requested_epoch - convergence_before.requested_epoch;
    // The gated owner and its coalesced follow-up are mandatory. Accept at most
    // one additional run caused by reconciliation publishing its own accepted
    // metadata edge; the many external burst events must never become one run
    // each. The pure one-follow-up state-machine contract is covered by the
    // dedicated ConvergenceDemand test.
    if (scheduled_delta < 2 || scheduled_delta > 3 ||
        completed_delta != scheduled_delta || repair_completed_epoch != repair_requested_epoch ||
        scheduled_delta >= requested_delta) {
        const auto convergence_after = s1.metadata_convergence_diagnostics();
        throw std::runtime_error(
            "unexpected convergence run count at catalogue repair: before_scheduled=" +
            std::to_string(convergence_before.runs_scheduled) +
            " before_completed=" + std::to_string(convergence_before.runs_completed) +
            " before_requested=" + std::to_string(convergence_before.requested_epoch) +
            " before_completed_epoch=" + std::to_string(convergence_before.completed_epoch) +
            " repair_scheduled=" + std::to_string(repair_runs_scheduled) +
            " repair_completed=" + std::to_string(repair_runs_completed) +
            " repair_requested=" + std::to_string(repair_requested_epoch) +
            " repair_completed_epoch=" + std::to_string(repair_completed_epoch) +
            " current_scheduled=" + std::to_string(convergence_after.runs_scheduled) +
            " current_completed=" + std::to_string(convergence_after.runs_completed) +
            " after_requested=" + std::to_string(convergence_after.requested_epoch) +
            " after_completed_epoch=" + std::to_string(convergence_after.completed_epoch) +
            " repairs_before=" + std::to_string(repairs_before) +
            " repairs_after=" +
            std::to_string(catalogue_repairs.load(std::memory_order_acquire)));
    }
    // One catalogue repair per convergence run that actually advanced this
    // node's committed metadata generation -- not one per burst event, which
    // is the property under test, and not exactly one, which was the old
    // assertion and is not something the product promises.
    //
    // `catalogue_dirty` is set by Service::loop when the local committed
    // generation moves. The gated owner run and its coalesced follow-up are
    // both mandatory (see above), so whether the burst's metadata arrives
    // entirely within the first run or is split across both is a timing
    // accident: either one or two generation changes, and therefore one or
    // two repairs. Measured failing that way 2 times in 183 runs on the
    // macOS laptop (2026-09-15): `before=2 after=4 scheduled_delta=2
    // requested_delta=20` -- two convergence runs, two repairs, from twenty
    // demand events. That is coalescing working, and the old assertion
    // called it a failure.
    // The subject: nine catalogue mutations, arriving as ~20 convergence
    // demand events, must not become ~20 catalogue repairs. A quarter of the
    // demand events is a generous ceiling on "coalesced" and still an order
    // of magnitude below a per-event storm; observed values are 1-2.
    //
    // Not pinned to exactly one, which is what this assertion said until
    // 2026-09-15 and is not a property the product has: whether the burst's
    // metadata lands entirely within the gated owner run or is split across
    // it and its mandatory coalesced follow-up is a timing accident, and each
    // generation advance correctly dirties the catalogue. Measured failing
    // that way twice in 183 runs (`before=2 after=4 scheduled_delta=2
    // requested_delta=20`).
    const auto repairs_after = catalogue_repairs.load(std::memory_order_acquire);
    const auto repairs_delta = repairs_after - repairs_before;
    if (repairs_delta < 1 || repairs_delta * 4 > requested_delta) {
        throw std::runtime_error(
            "unexpected catalogue repair count for one coalesced burst: repairs_before=" +
            std::to_string(repairs_before) + " repairs_after=" + std::to_string(repairs_after) +
            " gated_repairs=" + std::to_string(gated_repairs) +
            " scheduled_delta=" + std::to_string(scheduled_delta) +
            " completed_delta=" + std::to_string(completed_delta) +
            " requested_delta=" + std::to_string(requested_delta));
    }

    const auto unreclaimed = [&] {
        return std::none_of(superseded.begin(), superseded.end(), [&](const ObjectId& id) {
            return s1.node().local_store().has(id) || s2.node().local_store().has(id);
        });
    };
    const auto gc_started = Clock::now();
    const auto s2_wakeups_at_gc_start = s2.maintenance_wakeups();
    const std::string s2_stage_at_gc_start =
        std::string(s2.maintenance_stage()) + " " + s2.maintenance_sleep_diagnostic();
    if (!wait_until(unreclaimed, 12s)) {
        // Failing here has to distinguish a slow sweep from a stuck one, or
        // the next person reads "GC did not finish in 12 s" and calls it a
        // flake -- which is how this case survived five sightings. Give it a
        // bounded second chance purely to classify the failure, then say
        // which objects are left and on which node. Measured twice on
        // 2026-09-15: all four objects left on s2 (the node that wrote them)
        // and none on s1, with both catalogues converged at the same
        // generation and agreeing one artwork is live -- so not a slow sweep
        // on both nodes, but the writer reclaiming nothing while its peer
        // reclaimed everything. Four further occurrences in 600 reps all
        // reported `reclaimed_eventually=no` after 42 s: it is stuck, not
        // slow. See the P0 item in TODO/ACTIVE.md.
        const bool eventually = wait_until(unreclaimed, 15s);
        const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - gc_started).count();
        // Name what is left and where. Without this a failure here says only
        // "GC did not finish in 12 s", which cannot distinguish a slow sweep
        // from a genuine leak -- and this case has been waved past as a flake
        // more than once on exactly that ambiguity.
        std::string remaining;
        for (const auto& id : superseded) {
            const bool on1 = s1.node().local_store().has(id);
            const bool on2 = s2.node().local_store().has(id);
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
            claims += s1.node().retention_store().retained(RetentionClass::data, id) ? "held" : "free";
            claims += ",s2=";
            claims += s2.node().retention_store().retained(RetentionClass::data, id) ? "held" : "free";
            const auto state = s2.node().retention_store().claims(RetentionClass::data, id);
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
            std::to_string(s2.node().metadata_replica().committed_generation()) +
            " s2_known=" + std::to_string(s2.node().known_metadata_generation()) +
            " s2_all_reachable=" + (s2.node().membership().all_known_reachable() ? "yes" : "no") +
            " s2_stable=" + (cluster2.stable ? "yes" : "no") +
            " s2_heads=" + std::to_string(s2.node().metadata_replica().accepted_heads().size()) +
            " s1_heads=" + std::to_string(s1.node().metadata_replica().accepted_heads().size()) +
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

} // namespace

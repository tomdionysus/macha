// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's services built alone, over a node with no Service around it:
// what the composition root itself decides. The order it starts and stops
// them in is recorded against real configurations in test_lifecycle_record.
#include "media/media_engine.hpp"
#include "service/node_services.hpp"
#include "test_backend_support.hpp"

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// A node with local state recovered and the services built on it, as the
// Service builds them. Not started unless the test starts them.
class ServicesBench {
    TestCluster cluster_{ConfigProfile::isolated};
    mutable std::mutex mutex_;
    std::vector<std::string> events_;

  public:
    Config config;
    SubsystemRegistry registry;
    MaintenancePort port;
    std::unique_ptr<BareNode> node;
    std::unique_ptr<NodeServices> services;

    explicit ServicesBench(const std::function<void(Config&)>& tune = {}) {
        config = cluster_.node_config("services");
        if (tune)
            tune(config);
        node = std::make_unique<BareNode>(config, cluster_.keys());
        node->start();
        REQUIRE(node->wait_local_state_ready(10s));
        NodeServicesInstruments instruments;
        instruments.clock = std::make_shared<SystemMaintenanceClock>();
        instruments.lifecycle = [this](std::string_view event) {
            std::lock_guard lock(mutex_);
            events_.emplace_back(event);
        };
        instruments.constructed = Clock::now();
        services = std::make_unique<NodeServices>(*node, node->resources, node->local_state(),
                                                  node->metadata_server(), node->routes, registry,
                                                  port, std::move(instruments));
    }
    ~ServicesBench() {
        if (services)
            services->stop();
        services.reset();
        node->stop();
    }

    const std::filesystem::path& path() const { return cluster_.path(); }
    std::vector<std::string> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    RpcMessage ask(MessageType type, std::string_view json) {
        NodeInfo peer;
        peer.id = node->node_id();
        return node->routes.dispatch(peer, FrameType::control,
                                     RpcMessage{type, Bytes(json.begin(), json.end())});
    }
};

std::string text_of(const Bytes& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::string text_of(const HttpResponse& response) {
    return text_of(response.body);
}

SessionIdentity viewer() {
    return SessionIdentity{"session-id", Hash256{}, expand_roles(all_roles()), "viewer"};
}

HttpRequest request(std::string method, std::string path) {
    HttpRequest out;
    out.method = std::move(method);
    out.path = std::move(path);
    out.session = viewer();
    return out;
}

std::unique_ptr<MediaEngine> fake_engine(const StreamingConfig&) {
    return std::make_unique<FakeMediaEngine>();
}

// Streaming on, over the fake engine registered as the FFmpeg engine
// registers itself; a scanner that never calls a metadata provider.
void with_media(Config& config) {
    // Streaming is configured only beside the API; nothing here listens.
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();
    config.streaming.enabled = true;
    config.catalogue.scanner.enabled = true;
    config.catalogue.scanner.movies.tmdb.enabled = false;
    config.catalogue.scanner.tv.tmdb.enabled = false;
    config.catalogue.scanner.music.musicbrainz.enabled = false;
}

bool has_unmatched_hint(CatalogueHintQueue& hints, const std::string& path,
                        const std::string& media_id) {
    for (const auto& hint : hints.list())
        if (hint.path == path && hint.media_id == media_id &&
            hint.state == CatalogueHintState::no_match)
            return true;
    return false;
}

// Services built and never started hold their routes and have nothing to
// stop; destroyed, their routes answer no more.
MACHA_TEST("node_services", test_services_never_started_stop_nothing_and_answer_until_destroyed) {
    ServicesBench bench;
    const std::vector<std::string> built{"services constructed"};
    CHECK(bench.events() == built);
    bench.services->request_stop();
    bench.services->stop();
    CHECK(bench.events() == built);

    const auto jobs = bench.ask(MessageType::get_ingest_jobs, "");
    CHECK(jobs.type == MessageType::ingest_jobs_reply);
    CHECK(Json::parse(text_of(jobs.payload)).find("jobs")->asArray().empty());

    const auto action =
        bench.ask(MessageType::ingest_job_action, R"({"job_id":"none","action":"pause"})");
    CHECK(action.type == MessageType::ingest_job_action_reply);
    CHECK(!Json::parse(text_of(action.payload)).find("exists")->asBool());

    const auto intent = bench.ask(MessageType::torrent_intent, "{}");
    CHECK(intent.type == MessageType::torrent_intent_reply);
    const auto refused = Json::parse(text_of(intent.payload));
    CHECK(!refused.find("applied")->asBool());
    CHECK(refused.find("error")->asString() == "malformed intent");

    bench.services.reset();
    for (const auto type : {MessageType::get_ingest_jobs, MessageType::ingest_job_action,
                            MessageType::torrent_intent}) {
        const auto gone = bench.ask(type, "{}");
        CHECK(gone.type == MessageType::error);
        CHECK(text_of(gone.payload).find("is not available on this node") != std::string::npos);
    }
}

// A reload hands each section of the configuration to the service it
// belongs to.
MACHA_TEST("node_services", test_a_reload_reaches_every_service_with_live_limits) {
    set_media_engine_factory(fake_engine);
    ServicesBench bench([](Config& config) {
        with_media(config);
        config.catalogue.scanner.enabled = false;
        config.hydration.enabled = false;
    });
    auto& services = *bench.services;
    auto& fs = services.filesystem();
    fs.mkdir("/Movies", 0755, getuid(), getgid());
    const auto max_sessions = [&] {
        const auto status = services.streaming().handle(request("GET", "/api/v1/playback/status"));
        REQUIRE(status.status == 200);
        return Json::parse(text_of(status)).find("max_sessions")->asUInt64();
    };
    services.start();

    // As configured: no scanner, no hydration, the staging limit as given,
    // completed torrents kept.
    CHECK(!services.scanner().enabled());
    CHECK(!services.hydration().hydrator().status().enabled);
    CHECK(services.ingest().staging().limit() == bench.config.ingest.staging_limit);
    CHECK(!services.cluster_jobs().default_remove_after().has_value());
    CHECK(max_sessions() == bench.config.streaming.max_sessions);

    auto updated = bench.config;
    updated.catalogue.scanner.enabled = true;
    updated.hydration.enabled = true;
    updated.ingest.staging_limit = 7ULL * 1024 * 1024;
    updated.torrent.remove_on_complete_after = 5min;
    updated.streaming.max_sessions = 7;
    services.reconfigure(updated);

    CHECK(services.scanner().enabled());
    CHECK(services.hydration().hydrator().status().enabled);
    CHECK(services.ingest().staging().limit() == 7ULL * 1024 * 1024);
    CHECK(services.cluster_jobs().default_remove_after() == 5min);
    CHECK(max_sessions() == 7);
}

// The catalogue and playback routes are wired to this node's files, its
// media engine and its last availability survey.
MACHA_TEST("node_services", test_media_routes_answer_from_this_nodes_files_and_engine) {
    set_media_engine_factory(fake_engine);
    ServicesBench bench(with_media);
    auto& services = *bench.services;
    auto& fs = services.filesystem();
    fs.mkdir("/Movies", 0755, getuid(), getgid());
    const auto bytes = pattern(96 * 1024, 21);
    write_file(fs, "/Movies/film.mkv", bytes);
    const auto media_id = file_media_id(fs.getattr("/Movies/film.mkv"));
    const auto unknown = "macha:" + std::string(64, 'e');
    const auto profile = [&](const std::string& id) {
        return services.catalogue_api().handle(
            request("GET", "/api/v1/catalogue/media/" + id + "/profile"));
    };
    const auto keyframes = [&](const std::string& id) {
        return services.catalogue_api().handle(
            request("GET", "/api/v1/catalogue/media/" + id + "/keyframes"));
    };

    // A file this node holds: probed now, answered with the file's size, and
    // stored so the next answer needs no engine.
    CHECK(!services.catalogue().media_profile(media_id).has_value());
    const auto resolved = profile(media_id);
    REQUIRE(resolved.status == 200);
    const auto facts = Json::parse(text_of(resolved));
    CHECK(facts.find("media_id")->asString() == media_id);
    CHECK(facts.find("size")->asUInt64() == bytes.size());
    CHECK(facts.find("streams")->asArray().size() == 4);
    CHECK(resolved.headers.at("Cache-Control").find("immutable") != std::string::npos);

    // Media this node has no file for: nothing to probe and nothing to queue.
    CHECK(profile(unknown).status == 404);

    // A stored profile whose file is not here is served without a size, and
    // not as a permanent answer.
    const auto elsewhere = "macha:" + std::string(64, 'f');
    MediaProbeResult stored;
    stored.format = "matroska,webm";
    stored.duration_seconds = 12.0;
    stored.bitrate = 4'000'000;
    stored.streams.push_back(MediaStreamInfo{0, MediaStreamType::video, "h264", "High", "", 1920,
                                             1080, 0, 0, 8, true, false, 3'700'000});
    services.catalogue().put_media_profile(elsewhere, stored);
    const auto sizeless = profile(elsewhere);
    REQUIRE(sizeless.status == 200);
    CHECK(Json::parse(text_of(sizeless)).find("size")->isNull());
    CHECK(sizeless.headers.at("Cache-Control") == "private, no-cache");

    // The keyframe index: built by the engine for a file here (this engine
    // keeps none), absent for media that is not.
    const auto unsupported = keyframes(media_id);
    CHECK(unsupported.status == 422);
    CHECK(text_of(unsupported).find("keyframes_not_supported") != std::string::npos);
    CHECK(keyframes(unknown).status == 404);

    // Playback's facts about a file carry what the last survey knew of it:
    // nothing yet, said as such.
    auto media = request("GET", "/api/v1/playback/media");
    media.query["media_id"] = media_id;
    const auto listed = services.streaming().handle(media);
    REQUIRE(listed.status == 200);
    const auto listing = Json::parse(text_of(listed));
    const auto& entries = listing.find("media")->asArray();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().find("media_id")->asString() == media_id);
    REQUIRE(entries.front().find("availability") != nullptr);
    CHECK(entries.front().find("availability")->asString() == "unknown");
    CHECK(entries.front().find("surveyed_generation")->isNull());
    CHECK(entries.front().find("extents")->isNull());

    // Clearing an item's metadata puts its file, and only its file, in the
    // unmatched list, with no provider asked.
    write_file(fs, "/Movies/other.mkv", pattern(32 * 1024, 22));
    const auto other_id = file_media_id(fs.getattr("/Movies/other.mkv"));
    CatalogueItem item;
    item.id = "movie:film";
    item.kind = CatalogueKind::movie;
    item.title = "Film";
    item.media_ids = {media_id};
    (void)services.catalogue().upsert(item);
    CHECK(services.manage_api()
              .handle(request("DELETE", "/api/v1/catalogue/items/movie:film/metadata"))
              .status == 204);
    CHECK(has_unmatched_hint(services.catalogue_hints(), "/Movies/film.mkv", media_id));
    CHECK(!has_unmatched_hint(services.catalogue_hints(), "/Movies/other.mkv", other_id));
    CHECK(services.catalogue_hints().summary().queued == 0);
}

bool has_fuse_gauges(const std::map<std::string, uint64_t>& gauges) {
    return std::any_of(gauges.begin(), gauges.end(),
                       [](const auto& gauge) { return gauge.first.starts_with("fuse_"); });
}

// What an observation window samples of the services: how long each class
// has been idle, repair's figures, and the mount's while one is published.
MACHA_TEST("node_services", test_a_node_without_streaming_says_it_cannot_profile) {
    // Streaming off: no engine. A profile for a file this node holds cannot be
    // made here, and the node says so instead of failing the file.
    ServicesBench bench([](Config& config) {
        with_media(config);
        config.streaming.enabled = false;
    });
    auto& services = *bench.services;
    auto& fs = services.filesystem();
    write_file(fs, "/film.mkv", pattern(64 * 1024, 7));
    const auto media_id = file_media_id(fs.getattr("/film.mkv"));
    const auto answered = services.catalogue_api().handle(
        request("GET", "/api/v1/catalogue/media/" + media_id + "/profile"));
    CHECK(answered.status == 404);
    CHECK(text_of(answered).find("media_engine_unavailable") != std::string::npos);
    CHECK(!services.catalogue().media_profile(media_id).has_value());
}

MACHA_TEST("node_services", test_observation_gauges_follow_activity_repair_and_the_mount) {
    ServicesBench bench([](Config& config) { config.fuse.publication_quiet = 0ms; });
    auto& services = *bench.services;
    constexpr uint64_t never = 24ULL * 60 * 60 * 1000;

    const auto idle = services.observation_gauges();
    CHECK(idle.at("foreground_idle_ms") == never);
    CHECK(idle.at("interactive_idle_ms") == never);
    CHECK(idle.at("loader_idle_ms") == never);
    CHECK(!has_fuse_gauges(idle));

    // A write is loader activity and nobody's viewing.
    write_file(services.filesystem(), "/written.bin", pattern(64 * 1024, 51));
    const auto written = services.observation_gauges();
    CHECK(written.at("loader_idle_ms") < never);
    CHECK(written.at("foreground_idle_ms") == never);

    // Repair's figures are the store's own.
    const auto repair = services.store().repair_diagnostics();
    CHECK(written.at("repair_push_examined") == repair.push_examined);
    CHECK(written.at("repair_pull_examined") == repair.pull_examined);
    CHECK(written.at("repair_bytes_transferred") == repair.bytes_transferred);
    CHECK(written.at("repair_passes_completed") == repair.passes_completed);
    CHECK(written.at("repair_pull_unsourceable") == repair.pull_unsourceable);
    CHECK(written.at("repair_gate_ran") == repair.gate_ran);
    CHECK(written.at("repair_gate_share") == repair.gate_share);
    CHECK(written.at("repair_gate_credit") == repair.gate_credit);
    CHECK(written.at("repair_prompt_copies") == repair.prompt_copies);

    // A published mount adds what it has published; withdrawn, it is gone.
    auto fuse = bench.config.fuse;
    fuse.spool_path = bench.path() / "spool";
    fuse.operation_journal_path = *fuse.spool_path / "operations.log";
    auto frontend = make_fuse_frontend(services.filesystem(), bench.node->resources.memory, fuse);
    bench.registry.publish_fuse(frontend);
    auto handle = frontend->create("/mounted.bin", 0644, getuid(), getgid(), true, true, false);
    const auto payload = pattern(64 * 1024 + 3, 52);
    REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
    frontend->release(handle.inode, true);
    REQUIRE(frontend->wait_for_idle(10s));
    const auto mounted = services.observation_gauges();
    const auto diagnostics = frontend->diagnostics();
    CHECK(mounted.at("fuse_publications_completed") == 1);
    CHECK(mounted.at("fuse_publication_bytes_committed") == payload.size());
    CHECK(mounted.at("fuse_publication_bytes_confirmed") ==
          diagnostics.data_publication_bytes_confirmed);
    CHECK(mounted.at("fuse_spool_bytes") == diagnostics.spool_bytes);
    CHECK(mounted.at("fuse_parked_publications") == 0);

    bench.registry.withdraw_fuse(frontend.get());
    frontend->stop();
    CHECK(!has_fuse_gauges(services.observation_gauges()));
}

// The claims barrier before a commit: every extent the commit refers to is
// claimed under the commit's dot, a hole is not an object, and a commit whose
// objects cannot be claimed is refused and says which floor failed.
MACHA_TEST("node_services", test_a_commit_is_published_only_once_its_objects_are_claimed) {
    auto log = std::make_shared<ConcurrentCapturingLogger>(LogLevel::debug);
    Log::set_logger(log);
    ServicesBench bench;
    auto& services = *bench.services;
    auto& fs = services.filesystem();
    auto& claims = bench.node->claims();
    const auto self = bench.node->node_id();
    const auto extent = bench.config.extent_size;

    // Data, then a hole to the end.
    write_file(fs, "/sparse.bin", pattern(4096, 41));
    fs.truncate_file("/sparse.bin", 3 * extent);
    const auto sparse = fs.getattr("/sparse.bin");
    size_t holes = 0;
    size_t data = 0;
    const auto sequence = services.metadata().snapshot().mutation_sequences.at(self);
    for (const auto& ref : sparse.extents) {
        if (ref.hole) {
            ++holes;
            continue;
        }
        ++data;
        CHECK(claims.claims(RetentionClass::data, ref.id).adds.at(self) == sequence);
    }
    CHECK(holes >= 1);
    CHECK(data == 1);
    CHECK(claims.claim_objects(RetentionClass::data) == 1);

    // An entry bringing in an object nobody holds is refused and unpublished,
    // and the barrier says why.
    FsEntry phantom;
    phantom.type = EntryType::file;
    phantom.size = 4096;
    phantom.extents.push_back({0, 4096, object_id(pattern(4096, 42)), false});
    std::string refusal;
    try {
        services.metadata().mutate(
            [&](MetadataSnapshot& snapshot) { snapshot.entries["/phantom.bin"] = phantom; });
    } catch (const MetadataNotReady& error) {
        refusal = error.what();
    }
    CHECK(refusal == "DATA object is held by no node present before metadata publication");
    CHECK(!services.metadata().snapshot().entries.contains("/phantom.bin"));
    CHECK(claims.claim_objects(RetentionClass::data) == 1);
    bool named = false;
    for (const auto& [level, line] : log->records())
        named = named || (line.find("metadata retention barrier") != std::string::npos &&
                          line.find("data_objects=1") != std::string::npos &&
                          line.find("outcome=data-unheld") != std::string::npos);
    CHECK(named);
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
}

} // namespace

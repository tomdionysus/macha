// SPDX-License-Identifier: GPL-3.0-or-later
//
// What is the Service's own: the order it starts and stops in, what a startup
// that fails, stalls or is stopped part-way leaves behind, and how its HTTP
// front admits a request and sends it to the API that answers it.
#include "test_backend_support.hpp"

#include <cstdlib>
#include <fstream>
#include <sys/wait.h>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

uint16_t enable_api(Config& config) {
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();
    return config.catalogue.api.port;
}

// The Service's lifecycle steps in order; `hold` is entered at the step named.
class Lifecycle {
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
    std::string hold_at_;

  public:
    TestGate hold;

    explicit Lifecycle(std::string hold_at = {}) : hold_at_(std::move(hold_at)) {}
    ServiceInstruments instruments() {
        ServiceInstruments out;
        out.lifecycle = [this](std::string_view event) {
            {
                std::lock_guard lock(mutex_);
                events_.emplace_back(event);
            }
            if (event == hold_at_)
                hold.enter_and_wait();
        };
        return out;
    }
    std::vector<std::string> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    bool saw(std::string_view event) const {
        const auto all = events();
        return std::find(all.begin(), all.end(), event) != all.end();
    }
};

struct OpenOnExit {
    TestGate& gate;
    ~OpenOnExit() { gate.open(); }
};

bool absent(FileSystem& fs, const std::string& path) {
    try {
        (void)fs.getattr(path);
    } catch (const FsError& error) {
        return error.code() == ENOENT;
    }
    return false;
}

std::string failure_of(Service& service) {
    try {
        (void)service.filesystem();
    } catch (const std::exception& error) {
        return error.what();
    }
    return "no failure";
}

// A Service built and never started stops what it built, in order, once.
MACHA_TEST("service_root", test_a_service_never_started_stops_in_order_once) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("never-started");
    enable_api(config);
    Lifecycle lifecycle;
    const std::vector<std::string> expected{
        "stop catalogue-http", "request_stop status", "request_stop catalogue-http",
        "request_stop node",   "stop status",         "stop node"};
    {
        Service service(config, cluster.keys(), test_durability_window, {}, {}, {},
                        lifecycle.instruments());
        CHECK(!service.ready());
        service.stop();
        CHECK(lifecycle.events() == expected);
        service.stop();
        CHECK(lifecycle.events() == expected);
    }
    CHECK(lifecycle.events() == expected);
}

// Local recovery fails in one plane while the other is still recovering, then
// finishes: the stall diagnostic names the failure and what had recovered;
// once recovery ends the failure is what health, ordinary routes and every
// waiter report, Status still answers, and stop() completes.
struct StartupFailure {
    // The stage that throws, and what it throws.
    const char* failing;
    const char* thrown;
    // The other plane's stage, held until the failure has been recorded.
    const char* held;
    // What the stall diagnostic says meanwhile, and what callers are told
    // once startup has failed.
    std::vector<const char*> stalled;
    const char* reported;
};

void startup_fails(const StartupFailure& failure) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("startup-failure");
    const auto port = enable_api(config);
    config.service_startup_no_progress = 0ms;
    config.service_startup_timeout = 100ms;

    TestGate other_plane;
    std::mutex mutex;
    std::vector<std::string> stalls;
    Service service(
        config, cluster.keys(), test_durability_window,
        [&](std::string_view stage) {
            if (stage == failure.failing)
                throw std::runtime_error(failure.thrown);
            if (stage == failure.held)
                other_plane.enter_and_wait();
        },
        {},
        [&](std::string_view diagnostic) {
            std::lock_guard lock(mutex);
            stalls.emplace_back(diagnostic);
        });
    OpenOnExit release{other_plane};
    service.start();
    REQUIRE(other_plane.wait_for_entries(1, 10s));
    REQUIRE(wait_until([&] { return service.node().readiness().failed; }, 10s));

    // One plane has failed and the other has not finished: still starting,
    // and a waiter that gives up is told what failed.
    {
        const auto health = http_request(port, "GET", "/api/v1/health");
        CHECK(health.status == 503);
        CHECK(health.has("\"status\":\"starting\""));
        CHECK(failure_of(service).find("service startup stalled") != std::string::npos);
        std::lock_guard lock(mutex);
        REQUIRE(stalls.size() == 1);
        for (const char* said : failure.stalled) {
            if (stalls.front().find(said) == std::string::npos)
                std::cerr << "the stall diagnostic lacks '" << said << "': " << stalls.front()
                          << "\n";
            CHECK(stalls.front().find(said) != std::string::npos);
        }
    }

    other_plane.open();
    REQUIRE(wait_until(
        [&] { return http_request(port, "GET", "/api/v1/health").has("\"status\":\"failed\""); },
        10s));
    const auto health = http_request(port, "GET", "/api/v1/health");
    CHECK(health.status == 503);
    CHECK(health.has("\"service\":\"macha\""));
    CHECK(!service.ready());

    // A waiter gets the failure at once, not a stall.
    CHECK(failure_of(service) == failure.reported);
    {
        std::lock_guard lock(mutex);
        CHECK(stalls.size() == 1);
    }

    const auto token = bearer_header(service);
    const auto ordinary = http_request(port, "GET", "/api/v1/catalogue/status", token);
    CHECK(ordinary.status == 503);
    CHECK(ordinary.has("startup_failed"));
    CHECK(ordinary.has(failure.reported));
    CHECK(http_request(port, "GET", "/api/v1/status", token).status == 200);

    service.stop();
    CHECK(!service.ready());
}

MACHA_TEST("service_root", test_a_failed_startup_is_what_every_caller_is_told) {
    startup_fails({"data-storage", "backend missing", "control-storage",
                   {"control_plane=ready", "data_storage=recovering", "control_storage=recovering",
                    "failed=true"},
                   "backend missing"});
}

// The other plane, failing with nothing to say for itself.
MACHA_TEST("service_root", test_a_failed_startup_with_no_reason_still_says_it_failed) {
    startup_fails({"cache", "", "data-storage",
                   {"control_storage=ready", "cache=recovering", "data_storage=recovering",
                    "failed=true"},
                   "server startup failed"});
}

// A waiter that gives up before the control plane is online is told so.
MACHA_TEST("service_root", test_a_stall_before_the_control_plane_names_it_starting) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("stalled-control-plane");
    config.service_startup_no_progress = 0ms;
    config.service_startup_timeout = 100ms;

    TestGate control_gate;
    std::mutex mutex;
    std::vector<std::string> stalls;
    Service service(
        config, cluster.keys(), test_durability_window,
        [&](std::string_view stage) {
            if (stage == "control-plane")
                control_gate.enter_and_wait();
        },
        {},
        [&](std::string_view diagnostic) {
            std::lock_guard lock(mutex);
            stalls.emplace_back(diagnostic);
        });
    OpenOnExit release{control_gate};
    std::jthread starter([&] { service.start(); });
    REQUIRE(control_gate.wait_for_entries(1, 10s));

    CHECK(failure_of(service).find("service startup stalled") != std::string::npos);
    {
        std::lock_guard lock(mutex);
        REQUIRE(stalls.size() == 1);
        CHECK(stalls.front().find("control_plane=starting") != std::string::npos);
        CHECK(stalls.front().find("local_state=recovering") != std::string::npos);
        CHECK(stalls.front().find("failed=false") != std::string::npos);
    }

    control_gate.open();
    starter.join();
    service.stop();
}

// With nobody to tell, a stalled startup ends the process with status 1, for
// the supervisor to start a fresh one. That is the outcome, so the stalled
// Service runs in a child of this case; forked before any thread exists.
MACHA_TEST("service_root", test_a_stalled_startup_with_nobody_to_tell_ends_the_process) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("stalled-exit");
    config.service_startup_no_progress = 0ms;
    config.service_startup_timeout = 100ms;

    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        TestGate never;
        Service service(config, cluster.keys(), test_durability_window,
                        [&](std::string_view stage) {
                            if (stage == "data-storage")
                                never.enter_and_wait();
                        });
        service.start();
        try {
            (void)service.filesystem();
        } catch (...) {
        }
        // Not reached: the wait above ended the process.
        std::_Exit(3);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
}

// A stop that arrives while startup is between two steps ends startup there:
// nothing later is built or started, the node never becomes ready, and stop()
// takes down what exists.
void stop_arrives_at(const std::string& step, const std::vector<std::string>& never) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("stopped-in-startup");
    Lifecycle lifecycle(step);
    Service service(config, cluster.keys(), test_durability_window, {}, {}, {},
                    lifecycle.instruments());
    OpenOnExit release{lifecycle.hold};
    service.start();
    REQUIRE(lifecycle.hold.wait_for_entries(1, 30s));

    service.request_stop();
    lifecycle.hold.open();
    service.stop();

    CHECK(!service.ready());
    for (const auto& event : never) {
        if (lifecycle.saw(event))
            std::cerr << "after a stop at '" << step << "' the service still took '" << event
                      << "'\n";
        CHECK(!lifecycle.saw(event));
    }
    const auto events = lifecycle.events();
    REQUIRE(!events.empty());
    CHECK(events.back() == "stop node");
}

MACHA_TEST("service_root", test_a_stop_once_local_state_has_recovered_builds_no_services) {
    stop_arrives_at("local state recovered", {"services constructed", "services ready"});
}

MACHA_TEST("service_root", test_a_stop_once_services_are_built_starts_none_of_them) {
    stop_arrives_at("services constructed",
                    {"start media-information", "start maintenance", "services ready",
                     "cancel-io filesystem", "stop subsystems", "stop maintenance"});
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

std::map<std::string, std::string> bearer(const std::string& token) {
    return {{"Authorization", "Bearer " + token}};
}

std::string sign_in(uint16_t port, std::string_view username, std::string_view password) {
    Json::Object credentials{{"username", std::string(username)},
                             {"password", std::string(password)}};
    const auto body = Json(Json::Object{{"credentials", Json(std::move(credentials))}}).dump();
    const auto reply = http_request(port, "POST", "/api/v1/session", {}, body);
    REQUIRE(reply.status == 201);
    return reply.json().find("token")->asString();
}

// What admits a request: liveness, signing in, the web client and a stream
// capability need no token; everything else needs a live session, which is
// only as live as its account, and the one role its route names.
MACHA_TEST("service_root", test_a_request_is_admitted_by_session_capability_and_role) {
    TestService fixture("request-admission", ConfigProfile::isolated);
    auto& config = fixture.config();
    const auto port = enable_api(config);
    config.web.root = fixture.path() / "web";
    write_text(config.web.root / "index.html", "<!doctype html><title>macha client</title>");
    auto& service = fixture.start();
    const auto self = service.node().node_id();

    // No token, or one this node never minted: refused before any handler.
    CHECK(http_request(port, "GET", "/api/v1/catalogue/items").status == 401);
    CHECK(http_request(port, "GET", "/api/v1/catalogue/items", bearer("never-minted")).status == 401);

    // Liveness and the client itself need none; /api stays the server's.
    CHECK(http_request(port, "GET", "/api/v1/health").status == 200);
    const auto client = http_request(port, "GET", "/");
    CHECK(client.status == 200);
    CHECK(client.has("macha client"));
    CHECK(http_request(port, "GET", "/library/some/deep/link").has("macha client"));
    CHECK(http_request(port, "GET", "/api/v1/no-such-route").status == 401);

    // A stream URL carries its own authority; the session beside it does not.
    CHECK(http_request(port, "GET", "/api/v1/playback/sessions/none/stream/token/index.m3u8").status ==
          404);
    CHECK(http_request(port, "GET", "/api/v1/playback/sessions/none").status == 401);

    // Signing in needs no token, and the session it mints needs no role to
    // read itself.
    auto& users = service.accounts().users();
    const auto alice =
        users.create("alice", "correct horse battery", {std::string(role_media_viewer)}, self);
    REQUIRE(alice.has_value());
    const auto refused = http_request(port, "POST", "/api/v1/session", {},
                              R"({"credentials":{"username":"alice","password":"wrong horse"}})");
    CHECK(refused.status == 401);
    CHECK(refused.has("invalid_credentials"));
    const auto alice_token = sign_in(port, "alice", "correct horse battery");
    const auto own = http_request(port, "GET", "/api/v1/session", bearer(alice_token));
    CHECK(own.status == 200);
    CHECK(own.json().find("username")->asString() == "alice");

    // The route's role, by name; holding it reaches the API behind the route.
    const auto forbidden = http_request(port, "GET", "/api/v1/users", bearer(alice_token));
    CHECK(forbidden.status == 403);
    CHECK(forbidden.has("'manage_users'"));
    const auto listed = http_request(port, "GET", "/api/v1/users", bearer_header(service));
    CHECK(listed.status == 200);
    CHECK(listed.has("\"alice\""));
    CHECK(http_request(port, "GET", "/api/v1/catalogue/items", bearer(alice_token)).status == 200);

    // A credential change, arriving as a replicated record does, retires the
    // sessions minted before it; so does the account's removal.
    REQUIRE(users.update(alice->id, "another horse battery", std::nullopt, self).has_value());
    CHECK(http_request(port, "GET", "/api/v1/session", bearer(alice_token)).status == 401);
    const auto renewed = sign_in(port, "alice", "another horse battery");
    CHECK(http_request(port, "GET", "/api/v1/session", bearer(renewed)).status == 200);
    REQUIRE(users.remove(alice->id, self).has_value());
    CHECK(http_request(port, "GET", "/api/v1/session", bearer(renewed)).status == 401);
}

// Once services are ready each path reaches the API that owns it, told apart
// here by an answer only that API gives.
MACHA_TEST("service_root", test_each_route_reaches_the_api_that_owns_it) {
    TestService fixture("routes", ConfigProfile::isolated);
    const auto port = enable_api(fixture.config());
    auto& service = fixture.start();
    const auto admin = bearer_header(service);
    struct Route {
        const char* path;
        int status;
        const char* answer;
    };
    const std::vector<Route> routes{
        {"/api/v1/playback/nothing", 404, "\"message\":\"endpoint not found\"},\"status\""},
        {"/api/v1/ingest/jobs", 200, "\"jobs\":[]"},
        {"/api/v1/torrents/nothing", 404, "acquisition endpoint not found"},
        {"/api/v1/manage/nothing", 404, "management route not found"},
        {"/api/v1/manage/unmatched", 200, "\"items\":[]"},
        {"/api/v1/files", 200, "\"path\":\"/\""},
        {"/api/v1/files/nothing", 404, "no such file or directory"},
        {"/api/v1/catalogue/items", 200, "\"items\":[]"},
        {"/api/v1/nothing", 404, "\"error\":\"not_found\""},
    };
    for (const auto& route : routes) {
        const auto reply = http_request(port, "GET", route.path, admin);
        if (reply.status != route.status || !reply.has(route.answer))
            std::cerr << route.path << " answered " << reply.status << " " << reply.body << "\n";
        CHECK(reply.status == route.status);
        CHECK(reply.has(route.answer));
    }
}

FsEntry file_entry() {
    FsEntry entry;
    entry.type = EntryType::file;
    entry.mtime_ns = 1'700'000'000'000'000'000;
    entry.version = 3;
    return entry;
}

FsEntry directory_entry() {
    FsEntry entry;
    entry.type = EntryType::directory;
    entry.mode = 0755;
    return entry;
}

MetadataConflict entry_conflict(std::string key, uint8_t salt, std::optional<FsEntry> base,
                                std::optional<FsEntry> left, std::optional<FsEntry> right) {
    MetadataConflict conflict;
    conflict.kind = MetadataConflictKind::namespace_entry;
    conflict.key = std::move(key);
    conflict.left_head = sha256(pattern(32, salt));
    conflict.right_head = sha256(pattern(32, static_cast<uint8_t>(salt + 1)));
    conflict.base_entry = std::move(base);
    conflict.left_entry = std::move(left);
    conflict.right_entry = std::move(right);
    return conflict;
}

const Json* conflict_named(const Json& listing, const std::string& id) {
    for (const auto& item : listing.find("conflicts")->asArray())
        if (item.find("id")->asString() == id)
            return &item;
    return nullptr;
}

// Standing metadata conflicts over HTTP: each is listed with both
// alternatives and its common ancestor, and resolving one installs the side
// chosen and leaves the others standing.
MACHA_TEST("service_root", test_standing_conflicts_are_listed_and_resolved_over_http) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("conflicts");
    const auto port = enable_api(config);
    // The catalogue's own repair merges a catalogue conflict as soon as it
    // runs; held here so the conflict stands while the operator's routes act.
    std::atomic_bool hold_catalogue_repair{};
    TestGate catalogue_repair;
    Service service(config, cluster.keys(), test_durability_window, {},
                    [&](std::string_view stage) {
                        if (stage == "catalogue-repair-begin" && hold_catalogue_repair.load())
                            catalogue_repair.enter_and_wait();
                    });
    OpenOnExit release{catalogue_repair};
    service.start();
    const auto admin = bearer_header(service);
    const std::string route = "/api/v1/manage/metadata/conflicts";
    auto& metadata = service.metadata_manager();
    auto& fs = service.filesystem();
    REQUIRE(wait_metadata_writable(service));

    // Two catalogue roots: the earlier one as an alternative, the current one
    // as the common ancestor a catalogue conflict leaves installed.
    const auto root_after = [&](const char* id) {
        CatalogueItem item;
        item.id = id;
        item.kind = CatalogueKind::movie;
        item.title = id;
        (void)service.catalogue().upsert(item);
        const auto root = metadata.snapshot().catalogue_root;
        REQUIRE(root.has_value());
        return *root;
    };
    const auto earlier_root = root_after("movie:first");
    const auto current_root = root_after("movie:second");
    REQUIRE(earlier_root != current_root);

    hold_catalogue_repair = true;
    fs.mkdir("/kept-base", 0755, getuid(), getgid());
    const auto installed = fs.getattr("/kept-base");
    REQUIRE(catalogue_repair.wait_for_entries(1, 10s));

    const auto take_left = entry_conflict("/take-left", 1, std::nullopt, file_entry(),
                                          directory_entry());
    const auto take_right = entry_conflict("/take-right", 3, std::nullopt, file_entry(),
                                           directory_entry());
    const auto take_base = entry_conflict("/kept-base", 5, installed, file_entry(), std::nullopt);
    MetadataConflict catalogue;
    catalogue.kind = MetadataConflictKind::catalogue_root;
    catalogue.key = "catalogue";
    catalogue.left_head = sha256(pattern(32, 7));
    catalogue.right_head = sha256(pattern(32, 8));
    catalogue.base_catalogue_root = current_root;
    catalogue.left_catalogue_root = earlier_root;
    const std::vector<MetadataConflict> standing{take_left, take_right, take_base, catalogue};
    metadata.mutate([&](MetadataSnapshot& snapshot) {
        for (const auto& conflict : standing)
            snapshot.conflicts.emplace(metadata_conflict_id(conflict), conflict);
    });

    // The list is a read.
    CHECK(http_request(port, "POST", route, admin).status == 405);
    const auto listed = http_request(port, "GET", route, admin);
    REQUIRE(listed.status == 200);
    const auto listing = listed.json();
    CHECK(listing.find("generation")->asUInt64() == metadata.snapshot_view().generation);
    REQUIRE(listing.find("conflicts")->asArray().size() == standing.size());
    {
        const auto* item = conflict_named(listing, metadata_conflict_id(take_base));
        REQUIRE(item != nullptr);
        CHECK(item->find("kind")->asString() == "namespace_entry");
        CHECK(item->find("key")->asString() == "/kept-base");
        CHECK(item->find("left_head")->asString() == hex(take_base.left_head.bytes));
        CHECK(item->find("right_head")->asString() == hex(take_base.right_head.bytes));
        CHECK(item->find("base")->find("type")->asString() == "directory");
        CHECK(item->find("left")->find("type")->asString() == "file");
        CHECK(item->find("left")->find("size")->asUInt64() == 0);
        CHECK(item->find("left")->find("mtime_ns")->asUInt64() == 1'700'000'000'000'000'000ULL);
        CHECK(item->find("left")->find("version")->asUInt64() == 3);
        CHECK(item->find("left")->find("extents")->asUInt64() == 0);
        CHECK(item->find("right")->isNull());
    }
    {
        const auto* item = conflict_named(listing, metadata_conflict_id(catalogue));
        REQUIRE(item != nullptr);
        CHECK(item->find("kind")->asString() == "catalogue_root");
        CHECK(item->find("base")->asString() == to_string(current_root));
        CHECK(item->find("left")->asString() == to_string(earlier_root));
        CHECK(item->find("right")->isNull());
    }

    // A resolution is a POST to /{id}/resolve naming one of the three sides.
    const auto resolve = [&](const MetadataConflict& conflict, std::string_view choice) {
        return http_request(port, "POST",
                    route + "/" + metadata_conflict_id(conflict) + "/resolve?choice=" +
                        std::string(choice),
                    admin);
    };
    const auto left_id = metadata_conflict_id(take_left);
    CHECK(http_request(port, "GET", route + "/" + left_id + "/resolve?choice=left", admin).status == 405);
    CHECK(http_request(port, "POST", route + "/" + left_id, admin).status == 404);
    CHECK(http_request(port, "POST", route + "/" + left_id + "/settle?choice=left", admin).status == 404);
    const auto unnamed = http_request(port, "POST", route + "/" + left_id + "/resolve", admin);
    CHECK(unnamed.status == 400);
    CHECK(unnamed.has("bad_choice"));
    CHECK(resolve(take_left, "newest").status == 400);
    const auto unknown =
        http_request(port, "POST", route + "/" + std::string(64, '0') + "/resolve?choice=left", admin);
    CHECK(unknown.status == 409);
    CHECK(unknown.has("not_standing"));
    // Nothing above resolved anything.
    CHECK(metadata.snapshot().conflicts.size() == standing.size());
    CHECK(absent(fs, "/take-left"));

    CHECK(resolve(take_left, "left").status == 204);
    CHECK(fs.getattr("/take-left").type == EntryType::file);
    CHECK(resolve(take_right, "right").status == 204);
    CHECK(fs.getattr("/take-right").type == EntryType::directory);
    CHECK(resolve(take_base, "base").status == 204);
    CHECK(fs.getattr("/kept-base").type == EntryType::directory);
    // Resolved once: the same request again finds nothing standing.
    CHECK(resolve(take_left, "left").status == 409);

    // The catalogue's right side is "no catalogue".
    {
        const auto remaining = http_request(port, "GET", route, admin).json();
        REQUIRE(remaining.find("conflicts")->asArray().size() == 1);
        CHECK(conflict_named(remaining, metadata_conflict_id(catalogue)) != nullptr);
    }
    CHECK(resolve(catalogue, "right").status == 204);
    const auto settled = metadata.snapshot();
    CHECK(!settled.catalogue_root.has_value());
    CHECK(settled.conflicts.empty());
    CHECK(http_request(port, "GET", route, admin).json().find("conflicts")->asArray().empty());

    catalogue_repair.open();
    service.stop();
}

// A node still waiting for its bootstrap peer has no namespace: both routes
// say so rather than failing or answering from nothing.
MACHA_TEST("service_root", test_conflict_routes_say_when_metadata_is_unavailable) {
    TestService fixture("conflicts-unavailable", ConfigProfile::functional);
    fixture.config().bootstrap = {{"127.0.0.1", free_port()}};
    const auto port = enable_api(fixture.config());
    auto& service = fixture.start();
    const auto admin = bearer_header(service);
    const std::string route = "/api/v1/manage/metadata/conflicts";

    const auto listed = http_request(port, "GET", route, admin);
    CHECK(listed.status == 503);
    CHECK(listed.has("metadata_unavailable"));
    const auto resolved =
        http_request(port, "POST", route + "/" + std::string(64, '0') + "/resolve?choice=left", admin);
    CHECK(resolved.status == 503);
    CHECK(resolved.has("metadata_unavailable"));
}

} // namespace

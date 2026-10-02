// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/service.hpp"
#include "metadata/namespace_control_store.hpp"
#include "diagnostics.hpp"
#include "fuse/fuse_frontend.hpp"
#include "fuse/fuse_subsystem.hpp"
#include "json.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "startup_progress.hpp"
#include "supervised.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace macha {


Service::Service(Config config, ClusterKeys keys, NodeRuntime::StartupStageHook startup_stage_hook,
                 MaintenanceStageHook maintenance_stage_hook,
                 StartupStallHandler startup_stall_handler, ServiceInstruments instruments)
    : clock_(instruments.clock ? std::move(instruments.clock)
                               : std::make_shared<SystemMaintenanceClock>()),
      maintenance_trace_(std::move(instruments.trace)),
      lifecycle_(std::move(instruments.lifecycle)), node_(std::move(config), keys, std::move(startup_stage_hook),
            [clock = clock_] { return clock->now(); }), cluster_status_(node_),
      session_api_(node_), users_api_(node_),
      web_(node_.config().web, node_.config().catalogue.api.compression),
      maintenance_stage_hook_(std::move(maintenance_stage_hook)),
      startup_stall_handler_(std::move(startup_stall_handler)) {
    cluster_status_.attach_convergence_diagnostics(
        [this] { return maintenance_port_.metadata_convergence.diagnostics(); });
    // Installed once, for the life of the Service, rather than re-attached
    // whenever a mount comes and goes: the registry already answers "is there
    // a frontend right now", and a supervised FUSE can be rebuilt underneath
    // this provider any number of times.
    cluster_status_.attach_fuse_diagnostics(
        [this]() -> std::optional<FuseFrontendDiagnostics> {
            auto frontend = registry_.fuse();
            if (!frontend)
                return std::nullopt;
            return frontend->diagnostics();
        });
    if (node_.config().catalogue.api.enabled) {
        catalogue_http_ = std::make_unique<HttpServer>(
            node_.config().catalogue.api,
            [this](const HttpRequest& request) { return handle_http(request); },
            [this](const HttpRequest& request) { return capability_request(request); },
            [this](std::string_view token) -> std::optional<SessionIdentity> {
                auto session = node_.sessions().validate(token);
                if (!session)
                    return std::nullopt;
                // A user-bound session is only as live as the account behind
                // it. Both lookups are O(1) against this node's own replicas,
                // so an isolated node still answers: a deletion or password
                // change reaches here as a replicated user record, not as an
                // RPC we have to make.
                if (!session->user_id.empty()) {
                    auto user = node_.users().find(session->user_id);
                    if (!user || user->credential_generation != session->credential_generation)
                        return std::nullopt;
                }
                return std::optional(session_identity(*session));
            });
        // What answers on the control lane: liveness, cluster status, the
        // session and account routes. Everything else -- catalogue,
        // playback, the web client -- is the data lane, so a node that is
        // saturated serving fragments still says what is wrong with it.
        catalogue_http_->set_control_prefixes(
            {"/api/v1/health", "/api/v1/status", "/api/v1/session", "/api/v1/users"});
        cluster_status_.attach_http_diagnostics(
            [this]() -> std::optional<HttpServerDiagnostics> {
                if (!catalogue_http_)
                    return std::nullopt;
                return catalogue_http_->diagnostics();
            });
    }
}

// Each of these answers "nothing to report" when this node has no mount --
// not configured for one, or its subsystem is faulted between restarts. The
// shared_ptr is taken for the duration of the call so a subsystem restart
// cannot pull the frontend out from under a handler already inside one.
std::optional<BlockedNamespaceOperation> Service::blocked_namespace_operation() const {
    auto frontend = registry_.fuse();
    return frontend ? frontend->blocked_namespace_operation() : std::nullopt;
}

bool Service::skip_blocked_namespace_operation(uint64_t sequence) {
    auto frontend = registry_.fuse();
    return frontend && frontend->skip_blocked_namespace_operation(sequence);
}

std::vector<ParkedPublication> Service::parked_publications() const {
    auto frontend = registry_.fuse();
    return frontend ? frontend->parked_publications() : std::vector<ParkedPublication>{};
}

bool Service::retry_parked_publication(uint64_t inode) {
    auto frontend = registry_.fuse();
    return frontend && frontend->retry_parked_publication(inode);
}

bool Service::abandon_parked_publication(uint64_t inode) {
    auto frontend = registry_.fuse();
    return frontend && frontend->abandon_parked_publication(inode);
}

Service::~Service() {
    stop();
}

std::string_view Service::required_role(const HttpRequest& request) {
    // Roles are capabilities, not a ladder, so this maps a route to the single
    // capability it needs. Checked once here, before dispatch, rather than per
    // handler -- a check a new route can forget to add is not a gate.
    const bool mutating = request.method == "POST" || request.method == "PUT" ||
                          request.method == "PATCH" || request.method == "DELETE";

    // A session is the caller's own to mint, read and revoke, so gating it
    // would mean needing a role to find out which roles you have. It is the one
    // route that requires a valid session and no role at all.
    if (request.path == "/api/v1/session")
        return {};

    // Cluster and node health. Until 0.38.5 this carried no role, on the
    // argument that an importer watching an ingest is the person who most needs
    // it -- which is right, and is now expressed as an implication in
    // expand_roles() instead: every capability implies view_status, so that
    // account still sees it. What carrying no role could not express is the
    // other case: an account the cluster granted nothing -- a roles-less
    // anonymous session in a registered-users-only deployment -- was still
    // shown the cluster's topology, node names, capacities and diagnostics.
    //
    // Liveness probing is not this route. /api/v1/health answers that with no
    // token and no role; anything wanting more than "is this node serving" is
    // asking about the cluster and needs the capability that says so.
    if (request.path == "/api/v1/status" || request.path.starts_with("/api/v1/status/"))
        // A connectivity check is not a read: it makes this node dial every
        // peer on the caller's say-so.
        return mutating ? role_manager : role_view_status;

    // Provider search and artwork listings are not reads either: they make
    // this node call TMDB or MusicBrainz on the caller's say-so.
    if (request.path.starts_with("/api/v1/manage/providers"))
        return role_manager;

    // Managing accounts. "me" is the exception: everyone may change their own
    // password, and UsersApi refuses a role change made that way.
    if (UsersApi::routes(request.path))
        return request.path == "/api/v1/users/me" ? role_media_viewer : role_manage_users;

    // Acquisition is importing: torrents and ingest jobs.
    if (mutating && (request.path.starts_with("/api/v1/acquisition") ||
                     request.path.starts_with("/api/v1/ingest") ||
                     request.path.starts_with("/api/v1/torrent")))
        return role_importer;

    // Everything else that changes state: catalogue matches, files, namespaces,
    // and cluster identity associations.
    if (mutating && (request.path.starts_with("/api/v1/manage") ||
                     request.path.starts_with("/api/v1/catalogue")))
        return role_manager;

    // Reads, playback and cluster status.
    return role_media_viewer;
}

HttpResponse Service::handle_http(const HttpRequest& request) {
    // Liveness, for anything that needs to know whether this node is serving
    // before it has a token -- a load balancer, an uptime monitor, a client
    // choosing an endpoint. It carries the running version -- deliberately,
    // since 0.42.1: reading what a node is running without a token is how
    // every on-box check and every deploy verification is done, and a version
    // is a fact about this process rather than about the cluster. Nothing
    // beyond that: no node identity, no topology, no configuration. It is
    // reachable unauthenticated from wherever the API is reachable, so
    // everything that describes the cluster lives behind /api/v1/status and
    // the view_status role.
    if (request.path == "/api/v1/health")
        return health_response();
    // Session creation/introspection must work while local services are
    // still recovering -- the control plane is online long before local
    // storage and metadata finish recovery.
    if (request.path == "/api/v1/session")
        return session_api_.handle(request);

    if (const auto role = required_role(request); !role.empty() && request.session &&
                                                  !std::count(request.session->roles.begin(),
                                                              request.session->roles.end(), role))
        return http_error(403, "forbidden",
                          "this action requires the '" + std::string(role) + "' role");

    // After the gate, not before it: dispatching Status first was what made its
    // required_role() unreachable. It stays ahead of the services_ready_ check
    // below, because a node that is still recovering is exactly when it is
    // asked what is wrong.
    if (request.path == "/api/v1/status" || request.path.starts_with("/api/v1/status/"))
        return cluster_status_.handle(request);

    // Account management does not depend on local storage or metadata, for the
    // same reason session creation does not: an operator must be able to fix
    // an account on a node that is still recovering.
    if (UsersApi::routes(request.path))
        return users_api_.handle(request);

    // The web client is static files and does not depend on local services,
    // so it loads while they are still recovering -- the client can then show
    // what Status says rather than failing to load at all. Everything under
    // /api stays the server's, 404s included: WebApi refuses those itself,
    // but the routing says so too, so the isolation is visible here.
    if (web_.enabled() && !WebApi::api_path(request.path))
        return web_.handle(request);

    if (!services_ready_.load(std::memory_order_acquire)) {
        if (startup_failed_.load(std::memory_order_acquire)) {
            std::lock_guard lock(startup_mutex_);
            return http_error(503, "startup_failed",
                              startup_error_.empty() ? "server startup failed" : startup_error_);
        }
        return http_error(503, "service_recovering",
                          "server control plane is online; local services are still recovering");
    }

    if (request.path.starts_with("/api/v1/playback/"))
        return services_->streaming().handle(request);
    if (request.path.starts_with("/api/v1/ingest/") ||
        request.path.starts_with("/api/v1/torrents/"))
        return services_->acquisition_api().handle(request);
    constexpr std::string_view blocked_namespace_op_path =
        "/api/v1/manage/filesystem/blocked-namespace-operation";
    if (request.path == blocked_namespace_op_path) {
        if (request.method != "GET")
            return http_error(405, "method", "GET required");
        auto blocked = blocked_namespace_operation();
        if (!blocked)
            return http_error(404, "not_found",
                              "no namespace operation is currently blocked");
        Json::Object out{{"sequence", blocked->sequence},
                         {"kind", blocked->kind},
                         {"path", blocked->path},
                         {"error_code", blocked->error_code},
                         {"error_message", blocked->error_message},
                         {"blocked_for_ms", static_cast<uint64_t>(blocked->blocked_for.count())}};
        if (!blocked->secondary_path.empty())
            out["destination_path"] = blocked->secondary_path;
        return http_json(200, Json(std::move(out)).dump());
    }
    if (request.path == std::string(blocked_namespace_op_path) + "/skip") {
        if (request.method != "POST")
            return http_error(405, "method", "POST required");
        auto it = request.query.find("sequence");
        uint64_t sequence = 0;
        if (it == request.query.end() || it->second.empty())
            return http_error(400, "missing_sequence", "sequence query parameter is required");
        auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(),
                                         sequence);
        if (ec != std::errc{} || end != it->second.data() + it->second.size())
            return http_error(400, "bad_sequence", "sequence must be a non-negative integer");
        // Operator confirms the exact sequence number from a prior GET so a
        // race where the wedge changed underneath them fails closed instead
        // of silently abandoning a different operation.
        if (!skip_blocked_namespace_operation(sequence))
            return http_error(409, "not_blocked",
                              "no namespace operation with this sequence is currently blocked");
        return {204, "application/json; charset=utf-8", {}, {}};
    }
    // Discipline 4: standing metadata conflicts, listed and resolved here so
    // a set nobody knew about cannot silently persist (116 of them, 336 KB
    // in every snapshot and every merge delta, on 2026-09-06).
    constexpr std::string_view conflicts_path = "/api/v1/manage/metadata/conflicts";
    if (request.path == conflicts_path) {
        if (request.method != "GET")
            return http_error(405, "method", "GET required");
        auto view = metadata_manager().available_snapshot_view();
        if (!view)
            return http_error(503, "metadata_unavailable", "metadata snapshot not yet available");
        const auto describe = [](const std::optional<FsEntry>& entry) {
            if (!entry)
                return Json(nullptr);
            return Json(Json::Object{
                {"type", entry->type == EntryType::directory ? "directory" : "file"},
                {"size", entry->size},
                {"mtime_ns", static_cast<uint64_t>(entry->mtime_ns)},
                {"version", entry->version},
                {"extents", static_cast<uint64_t>(entry->extents.size())}});
        };
        const auto describe_root = [](const std::optional<ObjectId>& root) {
            return root ? Json(to_string(*root)) : Json(nullptr);
        };
        Json::Array items;
        for (const auto& [id, conflict] : view->snapshot->conflicts) {
            Json::Object item{
                {"id", id},
                {"kind", conflict.kind == MetadataConflictKind::namespace_entry
                             ? "namespace_entry"
                             : "catalogue_root"},
                {"key", conflict.key},
                {"left_head", hex(conflict.left_head.bytes)},
                {"right_head", hex(conflict.right_head.bytes)}};
            if (conflict.kind == MetadataConflictKind::namespace_entry) {
                item["base"] = describe(conflict.base_entry);
                item["left"] = describe(conflict.left_entry);
                item["right"] = describe(conflict.right_entry);
            } else {
                item["base"] = describe_root(conflict.base_catalogue_root);
                item["left"] = describe_root(conflict.left_catalogue_root);
                item["right"] = describe_root(conflict.right_catalogue_root);
            }
            items.push_back(Json(std::move(item)));
        }
        return http_json(200, Json(Json::Object{{"generation", view->generation},
                                                {"conflicts", std::move(items)}})
                                  .dump());
    }
    if (request.path.starts_with(std::string(conflicts_path) + "/")) {
        if (request.method != "POST")
            return http_error(405, "method", "POST required");
        // /conflicts/<id>/resolve?choice=left|right|base
        const auto rest = request.path.substr(conflicts_path.size() + 1);
        const auto slash = rest.find('/');
        if (slash == std::string::npos || rest.substr(slash + 1) != "resolve")
            return http_error(404, "not_found", "expected /conflicts/{id}/resolve");
        const auto id = rest.substr(0, slash);
        auto choice = request.query.find("choice");
        if (choice == request.query.end() ||
            (choice->second != "left" && choice->second != "right" && choice->second != "base"))
            return http_error(400, "bad_choice", "choice query parameter must be left, right or base");
        try {
            if (!metadata_manager().resolve_conflict(id, choice->second))
                return http_error(409, "not_standing",
                                  "no conflict with this id is standing (superseded or already resolved)");
        } catch (const MetadataNotReady& e) {
            return http_error(503, "metadata_unavailable", e.what());
        }
        return {204, "application/json; charset=utf-8", {}, {}};
    }
    constexpr std::string_view parked_path = "/api/v1/manage/filesystem/parked-publications";
    if (request.path == parked_path) {
        if (request.method != "GET")
            return http_error(405, "method", "GET required");
        Json::Array items;
        for (const auto& parked : parked_publications()) {
            items.push_back(Json(Json::Object{
                {"inode", parked.inode},
                {"path", parked.path},
                {"error_code", parked.error_code},
                {"error_message", parked.error_message},
                {"attempts", static_cast<uint64_t>(parked.attempts)},
                {"failing_for_ms", static_cast<uint64_t>(parked.failing_for.count())},
                {"parked_for_ms", static_cast<uint64_t>(parked.parked_for.count())},
                {"pending_bytes", parked.pending_bytes}}));
        }
        return http_json(200, Json(Json::Object{{"parked", std::move(items)}}).dump());
    }
    if (request.path.starts_with(std::string(parked_path) + "/")) {
        if (request.method != "POST")
            return http_error(405, "method", "POST required");
        // /parked-publications/<inode>/retry | /abandon
        const auto rest = request.path.substr(parked_path.size() + 1);
        const auto slash = rest.find('/');
        if (slash == std::string::npos)
            return http_error(404, "not_found", "expected /parked-publications/{inode}/retry|abandon");
        uint64_t inode = 0;
        const auto id = rest.substr(0, slash);
        auto [end, ec] = std::from_chars(id.data(), id.data() + id.size(), inode);
        if (ec != std::errc{} || end != id.data() + id.size())
            return http_error(400, "bad_inode", "inode must be a non-negative integer");
        const auto action = rest.substr(slash + 1);
        bool ok = false;
        if (action == "retry")
            ok = retry_parked_publication(inode);
        else if (action == "abandon")
            ok = abandon_parked_publication(inode);
        else
            return http_error(404, "not_found", "action must be retry or abandon");
        if (!ok)
            return http_error(409, "not_parked",
                              "no parked publication for this inode, or it cannot be "
                              "abandoned while writes are still landing");
        return {204, "application/json; charset=utf-8", {}, {}};
    }
    if (request.path.starts_with("/api/v1/manage"))
        return services_->manage_api().handle(request);
    return services_->catalogue_api().handle(request);
}

HttpResponse Service::health_response() const {
    // Three states, and the HTTP status carries the same answer as the body so
    // a probe that reads neither JSON nor anything else still works: 200 means
    // this node is serving, 503 means it is not yet (or will not without
    // intervention).
    std::string_view state = "ok";
    int status = 200;
    if (startup_failed_.load(std::memory_order_acquire)) {
        state = "failed";
        status = 503;
    } else if (!services_ready_.load(std::memory_order_acquire)) {
        state = "starting";
        status = 503;
    }
    // `service` says what answered, and is the only field a caller may gate on.
    // "I reached something" and "I reached Macha" are different questions, and
    // until 0.42.1 this body could not tell them apart: {"status":"ok"} is what
    // a router admin page, a container probe, or -- the case that prompted this
    // -- a web host serving a single-page app's index document for unknown
    // paths would produce, and a client probing its own origin would adopt
    // itself and then fail every call against a pile of HTML.
    //
    // It is present in every state, both 503s included, precisely because a
    // client probing an address it was handed is most likely to meet a node
    // that is still starting: identity and readiness are different axes, and a
    // caller that can tell them apart waits instead of giving up.
    //
    // `version` is here by an explicit operator decision (2026-09-15), taken
    // against the argument for leaving it out. The objection was that this
    // route needs no token, so naming the build tells an unauthenticated
    // caller which known defects apply. The answer taken: `service` already
    // names the product, and anyone who reads that will try the known Macha
    // exploits regardless -- the version narrows which one they reach for, it
    // does not decide whether they try. What it genuinely adds is a way to
    // index *unpatched* hosts at scale, which is a mass-scanner's economics
    // and not this project's threat model.
    //
    // Still no node id and no topology: those are the cluster's shape and stay
    // behind view_status. A client must gate on `service` alone -- asserting
    // on `version` would break it every release.
    return http_json(status, Json(Json::Object{{"service", std::string("macha")},
                                               {"status", std::string(state)},
                                               {"version", std::string(kServerVersion)}})
                                 .dump());
}

bool Service::capability_request(const HttpRequest& request) {
    // The two routes reachable with no bearer token at all, both of which must
    // stay exempt regardless of local readiness: liveness, and minting the
    // session every other route needs.
    if (request.path == "/api/v1/health")
        return true;
    if (SessionApi::capability_request(request))
        return true;
    // A browser asking for the client itself has no token yet, and cannot get
    // one until the client has loaded and asked for it. Only paths outside
    // /api reach this, and only the configured web root is ever served.
    if (web_.enabled() && !WebApi::api_path(request.path))
        return true;
    if (!services_ready_.load(std::memory_order_acquire))
        return false;
    if (services_->streaming().capability_request(request))
        return true;
    return services_->catalogue_api().capability_request(request);
}

std::string Service::describe_readiness_stall() const {
    const auto readiness = node_.readiness();
    auto phase = [](bool ready) { return ready ? "ready" : "recovering"; };
    return std::string("readiness: control_plane=") +
          (readiness.control_plane_online ? "ready" : "starting") +
          " data_storage=" + phase(readiness.data_storage_ready) +
          " control_storage=" + phase(readiness.control_storage_ready) +
          " cache=" + phase(readiness.cache_ready) +
          " retention=" + phase(readiness.retention_ready) +
          " metadata=" + phase(readiness.metadata_ready) +
          " local_state=" + phase(readiness.local_state_ready) +
          " failed=" + (readiness.failed ? "true" : "false");
}

void Service::wait_services_ready() {
    if (services_ready_.load(std::memory_order_acquire))
        return;
    // Discipline 2: the gate fires on *no progress*, not on elapsed time. A
    // slow recovery that keeps ticking startup_progress() (frames parsed,
    // deltas applied, journal records read, readiness stages) is left alone;
    // one that has not ticked for service_startup_no_progress is a stall. An
    // absolute ceiling is honoured only when configured.
    const auto& config = node_.config();
    const auto started = Clock::now();
    auto last_progress_at = started;
    auto last_progress = startup_progress();
    bool signalled = false;
    std::unique_lock lock(startup_mutex_);
    for (;;) {
        signalled = startup_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
            return services_ready_.load(std::memory_order_acquire) ||
                   startup_failed_.load(std::memory_order_acquire);
        });
        if (signalled)
            break;
        const auto now = Clock::now();
        if (const auto progress = startup_progress(); progress != last_progress) {
            last_progress = progress;
            last_progress_at = now;
        }
        if (config.service_startup_no_progress.count() > 0 &&
            now - last_progress_at >= config.service_startup_no_progress)
            break;
        if (config.service_startup_timeout.count() > 0 &&
            now - started >= config.service_startup_timeout)
            break;
    }
    if (services_ready_.load(std::memory_order_acquire))
        return;
    lock.unlock();
    if (signalled)
        throw std::runtime_error(startup_error_.empty() ? "server startup failed" : startup_error_);

    // Local-state readiness/subsystem construction neither completed nor
    // threw within the configured bound: a suspected internal stall (a lock
    // or lost wakeup somewhere below this point), not a clean, catchable
    // failure. The startup thread may be permanently blocked and can never
    // safely be joined, so ordinary exception unwinding here -- which would
    // destruct this Service and try to join it in stop() -- is not safe.
    // Terminate the process outright and rely on the service supervisor
    // (systemd Restart=on-failure in production) to bring up a fresh,
    // unstuck instance; recovery replay on the next boot is what actually
    // resolves the stalled state, not this process limping on.
    const auto diagnostic = describe_readiness_stall();
    const auto message =
        "service startup stalled: no recovery progress for " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - last_progress_at)
                           .count()) +
        "ms (gate " + std::to_string(config.service_startup_no_progress.count()) +
        "ms, ceiling " + std::to_string(config.service_startup_timeout.count()) + "ms, elapsed " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
                           .count()) +
        "ms); " + diagnostic + "; terminating for restart";
    Log::error(message);
    if (startup_stall_handler_) {
        // Test-only: observe the stall without killing the test process. The
        // underlying startup thread is still stuck; the caller is
        // responsible for releasing whatever it gated on before this Service
        // is destroyed, or its own join in stop() will hang the same way.
        startup_stall_handler_(diagnostic);
        throw std::runtime_error(message);
    }
    std::_Exit(1);
}

void Service::initialise_services(std::stop_token stop) {
    try {
        while (!stop.stop_requested()) {
            if (node_.wait_local_state_ready(std::chrono::milliseconds(100)))
                break;
            const auto readiness = node_.readiness();
            if (readiness.failed)
                throw std::runtime_error(
                    readiness.error.empty() ? "node local-state recovery failed" : readiness.error);
        }
        if (stop.stop_requested())
            return;

        auto services = std::make_unique<NodeServices>(
            node_, registry_, maintenance_port_,
            [this](ServiceEvent event) { signal_maintenance(event); },
            NodeServicesInstruments{clock_, maintenance_trace_, maintenance_stage_hook_, lifecycle_,
                                    constructed_});
        if (stop.stop_requested())
            return;
        services->start();
        // Status reads the graph through providers that outlive it: attached
        // now, detached before the graph stops.
        cluster_status_.attach_subsystem_diagnostics(
            [subsystems = &services->subsystems()] { return subsystems->statuses(); });
        cluster_status_.attach_repair_diagnostics(
            [store = &services->store()] { return store->repair_diagnostics(); });
        cluster_status_.attach_metadata(services->metadata());
        services_ = std::move(services);
        // Seed one initial validation pass. Later metadata/topology events use
        // the edge-triggered high-water object; storage-only events still wake
        // ordinary maintenance without scheduling redundant metadata work.
        maintenance_port_.metadata_convergence.request(node_.known_metadata_generation());
        node_.set_service_event_callback([this](ServiceEvent event) { signal_maintenance(event); });

        note_lifecycle("services ready");
        services_ready_.store(true, std::memory_order_release);
        startup_cv_.notify_all();
        Log::info("server local services ready");
        observations().event(
            {unix_ms(), "services_ready", {{"elapsed_ms", elapsed_us(constructed_) / 1000}}, {}});
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(startup_mutex_);
            startup_error_ = error.what();
        }
        startup_failed_.store(true, std::memory_order_release);
        startup_cv_.notify_all();
        Log::error("server service initialization failed: " + std::string(error.what()));
    }
}

void Service::start() {
    // Before everything else, so a startup that never finishes still leaves
    // its windows behind.
    observation_recorder_ = std::make_unique<ObservationRecorder>(
        observations(),
        std::make_shared<ObservationLog>(
            node_.config().state_path / "observation" / "observations.jsonl", 64ULL << 20),
        std::string(kBuildIdentity), std::chrono::minutes(1),
        [this] { return observation_gauges(); });
    observations().event({unix_ms(), "start", {}, {{"version", std::string(kBuildIdentity)}}});
    note_lifecycle("start observation");
    observation_recorder_->start();

    // Status is the first externally visible service. It depends only on the
    // lightweight node identity/membership state constructed from config, so
    // operators can observe startup even before the cluster listener or any
    // durable backend begins recovery.
    note_lifecycle("start status");
    cluster_status_.start();
    if (catalogue_http_) {
        note_lifecycle("start catalogue-http");
        catalogue_http_->start();
    }

    note_lifecycle("start node");
    node_.start();
    note_lifecycle("start startup");
    startup_ = std::jthread([this](std::stop_token stop) {
        run_supervised_once("service-startup", [this, stop] { initialise_services(stop); });
    });
}

void Service::request_stop() {
    if (startup_.joinable())
        startup_.request_stop();
    if (services_ready_.load(std::memory_order_acquire))
        services_->request_stop();
    note_lifecycle("request_stop status");
    cluster_status_.request_stop();
    if (catalogue_http_) {
        note_lifecycle("request_stop catalogue-http");
        catalogue_http_->request_stop();
    }
    note_lifecycle("request_stop node");
    node_.request_stop();
    startup_cv_.notify_all();
}

void Service::stop() {
    if (stopped_.exchange(true))
        return;
    Log::debug("shutdown: Service::stop begin");
    const auto stop_started = Clock::now();
    // Gauges read services this stop tears down; the final window goes
    // without them.
    observation_stopping_.store(true, std::memory_order_release);
    // The node's services stop first and completely, while the node still
    // admits DATA work and outbound RPC: a plugin writes into the store until
    // it is stopped. A startup still in progress is ended first; it may be
    // waiting on recovery, which the node's stop request ends.
    if (startup_.joinable()) {
        startup_.request_stop();
        if (!services_ready_.load(std::memory_order_acquire))
            node_.request_stop();
        startup_.join();
    }
    // The HTTP server routes requests into the services, so it stops before
    // them; Status reads them only through providers, detached first, and
    // stays until the node goes.
    if (catalogue_http_) {
        note_lifecycle("stop catalogue-http");
        catalogue_http_->stop();
    }
    if (services_) {
        cluster_status_.detach_metadata();
        cluster_status_.detach_subsystem_diagnostics();
        cluster_status_.detach_repair_diagnostics();
        node_.set_service_event_callback({});
        services_->stop();
    }
    request_stop();
    note_lifecycle("stop status");
    cluster_status_.stop();
    Log::debug("shutdown: NodeRuntime::stop calling");
    note_lifecycle("stop node");
    node_.stop();
    Log::debug("shutdown: Service::stop complete");
    if (observation_recorder_) {
        observations().event(
            {unix_ms(), "shutdown", {{"elapsed_ms", elapsed_us(stop_started) / 1000}}, {}});
        observation_recorder_->stop();
        observation_recorder_.reset();
    }
}

std::map<std::string, uint64_t> Service::observation_gauges() {
    std::map<std::string, uint64_t> gauges{{"rss_bytes", process_resident_bytes()},
                                           {"maintenance_wakeups", maintenance_wakeups()}};
    if (!services_ready_.load(std::memory_order_acquire) ||
        observation_stopping_.load(std::memory_order_acquire))
        return gauges;
    const auto idle_ms = [](std::chrono::milliseconds idle) {
        return static_cast<uint64_t>(std::max<int64_t>(0, idle.count()));
    };
    gauges["foreground_idle_ms"] = idle_ms(services_->store().foreground_idle_for());
    gauges["interactive_idle_ms"] = idle_ms(services_->store().interactive_idle_for());
    gauges["loader_idle_ms"] = idle_ms(services_->store().loader_idle_for());
    // Cumulative since start, as Status reports them; a window's rate is the
    // difference between consecutive windows.
    const auto repair = services_->store().repair_diagnostics();
    gauges["repair_push_examined"] = repair.push_examined;
    gauges["repair_pull_examined"] = repair.pull_examined;
    gauges["repair_bytes_transferred"] = repair.bytes_transferred;
    gauges["repair_passes_completed"] = repair.passes_completed;
    gauges["repair_pull_unsourceable"] = repair.pull_unsourceable;
    gauges["repair_gate_ran"] = repair.gate_ran;
    gauges["repair_gate_share"] = repair.gate_share;
    gauges["repair_gate_credit"] = repair.gate_credit;
    gauges["repair_prompt_copies"] = repair.prompt_copies;
    if (auto frontend = registry_.fuse()) {
        const auto fuse = frontend->diagnostics();
        gauges["fuse_publications_completed"] = fuse.data_publications_completed;
        gauges["fuse_publication_bytes_committed"] = fuse.data_publication_bytes_committed;
        gauges["fuse_publication_bytes_confirmed"] = fuse.data_publication_bytes_confirmed;
        gauges["fuse_spool_bytes"] = fuse.spool_bytes;
        gauges["fuse_parked_publications"] = fuse.parked_publications;
    }
    return gauges;
}

void Service::signal_maintenance(ServiceEvent event) {
    if (event == ServiceEvent::metadata && services_ready_.load(std::memory_order_acquire))
        services_->media_information().request_prune();
    bool wake = true;
    if (event == ServiceEvent::metadata || event == ServiceEvent::topology)
        wake = maintenance_port_.metadata_convergence.request(node_.known_metadata_generation());
    maintenance_port_.event.fetch_add(1, std::memory_order_release);
    // A burst received while its convergence pass is already queued/running
    // only advances the high-water epoch. The active owner observes that epoch
    // and schedules one follow-up; waking the same owner for every notice adds
    // no information and recreates the notification storm this state replaces.
    if (wake)
        maintenance_port_.wait_cv.notify_all();
}


} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"
#include "namespace_control_store.hpp"
#include "diagnostics.hpp"
#include "fuse_frontend.hpp"
#include "fuse_subsystem.hpp"
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

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig& policy) {
    // Settled object passes may sleep for minutes, but metadata/catalogue control
    // convergence should still be verified regularly.
    return std::clamp(policy.no_progress_backoff, std::chrono::milliseconds(5000),
                      std::chrono::milliseconds(30000));
}

namespace {

std::filesystem::path scrub_due_path(const std::filesystem::path& state_path) {
    return state_path / "maintenance" / "scrub.next";
}

void persist_scrub_due(const std::filesystem::path& path, uint64_t due_unix_ms) {
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write scrub schedule " + temp.string());
        out << due_unix_ms << '\n';
        out.flush();
        if (!out)
            throw std::runtime_error("cannot flush scrub schedule " + temp.string());
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(temp, error);
        throw std::runtime_error("cannot publish scrub schedule " + path.string());
    }
}

uint64_t initialise_scrub_due(const std::filesystem::path& state_path,
                              std::chrono::milliseconds interval) {
    const auto path = scrub_due_path(state_path);
    {
        std::ifstream in(path);
        uint64_t due{};
        if (in >> due && due)
            return due;
    }

    const auto now = unix_ms();
    const auto interval_ms = static_cast<uint64_t>(std::max<int64_t>(1, interval.count()));
    const auto due = now > std::numeric_limits<uint64_t>::max() - interval_ms
                         ? std::numeric_limits<uint64_t>::max()
                         : now + interval_ms;
    try {
        persist_scrub_due(path, due);
    } catch (const std::exception& error) {
        // The schedule file is only a neighbourliness hint. Failure to persist it
        // must not stop the server; this process still honours the in-memory due time.
        Log::debug("maintenance: scrub schedule persistence unavailable: " +
                   std::string(error.what()));
    }
    return due;
}

void log_slow_stage(std::string_view stage, Clock::time_point started,
                    const std::string& detail = {}) {
    const auto ms = elapsed_ms(started);
    if (ms < 50 || !Log::enabled(LogLevel::all))
        return;
    Log::trace("DIAG maintenance-stage stage=" + std::string(stage) + " elapsed_ms=" +
               std::to_string(ms) + (detail.empty() ? std::string{} : " " + detail));
}
} // namespace

Service::Service(Config config, ClusterKeys keys, NodeRuntime::StartupStageHook startup_stage_hook,
                 MaintenanceStageHook maintenance_stage_hook,
                 StartupStallHandler startup_stall_handler)
    : node_(std::move(config), keys, std::move(startup_stage_hook)), cluster_status_(node_),
      subsystems_(node_.config().plugin_path.value_or(std::filesystem::path{})),
      session_api_(node_), users_api_(node_),
      web_(node_.config().web, node_.config().catalogue.api.compression),
      startup_stall_handler_(std::move(startup_stall_handler)),
      maintenance_stage_hook_(std::move(maintenance_stage_hook)) {
    cluster_status_.attach_convergence_diagnostics(
        [this] { return metadata_convergence_.diagnostics(); });
    cluster_status_.attach_subsystem_diagnostics([this] { return subsystems_.statuses(); });
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
        return streaming_->handle(request);
    if (request.path.starts_with("/api/v1/ingest/") ||
        request.path.starts_with("/api/v1/torrents/"))
        return acquisition_api_->handle(request);
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
        return manage_api_->handle(request);
    return catalogue_api_->handle(request);
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
    if (streaming_ && streaming_->capability_request(request))
        return true;
    return catalogue_api_ && catalogue_api_->capability_request(request);
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

        auto store = std::make_unique<DistributedStore>(node_);
        auto metadata = std::make_unique<MetadataManager>(node_);
        auto catalogue = std::make_unique<CatalogueManager>(node_, *store, *metadata);
        auto fs = std::make_unique<FileSystem>(node_, *store, *metadata, &playback_);
        auto catalogue_hints = std::make_unique<CatalogueHintQueue>(node_.config().state_path);
        std::shared_ptr<MediaEngine> media_engine;
        if (node_.config().streaming.enabled)
            media_engine = make_libav_media_engine(node_.config().streaming);
        auto media_information = std::make_unique<MediaInformationService>(
            *fs, *catalogue, media_engine, node_.config().state_path);
        auto scanner = std::make_unique<CatalogueScanner>(node_, *fs, *catalogue, *catalogue_hints,
                                                          node_.config().catalogue.scanner,
                                                          std::unique_ptr<HttpClient>{},
                                                          std::chrono::seconds(5), media_engine,
                                                          media_information.get());
        auto hydration = std::make_unique<HydrationManager>(*store, playback_, *fs, *catalogue,
                                                            node_.config().hydration,
                                                            node_.config().read_ahead_extents);
        auto ingest = std::make_unique<IngestManager>(
            node_, *fs, *catalogue_hints, node_.config().ingest, media_information.get());
        auto torrent_search = std::make_unique<TorrentSearchManager>(node_.config().torrent);
        auto acquisition_api =
            std::make_unique<AcquisitionApi>(*ingest, registry_, *torrent_search);
        auto catalogue_api = std::make_unique<CatalogueApi>(
            *catalogue, *catalogue_hints,
            [scanner_ptr = scanner.get()](const std::vector<std::string>& media_ids) {
                scanner_ptr->request_media_rescan(media_ids);
            },
            [scanner_ptr = scanner.get()](const std::vector<std::string>& media_ids) {
                return scanner_ptr->request_media_profiles(media_ids);
            },
            [information = media_information.get(), fs_ptr = fs.get()](const std::string& media_id)
                -> std::optional<MediaProbeResult> {
                // Facts on demand: resolve at foreground priority and persist,
                // so a client asking what a file is never gets "not yet".
                if (!information) return std::nullopt;
                auto found = fs_ptr->find_media(media_id);
                if (!found) return std::nullopt;
                return information->resolve_playback(media_id, found->first, found->second,
                                                     Clock::now() + std::chrono::seconds(30));
            },
            node_.config().catalogue.api.artwork_capability_ttl);
        auto manage_api = std::make_unique<ManageApi>(node_, *metadata, *fs, *catalogue,
                                                      *catalogue_hints, *scanner);
        auto streaming = std::make_unique<PlaybackManager>(
            *fs, *catalogue, node_.config().catalogue.api, node_.config().streaming,
            media_engine,
            [scanner_ptr = scanner.get()](const std::vector<std::string>& media_ids) {
                return scanner_ptr->request_media_profiles(media_ids);
            }, media_information.get());

        metadata->set_publication_retention([this](const MetadataPublicationContext& context) {
            retain_metadata_publication(context);
        });

        store_ = std::move(store);
        metadata->set_namespace_store(store_.get());
        // Repair is the only component that learns an object is unobtainable,
        // and it learns it in the ordinary course of a maintenance pass. Wire
        // its counters to Status now that the store exists.
        cluster_status_.attach_repair_diagnostics(
            [store = store_.get()] { return store->repair_diagnostics(); });
        metadata_ = std::move(metadata);
        catalogue_ = std::move(catalogue);
        fs_ = std::move(fs);
        catalogue_hints_ = std::move(catalogue_hints);
        media_information_ = std::move(media_information);
        scanner_ = std::move(scanner);
        hydration_ = std::move(hydration);
        ingest_ = std::move(ingest);
        torrent_search_ = std::move(torrent_search);
        acquisition_api_ = std::move(acquisition_api);
        catalogue_api_ = std::move(catalogue_api);
        manage_api_ = std::move(manage_api);
        streaming_ = std::move(streaming);

        if (stop.stop_requested())
            return;

        media_information_->start();
        ingest_->start();
        // Only now: a subsystem plugin's context hands out references to the
        // services above (the torrent plugin needs IngestManager), and none
        // of them existed when this Service was constructed.
        //
        // FUSE is one of them now (libmacha-fuse), discovered from
        // plugin_path exactly as the torrent plugin is. What that buys is the
        // whole point of the exercise: a frontend whose journal replay throws
        // faults that subsystem and is retried, instead of unwinding to
        // main() and taking metadata, RPC, the HTTP API and playback down
        // with it. See TODO/2026-09-14-fuse-supervised-subsystem-plan.md.
        SubsystemContext context;
        context.config = &node_.config();
        context.node = &node_;
        context.ingest = ingest_.get();
        context.registry = &registry_;
        context.filesystem = fs_.get();
        context.hydration = hydration_.get();
        subsystems_.start(context);
        streaming_->start();
        scanner_->start();
        hydration_->start();
        cluster_status_.attach_metadata(*metadata_);
        // Seed one initial validation pass. Later metadata/topology events use
        // the edge-triggered high-water object; storage-only events still wake
        // ordinary maintenance without scheduling redundant metadata work.
        metadata_convergence_.request(node_.known_metadata_generation());
        node_.set_service_event_callback([this](ServiceEvent event) { signal_maintenance(event); });
        maintenance_ = std::jthread([this](std::stop_token maintenance_stop) {
            run_supervised("service-maintenance", [this, maintenance_stop] { loop(maintenance_stop); });
        });

        services_ready_.store(true, std::memory_order_release);
        startup_cv_.notify_all();
        Log::info("server local services ready");
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
    // Status is the first externally visible service. It depends only on the
    // lightweight node identity/membership state constructed from config, so
    // operators can observe startup even before the cluster listener or any
    // durable backend begins recovery.
    cluster_status_.start();
    if (catalogue_http_)
        catalogue_http_->start();

    node_.start();
    startup_ = std::jthread([this](std::stop_token stop) {
        run_supervised("service-startup", [this, stop] { initialise_services(stop); });
    });
}

void Service::request_stop() {
    if (startup_.joinable())
        startup_.request_stop();
    if (fs_)
        fs_->request_io_cancellation();
    if (ingest_)
        ingest_->request_stop();
    if (scanner_)
        scanner_->request_stop();
    if (media_information_)
        media_information_->request_stop();
    if (hydration_)
        hydration_->request_stop();
    cluster_status_.request_stop();
    if (catalogue_http_)
        catalogue_http_->request_stop();
    if (streaming_)
        streaming_->request_stop();
    if (manage_api_)
        manage_api_->request_stop();
    if (maintenance_.joinable()) {
        maintenance_.request_stop();
        maintenance_wait_cv_.notify_all();
    }
    node_.request_stop();
    // Service maintenance performs synchronous control-replication calls. A
    // thread stop token wakes its event wait but cannot complete an RPC future.
    // Close client routes now so every pending call fails promptly before
    // Service::stop joins the maintenance owner. NodeRuntime::stop later closes
    // the server and completes the remaining node-owned teardown.
    node_.cancel_outbound_calls();
    startup_cv_.notify_all();
}

void Service::stop() {
    Log::debug("shutdown: Service::stop begin");
    request_stop();
    if (startup_.joinable())
        startup_.join();
    // Before ingest: a subsystem plugin holds references to the services
    // below it (the torrent plugin submits completed downloads to ingest), so
    // every plugin instance must be destroyed while they are all still alive.
    subsystems_.stop();
    if (ingest_)
        ingest_->stop();
    if (scanner_)
        scanner_->stop();
    if (media_information_)
        media_information_->stop();
    if (hydration_)
        hydration_->stop();
    cluster_status_.detach_metadata();
    cluster_status_.detach_subsystem_diagnostics();
    // The provider holds a raw pointer into store_, which is declared after
    // cluster_status_ and therefore destroyed before it. Drop it here rather
    // than relying on nothing calling Status during teardown.
    cluster_status_.detach_repair_diagnostics();
    cluster_status_.stop();
    if (catalogue_http_)
        catalogue_http_->stop();
    if (streaming_)
        streaming_->stop();
    if (manage_api_)
        manage_api_->stop();
    if (maintenance_.joinable()) {
        Log::debug("shutdown: service maintenance request_stop");
        maintenance_.request_stop();
        Log::debug("shutdown: service maintenance joining");
        maintenance_.join();
        Log::debug("shutdown: service maintenance joined");
    }
    node_.set_service_event_callback({});
    Log::debug("shutdown: NodeRuntime::stop calling");
    node_.stop();
    Log::debug("shutdown: Service::stop complete");
}

void Service::signal_maintenance(ServiceEvent event) {
    if (event == ServiceEvent::metadata && media_information_)
        media_information_->request_prune();
    bool wake = true;
    if (event == ServiceEvent::metadata || event == ServiceEvent::topology)
        wake = metadata_convergence_.request(node_.known_metadata_generation());
    maintenance_event_.fetch_add(1, std::memory_order_release);
    // A burst received while its convergence pass is already queued/running
    // only advances the high-water epoch. The active owner observes that epoch
    // and schedules one follow-up; waking the same owner for every notice adds
    // no information and recreates the notification storm this state replaces.
    if (wake)
        maintenance_wait_cv_.notify_all();
}

void Service::retain_metadata_publication(const MetadataPublicationContext& context) {
    const RetentionDot dot{context.origin, context.sequence};
    std::vector<ObjectId> data;
    std::vector<ObjectId> control;
    // Phase timing for the pre-publication barrier: on the live cluster
    // (2026-09-07) mutations spent 9-15 s here while retain_data() itself
    // reported nothing over 250 ms, so the seconds were in the collection
    // step (a full parent snapshot decode, catalogue root diffs) or the
    // CONTROL claim. Name the phase instead of guessing.
    const auto barrier_started = Clock::now();
    uint64_t decode_ms = 0, collect_ms = 0, catalogue_ms = 0, data_ms = 0, control_ms = 0;
    const auto since_ms = [](Clock::time_point t) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
    };
    const auto report = [&](const char* outcome) {
        const auto total = since_ms(barrier_started);
        const bool failed = std::string_view(outcome) != "ok";
        if ((total >= 250 || failed) && Log::enabled(LogLevel::debug))
            Log::debug("metadata retention barrier dot=" + to_string(dot.origin).substr(0, 6) +
                       ":" + std::to_string(dot.sequence) + " total_ms=" + std::to_string(total) +
                       " decode_ms=" + std::to_string(decode_ms) +
                       " collect_ms=" + std::to_string(collect_ms) +
                       " catalogue_ms=" + std::to_string(catalogue_ms) +
                       " data_ms=" + std::to_string(data_ms) +
                       " control_ms=" + std::to_string(control_ms) +
                       " data_objects=" + std::to_string(data.size()) +
                       " control_objects=" + std::to_string(control.size()) +
                       " outcome=" + outcome);
    };

    auto add_entry = [&](const FsEntry& entry) {
        if (entry.type != EntryType::file)
            return;
        for (const auto& extent : entry.extents)
            if (!extent.hole)
                data.push_back(extent.id);
    };

    const auto decode_started = Clock::now();
    auto before = decode_snapshot(context.parent.payload);
    decode_ms = since_ms(decode_started);
    const auto collect_started = Clock::now();
    // Both namespaces below may be trees rather than maps, so every read of
    // them goes through the namespace primitives. These are retention claims:
    // an entry missed here is an object that never acquires liveness evidence
    // and can be collected while it is still referenced.
    auto namespace_nodes = ControlNamespaceNodeStore::for_reading(node_, *store_);
    const bool establish_baseline =
        !before.retention_baseline_complete && context.proposed.retention_baseline_complete;
    if (establish_baseline) {
        // Migration safety: before protocol-20 retention-aware GC is enabled for
        // an upgraded namespace, every object reachable from the reconciled
        // migration view must acquire physical liveness evidence. This is a
        // one-time potentially-large publication; normal partition-time GC does
        // not require global convergence after the baseline exists.
        for_each_namespace_entry(context.proposed, &namespace_nodes,
                                 [&](const std::string&, const FsEntry& entry) {
                                     add_entry(entry);
                                 });
        const auto conflict_extents = metadata_conflict_extent_roots(context.proposed);
        data.insert(data.end(), conflict_extents.begin(), conflict_extents.end());
        for (const auto& root : metadata_catalogue_root_set(context.proposed)) {
            auto objects = catalogue_->retention_objects(std::nullopt, root);
            data.insert(data.end(), objects.data.begin(), objects.data.end());
            control.insert(control.end(), objects.control.begin(), objects.control.end());
        }
        // The namespace tree is control objects too, every one of them
        // reachable from the root and none of them from anything else.
        if (context.proposed.namespace_root)
            collect_namespace_tree_nodes(*context.proposed.namespace_root, namespace_nodes,
                                         control);
    } else {
        if (context.delta) {
            for (const auto& [_, entry] : context.delta->upsert_entries)
                add_entry(entry);
            // A DLT8 append carries only the new extents, but the semantic
            // change is to the whole file: like the upsert it replaces, it
            // needs a fresh retention dot on every extent the file now holds
            // (a touch that carries no extents included), or a concurrent
            // delete could release the inherited claim.
            for (const auto& [path, _] : context.delta->append_entries) {
                if (auto found = namespace_entry(context.proposed, &namespace_nodes, path))
                    add_entry(*found);
            }
        } else {
            // The no-delta path: rediscover what changed by comparing the
            // two namespaces entry by entry. Under trees that is a walk of
            // one plus a lookup per path in the other, which is worse than
            // the two map walks it replaces -- and it is exactly the cost
            // Stage C removes by carrying the change set into the commit
            // instead. This branch is the fallback; every ordinary mutation
            // arrives with a delta and takes the cheap path above.
            for_each_namespace_entry(
                context.proposed, &namespace_nodes,
                [&](const std::string& path, const FsEntry& entry) {
                    const auto found = namespace_entry(before, &namespace_nodes, path);
                    if (!found || *found != entry)
                        add_entry(entry);
                });
        }

        // The tree nodes this commit introduced need claims exactly as the
        // catalogue shards it changed do. Without them the nodes that say
        // where every file lives are unreferenced control objects to the
        // collector, which is what they were from the cutover until this line.
        if (context.proposed.namespace_root &&
            before.namespace_root != context.proposed.namespace_root)
            collect_namespace_tree_changes(before.namespace_root, *context.proposed.namespace_root,
                                           namespace_nodes, control);

        const bool catalogue_changed =
            context.delta ? context.delta->catalogue != CatalogueDelta::unchanged
                          : before.catalogue_root != context.proposed.catalogue_root;
        if (catalogue_changed && context.proposed.catalogue_root) {
            const auto catalogue_started = Clock::now();
            auto objects = catalogue_->retention_objects(before.catalogue_root,
                                                         context.proposed.catalogue_root);
            catalogue_ms += since_ms(catalogue_started);
            data.insert(data.end(), objects.data.begin(), objects.data.end());
            control.insert(control.end(), objects.control.begin(), objects.control.end());
        }
        // A reconciliation may preserve catalogue conflict alternatives which
        // are not the effective root. New alternatives must be retained before
        // the merge commit can become accepted.
        if (!context.delta) {
            auto before_roots = metadata_catalogue_root_set(before);
            auto after_roots = metadata_catalogue_root_set(context.proposed);
            for (const auto& root : after_roots) {
                if (before_roots.contains(root))
                    continue;
                auto objects = catalogue_->retention_objects(std::nullopt, root);
                data.insert(data.end(), objects.data.begin(), objects.data.end());
                control.insert(control.end(), objects.control.begin(), objects.control.end());
            }
        }
    }

    std::sort(data.begin(), data.end());
    data.erase(std::unique(data.begin(), data.end()), data.end());
    std::sort(control.begin(), control.end());
    control.erase(std::unique(control.begin(), control.end()), control.end());
    collect_ms = since_ms(collect_started) - catalogue_ms;

    const auto data_started = Clock::now();
    const bool data_ok = data.empty() || store_->retain_data(data, dot);
    data_ms = since_ms(data_started);
    if (!data_ok) {
        report("data-floor-unavailable");
        throw MetadataNotReady("DATA retention floor unavailable before metadata publication");
    }
    const auto control_started = Clock::now();
    const bool control_ok =
        control.empty() ||
        store_->retain_control(control, dot, context.proposed.metadata_write_replicas_required);
    control_ms = since_ms(control_started);
    if (!control_ok) {
        report("control-floor-unavailable");
        throw MetadataNotReady("CONTROL retention floor unavailable before metadata publication");
    }
    report("ok");
    signal_maintenance(ServiceEvent::storage);
}

std::vector<GarbageRef> Service::collect_garbage(const std::vector<GarbageRef>& garbage) {
    const auto grace = node_.config().maintenance.garbage_grace;
    const auto grace_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(grace).count();
    const auto now_ns = wall_time_ns();
    std::vector<GarbageRef> matured;
    matured.reserve(garbage.size());

    for (const auto& candidate : garbage) {
        // A zero retirement time denotes a legacy tombstone. It is deliberately
        // ineligible until maintain_garbage_metadata() stamps it into the new
        // lifecycle, giving existing stores a fresh full grace period on upgrade.
        if (candidate.retired_at_ns <= 0 || now_ns < candidate.retired_at_ns ||
            now_ns - candidate.retired_at_ns < grace_ns)
            continue;

        // A matured tombstone no longer protects the object from the physical
        // reachability sweep. Do not delete authoritative bytes here: the sweep
        // performs an atomic age-check-and-remove, so a recently reaffirmed
        // identical object survives even if this retirement view is stale. A
        // cache copy is disposable and can be dropped immediately.
        node_.block_cache().remove(candidate.id);
        matured.push_back(candidate);
    }
    return matured;
}

void Service::maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                        const std::vector<GarbageRef>& stamp) {
    if (erase.empty() && stamp.empty())
        return;

    std::map<ObjectId, GarbageRef> erase_expected;
    std::map<ObjectId, GarbageRef> stamp_expected;
    for (const auto& candidate : erase)
        erase_expected[candidate.id] = candidate;
    for (const auto& candidate : stamp)
        stamp_expected[candidate.id] = candidate;

    metadata_->mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        std::erase_if(snapshot.garbage, [&](const GarbageRef& current) {
            auto expected = erase_expected.find(current.id);
            const bool remove = expected != erase_expected.end() && expected->second == current;
            if (remove)
                delta.erase_garbage.push_back(current.id);
            return remove;
        });

        const auto retired = wall_time_ns();
        for (auto& current : snapshot.garbage) {
            auto expected = stamp_expected.find(current.id);
            if (expected == stamp_expected.end() || expected->second != current)
                continue;
            // Legacy tombstones have neither a retirement time nor an ABA token.
            // Stamping rather than immediately collecting them preserves old
            // storage safely while allowing them to leave metadata after grace.
            current.retired_at_ns = retired;
            current.retirement_id = random_node_id();
            delta.upsert_garbage.push_back(current);
        }
        std::sort(delta.erase_garbage.begin(), delta.erase_garbage.end());
        delta.erase_garbage.erase(
            std::unique(delta.erase_garbage.begin(), delta.erase_garbage.end()),
            delta.erase_garbage.end());
    });
}

void Service::loop(std::stop_token stop) {
    const auto& policy = node_.config().maintenance;
    ThreadCpuReporter cpu_reporter("macha-maint", std::chrono::seconds(5), true);
    auto last_wall = Clock::now();
    auto last_cpu = std::clock();
    uint64_t last_metadata_remote_epoch = node_.remote_metadata_epoch();
    uint64_t last_metadata_demand_epoch{};
    std::vector<NodeId> last_active_nodes;
    bool metadata_dirty = metadata_convergence_.pending();
    bool catalogue_dirty = true;
    auto metadata_retry_due = Clock::time_point{};
    auto metadata_retry_backoff = maintenance_background_interval(policy);
    auto catalogue_retry_due = Clock::time_point{};
    auto formation_settle_due = Clock::time_point{};
    uint64_t last_local_metadata_generation = node_.metadata_replica().committed_generation();
    uint64_t observed_event = maintenance_event_.load(std::memory_order_acquire);
    auto network_quiescent_until = Clock::time_point{};
    auto local_quiescent_until = Clock::time_point{};
    auto gc_quiescent_until = Clock::time_point{};
    auto scrub_due_unix_ms = initialise_scrub_due(node_.config().state_path, policy.scrub_interval);
    double network_credit = 0.0;
    double local_credit = 0.0;
    // Replica repair shares time with every higher class by weight, in the
    // same duty-cycle form the FUSE frontend uses for viewer:loader. Until
    // 0.59.0 repair was switched off outright while anything was busy (a zero
    // busy fraction, and a yield on any activity), so a node that was always
    // ingesting never repaired: gbni-1 was still short of a copy of roughly
    // 0.9 TB when es-1 went on 2026-09-24, and that data was lost with it.
    WeightedLoaderService repair_share(policy.foreground_weight, policy.repair_weight,
                                       std::chrono::milliseconds(25));
    double scrub_credit = 0.0;
    std::optional<ObjectId> retained_data_repair_after;
    std::optional<ObjectId> retained_control_repair_after;
    // Why the destructive DATA sweep did not run, logged at DEBUG only when
    // the answer changes. Without it "GC is not reclaiming" has no line to
    // read; the 2026-09-15 artwork leak was diagnosed blind.
    std::string last_gc_skip_reason;
    bool release_horizon_incomplete_logged = false;

    while (!stop.stop_requested()) {
        maintenance_wakeups_.fetch_add(1, std::memory_order_relaxed);
        const auto enter_stage = [this](const char* name) {
            maintenance_stage_.store(name, std::memory_order_release);
        };
        enter_stage("pass-begin");
        auto now = Clock::now();
        metadata_dirty = metadata_convergence_.pending();
        const auto metadata_demand = metadata_convergence_.diagnostics().requested_epoch;
        if (metadata_demand != last_metadata_demand_epoch) {
            metadata_retry_due = Clock::time_point{};
            last_metadata_demand_epoch = metadata_demand;
        }
        const auto current_event = maintenance_event_.load(std::memory_order_acquire);
        const bool event_changed = current_event != observed_event;
        observed_event = current_event;
        const auto local_metadata_generation = node_.metadata_replica().committed_generation();
        if (local_metadata_generation != last_local_metadata_generation) {
            catalogue_dirty = true;
            metadata_retry_due = Clock::time_point{};
            last_local_metadata_generation = local_metadata_generation;
            gc_quiescent_until = now + policy.foreground_quiet;
        }
        if (event_changed) {
            network_quiescent_until = Clock::time_point{};
            local_quiescent_until = Clock::time_point{};
            // Object arrival and namespace publication are separate durable
            // operations. Start an exact quiet window so a sweep cannot race
            // the retention claim which makes newly-arrived bytes reachable.
            gc_quiescent_until = now + policy.foreground_quiet;
        }
        std::vector<NodeId> active_nodes;
        bool established_metadata_peer = false;
        for (const auto& peer : node_.membership().active()) {
            active_nodes.push_back(peer.id);
            established_metadata_peer = established_metadata_peer ||
                                        (peer.id != node_.node_id() &&
                                         peer.metadata_generation > 1);
        }
        std::sort(active_nodes.begin(), active_nodes.end());
        const auto remote_epoch = node_.remote_metadata_epoch();
        const bool topology_changed = active_nodes != last_active_nodes;
        if (remote_epoch != last_metadata_remote_epoch || topology_changed) {
            metadata_retry_due = Clock::time_point{};
            if (topology_changed)
                formation_settle_due = now + node_.config().dead_after;
            last_metadata_remote_epoch = remote_epoch;
            last_active_nodes = std::move(active_nodes);
        }
        auto wall_seconds = std::chrono::duration<double>(now - last_wall).count();
        if (wall_seconds <= 0.0)
            wall_seconds = std::chrono::duration<double>(policy.interval).count();
        auto cpu_now = std::clock();
        double cpu_seconds = static_cast<double>(cpu_now - last_cpu) / CLOCKS_PER_SEC;
        double cpu_load = wall_seconds > 0.0 ? std::max(0.0, cpu_seconds / wall_seconds) : 0.0;
        last_wall = now;
        last_cpu = cpu_now;

        auto playback_bytes = store_->take_foreground_bytes();
        auto interactive_bytes = store_->take_interactive_bytes();
        auto loader_bytes = store_->take_loader_bytes();
        const bool playback_busy =
            playback_bytes > 0 || store_->foreground_idle_for() < policy.foreground_quiet;
        const bool interactive_busy =
            interactive_bytes > 0 || store_->interactive_idle_for() < policy.foreground_quiet;
        // Law 3: the loader outranks background work, so background work has to
        // be able to see it. Until 0.53.0 this decision was the two viewer
        // classes alone, and an ingest feeds neither -- so a node importing
        // 36 GB called itself idle and handed maintenance its idle share of a
        // disk the import was waiting on (gbni-1, 2026-09-22: sdb at 91%
        // utilisation, macha-maint reading 51.6 MB/s, the import writing
        // 2.8 MB/s, and busy_bandwidth_fraction already set to 0.0).
        const bool loader_busy =
            loader_bytes > 0 || store_->loader_idle_for() < policy.foreground_quiet;
        // Priority law: playback/seek > mounted MachaDFS/useful prefetch >
        // user-requested loader work > repair/rebalance/scrub. All three
        // suppress background work, while the transport queues themselves keep
        // playback above mount I/O.
        bool busy = playback_busy || interactive_busy || loader_busy;
        double fraction = busy ? policy.busy_bandwidth_fraction : policy.idle_bandwidth_fraction;

        double bandwidth = store_->estimated_network_bps();
        if (bandwidth <= 0.0)
            bandwidth = static_cast<double>(policy.initial_bandwidth);
        if (policy.max_bandwidth)
            bandwidth = std::min(bandwidth, static_cast<double>(policy.max_bandwidth));

        // Process CPU includes foreground filesystem/RPC work. Above the target,
        // background work loses budget continuously rather than stopping in a
        // fixed block-per-tick pattern.
        double cpu_scale = 1.0;
        if (cpu_load > policy.cpu_target)
            cpu_scale = std::clamp(policy.cpu_target / cpu_load, 0.0, 1.0);

        double rate = bandwidth * fraction * cpu_scale;
        // Repair's byte rate does not drop with busyness; repair_share decides
        // how much of the time it gets. busy_bandwidth_fraction governs
        // rebalance and scrub only.
        const double repair_rate = bandwidth * policy.idle_bandwidth_fraction * cpu_scale;
        double burst_cap = std::max<double>(node_.config().extent_size, bandwidth * 5.0);
        network_credit = std::min(burst_cap, network_credit + repair_rate * wall_seconds);
        local_credit = std::min(burst_cap, local_credit + rate * wall_seconds);
        const auto wall_now_ms = unix_ms();
        const bool scrub_due = policy.scrub_fraction > 0.0 && wall_now_ms >= scrub_due_unix_ms;
        if (scrub_due) {
            scrub_credit =
                std::min(burst_cap, scrub_credit + rate * policy.scrub_fraction * wall_seconds);
        } else {
            // Do not bank weeks of scrub credit and explode into a large burst when
            // the next campaign becomes due. Outside a campaign, scrub is truly idle.
            scrub_credit = 0.0;
        }

        bool gc_due_this_pass = false;
        try {
            // Remote generation notices wake no kernel/FUSE path and perform no
            // metadata-replica I/O themselves. The maintenance owner advances the coherent
            // local metadata snapshot promptly, while settled verification remains
            // a bounded periodic control-plane task.
            bool metadata_ready_for_dependants = !metadata_dirty;
            if (metadata_dirty &&
                (metadata_retry_due == Clock::time_point{} || now >= metadata_retry_due)) {
                if (node_.metadata_replica().committed_generation() <= 1 &&
                    !established_metadata_peer &&
                    now < formation_settle_due) {
                    metadata_ready_for_dependants = false;
                    metadata_retry_due = formation_settle_due;
                } else {
                    const bool virgin_follower =
                        node_.metadata_replica().committed_generation() <= 1 &&
                        !established_metadata_peer &&
                        !last_active_nodes.empty() &&
                        node_.node_id() !=
                            *std::min_element(last_active_nodes.begin(), last_active_nodes.end());
                    if (virgin_follower) {
                        // Exactly one deterministic founder performs generation-2
                        // formation. Followers remain event-driven and wake on its
                        // metadata notice; the retry deadline only covers a founder
                        // disappearing without a final connectivity event.
                        metadata_ready_for_dependants = false;
                        metadata_retry_due = Clock::now() + metadata_retry_backoff;
                    } else {
                        const auto convergence_run = metadata_convergence_.begin();
                        if (!convergence_run) {
                            metadata_dirty = false;
                            metadata_ready_for_dependants = true;
                        } else {
                            enter_stage("metadata-repair");
                            const auto stage = Clock::now();
                            try {
                                if (maintenance_stage_hook_)
                                    maintenance_stage_hook_("metadata-repair-begin");
                                metadata_->repair_once();
                                metadata_->note_replica_validation(true);
                                metadata_dirty = metadata_convergence_.complete(*convergence_run);
                                metadata_ready_for_dependants = !metadata_dirty;
                                catalogue_dirty = true;
                                metadata_retry_due = Clock::time_point{};
                                metadata_retry_backoff = maintenance_background_interval(policy);
                            } catch (const std::exception& error) {
                                metadata_->note_replica_validation(false, error.what());
                                metadata_ready_for_dependants = false;
                                metadata_retry_due = Clock::now() + metadata_retry_backoff;
                                metadata_retry_backoff =
                                    std::min(policy.no_progress_backoff,
                                             std::max(metadata_retry_backoff * 2,
                                                      maintenance_background_interval(policy)));
                                // In 0.19, inability to validate/reconcile every active
                                // metadata head must not stall non-destructive DATA repair.
                                // A locally committed branch remains a valid source of live
                                // object reachability while reconciliation is pending.
                                // Destructive GC below is independently fenced on `stable`,
                                // so continuing here can only add/repair replicas.
                                const auto local = node_.metadata_replica().committed();
                                if (node_.metadata_replica().recovery_required() ||
                                    local.generation <= 1)
                                    throw;
                                Log::debug("metadata repair deferred; continuing non-destructive "
                                           "maintenance: " +
                                           std::string(error.what()));
                            } catch (...) {
                                metadata_->note_replica_validation(false,
                                                                   "metadata validation failed");
                                metadata_ready_for_dependants = false;
                                metadata_retry_due = Clock::now() + metadata_retry_backoff;
                                metadata_retry_backoff =
                                    std::min(policy.no_progress_backoff,
                                             std::max(metadata_retry_backoff * 2,
                                                      maintenance_background_interval(policy)));
                                const auto local = node_.metadata_replica().committed();
                                if (node_.metadata_replica().recovery_required() ||
                                    local.generation <= 1)
                                    throw;
                                Log::debug("metadata repair deferred; continuing non-destructive "
                                           "maintenance");
                            }
                            log_slow_stage("metadata-repair", stage);
                        }
                    }
                }
            }

            // Catalogue GETs are memory-only. Convergence therefore belongs here:
            // generation notices (or the short validation TTL) trigger a refresh
            // independently of foreground activity, while the normal settled-state
            // verification remains an idle/background operation. This keeps remote
            // catalogue changes live without ever putting metadata-replica I/O on an API thread.
            if (metadata_ready_for_dependants &&
                (catalogue_dirty || catalogue_->refresh_needed()) &&
                (catalogue_retry_due == Clock::time_point{} || now >= catalogue_retry_due)) {
                enter_stage("catalogue-repair");
                const auto stage = Clock::now();
                try {
                    if (maintenance_stage_hook_)
                        maintenance_stage_hook_("catalogue-repair-begin");
                    catalogue_->repair_once();
                    catalogue_dirty = catalogue_->refresh_needed();
                    catalogue_retry_due = Clock::time_point{};
                } catch (const std::exception& e) {
                    Log::debug("catalogue sync: " + std::string(e.what()));
                    catalogue_dirty = true;
                    catalogue_retry_due = Clock::now() + maintenance_background_interval(policy);
                }
                log_slow_stage("catalogue-repair", stage);
            }

            const bool allow_network_repair = repair_share.can_start(now, busy);
            const bool network_due = allow_network_repair && now >= network_quiescent_until &&
                                     network_credit >= node_.config().extent_size;
            store_->note_repair_gate(
                network_due                        ? DistributedStore::RepairGate::ran
                : !allow_network_repair            ? DistributedStore::RepairGate::share
                : now < network_quiescent_until    ? DistributedStore::RepairGate::quiescent
                                                   : DistributedStore::RepairGate::credit,
                network_credit);
            const bool garbage_due = !busy && !metadata_dirty;
            const bool gc_due = !busy && now >= gc_quiescent_until;
            gc_due_this_pass = gc_due;

            // Destructive maintenance is deliberately opportunistic: degraded
            // clusters retain garbage. Reclamation is enabled only after every
            // durably-known node has been directly reached by this process and
            // metadata repair has validated/converged that complete replica set.

            // Reachability GC, repair and explicit tombstone accounting share
            // one immutable namespace inventory. Rebuild it only when one of
            // those consumers can make progress or metadata has advanced.
            if (network_due || garbage_due || gc_due) {
                enter_stage("inventory");
                const auto inventory_stage = Clock::now();
                auto objects = fs_->maintenance_objects_cached();
                bool rebuilt_inventory = false;
                if (!maintenance_live_ || !maintenance_catalogue_complete_ ||
                    maintenance_inventory_generation_ != objects->metadata_generation) {
                    auto live = std::make_shared<std::vector<ObjectId>>(objects->live);
                    auto universal = std::make_shared<std::vector<ObjectId>>();
                    auto control_live = std::make_shared<std::vector<ObjectId>>();
                    auto catalogue_objects = catalogue_->maintenance_objects();
                    maintenance_catalogue_complete_ = catalogue_objects.complete;
                    live->insert(live->end(), catalogue_objects.live.begin(),
                                 catalogue_objects.live.end());
                    universal->insert(universal->end(), catalogue_objects.universal.begin(),
                                      catalogue_objects.universal.end());
                    control_live->insert(control_live->end(),
                                         catalogue_objects.control_live.begin(),
                                         catalogue_objects.control_live.end());
                    std::sort(live->begin(), live->end());
                    live->erase(std::unique(live->begin(), live->end()), live->end());
                    std::sort(universal->begin(), universal->end());
                    universal->erase(std::unique(universal->begin(), universal->end()),
                                     universal->end());
                    std::sort(control_live->begin(), control_live->end());
                    control_live->erase(std::unique(control_live->begin(), control_live->end()),
                                        control_live->end());

                    maintenance_garbage_.clear();
                    maintenance_stale_garbage_.clear();
                    maintenance_garbage_.reserve(objects->garbage.size());
                    maintenance_stale_garbage_.reserve(objects->garbage.size());
                    for (const auto& garbage : objects->garbage) {
                        if (std::binary_search(live->begin(), live->end(), garbage.id))
                            maintenance_stale_garbage_.push_back(garbage);
                        else
                            maintenance_garbage_.push_back(garbage);
                    }

                    maintenance_inventory_generation_ = objects->metadata_generation;
                    maintenance_live_ = std::move(live);
                    maintenance_universal_ = std::move(universal);
                    maintenance_control_live_ = std::move(control_live);
                    rebuilt_inventory = true;
                    // A newly-derived reachability set is never consumed by a
                    // destructive sweep in the same pass. Schedule exactly one
                    // follow-up after the foreground quiet boundary; without
                    // this deadline an otherwise quiescent service would have
                    // no reason to wake and consume the safe inventory.
                    gc_quiescent_until = Clock::now() + policy.foreground_quiet;
                }
                if (rebuilt_inventory && Log::enabled(LogLevel::all)) {
                    Log::trace("DIAG maintenance-inventory generation=" +
                               std::to_string(objects->metadata_generation) +
                               " entries=" + std::to_string(objects->entries) +
                               " extents=" + std::to_string(objects->extents) +
                               " live=" + std::to_string(maintenance_live_->size()) + " garbage=" +
                               std::to_string(maintenance_garbage_.size()) + " stale_garbage=" +
                               std::to_string(maintenance_stale_garbage_.size()) +
                               " elapsed_ms=" + std::to_string(elapsed_ms(inventory_stage)));
                }

                if (network_due) {
                    const auto extent = std::max<uint64_t>(1, node_.config().extent_size);

                    // A durable retention claim is a promise about this physical
                    // node, not merely an annotation on the node's current
                    // namespace view. If scrub/corruption removes a claimed copy
                    // belonging only to an unseen branch, ordinary live-set repair
                    // cannot discover it. Walk a tiny bounded claim slice first and
                    // actively restore missing claimed DATA/CONTROL objects.
                    size_t retained_repairs = 0;
                    auto repair_retained = [&](RetentionClass type,
                                               std::optional<ObjectId>& cursor) {
                        for (size_t examined = 0; examined < 2 && network_credit >= extent;
                             ++examined) {
                            bool complete = false;
                            auto id = node_.retention_store().next_retained(type, cursor, complete);
                            if (!id)
                                break;
                            const bool restored = type == RetentionClass::data
                                                      ? store_->ensure_local(*id, false)
                                                      : store_->ensure_control_local(*id);
                            if (restored) {
                                ++retained_repairs;
                                network_credit =
                                    std::max(0.0, network_credit - static_cast<double>(extent));
                            }
                        }
                    };
                    const auto higher_class_active = [this, &policy] {
                        const auto quiet = policy.foreground_quiet;
                        return store_->foreground_idle_for() < quiet ||
                               store_->interactive_idle_for() < quiet ||
                               store_->loader_idle_for() < quiet;
                    };
                    repair_share.started(Clock::now(), busy);
                    // A throw from either repair stage must still close the
                    // turn, or the pacer counts it active forever and never
                    // computes another cooldown.
                    struct RepairTurn {
                        WeightedLoaderService& share;
                        const decltype(higher_class_active)& active;
                        ~RepairTurn() { share.finished(Clock::now(), active()); }
                    } repair_turn{repair_share, higher_class_active};
                    enter_stage("retention-repair");
                    repair_retained(RetentionClass::data, retained_data_repair_after);
                    repair_retained(RetentionClass::control, retained_control_repair_after);

                    const auto byte_budget = static_cast<uint64_t>(network_credit);
                    // The byte budget alone does not constrain have-object
                    // probes: a settled or mostly-settled namespace could issue
                    // thousands of synchronous control RPCs while consuming no
                    // network credit. Bound each repair slice independently.
                    const size_t operation_budget =
                        static_cast<size_t>(std::clamp<uint64_t>((byte_budget / extent), 4, 16));
                    enter_stage("network-repair");
                    const auto repair_stage = Clock::now();
                    auto repair = store_->repair_step(
                        byte_budget, operation_budget, maintenance_live_.get(),
                        maintenance_universal_.get(),
                        [&] {
                            // Repair's turn ends at the next operation boundary
                            // once its weighted slice is spent; it is paced,
                            // never stopped.
                            return repair_share.should_yield(Clock::now(), higher_class_active());
                        },
                        maintenance_inventory_generation_);
                    log_slow_stage("network-repair", repair_stage,
                                   "bytes=" + std::to_string(repair.bytes_transferred) +
                                       " retained_repairs=" + std::to_string(retained_repairs) +
                                       " push_examined=" + std::to_string(repair.push_examined) +
                                       " pull_examined=" + std::to_string(repair.pull_examined) +
                                       " remote_ops=" + std::to_string(repair.remote_operations) +
                                       " complete=" + std::to_string(repair.complete ? 1 : 0) +
                                       " yielded=" + std::to_string(repair.yielded ? 1 : 0));
                    if (repair.bytes_transferred) {
                        network_credit = std::max(
                            0.0, network_credit - static_cast<double>(repair.bytes_transferred));
                    }
                    if (repair.yielded) {
                        Log::trace("maintenance: repair yielded to foreground I/O");
                    } else if (!retained_repairs && !repair.bytes_transferred && repair.complete) {
                        network_credit = 0.0;
                        network_quiescent_until = Clock::time_point::max();
                        Log::trace("maintenance: repair quiescent; waiting for an event");
                    }
                }

                const bool cluster_gc_healthy = node_.membership().all_known_reachable();
                const bool cluster_gc_stable =
                    cluster_gc_healthy && metadata_->cluster_status().stable;

                if (garbage_due && !rebuilt_inventory && cluster_gc_stable &&
                    maintenance_catalogue_complete_) {
                    auto matured = collect_garbage(maintenance_garbage_);
                    std::vector<GarbageRef> legacy;
                    for (const auto& candidate : maintenance_garbage_) {
                        if (candidate.retired_at_ns == 0)
                            legacy.push_back(candidate);
                    }

                    auto erase = maintenance_stale_garbage_;
                    erase.insert(erase.end(), matured.begin(), matured.end());
                    if (!erase.empty() || !legacy.empty()) {
                        maintain_garbage_metadata(erase, legacy);
                    }
                }

                // The catalogue control live-set is derived from the same immutable
                // metadata inventory as DATA reachability.  A foreground catalogue
                // mutation may advance metadata after that inventory was built.  Never
                // sweep the control store using such a stale set: with a zero/short
                // grace period it could delete a newly-published manifest or shard
                // before the next maintenance pass observes the successor generation.
                const auto current_metadata_view = metadata_->available_snapshot_view();
                const auto release_metadata_view = metadata_->retention_release_view();
                const bool destructive_gc_enabled =
                    cluster_gc_stable && release_metadata_view &&
                    release_metadata_view->snapshot->retention_baseline_complete;

                // Retention release is local and causal. A sole accepted head
                // provides a complete live-object set plus the mutation clock of
                // claim dots it has actually observed. Claims from unseen concurrent
                // branches are not dominated by that clock and therefore survive.
                if (auto floor = metadata_->retention_release_view();
                    floor && floor->hash != retention_release_floor_hash_) {
                    auto data_live = std::make_shared<std::vector<ObjectId>>();
                    auto control_live = std::make_shared<std::vector<ObjectId>>();
                    bool complete = true;
                    // The live set destructive GC acts on. Read the
                    // namespace in whichever form it is in: an empty one here
                    // means "collect everything".
                    auto release_nodes =
                        ControlNamespaceNodeStore::for_reading(node_, *store_);
                    for_each_namespace_entry(
                        *floor->snapshot, &release_nodes,
                        [&](const std::string&, const FsEntry& entry) {
                            if (entry.type != EntryType::file)
                                return;
                            for (const auto& extent_ref : entry.extents)
                                if (!extent_ref.hole)
                                    data_live->push_back(extent_ref.id);
                        });
                    const auto conflict_extents = metadata_conflict_extent_roots(*floor->snapshot);
                    data_live->insert(data_live->end(), conflict_extents.begin(),
                                      conflict_extents.end());

                    for (const auto& root : metadata_catalogue_root_set(*floor->snapshot)) {
                        try {
                            auto retained = catalogue_->retention_objects(std::nullopt, root);
                            data_live->insert(data_live->end(), retained.data.begin(),
                                              retained.data.end());
                            control_live->insert(control_live->end(), retained.control.begin(),
                                                 retained.control.end());
                        } catch (const std::exception& error) {
                            complete = false;
                            Log::debug("retention release horizon catalogue unavailable root=" +
                                       to_string(root) + " error=" + error.what());
                        }
                    }
                    // The namespace tree itself. A live set that omits it is a
                    // live set that lets the collector delete the namespace,
                    // and a partial walk is worse than none -- so an unreadable
                    // node marks the whole set incomplete and nothing is
                    // released against it.
                    if (floor->snapshot->namespace_root) {
                        try {
                            collect_namespace_tree_nodes(*floor->snapshot->namespace_root,
                                                         release_nodes, *control_live);
                        } catch (const std::exception& error) {
                            complete = false;
                            Log::warn("retention release horizon namespace tree unavailable root=" +
                                      to_string(*floor->snapshot->namespace_root) +
                                      " error=" + error.what());
                        }
                    }
                    std::sort(data_live->begin(), data_live->end());
                    data_live->erase(std::unique(data_live->begin(), data_live->end()),
                                     data_live->end());
                    std::sort(control_live->begin(), control_live->end());
                    control_live->erase(std::unique(control_live->begin(), control_live->end()),
                                        control_live->end());
                    if (complete) {
                        retention_release_floor_hash_ = floor->hash;
                        retention_release_data_live_ = std::move(data_live);
                        retention_release_control_live_ = std::move(control_live);
                        retention_release_clock_ = floor->snapshot->mutation_sequences;
                        retention_release_complete_ = true;
                    }
                }

                if (gc_due && destructive_gc_enabled && maintenance_catalogue_complete_ &&
                    maintenance_control_live_ &&
                    maintenance_inventory_generation_ >= node_.known_metadata_generation()) {
                    // Destructive retention release is fenced by direct reachability of
                    // every durably-known node. A partition may continue to accumulate
                    // causal tombstones/claims, but it cannot reclaim authoritative bytes.
                    if (retention_release_complete_ && retention_release_control_live_) {
                        (void)node_.retention_store().release_unreferenced(
                            RetentionClass::control, *retention_release_control_live_,
                            retention_release_clock_, 64);
                    }
                    enter_stage("control-gc");
                    const auto removed = catalogue_->control_gc_step(*maintenance_control_live_,
                                                                     policy.garbage_grace, 32);
                    (void)node_.retention_store().prune_unclaimed(
                        RetentionClass::control,
                        [this](const ObjectId& id) { return node_.control_store().has(id); }, 64);
                    if (removed)
                        Log::debug("catalogue control GC removed=" + std::to_string(removed));
                }

                // Physical mark/sweep also catches objects that never acquired a
                // tombstone at all (for example, a DATA put followed by process
                // death before metadata commit acceptance). Recent tombstones are protected for
                // the same grace interval, and legacy tombstones remain protected
                // until their first 0.10.x maintenance stamp has committed.
                // Retention claims are checked immediately before every
                // physical delete, so an inventory that predates a newer
                // metadata generation remains safe for orphan cleanup.
                std::string gc_skip_reason;
                if (!gc_due)
                    gc_skip_reason = busy ? "foreground busy"
                                     : gc_quiescent_until == Clock::time_point::max()
                                         ? "complete; waiting for an event"
                                         : "quiet window";
                else if (rebuilt_inventory)
                    gc_skip_reason = "inventory rebuilt this pass";
                else if (!cluster_gc_stable)
                    gc_skip_reason = cluster_gc_healthy ? "metadata not stable"
                                                        : "not every known node reachable";
                else if (!release_metadata_view)
                    gc_skip_reason = "no sole accepted head for retention release";
                else if (!release_metadata_view->snapshot->retention_baseline_complete)
                    gc_skip_reason = "retention baseline incomplete";
                else if (!maintenance_catalogue_complete_)
                    gc_skip_reason = "catalogue inventory incomplete";
                else if (!maintenance_live_)
                    gc_skip_reason = "no reachability inventory";
                else if (maintenance_inventory_generation_ < node_.known_metadata_generation())
                    gc_skip_reason = "inventory generation " +
                                     std::to_string(maintenance_inventory_generation_) +
                                     " behind known " +
                                     std::to_string(node_.known_metadata_generation());
                if (gc_skip_reason != last_gc_skip_reason) {
                    last_gc_skip_reason = gc_skip_reason;
                    if (Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection sweep node=" +
                                   to_string(node_.node_id()).substr(0, 6) +
                                   (gc_skip_reason.empty() ? std::string(" enabled")
                                                           : " skipped: " + gc_skip_reason));
                }
                if (gc_due && !rebuilt_inventory && destructive_gc_enabled &&
                    maintenance_catalogue_complete_ && maintenance_live_ &&
                    maintenance_inventory_generation_ >= node_.known_metadata_generation()) {
                    if (retention_release_complete_ && retention_release_data_live_) {
                        const auto released = node_.retention_store().release_unreferenced(
                            RetentionClass::data, *retention_release_data_live_,
                            retention_release_clock_, 64);
                        if (released && Log::enabled(LogLevel::debug))
                            Log::debug("retention released DATA claims node=" +
                                       to_string(node_.node_id()).substr(0, 6) + " count=" +
                                       std::to_string(released));
                    } else if (!release_horizon_incomplete_logged) {
                        release_horizon_incomplete_logged = true;
                        Log::debug("retention release skipped: horizon incomplete");
                    }
                    std::vector<ObjectId> protected_ids;
                    protected_ids.reserve(maintenance_garbage_.size());
                    const auto now_ns = wall_time_ns();
                    const auto grace_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(policy.garbage_grace)
                            .count();
                    for (const auto& candidate : maintenance_garbage_) {
                        const bool matured = candidate.retired_at_ns > 0 &&
                                             now_ns >= candidate.retired_at_ns &&
                                             now_ns - candidate.retired_at_ns >= grace_ns;
                        if (!matured)
                            protected_ids.push_back(candidate.id);
                    }
                    std::sort(protected_ids.begin(), protected_ids.end());
                    protected_ids.erase(std::unique(protected_ids.begin(), protected_ids.end()),
                                        protected_ids.end());

                    enter_stage("garbage-collect");
                    const auto gc_stage = Clock::now();
                    // Tombstones carry their own exact retirement deadline.
                    // Untombstoned bytes, however, may be the data half of an
                    // in-flight publication and have no causal marker yet. Give
                    // that orphan sweep one retry window before it may reclaim;
                    // a zero tombstone grace must not collapse this write/claim
                    // safety window to zero.
                    const auto orphan_grace =
                        std::max(policy.garbage_grace, policy.no_progress_backoff);
                    auto gc = node_.local_store().gc_step(
                        *maintenance_live_, protected_ids, orphan_grace, 64,
                        [this] {
                            const auto quiet = node_.config().maintenance.foreground_quiet;
                            return store_->foreground_idle_for() < quiet ||
                                   store_->interactive_idle_for() < quiet ||
                                   store_->loader_idle_for() < quiet;
                        },
                        [this](const ObjectId& id) {
                            return node_.retention_store().retained(RetentionClass::data, id);
                        });
                    log_slow_stage("garbage-collect", gc_stage,
                                   "reclaimed_bytes=" + std::to_string(gc.bytes) +
                                       " objects=" + std::to_string(gc.objects));
                    if ((gc.deferred || gc.objects || gc.yielded) && Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection sweep node=" +
                                   to_string(node_.node_id()).substr(0, 6) + " objects=" +
                                   std::to_string(gc.objects) + " bytes=" +
                                   std::to_string(gc.bytes) + " deferred=" +
                                   std::to_string(gc.deferred ? 1 : 0) + " yielded=" +
                                   std::to_string(gc.yielded ? 1 : 0) + " complete=" +
                                   std::to_string(gc.complete ? 1 : 0) + " live=" +
                                   std::to_string(maintenance_live_->size()) + " protected=" +
                                   std::to_string(protected_ids.size()));
                    (void)node_.retention_store().prune_unclaimed(
                        RetentionClass::data,
                        [this](const ObjectId& id) { return node_.local_store().has(id); }, 64);
                    if (gc.bytes && Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection reclaimed " + std::to_string(gc.bytes) +
                                   " local bytes");
                    if (gc.yielded) {
                        Log::trace("maintenance: garbage collection yielded to foreground I/O");
                    } else if (gc.complete) {
                        if (gc.deferred) {
                            gc_quiescent_until = Clock::now() + orphan_grace;
                            Log::trace(
                                "maintenance: recent orphan deferred to exact grace deadline");
                        } else {
                            gc_quiescent_until = Clock::time_point::max();
                            Log::trace(
                                "maintenance: garbage collection complete; waiting for an event");
                        }
                    }
                }
            }

            // Local maintenance advances persistent filesystem cursors. A scheduler
            // slice examines at most 64 objects and yields immediately to foreground
            // work; it never rebuilds a complete object list merely to discover that
            // placement is already correct.
            if (!busy && now >= local_quiescent_until &&
                local_credit >= node_.config().extent_size) {
                enter_stage("local-rebalance");
                const auto rebalance_stage = Clock::now();
                auto rebalance = node_.local_store().rebalance_step(
                    static_cast<uint64_t>(local_credit), 64, [this] {
                        const auto quiet = node_.config().maintenance.foreground_quiet;
                        return store_->foreground_idle_for() < quiet ||
                               store_->interactive_idle_for() < quiet ||
                               store_->loader_idle_for() < quiet;
                    });
                log_slow_stage("local-rebalance", rebalance_stage,
                               "bytes=" + std::to_string(rebalance.bytes) +
                                   " objects=" + std::to_string(rebalance.objects));
                if (rebalance.bytes)
                    local_credit =
                        std::max(0.0, local_credit - static_cast<double>(rebalance.bytes));
                if (rebalance.yielded) {
                    Log::trace("maintenance: local rebalance yielded to foreground I/O");
                } else if (rebalance.complete && !rebalance.bytes) {
                    local_credit = 0.0;
                    local_quiescent_until = Clock::time_point::max();
                    Log::trace("maintenance: local rebalance quiescent; waiting for an event");
                }
            }

            // Retention publication appends are deliberately batched but still
            // safety-critical durable records. Compact them only on the background
            // owner so foreground metadata latency never pays checkpoint rewrite
            // cost. Snapshot-before-truncate makes interruption idempotent.
            if (!busy) {
                enter_stage("retention-compact");
                (void)node_.retention_store().compact_if_needed(4096);
            }

            // Metadata ancestry is only ever re-rooted once a durable,
            // cluster-wide proof establishes the exact same accepted-head
            // hash as every durably-known participant's ancestry floor --
            // see MetadataManager::attempt_history_checkpoint() and
            // HistoryCheckpointProof. The prior generation-only status check
            // allowed a returning accepted branch to outlive the common
            // ancestor on every replica; this protocol replaces it. A no-op
            // most ticks: it returns immediately unless local size
            // thresholds are actually due, and aborts silently -- retrying
            // next cycle -- unless every participant is currently reachable
            // and already agrees on a single head.
            // An accepted head this replica cannot replay locally is excluded
            // from reads (MetadataReplica cooldown) and re-anchored here from
            // any peer that can still materialize it -- the live alternative
            // to the old restart-and-quarantine path. A no-op unless a head
            // is actually flagged.
            try {
                enter_stage("head-repair");
                (void)metadata_->repair_unreconstructable_heads();
            } catch (const std::exception& error) {
                Log::debug("maintenance: metadata head repair failed: " +
                           std::string(error.what()));
            }

            if (!busy) {
                try {
                    enter_stage("history-checkpoint");
                    metadata_->attempt_history_checkpoint();
                } catch (const std::exception& error) {
                    Log::debug("maintenance: history checkpoint attempt failed: " +
                               std::string(error.what()));
                }
            }

            // Packed DATA tombstones are physical dead space. Compact one
            // victim pack per backend at a time; unlike the old whole-store
            // rewrite this has a fixed temporary-space envelope and therefore
            // remains viable on multi-terabyte backends.
            if (!busy) {
                enter_stage("compact-packs");
                (void)node_.local_store().compact_packs(stop);
            }

            if (!busy && scrub_due && scrub_credit >= node_.config().extent_size) {
                enter_stage("scrub");
                const auto scrub_stage = Clock::now();
                auto scrub =
                    node_.local_store().scrub_step(static_cast<uint64_t>(scrub_credit), 64, [this] {
                        const auto quiet = node_.config().maintenance.foreground_quiet;
                        return store_->foreground_idle_for() < quiet ||
                               store_->interactive_idle_for() < quiet ||
                               store_->loader_idle_for() < quiet;
                    });
                log_slow_stage("scrub", scrub_stage,
                               "bytes=" + std::to_string(scrub.bytes) +
                                   " objects=" + std::to_string(scrub.objects));
                if (scrub.bytes)
                    scrub_credit = std::max(0.0, scrub_credit - static_cast<double>(scrub.bytes));
                if (scrub.yielded) {
                    Log::trace("maintenance: scrub yielded to foreground I/O");
                } else if (scrub.complete) {
                    // A proactive scrub is a low-frequency integrity campaign. Once
                    // the complete physical pass finishes, stay genuinely idle until
                    // the next scheduled campaign instead of restarting after the
                    // generic no-progress backoff used by repair/rebalance.
                    scrub_credit = 0.0;
                    const auto completed = unix_ms();
                    const auto interval_ms = static_cast<uint64_t>(policy.scrub_interval.count());
                    scrub_due_unix_ms =
                        completed > std::numeric_limits<uint64_t>::max() - interval_ms
                            ? std::numeric_limits<uint64_t>::max()
                            : completed + interval_ms;
                    try {
                        persist_scrub_due(scrub_due_path(node_.config().state_path),
                                          scrub_due_unix_ms);
                    } catch (const std::exception& error) {
                        Log::debug("maintenance: scrub schedule persistence unavailable: " +
                                   std::string(error.what()));
                    }
                    Log::trace("maintenance: scrub pass complete; next campaign scheduled");
                }
            }

        } catch (const MetadataNotReady&) {
            // Normal startup/recovery state. MetadataManager has already
            // published any availability transition; do not duplicate it on
            // every maintenance pass.
        } catch (const std::exception& e) {
            const std::string_view message(e.what());
            if (message !=
                    "metadata durable replica set unavailable; reconciliation may be required" &&
                message != "metadata write durability floor unavailable")
                Log::debug("maintenance: " + std::string(message));
        }

        if (gc_due_this_pass && !busy) {
            // An immature tombstone is concrete future work, not a reason for a
            // maintenance cadence. Once a GC pass has evaluated the final
            // accepted inventory, arm one exact steady-clock wake for its
            // earliest wall-clock retirement deadline. Without this, the
            // protected object makes the physical sweep look quiescent and it
            // can sleep forever unless an unrelated event happens after grace.
            const auto grace_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(policy.garbage_grace).count();
            const auto wall_now_ns = wall_time_ns();
            std::optional<std::chrono::nanoseconds> earliest_remaining;
            for (const auto& candidate : maintenance_garbage_) {
                if (candidate.retired_at_ns <= 0 || grace_ns <= 0)
                    continue;
                int64_t remaining_ns{};
                if (wall_now_ns < candidate.retired_at_ns) {
                    const auto until_retirement = candidate.retired_at_ns - wall_now_ns;
                    remaining_ns = until_retirement > std::numeric_limits<int64_t>::max() - grace_ns
                                       ? std::numeric_limits<int64_t>::max()
                                       : until_retirement + grace_ns;
                } else {
                    const auto elapsed = wall_now_ns - candidate.retired_at_ns;
                    if (elapsed >= grace_ns)
                        continue;
                    remaining_ns = grace_ns - elapsed;
                }
                const auto remaining = std::chrono::nanoseconds(remaining_ns);
                if (!earliest_remaining || remaining < *earliest_remaining)
                    earliest_remaining = remaining;
            }
            if (earliest_remaining)
                gc_quiescent_until = Clock::now() + *earliest_remaining;
        }

        cpu_reporter.tick();
        auto deadline = Clock::time_point::max();
        const auto now_after_work = Clock::now();
        // `complete()` can turn an in-flight burst into one pending follow-up
        // without emitting another external service event. A dirty owner with
        // no retry delay is therefore runnable now; sleeping until an unrelated
        // event loses the edge and can strand metadata (and its catalogue
        // dependent) indefinitely. The same rule applies when catalogue repair
        // deliberately leaves one more current-state pass to perform.
        if (metadata_dirty)
            deadline =
                std::min(deadline, metadata_retry_due == Clock::time_point{} ? now_after_work
                                                                             : metadata_retry_due);
        if (catalogue_dirty && !metadata_dirty)
            deadline = std::min(deadline, catalogue_retry_due == Clock::time_point{}
                                              ? now_after_work
                                              : catalogue_retry_due);

        if (gc_quiescent_until != Clock::time_point{} &&
            gc_quiescent_until != Clock::time_point::max()) {
            if (gc_quiescent_until > now_after_work) {
                deadline = std::min(deadline, gc_quiescent_until);
            } else if (!gc_due_this_pass && !busy) {
                // Work earlier in this pass (notably metadata reconciliation)
                // may run across an exact GC deadline. Re-evaluate immediately
                // instead of discarding that elapsed deadline and sleeping
                // indefinitely. Once a due pass has evaluated GC, gc_due is
                // true and normal event-driven quiescence still applies.
                deadline = std::min(deadline, now_after_work);
            }
        }
        const auto scrub_now_ms = unix_ms();
        if (scrub_due_unix_ms <= scrub_now_ms) {
            if (scrub_credit < node_.config().extent_size && rate > 0.0 &&
                policy.scrub_fraction > 0.0) {
                const auto seconds =
                    (static_cast<double>(node_.config().extent_size) - scrub_credit) /
                    (rate * policy.scrub_fraction);
                if (std::isfinite(seconds) && seconds > 0.0)
                    deadline = std::min(deadline, now_after_work +
                                                      std::chrono::duration_cast<Clock::duration>(
                                                          std::chrono::duration<double>(seconds)));
            }
        } else {
            deadline = std::min(deadline, now_after_work + std::chrono::milliseconds(
                                                               scrub_due_unix_ms - scrub_now_ms));
        }

        if (busy) {
            // A busy pass suppressed GC, repair and rebalance; it must hand
            // the decision back once the foreground goes quiet. Until 0.41.1
            // it armed that wake-up only while the foreground was still
            // inside its quiet period at the end of the pass. A pass that
            // started busy and ended after the quiet period had elapsed --
            // with the GC window already expired, so the GC deadline above
            // did not apply either -- slept for the next unrelated event
            // (the scrub deadline: days). Measured on the writer of a
            // catalogue burst: one maintenance pass in 27 s, `busy=1
            // gc_quiet_ms=-2 wait_ms=2591999883`, four superseded artwork
            // objects never reclaimed (2026-09-15, 1-2 in 60 runs). Now the
            // pass re-evaluates as soon as the quiet period is met, and at
            // once when it already has.
            auto idle = std::min({store_->foreground_idle_for(), store_->interactive_idle_for(),
                                  store_->loader_idle_for()});
            deadline = std::min(deadline, now_after_work + (idle < policy.foreground_quiet
                                                                ? policy.foreground_quiet - idle
                                                                : Clock::duration{}));
        }

        auto credit_deadline = [&](double credit, Clock::time_point quiescent) {
            if (quiescent == Clock::time_point::max() || rate <= 0.0 ||
                credit >= node_.config().extent_size)
                return;
            const auto seconds = (static_cast<double>(node_.config().extent_size) - credit) / rate;
            if (std::isfinite(seconds) && seconds > 0.0)
                deadline = std::min(deadline,
                                    now_after_work + std::chrono::duration_cast<Clock::duration>(
                                                         std::chrono::duration<double>(seconds)));
        };
        {
            // Credit alone is not enough while repair is cooling down behind a
            // busy class: wake when its next turn is due, not on the next
            // unrelated event.
            if (network_quiescent_until != Clock::time_point::max() && repair_rate > 0.0 &&
                network_credit < node_.config().extent_size) {
                const auto seconds =
                    (static_cast<double>(node_.config().extent_size) - network_credit) / repair_rate;
                if (std::isfinite(seconds) && seconds > 0.0)
                    deadline = std::min(deadline,
                                        now_after_work + std::chrono::duration_cast<Clock::duration>(
                                                             std::chrono::duration<double>(seconds)));
            }
            if (network_quiescent_until != Clock::time_point::max()) {
                const auto turn = repair_share.wait_for(now_after_work, busy);
                if (turn > Clock::duration{})
                    deadline = std::min(deadline, now_after_work + turn);
            }
        }
        credit_deadline(local_credit, local_quiescent_until);

        // A maintenance action can itself commit metadata (for example,
        // retiring a matured garbage marker). That commit is a real event and
        // must not be lost merely because it arrived before this pass reached
        // the condition-variable wait.
        if (maintenance_event_.load(std::memory_order_acquire) != observed_event)
            continue;

        enter_stage("wait");
        {
            const auto relative = [&](Clock::time_point at) -> int64_t {
                if (at == Clock::time_point::max())
                    return -1;
                if (at == Clock::time_point{})
                    return -3;
                return std::chrono::duration_cast<std::chrono::milliseconds>(at - now_after_work)
                    .count();
            };
            maintenance_last_wait_ms_.store(relative(deadline), std::memory_order_release);
            maintenance_last_gc_quiet_ms_.store(relative(gc_quiescent_until),
                                                std::memory_order_release);
            maintenance_last_flags_.store(static_cast<uint8_t>((busy ? 1 : 0) |
                                                               (gc_due_this_pass ? 2 : 0)),
                                          std::memory_order_release);
        }
        std::unique_lock wait_lock(maintenance_wait_mutex_);
        const auto waiting_event = maintenance_event_.load(std::memory_order_acquire);
        auto changed = [this, waiting_event] {
            return maintenance_event_.load(std::memory_order_acquire) != waiting_event;
        };
        if (deadline == Clock::time_point::max())
            maintenance_wait_cv_.wait(wait_lock, stop, changed);
        else
            maintenance_wait_cv_.wait_until(wait_lock, stop, deadline, changed);
    }
}
} // namespace macha

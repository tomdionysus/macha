// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_api.hpp"

#include "fuse_mountpoint.hpp"
#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <map>

namespace macha {
namespace {

// Ranked so a worse condition can only escalate reported health, never let a
// later, milder check downgrade it back.
enum class HealthSeverity { healthy, degraded, recovering, critical };

// Stated once, and carried in the lightweight response so a client learns
// where the expensive half went from the payload itself.
constexpr std::string_view diagnostics_path = "/api/v1/status/diagnostics";

std::string_view health_name(HealthSeverity severity) {
    switch (severity) {
    case HealthSeverity::healthy: return "healthy";
    case HealthSeverity::degraded: return "degraded";
    case HealthSeverity::recovering: return "recovering";
    case HealthSeverity::critical: return "critical";
    }
    return "unknown";
}

void escalate(HealthSeverity& health, HealthSeverity candidate) {
    if (candidate > health)
        health = candidate;
}

PersistedNodeStatus persisted(const NodeTelemetry& telemetry) {
    PersistedNodeStatus out;
    out.boot_id = telemetry.boot_id;
    out.observed_unix_ms = telemetry.observed_unix_ms;
    out.version = telemetry.version;
    out.host = telemetry.host;
    out.failure_domain = telemetry.failure_domain;
    out.port = telemetry.port;
    out.storage_capacity = telemetry.storage_capacity;
    out.storage_used = telemetry.storage_used;
    out.cache_capacity = telemetry.cache_capacity;
    out.cache_used = telemetry.cache_used;
    out.metadata_generation = telemetry.metadata_generation;
    out.storage_backends_online = telemetry.storage_backends_online;
    out.api_endpoint = telemetry.api_endpoint;
    return out;
}

void merge_membership(PersistedNodeStatus& out, const NodeInfo& member) {
    // Membership is the authoritative live cluster view. Telemetry may enrich
    // it, but absence of telemetry must never make a connected node disappear
    // from Status or make a voter look offline.
    out.observed_unix_ms = std::max(out.observed_unix_ms, member.seen_unix_ms);
    out.host = member.host;
    out.failure_domain = member.failure_domain;
    out.port = member.port;
    out.metadata_generation = std::max(out.metadata_generation, member.metadata_generation);
}

Json bytes_pair(uint64_t used, uint64_t capacity) {
    return Json::Object{{"available", true},
                        {"capacity_bytes", capacity},
                        {"used_bytes", used},
                        {"free_bytes", capacity > used ? capacity - used : 0}};
}

Json unavailable_bytes(std::optional<uint64_t> capacity = {}) {
    return Json::Object{{"available", false},
                        {"capacity_bytes", capacity ? Json(*capacity) : Json(nullptr)},
                        {"used_bytes", Json(nullptr)},
                        {"free_bytes", Json(nullptr)}};
}

Json rpc_timing_json(const RpcServerWorkStats::Timing& timing) {
    return Json::Object{
        {"requests", timing.requests},
        {"queue_wait_us_total", timing.queue_wait_us_total},
        {"queue_wait_us_max", timing.queue_wait_us_max},
        {"handler_us_total", timing.handler_us_total},
        {"handler_us_max", timing.handler_us_max},
    };
}

Json endpoint_json(const Endpoint& endpoint, std::string_view source = {}) {
    Json::Object out{{"host", endpoint.host}, {"port", static_cast<uint64_t>(endpoint.port)}};
    if (!source.empty())
        out["source"] = std::string(source);
    return Json(std::move(out));
}

Json public_connectivity_json(const PublicConnectivityStatus& status) {
    Json::Object upnp;
    upnp["enabled"] = status.upnp.enabled;
    upnp["support_built"] = status.upnp.support_built;
    upnp["gateway_found"] = status.upnp.gateway_found;
    upnp["mapping_active"] = status.upnp.mapping_active;
    upnp["mapping_created"] = status.upnp.mapping_created;
    upnp["mapping_owned"] = status.upnp.mapping_owned;
    upnp["private_wan"] = status.upnp.private_wan;
    upnp["lan_address"] =
        status.upnp.lan_address.empty() ? Json(nullptr) : Json(status.upnp.lan_address);
    upnp["external_address"] =
        status.upnp.external_address.empty() ? Json(nullptr) : Json(status.upnp.external_address);
    upnp["internal_port"] = static_cast<uint64_t>(status.upnp.internal_port);
    upnp["external_port"] = static_cast<uint64_t>(status.upnp.external_port);
    upnp["lease_seconds"] = static_cast<uint64_t>(status.upnp.lease_seconds);
    upnp["igd_status"] = static_cast<int64_t>(status.upnp.igd_status);
    upnp["error"] = status.upnp.error.empty() ? Json(nullptr) : Json(status.upnp.error);

    Json::Object external_ip;
    external_ip["enabled"] = status.external_ip.enabled;
    external_ip["attempted"] = status.external_ip.attempted;
    external_ip["address"] =
        status.external_ip.address.empty() ? Json(nullptr) : Json(status.external_ip.address);
    external_ip["error"] =
        status.external_ip.error.empty() ? Json(nullptr) : Json(status.external_ip.error);

    Json::Object check;
    check["enabled"] = status.check_enabled;
    check["self_probe"] = status.self_probe;
    check["error"] =
        status.self_probe_error.empty() ? Json(nullptr) : Json(status.self_probe_error);
    check["checked_at_unix_ms"] = status.checked_unix_ms;
    // A same-node TCP connect is a NAT loopback diagnostic, not proof that an
    // arbitrary Internet host can reach the advertised endpoint. A future peer
    // probe can promote this field without changing the status shape.
    check["externally_verified"] = false;

    Json::Object out;
    out["configured"] = endpoint_json(status.configured);
    out["advertised"] = endpoint_json(status.advertised, status.advertised_source);
    out["upnp"] = std::move(upnp);
    out["external_ip"] = std::move(external_ip);
    out["check"] = std::move(check);
    return Json(std::move(out));
}

// network.inbound_capable / storage.hosts_extents as this node currently
// resolves them, beside the modes that were configured, and the dial-back
// evidence the answer rests on.
void add_inbound_resolution(Json::Object& connectivity, const InboundResolution& resolution) {
    connectivity["inbound_capable"] = resolution.inbound_capable;
    connectivity["inbound_capable_mode"] = std::string(tristate_name(resolution.inbound_capable_mode));
    connectivity["inbound_capable_source"] = resolution.source;
    connectivity["inbound_capable_decided_at_unix_ms"] =
        resolution.decided_unix_ms ? Json(resolution.decided_unix_ms) : Json(nullptr);
    connectivity["hosts_extents"] = resolution.hosts_extents;
    connectivity["hosts_extents_mode"] = std::string(tristate_name(resolution.hosts_extents_mode));
    Json::Object probe;
    probe["last_probe_unix_ms"] =
        resolution.last_probe_unix_ms ? Json(resolution.last_probe_unix_ms) : Json(nullptr);
    probe["last_probe_peer"] =
        resolution.last_probe_peer.empty() ? Json(nullptr) : Json(resolution.last_probe_peer);
    probe["last_probe_error"] =
        resolution.last_probe_error.empty() ? Json(nullptr) : Json(resolution.last_probe_error);
    probe["consecutive_failures"] = static_cast<uint64_t>(resolution.consecutive_probe_failures);
    connectivity["dial_back"] = std::move(probe);
}

Json identity_reset_json(const IdentityAssociationReset& reset) {
    Json::Object out;
    out["scope"] = identity_reset_key(reset.host, reset.port);
    out["host"] = reset.host;
    out["port"] = reset.port ? Json(static_cast<uint64_t>(reset.port)) : Json(nullptr);
    out["stale_node_id"] =
        reset.stale_node_id == NodeId{} ? Json(nullptr) : Json(to_string(reset.stale_node_id));
    out["epoch"] = reset.epoch;
    out["reset_at_unix_ms"] = reset.reset_unix_ms;
    out["reset_by_node_id"] = to_string(reset.reset_by);
    out["reason"] = reset.reason.empty() ? Json(nullptr) : Json(reset.reason);
    return Json(std::move(out));
}

// A live telemetry sample that is stale (older than the caller's freshness
// window) must be treated exactly like having no live sample at all for
// numeric purposes: a sample can be arbitrarily old, and presenting its
// resource/runtime figures as current would fabricate data. "authoritative"
// means a genuinely fresh, current measurement exists.
struct EffectiveNodeTelemetry {
    bool authoritative{};
    uint64_t storage_capacity{};
    uint64_t storage_used{};
    uint64_t cache_capacity{};
    uint64_t cache_used{};
    uint32_t storage_backends_online{};
};

EffectiveNodeTelemetry effective_telemetry(const NodeTelemetry* live, bool stale,
                                           const PersistedNodeStatus& durable,
                                           bool telemetry_known) {
    EffectiveNodeTelemetry out;
    // A fresh (non-stale) sample is only authoritative once the sender itself
    // reports "ready": a node mid-recovery legitimately publishes fresh
    // zero-valued capacity/usage, and presenting that as a current
    // measurement is indistinguishable from a real empty node.
    out.authoritative = live && !stale && live->phase == NodePhase::ready;
    if (out.authoritative) {
        out.storage_capacity = live->storage_capacity;
        out.storage_used = live->storage_used;
        out.cache_capacity = live->cache_capacity;
        out.cache_used = live->cache_used;
        out.storage_backends_online = live->storage_backends_online;
    } else if (telemetry_known) {
        out.storage_capacity = durable.storage_capacity;
        out.storage_used = durable.storage_used;
        out.cache_capacity = durable.cache_capacity;
        out.cache_used = durable.cache_used;
        out.storage_backends_online = durable.storage_backends_online;
    }
    return out;
}

Json node_json(const NodeId& id, const PersistedNodeStatus& durable, const NodeInfo* member,
               const NodeTelemetry* live, uint64_t live_age_ms, bool online, bool stale,
               bool retired, bool telemetry_known, bool metadata_replica,
               const IdentityAssociationReset* identity_reset,
               const InboundResolution* local_resolution) {
    Json::Object node;
    node["id"] = to_string(id);
    node["state"] = retired ? "retired" : (online ? "online" : "offline");
    // The gossiped self-declarations (0.42.0). Membership is the only source:
    // a node known solely from durable telemetry predates the flags or has
    // never been heard from, and null says so rather than guessing.
    if (member) {
        node["inbound_capable"] = node_inbound_capable(*member);
        node["hosts_extents"] = node_hosts_extents(*member);
        // The host/port above are a routing key for a node that cannot be
        // dialled; say so where an operator would otherwise try to connect.
        node["dialable"] = node_inbound_capable(*member);
    } else {
        node["inbound_capable"] = Json(nullptr);
        node["hosts_extents"] = Json(nullptr);
        node["dialable"] = Json(nullptr);
    }
    if (local_resolution) {
        node["inbound_capable_mode"] =
            std::string(tristate_name(local_resolution->inbound_capable_mode));
        node["hosts_extents_mode"] =
            std::string(tristate_name(local_resolution->hosts_extents_mode));
    }
    node["telemetry_freshness"] =
        live ? (stale ? "stale" : "live") : (telemetry_known ? "last_known" : "unavailable");
    // The node's own reported startup phase, using the same vocabulary as
    // this API's local root.startup.phase. Only trustworthy (fresh and, for
    // the sender, self-reported ready/recovering/starting) telemetry can
    // answer this; otherwise it is honestly "unknown" rather than assumed.
    node["phase"] = (live && !stale) ? std::string(node_phase_name(live->phase)) : "unknown";
    node["observed_at_unix_ms"] =
        live ? live->observed_unix_ms
             : (telemetry_known ? durable.observed_unix_ms
                                : (member ? member->seen_unix_ms : durable.observed_unix_ms));
    node["live_age_ms"] = live ? Json(live_age_ms) : Json(nullptr);
    node["version"] = live ? live->version : durable.version;
    node["host"] = member ? member->host : (live ? live->host : durable.host);
    node["port"] =
        static_cast<uint64_t>(member ? member->port : (live ? live->port : durable.port));
    node["failure_domain"] =
        member ? member->failure_domain : (live ? live->failure_domain : durable.failure_domain);
    // Membership and telemetry are two independent sightings of the same
    // monotonic counter, and either can be the older one: membership carries 0
    // for a peer whose record predates its first generation notice, while a
    // stale sample lags a peer that has since advanced. Taking the larger is
    // therefore the fresher answer, not a guess -- reporting a healthy peer as
    // generation 0 because membership happened to win is the bug this avoids.
    // The durable last-known value answers only when neither source has one.
    const uint64_t observed_generation =
        std::max(member ? member->metadata_generation : uint64_t{0},
                 live ? live->metadata_generation : uint64_t{0});
    node["metadata_generation"] =
        observed_generation ? observed_generation : durable.metadata_generation;

    // Where other clients should reach this node's HTTP API - distinct from
    // host/port above, which is the RPC bind address and not necessarily the
    // right port (or even protocol) for a REST call. Omitted entirely rather
    // than reported as empty/0 when unknown, so older or API-less peers in a
    // mixed cluster are simply not discovered rather than guessed at.
    // The API endpoint, complete with scheme, distinct from host/port above
    // which is this node's RPC address and is never proxied.
    const auto& api_endpoint = live ? live->api_endpoint : durable.api_endpoint;
    if (!api_endpoint.empty()) {
        node["api_endpoint"] = api_endpoint;
    }

    const auto effective = effective_telemetry(live, stale, durable, telemetry_known);
    const bool telemetry_available = effective.authoritative || telemetry_known;
    const auto storage_capacity_for_roles =
        telemetry_available ? effective.storage_capacity : (member ? member->capacity : 0);
    node["storage"] =
        telemetry_available
            ? bytes_pair(effective.storage_used, effective.storage_capacity)
            : unavailable_bytes(member ? std::optional<uint64_t>(member->capacity) : std::nullopt);
    node["cache"] =
        telemetry_available ? bytes_pair(effective.cache_used, effective.cache_capacity) : unavailable_bytes();
    node["storage_backends_online"] =
        telemetry_available ? Json(static_cast<uint64_t>(effective.storage_backends_online)) : Json(nullptr);

    Json::Array roles;
    if (storage_capacity_for_roles)
        roles.emplace_back("storage");
    if (telemetry_available && effective.cache_capacity)
        roles.emplace_back("cache");
    if (metadata_replica)
        roles.emplace_back("metadata-replica");
    node["roles"] = std::move(roles);

    // Unlike capacity and usage above, these are measurements of the sending
    // process itself at a stated instant: an old load average is an old
    // measurement, not a fabricated one, and `telemetry_freshness` plus
    // `live_age_ms` already tell the consumer exactly how old. Blanking them
    // the moment a sample crosses the freshness line (15s at the default
    // heartbeat) is what leaves an operator with no view of a busy or distant
    // node at the moment they most want one, so a stale sample keeps them.
    // Nothing aggregates these across nodes, so a stale figure cannot leak
    // into a cluster-wide total the way a stale capacity would.
    Json::Object runtime;
    if (live && online) {
        runtime["uptime_ms"] = live->uptime_ms;
        runtime["rss_bytes"] = live->rss_bytes;
        runtime["process_cpu_percent"] =
            static_cast<double>(live->process_cpu_milli_percent) / 1000.0;
        runtime["load1"] = static_cast<double>(live->load1_milli) / 1000.0;
        // Only when known. load1 and process_cpu_percent are per-core, so a
        // consumer needs this to compare them across non-uniform nodes -- and
        // an absent field it can abstain on is safer than a zero it might
        // divide by.
        if (live->cpu_cores)
            runtime["cpu_cores"] = static_cast<uint64_t>(live->cpu_cores);
        // Physical RAM, and only when known. rss_bytes above is this process's
        // own resident set, which is a different quantity by orders of
        // magnitude -- a consumer labelling either as "memory" wants this one.
        if (live->memory_total_bytes)
            runtime["memory_total_bytes"] = live->memory_total_bytes;
        runtime["peers_known"] = static_cast<uint64_t>(live->peers_known);
        runtime["peers_active"] = static_cast<uint64_t>(live->peers_active);
        runtime["rpc_connections_created"] = live->rpc_connections_created;
        runtime["rpc_connections_reused"] = live->rpc_connections_reused;
        runtime["rpc_connections_canonical"] = live->rpc_connections_canonical;
    }
    node["runtime"] = std::move(runtime);

    // Configuration this node enforces, not a measurement of it -- separate
    // from `runtime` above for that reason. A client needs these about every
    // node it might fail over to, not only the one it is playing from, which
    // is why they ride the payload that already describes every node rather
    // than a per-endpoint call the client would have to make N times.
    //
    // Omitted when the node did not report them: an older node, or one with
    // streaming disabled. A consumer must read absence as "this node cannot
    // say" and fall back to its own conservative bound -- never shorten a
    // budget on the strength of a missing field, and never substitute another
    // node's figure, which is a fact about that node and not this one.
    Json::Object playback;
    if (live && online) {
        if (live->playback_startup_timeout_ms)
            playback["startup_timeout_ms"] =
                static_cast<uint64_t>(live->playback_startup_timeout_ms);
        if (live->playback_segment_timeout_ms)
            playback["segment_timeout_ms"] =
                static_cast<uint64_t>(live->playback_segment_timeout_ms);
        if (live->playback_pipeline_idle_ms)
            playback["pipeline_idle_ms"] =
                static_cast<uint64_t>(live->playback_pipeline_idle_ms);
        if (live->playback_session_idle_ms)
            playback["session_idle_ms"] =
                static_cast<uint64_t>(live->playback_session_idle_ms);
        // The limit only; the current count is deliberately not here. This
        // payload is cached by its consumers, and the count is the most
        // perishable number the API carries -- it moves whenever anyone on the
        // account starts or stops anything, from a device neither end can see.
        // It appears only where it is computed live: the creation payload, the
        // collection listing, and the refusal.
        if (live->playback_max_sessions_per_account)
            playback["max_sessions_per_account"] =
                static_cast<uint64_t>(live->playback_max_sessions_per_account);
        // The node-wide cap beside the per-account one. 0.48.0 shipped the
        // second without the first, which left the two 429s asymmetric where
        // it mattered: a client could say "another screen on this account is
        // playing" and could not say "this node is full".
        if (live->playback_max_sessions)
            playback["max_sessions"] = static_cast<uint64_t>(live->playback_max_sessions);
        // How long this node lets a session hold a transcode entitlement with
        // no stream activity. A client pausing for longer must refresh it --
        // a playlist fetch is enough -- or reacquire on resume and risk a 429.
        if (live->playback_transcode_entitlement_idle_ms)
            playback["transcode_entitlement_idle_ms"] =
                static_cast<uint64_t>(live->playback_transcode_entitlement_idle_ms);
    }
    node["playback"] = std::move(playback);
    node["identity_association_reset"] =
        identity_reset ? identity_reset_json(*identity_reset) : Json(nullptr);
    return node;
}

std::optional<NodeId> parse_node_id(std::string_view text) {
    auto raw = unhex(std::string(text));
    if (!raw || raw->size() != 16)
        return {};
    NodeId id;
    std::copy(raw->begin(), raw->end(), id.bytes.begin());
    return id;
}

} // namespace

ClusterStatusService::ClusterStatusService(NodeRuntime& node) : node_(node) {}

void ClusterStatusService::attach_fuse_diagnostics(
    std::function<std::optional<FuseFrontendDiagnostics>()> provider) {
    std::lock_guard lock(operational_diagnostics_mutex_);
    fuse_diagnostics_ = std::move(provider);
}

void ClusterStatusService::detach_fuse_diagnostics() {
    std::lock_guard lock(operational_diagnostics_mutex_);
    fuse_diagnostics_ = {};
}

void ClusterStatusService::attach_repair_diagnostics(
    std::function<DistributedStore::RepairDiagnostics()> provider) {
    std::lock_guard lock(operational_diagnostics_mutex_);
    repair_diagnostics_ = std::move(provider);
}

void ClusterStatusService::detach_repair_diagnostics() {
    std::lock_guard lock(operational_diagnostics_mutex_);
    repair_diagnostics_ = {};
}

void ClusterStatusService::attach_convergence_diagnostics(
    std::function<ConvergenceDemandDiagnostics()> provider) {
    std::lock_guard lock(operational_diagnostics_mutex_);
    convergence_diagnostics_ = std::move(provider);
}

void ClusterStatusService::attach_subsystem_diagnostics(
    std::function<std::vector<SubsystemStatus>()> provider) {
    std::lock_guard lock(operational_diagnostics_mutex_);
    subsystem_diagnostics_ = std::move(provider);
}

void ClusterStatusService::detach_subsystem_diagnostics() {
    std::lock_guard lock(operational_diagnostics_mutex_);
    subsystem_diagnostics_ = {};
}

void ClusterStatusService::attach_http_diagnostics(
    std::function<std::optional<HttpServerDiagnostics>()> provider) {
    std::lock_guard lock(operational_diagnostics_mutex_);
    http_diagnostics_ = std::move(provider);
}

ClusterStatusService::~ClusterStatusService() {
    stop();
}

void ClusterStatusService::start() {
    if (persistence_.joinable())
        return;
    persistence_ = std::jthread([this](std::stop_token stop) {
        run_supervised("status-persistence", [this, stop] { persistence_loop(stop); });
    });
}

void ClusterStatusService::request_stop() {
    if (!persistence_.joinable())
        return;
    persistence_.request_stop();
    wait_cv_.notify_all();
}

void ClusterStatusService::stop() {
    request_stop();
    if (persistence_.joinable())
        persistence_.join();
}

void ClusterStatusService::persist_local_status() {
    // Status persistence is deliberately outside namespace metadata. Even a
    // five-minute observational checkpoint must never serialize a large
    // namespace, acquire the metadata mutation lock, or enter metadata publication CAS.
    // Defer the tiny local durable write while viewer-critical work is active.
    // durable_replace_file() includes the durability barrier we want for the
    // last-known cache. Keep that I/O well clear of interactive traffic rather
    // than allowing observational state to introduce an fsync into a busy node.
    constexpr auto idle_before_persist = std::chrono::seconds(30);
    if (node_.activity_idle_for(FrameType::foreground) < idle_before_persist ||
        node_.activity_idle_for(FrameType::read_ahead) < idle_before_persist)
        return;
    node_.telemetry().persist();
    node_.sessions().persist();
}

void ClusterStatusService::persistence_loop(std::stop_token stop) {
    // Give startup membership/metadata formation a short head start, then keep one
    // coalesced durable observation per node. A failed checkpoint is retried; no
    // historical telemetry backlog is ever replayed into metadata.
    auto delay = std::chrono::seconds(10);
    while (!stop.stop_requested()) {
        std::unique_lock lock(wait_mutex_);
        wait_cv_.wait_for(lock, stop, delay, [] { return false; });
        lock.unlock();
        if (stop.stop_requested())
            break;
        try {
            persist_local_status();
            delay = std::chrono::minutes(5);
        } catch (const std::exception& error) {
            Log::debug("status checkpoint unavailable: " + std::string(error.what()));
            delay = std::chrono::seconds(30);
        }
    }
}

HttpResponse ClusterStatusService::status_response(const std::optional<NodeId>& only) {
    auto* metadata_manager = metadata_.load(std::memory_order_acquire);
    std::shared_ptr<const MetadataSnapshot> metadata;
    uint64_t metadata_generation = 0;
    if (metadata_manager) {
        try {
            if (auto available = metadata_manager->available_snapshot_view()) {
                metadata = available->snapshot;
                metadata_generation = available->generation;
            }
        } catch (const std::exception& error) {
            Log::debug("status metadata unavailable: " + std::string(error.what()));
        }
    }

    // Current cluster membership is independent of telemetry. These are small,
    // in-memory snapshots under Membership's short mutex; they do no network or
    // disk I/O. Telemetry only decorates members after this authoritative view
    // has been established.
    const auto membership = node_.membership().snapshot();
    const auto& membership_all = membership.all;
    const auto& membership_active = membership.active;
    std::set<NodeId> active_members;
    std::map<NodeId, NodeInfo> members_by_id;
    for (const auto& member : membership_all)
        members_by_id.emplace(member.id, member);
    for (const auto& member : membership_active)
        active_members.insert(member.id);

    const auto fresh_for = std::max(node_.config().heartbeat * 3, std::chrono::milliseconds(5000));
    auto views = node_.telemetry().views(fresh_for);
    std::map<NodeId, TelemetryView> live;
    for (auto& view : views)
        live.emplace(view.telemetry.node_id, std::move(view));

    std::map<NodeId, PersistedNodeStatus> known;
    if (metadata)
        known = metadata->node_status; // Read-only compatibility with early SM9 checkpoints.
    for (const auto& telemetry : node_.telemetry().persisted())
        known[telemetry.node_id] = persisted(telemetry);
    // A stale, or fresh-but-still-recovering, live view must not clobber the
    // durable/last-known record with itself: only a genuinely fresh AND ready
    // observation should become the new "last known" baseline. Otherwise a
    // node's numbers would never actually fall back to anything different
    // once its telemetry goes stale, and a node's own in-progress recovery
    // (which legitimately reports zero capacity/usage while not yet ready)
    // would overwrite its last known-good figures with that same zero within
    // this very call.
    for (const auto& [id, view] : live)
        if (view.fresh && view.telemetry.phase == NodePhase::ready)
            known[id] = persisted(view.telemetry);
    for (const auto& member : membership_all)
        merge_membership(known[member.id], member);

    std::set<NodeId> telemetry_known;
    if (metadata)
        for (const auto& [id, _] : metadata->node_status)
            telemetry_known.insert(id);
    for (const auto& telemetry : node_.telemetry().persisted())
        telemetry_known.insert(telemetry.node_id);
    // Mirror the fresh-and-ready gate above: "known" here means a genuinely
    // trustworthy durable/last-known source exists, not merely that some
    // telemetry (however stale or still-recovering) was once observed.
    for (const auto& [id, view] : live)
        if (view.fresh && view.telemetry.phase == NodePhase::ready)
            telemetry_known.insert(id);

    // Operational resets are locally durable before their optional metadata
    // audit. Merge both sources so Status reflects retirement immediately.
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_resets;
    if (metadata)
        identity_resets = metadata->identity_resets;
    for (const auto& reset : node_.identity_resets()) {
        const auto key = identity_reset_key(reset.host, reset.port);
        auto found = identity_resets.find(key);
        if (found == identity_resets.end() || found->second.epoch < reset.epoch)
            identity_resets[key] = reset;
    }

    uint64_t known_capacity = 0, known_used = 0, online_capacity = 0, online_used = 0;
    uint64_t known_cache_capacity = 0, known_cache_used = 0, online_cache_capacity = 0,
             online_cache_used = 0;
    bool known_storage_available = true, online_storage_available = true;
    bool known_cache_available = true, online_cache_available = true;
    size_t known_nodes = 0, online_nodes = 0;
    size_t inbound_incapable_nodes = 0, known_hosting_nodes = 0, online_hosting_nodes = 0;
    size_t online_capable_hosting_nodes = 0;
    bool online_node_recovering = false;
    const auto local_resolution = node_.inbound_resolution();
    Json::Array nodes;
    for (const auto& [id, durable] : known) {
        if (only && id != *only)
            continue;
        const auto found = live.find(id);
        const NodeTelemetry* current = found == live.end() ? nullptr : &found->second.telemetry;
        const auto member_found = members_by_id.find(id);
        const NodeInfo* member =
            member_found == members_by_id.end() ? nullptr : &member_found->second;
        const uint64_t age =
            found == live.end() ? 0 : static_cast<uint64_t>(found->second.age.count());
        const bool online = active_members.contains(id);
        const bool stale = current && found->second.age > fresh_for;
        const bool metadata_replica = true;

        const auto& host = member ? member->host : (current ? current->host : durable.host);
        const auto port = member ? member->port : (current ? current->port : durable.port);
        const auto observed_unix_ms = std::max(
            {durable.observed_unix_ms, member ? member->seen_unix_ms : uint64_t{},
             current ? current->observed_unix_ms : uint64_t{}});
        const IdentityAssociationReset* identity_reset = nullptr;
        if (!host.empty() && port) {
            for (const auto& [_, reset] : identity_resets) {
                if (!identity_reset_matches_endpoint(reset, host, port) ||
                    !identity_reset_matches_node(reset, id))
                    continue;
                if (!identity_reset || reset.reset_unix_ms > identity_reset->reset_unix_ms)
                    identity_reset = &reset;
            }
        }
        // A reset retires the pre-reset durable identity, but it is only a
        // freshness boundary: a later directly authenticated observation of
        // the same identity remains an ordinary live/known node.
        const bool retired = !online && identity_reset &&
                             observed_unix_ms <= identity_reset->reset_unix_ms;
        const InboundResolution* resolution_for_node =
            id == node_.node_id() ? &local_resolution : nullptr;
        if (retired) {
            if (only)
                nodes.push_back(node_json(id, durable, member, current, age, false, stale, true,
                                          telemetry_known.contains(id), metadata_replica,
                                          identity_reset, resolution_for_node));
            continue;
        }
        ++known_nodes;
        // A node that hosts no extents has no durable capacity to count, and
        // must not make the aggregate look short of something it never had.
        const bool hosting = !member || node_hosts_extents(*member);
        const bool inbound_capable = !member || node_inbound_capable(*member);
        if (!inbound_capable)
            ++inbound_incapable_nodes;
        if (hosting)
            ++known_hosting_nodes;

        const auto effective = effective_telemetry(current, stale, durable, telemetry_known.contains(id));
        const bool telemetry_available = effective.authoritative || telemetry_known.contains(id);
        const auto storage_capacity =
            !hosting ? 0
                     : (telemetry_available ? effective.storage_capacity
                                            : (member ? member->capacity : 0));
        known_capacity += storage_capacity;
        known_storage_available = known_storage_available && (telemetry_available || !hosting);
        known_cache_available = known_cache_available && telemetry_available;
        if (telemetry_available) {
            if (hosting)
                known_used += effective.storage_used;
            known_cache_capacity += effective.cache_capacity;
            known_cache_used += effective.cache_used;
        }
        if (online) {
            ++online_nodes;
            if (hosting)
                ++online_hosting_nodes;
            if (hosting && inbound_capable)
                ++online_capable_hosting_nodes;
            online_capacity += storage_capacity;
            online_storage_available = online_storage_available && (telemetry_available || !hosting);
            online_cache_available = online_cache_available && telemetry_available;
            if (telemetry_available) {
                if (hosting)
                    online_used += effective.storage_used;
                online_cache_capacity += effective.cache_capacity;
                online_cache_used += effective.cache_used;
            }
            // Self's own readiness is already reported synchronously and
            // exactly via `readiness` above; this signal exists to surface a
            // *remote* peer's recovery, which has no other synchronous
            // source. Self's own published telemetry can briefly lag its own
            // readiness transition, so including it here would just be a
            // redundant, racier duplicate of the existing local check.
            if (id != node_.node_id() && current && !stale && current->phase != NodePhase::ready)
                online_node_recovering = true;
        }
        nodes.push_back(node_json(id, durable, member, current, age, online, stale, false,
                                  telemetry_available, metadata_replica, identity_reset,
                                  resolution_for_node));
    }

    if (only) {
        if (nodes.empty())
            return http_error(404, "node_not_found", "unknown cluster node");
        return http_json(200, nodes.front().dump());
    }

    const auto published_metadata =
        metadata_manager ? metadata_manager->cluster_status() : MetadataClusterStatus{};
    const size_t metadata_replicas = known_nodes == 0 ? published_metadata.replicas : known_nodes;
    const size_t active_metadata_replicas = online_nodes;
    const size_t metadata_min_write_replicas = node_.config().metadata_min_write_replicas;

    // Read availability comes from the already-decoded committed snapshot.
    // Write availability is the durability floor: validation/stability is
    // reported separately because reconciliation debt does not revoke the right
    // of any reachable floor-sized cohort to attempt a metadata mutation.
    MetadataAvailability metadata_availability = published_metadata.availability;
    if (metadata) {
        if (metadata_availability == MetadataAvailability::unavailable)
            metadata_availability = MetadataAvailability::read_only;
        if (metadata_availability == MetadataAvailability::writable &&
            active_metadata_replicas < metadata_min_write_replicas)
            metadata_availability = MetadataAvailability::read_only;
    } else {
        metadata_availability = MetadataAvailability::unavailable;
    }
    const bool metadata_read_available = metadata_availability != MetadataAvailability::unavailable;
    const bool metadata_write_available = metadata_availability == MetadataAvailability::writable;

    const auto readiness = node_.readiness();
    HealthSeverity health = HealthSeverity::healthy;
    Json::Array conditions;
    if (readiness.failed) {
        escalate(health, HealthSeverity::critical);
        conditions.emplace_back("local startup recovery failed");
    } else if (!readiness.local_state_ready || !metadata_manager) {
        escalate(health, HealthSeverity::recovering);
        conditions.emplace_back(readiness.local_state_ready ? "local services are starting"
                                                            : "local state is recovering");
    } else if (!metadata_read_available) {
        escalate(health, HealthSeverity::critical);
        conditions.emplace_back("metadata unavailable");
    } else if (!metadata_write_available) {
        escalate(health, HealthSeverity::degraded);
        conditions.emplace_back("metadata read-only");
    }
    if (online_nodes < known_nodes) {
        escalate(health, HealthSeverity::degraded);
        conditions.emplace_back("one or more known nodes are offline");
    }
    if (online_node_recovering) {
        escalate(health, HealthSeverity::degraded);
        conditions.emplace_back("one or more online nodes are still recovering");
    }
    if (online_capacity < known_capacity) {
        escalate(health, HealthSeverity::degraded);
        conditions.emplace_back("some known durable capacity is unavailable");
    }
    // Nodes that accept no inbound connections, and what that does to where
    // extents can live (0.42.0). The first is information, not a fault; the
    // other two are the shapes decision 3 of the plan rules out.
    if (inbound_incapable_nodes)
        conditions.emplace_back(std::to_string(inbound_incapable_nodes) + " node" +
                                (inbound_incapable_nodes == 1 ? " accepts" : "s accept") +
                                " no inbound connections");
    if (online_nodes && !online_capable_hosting_nodes) {
        escalate(health, HealthSeverity::critical);
        conditions.emplace_back("no inbound-capable node hosts extents");
    }
    const auto replication = node_.config().replication;
    if (replication > known_hosting_nodes) {
        escalate(health, HealthSeverity::degraded);
        conditions.emplace_back("replication " + std::to_string(replication) + " requires " +
                                std::to_string(replication) + " extent-hosting nodes; " +
                                std::to_string(known_hosting_nodes) + " known");
    }

    Json::Object cluster;
    cluster["health"] = std::string(health_name(health));
    cluster["conditions"] = std::move(conditions);
    cluster["nodes_known"] = static_cast<uint64_t>(known_nodes);
    cluster["nodes_online"] = static_cast<uint64_t>(online_nodes);
    cluster["nodes_hosting_extents"] = static_cast<uint64_t>(known_hosting_nodes);
    cluster["nodes_hosting_extents_online"] = static_cast<uint64_t>(online_hosting_nodes);
    cluster["nodes_inbound_incapable"] = static_cast<uint64_t>(inbound_incapable_nodes);
    cluster["metadata_generation"] =
        metadata_generation ? metadata_generation : published_metadata.generation;
    cluster["metadata_replicas"] = static_cast<uint64_t>(metadata_replicas);
    cluster["metadata_replicas_online"] = static_cast<uint64_t>(active_metadata_replicas);
    cluster["metadata_min_write_replicas"] = static_cast<uint64_t>(metadata_min_write_replicas);
    // Transitional API aliases for 0.18 clients. They carry the new values and
    // should not be interpreted as a fixed voter set or majority quorum.
    cluster["metadata_voters"] = static_cast<uint64_t>(metadata_replicas);
    cluster["metadata_voters_online"] = static_cast<uint64_t>(active_metadata_replicas);
    cluster["metadata_quorum_required"] = static_cast<uint64_t>(metadata_min_write_replicas);
    cluster["metadata_availability"] = metadata_availability_name(metadata_availability);
    cluster["metadata_read_available"] = metadata_read_available;
    cluster["metadata_quorum_available"] = metadata_write_available; // deprecated alias
    cluster["metadata_write_available"] = metadata_write_available;
    cluster["metadata_replica_set_validated"] = published_metadata.stable;
    cluster["metadata_quorum_validated"] = published_metadata.stable; // deprecated alias
    cluster["metadata_replica_set_validated_at_unix_ms"] = published_metadata.observed_unix_ms;
    cluster["metadata_quorum_validated_at_unix_ms"] =
        published_metadata.observed_unix_ms; // deprecated alias
    cluster["storage_known"] = known_storage_available ? bytes_pair(known_used, known_capacity)
                                                       : unavailable_bytes(known_capacity);
    cluster["storage_online"] = online_storage_available ? bytes_pair(online_used, online_capacity)
                                                         : unavailable_bytes(online_capacity);
    cluster["cache_known"] = known_cache_available
                                 ? bytes_pair(known_cache_used, known_cache_capacity)
                                 : unavailable_bytes();
    cluster["cache_online"] = online_cache_available
                                  ? bytes_pair(online_cache_used, online_cache_capacity)
                                  : unavailable_bytes();

    Json::Object startup;
    startup["phase"] = readiness.failed
                           ? "failed"
                           : (readiness.local_state_ready && metadata_manager
                                  ? "ready"
                                  : (readiness.control_plane_online ? "recovering" : "starting"));
    startup["control_plane"] = readiness.control_plane_online ? "ready" : "starting";
    startup["api"] = "ready";
    startup["data_storage"] = readiness.data_storage_ready ? "ready" : "recovering";
    startup["control_storage"] = readiness.control_storage_ready ? "ready" : "recovering";
    startup["cache"] = readiness.cache_ready ? "ready" : "recovering";
    startup["retention"] = readiness.retention_ready ? "ready" : "recovering";
    startup["metadata"] = readiness.metadata_ready ? "ready" : "recovering";
    startup["services"] = metadata_manager ? "ready" : "recovering";
    startup["started_at_unix_ms"] = readiness.started_unix_ms;
    startup["ready_at_unix_ms"] =
        readiness.ready_unix_ms ? Json(readiness.ready_unix_ms) : Json(nullptr);
    startup["error"] = readiness.error.empty() ? Json(nullptr) : Json(readiness.error);

    Json::Object root;
    root["cluster"] = std::move(cluster);
    root["startup"] = std::move(startup);
    root["nodes"] = std::move(nodes);
    {
        auto connectivity = public_connectivity_json(node_.public_connectivity_status());
        add_inbound_resolution(connectivity.asObject(), local_resolution);
        root["connectivity"] = std::move(connectivity);
    }

    std::function<std::vector<SubsystemStatus>()> subsystem_provider;
    {
        std::lock_guard lock(operational_diagnostics_mutex_);
        subsystem_provider = subsystem_diagnostics_;
    }
    Json::Array subsystems;
    if (subsystem_provider) {
        for (const auto& status : subsystem_provider()) {
            Json::Object entry;
            entry["name"] = status.name;
            entry["state"] = std::string(subsystem_state_name(status.state));
            entry["restart_count"] = static_cast<uint64_t>(status.restart_count);
            entry["last_fault"] = status.last_fault.empty() ? Json(nullptr) : Json(status.last_fault);
            subsystems.push_back(std::move(entry));
        }
    }
    root["subsystems"] = std::move(subsystems);
    // Named rather than assumed: a client that was reading `diagnostics` off
    // this response and now finds it absent would otherwise get `undefined`
    // and no explanation, which is the silent-nothing failure this project has
    // been bitten by before. The pointer travels with the payload.
    root["diagnostics_endpoint"] = std::string(diagnostics_path);
    root["generated_at_unix_ms"] = unix_ms();
    return http_json(200, Json(std::move(root)).dump());
}

// Everything above this line is membership, telemetry and readiness the node
// already holds decoded: a handful of short mutexes, no I/O, no network. What
// follows is the other kind, and it moved here in 0.39.1 so that polling the
// first no longer pays for the second. See status_api.hpp for why that
// distinction is about locks rather than about arithmetic.
HttpResponse ClusterStatusService::diagnostics_response() {
    auto* metadata_manager = metadata_.load(std::memory_order_acquire);
    std::shared_ptr<const MetadataSnapshot> metadata;
    if (metadata_manager) {
        try {
            if (auto available = metadata_manager->available_snapshot_view())
                metadata = available->snapshot;
        } catch (const std::exception& error) {
            Log::debug("status metadata unavailable: " + std::string(error.what()));
        }
    }
    const auto readiness = node_.readiness();

    // Process-lifetime aggregate diagnostics are read directly from local
    // atomics. They create no sampling loop, persistence work, or gossip load.
    Json::Object diagnostics;
    Json::Object metadata_diagnostics;
    metadata_diagnostics["available"] = readiness.metadata_ready;
    if (readiness.metadata_ready) {
        const auto values = node_.metadata_replica().diagnostics();
        metadata_diagnostics["historical_requests"] = values.historical_requests;
        metadata_diagnostics["historical_reconstructions"] = values.historical_reconstructions;
        metadata_diagnostics["historical_deltas_applied"] = values.historical_deltas_applied;
        metadata_diagnostics["materialization_cache_hits"] = values.materialization_cache_hits;
        metadata_diagnostics["materialization_cache_misses"] = values.materialization_cache_misses;
        metadata_diagnostics["materialization_cache_evictions"] =
            values.materialization_cache_evictions;
        metadata_diagnostics["materialization_cache_entries"] =
            static_cast<uint64_t>(values.materialization_cache_entries);
        metadata_diagnostics["materialization_cache_bytes"] =
            values.materialization_cache_bytes;
        metadata_diagnostics["materialization_cache_limit_bytes"] =
            values.materialization_cache_limit_bytes;
        metadata_diagnostics["history_records"] = values.history_records;
        metadata_diagnostics["history_file_bytes"] = values.history_file_bytes;
        metadata_diagnostics["history_resident_payload_bytes"] =
            values.history_resident_payload_bytes;
        metadata_diagnostics["accepted_head_persistence_writes"] =
            values.accepted_head_persistence_writes;
        metadata_diagnostics["accepted_head_persistence_bytes"] =
            values.accepted_head_persistence_bytes;
        metadata_diagnostics["accepted_head_persistence_failures"] =
            values.accepted_head_persistence_failures;
    }
    // Discipline 4: standing conflicts are visible here and listed/resolved
    // under /api/v1/manage/metadata/conflicts; superseded/resolved are
    // process-lifetime counters of conflicts that left the snapshot.
    if (metadata) {
        uint64_t namespace_conflicts = 0, catalogue_conflicts = 0;
        for (const auto& [id, conflict] : metadata->conflicts) {
            if (conflict.kind == MetadataConflictKind::namespace_entry)
                ++namespace_conflicts;
            else
                ++catalogue_conflicts;
        }
        metadata_diagnostics["conflicts"] = static_cast<uint64_t>(metadata->conflicts.size());
        metadata_diagnostics["namespace_conflicts"] = namespace_conflicts;
        metadata_diagnostics["catalogue_conflicts"] = catalogue_conflicts;
        metadata_diagnostics["tombstones"] = static_cast<uint64_t>(metadata->garbage.size());
    }
    if (metadata_manager) {
        metadata_diagnostics["conflicts_superseded"] = metadata_manager->conflicts_superseded();
        metadata_diagnostics["conflicts_resolved"] = metadata_manager->conflicts_resolved();
        // Where a mutation's wall time goes since start: the pre-publication
        // retention barrier and the commit fan-out (totals and maxima, ms).
        const auto timing = metadata_manager->mutation_timing();
        metadata_diagnostics["mutations"] = timing.mutations;
        metadata_diagnostics["mutation_retention_ms_total"] = timing.retention_ms_total;
        metadata_diagnostics["mutation_retention_ms_max"] = timing.retention_ms_max;
        metadata_diagnostics["mutation_publish_ms_total"] = timing.publish_ms_total;
        metadata_diagnostics["mutation_publish_ms_max"] = timing.publish_ms_max;
    }
    diagnostics["metadata"] = std::move(metadata_diagnostics);

    const auto rpc = node_.rpc_server_work_stats();
    Json::Object rpc_diagnostics;
    rpc_diagnostics["metadata_pending_jobs"] = static_cast<uint64_t>(rpc.metadata_pending_jobs);
    rpc_diagnostics["metadata_pending_bytes"] = static_cast<uint64_t>(rpc.metadata_pending_bytes);
    rpc_diagnostics["metadata_active_jobs"] = static_cast<uint64_t>(rpc.metadata_active_jobs);
    rpc_diagnostics["metadata_rejected_jobs"] = rpc.metadata_rejected_jobs;
    rpc_diagnostics["fast_control_pending_bytes"] =
        static_cast<uint64_t>(rpc.fast_control_pending_bytes);
    rpc_diagnostics["control_pending_bytes"] =
        static_cast<uint64_t>(rpc.control_pending_bytes);
    rpc_diagnostics["data_pending_bytes"] = static_cast<uint64_t>(rpc.data_pending_bytes);
    rpc_diagnostics["rejected_jobs"] = rpc.rejected_jobs;
    Json::Object frame_timings;
    for (const auto& [frame, timing] : rpc.frame_timings)
        frame_timings[frame_type_name(frame)] = rpc_timing_json(timing);
    rpc_diagnostics["frame_timings"] = std::move(frame_timings);
    Json::Object message_timings;
    for (const auto& [message, timing] : rpc.message_timings)
        message_timings[message_type_name(message)] = rpc_timing_json(timing);
    rpc_diagnostics["message_timings"] = std::move(message_timings);
    diagnostics["rpc_server"] = std::move(rpc_diagnostics);

    const auto transport = node_.rpc_stats();
    Json::Object transport_diagnostics;
    transport_diagnostics["connections_created"] = transport.connections_created;
    transport_diagnostics["connections_reused"] = transport.connections_reused;
    // Smoothed CONTROL-lane round trip per peer (node id -> ms), the same
    // measure commit fan-out uses to try the nearest replicas first.
    Json::Object peer_latency;
    for (const auto& [peer, latency] : node_.peer_latencies())
        peer_latency[to_string(peer)] = static_cast<uint64_t>(latency.count());
    transport_diagnostics["peer_latency_ms"] = std::move(peer_latency);
    transport_diagnostics["canonical_connections"] = transport.canonical_connections;
    diagnostics["rpc_transport"] = std::move(transport_diagnostics);

    const auto data_resource = node_.data_resources().stats();
    Json::Object data_resource_diagnostics;
    data_resource_diagnostics["capacity_bytes"] = data_resource.capacity_bytes;
    data_resource_diagnostics["viewer_reserve_bytes"] = data_resource.viewer_reserve_bytes;
    data_resource_diagnostics["used_bytes"] = data_resource.used_bytes;
    data_resource_diagnostics["peak_used_bytes"] = data_resource.peak_used_bytes;
    data_resource_diagnostics["viewer_admissions"] = data_resource.viewer_admissions;
    data_resource_diagnostics["loader_admissions"] = data_resource.loader_admissions;
    data_resource_diagnostics["speculative_admissions"] =
        data_resource.speculative_admissions;
    data_resource_diagnostics["viewer_waits"] = data_resource.viewer_waits;
    data_resource_diagnostics["loader_waits"] = data_resource.loader_waits;
    data_resource_diagnostics["speculative_waits"] = data_resource.speculative_waits;
    // Background effort ceiling (maintenance.background_concurrency) and use.
    data_resource_diagnostics["background_limit"] = data_resource.background_limit;
    data_resource_diagnostics["background_active"] = data_resource.background_active;
    data_resource_diagnostics["peak_background_active"] = data_resource.peak_background_active;
    data_resource_diagnostics["cancelled_waits"] = data_resource.cancelled_waits;
    diagnostics["data_resources"] = std::move(data_resource_diagnostics);

    const auto retained_memory = node_.retained_memory().stats();
    Json::Object retained_memory_diagnostics;
    retained_memory_diagnostics["capacity_bytes"] = retained_memory.capacity_bytes;
    retained_memory_diagnostics["control_reserve_bytes"] =
        retained_memory.control_reserve_bytes;
    retained_memory_diagnostics["viewer_reserve_bytes"] =
        retained_memory.viewer_reserve_bytes;
    retained_memory_diagnostics["loader_reserve_bytes"] =
        retained_memory.loader_reserve_bytes;
    // The slice only inbound frame reassembly may draw on. Reported because
    // its exhaustion is what turns a stuck publication into a node whose peer
    // channels drop, and an operator needs to see the two side by side.
    retained_memory_diagnostics["reassembly_reserve_bytes"] =
        retained_memory.reassembly_reserve_bytes;
    retained_memory_diagnostics["used_bytes"] = retained_memory.used_bytes;
    retained_memory_diagnostics["peak_used_bytes"] = retained_memory.peak_used_bytes;
    retained_memory_diagnostics["reclaimable_bytes"] = retained_memory.reclaimable_bytes;
    retained_memory_diagnostics["shed_requests"] = retained_memory.shed_requests;
    retained_memory_diagnostics["cancelled_waits"] = retained_memory.cancelled_waits;
    retained_memory_diagnostics["restored_bytes"] = retained_memory.restored_bytes;
    retained_memory_diagnostics["overcommit_bytes"] =
        retained_memory.used_bytes > retained_memory.capacity_bytes
            ? retained_memory.used_bytes - retained_memory.capacity_bytes
            : 0;
    Json::Object retained_admissions;
    Json::Object retained_waits;
    constexpr std::array<std::string_view, 4> memory_class_names{
        "control", "viewer", "loader", "speculative"};
    for (size_t i = 0; i < memory_class_names.size(); ++i) {
        retained_admissions[std::string(memory_class_names[i])] = retained_memory.admissions[i];
        retained_waits[std::string(memory_class_names[i])] = retained_memory.waits[i];
    }
    retained_memory_diagnostics["admissions"] = std::move(retained_admissions);
    retained_memory_diagnostics["waits"] = std::move(retained_waits);
    Json::Object retained_owners;
    constexpr std::array<std::string_view, static_cast<size_t>(MemoryOwner::count)> owner_names{
        "fuse_request", "fuse_operation", "publication", "rpc_frame", "object_payload",
        "durability", "metadata", "catalogue", "media_profile", "playback_segment", "cache"};
    for (size_t i = 0; i < owner_names.size(); ++i)
        retained_owners[std::string(owner_names[i])] = retained_memory.owner_bytes[i];
    retained_memory_diagnostics["owners"] = std::move(retained_owners);
    diagnostics["retained_memory"] = std::move(retained_memory_diagnostics);

    Json::Object data_store_diagnostics;
    data_store_diagnostics["available"] = false;
    if (readiness.data_storage_ready) {
        try {
            const auto data_store = node_.local_store().diagnostics();
            data_store_diagnostics["available"] = true;
            data_store_diagnostics["loose_reaffirmation_fast_paths"] =
                data_store.loose_reaffirmation_fast_paths;
            data_store_diagnostics["loose_reaffirmation_full_validations"] =
                data_store.loose_reaffirmation_full_validations;
            data_store_diagnostics["presence_index_entries"] = data_store.presence_index_entries;
            data_store_diagnostics["pack_recovery_truncated_tails"] =
                data_store.pack_recovery_truncated_tails;
            data_store_diagnostics["pack_recovery_skipped_regions"] =
                data_store.pack_recovery_skipped_regions;
            data_store_diagnostics["pack_recovery_skipped_bytes"] =
                data_store.pack_recovery_skipped_bytes;
        } catch (...) {
            // Readiness can transition while Status is assembled. Diagnostics
            // are observational and must never make the startup API fail.
            data_store_diagnostics["available"] = false;
        }
    }
    diagnostics["data_store"] = std::move(data_store_diagnostics);

    std::function<std::optional<FuseFrontendDiagnostics>()> fuse_provider;
    std::function<ConvergenceDemandDiagnostics()> convergence_provider;
    std::function<DistributedStore::RepairDiagnostics()> repair_provider;
    std::function<std::optional<HttpServerDiagnostics>()> http_provider;
    {
        std::lock_guard lock(operational_diagnostics_mutex_);
        fuse_provider = fuse_diagnostics_;
        convergence_provider = convergence_diagnostics_;
        repair_provider = repair_diagnostics_;
        http_provider = http_diagnostics_;
    }

    Json::Object filesystem_diagnostics;
    filesystem_diagnostics["available"] = false;
    if (fuse_provider) {
        try {
            if (auto values = fuse_provider()) {
                filesystem_diagnostics["available"] = true;
                filesystem_diagnostics["timed_out_requests"] = values->timed_out_requests;
                filesystem_diagnostics["merged_publications"] = values->merged_publications;
                filesystem_diagnostics["data_publication_requests"] =
                    values->data_publication_requests;
                filesystem_diagnostics["data_publication_notifications_suppressed"] =
                    values->data_publication_notifications_suppressed;
                filesystem_diagnostics["spool_pressure_publication_sweeps"] =
                    values->spool_pressure_publication_sweeps;
                filesystem_diagnostics["data_publication_coalesced_queued"] =
                    values->data_publication_coalesced_queued;
                filesystem_diagnostics["data_publication_coalesced_running"] =
                    values->data_publication_coalesced_running;
                filesystem_diagnostics["data_publication_coalesced_unconfirmed"] =
                    values->data_publication_coalesced_unconfirmed;
                filesystem_diagnostics["data_publications_started"] =
                    values->data_publications_started;
                filesystem_diagnostics["data_publications_completed"] =
                    values->data_publications_completed;
                filesystem_diagnostics["data_publication_peak_active"] =
                    values->data_publication_peak_active;
                filesystem_diagnostics["data_publication_quanta"] =
                    values->data_publication_quanta;
                filesystem_diagnostics["data_publication_yields"] =
                    values->data_publication_yields;
                filesystem_diagnostics["data_publication_peak_inflight_bytes"] =
                    values->data_publication_peak_inflight_bytes;
                filesystem_diagnostics["data_publication_pipeline_limit_bytes"] =
                    values->data_publication_pipeline_limit_bytes;
                filesystem_diagnostics["data_publication_peak_pipeline_extents"] =
                    values->data_publication_peak_pipeline_extents;
                filesystem_diagnostics["data_closed_priority_selections"] =
                    values->data_closed_priority_selections;
                filesystem_diagnostics["data_retirement_priority_selections"] =
                    values->data_retirement_priority_selections;
                filesystem_diagnostics["open_publications"] = values->open_publications;
                filesystem_diagnostics["peak_open_publications"] =
                    values->peak_open_publications;
                filesystem_diagnostics["publication_max_open_writers"] =
                    values->publication_max_open_writers;
                filesystem_diagnostics["data_publication_selections_under_writer_cap"] =
                    values->data_publication_selections_under_writer_cap;
                filesystem_diagnostics["data_publication_progress_events"] =
                    values->data_publication_progress_events;
                filesystem_diagnostics["data_publication_bytes_read"] =
                    values->data_publication_bytes_read;
                filesystem_diagnostics["data_publication_bytes_committed"] =
                    values->data_publication_bytes_committed;
                filesystem_diagnostics["data_publication_bytes_confirmed"] =
                    values->data_publication_bytes_confirmed;
                filesystem_diagnostics["data_publication_completed_spool_bytes_read"] =
                    values->data_publication_completed_spool_bytes_read;
                filesystem_diagnostics["data_publication_completed_source_bytes_read"] =
                    values->data_publication_completed_source_bytes_read;
                filesystem_diagnostics["data_publication_completed_reused_extents"] =
                    values->data_publication_completed_reused_extents;
                filesystem_diagnostics["data_publication_completed_put_extents"] =
                    values->data_publication_completed_put_extents;
                filesystem_diagnostics["data_overlay_read_queries"] =
                    values->data_overlay_read_queries;
                filesystem_diagnostics["data_overlay_ranges_examined"] =
                    values->data_overlay_ranges_examined;
                filesystem_diagnostics["data_overlay_descriptors_copied"] =
                    values->data_overlay_descriptors_copied;
                filesystem_diagnostics["retained_data_operations"] =
                    values->retained_data_operations;
                filesystem_diagnostics["retained_data_operation_bytes"] =
                    values->retained_data_operation_bytes;
                filesystem_diagnostics["retained_overlay_ranges"] =
                    values->retained_overlay_ranges;
                filesystem_diagnostics["retained_overlay_bytes"] =
                    values->retained_overlay_bytes;
                filesystem_diagnostics["retained_publication_operations"] =
                    values->retained_publication_operations;
                filesystem_diagnostics["retained_publication_operation_bytes"] =
                    values->retained_publication_operation_bytes;
                filesystem_diagnostics["operation_metadata_bytes"] =
                    values->operation_metadata_bytes;
                filesystem_diagnostics["peak_operation_metadata_bytes"] =
                    values->peak_operation_metadata_bytes;
                filesystem_diagnostics["operation_metadata_limit_bytes"] =
                    values->operation_metadata_limit_bytes;
                filesystem_diagnostics["operation_metadata_waits"] =
                    values->operation_metadata_waits;
                filesystem_diagnostics["retained_durability_tickets"] =
                    values->retained_durability_tickets;
                filesystem_diagnostics["data_publication_inflight_bytes"] =
                    values->data_publication_inflight_bytes;
                filesystem_diagnostics["backend_failures"] = values->backend_failures;
                filesystem_diagnostics["durability_batches"] = values->durability_batches;
                filesystem_diagnostics["durability_writes"] = values->durability_writes;
                filesystem_diagnostics["namespace_operations_admitted"] =
                    values->namespace_operations_admitted;
                filesystem_diagnostics["namespace_operations_recovered"] =
                    values->namespace_operations_recovered;
                filesystem_diagnostics["namespace_publication_attempts"] =
                    values->namespace_publication_attempts;
                filesystem_diagnostics["namespace_publication_batches"] =
                    values->namespace_publication_batches;
                filesystem_diagnostics["namespace_operations_batched"] =
                    values->namespace_operations_batched;
                filesystem_diagnostics["namespace_operations_published"] =
                    values->namespace_operations_published;
                filesystem_diagnostics["namespace_operations_confirmed"] =
                    values->namespace_operations_confirmed;
                // The mount is stale exactly when refreshed < available. Both
                // are exposed so an operator can see it without a rebuild.
                filesystem_diagnostics["namespace_refreshed_revision"] =
                    values->namespace_refreshed_revision;
                filesystem_diagnostics["namespace_available_revision"] =
                    values->namespace_available_revision;
                // Files whose publication exhausted its retry budget; details
                // and actions under /api/v1/manage/filesystem/parked-publications.
                filesystem_diagnostics["parked_publications"] = values->parked_publications;
                filesystem_diagnostics["publication_retries_backed_off"] =
                    values->publication_retries_backed_off;
                filesystem_diagnostics["publications_retrying_persistently"] =
                    values->publications_retrying_persistently;
                // What recovery resolved instead of refusing (discipline 3).
                filesystem_diagnostics["journal_recovery_skipped_frames"] =
                    values->journal_recovery_skipped_frames;
                filesystem_diagnostics["journal_recovery_quarantined_bytes"] =
                    values->journal_recovery_quarantined_bytes;
                filesystem_diagnostics["recovery_dropped_operations"] =
                    values->recovery_dropped_operations;
                filesystem_diagnostics["publications_abandoned"] =
                    values->publications_abandoned;
                // The host directory under the mount: entries found there at
                // startup are hidden by the mount, and whether the directory
                // is immutable while no mount covers it.
                filesystem_diagnostics["mountpoint_stray_entries"] =
                    fuse_mountpoint_preparation().stray_entries;
                filesystem_diagnostics["mountpoint_immutable"] =
                    fuse_mountpoint_preparation().immutable;
                filesystem_diagnostics["journal_append_batches"] = values->journal_append_batches;
                filesystem_diagnostics["journal_records_appended"] =
                    values->journal_records_appended;
                filesystem_diagnostics["journal_durability_barriers"] =
                    values->journal_durability_barriers;
                filesystem_diagnostics["spool_bytes"] = values->spool_bytes;
                filesystem_diagnostics["spool_limit_bytes"] = values->spool_limit_bytes;
                filesystem_diagnostics["spool_publish_rate_bytes_per_second"] =
                    values->spool_publish_rate_bytes_per_second;
                filesystem_diagnostics["spool_publish_rate_window_bytes"] =
                    values->spool_publish_rate_window_bytes;
                filesystem_diagnostics["spool_publish_rate_window_ms"] =
                    values->spool_publish_rate_window_ms;
                filesystem_diagnostics["spool_throttle_waits"] =
                    values->spool_throttle_waits;
                filesystem_diagnostics["spool_throttle_wait_ms"] =
                    values->spool_throttle_wait_ms;
                filesystem_diagnostics["pending_write_request_bytes"] =
                    values->pending_write_request_bytes;
                filesystem_diagnostics["peak_pending_write_request_bytes"] =
                    values->peak_pending_write_request_bytes;
                filesystem_diagnostics["pending_write_request_limit_bytes"] =
                    values->pending_write_request_limit_bytes;
                filesystem_diagnostics["extent_executor_workers"] =
                    values->extent_executor_workers;
                filesystem_diagnostics["extent_executor_queued"] =
                    values->extent_executor_queued;
                filesystem_diagnostics["extent_executor_active"] =
                    values->extent_executor_active;
                filesystem_diagnostics["extent_executor_peak_queued"] =
                    values->extent_executor_peak_queued;
                filesystem_diagnostics["extent_executor_peak_active"] =
                    values->extent_executor_peak_active;
                filesystem_diagnostics["extent_executor_submitted"] =
                    values->extent_executor_submitted;
                filesystem_diagnostics["inode_count"] = values->inode_count;
                filesystem_diagnostics["peak_inode_count"] = values->peak_inode_count;
                filesystem_diagnostics["reclaimed_inode_count"] =
                    values->reclaimed_inode_count;
            }
        } catch (const std::exception& error) {
            Log::debug("status filesystem diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["filesystem"] = std::move(filesystem_diagnostics);

    Json::Object convergence_diagnostics;
    convergence_diagnostics["available"] = false;
    if (convergence_provider) {
        try {
            const auto values = convergence_provider();
            convergence_diagnostics["available"] = true;
            convergence_diagnostics["events_received"] = values.events_received;
            convergence_diagnostics["runs_scheduled"] = values.runs_scheduled;
            convergence_diagnostics["runs_completed"] = values.runs_completed;
            convergence_diagnostics["requested_epoch"] = values.requested_epoch;
            convergence_diagnostics["completed_epoch"] = values.completed_epoch;
            convergence_diagnostics["latest_generation"] = values.latest_generation;
            convergence_diagnostics["scheduled"] = values.scheduled;
        } catch (const std::exception& error) {
            Log::debug("status convergence diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["convergence"] = std::move(convergence_diagnostics);

    // What replica repair could not obtain. `unsourceable_objects` climbing
    // across passes is the closest thing this node has to "there are extents
    // nothing in the cluster can serve" -- after a node is removed, that is
    // the question an operator actually has, and nothing answered it before
    // 0.40.0. A single pass can also miss because a peer was busy or a budget
    // ran out, so a small non-zero figure that stops growing is ordinary.
    // `local_unreadable_objects` is the other half: objects this node's own
    // store listed and then could not read back, which is what a failing disk
    // looks like from up here.
    Json::Object repair_diagnostics;
    repair_diagnostics["available"] = false;
    if (repair_provider) {
        try {
            const auto values = repair_provider();
            repair_diagnostics["available"] = true;
            repair_diagnostics["unsourceable_objects"] = values.pull_unsourceable;
            repair_diagnostics["local_unreadable_objects"] = values.local_unreadable;
            Json::Array sample;
            for (const auto& id : values.unsourceable_sample)
                sample.push_back(to_string(id));
            repair_diagnostics["unsourceable_sample"] = std::move(sample);
        } catch (const std::exception& error) {
            Log::debug("status repair diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["repair"] = std::move(repair_diagnostics);

    // The HTTP server that is answering this very request. `reactor_stalls`
    // is the runtime half of the rule that the reactor may not call anything
    // that sleeps: a non-zero count means something did, and the longest
    // pass says for how long. Idle keep-alive connections are counted so
    // that "a client family holding connections" is visible rather than
    // inferred, which is what the 10 s Status question of 2026-09-13 lacked.
    Json::Object http_diagnostics;
    http_diagnostics["available"] = false;
    if (http_provider) {
        try {
            if (const auto values = http_provider()) {
                http_diagnostics["available"] = true;
                http_diagnostics["reactor_passes"] = values->reactor_passes;
                http_diagnostics["reactor_stalls"] = values->reactor_stalls;
                http_diagnostics["reactor_longest_pass_ms"] = values->reactor_longest_pass_ms;
                http_diagnostics["connections_open"] = values->connections_open;
                http_diagnostics["connections_idle_keep_alive"] =
                    values->connections_idle_keep_alive;
                http_diagnostics["connections_writing"] = values->connections_writing;
                http_diagnostics["connections_deferred"] = values->connections_deferred;
                http_diagnostics["connections_refused"] = values->connections_refused;
                http_diagnostics["staged_bytes"] = values->staged_bytes;
                http_diagnostics["requests_served"] = values->requests_served;
                http_diagnostics["requests_deferred"] = values->requests_deferred;
                http_diagnostics["requests_overloaded"] = values->requests_overloaded;
                http_diagnostics["slow_requests"] = values->slow_requests;
                http_diagnostics["responses_compressed"] = values->responses_compressed;
                http_diagnostics["compression_bytes_saved"] = values->compression_bytes_saved;
                const auto lane_json = [](const HttpServerDiagnostics::Lane& lane) {
                    return Json::Object{{"workers", lane.workers},
                                        {"busy", lane.busy},
                                        {"queued", lane.queued},
                                        {"peak_queued", lane.peak_queued},
                                        {"queue_wait_ms_max", lane.queue_wait_ms_max},
                                        {"handled", lane.handled}};
                };
                http_diagnostics["control_lane"] = lane_json(values->control);
                http_diagnostics["data_lane"] = lane_json(values->data);
            }
        } catch (const std::exception& error) {
            Log::debug("status http diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["http"] = std::move(http_diagnostics);

    // Auth state is local to each node and converges by gossip, so the only
    // way to see whether it actually has converged is to compare these across
    // nodes -- table_hash is stable for the same contents, the way
    // metadata_generation is.
    Json::Object auth_diagnostics;
    // An empty table is the upgrade lockout described in NodeRuntime::start.
    // Surfaced here as well as in the log, because by the time anyone looks
    // the log line has usually scrolled and Status is what they reach for.
    const auto user_count = node_.users().size();
    auth_diagnostics["users"] = static_cast<uint64_t>(user_count);
    auth_diagnostics["accounts_initialised"] = user_count > 0;
    auth_diagnostics["user_tombstones"] = static_cast<uint64_t>(node_.users().tombstones());
    auth_diagnostics["user_table_hash"] = to_string(node_.users().table_hash());
    auth_diagnostics["allow_anonymous"] = node_.config().session.allow_anonymous;
    // What an unauthenticated visitor may do lives in the anonymous account,
    // not in config, so report the account's current roles rather than a
    // setting that no longer decides anything.
    Json::Array anonymous_roles;
    if (auto anonymous = node_.users().find_by_username(anonymous_username))
        for (const auto& role : anonymous->roles)
            anonymous_roles.push_back(role);
    auth_diagnostics["anonymous_roles"] = std::move(anonymous_roles);
    diagnostics["auth"] = std::move(auth_diagnostics);

    Json::Object root;
    root["diagnostics"] = std::move(diagnostics);
    root["generated_at_unix_ms"] = unix_ms();
    return http_json(200, Json(std::move(root)).dump());
}

HttpResponse ClusterStatusService::connectivity_check(const std::optional<NodeId>& only) {
    // The cluster-wide diagnostic action also refreshes this node's public
    // endpoint discovery and explicitly runs the self/NAT-loopback probe. The
    // node-specific action remains the existing peer RPC reachability check.
    std::optional<PublicConnectivityStatus> public_status;
    if (!only)
        public_status = node_.refresh_public_connectivity(true, true);

    Json::Array results;
    bool found_requested = !only.has_value();
    for (const auto& member : node_.membership().all()) {
        if (only && member.id != *only)
            continue;
        found_requested = true;
        bool reachable = member.id == node_.node_id();
        std::string error;
        if (!reachable) {
            try {
                reachable = node_.call(member, MessageType::ping).message.type == MessageType::ok;
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        Json::Object item{{"node_id", to_string(member.id)}, {"reachable", reachable}};
        if (!error.empty())
            item["error"] = error;
        results.emplace_back(std::move(item));
    }
    if (!found_requested)
        return http_error(404, "node_not_found", "unknown cluster node");
    Json::Object root{{"results", std::move(results)}, {"checked_at_unix_ms", unix_ms()}};
    if (public_status) {
        auto connectivity = public_connectivity_json(*public_status);
        add_inbound_resolution(connectivity.asObject(), node_.inbound_resolution());
        root["connectivity"] = std::move(connectivity);
    }
    return http_json(200, Json(std::move(root)).dump());
}

HttpResponse ClusterStatusService::handle(const HttpRequest& request) {
    if (request.method == "GET" &&
        (request.path == "/api/v1/status" || request.path == "/api/v1/status/nodes"))
        return status_response();
    if (request.method == "GET" && request.path == diagnostics_path)
        return diagnostics_response();
    if (request.method == "POST" && request.path == "/api/v1/status/connectivity/check")
        return connectivity_check({});

    constexpr std::string_view prefix = "/api/v1/status/nodes/";
    if (request.path.starts_with(prefix)) {
        auto tail = std::string_view(request.path).substr(prefix.size());
        constexpr std::string_view check = "/connectivity/check";
        bool connectivity = false;
        if (tail.ends_with(check)) {
            connectivity = true;
            tail.remove_suffix(check.size());
        }
        auto id = parse_node_id(tail);
        if (!id)
            return http_error(400, "bad_node_id", "node id must be a 32-character hexadecimal id");
        if (request.method == "GET" && !connectivity)
            return status_response(*id);
        if (request.method == "POST" && connectivity)
            return connectivity_check(*id);
    }
    return http_error(404, "not_found", "status route not found");
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/status_api.hpp"
#include "service/maintenance.hpp"
#include "subsystem/subsystem_registry.hpp"

#include "fuse/fuse_mountpoint.hpp"
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

// Carried in the lightweight response so a client learns from the payload
// where the expensive half went.
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
    out.node_name = telemetry.node_name;
    return out;
}

void merge_membership(PersistedNodeStatus& out, const NodeInfo& member) {
    // Membership is the authoritative live view: missing telemetry must never hide
    // a connected node or make a voter look offline.
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
    upnp["error_code"] = status.upnp.error_code.empty() ? Json(nullptr) : Json(status.upnp.error_code);
    upnp["error"] = status.upnp.error.empty() ? Json(nullptr) : Json(status.upnp.error);

    Json::Object external_ip;
    external_ip["enabled"] = status.external_ip.enabled;
    external_ip["attempted"] = status.external_ip.attempted;
    external_ip["address"] =
        status.external_ip.address.empty() ? Json(nullptr) : Json(status.external_ip.address);
    external_ip["error_code"] =
        status.external_ip.error_code.empty() ? Json(nullptr) : Json(status.external_ip.error_code);
    external_ip["error"] =
        status.external_ip.error.empty() ? Json(nullptr) : Json(status.external_ip.error);

    Json::Object check;
    check["enabled"] = status.check_enabled;
    check["self_probe"] = status.self_probe;
    check["error"] =
        status.self_probe_error.empty() ? Json(nullptr) : Json(status.self_probe_error);
    check["checked_at_unix_ms"] = status.checked_unix_ms;
    // A same-node TCP connect is a NAT loopback diagnostic, not proof that an
    // arbitrary Internet host can reach the advertised endpoint.
    check["externally_verified"] = false;

    Json::Object out;
    out["configured"] = endpoint_json(status.configured);
    out["advertised"] = endpoint_json(status.advertised, status.advertised_source);
    out["upnp"] = std::move(upnp);
    out["external_ip"] = std::move(external_ip);
    out["check"] = std::move(check);
    return Json(std::move(out));
}

// network.inbound_capable / storage.hosts_extents as this node resolves them,
// beside the configured modes and the dial-back evidence.
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

// A stale live sample (older than the caller's freshness window) counts as no
// sample for numeric purposes: presenting old figures as current would
// fabricate data. "authoritative" means a fresh measurement exists.
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
    // A fresh sample is authoritative only once its sender reports "ready": a
    // recovering node publishes fresh zero capacity/usage, indistinguishable from
    // a real empty node.
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
    // The gossiped self-declarations. Membership is the only source; null when
    // the node is known only from durable telemetry.
    if (member) {
        node["inbound_capable"] = node_inbound_capable(*member);
        node["hosts_extents"] = node_hosts_extents(*member);
        // The host/port above are a routing key for a node that cannot be dialled.
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
    // The node's own reported startup phase, in the vocabulary of
    // root.startup.phase. Only fresh telemetry from a ready, recovering or starting
    // sender answers; otherwise "unknown".
    node["phase"] = (live && !stale) ? std::string(node_phase_name(live->phase)) : "unknown";
    node["observed_at_unix_ms"] =
        live ? live->observed_unix_ms
             : (telemetry_known ? durable.observed_unix_ms
                                : (member ? member->seen_unix_ms : durable.observed_unix_ms));
    node["live_age_ms"] = live ? Json(live_age_ms) : Json(nullptr);
    node["version"] = live ? live->version : durable.version;
    // The operator's display name for the node; null when it has none.
    {
        const auto& name = live ? live->node_name : durable.node_name;
        node["node_name"] = name.empty() ? Json(nullptr) : Json(name);
    }
    node["host"] = member ? member->host : (live ? live->host : durable.host);
    node["port"] =
        static_cast<uint64_t>(member ? member->port : (live ? live->port : durable.port));
    node["failure_domain"] =
        member ? member->failure_domain : (live ? live->failure_domain : durable.failure_domain);
    // Membership and telemetry independently sight the same monotonic counter and
    // either may lag (membership carries 0 before a peer's first generation notice;
    // a stale sample trails), so the larger is the fresher answer. The durable
    // last-known value answers only when neither has one.
    const uint64_t observed_generation =
        std::max(member ? member->metadata_generation : uint64_t{0},
                 live ? live->metadata_generation : uint64_t{0});
    node["metadata_generation"] =
        observed_generation ? observed_generation : durable.metadata_generation;

    // The HTTP API endpoint, with scheme; distinct from host/port above, the RPC
    // address. Omitted when unknown, so API-less peers are not guessed at.
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
    {
        auto cache = telemetry_available
                         ? bytes_pair(effective.cache_used, effective.cache_capacity).asObject()
                         : unavailable_bytes().asObject();
        // What the cache has done, beside how full it is: `used` depends on writes
        // alone, so it cannot show a cache that never returns a byte. Monotonic and
        // diffed by the consumer. Not summed into the cluster rollup, where healthy
        // nodes would mask a storage-less edge node at zero. From the live sample only:
        // a durable value republished after a restart would make a diff go backwards.
        if (live && !stale) {
            cache["hits"] = static_cast<uint64_t>(live->cache_hits);
            cache["misses"] = static_cast<uint64_t>(live->cache_misses);
            cache["evictions"] = static_cast<uint64_t>(live->cache_evictions);
        }
        node["cache"] = Json(std::move(cache));
    }
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

    // Measurements of the sending process at a stated instant; consumers read
    // their age from `telemetry_freshness` and `live_age_ms`. A stale sample keeps
    // them, so a busy or distant node stays visible, and nothing aggregates them
    // across nodes.
    Json::Object runtime;
    if (live && online) {
        runtime["uptime_ms"] = live->uptime_ms;
        runtime["rss_bytes"] = live->rss_bytes;
        runtime["process_cpu_percent"] =
            static_cast<double>(live->process_cpu_milli_percent) / 1000.0;
        runtime["load1"] = static_cast<double>(live->load1_milli) / 1000.0;
        // Only when known. load1 and process_cpu_percent are per-core, so consumers
        // need this to compare non-uniform nodes; absence is safer than a zero divisor.
        if (live->cpu_cores)
            runtime["cpu_cores"] = static_cast<uint64_t>(live->cpu_cores);
        // Physical RAM, only when known; rss_bytes above is this process's resident set.
        if (live->memory_total_bytes)
            runtime["memory_total_bytes"] = live->memory_total_bytes;
        runtime["peers_known"] = static_cast<uint64_t>(live->peers_known);
        runtime["peers_active"] = static_cast<uint64_t>(live->peers_active);
        runtime["rpc_connections_created"] = live->rpc_connections_created;
        runtime["rpc_connections_reused"] = live->rpc_connections_reused;
        runtime["rpc_connections_canonical"] = live->rpc_connections_canonical;
    }
    node["runtime"] = std::move(runtime);

    // The node's own inter-node Macha traffic by frame class: totals since start
    // and the rate over its last telemetry interval (window_ms, at the sample's
    // time). Null when not reported (stale sample); a rate is null on a node's
    // first sample after start.
    if (live && online && !live->traffic.empty()) {
        const bool rated = live->traffic_window_ms > 0;
        Json::Array classes;
        for (const auto& entry : live->traffic) {
            const char* name = nullptr;
            switch (static_cast<FrameType>(entry.frame_class)) {
            case FrameType::control: name = "control"; break;
            case FrameType::foreground: name = "foreground"; break;
            case FrameType::read_ahead: name = "read_ahead"; break;
            case FrameType::speculative: name = "speculative"; break;
            case FrameType::loader: name = "loader"; break;
            }
            if (!name)
                continue;
            classes.push_back(Json(Json::Object{
                {"class", name},
                {"in_bytes", entry.in_bytes},
                {"out_bytes", entry.out_bytes},
                {"in_bytes_per_s",
                 rated ? Json(static_cast<uint64_t>(entry.in_bytes_per_s)) : Json(nullptr)},
                {"out_bytes_per_s",
                 rated ? Json(static_cast<uint64_t>(entry.out_bytes_per_s)) : Json(nullptr)}}));
        }
        node["traffic"] = Json(Json::Object{
            {"as_of_unix_ms", live->observed_unix_ms},
            {"window_ms", rated ? Json(static_cast<uint64_t>(live->traffic_window_ms))
                                : Json(nullptr)},
            {"classes", Json(std::move(classes))}});
    } else {
        node["traffic"] = nullptr;
    }

    // Configuration this node enforces, not a measurement, hence separate from
    // `runtime`. Carried for every node so a client can judge failover targets
    // without N calls. Omitted when not reported (e.g. streaming disabled): read
    // absence as "cannot say" and use a conservative bound, never another node's
    // figure.
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
        // The limit only: the current count is too perishable for a cached payload.
        // It appears where computed live: the creation payload, the collection
        // listing, and the refusal.
        if (live->playback_max_sessions_per_account)
            playback["max_sessions_per_account"] =
                static_cast<uint64_t>(live->playback_max_sessions_per_account);
        if (live->playback_max_transcodes_per_account)
            playback["max_transcodes_per_account"] =
                static_cast<uint64_t>(live->playback_max_transcodes_per_account);
        // The node-wide cap beside the per-account one, so a client can tell "another
        // screen on this account" from "this node is full".
        if (live->playback_max_sessions)
            playback["max_sessions"] = static_cast<uint64_t>(live->playback_max_sessions);
        // How long this node lets a session hold a transcode entitlement with
        // no stream activity. A client pausing for longer must refresh it --
        // a playlist fetch is enough -- or reacquire on resume and risk a 429.
        if (live->playback_transcode_entitlement_idle_ms)
            playback["transcode_entitlement_idle_ms"] =
                static_cast<uint64_t>(live->playback_transcode_entitlement_idle_ms);
        // start=async: present only on a node that offers it.
        if (live->playback_startup_no_progress_ms)
            playback["startup_no_progress_ms"] =
                static_cast<uint64_t>(live->playback_startup_no_progress_ms);
        if (live->playback_start_wait_max_ms)
            playback["start_wait_max_ms"] = static_cast<uint64_t>(live->playback_start_wait_max_ms);
        if (live->playback_start_failed_retention_ms)
            playback["start_failed_retention_ms"] =
                static_cast<uint64_t>(live->playback_start_failed_retention_ms);
        // Sustained transcode rate per source kind this node has transcoded; unseen
        // kinds are absent.
        if (!live->playback_transcode_rates.empty()) {
            Json::Array rates;
            for (const auto& rate : live->playback_transcode_rates) {
                Json::Object entry{{"kind", rate.kind},
                                   {"codec", rate.codec},
                                   {"rate", static_cast<double>(rate.rate_milli) / 1000.0},
                                   {"observations", static_cast<uint64_t>(rate.observations)},
                                   {"concurrent", static_cast<uint64_t>(rate.concurrent)}};
                if (rate.kind == "video") {
                    entry["bit_depth"] = static_cast<uint64_t>(rate.bit_depth);
                    entry["height_class"] = static_cast<uint64_t>(rate.height_class);
                }
                rates.push_back(Json(std::move(entry)));
            }
            playback["transcode_rates"] = std::move(rates);
        }
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

ClusterStatusService::ClusterStatusService(NodeRuntime& node, Accounts& accounts,
                                           const ActivityClocks& activity,
                                           const DataResourceArbiter& data_resources,
                                           const RetainedMemoryLedger& retained_memory)
    : node_(node), accounts_(accounts), activity_(activity), data_resources_(data_resources),
      retained_memory_(retained_memory) {}

ClusterStatusService::~ClusterStatusService() {
    stop();
}

void ClusterStatusService::start() {
    if (persistence_.joinable())
        return;
    persistence_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("status-persistence", stop, [this, stop] { persistence_loop(stop); });
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
    // Status persistence stays outside namespace metadata: an observational
    // checkpoint must never serialise the namespace, take the metadata mutation
    // lock or enter publication CAS. The small durable write (with its fsync) is
    // deferred while viewer-critical work is active.
    constexpr auto idle_before_persist = std::chrono::seconds(30);
    if (activity_.idle_for(FrameType::foreground) < idle_before_persist ||
        activity_.idle_for(FrameType::read_ahead) < idle_before_persist)
        return;
    node_.telemetry().persist();
    accounts_.sessions().persist();
}

void ClusterStatusService::persistence_loop(std::stop_token stop) {
    // After a short startup head start, keep one coalesced durable observation
    // per node. A failed checkpoint is retried; no telemetry backlog is replayed.
    auto delay = std::chrono::seconds(10);
    while (!stop.stop_requested()) {
        Lock lock(wait_mutex_);
        wait_cv_.wait_for(lock.native(), stop, delay, [] { return false; });
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

HttpResponse ClusterStatusService::status_response(const StatusSources& sources,
                                                   const std::optional<NodeId>& only) {
    auto* metadata_manager = sources.metadata;
    std::shared_ptr<const MetadataSnapshot> metadata;
    uint64_t metadata_generation = 0;
    if (metadata_manager) {
        try {
            if (auto available = metadata_manager->current()) {
                metadata = available->snapshot;
                metadata_generation = available->generation;
            }
        } catch (const std::exception& error) {
            Log::debug("status metadata unavailable: " + std::string(error.what()));
        }
    }

    // Membership is independent of telemetry: small in-memory snapshots under
    // Membership's short mutex, no I/O. Telemetry only decorates these members.
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
        known = metadata->node_status; // Read-only: older checkpoints carry it.
    for (const auto& telemetry : node_.telemetry().persisted())
        known[telemetry.node_id] = persisted(telemetry);
    // Only a fresh and ready observation becomes the new last-known baseline:
    // otherwise stale figures could never fall back, and a recovering node's
    // zero capacity/usage would overwrite its last known-good figures.
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
    // Mirrors the fresh-and-ready gate above: "known" means a trustworthy
    // last-known source exists, not merely that telemetry was once seen.
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
        // A reset retires the pre-reset durable identity only as a freshness
        // boundary: a later directly authenticated sighting is an ordinary node.
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
        // A node hosting no extents has no durable capacity, so it must not make the
        // aggregate look short.
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
            // Self's readiness is reported exactly via `readiness` above, and its own
            // telemetry can lag it; this signal surfaces remote peers' recovery only.
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
        metadata_manager ? metadata_manager->status() : MetadataClusterStatus{};
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
    // Nodes accepting no inbound connections, and the effect on where extents can
    // live. The first is information; the other two are faults.
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
    // Compatibility aliases carrying the current values; not a fixed voter set or
    // majority quorum.
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
    startup["error_code"] = readiness.error.empty() ? Json(nullptr) : Json("recovery_failed");
    startup["error"] = readiness.error.empty() ? Json(nullptr) : Json(readiness.error);

    Json::Object root;
    // Which node produced this response, matching an `id` in nodes[]. A client
    // joins on it to attach node identity to the address it reached:
    // api_endpoint is the advertised name, which differs from that address (LAN
    // IP vs DNS name) exactly when it matters, and guessing would merge distinct
    // nodes.
    root["node_id"] = to_string(node_.node_id());
    root["cluster"] = std::move(cluster);
    root["startup"] = std::move(startup);
    root["nodes"] = std::move(nodes);
    {
        auto connectivity = public_connectivity_json(node_.public_connectivity_status());
        add_inbound_resolution(connectivity.asObject(), local_resolution);
        root["connectivity"] = std::move(connectivity);
    }

    Json::Array subsystems;
    if (sources.subsystems) {
        for (const auto& status : sources.subsystems->statuses()) {
            Json::Object entry;
            entry["name"] = status.name;
            entry["state"] = std::string(subsystem_state_name(status.state));
            entry["restart_count"] = static_cast<uint64_t>(status.restart_count);
            entry["last_fault"] = status.last_fault.empty() ? Json(nullptr) : Json(status.last_fault);
            subsystems.push_back(std::move(entry));
        }
    }
    root["subsystems"] = std::move(subsystems);
    // Every supervised thread by name; on a fault each is restarted, escalated or
    // ended according to its kind (supervised.hpp).
    Json::Array threads;
    for (const auto& status : supervised_thread_statuses()) {
        Json::Object entry;
        entry["name"] = status.name;
        entry["running"] = static_cast<uint64_t>(status.running);
        entry["restarting"] = static_cast<uint64_t>(status.restarting);
        entry["faults"] = status.faults;
        entry["last_fault_code"] =
            status.last_fault_code.empty() ? Json(nullptr) : Json(status.last_fault_code);
        entry["last_fault"] = status.last_fault.empty() ? Json(nullptr) : Json(status.last_fault);
        entry["last_fault_unix_ms"] =
            status.last_fault_unix_ms ? Json(status.last_fault_unix_ms) : Json(nullptr);
        threads.push_back(std::move(entry));
    }
    root["threads"] = std::move(threads);
    // Named so a client that finds `diagnostics` absent is told where it went.
    root["diagnostics_endpoint"] = std::string(diagnostics_path);
    root["generated_at_unix_ms"] = unix_ms();
    return http_json(200, Json(std::move(root)).dump());
}

// Everything above is state the node already holds decoded: short mutexes, no
// I/O, no network. What follows touches most subsystems' locks, so it has its
// own route (see status_api.hpp).
HttpResponse ClusterStatusService::diagnostics_response(const StatusSources& sources) {
    auto* metadata_manager = sources.metadata;
    std::shared_ptr<const MetadataSnapshot> metadata;
    if (metadata_manager) {
        try {
            if (auto available = metadata_manager->current())
                metadata = available->snapshot;
        } catch (const std::exception& error) {
            Log::debug("status metadata unavailable: " + std::string(error.what()));
        }
    }
    const auto readiness = node_.readiness();

    // Process-lifetime aggregates read from local atomics: no sampling loop,
    // persistence or gossip.
    Json::Object diagnostics;
    Json::Object metadata_diagnostics;
    metadata_diagnostics["available"] = sources.local != nullptr;
    if (sources.local) {
        const auto values = sources.local->replica().diagnostics();
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
    // Standing conflicts, listed and resolved under
    // /api/v1/manage/metadata/conflicts; superseded/resolved are process-lifetime
    // counters of conflicts that left the snapshot.
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

    const auto data_resource = data_resources_.stats();
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
    // Measured device service time and whether it is being defended, so a
    // throttled loader is distinguishable from an unwell node. `device_worst_us`
    // shows the single slow write a healthy mean hides.
    data_resource_diagnostics["device_pressured"] = data_resource.device_pressured;
    data_resource_diagnostics["device_service_us"] = data_resource.device_service_us;
    data_resource_diagnostics["device_worst_us"] = data_resource.device_worst_us;
    data_resource_diagnostics["device_slowdown_percent"] = data_resource.device_slowdown_percent;
    data_resource_diagnostics["device_pressure_onsets"] = data_resource.device_pressure_onsets;
    // Onsets say the device went under; this says what that cost.
    data_resource_diagnostics["pressure_refusals"] = data_resource.pressure_refusals;
    data_resource_diagnostics["cancelled_waits"] = data_resource.cancelled_waits;
    diagnostics["data_resources"] = std::move(data_resource_diagnostics);

    const auto retained_memory = retained_memory_.stats();
    Json::Object retained_memory_diagnostics;
    retained_memory_diagnostics["capacity_bytes"] = retained_memory.capacity_bytes;
    retained_memory_diagnostics["control_reserve_bytes"] =
        retained_memory.control_reserve_bytes;
    retained_memory_diagnostics["viewer_reserve_bytes"] =
        retained_memory.viewer_reserve_bytes;
    retained_memory_diagnostics["loader_reserve_bytes"] =
        retained_memory.loader_reserve_bytes;
    // The slice only inbound frame reassembly may draw on: its exhaustion turns a
    // stuck publication into dropped peer channels.
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
    if (sources.local) {
        try {
            const auto data_store = sources.local->data().diagnostics();
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

    Json::Object filesystem_diagnostics;
    filesystem_diagnostics["available"] = false;
    // Held for the call, so a subsystem restart cannot pull the frontend away.
    const auto fuse = sources.registry ? sources.registry->fuse() : nullptr;
    if (fuse) {
        try {
            const auto values = fuse->diagnostics();
            filesystem_diagnostics["available"] = true;
            filesystem_diagnostics["timed_out_requests"] = values.timed_out_requests;
            filesystem_diagnostics["merged_publications"] = values.merged_publications;
            filesystem_diagnostics["data_publication_requests"] =
                values.data_publication_requests;
            filesystem_diagnostics["data_publication_notifications_suppressed"] =
                values.data_publication_notifications_suppressed;
            filesystem_diagnostics["spool_pressure_publication_sweeps"] =
                values.spool_pressure_publication_sweeps;
            filesystem_diagnostics["data_publication_coalesced_queued"] =
                values.data_publication_coalesced_queued;
            filesystem_diagnostics["data_publication_coalesced_running"] =
                values.data_publication_coalesced_running;
            filesystem_diagnostics["data_publication_coalesced_unconfirmed"] =
                values.data_publication_coalesced_unconfirmed;
            filesystem_diagnostics["data_publications_started"] =
                values.data_publications_started;
            filesystem_diagnostics["data_publications_completed"] =
                values.data_publications_completed;
            filesystem_diagnostics["data_publication_peak_active"] =
                values.data_publication_peak_active;
            filesystem_diagnostics["data_publication_quanta"] =
                values.data_publication_quanta;
            filesystem_diagnostics["data_publication_yields"] =
                values.data_publication_yields;
            filesystem_diagnostics["data_publication_peak_inflight_bytes"] =
                values.data_publication_peak_inflight_bytes;
            filesystem_diagnostics["data_publication_pipeline_limit_bytes"] =
                values.data_publication_pipeline_limit_bytes;
            filesystem_diagnostics["data_publication_peak_pipeline_extents"] =
                values.data_publication_peak_pipeline_extents;
            filesystem_diagnostics["data_closed_priority_selections"] =
                values.data_closed_priority_selections;
            filesystem_diagnostics["data_retirement_priority_selections"] =
                values.data_retirement_priority_selections;
            filesystem_diagnostics["open_publications"] = values.open_publications;
            filesystem_diagnostics["peak_open_publications"] =
                values.peak_open_publications;
            filesystem_diagnostics["publication_max_open_writers"] =
                values.publication_max_open_writers;
            filesystem_diagnostics["data_publication_selections_under_writer_cap"] =
                values.data_publication_selections_under_writer_cap;
            filesystem_diagnostics["data_publication_progress_events"] =
                values.data_publication_progress_events;
            filesystem_diagnostics["data_publication_bytes_read"] =
                values.data_publication_bytes_read;
            filesystem_diagnostics["data_publication_bytes_committed"] =
                values.data_publication_bytes_committed;
            filesystem_diagnostics["data_publication_bytes_confirmed"] =
                values.data_publication_bytes_confirmed;
            filesystem_diagnostics["data_publication_completed_spool_bytes_read"] =
                values.data_publication_completed_spool_bytes_read;
            filesystem_diagnostics["data_publication_completed_source_bytes_read"] =
                values.data_publication_completed_source_bytes_read;
            filesystem_diagnostics["data_publication_completed_reused_extents"] =
                values.data_publication_completed_reused_extents;
            filesystem_diagnostics["data_publication_completed_put_extents"] =
                values.data_publication_completed_put_extents;
            filesystem_diagnostics["data_overlay_read_queries"] =
                values.data_overlay_read_queries;
            filesystem_diagnostics["data_overlay_ranges_examined"] =
                values.data_overlay_ranges_examined;
            filesystem_diagnostics["data_overlay_descriptors_copied"] =
                values.data_overlay_descriptors_copied;
            filesystem_diagnostics["retained_data_operations"] =
                values.retained_data_operations;
            filesystem_diagnostics["retained_data_operation_bytes"] =
                values.retained_data_operation_bytes;
            filesystem_diagnostics["retained_overlay_ranges"] =
                values.retained_overlay_ranges;
            filesystem_diagnostics["retained_overlay_bytes"] =
                values.retained_overlay_bytes;
            filesystem_diagnostics["retained_publication_operations"] =
                values.retained_publication_operations;
            filesystem_diagnostics["retained_publication_operation_bytes"] =
                values.retained_publication_operation_bytes;
            filesystem_diagnostics["operation_metadata_bytes"] =
                values.operation_metadata_bytes;
            filesystem_diagnostics["peak_operation_metadata_bytes"] =
                values.peak_operation_metadata_bytes;
            filesystem_diagnostics["operation_metadata_limit_bytes"] =
                values.operation_metadata_limit_bytes;
            filesystem_diagnostics["operation_metadata_waits"] =
                values.operation_metadata_waits;
            filesystem_diagnostics["retained_durability_tickets"] =
                values.retained_durability_tickets;
            filesystem_diagnostics["data_publication_inflight_bytes"] =
                values.data_publication_inflight_bytes;
            filesystem_diagnostics["backend_failures"] = values.backend_failures;
            filesystem_diagnostics["durability_batches"] = values.durability_batches;
            filesystem_diagnostics["durability_writes"] = values.durability_writes;
            filesystem_diagnostics["namespace_operations_admitted"] =
                values.namespace_operations_admitted;
            filesystem_diagnostics["namespace_operations_recovered"] =
                values.namespace_operations_recovered;
            filesystem_diagnostics["namespace_publication_attempts"] =
                values.namespace_publication_attempts;
            filesystem_diagnostics["namespace_publication_batches"] =
                values.namespace_publication_batches;
            filesystem_diagnostics["namespace_operations_batched"] =
                values.namespace_operations_batched;
            filesystem_diagnostics["namespace_operations_published"] =
                values.namespace_operations_published;
            filesystem_diagnostics["namespace_operations_confirmed"] =
                values.namespace_operations_confirmed;
            // The mount is stale exactly when refreshed < available. Both
            // are exposed so an operator can see it without a rebuild.
            filesystem_diagnostics["namespace_refreshed_revision"] =
                values.namespace_refreshed_revision;
            filesystem_diagnostics["namespace_available_revision"] =
                values.namespace_available_revision;
            // Files whose publication exhausted its retry budget; details
            // and actions under /api/v1/manage/filesystem/parked-publications.
            filesystem_diagnostics["parked_publications"] = values.parked_publications;
            filesystem_diagnostics["publication_retries_backed_off"] =
                values.publication_retries_backed_off;
            filesystem_diagnostics["publications_retrying_persistently"] =
                values.publications_retrying_persistently;
            // What recovery resolved instead of refusing.
            filesystem_diagnostics["journal_recovery_skipped_frames"] =
                values.journal_recovery_skipped_frames;
            filesystem_diagnostics["journal_recovery_quarantined_bytes"] =
                values.journal_recovery_quarantined_bytes;
            filesystem_diagnostics["recovery_dropped_operations"] =
                values.recovery_dropped_operations;
            filesystem_diagnostics["publications_abandoned"] =
                values.publications_abandoned;
            // The host directory under the mount: entries found there at
            // startup are hidden by the mount, and whether the directory
            // is immutable while no mount covers it.
            filesystem_diagnostics["mountpoint_stray_entries"] =
                fuse_mountpoint_preparation().stray_entries;
            filesystem_diagnostics["mountpoint_immutable"] =
                fuse_mountpoint_preparation().immutable;
            filesystem_diagnostics["journal_append_batches"] = values.journal_append_batches;
            filesystem_diagnostics["journal_records_appended"] =
                values.journal_records_appended;
            filesystem_diagnostics["journal_durability_barriers"] =
                values.journal_durability_barriers;
            filesystem_diagnostics["spool_bytes"] = values.spool_bytes;
            filesystem_diagnostics["spool_limit_bytes"] = values.spool_limit_bytes;
            filesystem_diagnostics["spool_publish_rate_bytes_per_second"] =
                values.spool_publish_rate_bytes_per_second;
            filesystem_diagnostics["spool_publish_rate_window_bytes"] =
                values.spool_publish_rate_window_bytes;
            filesystem_diagnostics["spool_publish_rate_window_ms"] =
                values.spool_publish_rate_window_ms;
            filesystem_diagnostics["spool_throttle_waits"] =
                values.spool_throttle_waits;
            filesystem_diagnostics["spool_throttle_wait_ms"] =
                values.spool_throttle_wait_ms;
            filesystem_diagnostics["pending_write_request_bytes"] =
                values.pending_write_request_bytes;
            filesystem_diagnostics["peak_pending_write_request_bytes"] =
                values.peak_pending_write_request_bytes;
            filesystem_diagnostics["pending_write_request_limit_bytes"] =
                values.pending_write_request_limit_bytes;
            filesystem_diagnostics["extent_executor_workers"] =
                values.extent_executor_workers;
            filesystem_diagnostics["extent_executor_queued"] =
                values.extent_executor_queued;
            filesystem_diagnostics["extent_executor_active"] =
                values.extent_executor_active;
            filesystem_diagnostics["extent_executor_peak_queued"] =
                values.extent_executor_peak_queued;
            filesystem_diagnostics["extent_executor_peak_active"] =
                values.extent_executor_peak_active;
            filesystem_diagnostics["extent_executor_submitted"] =
                values.extent_executor_submitted;
            filesystem_diagnostics["inode_count"] = values.inode_count;
            filesystem_diagnostics["peak_inode_count"] = values.peak_inode_count;
            filesystem_diagnostics["reclaimed_inode_count"] =
                values.reclaimed_inode_count;
        } catch (const std::exception& error) {
            Log::debug("status filesystem diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["filesystem"] = std::move(filesystem_diagnostics);

    Json::Object convergence_diagnostics;
    convergence_diagnostics["available"] = false;
    if (sources.maintenance) {
        try {
            const auto values = sources.maintenance->metadata_convergence.diagnostics();
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

    // What replica repair could not obtain. `unsourceable_objects` climbing across
    // passes means extents nothing in the cluster can serve; a small figure that
    // stops growing is ordinary (a busy peer, an exhausted budget).
    // `local_unreadable_objects` counts objects this store listed but could not
    // read back: a failing disk.
    Json::Object repair_diagnostics;
    repair_diagnostics["available"] = false;
    if (sources.store) {
        try {
            const auto values = sources.store->repair_diagnostics();
            repair_diagnostics["available"] = true;
            repair_diagnostics["unsourceable_objects"] = values.pull_unsourceable;
            repair_diagnostics["local_unreadable_objects"] = values.local_unreadable;
            Json::Array sample;
            for (const auto& id : values.unsourceable_sample)
                sample.push_back(to_string(id));
            repair_diagnostics["unsourceable_sample"] = std::move(sample);
            // Is repair moving, and where is it? push walks this node's store,
            // then pull walks the live set; only the pull finds and counts
            // what this node lacks.
            repair_diagnostics["push_examined"] = values.push_examined;
            repair_diagnostics["pull_examined"] = values.pull_examined;
            repair_diagnostics["bytes_transferred"] = values.bytes_transferred;
            repair_diagnostics["passes_completed"] = values.passes_completed;
            repair_diagnostics["push_phase_complete"] = values.push_phase_complete;
            Json::Object gates;
            gates["ran"] = values.gate_ran;
            gates["share"] = values.gate_share;
            gates["quiescent"] = values.gate_quiescent;
            gates["credit"] = values.gate_credit;
            repair_diagnostics["pass_gates"] = std::move(gates);
            repair_diagnostics["last_credit_bytes"] = values.last_credit_bytes;
            // Repair under a higher class takes turns on a weighted share,
            // so one pass's gate does not say whether it is being held back:
            // the classes active at the latest pass do.
            Json::Array paced_by;
            if (values.last_gate) {
                if (values.paced_by & DistributedStore::paced_by_playback)
                    paced_by.push_back("playback");
                if (values.paced_by & DistributedStore::paced_by_mounted_filesystem)
                    paced_by.push_back("mounted_filesystem");
                if (values.paced_by & DistributedStore::paced_by_loader)
                    paced_by.push_back("loader");
                if (values.paced_by & DistributedStore::paced_by_peer_playback)
                    paced_by.push_back("peer_playback");
            }
            const char* pace = "unknown";
            if (!paced_by.empty()) {
                pace = "paced";
            } else if (values.last_gate) {
                switch (*values.last_gate) {
                case DistributedStore::RepairGate::ran:
                case DistributedStore::RepairGate::share: pace = "running"; break;
                case DistributedStore::RepairGate::quiescent: pace = "settling"; break;
                case DistributedStore::RepairGate::credit: pace = "awaiting_credit"; break;
                }
            }
            repair_diagnostics["pace"] = pace;
            repair_diagnostics["paced_by"] = std::move(paced_by);
            // The quick second copy of each new object.
            Json::Object prompt;
            prompt["queued"] = values.prompt_queued;
            prompt["copies"] = values.prompt_copies;
            prompt["failures"] = values.prompt_failures;
            prompt["skipped_no_room"] = values.prompt_skipped_no_room;
            prompt["dropped"] = values.prompt_dropped;
            diagnostics["prompt_replication"] = std::move(prompt);
        } catch (const std::exception& error) {
            Log::debug("status repair diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["repair"] = std::move(repair_diagnostics);

    // The HTTP server answering this request. `reactor_stalls` counts reactor
    // passes that slept (the reactor must not), with the longest pass. Idle
    // keep-alive connections are counted so connection-holding clients are visible.
    Json::Object http_diagnostics;
    http_diagnostics["available"] = false;
    if (sources.http) {
        try {
            const auto values = sources.http->diagnostics();
            http_diagnostics["available"] = true;
            http_diagnostics["reactor_passes"] = values.reactor_passes;
            http_diagnostics["reactor_stalls"] = values.reactor_stalls;
            http_diagnostics["reactor_longest_pass_ms"] = values.reactor_longest_pass_ms;
            http_diagnostics["connections_open"] = values.connections_open;
            http_diagnostics["connections_idle_keep_alive"] =
                values.connections_idle_keep_alive;
            http_diagnostics["connections_writing"] = values.connections_writing;
            http_diagnostics["connections_deferred"] = values.connections_deferred;
            http_diagnostics["connections_refused"] = values.connections_refused;
            http_diagnostics["staged_bytes"] = values.staged_bytes;
            http_diagnostics["requests_served"] = values.requests_served;
            http_diagnostics["requests_deferred"] = values.requests_deferred;
            http_diagnostics["requests_overloaded"] = values.requests_overloaded;
            http_diagnostics["slow_requests"] = values.slow_requests;
            http_diagnostics["responses_compressed"] = values.responses_compressed;
            http_diagnostics["compression_bytes_saved"] = values.compression_bytes_saved;
            const auto lane_json = [](const HttpServerDiagnostics::Lane& lane) {
                return Json::Object{{"workers", lane.workers},
                                    {"busy", lane.busy},
                                    {"queued", lane.queued},
                                    {"peak_queued", lane.peak_queued},
                                    {"queue_wait_ms_max", lane.queue_wait_ms_max},
                                    {"handled", lane.handled}};
            };
            http_diagnostics["control_lane"] = lane_json(values.control);
            http_diagnostics["data_lane"] = lane_json(values.data);
        } catch (const std::exception& error) {
            Log::debug("status http diagnostics unavailable: " + std::string(error.what()));
        }
    }
    diagnostics["http"] = std::move(http_diagnostics);

    // Auth state is per node and converges by gossip; compare these across nodes
    // to check convergence (table_hash is stable for equal contents).
    Json::Object auth_diagnostics;
    // An empty user table means nobody can sign in; surfaced here so it is seen.
    const auto user_count = accounts_.users().size();
    auth_diagnostics["users"] = static_cast<uint64_t>(user_count);
    auth_diagnostics["accounts_initialised"] = user_count > 0;
    auth_diagnostics["user_tombstones"] = static_cast<uint64_t>(accounts_.users().tombstones());
    auth_diagnostics["user_table_hash"] = to_string(accounts_.users().table_hash());
    auth_diagnostics["allow_anonymous"] = node_.config().session.allow_anonymous;
    // What an anonymous visitor may do is the anonymous account's roles.
    Json::Array anonymous_roles;
    if (auto anonymous = accounts_.users().find_by_username(anonymous_username))
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
    // The cluster-wide action also refreshes this node's public endpoint discovery
    // and runs the self/NAT-loopback probe; the node-specific action is the peer
    // RPC reachability check.
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
        if (!error.empty()) {
            item["error_code"] = std::string("rpc_failed");
            item["error"] = error;
        }
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

HttpResponse ClusterStatusService::handle(const HttpRequest& request,
                                          const StatusSources& sources) {
    if (request.method == "GET" &&
        (request.path == "/api/v1/status" || request.path == "/api/v1/status/nodes"))
        return status_response(sources);
    if (request.method == "GET" && request.path == diagnostics_path)
        return diagnostics_response(sources);
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
            return status_response(sources, *id);
        if (request.method == "POST" && connectivity)
            return connectivity_check(*id);
    }
    return http_error(404, "not_found", "status route not found");
}

} // namespace macha

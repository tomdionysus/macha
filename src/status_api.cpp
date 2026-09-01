// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_api.hpp"

#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <map>

namespace macha {
namespace {

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

Json node_json(const NodeId& id, const PersistedNodeStatus& durable, const NodeInfo* member,
               const NodeTelemetry* live, uint64_t live_age_ms, bool online, bool stale,
               bool telemetry_known, bool metadata_replica,
               const IdentityAssociationReset* identity_reset) {
    Json::Object node;
    node["id"] = to_string(id);
    node["state"] = online ? "online" : "offline";
    node["telemetry_freshness"] =
        live ? (stale ? "stale" : "live") : (telemetry_known ? "last_known" : "unavailable");
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
    node["metadata_generation"] =
        member ? member->metadata_generation
               : (live ? live->metadata_generation : durable.metadata_generation);

    const auto storage_capacity =
        live ? live->storage_capacity
             : (telemetry_known ? durable.storage_capacity : (member ? member->capacity : 0));
    const auto storage_used = live ? live->storage_used : durable.storage_used;
    const auto cache_capacity = live ? live->cache_capacity : durable.cache_capacity;
    const auto cache_used = live ? live->cache_used : durable.cache_used;
    node["storage"] =
        live || telemetry_known
            ? bytes_pair(storage_used, storage_capacity)
            : unavailable_bytes(member ? std::optional<uint64_t>(member->capacity) : std::nullopt);
    node["cache"] =
        live || telemetry_known ? bytes_pair(cache_used, cache_capacity) : unavailable_bytes();
    node["storage_backends_online"] =
        live || telemetry_known
            ? Json(static_cast<uint64_t>(live ? live->storage_backends_online
                                              : durable.storage_backends_online))
            : Json(nullptr);

    Json::Array roles;
    if (storage_capacity)
        roles.emplace_back("storage");
    if (cache_capacity)
        roles.emplace_back("cache");
    if (metadata_replica)
        roles.emplace_back("metadata-replica");
    node["roles"] = std::move(roles);

    Json::Object runtime;
    if (live && online) {
        runtime["uptime_ms"] = live->uptime_ms;
        runtime["rss_bytes"] = live->rss_bytes;
        runtime["process_cpu_percent"] =
            static_cast<double>(live->process_cpu_milli_percent) / 1000.0;
        runtime["load1"] = static_cast<double>(live->load1_milli) / 1000.0;
        runtime["peers_known"] = static_cast<uint64_t>(live->peers_known);
        runtime["peers_active"] = static_cast<uint64_t>(live->peers_active);
        runtime["rpc_connections_created"] = live->rpc_connections_created;
        runtime["rpc_connections_reused"] = live->rpc_connections_reused;
        runtime["rpc_connections_canonical"] = live->rpc_connections_canonical;
    }
    node["runtime"] = std::move(runtime);
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

void ClusterStatusService::attach_convergence_diagnostics(
    std::function<ConvergenceDemandDiagnostics()> provider) {
    std::lock_guard lock(operational_diagnostics_mutex_);
    convergence_diagnostics_ = std::move(provider);
}

ClusterStatusService::~ClusterStatusService() {
    stop();
}

void ClusterStatusService::start() {
    if (persistence_.joinable())
        return;
    persistence_ = std::jthread([this](std::stop_token stop) { persistence_loop(stop); });
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
    for (const auto& [id, view] : live)
        known[id] = persisted(view.telemetry);
    for (const auto& member : membership_all)
        merge_membership(known[member.id], member);

    std::set<NodeId> telemetry_known;
    if (metadata)
        for (const auto& [id, _] : metadata->node_status)
            telemetry_known.insert(id);
    for (const auto& telemetry : node_.telemetry().persisted())
        telemetry_known.insert(telemetry.node_id);
    for (const auto& [id, _] : live)
        telemetry_known.insert(id);

    uint64_t known_capacity = 0, known_used = 0, online_capacity = 0, online_used = 0;
    uint64_t known_cache_capacity = 0, known_cache_used = 0, online_cache_capacity = 0,
             online_cache_used = 0;
    bool known_storage_available = true, online_storage_available = true;
    bool known_cache_available = true, online_cache_available = true;
    size_t online_nodes = 0;
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

        const bool has_telemetry = current || telemetry_known.contains(id);
        const auto storage_capacity =
            current ? current->storage_capacity
                    : (has_telemetry ? durable.storage_capacity : (member ? member->capacity : 0));
        const auto storage_used = current ? current->storage_used : durable.storage_used;
        const auto cache_capacity = current ? current->cache_capacity : durable.cache_capacity;
        const auto cache_used = current ? current->cache_used : durable.cache_used;
        known_capacity += storage_capacity;
        known_storage_available = known_storage_available && has_telemetry;
        known_cache_available = known_cache_available && has_telemetry;
        if (has_telemetry) {
            known_used += storage_used;
            known_cache_capacity += cache_capacity;
            known_cache_used += cache_used;
        }
        if (online) {
            ++online_nodes;
            online_capacity += storage_capacity;
            online_storage_available = online_storage_available && has_telemetry;
            online_cache_available = online_cache_available && has_telemetry;
            if (has_telemetry) {
                online_used += storage_used;
                online_cache_capacity += cache_capacity;
                online_cache_used += cache_used;
            }
        }
        const IdentityAssociationReset* identity_reset = nullptr;
        if (metadata) {
            const auto& host = member ? member->host : (current ? current->host : durable.host);
            const auto port = member ? member->port : (current ? current->port : durable.port);
            if (!host.empty() && port) {
                for (const auto& [_, reset] : metadata->identity_resets) {
                    if (!identity_reset_matches_endpoint(reset, host, port) ||
                        !identity_reset_matches_node(reset, id))
                        continue;
                    if (!identity_reset || reset.reset_unix_ms > identity_reset->reset_unix_ms)
                        identity_reset = &reset;
                }
            }
        }
        nodes.push_back(node_json(id, durable, member, current, age, online, stale, has_telemetry,
                                  metadata_replica, identity_reset));
    }

    if (only) {
        if (nodes.empty())
            return http_error(404, "node_not_found", "unknown cluster node");
        return http_json(200, nodes.front().dump());
    }

    const auto published_metadata =
        metadata_manager ? metadata_manager->cluster_status() : MetadataClusterStatus{};
    const size_t metadata_replicas = known.empty() ? published_metadata.replicas : known.size();
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
    std::string health = "healthy";
    Json::Array conditions;
    if (readiness.failed) {
        health = "critical";
        conditions.emplace_back("local startup recovery failed");
    } else if (!readiness.local_state_ready || !metadata_manager) {
        health = "recovering";
        conditions.emplace_back(readiness.local_state_ready ? "local services are starting"
                                                            : "local state is recovering");
    } else if (!metadata_read_available) {
        health = "critical";
        conditions.emplace_back("metadata unavailable");
    } else if (!metadata_write_available) {
        health = "degraded";
        conditions.emplace_back("metadata read-only");
    }
    if (online_nodes < known.size()) {
        if (health == "healthy")
            health = "degraded";
        conditions.emplace_back("one or more known nodes are offline");
    }
    if (online_capacity < known_capacity) {
        if (health == "healthy")
            health = "degraded";
        conditions.emplace_back("some known durable capacity is unavailable");
    }

    Json::Object cluster;
    cluster["health"] = health;
    cluster["conditions"] = std::move(conditions);
    cluster["nodes_known"] = static_cast<uint64_t>(known.size());
    cluster["nodes_online"] = static_cast<uint64_t>(online_nodes);
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
    root["connectivity"] = public_connectivity_json(node_.public_connectivity_status());

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
    diagnostics["metadata"] = std::move(metadata_diagnostics);

    const auto rpc = node_.rpc_server_work_stats();
    Json::Object rpc_diagnostics;
    rpc_diagnostics["metadata_pending_jobs"] = static_cast<uint64_t>(rpc.metadata_pending_jobs);
    rpc_diagnostics["metadata_pending_bytes"] = static_cast<uint64_t>(rpc.metadata_pending_bytes);
    rpc_diagnostics["metadata_active_jobs"] = static_cast<uint64_t>(rpc.metadata_active_jobs);
    rpc_diagnostics["metadata_rejected_jobs"] = rpc.metadata_rejected_jobs;
    Json::Object frame_timings;
    for (const auto& [frame, timing] : rpc.frame_timings)
        frame_timings[frame_type_name(frame)] = rpc_timing_json(timing);
    rpc_diagnostics["frame_timings"] = std::move(frame_timings);
    Json::Object message_timings;
    for (const auto& [message, timing] : rpc.message_timings)
        message_timings[message_type_name(message)] = rpc_timing_json(timing);
    rpc_diagnostics["message_timings"] = std::move(message_timings);
    diagnostics["rpc_server"] = std::move(rpc_diagnostics);

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
    data_resource_diagnostics["cancelled_waits"] = data_resource.cancelled_waits;
    diagnostics["data_resources"] = std::move(data_resource_diagnostics);

    std::function<std::optional<FuseFrontendDiagnostics>()> fuse_provider;
    std::function<ConvergenceDemandDiagnostics()> convergence_provider;
    {
        std::lock_guard lock(operational_diagnostics_mutex_);
        fuse_provider = fuse_diagnostics_;
        convergence_provider = convergence_diagnostics_;
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
    if (public_status)
        root["connectivity"] = public_connectivity_json(*public_status);
    return http_json(200, Json(std::move(root)).dump());
}

HttpResponse ClusterStatusService::handle(const HttpRequest& request) {
    if (request.method == "GET" &&
        (request.path == "/api/v1/status" || request.path == "/api/v1/status/nodes"))
        return status_response();
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/dannan_bridge.hpp"

#include "log.hpp"

#include <algorithm>

#include <unistd.h>

namespace macha {

std::string advertised_host(const Config& config) {
    if (!config.advertise_host.empty())
        return config.advertise_host;
    if (config.listen_host == "127.0.0.1" || config.listen_host == "::1")
        return config.listen_host;
    char hostname[256]{};
    if (!gethostname(hostname, 255) && hostname[0])
        return hostname;
    return "127.0.0.1";
}

std::string advertised_failure_domain(const Config& config, const std::string& host) {
    return config.failure_domain.empty() ? host : config.failure_domain;
}

dannan::NodeConfig dannan_node_config(const Config& config, dannan::identity::ClusterKey key) {
    dannan::NodeConfig out;
    out.state_path = config.state_path;
    out.key = key;
    out.node_name = config.node_name;
    out.listen_host = config.listen_host;
    out.advertise_host = advertised_host(config);
    out.failure_domain = advertised_failure_domain(config, out.advertise_host);
    out.port = config.port;
    for (const auto& endpoint : config.bootstrap)
        out.bootstrap.push_back(dannan::net::Endpoint{endpoint.host, endpoint.port});
    switch (config.inbound_capable) {
    case Tristate::yes:
        out.inbound = dannan::reach::Inbound::yes;
        break;
    case Tristate::no:
        out.inbound = dannan::reach::Inbound::no;
        break;
    case Tristate::automatic:
        out.inbound = dannan::reach::Inbound::automatic;
        break;
    }
    out.heartbeat = config.heartbeat;
    out.dead_after = config.dead_after;
    out.forget_after = config.maintenance.garbage_grace;
    out.telemetry_interval = config.telemetry_interval;
    out.connect_timeout = config.connect_timeout;
    out.max_frame = config.max_frame_size;
    // Each call names its own deadline and watch; none is applied node-wide.
    out.control_no_progress = std::chrono::milliseconds(0);
    out.data_no_progress = std::chrono::milliseconds(0);
    out.control_stall_notice = std::chrono::milliseconds(0);
    out.data_stall_notice = std::chrono::milliseconds(0);
    out.dial_back_min_interval = config.dial_back_probe_min_interval;
    out.reprobe_while_capable = config.inbound_reprobe_while_capable;
    out.reprobe_while_incapable = config.inbound_reprobe_while_incapable;
    out.connectivity.upnp.enabled = config.upnp.enabled;
    out.connectivity.upnp.external_port = config.upnp.external_port;
    out.connectivity.upnp.discovery_timeout = config.upnp.discovery_timeout;
    out.connectivity.upnp.lease_seconds = config.upnp.lease_seconds;
    out.connectivity.external_ip.enabled = config.external_ip.enabled;
    out.connectivity.external_ip.timeout = config.external_ip.timeout;
    out.connectivity.check.enabled = config.connectivity_check.enabled;
    out.connectivity.check.timeout = config.connectivity_check.timeout;
    // Macha's own validate is the one fit; the node is given its results and no limit.
    out.memory_bytes = config.runtime.retained_memory_bytes;
    out.control_reserve_bytes = config.runtime.control_memory_reserve_bytes;
    out.foreground_reserve_bytes = config.runtime.viewer_memory_reserve_bytes;
    out.loader_reserve_bytes = config.runtime.loader_memory_reserve_bytes;
    out.reassembly_reserve_bytes = config.runtime.reassembly_memory_reserve_bytes;
    out.memory_limit_bytes = 0;
    return out;
}

dannan::identity::ClusterKey dannan_cluster_key(const ClusterKeys& keys) {
    dannan::identity::ClusterKey out;
    out.master = keys.master;
    out.auth = out.derive("dannan/auth/1");
    const auto id = out.derive("dannan/cluster-id/1");
    std::copy_n(id.begin(), out.cluster_id.size(), out.cluster_id.begin());
    return out;
}

dannan::membership::IdentityReset dannan_identity_reset(const IdentityAssociationReset& reset) {
    dannan::membership::IdentityReset out;
    out.host = reset.host;
    out.port = reset.port;
    out.stale_node_id = dannan_id(reset.stale_node_id);
    out.epoch = reset.epoch;
    out.reset_unix_ms = reset.reset_unix_ms;
    out.reset_by = dannan_id(reset.reset_by);
    out.reason = reset.reason;
    return out;
}

IdentityAssociationReset macha_identity_reset(const dannan::membership::IdentityReset& reset) {
    IdentityAssociationReset out;
    out.host = reset.host;
    out.port = reset.port;
    out.stale_node_id = macha_id(reset.stale_node_id);
    out.epoch = reset.epoch;
    out.reset_unix_ms = reset.reset_unix_ms;
    out.reset_by = macha_id(reset.reset_by);
    out.reason = reset.reason;
    return out;
}

PublicConnectivityStatus macha_connectivity(const dannan::reach::PublicConnectivityStatus& status) {
    PublicConnectivityStatus out;
    out.configured = Endpoint{status.configured.host, status.configured.port};
    out.advertised = Endpoint{status.advertised.host, status.advertised.port};
    out.advertised_source = status.advertised_source;
    out.upnp.enabled = status.upnp.enabled;
    out.upnp.support_built = status.upnp.support_built;
    out.upnp.gateway_found = status.upnp.gateway_found;
    out.upnp.mapping_active = status.upnp.mapping_active;
    out.upnp.mapping_created = status.upnp.mapping_created;
    out.upnp.mapping_owned = status.upnp.mapping_owned;
    out.upnp.private_wan = status.upnp.private_wan;
    out.upnp.lan_address = status.upnp.lan_address;
    out.upnp.external_address = status.upnp.external_address;
    out.upnp.internal_port = status.upnp.internal_port;
    out.upnp.external_port = status.upnp.external_port;
    out.upnp.lease_seconds = status.upnp.lease_seconds;
    out.upnp.igd_status = status.upnp.igd_status;
    out.upnp.error_code = status.upnp.error_code;
    out.upnp.error = status.upnp.error;
    out.external_ip.enabled = status.external_ip.enabled;
    out.external_ip.attempted = status.external_ip.attempted;
    out.external_ip.address = status.external_ip.address;
    out.external_ip.error_code = status.external_ip.error_code;
    out.external_ip.error = status.external_ip.error;
    out.check_enabled = status.check_enabled;
    out.self_probe = status.self_probe;
    out.self_probe_error = status.self_probe_error;
    out.checked_unix_ms = status.checked_unix_ms;
    return out;
}

RpcStats macha_rpc_stats(const dannan::net::ClientStats& stats) {
    RpcStats out;
    out.connections_created = stats.connections_created;
    out.connections_reused = stats.reused;
    out.canonical_connections = stats.canonical_connections;
    return out;
}

namespace {
RpcServerWorkStats::Timing macha_timing(const dannan::net::Timing& timing) {
    RpcServerWorkStats::Timing out;
    out.requests = timing.requests;
    out.queue_wait_us_total = timing.queue_wait_us_total;
    out.queue_wait_us_max = timing.queue_wait_us_max;
    out.handler_us_total = timing.handler_us_total;
    out.handler_us_max = timing.handler_us_max;
    return out;
}
} // namespace

RpcServerWorkStats macha_work_stats(const dannan::net::WorkStats& stats) {
    RpcServerWorkStats out;
    out.metadata_pending_jobs = stats.ordered_pending_jobs;
    out.metadata_pending_bytes = stats.ordered_pending_bytes;
    out.metadata_active_jobs = stats.ordered_active_jobs;
    out.metadata_rejected_jobs = stats.ordered_rejected;
    out.fast_control_pending_bytes = stats.fast_control_pending_bytes;
    out.control_pending_bytes = stats.control_pending_bytes;
    out.data_pending_bytes = stats.data_pending_bytes;
    out.rejected_jobs = stats.rejected;
    for (const auto& [frame, timing] : stats.frame_timings)
        out.frame_timings[macha_frame(frame)] = macha_timing(timing);
    for (const auto& [type, timing] : stats.message_timings)
        out.message_timings[macha_type(type)] = macha_timing(timing);
    return out;
}

bool resolve_hosts_extents(const Config& config, bool inbound_capable) {
    switch (config.hosts_extents) {
    case Tristate::yes:
        return true;
    case Tristate::no:
        return false;
    case Tristate::automatic:
        break;
    }
    return !config.storage_backends.empty() && inbound_capable;
}

InboundResolution macha_inbound(const Config& config, const dannan::reach::InboundState& state) {
    InboundResolution out;
    out.inbound_capable_mode = config.inbound_capable;
    out.hosts_extents_mode = config.hosts_extents;
    out.inbound_capable = state.capable;
    out.hosts_extents = resolve_hosts_extents(config, state.capable);
    out.source = state.source;
    out.decided_unix_ms = state.decided_unix_ms;
    out.last_probe_unix_ms = state.last_probe_unix_ms;
    out.last_probe_peer = state.last_probe_peer;
    out.last_probe_error = state.last_probe_error;
    out.consecutive_probe_failures = state.consecutive_failures;
    return out;
}

namespace {
// The two enums have equal values, all to error.
LogLevel macha_level(dannan::status::LogLevel level) noexcept {
    return static_cast<LogLevel>(static_cast<unsigned char>(level));
}

class MachaLogSink final : public dannan::status::LogSink {
  public:
    bool enabled(dannan::status::LogLevel level) const noexcept override {
        return Log::enabled(macha_level(level));
    }
    void write(dannan::status::LogLevel level, std::string_view scope,
               std::string_view message) override {
        std::string line;
        line.reserve(scope.size() + 2 + message.size());
        if (!scope.empty())
            line.append(scope).append(": ");
        line.append(message);
        Log::emit(macha_level(level), line);
    }
};
} // namespace

std::shared_ptr<dannan::status::LogSink> macha_log_sink() {
    return std::make_shared<MachaLogSink>();
}

dannan::net::Message dannan_message(const RpcMessage& message) {
    dannan::net::Message out;
    out.type = dannan_type(message.type);
    out.payload = message.payload;
    return out;
}

RpcMessage macha_message(dannan::net::Message message) {
    return RpcMessage(macha_type(message.type), std::move(message.payload));
}

NodeInfo node_info_from(const dannan::membership::NodeRecord& record) {
    NodeInfo node;
    node.id = macha_id(record.id);
    node.host = record.host;
    node.port = record.port;
    node.failure_domain = record.failure_domain;
    node.seen_unix_ms = record.seen_unix_ms;
    node.capacity = record.advert_u64(advert::capacity).value_or(0);
    node.used = record.advert_u64(advert::used).value_or(0);
    node.metadata_generation = record.advert_u64(advert::metadata_generation).value_or(0);
    node.metadata_write_replicas_required =
        static_cast<uint32_t>(record.advert_u64(advert::metadata_write_replicas).value_or(0));
    node.flags =
        node_flags_for(record.inbound_capable(), record.advert(advert::hosts_extents).has_value());
    return node;
}

namespace {

using dannan::net::Frames;
using dannan::net::Lanes;
using dannan::net::Registration;

// Section 7.2 of the first cutover: lanes from Macha's allowed_on_lane, frames from its
// class_allowed, notifications from is_notification_message, ordered from
// metadata_mutation_request. No re-registered type sets its own no-progress or stall
// notice: every call names its own deadline.
Registration rule(std::string name, Lanes lanes, Frames frames, bool notification = false,
                  bool ordered = false) {
    Registration out;
    out.name = std::move(name);
    out.lanes = lanes;
    out.frames = frames;
    out.notification = notification;
    out.ordered = ordered;
    return out;
}

std::vector<MachaRegistration> build_registrations() {
    std::vector<MachaRegistration> out;
    const auto control = [&](MessageType type, Frames frames = Frames::any,
                             bool notification = false, bool ordered = false) {
        out.push_back(
            {type, rule(message_type_name(type), Lanes::control, frames, notification, ordered)});
    };
    const auto data = [&](MessageType type) {
        out.push_back({type, rule(message_type_name(type), Lanes::data, Frames::not_control)});
    };

    control(MessageType::have_object);
    data(MessageType::get_object);
    data(MessageType::put_object);
    control(MessageType::get_metadata);
    control(MessageType::cas_metadata);
    control(MessageType::seed_metadata);
    {
        // Read on the session as it arrives, ahead of every queue, so metadata
        // convergence never waits behind control work. Its handler never waits.
        auto notice = rule(message_type_name(MessageType::metadata_notice), Lanes::control,
                           Frames::control_only, true);
        notice.on_session = true;
        out.push_back({MessageType::metadata_notice, std::move(notice)});
    }
    control(MessageType::get_committed_metadata, Frames::control_only);
    control(MessageType::checkpoint_metadata);
    control(MessageType::delete_object);
    control(MessageType::commit_metadata);
    control(MessageType::cas_metadata_delta);
    control(MessageType::get_control_object);
    control(MessageType::put_control_object);
    control(MessageType::get_metadata_identity);
    data(MessageType::put_object_deferred);
    data(MessageType::object_durability_barrier);
    control(MessageType::get_metadata_history_entry);
    control(MessageType::has_metadata_history_entry);
    control(MessageType::put_metadata_history_entry, Frames::any, false, true);
    control(MessageType::get_metadata_heads);
    control(MessageType::put_metadata_commit, Frames::any, false, true);
    control(MessageType::accept_metadata_commit, Frames::any, false, true);
    control(MessageType::retain_objects);
    control(MessageType::get_ingest_jobs);
    control(MessageType::ingest_job_action);
    control(MessageType::get_torrent_jobs);
    control(MessageType::torrent_job_action);
    control(MessageType::session_sync, Frames::control_only, true);
    control(MessageType::have_objects);
    control(MessageType::get_metadata_history_record);
    control(MessageType::user_sync, Frames::control_only, true);
    control(MessageType::have_control_objects, Frames::control_only);
    control(MessageType::torrent_intent);
    control(MessageType::have_valid_objects);
    control(MessageType::trie_diff);
    control(MessageType::get_unmatched_hints);
    control(MessageType::submit_ingest_job);

    control(MessageType::bool_reply);
    data(MessageType::object_reply);
    control(MessageType::metadata_reply);
    control(MessageType::cas_reply);
    control(MessageType::control_object_reply);
    control(MessageType::metadata_identity_reply);
    control(MessageType::metadata_history_entry_reply);
    control(MessageType::metadata_heads_reply);
    control(MessageType::ingest_jobs_reply);
    control(MessageType::ingest_job_action_reply);
    control(MessageType::torrent_jobs_reply);
    control(MessageType::torrent_job_action_reply);
    control(MessageType::session_sync_reply, Frames::control_only);
    control(MessageType::have_objects_reply);
    control(MessageType::user_sync_reply, Frames::control_only);
    control(MessageType::have_control_objects_reply, Frames::control_only);
    control(MessageType::torrent_intent_reply);
    control(MessageType::have_valid_objects_reply);
    control(MessageType::trie_diff_reply);
    control(MessageType::unmatched_hints_reply);
    control(MessageType::ingest_submit_reply);
    return out;
}

} // namespace

const std::vector<MachaRegistration>& macha_registrations() {
    static const auto registrations = build_registrations();
    return registrations;
}

const std::vector<dannan::net::MessageType>& macha_retired_types() {
    static const std::vector<dannan::net::MessageType> retired{
        dannan::net::MessageType{549}, dannan::net::MessageType{550}, dannan::net::MessageType{560},
        dannan::net::MessageType{636}};
    return retired;
}

} // namespace macha

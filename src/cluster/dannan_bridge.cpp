// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/dannan_bridge.hpp"

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
    out.key = std::move(key);
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
    node.flags = node_flags_for(record.inbound_capable(),
                                record.advert_u64(advert::hosts_extents).value_or(0) != 0);
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
    const auto control = [&](MessageType type, Frames frames = Frames::any, bool notification = false,
                             bool ordered = false) {
        out.push_back({type, rule(message_type_name(type), Lanes::control, frames, notification, ordered)});
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

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversions between Macha's cluster vocabulary and Dannan's, and the wire rules
// Macha registers: pure functions, tested over everything they cover.
#include "cluster/dannan_bridge.hpp"
#include "cluster/telemetry_bridge.hpp"
#include "test_framework.hpp"

#include <dannan/net/routes.h>

#include <set>

using namespace macha;

namespace {

std::set<MessageType> registered_types() {
    std::set<MessageType> out;
    for (const auto& entry : macha_registrations())
        out.insert(entry.type);
    return out;
}

const MachaRegistration& registration_of(MessageType type) {
    for (const auto& entry : macha_registrations())
        if (entry.type == type)
            return entry;
    throw std::logic_error("not registered");
}

} // namespace

// Every Macha type at 512 or above is registered once, under its own name, and none of
// Dannan's core numbers is.
MACHA_TEST("dannan_bridge", test_every_macha_type_is_registered_once_under_its_name) {
    const auto types = registered_types();
    REQUIRE(types.size() == macha_registrations().size());
    for (const auto& entry : macha_registrations()) {
        REQUIRE(static_cast<uint16_t>(entry.type) >= dannan::net::first_user_type);
        REQUIRE(entry.registration.name == message_type_name(entry.type));
    }
    // 512 + every number Macha used, less the four retired.
    for (uint16_t number = 512; number < 640; ++number) {
        const auto type = static_cast<MessageType>(number);
        const bool retired = number == 549 || number == 550 || number == 560 || number == 636;
        const bool named = std::string(message_type_name(type)) != "unknown";
        REQUIRE(types.contains(type) == (named && !retired));
    }
}

// The rules section 7.2 names: five DATA-lane bulk types, three ordered metadata
// mutations, three notifications, and the metadata notice read on the session (on its
// own: Dannan refuses a notification handled on the session that is also fast control).
MACHA_TEST("dannan_bridge", test_the_registrations_carry_section_seven_two_rules) {
    std::set<MessageType> data, ordered, notifications, control_only;
    for (const auto& entry : macha_registrations()) {
        const auto& rule = entry.registration;
        if (rule.lanes == dannan::net::Lanes::data) {
            data.insert(entry.type);
            REQUIRE(rule.frames == dannan::net::Frames::not_control);
        } else {
            REQUIRE(rule.lanes == dannan::net::Lanes::control);
        }
        if (rule.ordered)
            ordered.insert(entry.type);
        if (rule.notification)
            notifications.insert(entry.type);
        if (rule.frames == dannan::net::Frames::control_only)
            control_only.insert(entry.type);
        REQUIRE(!rule.no_progress.has_value());
        REQUIRE(!rule.stall_notice.has_value());
    }
    const std::set<MessageType> want_data{
        MessageType::get_object, MessageType::put_object, MessageType::put_object_deferred,
        MessageType::object_durability_barrier, MessageType::object_reply};
    const std::set<MessageType> want_ordered{MessageType::put_metadata_history_entry,
                                             MessageType::put_metadata_commit,
                                             MessageType::accept_metadata_commit};
    const std::set<MessageType> want_notifications{
        MessageType::metadata_notice, MessageType::session_sync, MessageType::user_sync};
    const std::set<MessageType> want_control_only{
        MessageType::metadata_notice,      MessageType::get_committed_metadata,
        MessageType::session_sync,         MessageType::user_sync,
        MessageType::have_control_objects, MessageType::session_sync_reply,
        MessageType::user_sync_reply,      MessageType::have_control_objects_reply};
    REQUIRE(data == want_data);
    REQUIRE(ordered == want_ordered);
    REQUIRE(notifications == want_notifications);
    REQUIRE(control_only == want_control_only);
    const auto& notice = registration_of(MessageType::metadata_notice).registration;
    // A plain notification, as Dannan requires of one handled on the session.
    REQUIRE(notice.on_session);
    REQUIRE(!notice.fast_control);
    REQUIRE(!notice.ordered);
    for (const auto& entry : macha_registrations())
        if (entry.type != MessageType::metadata_notice) {
            REQUIRE(!entry.registration.on_session);
            REQUIRE(!entry.registration.fast_control);
        }
}

// The whole table goes into Dannan's route table, and the retired numbers are refused
// from then on.
MACHA_TEST("dannan_bridge", test_dannan_takes_the_table_and_refuses_the_retired_numbers) {
    auto table = dannan::net::RouteTable::with_dannan_types();
    for (const auto& entry : macha_registrations())
        table.add(dannan_type(entry.type), entry.registration);
    for (const auto type : macha_retired_types())
        table.retire(type);
    for (const auto type : macha_retired_types()) {
        bool refused = false;
        try {
            table.add(type, dannan::net::Registration{"reused"});
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        REQUIRE(refused);
    }
    for (const auto& entry : macha_registrations()) {
        REQUIRE(table.find(dannan_type(entry.type)) != nullptr);
        REQUIRE(table.is_ordered(dannan_type(entry.type)) == entry.registration.ordered);
    }
    // The transport's own types are Dannan's core types at the numbers Macha now uses.
    for (const auto type : {MessageType::ping, MessageType::members, MessageType::session_retire,
                            MessageType::promote_read_ahead, MessageType::promote_foreground,
                            MessageType::cancel_transfer, MessageType::telemetry,
                            MessageType::identity_resets, MessageType::dial_request,
                            MessageType::dial_back_probe, MessageType::ok, MessageType::error,
                            MessageType::members_reply, MessageType::telemetry_reply,
                            MessageType::identity_resets_reply, MessageType::dial_back_probe_reply}) {
        REQUIRE(table.is_core(dannan_type(type)));
        REQUIRE(table.find(dannan_type(type)) != nullptr);
    }
}

// A node record reads as Macha's NodeInfo: its place from the record, the figures from
// Macha's adverts, zero for an advert absent, the flags from the capability bit and the
// hosts_extents advert.
MACHA_TEST("dannan_bridge", test_a_node_record_reads_as_node_info) {
    dannan::membership::NodeRecord record;
    record.id.bytes.fill(7);
    record.host = "inverbeg.macha.network";
    record.port = 7437;
    record.failure_domain = "site-b";
    record.seen_unix_ms = 1794345600000ULL;
    const auto bare = node_info_from(record);
    REQUIRE(bare.id.bytes == record.id.bytes);
    REQUIRE(bare.host == record.host);
    REQUIRE(bare.port == 7437);
    REQUIRE(bare.failure_domain == "site-b");
    REQUIRE(bare.seen_unix_ms == record.seen_unix_ms);
    REQUIRE(bare.capacity == 0);
    REQUIRE(bare.used == 0);
    REQUIRE(bare.metadata_generation == 0);
    REQUIRE(bare.metadata_write_replicas_required == 0);
    REQUIRE(node_inbound_capable(bare));
    REQUIRE(!node_hosts_extents(bare));

    record.adverts[advert::capacity] = dannan::membership::u64_advert(9895604649984ULL);
    record.adverts[advert::used] = dannan::membership::u64_advert(1835376108947ULL);
    record.adverts[advert::metadata_generation] = dannan::membership::u64_advert(168518);
    record.adverts[advert::metadata_write_replicas] = dannan::membership::u64_advert(1);
    record.adverts[advert::hosts_extents] = dannan::membership::u64_advert(1);
    record.capabilities = 0;
    const auto full = node_info_from(record);
    REQUIRE(full.capacity == 9895604649984ULL);
    REQUIRE(full.used == 1835376108947ULL);
    REQUIRE(full.metadata_generation == 168518);
    REQUIRE(full.metadata_write_replicas_required == 1);
    REQUIRE(!node_inbound_capable(full));
    REQUIRE(node_hosts_extents(full));

    // An advert of another width is absent.
    record.adverts[advert::capacity] = {1, 2, 3};
    REQUIRE(node_info_from(record).capacity == 0);
}

// Every key section 3 names reaches its NodeConfig field; the advertised host and failure
// domain fall back as Macha's do; no node-wide watch is applied to calls; the memory
// figures are Macha's own fit, with no limit for the node to fit again.
MACHA_TEST("dannan_bridge", test_the_node_config_is_macha_config_key_by_key) {
    Config config;
    config.state_path = "/var/lib/macha";
    config.node_name = "inverbeg";
    config.listen_host = "0.0.0.0";
    config.advertise_host = "inverbeg.macha.network";
    config.failure_domain = "site-b";
    config.port = 7437;
    config.bootstrap = {Endpoint{"macnessa.macha.network", 7437}};
    config.inbound_capable = Tristate::yes;
    config.heartbeat = std::chrono::milliseconds(6000);
    config.dead_after = std::chrono::milliseconds(31000);
    config.maintenance.garbage_grace = std::chrono::hours(24 * 31);
    config.telemetry_interval = std::chrono::milliseconds(11000);
    config.connect_timeout = std::chrono::milliseconds(2600);
    config.max_frame_size = 512 * 1024;
    config.control_no_progress_deadline = std::chrono::milliseconds(30000);
    config.control_stall_notice = std::chrono::milliseconds(5000);
    config.data_stall_notice = std::chrono::milliseconds(120000);
    config.dial_back_probe_min_interval = std::chrono::milliseconds(11000);
    config.inbound_reprobe_while_capable = std::chrono::minutes(61);
    config.inbound_reprobe_while_incapable = std::chrono::minutes(11);
    config.upnp.enabled = true;
    config.upnp.external_port = 17437;
    config.upnp.discovery_timeout = std::chrono::milliseconds(2100);
    config.upnp.lease_seconds = 3600;
    config.external_ip.enabled = true;
    config.external_ip.timeout = std::chrono::milliseconds(3100);
    config.connectivity_check.enabled = true;
    config.connectivity_check.timeout = std::chrono::milliseconds(3200);
    config.runtime.memory_bytes = 4ULL << 30;
    config.runtime.retained_memory_bytes = 700ULL << 20;
    config.runtime.control_memory_reserve_bytes = 65ULL << 20;
    config.runtime.viewer_memory_reserve_bytes = 193ULL << 20;
    config.runtime.loader_memory_reserve_bytes = 66ULL << 20;
    config.runtime.reassembly_memory_reserve_bytes = 33ULL << 20;

    const auto node = dannan_node_config(config, dannan::identity::ClusterKey{});
    REQUIRE(node.state_path == config.state_path);
    REQUIRE(node.node_name == "inverbeg");
    REQUIRE(node.listen_host == "0.0.0.0");
    REQUIRE(node.advertise_host == "inverbeg.macha.network");
    REQUIRE(node.failure_domain == "site-b");
    REQUIRE(node.port == 7437);
    REQUIRE(node.bootstrap.size() == 1);
    REQUIRE(node.bootstrap[0].host == "macnessa.macha.network");
    REQUIRE(node.bootstrap[0].port == 7437);
    REQUIRE(node.inbound == dannan::reach::Inbound::yes);
    REQUIRE(node.heartbeat == std::chrono::milliseconds(6000));
    REQUIRE(node.dead_after == std::chrono::milliseconds(31000));
    REQUIRE(node.forget_after == std::chrono::hours(24 * 31));
    REQUIRE(node.telemetry_interval == std::chrono::milliseconds(11000));
    REQUIRE(node.connect_timeout == std::chrono::milliseconds(2600));
    REQUIRE(node.max_frame == 512 * 1024);
    REQUIRE(node.control_no_progress.count() == 0);
    REQUIRE(node.data_no_progress.count() == 0);
    REQUIRE(node.control_stall_notice.count() == 0);
    REQUIRE(node.data_stall_notice.count() == 0);
    REQUIRE(node.dial_back_min_interval == std::chrono::milliseconds(11000));
    REQUIRE(node.reprobe_while_capable == std::chrono::minutes(61));
    REQUIRE(node.reprobe_while_incapable == std::chrono::minutes(11));
    REQUIRE(node.connectivity.upnp.enabled);
    REQUIRE(node.connectivity.upnp.external_port == 17437);
    REQUIRE(node.connectivity.upnp.discovery_timeout == std::chrono::milliseconds(2100));
    REQUIRE(node.connectivity.upnp.lease_seconds == 3600);
    REQUIRE(node.connectivity.external_ip.enabled);
    REQUIRE(node.connectivity.external_ip.timeout == std::chrono::milliseconds(3100));
    REQUIRE(node.connectivity.check.enabled);
    REQUIRE(node.connectivity.check.timeout == std::chrono::milliseconds(3200));
    REQUIRE(node.memory_bytes == (700ULL << 20));
    REQUIRE(node.control_reserve_bytes == (65ULL << 20));
    REQUIRE(node.foreground_reserve_bytes == (193ULL << 20));
    REQUIRE(node.loader_reserve_bytes == (66ULL << 20));
    REQUIRE(node.reassembly_reserve_bytes == (33ULL << 20));
    REQUIRE(node.memory_limit_bytes == 0);

    // The fallbacks: loopback listen is advertised as itself, the domain is the host.
    Config bare;
    bare.listen_host = "127.0.0.1";
    const auto fallback = dannan_node_config(bare, dannan::identity::ClusterKey{});
    REQUIRE(fallback.advertise_host == "127.0.0.1");
    REQUIRE(fallback.failure_domain == "127.0.0.1");
    bare.inbound_capable = Tristate::no;
    REQUIRE(dannan_node_config(bare, {}).inbound == dannan::reach::Inbound::no);
    bare.inbound_capable = Tristate::automatic;
    REQUIRE(dannan_node_config(bare, {}).inbound == dannan::reach::Inbound::automatic);
    // Any other listen address advertises the machine's name, never an empty host.
    bare.listen_host = "0.0.0.0";
    REQUIRE(!advertised_host(bare).empty());
}

// Macha's telemetry goes into Dannan's record and comes back whole: core fields as Dannan
// writes them, hosted fields 1024 to 1045 in their order, nothing written for a zero, at
// most 256 transcode rates, and one unreadable field drops that record alone.
MACHA_TEST("dannan_bridge", test_telemetry_goes_into_the_record_and_back) {
    REQUIRE(hosted_telemetry_layout().size() == 22);
    REQUIRE(hosted_telemetry_layout().front() == 1024);
    REQUIRE(hosted_telemetry_layout().back() == 1045);

    dannan::status::Telemetry record;
    dannan::identity::NodeId id;
    id.bytes.fill(3);
    record.set_bytes(dannan::status::field::node_id, id.bytes);
    record.set_u64(dannan::status::field::sequence, 7);
    record.set_text(dannan::status::field::version, "0.94.0");
    record.set_text(dannan::status::field::host, "macnessa.macha.network");
    record.set_u16(dannan::status::field::port, 7437);
    record.set_u32(dannan::status::field::peers_active, 2);
    record.set_phase(dannan::status::Phase::recovering);
    record.set_traffic(1000, {{2, 10, 20, 30, 40}});

    HostedTelemetry hosted;
    hosted.storage_capacity = 10995116277760ULL;
    hosted.storage_used = 6364071813037ULL;
    hosted.cache_capacity = 1ULL << 30;
    hosted.metadata_generation = 168518;
    hosted.storage_backends_online = 1;
    hosted.api_endpoint = "http://10.44.1.50:7438";
    hosted.playback.startup_timeout_ms = 30000;
    hosted.playback.max_transcodes_per_account = 2;
    hosted.playback.start_failed_retention_ms = 60000;
    hosted.cache.hits = 5;
    hosted.cache.evictions = 9;
    for (uint32_t i = 0; i < 300; ++i)
        hosted.playback.transcode_rates.push_back(TranscodeRate{"video", "hevc", 10, 2160, i, 1, 1});
    write_hosted_telemetry(record, hosted);
    // Zero values are not written: cache_used, cache_misses and the rest.
    REQUIRE(!record.get(hosted_field::cache_used).has_value());
    REQUIRE(!record.get(hosted_field::cache_misses).has_value());

    const auto back = node_telemetry_from(record);
    REQUIRE(back.has_value());
    REQUIRE(back->node_id.bytes == id.bytes);
    REQUIRE(back->sequence == 7);
    REQUIRE(back->version == "0.94.0");
    REQUIRE(back->host == "macnessa.macha.network");
    REQUIRE(back->port == 7437);
    REQUIRE(back->peers_active == 2);
    REQUIRE(back->phase == NodePhase::recovering);
    REQUIRE(back->traffic_window_ms == 1000);
    REQUIRE(back->traffic.size() == 1);
    REQUIRE(back->traffic[0].frame_class == 2);
    REQUIRE(back->traffic[0].out_bytes_per_s == 40);
    REQUIRE(back->storage_capacity == hosted.storage_capacity);
    REQUIRE(back->storage_used == hosted.storage_used);
    REQUIRE(back->cache_capacity == hosted.cache_capacity);
    REQUIRE(back->cache_used == 0);
    REQUIRE(back->metadata_generation == 168518);
    REQUIRE(back->storage_backends_online == 1);
    REQUIRE(back->api_endpoint == "http://10.44.1.50:7438");
    REQUIRE(back->playback_startup_timeout_ms == 30000);
    REQUIRE(back->playback_max_transcodes_per_account == 2);
    REQUIRE(back->playback_start_failed_retention_ms == 60000);
    REQUIRE(back->cache_hits == 5);
    REQUIRE(back->cache_evictions == 9);
    REQUIRE(back->playback_transcode_rates.size() == 256);
    REQUIRE(back->playback_transcode_rates.back().rate_milli == 255);

    // Clearing a figure removes its field.
    hosted.playback.transcode_rates.clear();
    hosted.storage_used = 0;
    write_hosted_telemetry(record, hosted);
    REQUIRE(!record.get(hosted_field::playback_transcode_rates).has_value());
    REQUIRE(!record.get(hosted_field::storage_used).has_value());

    // An empty rates field, as a peer whose set_bytes kept one would send, reads as none.
    auto empty_rates = record;
    empty_rates.set_bytes(hosted_field::playback_transcode_rates, {});
    const auto read = node_telemetry_from(empty_rates);
    REQUIRE(read.has_value());
    REQUIRE(read->playback_transcode_rates.empty());

    // A hosted field of the wrong width drops the record, and only it.
    auto bad = record;
    bad.set_bytes(hosted_field::storage_capacity, std::vector<uint8_t>{1, 2, 3});
    REQUIRE(!node_telemetry_from(bad).has_value());
    REQUIRE(node_telemetry_from(record).has_value());
}

// Every frame class, id and message survives the trip both ways.
MACHA_TEST("dannan_bridge", test_frames_ids_and_messages_convert_both_ways) {
    for (const auto frame : {FrameType::control, FrameType::foreground, FrameType::read_ahead,
                             FrameType::speculative, FrameType::loader})
        REQUIRE(macha_frame(dannan_frame(frame)) == frame);
    REQUIRE(work_class(FrameType::foreground) == WorkClass::viewer);
    REQUIRE(dannan::sched::work_class(dannan_frame(FrameType::foreground)) ==
            dannan::sched::WorkClass::foreground);
    NodeId id;
    for (size_t i = 0; i < id.bytes.size(); ++i)
        id.bytes[i] = static_cast<uint8_t>(i * 17);
    REQUIRE(macha_id(dannan_id(id)) == id);
    const RpcMessage message(MessageType::trie_diff, Bytes{1, 2, 3});
    const auto back = macha_message(dannan_message(message));
    REQUIRE(back.type == MessageType::trie_diff);
    REQUIRE(back.payload == message.payload);
    REQUIRE(static_cast<uint16_t>(dannan_message(message).type) == 561);
}

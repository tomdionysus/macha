// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversions between Macha's cluster vocabulary and Dannan's, and the wire rules
// Macha registers: pure functions, tested over everything they cover.
#include "cluster/dannan_bridge.hpp"
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

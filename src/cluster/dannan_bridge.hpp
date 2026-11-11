// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conversions between Macha's cluster vocabulary and Dannan's, and the wire rules
// Macha registers for each of its message types. Pure: no node, no I/O.

#include "cluster/frame_type.hpp"
#include "cluster/net.hpp"
#include "types.hpp"

#include <dannan/membership/node_record.h>
#include <dannan/net/message.h>
#include <dannan/net/routes.h>
#include <dannan/sched/work.h>

#include <optional>
#include <string>
#include <vector>

namespace macha {

// The adverts Macha puts in its node record; Dannan carries them unread.
namespace advert {
inline const std::string capacity = "macha/capacity";
inline const std::string used = "macha/used";
inline const std::string metadata_generation = "macha/metadata-generation";
inline const std::string metadata_write_replicas = "macha/metadata-write-replicas";
inline const std::string hosts_extents = "macha/hosts-extents";
} // namespace advert

// A Macha type's wire number, which is also Dannan's.
inline dannan::net::MessageType dannan_type(MessageType type) noexcept {
    return dannan::net::MessageType{static_cast<uint16_t>(type)};
}
inline MessageType macha_type(dannan::net::MessageType type) noexcept {
    return static_cast<MessageType>(static_cast<uint16_t>(type));
}

// The two enums have equal values, 1 to 5.
inline dannan::sched::FrameType dannan_frame(FrameType frame) noexcept {
    return static_cast<dannan::sched::FrameType>(static_cast<uint8_t>(frame));
}
inline FrameType macha_frame(dannan::sched::FrameType frame) noexcept {
    return static_cast<FrameType>(static_cast<uint8_t>(frame));
}

inline dannan::identity::NodeId dannan_id(const NodeId& id) noexcept {
    dannan::identity::NodeId out;
    out.bytes = id.bytes;
    return out;
}
inline NodeId macha_id(const dannan::identity::NodeId& id) noexcept {
    NodeId out;
    out.bytes = id.bytes;
    return out;
}

dannan::net::Message dannan_message(const RpcMessage& message);
RpcMessage macha_message(dannan::net::Message message);

// A node record as Macha's NodeInfo: place and identity from the record, the storage
// figures, metadata generation and write floor from Macha's adverts (zero when
// absent), inbound capability from the record's capability bit, hosts_extents from
// its advert.
NodeInfo node_info_from(const dannan::membership::NodeRecord& record);

// Macha's types with their wire rules (first cutover, section 7.2), registered before
// the node starts; Dannan's core types are not among them.
struct MachaRegistration {
    MessageType type;
    dannan::net::Registration registration;
};
const std::vector<MachaRegistration>& macha_registrations();

// Numbers once used and never to be reused: 549, 550, 560, 636.
const std::vector<dannan::net::MessageType>& macha_retired_types();

} // namespace macha

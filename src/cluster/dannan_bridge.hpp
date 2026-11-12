// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conversions between Macha's cluster vocabulary and Dannan's, and the wire rules
// Macha registers for each of its message types. Pure: no node, no I/O.

#include "cluster/frame_type.hpp"
#include "cluster/inbound_resolution.hpp"
#include "cluster/net.hpp"
#include "cluster/public_connectivity.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "types.hpp"

#include <dannan/identity/cluster_key.h>
#include <dannan/membership/node_record.h>
#include <dannan/net/message.h>
#include <dannan/net/routes.h>
#include <dannan/node.h>
#include <dannan/sched/work.h>
#include <dannan/status/log.h>

#include <memory>
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
// Present while the node hosts extents, whatever its value: Dagda's name, which the
// state importer writes too.
inline const std::string hosts_extents = "dagda/extents";
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
// the extents advert's presence.
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

// The host this node advertises: network.advertise; else the listen address when it is
// loopback; else the machine's name; else 127.0.0.1. And its failure domain:
// network.failure_domain, else that host.
std::string advertised_host(const Config& config);
std::string advertised_failure_domain(const Config& config, const std::string& host);

// The cluster key as Dannan derives it from the same file: the master is the file's
// SHA-256 for both, so Dannan's auth and cluster id follow from Macha's master.
dannan::identity::ClusterKey dannan_cluster_key(const ClusterKeys& keys);

dannan::membership::IdentityReset dannan_identity_reset(const IdentityAssociationReset& reset);
IdentityAssociationReset macha_identity_reset(const dannan::membership::IdentityReset& reset);

PublicConnectivityStatus macha_connectivity(const dannan::reach::PublicConnectivityStatus& status);
RpcStats macha_rpc_stats(const dannan::net::ClientStats& stats);
RpcServerWorkStats macha_work_stats(const dannan::net::WorkStats& stats);

// The node's inbound answer with Macha's configured modes beside it, and hosts_extents
// resolved from it: `auto` is "yes if there is somewhere to put them and peers can
// fetch them".
bool resolve_hosts_extents(const Config& config, bool inbound_capable);
InboundResolution macha_inbound(const Config& config, const dannan::reach::InboundState& state);

// The node's log, written through Macha's: one level (Macha's log_level), one
// format and one destination for every line the process writes. Dannan's scope
// leads the message, as "scope: message".
std::shared_ptr<dannan::status::LogSink> macha_log_sink();

// The node's configuration from Macha's (first cutover, section 3). Every shared default
// agrees; what Macha computes first is computed here: the advertised host and failure
// domain, and no node-wide watch on calls (each call names its own; F1). What only the
// host can give (the gateway, version, planes, telemetry, log sink, gauges, clocks) is
// left for it to set.
dannan::NodeConfig dannan_node_config(const Config& config, dannan::identity::ClusterKey key);

} // namespace macha

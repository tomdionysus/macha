// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

struct NodeId {
    std::array<uint8_t, 16> bytes{};
    auto operator<=>(const NodeId&) const = default;
};

struct Hash256 {
    std::array<uint8_t, 32> bytes{};
    auto operator<=>(const Hash256&) const = default;
};

using ObjectId = Hash256;

struct Endpoint {
    std::string host;
    uint16_t port{};
};

struct IdentityAssociationReset {
    std::string host;
    uint16_t port{};
    NodeId stale_node_id{};
    uint64_t epoch{};
    uint64_t reset_unix_ms{};
    NodeId reset_by{};
    std::string reason;
    auto operator<=>(const IdentityAssociationReset&) const = default;
};

// Self-declared node properties, gossiped so every peer makes the same dialling
// and placement decisions. Both default to set: an ordinary dialable storage node.
inline constexpr uint8_t node_flag_inbound_capable = 1U << 0;
inline constexpr uint8_t node_flag_hosts_extents = 1U << 1;
inline constexpr uint8_t node_flags_default = node_flag_inbound_capable | node_flag_hosts_extents;

struct NodeInfo {
    NodeId id{};
    std::string host;
    std::string failure_domain;
    uint16_t port{};
    uint64_t capacity{};
    uint64_t used{};
    uint64_t seen_unix_ms{};
    uint64_t metadata_generation{};
    // Metadata safety floor. Every metadata-capable node must advertise the same
    // value; mismatches fail closed so a weaker node cannot mint an acceptance
    // certificate.
    uint32_t metadata_write_replicas_required{};
    // node_flag_* bits. `inbound_capable` clear means peers must never dial
    // this node's advertised endpoint: it reaches them, they answer over the
    // session it opened, and a lane it has not opened is asked for with a
    // dial_request. `hosts_extents` clear means the node is never a DATA
    // placement owner or fallback holder (an edge node with no storage.data).
    uint8_t flags{node_flags_default};
};

inline bool node_inbound_capable(const NodeInfo& node) noexcept {
    return (node.flags & node_flag_inbound_capable) != 0;
}
inline bool node_hosts_extents(const NodeInfo& node) noexcept {
    return (node.flags & node_flag_hosts_extents) != 0;
}
inline uint8_t node_flags_for(bool inbound_capable, bool hosts_extents) noexcept {
    return static_cast<uint8_t>((inbound_capable ? node_flag_inbound_capable : 0U) |
                                (hosts_extents ? node_flag_hosts_extents : 0U));
}

struct NodeIdHash {
    size_t operator()(const NodeId&) const noexcept;
};

// Identity HttpServer attaches to a request once its bearer token validates
// against the session store (session.hpp); no revocation or expiry bookkeeping.
struct SessionIdentity {
    std::string id;
    Hash256 token_hash{};
    std::vector<std::string> roles;
    // Default member initialiser keeps designated-initialiser construction clear
    // of -Wmissing-field-initializers.
    std::string user_id{}; // empty for an anonymous session
};

std::string hex(std::span<const uint8_t>);
std::optional<Bytes> unhex(const std::string&);
std::string to_string(const NodeId&);
std::string to_string(const ObjectId&);
std::string endpoint_identity_key(std::string_view host, uint16_t port);
std::string identity_reset_key(std::string_view host, uint16_t port);
bool identity_reset_matches_endpoint(const IdentityAssociationReset&, std::string_view host, uint16_t port);
bool identity_reset_matches_node(const IdentityAssociationReset&, const NodeId&);
inline std::string endpoint_identity_key(const Endpoint& endpoint) {
    return endpoint_identity_key(endpoint.host, endpoint.port);
}
uint64_t unix_ms();
} // namespace macha

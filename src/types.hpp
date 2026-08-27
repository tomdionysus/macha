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

struct NodeInfo {
    NodeId id{};
    std::string host;
    std::string failure_domain;
    uint16_t port{};
    uint64_t capacity{};
    uint64_t used{};
    uint64_t seen_unix_ms{};
    uint64_t metadata_generation{};
};

struct NodeIdHash {
    size_t operator()(const NodeId&) const noexcept;
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

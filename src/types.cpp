// SPDX-License-Identifier: GPL-3.0-or-later
#include "types.hpp"

#include <charconv>

namespace macha {
size_t NodeIdHash::operator()(const NodeId& id) const noexcept {
    size_t value = 1469598103934665603ULL;
    for (auto byte : id.bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

std::string hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
}

std::optional<Bytes> unhex(const std::string& value) {
    if (value.size() % 2)
        return {};

    Bytes out(value.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned parsed = 0;
        auto begin = value.data() + i * 2;
        auto [end, error] = std::from_chars(begin, begin + 2, parsed, 16);
        if (error != std::errc{} || end != begin + 2 || parsed > 255)
            return {};
        out[i] = static_cast<uint8_t>(parsed);
    }
    return out;
}

std::string to_string(const NodeId& id) {
    return hex(id.bytes);
}

std::string to_string(const ObjectId& id) {
    return hex(id.bytes);
}


std::string endpoint_identity_key(std::string_view host, uint16_t port) {
    // Brackets keep IPv6 endpoints unambiguous while preserving the familiar
    // host:port representation used in diagnostics and management responses.
    return "[" + std::string(host) + "]:" + std::to_string(port);
}

std::string identity_reset_key(std::string_view host, uint16_t port) {
    if (port)
        return endpoint_identity_key(host, port);
    return "[" + std::string(host) + "]:*";
}

bool identity_reset_matches_endpoint(const IdentityAssociationReset& reset, std::string_view host,
                                     uint16_t port) {
    return reset.host == host && (!reset.port || reset.port == port);
}

bool identity_reset_matches_node(const IdentityAssociationReset& reset, const NodeId& node) {
    return reset.stale_node_id == NodeId{} || reset.stale_node_id == node;
}

uint64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace macha

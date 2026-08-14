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

uint64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace macha

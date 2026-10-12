// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/net.hpp"
#include "codec.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace macha {

// Why a peer refused a request, said as the HTTP API says it: the HTTP status
// number, a snake_case code, and the reason in English.
struct RpcRefusal {
    // Zero when the refusal names none: a peer before 0.90.88, or the
    // transport refusing on the peer's behalf.
    uint16_t status{};
    std::string code;
    std::string message;
};

// The reason goes first, so a reader that takes only the text still reads it.
inline RpcMessage error_reply(uint16_t status, std::string_view code, std::string_view message) {
    Writer writer;
    writer.string(std::string(message));
    writer.u16(status);
    writer.string(std::string(code));
    return {MessageType::error, writer.take()};
}

// The refusal an error reply carries; empty for any other reply.
inline std::optional<RpcRefusal> rpc_refusal(const RpcMessage& message) {
    if (message.type != MessageType::error)
        return std::nullopt;
    RpcRefusal out;
    try {
        Reader reader(message.payload);
        if (!reader.remaining())
            return out;
        out.message = reader.string();
        if (!reader.remaining())
            return out;
        out.status = reader.u16();
        out.code = reader.string();
    } catch (const std::exception&) {
    }
    return out;
}

// Whether asking again may get another answer: 429, 500, 502, 503 and 504,
// and a refusal that names no status. Any other refusal stands.
inline bool rpc_try_later(const RpcRefusal& refusal) noexcept {
    switch (refusal.status) {
    case 0:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504: return true;
    default: return false;
    }
}

} // namespace macha

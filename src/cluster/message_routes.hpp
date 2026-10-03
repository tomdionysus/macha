// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "cluster/net.hpp"
#include "codec.hpp"
#include "contract/thread_safety.hpp"

#include <array>
#include <functional>
#include <stdexcept>
#include <string>

namespace macha {

// Where the node's RPC server sends each inbound request: one handler per
// message type, bound by whoever constructs the part that answers it and
// unbound before that part is destroyed. unbind() waits for calls in flight,
// so no call reaches a destroyed part. A request whose type is unbound, or
// whose handler throws, gets an error reply. Thread-safe; a handler runs under
// the route lock and must not bind or unbind.
// The reply a handler gives when it refuses a request: the reason as text.
inline RpcMessage error_reply(const std::string& text) {
    Writer writer;
    writer.string(text);
    return {MessageType::error, writer.take()};
}

class MessageRoutes {
  public:
    using Handler =
        std::function<RpcMessage(const NodeInfo& peer, FrameType, const RpcMessage& request)>;

    void bind(MessageType type, Handler handler) {
        WriteLock lock(mutex_);
        handlers_[index(type)] = std::move(handler);
    }
    // Waits for calls in flight on any route.
    void unbind(MessageType type) {
        WriteLock lock(mutex_);
        handlers_[index(type)] = {};
    }
    RpcMessage dispatch(const NodeInfo& peer, FrameType frame_type,
                        const RpcMessage& request) const {
        if (static_cast<size_t>(request.type) >= route_count)
            return error_reply("unsupported request");
        ReadLock lock(mutex_);
        const auto& handler = handlers_[index(request.type)];
        if (!handler)
            return error_reply(std::string(message_type_name(request.type)) +
                               " is not available on this node");
        try {
            return handler(peer, frame_type, request);
        } catch (const std::exception& failure) {
            return error_reply(failure.what());
        }
    }

  private:
    // Every message type is numbered below this.
    static constexpr size_t route_count = 128;
    static size_t index(MessageType type) {
        const auto value = static_cast<size_t>(type);
        if (value >= route_count)
            throw std::out_of_range("message type beyond the route table");
        return value;
    }

    // Held shared across each handler call, which may do any I/O.
    mutable IoSharedMutex mutex_;
    std::array<Handler, route_count> handlers_ MACHA_GUARDED_BY(mutex_);
};

} // namespace macha

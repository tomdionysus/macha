// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "cluster/net.hpp"
#include "cluster/rpc_refusal.hpp"
#include "codec.hpp"
#include "contract/thread_safety.hpp"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace macha {

// Where the node's RPC server sends each inbound request: one handler per
// message type, bound by whoever constructs the part that answers it and
// unbound before that part is destroyed. unbind() waits for calls in flight,
// so no call reaches a destroyed part. A request whose type is unbound, or
// whose handler throws, gets an error reply. Thread-safe; a handler runs under
// the route lock and must not bind or unbind.
class MessageRoutes {
  public:
    using Handler =
        std::function<RpcMessage(const NodeInfo& peer, FrameType, const RpcMessage& request)>;

    void bind(MessageType type, Handler handler) {
        WriteLock lock(mutex_);
        handlers_[type] = std::move(handler);
    }
    // Waits for calls in flight on any route.
    void unbind(MessageType type) {
        WriteLock lock(mutex_);
        handlers_.erase(type);
    }
    RpcMessage dispatch(const NodeInfo& peer, FrameType frame_type,
                        const RpcMessage& request) const {
        ReadLock lock(mutex_);
        const auto found = handlers_.find(request.type);
        if (found == handlers_.end() || !found->second)
            return error_reply(501, "not_implemented",
                               std::string(message_type_name(request.type)) +
                                   " is not available on this node");
        try {
            return found->second(peer, frame_type, request);
        } catch (const std::exception& failure) {
            return error_reply(500, "internal_error", failure.what());
        }
    }

  private:
    // Held shared across each handler call, which may do any I/O.
    mutable IoSharedMutex mutex_;
    std::map<MessageType, Handler> handlers_ MACHA_GUARDED_BY(mutex_);
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/rpc_wait.hpp"

#include "diagnostics.hpp"
#include "log.hpp"

#include <stdexcept>
#include <string>

namespace macha {

RpcReply wait_for_reply(AsyncRpc& async, std::string_view peer, MessageType type,
                        FrameType frame_type, Clock::time_point call_started,
                        std::chrono::milliseconds stall_notice,
                        std::chrono::milliseconds no_progress_deadline) {
    if (stall_notice.count() <= 0 && no_progress_deadline.count() <= 0)
        return async.get();
    auto poll = stall_notice.count() > 0 ? stall_notice : no_progress_deadline;
    if (no_progress_deadline.count() > 0)
        poll = std::min(poll, no_progress_deadline);
    while (async.wait_for(poll) != std::future_status::ready) {
        const auto idle = async.idle_for();
        if (no_progress_deadline.count() > 0 && idle >= no_progress_deadline) {
            async.cancel();
            const auto text =
                std::string("RPC made no progress for ") + std::to_string(idle.count()) + " ms (" +
                frame_type_name(frame_type) + ") peer=" + std::string(peer) +
                " message=" + message_type_name(type) +
                " age_ms=" + std::to_string(elapsed_ms(call_started)) + "; cancelled for retry";
            Log::debug(text);
            throw std::runtime_error(text);
        }
        if (stall_notice.count() > 0 && idle >= stall_notice) {
            Log::debug(std::string("RPC stalled (") + frame_type_name(frame_type) +
                       ") peer=" + std::string(peer) + " message=" + message_type_name(type) +
                       " age_ms=" + std::to_string(elapsed_ms(call_started)) +
                       " no_progress_ms=" + std::to_string(idle.count()) +
                       (no_progress_deadline.count() > 0
                            ? "; deadline_ms=" + std::to_string(no_progress_deadline.count())
                            : "; request remains active while peer health is monitored"));
        }
    }
    return async.get();
}

} // namespace macha

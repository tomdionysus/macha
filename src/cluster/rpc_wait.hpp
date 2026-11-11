// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/net.hpp"

#include <chrono>
#include <string_view>

namespace macha {

// How a synchronous call waits for its reply: a stall notice in the journal once the
// reply has made no progress for `stall_notice`, and, past `no_progress_deadline`, the
// call cancelled and failed as transient, for the caller to retry under its own policy.
// Zero turns either off. Throws what the call's future throws.
RpcReply wait_for_reply(AsyncRpc& async, std::string_view peer, MessageType type,
                        FrameType frame_type, Clock::time_point call_started,
                        std::chrono::milliseconds stall_notice,
                        std::chrono::milliseconds no_progress_deadline);

} // namespace macha

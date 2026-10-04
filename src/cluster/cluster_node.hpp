// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/membership.hpp"
#include "cluster/net.hpp"
#include "config.hpp"
#include "contract/work.hpp"

#include <chrono>
#include <optional>
#include <span>

namespace macha {

// This node as the parts that place, fetch and repair objects see it: its
// identity and configuration, the membership placement ranks, and calls to
// its peers. Handed to each part at construction and owned by whoever
// constructed it, which keeps it alive for the part's lifetime. NodeRuntime
// is the production node.
class ClusterNode {
  public:
    virtual ~ClusterNode() = default;

    // Fixed at construction.
    static constexpr Waits config_waits = Waits::none;
    static constexpr ThreadSafety config_safety = ThreadSafety::thread_safe;
    virtual const Config& config() const = 0;

    static constexpr Waits node_id_waits = Waits::none;
    static constexpr ThreadSafety node_id_safety = ThreadSafety::thread_safe;
    virtual NodeId node_id() const = 0;

    // Names this process's durable placements; a replica stamped with another
    // epoch was placed by an earlier incarnation.
    static constexpr Waits durability_epoch_waits = Waits::none;
    static constexpr ThreadSafety durability_epoch_safety = ThreadSafety::thread_safe;
    virtual NodeId durability_epoch() const = 0;

    // The roster placement ranks, this node included. Membership is itself
    // thread safe.
    static constexpr Waits membership_waits = Waits::none;
    static constexpr ThreadSafety membership_safety = ThreadSafety::thread_safe;
    virtual Membership& membership() = 0;
    virtual const Membership& membership() const = 0;

    // One call to `peer`, returning its reply. Throws when the call cannot be
    // placed, fails, or makes no progress within the node's deadline for it.
    static constexpr Waits call_waits = Waits::network | Waits::locks;
    static constexpr ThreadSafety call_safety = ThreadSafety::thread_safe;
    virtual RpcReply call(const NodeInfo& peer, MessageType, std::span<const uint8_t> payload,
                          FrameType) = 0;

    // Places one call to `peer` and returns without its reply; may dial a
    // lane first. Throws when the call cannot be placed.
    static constexpr Waits call_async_waits = Waits::network | Waits::locks;
    static constexpr ThreadSafety call_async_safety = ThreadSafety::thread_safe;
    virtual AsyncRpc call_async(const NodeInfo& peer, MessageType,
                                std::span<const uint8_t> payload, FrameType) = 0;

    // The smoothed round trip to `peer`, once one has been measured.
    static constexpr Waits peer_latency_waits = Waits::none;
    static constexpr ThreadSafety peer_latency_safety = ThreadSafety::thread_safe;
    virtual std::optional<std::chrono::milliseconds> peer_latency(const NodeId& peer) const = 0;

    // The DATA pool's figures, published to peers with this node's info.
    static constexpr Waits advertise_storage_waits = Waits::none;
    static constexpr ThreadSafety advertise_storage_safety = ThreadSafety::thread_safe;
    virtual void advertise_storage(uint64_t used, uint64_t capacity) = 0;
};

} // namespace macha

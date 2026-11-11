// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Dannan's node, hosted for Macha: the io context and its threads, the node, every
// Macha message type registered with its wire rules, and calls placed with the deadline
// each caller names. Requests reach Macha's MessageRoutes, so every part binds and
// unbinds its handlers as before.

#include "cluster/membership.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/net.hpp"
#include "types.hpp"

#include <dannan/node.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace macha {

// Dannan's roster as Macha's Membership reads it: each record as NodeInfo, this node
// left to Membership itself. Lives no longer than the node.
class DannanMembership final : public MembershipView {
    const dannan::Nodes& nodes_;
    const dannan::identity::NodeId self_;

  public:
    DannanMembership(const dannan::Nodes& nodes, dannan::identity::NodeId self)
        : nodes_(nodes), self_(self) {}
    MembershipSnapshot peers() const override;
    bool directly_reachable(const NodeId& id) const override;
    bool all_known_reachable() const override;
};

class DannanHost {
  public:
    // Told of a peer's metadata notice as it arrives, on the session, ahead of every
    // queue. Must not wait.
    using MetadataNotice = std::function<void(const NodeInfo& peer, const RpcMessage& notice)>;

    // Builds the node over `config` (state lock taken, state read), registers every
    // Macha type and retires the four unused numbers. The io threads run from here, so
    // the node's own work proceeds before start(). Throws what Node's constructor throws.
    DannanHost(dannan::NodeConfig config, MessageRoutes& routes, size_t io_threads,
               MetadataNotice metadata_notice);
    ~DannanHost();
    DannanHost(const DannanHost&) = delete;
    DannanHost& operator=(const DannanHost&) = delete;

    void start();
    void request_stop();
    // Fails every outbound call now and admits no more; the node runs on.
    void cancel_outbound_calls();
    // The node stopped and destroyed, then the io threads joined. Idempotent. Nothing
    // may use node() after.
    void stop();

    dannan::Node& node() noexcept { return *node_; }
    const dannan::Node& node() const noexcept { return *node_; }

    // A call to `peer`, or to an endpoint, ending at the latest when `deadline` passes.
    // The future holds the reply (an error reply is a reply), or throws why none came.
    AsyncRpc call_async(const NodeInfo& peer, MessageType type, std::span<const uint8_t> payload,
                        FrameType frame, std::chrono::milliseconds deadline);
    AsyncRpc call_async(const Endpoint& endpoint, MessageType type,
                        std::span<const uint8_t> payload, FrameType frame,
                        std::chrono::milliseconds deadline);

  private:
    // The peer's record as the roster knows it, else one made from what Macha holds.
    dannan::membership::NodeRecord record_for(const NodeInfo& peer) const;
    AsyncRpc wrap(std::shared_ptr<std::promise<RpcReply>> promise,
                  std::shared_ptr<dannan::net::CallHandle> handle);

    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::unique_ptr<dannan::Node> node_;
    std::vector<std::jthread> io_threads_;
    std::atomic_bool started_{};
    std::atomic_bool stopped_{};
};

} // namespace macha

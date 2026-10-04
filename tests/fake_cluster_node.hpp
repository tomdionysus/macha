// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "test_support.hpp"

#include "cluster/cluster_node.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/local_state.hpp"
#include "cluster/node_identity.hpp"
#include "cluster/node_resources.hpp"

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace macha::test_support {

// A ClusterNode with no transport. The roster is what the test adds, and a
// call to a peer runs that peer's handler in this process: call() on the
// caller's thread, call_async() on a thread of its own, so a handler may
// block (on a TestGate) without holding the caller. A cancelled or aborted
// call fails at once, as a closed route fails it; its handler still runs to
// the end. Every call is recorded.
class FakeClusterNode final : public ClusterNode {
  public:
    using Handler = std::function<RpcMessage(MessageType, const Bytes& payload, FrameType)>;
    struct Call {
        NodeId peer;
        MessageType type{};
        FrameType frame_type{};
        size_t bytes{};
    };

    FakeClusterNode(Config config, NodeInfo self, NodeId epoch)
        : config_(std::move(config)), self_id_(self.id), epoch_(epoch),
          members_(std::move(self), std::chrono::hours(1)) {}

    ~FakeClusterNode() override {
        std::vector<std::jthread> running;
        {
            std::lock_guard lock(mutex_);
            running.swap(threads_);
        }
        running.clear();
    }

    // Adds `peer` to the roster, answering with `handler`. A handler-less
    // peer answers every request with ok.
    void add_peer(const NodeInfo& peer, Handler handler = {}) {
        {
            std::lock_guard lock(mutex_);
            handlers_[peer.id] = std::move(handler);
        }
        members_.observe(peer, true);
    }
    // Calls to `peer` cannot be placed: call_async() and call() throw.
    void refuse(const NodeId& peer) {
        std::lock_guard lock(mutex_);
        refused_.insert(peer);
    }

    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    size_t calls_of(MessageType type, std::optional<NodeId> peer = {}) const {
        std::lock_guard lock(mutex_);
        return static_cast<size_t>(std::count_if(calls_.begin(), calls_.end(), [&](const Call& c) {
            return c.type == type && (!peer || c.peer == *peer);
        }));
    }

    const Config& config() const override { return config_; }
    NodeId node_id() const override { return self_id_; }
    NodeId durability_epoch() const override { return epoch_; }
    Membership& membership() override { return members_; }
    const Membership& membership() const override { return members_; }
    std::optional<std::chrono::milliseconds> peer_latency(const NodeId&) const override {
        return std::nullopt;
    }
    void advertise_storage(uint64_t used, uint64_t capacity) override {
        members_.storage(used, capacity);
    }

    RpcReply call(const NodeInfo& peer, MessageType type, std::span<const uint8_t> payload,
                  FrameType frame_type) override {
        auto handler = place(peer, type, payload, frame_type);
        return {peer, answer(handler, type, Bytes(payload.begin(), payload.end()), frame_type)};
    }

    AsyncRpc call_async(const NodeInfo& peer, MessageType type, std::span<const uint8_t> payload,
                        FrameType frame_type) override {
        auto handler = place(peer, type, payload, frame_type);
        auto promise = std::make_shared<std::promise<RpcReply>>();
        auto future = promise->get_future();
        const auto started = Clock::now();
        auto fail = [promise] {
            try {
                promise->set_exception(
                    std::make_exception_ptr(std::runtime_error("RPC cancelled")));
            } catch (const std::future_error&) {
            }
        };
        std::lock_guard lock(mutex_);
        threads_.emplace_back([promise, peer, handler = std::move(handler), type, frame_type,
                               request = Bytes(payload.begin(), payload.end())] {
            try {
                auto reply = answer(handler, type, request, frame_type);
                promise->set_value({peer, std::move(reply)});
            } catch (const std::future_error&) {
            } catch (...) {
                try {
                    promise->set_exception(std::current_exception());
                } catch (const std::future_error&) {
                }
            }
        });
        return AsyncRpc(std::move(future), fail, fail, {}, [started] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        });
    }

  private:
    static RpcMessage answer(const Handler& handler, MessageType type, const Bytes& payload,
                             FrameType frame_type) {
        if (!handler)
            return {MessageType::ok, {}};
        return handler(type, payload, frame_type);
    }

    Handler place(const NodeInfo& peer, MessageType type, std::span<const uint8_t> payload,
                  FrameType frame_type) {
        std::lock_guard lock(mutex_);
        calls_.push_back({peer.id, type, frame_type, payload.size()});
        if (refused_.contains(peer.id))
            throw std::runtime_error("RPC to " + peer.host + " cannot be placed");
        const auto found = handlers_.find(peer.id);
        if (found == handlers_.end())
            throw std::runtime_error("RPC to an unknown peer");
        return found->second;
    }

    const Config config_;
    const NodeId self_id_;
    const NodeId epoch_;
    Membership members_;
    mutable std::mutex mutex_;
    std::map<NodeId, Handler> handlers_;
    std::set<NodeId> refused_;
    std::vector<Call> calls_;
    std::vector<std::jthread> threads_;
};

// One node's object storage with no transport: a FakeClusterNode, the
// node-wide resources and recovered local stores, built as the root builds
// them. `tune` adjusts the configuration first.
class StoreBench {
  public:
    explicit StoreBench(const std::function<void(Config&)>& tune = {})
        : config_(configure(cluster_, tune)), resources(config_),
          identity_(config_.state_path, cluster_.keys()),
          node(config_, self_info(config_, identity_.id), identity_.durability_epoch),
          local(config_, identity_, node, progress_, {}, std::stop_token{},
                test_durability_window) {
        node.advertise_storage(local.data().used(), local.data().limit());
    }
    ~StoreBench() { resources.stop(); }

    const Config& config() const { return config_; }
    const ClusterKeys& keys() const { return cluster_.keys(); }
    // A DistributedStore over this node, as the root builds one.
    std::unique_ptr<DistributedStore> store(DistributedStoreOptions options = {}) {
        return std::make_unique<DistributedStore>(node, local, resources.activity, resources.data,
                                                  resources.memory, resources.events,
                                                  std::move(options));
    }
    // A peer with `capacity` bytes, `used` of them, in a failure domain of its own.
    static NodeInfo peer(uint64_t capacity = 64ULL * 1024 * 1024 * 1024, uint64_t used = 0) {
        NodeInfo info;
        info.id = random_node_id();
        info.host = "peer-" + to_string(info.id).substr(0, 8);
        info.failure_domain = info.host;
        info.port = 9;
        info.capacity = capacity;
        info.used = used;
        info.seen_unix_ms = unix_ms();
        return info;
    }

  private:
    static Config configure(const TestCluster& cluster, const std::function<void(Config&)>& tune) {
        auto config = config_for(cluster.path() / "node", cluster.keyfile(), 1);
        config.replication = 1;
        config.write_copies = 1;
        config.metadata_write_copies = 1;
        if (tune)
            tune(config);
        return normalize_config(std::move(config));
    }
    static NodeInfo self_info(const Config& config, const NodeId& id) {
        NodeInfo self;
        self.id = id;
        self.host = config.advertise_host;
        self.port = config.port;
        self.failure_domain = "local";
        self.seen_unix_ms = unix_ms();
        return self;
    }

    TestCluster cluster_;
    Config config_;

  public:
    NodeResources resources;

  private:
    NodeIdentity identity_;
    RecoveryProgress progress_;

  public:
    FakeClusterNode node;
    LocalState local;
};

// The reply a peer gives to have_valid_objects: `holds` for every id asked.
inline RpcMessage presence_reply(const Bytes& request, bool holds) {
    Reader reader(request);
    const auto count = reader.u32();
    Writer writer;
    writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        (void)reader.fixed<32>();
        writer.u8(holds ? 1 : 0);
    }
    return {MessageType::have_valid_objects_reply, writer.take()};
}

// The reply a peer gives to get_object for `bytes`.
inline RpcMessage object_reply(std::span<const uint8_t> bytes) {
    Writer writer;
    writer.fixed(object_id(bytes).bytes);
    writer.bytes(bytes);
    return {MessageType::object_reply, writer.take()};
}

} // namespace macha::test_support

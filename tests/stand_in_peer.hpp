// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A peer the node under test meets over the real transport: Dannan's node with no
// Macha behind it. Its id, place, adverts, telemetry and answers are the test's, so a
// test that needs a peer of a given shape has one the node learns of the way it
// learns of any other, by meeting it.

#include "cluster/dannan_bridge.hpp"
#include "cluster/dannan_host.hpp"
#include "cluster/telemetry_bridge.hpp"
#include "test_support.hpp"

#include <dannan/identity/cluster_key.h>

#include <fstream>
#include <functional>
#include <memory>
#include <optional>

namespace macha::test_support {

class StandInPeer {
  public:
    struct Options {
        // Whom it dials first; it is dialled as well, so either way they meet.
        std::vector<Endpoint> bootstrap{};
        // The id it runs under; random when none.
        std::optional<NodeId> id{};
        std::string failure_domain{};
        std::string node_name{};
        // Its record's fields beyond Dannan's own, written at every refresh. A long
        // interval keeps the record to itself until refreshed: its first goes before
        // it has met anyone.
        std::function<void(dannan::status::Telemetry&)> telemetry{};
        std::chrono::milliseconds telemetry_interval{250};
        bool hosts_extents{true};
    };

    StandInPeer(const TestCluster& cluster, const std::string& name, Options options)
        : StandInPeer(cluster.path(), cluster.keyfile(), name, std::move(options)) {}

    // Its state under `root`, in the cluster whose key is `keyfile`.
    StandInPeer(const std::filesystem::path& root, const std::filesystem::path& keyfile,
                const std::string& name, Options options)
        : port_(free_port()) {
        auto config = config_for(root / name, keyfile, port_, options.bootstrap,
                                 ConfigProfile::isolated);
        if (!options.failure_domain.empty())
            config.failure_domain = options.failure_domain;
        config.node_name = options.node_name;
        std::filesystem::create_directories(config.state_path);
        if (options.id) {
            std::ofstream out(config.state_path / "node-id");
            out << to_string(*options.id) << "\n";
        }
        auto node = dannan_node_config(config, dannan::identity::load_cluster_key(keyfile));
        node.version = "stand-in";
        node.telemetry_interval = options.telemetry_interval;
        node.telemetry_layout = hosted_telemetry_layout();
        node.telemetry_fields = std::move(options.telemetry);
        node.log = macha_log_sink();
        host_ = std::make_unique<DannanHost>(std::move(node), routes, 2, DannanHost::MetadataNotice{});
        if (options.hosts_extents)
            host_->node().nodes().advertise(advert::hosts_extents, dannan::net::Bytes{1});
    }

    ~StandInPeer() {
        if (host_)
            host_->stop();
    }
    StandInPeer(const StandInPeer&) = delete;
    StandInPeer& operator=(const StandInPeer&) = delete;

    void start() { host_->start(); }
    void stop() { host_->stop(); }

    // Its storage figures, as Macha's adverts carry them.
    void advertise_storage(uint64_t capacity, uint64_t used) {
        auto& nodes = host_->node().nodes();
        nodes.advertise(advert::capacity, dannan::membership::u64_advert(capacity));
        nodes.advertise(advert::used, dannan::membership::u64_advert(used));
    }
    // Any of Macha's u64 adverts (dannan_bridge.hpp's advert names).
    void advertise(const std::string& key, uint64_t value) {
        host_->node().nodes().advertise(key, dannan::membership::u64_advert(value));
    }

    // Calls from the stand-in, as any node places them.
    AsyncRpc call_async(const Endpoint& endpoint, MessageType type, std::span<const uint8_t> payload,
                        FrameType frame, std::chrono::milliseconds deadline) {
        return host_->call_async(endpoint, type, payload, frame, deadline);
    }
    RpcReply call(const Endpoint& endpoint, MessageType type, std::span<const uint8_t> payload,
                  FrameType frame, std::chrono::milliseconds deadline) {
        return call_async(endpoint, type, payload, frame, deadline).get();
    }

    NodeId id() const { return macha_id(host_->node().nodes().self().id); }
    NodeInfo info() const { return node_info_from(host_->node().nodes().self()); }
    Endpoint endpoint() const { return {"127.0.0.1", port_}; }
    dannan::Node& node() { return host_->node(); }

    // Answers for the types the test binds; any other is refused 501.
    MessageRoutes routes;

  private:
    uint16_t port_;
    std::unique_ptr<DannanHost> host_;
};

// Whether `node` counts `peer` among its active nodes.
inline bool active_on(const ClusterNode& node, const NodeId& peer) {
    const auto active = node.membership().active();
    return std::any_of(active.begin(), active.end(),
                       [&](const NodeInfo& info) { return info.id == peer; });
}

} // namespace macha::test_support

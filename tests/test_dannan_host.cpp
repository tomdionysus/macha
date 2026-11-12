// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dannan's node hosted for Macha: two hosts in one process over loopback. A call reaches
// the handler bound in the peer's MessageRoutes and its reply comes back; an unbound
// type is answered 501; a metadata notice reaches its own handler on the session.
#include "cluster/dannan_bridge.hpp"
#include "cluster/dannan_host.hpp"
#include "cluster/rpc_refusal.hpp"
#include "cluster/telemetry_bridge.hpp"
#include "stand_in_peer.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include <dannan/identity/cluster_key.h>

#include <atomic>
#include <mutex>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

dannan::NodeConfig host_config(const std::filesystem::path& state, const std::filesystem::path& key,
                               uint16_t port, std::vector<dannan::net::Endpoint> bootstrap = {}) {
    Config config;
    config.state_path = state;
    config.listen_host = "127.0.0.1";
    config.port = port;
    auto node = dannan_node_config(config, dannan::identity::load_cluster_key(key));
    node.bootstrap = std::move(bootstrap);
    node.version = "test";
    return node;
}

} // namespace

MACHA_TEST("dannan_host", test_a_call_reaches_the_peers_route_and_answers) {
    TempDir dir;
    const auto key = dir.path() / "cluster.key";
    write_key(key);
    const auto port_a = free_port();
    const auto port_b = free_port();

    MessageRoutes routes_a, routes_b;
    std::mutex notices_mutex;
    std::vector<uint64_t> notices;
    routes_b.bind(MessageType::get_ingest_jobs,
                  [](const NodeInfo& peer, FrameType frame, const RpcMessage& request) {
                      Writer writer;
                      writer.fixed(peer.id.bytes);
                      writer.u8(static_cast<uint8_t>(frame));
                      writer.bytes(request.payload);
                      return RpcMessage(MessageType::ingest_jobs_reply, writer.take());
                  });

    DannanHost a(host_config(dir.path() / "a", key, port_a), routes_a, 2, {});
    DannanHost b(host_config(dir.path() / "b", key, port_b, {{"127.0.0.1", port_a}}), routes_b, 2,
                 [&](const NodeInfo&, const RpcMessage& notice) {
                     Reader reader(notice.payload);
                     std::lock_guard lock(notices_mutex);
                     notices.push_back(reader.u64());
                 });
    a.start();
    b.start();

    const auto peer_b = node_info_from(b.node().nodes().self());
    const Bytes payload{1, 2, 3, 4};
    auto reply =
        a.call_async(peer_b, MessageType::get_ingest_jobs, payload, FrameType::control, 30s).get();
    REQUIRE(reply.message.type == MessageType::ingest_jobs_reply);
    Reader reader(reply.message.payload);
    REQUIRE(reader.fixed<16>() == a.node().nodes().self().id.bytes);
    REQUIRE(reader.u8() == static_cast<uint8_t>(FrameType::control));
    REQUIRE(reader.bytes(64) == payload);
    REQUIRE(reply.peer.id.bytes == peer_b.id.bytes);

    // A type registered but bound nowhere is refused 501, not lost.
    auto refused =
        a.call_async(peer_b, MessageType::get_torrent_jobs, {}, FrameType::control, 30s).get();
    REQUIRE(refused.message.type == MessageType::error);
    const auto refusal = rpc_refusal(refused.message);
    REQUIRE(refusal.has_value());
    REQUIRE(refusal->status == 501);

    // The metadata notice reaches its own handler, ahead of every queue.
    Writer notice;
    notice.u64(168518);
    a.node().channels().broadcast(
        dannan_message(RpcMessage(MessageType::metadata_notice, notice.take())));
    REQUIRE(wait_until(
        [&] {
            std::lock_guard lock(notices_mutex);
            return !notices.empty();
        },
        10s));
    {
        std::lock_guard lock(notices_mutex);
        REQUIRE(notices.front() == 168518);
    }

    // Stopping waits on nothing either host still holds.
    b.stop();
    a.stop();
}

// Membership following Dannan's roster: the peer that bootstrapped is active and directly
// reachable, this node's own entry is Membership's with its figures, and a peer handed to
// observe() changes nothing, the roster being the node's.
MACHA_TEST("dannan_host", test_membership_follows_the_nodes_roster) {
    TempDir dir;
    const auto key = dir.path() / "cluster.key";
    write_key(key);
    const auto port_a = free_port();
    const auto port_b = free_port();
    MessageRoutes routes_a, routes_b;
    DannanHost a(host_config(dir.path() / "a", key, port_a), routes_a, 2, {});
    DannanHost b(host_config(dir.path() / "b", key, port_b, {{"127.0.0.1", port_a}}), routes_b, 2,
                 {});
    a.start();
    b.start();

    auto self = node_info_from(a.node().nodes().self());
    self.capacity = 1000;
    Membership members(self, 30s);
    members.follow(
        std::make_shared<DannanMembership>(a.node().nodes(), a.node().nodes().self().id));
    const auto peer_b = macha_id(b.node().nodes().self().id);
    REQUIRE(wait_until(
        [&] {
            const auto active = members.active();
            return active.size() == 2 && active[1].id == peer_b;
        },
        10s));
    REQUIRE(members.active().front().id == self.id);
    REQUIRE(members.active().front().capacity == 1000);
    REQUIRE(members.all().size() == 2);
    REQUIRE(wait_until([&] { return members.directly_reachable(peer_b); }, 10s));
    REQUIRE(members.directly_reachable(self.id));
    REQUIRE(wait_until([&] { return members.all_known_reachable(); }, 10s));

    NodeInfo stranger;
    stranger.id.bytes.fill(9);
    stranger.host = "198.51.100.1";
    stranger.port = 7437;
    stranger.seen_unix_ms = unix_ms();
    members.observe(stranger, true);
    REQUIRE(members.all().size() == 2);

    b.stop();
    a.stop();
}

// TelemetryStore following Dannan's records: the peer's hosted fields, written by its
// telemetry_fields, arrive and read back as Macha's NodeTelemetry.
MACHA_TEST("dannan_host", test_telemetry_follows_the_nodes_records) {
    TempDir dir;
    const auto key = dir.path() / "cluster.key";
    write_key(key);
    const auto port_a = free_port();
    const auto port_b = free_port();
    MessageRoutes routes_a, routes_b;
    auto config_a = host_config(dir.path() / "a", key, port_a);
    config_a.telemetry_interval = 250ms;
    auto config_b = host_config(dir.path() / "b", key, port_b, {{"127.0.0.1", port_a}});
    config_b.telemetry_interval = 250ms;
    config_b.telemetry_layout = hosted_telemetry_layout();
    config_b.telemetry_fields = [](dannan::status::Telemetry& record) {
        HostedTelemetry hosted;
        hosted.storage_capacity = 9895604649984ULL;
        hosted.storage_used = 1835376108947ULL;
        hosted.api_endpoint = "http://192.168.1.50:7438";
        write_hosted_telemetry(record, hosted);
    };
    DannanHost a(std::move(config_a), routes_a, 2, {});
    DannanHost b(std::move(config_b), routes_b, 2, {});
    a.start();
    b.start();

    TelemetryStore store(std::make_shared<DannanTelemetry>(a.node().telemetry()));
    const auto peer_b = macha_id(b.node().nodes().self().id);
    std::optional<NodeTelemetry> seen;
    REQUIRE(wait_until(
        [&] {
            for (const auto& view : store.views(10s))
                if (view.telemetry.node_id == peer_b && view.telemetry.storage_capacity) {
                    seen = view.telemetry;
                    return true;
                }
            return false;
        },
        15s));
    REQUIRE(seen->storage_capacity == 9895604649984ULL);
    REQUIRE(seen->storage_used == 1835376108947ULL);
    REQUIRE(seen->api_endpoint == "http://192.168.1.50:7438");
    REQUIRE(seen->version == "test");
    REQUIRE(store.local().has_value());
    REQUIRE(store.local()->node_id == macha_id(a.node().nodes().self().id));

    b.stop();
    a.stop();
}

// A call whose deadline passes fails with the reason, and the host stops with calls
// still pending.
MACHA_TEST("dannan_host", test_a_call_past_its_deadline_fails_and_stop_does_not_wait) {
    TempDir dir;
    const auto key = dir.path() / "cluster.key";
    write_key(key);
    const auto port_a = free_port();
    const auto port_b = free_port();
    MessageRoutes routes_a, routes_b;
    std::atomic_bool release{};
    routes_b.bind(MessageType::get_ingest_jobs, [&](const NodeInfo&, FrameType, const RpcMessage&) {
        wait_until([&] { return release.load(); }, 20s);
        return RpcMessage(MessageType::ingest_jobs_reply, {});
    });
    DannanHost a(host_config(dir.path() / "a", key, port_a), routes_a, 2, {});
    DannanHost b(host_config(dir.path() / "b", key, port_b), routes_b, 2, {});
    a.start();
    b.start();
    const auto peer_b = node_info_from(b.node().nodes().self());
    auto slow = a.call_async(peer_b, MessageType::get_ingest_jobs, {}, FrameType::control, 300ms);
    // Idle from when the call was made, placed or not: never the "not placed" maximum.
    REQUIRE(slow.idle_for() < 300ms);
    bool failed = false;
    try {
        (void)slow.get();
    } catch (const std::exception&) {
        failed = true;
    }
    REQUIRE(failed);
    release = true;
    b.stop();
    a.stop();
}

// What Status reads of the node: its threads under Macha's names, those with no Macha
// counterpart left out; and the routes usable now in the local telemetry record, a core
// field the node leaves unwritten.
MACHA_TEST("dannan_host", test_status_reads_the_nodes_threads_and_canonical_routes) {
    TestCluster cluster;
    const auto port_a = free_port();
    const auto port_b = free_port();
    auto config_a = config_for(cluster.path() / "a", cluster.keyfile(), port_a);
    auto config_b =
        config_for(cluster.path() / "b", cluster.keyfile(), port_b, {{"127.0.0.1", port_a}});
    config_a.telemetry_interval = 250ms;
    BareNode a(config_a, cluster.keys());
    BareNode b(config_b, cluster.keys());
    a.start();
    b.start();

    const auto named = [&](std::string_view name) {
        const auto threads = a.thread_statuses();
        return std::any_of(threads.begin(), threads.end(),
                           [&](const SupervisedThreadStatus& t) { return t.name == name; });
    };
    REQUIRE(wait_until([&] { return named("cluster-telemetry") && named("cluster-maintenance"); },
                       10s));
    REQUIRE(named("cluster-runtime"));
    REQUIRE(!named("cluster-roster"));

    REQUIRE(wait_until(
        [&] {
            const auto local = a.telemetry().local();
            return local && local->rpc_connections_canonical > 0;
        },
        15s));
}

// Job traffic keeps its class from Macha's caller to the peer's handler: polls travel
// as loader and actions as foreground, as Macha's registrations admit them.
MACHA_TEST("dannan_host", test_job_traffic_travels_as_its_own_class) {
    TestCluster cluster;
    std::mutex seen_mutex;
    std::vector<FrameType> seen;
    StandInPeer peer(cluster, "jobs-peer", {});
    const auto record = [&](FrameType frame) {
        std::lock_guard lock(seen_mutex);
        seen.push_back(frame);
    };
    peer.routes.bind(MessageType::get_torrent_jobs,
                     [&](const NodeInfo&, FrameType frame, const RpcMessage&) {
                         record(frame);
                         return RpcMessage{MessageType::torrent_jobs_reply, {}};
                     });
    peer.routes.bind(MessageType::ingest_job_action,
                     [&](const NodeInfo&, FrameType frame, const RpcMessage&) {
                         record(frame);
                         return RpcMessage{MessageType::ingest_job_action_reply, {}};
                     });
    peer.start();
    BareNode node(cluster.node_config("jobs-caller"), cluster.keys());
    node.start();

    const auto polled = node.call(peer.endpoint(), MessageType::get_torrent_jobs, {},
                                  FrameType::loader, call_deadline::control);
    REQUIRE(polled.message.type == MessageType::torrent_jobs_reply);
    const Bytes action{'{', '}'};
    const auto acted = node.call(peer.endpoint(), MessageType::ingest_job_action, action,
                                 FrameType::foreground, call_deadline::control);
    REQUIRE(acted.message.type == MessageType::ingest_job_action_reply);
    std::lock_guard lock(seen_mutex);
    REQUIRE(seen == std::vector<FrameType>({FrameType::loader, FrameType::foreground}));
}

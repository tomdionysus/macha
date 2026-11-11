// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dannan's node hosted for Macha: two hosts in one process over loopback. A call reaches
// the handler bound in the peer's MessageRoutes and its reply comes back; an unbound
// type is answered 501; a metadata notice reaches its own handler on the session.
#include "cluster/dannan_bridge.hpp"
#include "cluster/dannan_host.hpp"
#include "cluster/rpc_refusal.hpp"
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
    auto reply = a.call_async(peer_b, MessageType::get_ingest_jobs, payload, FrameType::control, 30s)
                     .get();
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
    a.node().channels().broadcast(dannan_message(RpcMessage(MessageType::metadata_notice, notice.take())));
    REQUIRE(wait_until([&] {
        std::lock_guard lock(notices_mutex);
        return !notices.empty();
    }, 10s));
    {
        std::lock_guard lock(notices_mutex);
        REQUIRE(notices.front() == 168518);
    }

    // Stopping waits on nothing either host still holds.
    b.stop();
    a.stop();
}

// Membership following Dannan's roster: the peer that bootstrapped is active and directly
// reachable, this node's own entry is Membership's with its figures, and gossip handed to
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
    members.follow(std::make_shared<DannanMembership>(a.node().nodes(), a.node().nodes().self().id));
    const auto peer_b = macha_id(b.node().nodes().self().id);
    REQUIRE(wait_until([&] {
        const auto active = members.active();
        return active.size() == 2 && active[1].id == peer_b;
    }, 10s));
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
    REQUIRE(members.forget_unseen() == 0);

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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "metadata/namespace_control_store.hpp"
#include "cluster/placement.hpp"
#include "startup_progress.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_FAST_TEST("rpc_cluster", test_rpc_reassembly_has_count_byte_and_message_bounds) {
    MessageAssembler assembler(2, 8, 16);
    auto fragment = [](uint64_t request, bool first, bool last, size_t bytes) {
        return WireFragment{request, FrameType::foreground, MessageType::put_object, first,
                            last,    Bytes(bytes, 0x5a)};
    };

    CHECK(!assembler.push(fragment(1, true, false, 3)).has_value());
    CHECK(!assembler.push(fragment(2, true, false, 3)).has_value());
    CHECK(assembler.incomplete_messages() == 2);
    CHECK(assembler.incomplete_bytes() == 6);

    bool count_rejected = false;
    try {
        (void)assembler.push(fragment(3, true, false, 1));
    } catch (...) {
        count_rejected = true;
    }
    CHECK(count_rejected);
    CHECK(assembler.incomplete_messages() == 2);

    bool aggregate_rejected = false;
    try {
        (void)assembler.push(fragment(1, false, false, 3));
    } catch (...) {
        aggregate_rejected = true;
    }
    CHECK(aggregate_rejected);
    CHECK(assembler.incomplete_bytes() == 6);

    assembler.discard(2);
    CHECK(assembler.incomplete_messages() == 1);
    CHECK(assembler.incomplete_bytes() == 3);
    auto complete = assembler.push(fragment(1, false, true, 0));
    REQUIRE(complete.has_value());
    CHECK(complete->message.payload.size() == 3);
    CHECK(assembler.incomplete_messages() == 0);
    CHECK(assembler.incomplete_bytes() == 0);

    MessageAssembler message_bound(4, 64, 4);
    CHECK(!message_bound.push(fragment(9, true, false, 3)).has_value());
    bool message_rejected = false;
    try {
        (void)message_bound.push(fragment(9, false, true, 2));
    } catch (...) {
        message_rejected = true;
    }
    CHECK(message_rejected);
    CHECK(message_bound.incomplete_bytes() == 3);
}

MACHA_FAST_TEST("rpc_cluster", test_rpc_reassembly_is_process_memory_charged_until_consumed) {
    RetainedMemoryLedger memory(4096, 512, 1024, 512);
    MessageAssembler assembler(4, 2048, 2048, &memory);
    auto fragment = [](uint64_t request, bool first, bool last, size_t bytes,
                       FrameType frame_type = FrameType::loader) {
        return WireFragment{request, frame_type, MessageType::put_object, first, last,
                            Bytes(bytes, 0x5a)};
    };

    CHECK(!assembler.push(fragment(1, true, false, 700)).has_value());
    const auto partial = memory.stats().owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)];
    CHECK(partial >= 700);

    auto complete = assembler.push(fragment(1, false, true, 300));
    REQUIRE(complete.has_value());
    CHECK(complete->message.payload.size() == 1000);
    CHECK(memory.stats().owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] >= 1000);

    auto delivered = std::move(complete->message);
    complete.reset();
    CHECK(memory.stats().owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] >= 1000);
    delivered = {};
    CHECK(memory.stats().owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] == 0);

    // Reassembly is bounded at the non-control capacity, not the durable-lower
    // budget: a write's confirmation must be reassembled while publication
    // holds that budget. The ceiling is 4096 minus the control reserve (3584),
    // so 2000 + 1800 is refused.
    MessageAssembler saturated(4, 4096, 4096, &memory);
    CHECK(!saturated.push(fragment(2, true, false, 2000)).has_value());
    bool rejected = false;
    try {
        (void)saturated.push(fragment(3, true, false, 1800));
    } catch (...) {
        rejected = true;
    }
    CHECK(rejected);
}

MACHA_TEST("rpc_cluster", test_async_rpc_move_ownership) {
    std::atomic_int cancelled{};

    // Moving an AsyncRpc transfers cancellation ownership; the moved-from
    // object's destruction is inert (libc++ may leave a moved-from std::function non-empty).
    std::optional<AsyncRpc> moved;
    {
        std::promise<RpcReply> promise;
        AsyncRpc original(promise.get_future(), [&] { ++cancelled; }, {});
        moved.emplace(std::move(original));
    }
    CHECK(cancelled.load() == 0);
    moved.reset();
    CHECK(cancelled.load() == 1);

    // Move assignment cancels the target's request and leaves the source inert.
    std::promise<RpcReply> first_promise;
    std::promise<RpcReply> second_promise;
    AsyncRpc first(first_promise.get_future(), [&] { ++cancelled; }, {});
    {
        AsyncRpc second(second_promise.get_future(), [&] { ++cancelled; }, {});
        first = std::move(second);
        CHECK(cancelled.load() == 2);
    }
    CHECK(cancelled.load() == 2);
}

MACHA_TEST("rpc_cluster", test_best_effort_telemetry_notifications_reach_both_route_directions) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto local_port = free_port();
    const auto remote_port = free_port();

    NodeInfo local_info;
    local_info.id = random_node_id();
    local_info.host = "127.0.0.1";
    local_info.port = local_port;
    NodeInfo remote_info;
    remote_info.id = random_node_id();
    remote_info.host = "127.0.0.1";
    remote_info.port = remote_port;

    std::atomic_uint64_t local_received{};
    std::atomic_uint64_t remote_received{};
    auto handler = [](std::atomic_uint64_t& received, const RpcMessage& request) {
        if (request.type == MessageType::telemetry) {
            const auto values = decode_telemetry_set(request.payload);
            if (!values.empty())
                received.fetch_add(1, std::memory_order_relaxed);
            return RpcMessage{MessageType::telemetry_reply, {}};
        }
        return RpcMessage{MessageType::ok, {}};
    };

    RpcClient local_client(
        keys, [local_info] { return local_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        5s, 30s, 4096);
    RpcClient remote_client(
        keys, [remote_info] { return remote_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        5s, 30s, 4096);
    RpcServer local_server(
        "127.0.0.1", local_port, keys, local_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            return handler(local_received, request);
        },
        [](const NodeInfo&) {}, 4096);
    RpcServer remote_server(
        "127.0.0.1", remote_port, keys, remote_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            return handler(remote_received, request);
        },
        [](const NodeInfo&) {}, 4096);
    local_server.attach_client(local_client);
    remote_server.attach_client(remote_client);
    local_server.start();
    remote_server.start();

    const Endpoint local_endpoint{"127.0.0.1", local_port};
    REQUIRE(remote_client.call(local_endpoint, MessageType::ping, {}, 1s).message.type ==
            MessageType::ok);

    NodeTelemetry telemetry;
    telemetry.node_id = remote_info.id;
    telemetry.boot_id = random_node_id();
    telemetry.sequence = 1;
    telemetry.observed_unix_ms = unix_ms();
    telemetry.host = remote_info.host;
    telemetry.port = remote_info.port;
    const RpcMessage notice{MessageType::telemetry, encode_telemetry_set({telemetry})};

    // Telemetry is gossiped as SPECULATIVE. Dialler to acceptor: RpcServer::session_loop.
    REQUIRE(wait_until(
        [&] {
            (void)remote_client.broadcast_best_effort(notice, FrameType::speculative);
            return local_received.load(std::memory_order_relaxed) > 0;
        },
        2s));

    // Acceptor to dialler: PeerConnection::reader_loop. Route reconciliation
    // may retain either direction, so both must work.
    REQUIRE(wait_until(
        [&] {
            (void)local_client.broadcast_best_effort(notice, FrameType::speculative);
            return remote_received.load(std::memory_order_relaxed) > 0;
        },
        2s));

    // CONTROL is also a legal class for telemetry.
    const auto before = local_received.load(std::memory_order_relaxed);
    REQUIRE(wait_until(
        [&] {
            (void)remote_client.broadcast_best_effort(notice, FrameType::control);
            return local_received.load(std::memory_order_relaxed) > before;
        },
        2s));

    remote_client.stop();
    local_client.stop();
    remote_server.stop();
    local_server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_v15_frame_priority_and_variable_length) {
    CHECK(frame_type_priority(FrameType::control) < frame_type_priority(FrameType::foreground));
    CHECK(frame_type_priority(FrameType::foreground) < frame_type_priority(FrameType::read_ahead));
    CHECK(frame_type_priority(FrameType::read_ahead) < frame_type_priority(FrameType::loader));
    CHECK(frame_type_priority(FrameType::loader) < frame_type_priority(FrameType::speculative));
    CHECK(std::string(frame_type_name(FrameType::loader)) == "loader");
    CHECK(default_frame_type(MessageType::ping) == FrameType::control);
    CHECK(default_frame_type(MessageType::get_object) == FrameType::foreground);
    CHECK(default_frame_type(MessageType::get_control_object) == FrameType::speculative);
    CHECK(std::string(message_type_name(MessageType::put_metadata_commit)) ==
          "put_metadata_commit");
    CHECK(std::string(message_type_name(MessageType::accept_metadata_commit)) ==
          "accept_metadata_commit");
    CHECK(std::string(message_type_name(MessageType::get_metadata_heads)) == "get_metadata_heads");

    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    std::mutex order_mutex;
    std::vector<uint8_t> order;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object && !request.payload.empty()) {
                std::lock_guard lock(order_mutex);
                order.push_back(request.payload.front());
            }
            return RpcMessage{MessageType::ok, request.payload};
        },
        [](const NodeInfo&) {}, 4096);
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        5s, 30s, 4096);
    Endpoint endpoint{"127.0.0.1", port};

    // Not a multiple of max_frame_size: the final frame is short, not padded.
    auto odd = pattern(12'345);
    auto odd_reply = client.call(endpoint, MessageType::ping, odd, 1s);
    CHECK(odd_reply.message.payload == odd);

    auto metadata_reply =
        client.call(endpoint, MessageType::get_metadata, Bytes{0x4d}, FrameType::read_ahead, 2s);
    CHECK(metadata_reply.message.type == MessageType::ok);
    CHECK(metadata_reply.message.payload == Bytes{0x4d});

    // Control objects run at speculative priority but stay on the CONTROL
    // session, so catalogue bootstrap needs no DATA lane.
    auto control_object_reply = client.call(endpoint, MessageType::put_control_object,
                                            Bytes{0x43, 0x41, 0x54}, FrameType::speculative, 2s);
    CHECK(control_object_reply.message.type == MessageType::ok);
    CHECK(client.stats().canonical_connections == 1);

    // Loader is its own on-wire class, valid for bulk DATA.
    auto loader_reply =
        client.call(endpoint, MessageType::put_object, Bytes{0x4c}, FrameType::loader, 2s);
    CHECK(loader_reply.message.type == MessageType::ok);
    CHECK(loader_reply.message.payload == Bytes{0x4c});
    // The calls above leave a smoothed peer latency (commit fan-out orders
    // replicas by it); on loopback, well under a second.
    {
        const auto latency = client.peer_latency(server_info.id);
        REQUIRE(latency.has_value());
        CHECK(*latency < 1000ms);
        CHECK(client.peer_latencies().size() == 1);
    }
    {
        std::lock_guard lock(order_mutex);
        order.clear();
    }

    // The writer reconsiders priority after every frame (<= 4 KiB), so
    // foreground work overtakes a large speculative transfer.
    Bytes speculative(32 * 1024 * 1024, 0x53);
    auto background =
        client.call_async(endpoint, MessageType::put_object, speculative, FrameType::speculative);
    auto foreground =
        client.call_async(endpoint, MessageType::put_object, Bytes{0x46}, FrameType::foreground);
    REQUIRE(foreground.wait_for(2s) == std::future_status::ready);
    CHECK(foreground.get().message.type == MessageType::ok);
    {
        std::lock_guard lock(order_mutex);
        REQUIRE(!order.empty());
        CHECK(order.front() == 0x46);
    }
    REQUIRE(background.wait_for(10s) == std::future_status::ready);
    CHECK(background.get().message.type == MessageType::ok);
    CHECK(client.stats().canonical_connections == 2);
    const auto work = server.work_stats();
    REQUIRE(work.frame_timings.contains(FrameType::loader));
    CHECK(work.frame_timings.at(FrameType::loader).requests >= 1);

    // A transfer cancelled while one of its frames is outside the outbound
    // deque is not requeued; the peer would otherwise see continuation frames
    // after cancel_transfer and tear down the connection.
    Bytes cancelled_payload(32 * 1024 * 1024, 0x43);
    auto cancelled = client.call_async(endpoint, MessageType::put_object, cancelled_payload,
                                       FrameType::speculative);
    std::this_thread::sleep_for(2ms);
    cancelled.cancel();
    auto after_cancel = client.call(endpoint, MessageType::ping, Bytes{0x50}, 2s);
    CHECK(after_cancel.message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 2);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_loader_put_does_not_signal_viewer_activity) {
    TestNode fixture("loader-activity-class");
    auto& config = fixture.config();
    config.replication = 1;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    auto& node = fixture.start();

    // Clear startup accounting; a loader write must not refresh the viewer clock.
    (void)node.resources.activity.take_bytes(FrameType::read_ahead);
    (void)node.resources.activity.take_bytes(FrameType::foreground);
    (void)node.resources.activity.take_bytes(FrameType::loader);
    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    auto bytes = pattern(256 * 1024, 91);
    REQUIRE(store.put(bytes) == object_id(bytes));
    CHECK(node.resources.activity.take_bytes(FrameType::read_ahead) == 0);
    CHECK(node.resources.activity.take_bytes(FrameType::foreground) == 0);
}

MACHA_TEST("rpc_cluster", test_a_loader_write_is_visible_to_maintenance_as_its_own_class) {
    // A loader write is not a viewer, but it must refresh the loader clock:
    // maintenance ranks below loader work and judges idleness from these clocks.
    TestNode fixture("loader-activity-clock");
    auto& config = fixture.config();
    config.replication = 1;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    auto& node = fixture.start();

    (void)node.resources.activity.take_bytes(FrameType::loader);
    (void)node.resources.activity.take_bytes(FrameType::foreground);
    (void)node.resources.activity.take_bytes(FrameType::read_ahead);
    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    auto bytes = pattern(256 * 1024, 91);
    REQUIRE(store.put(bytes) == object_id(bytes));

    // Seen, as its own class, with the viewer clocks untouched.
    CHECK(store.loader_idle_for() < 5s);
    CHECK(store.take_loader_bytes() >= bytes.size());
    CHECK(store.take_foreground_bytes() == 0);
    CHECK(store.take_interactive_bytes() == 0);

    // Not a viewer to the DATA pressure gate or the torrent rate clamp.
    CHECK(!fixture.resources().activity.viewer_recently_active(30s));
}

MACHA_TEST("rpc_cluster", test_concurrent_object_fetch_waiters_share_one_retained_buffer) {
    TestNode fixture("shared-object-buffer", ConfigProfile::functional);
    auto& config = fixture.config();
    config.replication = 2;
    config.metadata_min_write_replicas = 1;
    config.heartbeat = 30s;
    auto& node = fixture.start();

    const auto bytes = pattern(512 * 1024, 73);
    const auto id = object_id(bytes);
    const auto port = free_port();
    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "127.0.0.1";
    peer.port = port;
    peer.failure_domain = "remote";
    peer.capacity = 1024ULL * 1024 * 1024;
    peer.seen_unix_ms = unix_ms();

    TestGate reply_gate;
    std::atomic_uint fetches{};
    RpcServer server(
        "127.0.0.1", port, fixture.keys(), peer,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::get_object) {
                ++fetches;
                reply_gate.enter_and_wait();
                Writer writer;
                writer.fixed(id.bytes);
                writer.bytes(bytes);
                return RpcMessage{MessageType::object_reply, writer.take()};
            }
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {}, 4ULL * 1024 * 1024, {}, &node.resources.memory);
    server.start();
    node.membership().observe(peer, true);

    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    auto first = std::async(std::launch::async, [&] {
        return store.get_shared(id, 0, FrameType::foreground);
    });
    REQUIRE(reply_gate.wait_for_entries(1));
    auto second = std::async(std::launch::async, [&] {
        return store.get_shared(id, 0, FrameType::foreground);
    });
    std::this_thread::sleep_for(20ms);
    reply_gate.open();

    auto a = first.get();
    auto b = second.get();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() == b.get());
    CHECK(a->bytes == bytes);
    CHECK(fetches.load() == 1);

    const auto held = node.resources.memory.stats()
                          .owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)];
    CHECK(held >= bytes.size());
    a.reset();
    CHECK(node.resources.memory.stats()
              .owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] >= bytes.size());
    b.reset();
    REQUIRE(wait_until([&] {
        return node.resources.memory.stats()
                   .owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] < bytes.size();
    }));

    server.stop();
}

MACHA_TEST("rpc_cluster", test_repair_does_not_push_to_a_peer_with_no_room) {
    // A peer that advertises no room for an extent is not a push target.
    TestNode fixture("repair-full-peer", ConfigProfile::functional);
    auto& config = fixture.config();
    config.replication = 2;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    config.heartbeat = 30s;
    auto& node = fixture.start();

    std::atomic_uint probes{};
    std::atomic_uint puts{};
    const auto port = free_port();
    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "127.0.0.1";
    peer.port = port;
    peer.failure_domain = "remote";
    peer.capacity = 10ULL * 1024 * 1024 * 1024;
    peer.used = peer.capacity - 81;
    peer.seen_unix_ms = unix_ms();
    RpcServer server(
        "127.0.0.1", port, fixture.keys(), peer,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::have_object) {
                ++probes;
                Writer writer;
                writer.u8(0);
                return RpcMessage{MessageType::bool_reply, writer.take()};
            }
            if (request.type == MessageType::put_object) ++puts;
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {}, 4ULL * 1024 * 1024, {}, &node.resources.memory);
    server.start();
    node.membership().observe(peer, true);

    const auto bytes = pattern(256 * 1024, 31);
    const auto id = object_id(bytes);
    REQUIRE(node.local_store().put(id, bytes));
    const std::vector<ObjectId> live{id};
    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    for (int pass = 0; pass < 4; ++pass)
        (void)store.repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(probes.load() == 0);
    CHECK(puts.load() == 0);
    // Never dropped for want of a second copy.
    CHECK(node.local_store().has(id));

    // With room, the same peer is pushed to.
    peer.used = 0;
    peer.seen_unix_ms = unix_ms();
    node.membership().observe(peer, true);
    for (int pass = 0; pass < 4 && puts.load() == 0; ++pass)
        (void)store.repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(puts.load() > 0);
    server.stop();
}

MACHA_TEST("rpc_cluster", test_repair_probes_without_credit_and_transfers_only_with_it) {
    // Probing is paid for by the operation budget; credit pays only for a
    // transfer's bytes.
    TestNode fixture("repair-credit-gates-transfers", ConfigProfile::functional);
    auto& config = fixture.config();
    config.replication = 2;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    config.heartbeat = 30s;
    auto& node = fixture.start();

    std::atomic_uint probes{};
    std::atomic_uint puts{};
    std::atomic_uint fetches{};
    std::atomic_bool peer_holds_held{};
    const auto held_bytes = pattern(256 * 1024, 41);
    const auto held = object_id(held_bytes);
    const auto wanted_bytes = pattern(256 * 1024, 42);
    const auto wanted = object_id(wanted_bytes);
    const auto port = free_port();
    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "127.0.0.1";
    peer.port = port;
    peer.failure_domain = "remote";
    peer.capacity = 1024ULL * 1024 * 1024;
    peer.seen_unix_ms = unix_ms();
    RpcServer server(
        "127.0.0.1", port, fixture.keys(), peer,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::have_valid_objects) {
                Reader reader(request.payload);
                const auto count = reader.u32();
                Writer writer;
                writer.u32(count);
                for (uint32_t i = 0; i < count; ++i) {
                    (void)reader.fixed<32>();
                    ++probes;
                    writer.u8(peer_holds_held.load() ? 1 : 0);
                }
                return RpcMessage{MessageType::have_valid_objects_reply, writer.take()};
            }
            if (request.type == MessageType::get_object) {
                ++fetches;
                Writer writer;
                writer.fixed(wanted.bytes);
                writer.bytes(wanted_bytes);
                return RpcMessage{MessageType::object_reply, writer.take()};
            }
            if (request.type == MessageType::put_object) ++puts;
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {}, 4ULL * 1024 * 1024, {}, &node.resources.memory);
    server.start();
    node.membership().observe(peer, true);

    REQUIRE(node.local_store().put(held, held_bytes));
    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    REQUIRE(store.should_own(wanted));
    const auto short_of_an_extent = node.config().extent_size - 1;

    // Push: the peer is probed, and the put waits for credit of the object's own size.
    const std::vector<ObjectId> held_live{held};
    auto push = store.repair_step(held_bytes.size() - 1, 16, held_live);
    CHECK(probes.load() > 0);
    CHECK(puts.load() == 0);
    CHECK(push.credit_limited);
    CHECK(!push.complete);
    push = store.repair_step(held_bytes.size(), 16, held_live);
    CHECK(puts.load() == 1);
    CHECK(!push.credit_limited);
    peer_holds_held = true;

    // Pull: an object held here is passed over by index, the fetch of the
    // missing one waits for credit, and the cursor stays on it.
    std::vector<ObjectId> pull_live{held, wanted};
    std::sort(pull_live.begin(), pull_live.end());
    DistributedStore puller(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    DistributedStore::RepairResult pull;
    for (int step = 0; step < 8 && !pull.credit_limited; ++step)
        pull = puller.repair_step(short_of_an_extent, 16, pull_live);
    CHECK(pull.credit_limited);
    CHECK(fetches.load() == 0);
    CHECK(!node.local_store().has(wanted));
    for (int step = 0; step < 8 && !node.local_store().has(wanted); ++step)
        (void)puller.repair_step(node.config().extent_size, 16, pull_live);
    CHECK(fetches.load() == 1);
    CHECK(node.local_store().has(wanted));
    server.stop();
}

namespace {
// A peer for repair's push phase: answers presence as configured, counts what
// it was asked, and accepts puts.
struct RepairPeer {
    std::atomic_bool knows_batch{true};
    std::atomic_bool holds_everything{};
    std::atomic_uint batch_requests{};
    std::atomic_uint single_probes{};
    std::atomic_uint puts{};
    std::atomic_uint puts_in_flight{};
    std::atomic_uint max_puts_in_flight{};
    std::chrono::milliseconds put_delay{};
    NodeInfo info;
    std::unique_ptr<RpcServer> server;

    RepairPeer(TestNode& fixture, BareNode& node) {
        info.id = random_node_id();
        info.host = "127.0.0.1";
        info.port = free_port();
        info.failure_domain = "remote";
        info.capacity = 64ULL * 1024 * 1024 * 1024;
        info.seen_unix_ms = unix_ms();
        server = std::make_unique<RpcServer>(
            "127.0.0.1", info.port, fixture.keys(), info,
            [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                if (request.type == MessageType::have_valid_objects) {
                    if (!knows_batch.load())
                        return RpcMessage{MessageType::error, {}};
                    ++batch_requests;
                    Reader reader(request.payload);
                    const auto count = reader.u32();
                    Writer writer;
                    writer.u32(count);
                    for (uint32_t i = 0; i < count; ++i) {
                        (void)reader.fixed<32>();
                        writer.u8(holds_everything.load() ? 1 : 0);
                    }
                    return RpcMessage{MessageType::have_valid_objects_reply, writer.take()};
                }
                if (request.type == MessageType::have_object) {
                    ++single_probes;
                    Writer writer;
                    writer.u8(holds_everything.load() ? 1 : 0);
                    return RpcMessage{MessageType::bool_reply, writer.take()};
                }
                if (request.type == MessageType::put_object) {
                    const auto now = ++puts_in_flight;
                    auto seen = max_puts_in_flight.load();
                    while (now > seen && !max_puts_in_flight.compare_exchange_weak(seen, now)) {
                    }
                    std::this_thread::sleep_for(put_delay);
                    --puts_in_flight;
                    ++puts;
                }
                return RpcMessage{MessageType::ok, {}};
            },
            [](const NodeInfo&) {}, 4ULL * 1024 * 1024, RpcServerExecutionLimits{},
            &node.resources.memory);
        server->start();
        node.membership().observe(info, true);
    }
    ~RepairPeer() { server->stop(); }

    void report_viewers(NodeRuntime& node, uint64_t sequence, uint32_t foreground_bps) {
        NodeTelemetry telemetry;
        telemetry.node_id = info.id;
        telemetry.boot_id = NodeId{};
        telemetry.sequence = sequence;
        telemetry.observed_unix_ms = unix_ms();
        telemetry.host = info.host;
        telemetry.port = info.port;
        telemetry.traffic = {TrafficClass{static_cast<uint8_t>(FrameType::foreground), 0, 0,
                                          foreground_bps, 0}};
        node.telemetry().observe(telemetry, true);
    }
};

TestNode& repair_fixture(std::optional<TestNode>& slot, const char* name) {
    slot.emplace(name, ConfigProfile::functional);
    auto& config = slot->config();
    config.replication = 2;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    config.heartbeat = 30s;
    return *slot;
}
} // namespace

MACHA_TEST("rpc_cluster", test_repair_probes_a_window_per_request_not_per_object) {
    // A push step asks about its whole window, up to have_valid_objects_max
    // objects a request, each checked on the peer as have_object checks one.
    std::optional<TestNode> slot;
    auto& fixture = repair_fixture(slot, "repair-batched-presence");
    auto& node = fixture.start();
    RepairPeer peer(fixture, node);
    peer.holds_everything = true;

    std::vector<ObjectId> live;
    for (int i = 0; i < 40; ++i) {
        const auto bytes = pattern(8 * 1024, 300 + i);
        const auto id = object_id(bytes);
        REQUIRE(node.local_store().put(id, bytes));
        live.push_back(id);
    }
    std::sort(live.begin(), live.end());

    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    const auto step = store.repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(step.push_examined == live.size());
    CHECK(peer.batch_requests.load() == 3); // 16 + 16 + 8
    CHECK(peer.single_probes.load() == 0);
    CHECK(peer.puts.load() == 0);

    // A peer without have_valid_objects is asked one object at a time.
    peer.knows_batch = false;
    DistributedStore older(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    const auto fallback = older.repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(fallback.push_examined == live.size());
    CHECK(peer.single_probes.load() == live.size());
    CHECK(peer.puts.load() == 0);
}

MACHA_TEST("rpc_cluster", test_repair_pushes_several_objects_at_once) {
    // A step's pushes go out together, not one round trip at a time.
    std::optional<TestNode> slot;
    auto& fixture = repair_fixture(slot, "repair-pipelined-pushes");
    auto& node = fixture.start();
    RepairPeer peer(fixture, node);
    peer.put_delay = 300ms;

    std::vector<ObjectId> live;
    for (int i = 0; i < 8; ++i) {
        const auto bytes = pattern(24 * 1024, 400 + i);
        const auto id = object_id(bytes);
        REQUIRE(node.local_store().put(id, bytes));
        live.push_back(id);
    }
    std::sort(live.begin(), live.end());

    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    const auto started = Clock::now();
    const auto step = store.repair_step(64ULL * 1024 * 1024, 16, live);
    const auto elapsed = Clock::now() - started;
    CHECK(peer.puts.load() == live.size());
    CHECK(step.push_examined == live.size());
    CHECK(step.bytes_transferred == live.size() * 24 * 1024);
    CHECK(peer.max_puts_in_flight.load() > 1);
    // Serially this is 8 x 300 ms.
    CHECK(elapsed < 1500ms);
}

MACHA_TEST("rpc_cluster", test_repair_pass_keeps_its_place_across_generations_and_restarts) {
    // The push cursor survives a new live-set generation and a restart; each
    // object passed again would cost a full read on the peer.
    std::optional<TestNode> slot;
    auto& fixture = repair_fixture(slot, "repair-pass-position");
    auto& node = fixture.start();
    RepairPeer peer(fixture, node);
    peer.holds_everything = true;

    std::vector<ObjectId> live;
    for (int i = 0; i < 100; ++i) {
        const auto bytes = pattern(4 * 1024, 500 + i);
        const auto id = object_id(bytes);
        REQUIRE(node.local_store().put(id, bytes));
        live.push_back(id);
    }
    std::sort(live.begin(), live.end());
    const auto position = fixture.config().state_path / "repair" / "push-position";

    {
        DistributedStore store(node, node.resources.activity, node.resources.data,
                           node.resources.memory, node.resources.events, DistributedStoreOptions{position, {}});
        const auto first = store.repair_step(64ULL * 1024 * 1024, 16, live, {}, 1);
        CHECK(first.push_examined == 64);
        CHECK(peer.batch_requests.load() == 4);
        // A new generation continues from where the pass had reached.
        const auto second = store.repair_step(64ULL * 1024 * 1024, 16, live, {}, 2);
        CHECK(second.push_examined == 36);
        CHECK(peer.batch_requests.load() == 4 + 3);
        // The pass spanned a change, so it is not reported settled.
        CHECK(!second.complete);
    }

    // A pass part-way through survives a restart: the stopped store saved
    // where it was, and a new one skips that far by index without asking.
    const auto restart_position = fixture.config().state_path / "repair" / "restart-position";
    peer.batch_requests = 0;
    {
        DistributedStore store(node, node.resources.activity, node.resources.data,
                           node.resources.memory, node.resources.events, DistributedStoreOptions{restart_position, {}});
        const auto before = store.repair_step(64ULL * 1024 * 1024, 16, live, {}, 3);
        CHECK(before.push_examined == 64);
        CHECK(peer.batch_requests.load() == 4);
    }
    peer.batch_requests = 0;
    DistributedStore restarted(node, node.resources.activity, node.resources.data,
                           node.resources.memory, node.resources.events, DistributedStoreOptions{restart_position, {}});
    const auto resumed = restarted.repair_step(64ULL * 1024 * 1024, 16, live, {}, 3);
    CHECK(resumed.push_examined == 36);
    CHECK(peer.batch_requests.load() == 3);
}

MACHA_TEST("rpc_cluster", test_repair_without_a_generation_tells_live_sets_apart_by_identity) {
    // With no generation, a pass that a different live set arrived part-way
    // through is not reported settled; the same set again is.
    std::optional<TestNode> slot;
    auto& fixture = repair_fixture(slot, "repair-live-identity");
    auto& node = fixture.start();
    RepairPeer peer(fixture, node);
    peer.holds_everything = true;

    std::vector<ObjectId> live;
    for (int i = 0; i < 100; ++i) {
        const auto bytes = pattern(4 * 1024, 700 + i);
        const auto id = object_id(bytes);
        REQUIRE(node.local_store().put(id, bytes));
        live.push_back(id);
    }
    std::sort(live.begin(), live.end());
    const std::vector<ObjectId> another = live;

    // Steps until a pass ends; returns whether that pass was reported
    // settled. `second` is the live set from the second step on.
    const auto pass_settles = [&](const std::vector<ObjectId>& second) {
        DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
        REQUIRE(!store.repair_step(64ULL * 1024 * 1024, 16, live).complete);
        for (int step = 0; step < 20; ++step) {
            const auto passes = store.repair_diagnostics().passes_completed;
            const auto result = store.repair_step(64ULL * 1024 * 1024, 16, second);
            if (store.repair_diagnostics().passes_completed > passes)
                return result.complete;
        }
        CHECK(!"no repair pass ended in 20 steps");
        return false;
    };
    CHECK(pass_settles(live));
    CHECK(!pass_settles(another));
}

MACHA_TEST("rpc_cluster", test_repair_bandwidth_estimate_ignores_small_transfers) {
    // Small-object pushes measure round trips and far-end writes, not
    // bandwidth, so they must not drag the link estimate down.
    std::optional<TestNode> slot;
    auto& fixture = repair_fixture(slot, "repair-estimate-extents");
    auto& node = fixture.start();
    RepairPeer peer(fixture, node);
    peer.put_delay = 50ms;

    const auto small_bytes = pattern(16 * 1024, 71);
    const auto small = object_id(small_bytes);
    REQUIRE(node.local_store().put(small, small_bytes));
    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    const std::vector<ObjectId> small_live{small};
    (void)store.repair_step(64ULL * 1024 * 1024, 16, small_live);
    REQUIRE(peer.puts.load() == 1);
    CHECK(store.estimated_network_bps() == 0.0);

    const auto large_bytes = pattern(node.config().extent_size / 2, 72);
    const auto large = object_id(large_bytes);
    REQUIRE(node.local_store().put(large, large_bytes));
    DistributedStore extents(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    const std::vector<ObjectId> large_live{large};
    (void)extents.repair_step(64ULL * 1024 * 1024, 16, large_live);
    REQUIRE(peer.puts.load() == 2);
    CHECK(extents.estimated_network_bps() > 0.0);

    // This node's speculative bytes out carry both objects.
    const auto totals = node.traffic_totals();
    CHECK(totals.out_bytes[static_cast<size_t>(FrameType::speculative)] >=
          small_bytes.size() + large_bytes.size());
}

MACHA_TEST("rpc_cluster", test_repair_is_paced_not_stopped_while_a_peer_serves_viewers) {
    // A peer's viewers pace repair as local ones do: weighted turns, never a
    // stop. With a peer reporting viewer traffic throughout, the missing copy
    // still comes back and the pacer takes turns.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 1;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 500ms;
        config->maintenance.no_progress_backoff = 500ms;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));
    REQUIRE(s1.node().wait_local_state_ready(std::chrono::seconds{10}));
    REQUIRE(s2.node().wait_local_state_ready(std::chrono::seconds{10}));

    // Another node, as s2's telemetry sees it, playing throughout.
    std::atomic_bool watching{true};
    std::thread viewer([&] {
        const auto peer = random_node_id();
        const auto boot = random_node_id();
        for (uint64_t sequence = 1; watching.load(); ++sequence) {
            NodeTelemetry telemetry;
            telemetry.node_id = peer;
            telemetry.boot_id = boot;
            telemetry.sequence = sequence;
            telemetry.observed_unix_ms = unix_ms();
            telemetry.host = "127.0.0.9";
            telemetry.port = 9;
            telemetry.traffic = {TrafficClass{static_cast<uint8_t>(FrameType::foreground), 0, 0,
                                              1'300'000, 0}};
            s2.node().telemetry().observe(telemetry, true);
            std::this_thread::sleep_for(50ms);
        }
    });
    REQUIRE(wait_until([&] { return s2.node().peer_viewers_active(3000ms); }));

    const auto bytes = pattern(96 * 1024 + 31);
    const auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    const RetentionDot claim{s1.node().node_id(), 0xfeed};
    s1.node().claims().retain(RetentionClass::data, id, claim);
    s2.node().claims().retain(RetentionClass::data, id, claim);
    REQUIRE(!s2.node().local_store().valid(id));
    const auto share_before = s2.repair_diagnostics().gate_share;
    s2.resources().events.notify(NodeEvent::storage);

    const bool restored = wait_until([&] { return s2.node().local_store().valid(id); }, 10s);
    const auto share_after = s2.repair_diagnostics().gate_share;
    watching = false;
    viewer.join();
    REQUIRE(restored);
    CHECK(*s2.node().local_store().get(id) == bytes);
    // Paced: the weighted share turned passes away between repair's turns.
    CHECK(share_after > share_before);
}

MACHA_TEST("rpc_cluster", test_repair_decides_already_held_without_reading_the_extent) {
    // Repair checks presence by index lookup, never by reading the extent;
    // corruption belongs to scrub and the read path.
    TestNode fixture("repair-presence-by-index", ConfigProfile::functional);
    auto& config = fixture.config();
    config.replication = 1;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    auto& node = fixture.start();

    std::vector<ObjectId> live;
    for (int i = 0; i < 24; ++i) {
        const auto bytes = pattern(64 * 1024, 200 + i);
        const auto id = object_id(bytes);
        REQUIRE(node.local_store().put(id, bytes));
        live.push_back(id);
    }
    std::sort(live.begin(), live.end());

    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    const auto before = node.resources.data.stats().speculative_admissions;
    size_t examined = 0;
    for (int pass = 0; pass < 8 && examined < live.size(); ++pass) {
        const auto result = store.repair_step(8ULL * 1024 * 1024, 16, live);
        examined += result.pull_examined;
    }
    CHECK(examined >= live.size());
    CHECK(node.resources.data.stats().speculative_admissions == before);
    // The progress shows in the diagnostics.
    CHECK(store.repair_diagnostics().pull_examined == examined);
}

MACHA_TEST("rpc_cluster", test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended) {
    // Repair yields between operations, never inside one: the turn ends the
    // moment the peer receives the request, and the extent still lands.
    TestNode fixture("repair-in-flight-pull", ConfigProfile::functional);
    auto& config = fixture.config();
    config.replication = 2;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    config.heartbeat = 30s;
    auto& node = fixture.start();

    const auto bytes = pattern(512 * 1024, 77);
    const auto id = object_id(bytes);
    const auto port = free_port();
    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "127.0.0.1";
    peer.port = port;
    peer.failure_domain = "remote";
    peer.capacity = 1024ULL * 1024 * 1024;
    peer.seen_unix_ms = unix_ms();

    std::atomic_uint fetches{};
    RpcServer server(
        "127.0.0.1", port, fixture.keys(), peer,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::get_object) {
                ++fetches;
                // Long enough for a poll of the yield predicate to see it.
                std::this_thread::sleep_for(150ms);
                Writer writer;
                writer.fixed(id.bytes);
                writer.bytes(bytes);
                return RpcMessage{MessageType::object_reply, writer.take()};
            }
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {}, 4ULL * 1024 * 1024, {}, &node.resources.memory);
    server.start();
    node.membership().observe(peer, true);

    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    REQUIRE(store.should_own(id));
    REQUIRE(!node.local_store().has(id));
    const std::vector<ObjectId> live{id};

    auto result = store.repair_step(8ULL * 1024 * 1024, 8, live,
                                    [&] { return fetches.load() > 0; });
    CHECK(fetches.load() == 1);
    CHECK(result.bytes_transferred == bytes.size());
    REQUIRE(node.local_store().valid(id));
    CHECK(*node.local_store().get(id) == bytes);

    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_v15_persistence_and_multiplexing) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate first_slow_gate;
    std::atomic_uint slow_calls{};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (!request.payload.empty() && request.payload.front() == 1) {
                if (slow_calls.fetch_add(1) == 0)
                    first_slow_gate.enter_and_wait();
                else
                    std::this_thread::sleep_for(80ms);
            }
            return RpcMessage{MessageType::ok, request.payload};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms);
    Endpoint endpoint{"127.0.0.1", port};

    Bytes slow_payload{1};
    Bytes fast_payload{2};
    auto slow = client.call_async(endpoint, MessageType::ping, slow_payload);
    REQUIRE(first_slow_gate.wait_for_entries(1));
    auto fast = client.call_async(endpoint, MessageType::ping, fast_payload);

    REQUIRE(fast.wait_for(150ms) == std::future_status::ready);
    auto fast_reply = fast.get();
    CHECK(fast_reply.message.type == MessageType::ok);
    CHECK(fast_reply.message.payload == fast_payload);
    first_slow_gate.open();
    REQUIRE(slow.wait_for(500ms) == std::future_status::ready);
    CHECK(slow.get().message.payload == slow_payload);

    // An empty request straight after prior frames on the same channel stays in sync.
    auto empty_reply = client.call(endpoint, MessageType::ping, {}, 1s);
    CHECK(empty_reply.message.type == MessageType::ok);
    CHECK(empty_reply.message.payload.empty());

    auto stats = client.stats();
    CHECK(stats.connections_created == 1);
    CHECK(stats.connections_reused >= 2);

    // call()'s interval observes stalls; it is not a deadline. A healthy RPC
    // may take far longer and still completes on the same connection.
    Bytes very_slow_payload{1};
    auto started = Clock::now();
    auto very_slow = client.call(endpoint, MessageType::ping, very_slow_payload, 50ms);
    CHECK(very_slow.message.type == MessageType::ok);
    CHECK(very_slow.message.payload == very_slow_payload);
    CHECK(Clock::now() - started >= 60ms);

    // The connection stays usable after a slow request, with no reconnect.
    auto recovered = client.call(endpoint, MessageType::ping, fast_payload, 50ms);
    CHECK(recovered.message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 1);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_concurrent_cold_data_calls_share_one_dial) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage& request) {
            return RpcMessage{MessageType::ok, request.payload};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms);

    constexpr size_t callers = 24;
    std::atomic_size_t ready{};
    std::atomic_bool release{};
    std::atomic_size_t failures{};
    std::vector<std::jthread> threads;
    threads.reserve(callers);
    for (size_t i = 0; i < callers; ++i) {
        threads.emplace_back([&, i] {
            ++ready;
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
            try {
                Bytes payload{static_cast<uint8_t>(i)};
                auto reply = client.call(server_info, MessageType::put_object, payload,
                                         FrameType::foreground, 2s);
                if (reply.message.type != MessageType::ok || reply.message.payload != payload)
                    ++failures;
            } catch (...) {
                ++failures;
            }
        });
    }
    REQUIRE(wait_until([&] { return ready.load() == callers; }));
    release.store(true, std::memory_order_release);
    threads.clear();

    CHECK(failures.load() == 0);
    CHECK(client.stats().connections_created == 1);
    CHECK(client.stats().canonical_connections == 1);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_full_extent_reply_uses_owned_transport_handoff) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";
    const auto extent_reply = pattern(4 * 1024 * 1024 + 36, 117);
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::get_object)
                return RpcMessage{MessageType::object_reply, extent_reply};
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms);

    auto reply = client.call(server_info, MessageType::get_object, Bytes{0x01},
                             FrameType::foreground, 2s);
    CHECK(reply.message.type == MessageType::object_reply);
    CHECK(reply.message.payload == extent_reply);

    // The fragmented DATA session stays aligned and reusable.
    auto repeated = client.call(server_info, MessageType::get_object, Bytes{0x02},
                                FrameType::foreground, 2s);
    CHECK(repeated.message.payload == extent_reply);
    CHECK(client.stats().connections_created == 1);
    CHECK(client.stats().canonical_connections == 1);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_v15_bidirectional_and_deduplication) {
    TestCluster cluster;
    const auto& keys = cluster.keys();

    struct TestNode {
        NodeInfo info;
        RpcClient client;
        RpcServer server;

        TestNode(ClusterKeys keys, NodeInfo node, RpcServer::Handler handler)
            : info(std::move(node)), client(
                                         keys, [this] { return info; }, [](const NodeInfo&) {},
                                         [](uint64_t) {}, 500ms, 10s, 30s),
              server("127.0.0.1", info.port, keys, info, std::move(handler),
                     [](const NodeInfo&) {}) {
            server.attach_client(client);
            server.start();
        }
        ~TestNode() {
            server.stop();
            client.stop();
        }
    };

    auto node_info = [] {
        NodeInfo node;
        node.id = random_node_id();
        node.host = "127.0.0.1";
        node.port = free_port();
        node.failure_domain = "rpc-test";
        return node;
    };
    auto echo = [](const NodeInfo&, FrameType, const RpcMessage& request) {
        return RpcMessage{MessageType::ok, request.payload};
    };

    // Once A has called B, B calls back over the accepted socket, not a second connection.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        CHECK(a.client.call(b.info, MessageType::members, Bytes{1}, 1s).message.payload ==
              Bytes{1});
        CHECK(b.client.stats().connections_created == 0);
        CHECK(b.client.call(a.info, MessageType::members, Bytes{2}, 1s).message.payload ==
              Bytes{2});
        CHECK(b.client.stats().connections_created == 0);

        // DATA is a second canonical lane, opened lazily by A; B reuses it.
        CHECK(a.client.call(b.info, MessageType::put_object, Bytes{3}, FrameType::foreground, 1s)
                  .message.payload == Bytes{3});
        CHECK(b.client.call(a.info, MessageType::get_object, Bytes{4}, FrameType::foreground, 1s)
                  .message.payload == Bytes{4});
        CHECK(b.client.stats().connections_created == 0);
        CHECK(a.client.stats().canonical_connections == 2);
        CHECK(b.client.stats().canonical_connections == 2);

    }

    // A simultaneous cross-dial: both nodes pick the same winner and reuse it.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        std::atomic_int ready{};
        std::exception_ptr a_error;
        std::exception_ptr b_error;
        std::jthread a_call([&] {
            try {
                ++ready;
                while (ready.load() < 2)
                    std::this_thread::yield();
                auto reply = a.client.call(b.info, MessageType::members, Bytes{3}, 1s);
                if (reply.message.payload != Bytes{3})
                    throw std::runtime_error("bad A cross-dial reply");
            } catch (...) {
                a_error = std::current_exception();
            }
        });
        std::jthread b_call([&] {
            try {
                ++ready;
                while (ready.load() < 2)
                    std::this_thread::yield();
                auto reply = b.client.call(a.info, MessageType::members, Bytes{4}, 1s);
                if (reply.message.payload != Bytes{4})
                    throw std::runtime_error("bad B cross-dial reply");
            } catch (...) {
                b_error = std::current_exception();
            }
        });
        a_call.join();
        b_call.join();
        if (a_error)
            std::rethrow_exception(a_error);
        if (b_error)
            std::rethrow_exception(b_error);

        REQUIRE(wait_until([&] {
            return a.client.stats().canonical_connections == 1 &&
                   b.client.stats().canonical_connections == 1;
        }));
        auto a_created = a.client.stats().connections_created;
        auto b_created = b.client.stats().connections_created;
        CHECK(a.client.call(b.info, MessageType::members, Bytes{5}, 1s).message.payload ==
              Bytes{5});
        CHECK(b.client.call(a.info, MessageType::members, Bytes{6}, 1s).message.payload ==
              Bytes{6});
        CHECK(a.client.stats().connections_created == a_created);
        CHECK(b.client.stats().connections_created == b_created);
    }

    // A hostname alias may cause a transient second dial; NodeId dedup leaves one route.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        Endpoint numeric{"127.0.0.1", b.info.port};
        Endpoint alias{"localhost", b.info.port};
        CHECK(a.client.call(numeric, MessageType::members, Bytes{7}, 1s).message.payload ==
              Bytes{7});
        CHECK(a.client.call(alias, MessageType::members, Bytes{8}, 1s).message.payload == Bytes{8});
        REQUIRE(wait_until([&] { return a.client.stats().canonical_connections == 1; }));
        auto created = a.client.stats().connections_created;
        CHECK(a.client.call(alias, MessageType::members, Bytes{9}, 1s).message.payload == Bytes{9});
        CHECK(a.client.stats().connections_created == created);
    }

    // A failed dial's backoff must not mask a canonical route that arrives
    // inbound straight afterwards.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        Endpoint b_endpoint{"127.0.0.1", b.info.port};

        b.server.stop();
        bool failed = false;
        try {
            (void)a.client.call(b_endpoint, MessageType::members, Bytes{10}, 1s);
        } catch (...) {
            failed = true;
        }
        REQUIRE(failed);

        b.server.attach_client(b.client);
        b.server.start();
        CHECK(b.client.call(a.info, MessageType::members, Bytes{11}, 1s).message.payload ==
              Bytes{11});
        REQUIRE(wait_until([&] { return a.client.stats().canonical_connections == 1; }));

        // Inside the 250 ms backoff window: the inbound route is used first.
        CHECK(a.client.call(b_endpoint, MessageType::members, Bytes{12}, 1s).message.payload ==
              Bytes{12});
    }

    // Retirement is a drain, not a reset: a slow request on the connection
    // arbitration discards still completes.
    {
        auto first = node_info();
        auto second = node_info();
        auto lower_info = first.id < second.id ? first : second;
        auto higher_info = first.id < second.id ? second : first;
        TestGate retired_connection_gate;
        auto lower_handler = [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (!request.payload.empty() && request.payload.front() == 42)
                retired_connection_gate.enter_and_wait();
            return RpcMessage{MessageType::ok, request.payload};
        };
        TestNode lower(keys, lower_info, lower_handler);
        TestNode higher(keys, higher_info, echo);

        auto slow = higher.client.call_async(lower.info, MessageType::members, Bytes{42});
        REQUIRE(retired_connection_gate.wait_for_entries(1));
        CHECK(lower.client.call(higher.info, MessageType::members, Bytes{43}, 1s).message.payload ==
              Bytes{43});
        retired_connection_gate.open();
        REQUIRE(slow.wait_for(1s) == std::future_status::ready);
        CHECK(slow.get().message.payload == Bytes{42});
        REQUIRE(wait_until([&] {
            return lower.client.stats().canonical_connections == 1 &&
                   higher.client.stats().canonical_connections == 1;
        }));
        auto created = higher.client.stats().connections_created;
        CHECK(higher.client.call(lower.info, MessageType::members, Bytes{44}, 1s).message.payload ==
              Bytes{44});
        CHECK(higher.client.stats().connections_created == created);
    }
}

MACHA_TEST("rpc_cluster", test_mutual_bootstrap_prunes_cross_dial) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);
    n1.start();
    n2.start();

    REQUIRE(wait_until([&] {
        const auto a = n1.rpc_stats();
        const auto b = n2.rpc_stats();
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2 &&
               a.canonical_connections == 1 && b.canonical_connections == 1;
    }));

    // A late dial still in flight can land after the route is canonical, so
    // wait for one quiet window (six heartbeats with nothing created on either
    // side), then assert that the next is quiet too: the churn stops.
    REQUIRE(wait_until([&] {
        const auto a0 = n1.rpc_stats().connections_created;
        const auto b0 = n2.rpc_stats().connections_created;
        std::this_thread::sleep_for(120ms);
        return n1.rpc_stats().connections_created == a0 &&
               n2.rpc_stats().connections_created == b0;
    }, 5s, 0ms));

    const auto a_before = n1.rpc_stats();
    const auto b_before = n2.rpc_stats();
    std::this_thread::sleep_for(120ms); // six configured heartbeats
    const auto a_after = n1.rpc_stats();
    const auto b_after = n2.rpc_stats();

    CHECK(a_after.canonical_connections == 1);
    CHECK(b_after.canonical_connections == 1);
    CHECK(a_after.connections_created == a_before.connections_created);
    CHECK(b_after.connections_created == b_before.connections_created);

    n2.stop();
    n1.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_v7_handshake_is_rejected) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server";
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage&) {
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    NodeInfo old_client;
    old_client.id = random_node_id();
    old_client.host = "127.0.0.1";
    old_client.port = free_port();
    old_client.failure_domain = "old-client";
    auto ephemeral = x25519_generate();
    auto nonce_bytes = random_bytes(32);
    std::array<uint8_t, 32> nonce{};
    std::copy(nonce_bytes.begin(), nonce_bytes.end(), nonce.begin());

    Writer hello_writer;
    hello_writer.u16(7);
    hello_writer.u32(256 * 1024); // v7 had no transport-lane byte.
    hello_writer.fixed(keys.cluster_id);
    hello_writer.fixed(old_client.id.bytes);
    hello_writer.fixed(nonce);
    hello_writer.fixed(ephemeral.public_key);
    encode_node_info(hello_writer, old_client);
    auto hello = hello_writer.take();

    Bytes authenticated(reinterpret_cast<const uint8_t*>("client/v7"),
                        reinterpret_cast<const uint8_t*>("client/v7") + 9);
    authenticated.insert(authenticated.end(), hello.begin(), hello.end());
    Writer envelope;
    envelope.bytes(hello);
    envelope.fixed(hmac_sha256(keys.auth, authenticated));
    Writer framed;
    framed.u32(static_cast<uint32_t>(envelope.data().size()));
    framed.raw(envelope.data());

    size_t sent = 0;
    while (sent < framed.data().size()) {
        auto count = send(fd, framed.data().data() + sent, framed.data().size() - sent, 0);
        REQUIRE(count > 0);
        sent += static_cast<size_t>(count);
    }

    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t byte{};
    CHECK(recv(fd, &byte, 1, 0) == 0);
    close(fd);
    OPENSSL_cleanse(ephemeral.private_key.data(), ephemeral.private_key.size());
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_slow_control_does_not_abort_data) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object) {
                std::this_thread::sleep_for(120ms);
                return RpcMessage{MessageType::ok, {}};
            }
            if (request.type == MessageType::members) {
                std::this_thread::sleep_for(120ms);
                return RpcMessage{MessageType::ok, request.payload};
            }
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        20ms, 80ms);
    Endpoint endpoint{"127.0.0.1", port};

    // 20 ms is not a deadline: both 120 ms RPCs complete, outliving the 80 ms
    // peer-death window while control pings prove the peer alive.
    Bytes object_payload(256 * 1024, 0x5a);
    auto data = client.call_async(endpoint, MessageType::put_object, object_payload);
    std::this_thread::sleep_for(5ms);

    auto started = Clock::now();
    auto control = client.call(endpoint, MessageType::members, Bytes{1}, 20ms);
    CHECK(control.message.type == MessageType::ok);
    CHECK(Clock::now() - started >= 100ms);

    REQUIRE(data.wait_for(2s) == std::future_status::ready);
    CHECK(data.get().message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 2);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_request_payload_is_charged_until_handler_completion) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto port = free_port();
    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    RetainedMemoryLedger memory(1024 * 1024, 64 * 1024, 256 * 1024, 64 * 1024);
    TestGate handler_gate;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object)
                handler_gate.enter_and_wait();
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {}, 256 * 1024, {}, &memory);
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms);

    Bytes payload(64 * 1024, 0x5a);
    auto request = client.call_async(Endpoint{"127.0.0.1", port}, MessageType::put_object,
                                     payload, FrameType::loader);
    REQUIRE(handler_gate.wait_for_entries(1));
    const auto active = memory.stats();
    CHECK(active.owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] >= payload.size());
    CHECK(active.used_bytes ==
          active.owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)]);

    handler_gate.open();
    REQUIRE(request.wait_for(2s) == std::future_status::ready);
    CHECK(request.get().message.type == MessageType::ok);
    REQUIRE(wait_until([&] { return memory.stats().used_bytes == 0; }));
    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_health_and_control_not_starved_by_data) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate data_workers_gate;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object) {
                data_workers_gate.enter_and_wait();
                return RpcMessage{MessageType::ok, {}};
            }
            if (request.type == MessageType::members)
                return RpcMessage{MessageType::ok, {}};
            if (request.type == MessageType::ping)
                return RpcMessage{MessageType::ok, {}};
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // With every data worker busy, health and membership stay prompt on the control stream.
    std::vector<AsyncRpc> bulk;
    for (int i = 0; i < 8; ++i)
        bulk.push_back(client.call_async(endpoint, MessageType::put_object, Bytes{0x5a},
                                         FrameType::speculative));
    REQUIRE(data_workers_gate.wait_for_entries(6));

    auto started = Clock::now();
    auto health = client.call(endpoint, MessageType::ping, {}, 20ms);
    CHECK(health.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 150ms);

    started = Clock::now();
    auto control = client.call(endpoint, MessageType::members, {}, 20ms);
    CHECK(control.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 150ms);

    CHECK(client.stats().canonical_connections == 2);

    data_workers_gate.open();
    for (auto& rpc : bulk) {
        REQUIRE(rpc.wait_for(1s) == std::future_status::ready);
        CHECK(rpc.get().message.type == MessageType::ok);
    }

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_have_objects_flood_does_not_delay_unrelated_control_rpc) {
    // Retention checks (have_objects) use a data-lane FrameType and so run on
    // data_workers_; with every data worker blocked on them, control messages
    // on the control pools are not delayed.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate data_workers_gate;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::have_objects) {
                data_workers_gate.enter_and_wait();
                Writer writer;
                writer.u32(0);
                return RpcMessage{MessageType::have_objects_reply, writer.take()};
            }
            if (request.type == MessageType::members)
                return RpcMessage{MessageType::members_reply, {}};
            if (request.type == MessageType::ping)
                return RpcMessage{MessageType::ok, {}};
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Every data worker busy with have_objects, as a large retain_data() batch makes it.
    std::vector<AsyncRpc> bulk;
    for (int i = 0; i < 8; ++i)
        bulk.push_back(
            client.call_async(endpoint, MessageType::have_objects, Bytes{}, FrameType::loader));
    REQUIRE(data_workers_gate.wait_for_entries(6));

    auto started = Clock::now();
    auto health = client.call(endpoint, MessageType::ping, {}, 20ms);
    CHECK(health.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 150ms);

    started = Clock::now();
    auto control = client.call(endpoint, MessageType::members, {}, 20ms);
    CHECK(control.message.type == MessageType::members_reply);
    CHECK(Clock::now() - started < 150ms);

    data_workers_gate.open();
    for (auto& rpc : bulk) {
        REQUIRE(rpc.wait_for(1s) == std::future_status::ready);
        CHECK(rpc.get().message.type == MessageType::have_objects_reply);
    }

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_health_not_starved_by_slow_control_handlers) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate slow_control_gate;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::have_object) {
                slow_control_gate.enter_and_wait();
                return RpcMessage{MessageType::bool_reply, Bytes{1}};
            }
            if (request.type == MessageType::members)
                return RpcMessage{MessageType::members_reply, {}};
            if (request.type == MessageType::ping)
                return RpcMessage{MessageType::ok, {}};
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // With both control handlers busy, health and membership use the fast-control executor.
    auto slow1 =
        client.call_async(endpoint, MessageType::have_object, Bytes{1}, FrameType::control);
    auto slow2 =
        client.call_async(endpoint, MessageType::have_object, Bytes{2}, FrameType::control);
    REQUIRE(slow_control_gate.wait_for_entries(2));

    auto started = Clock::now();
    auto health = client.call(endpoint, MessageType::ping, {}, 20ms);
    CHECK(health.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 150ms);

    started = Clock::now();
    auto members = client.call(endpoint, MessageType::members, {}, 20ms);
    CHECK(members.message.type == MessageType::members_reply);
    CHECK(Clock::now() - started < 150ms);

    slow_control_gate.open();
    REQUIRE(slow1.wait_for(1s) == std::future_status::ready);
    REQUIRE(slow2.wait_for(1s) == std::future_status::ready);
    CHECK(slow1.get().message.type == MessageType::bool_reply);
    CHECK(slow2.get().message.type == MessageType::bool_reply);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_call_fails_after_no_progress_deadline) {
    // A control call that makes no progress fails with a transient error at
    // its deadline; a slow call inside it is unaffected; zero means no deadline.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate stuck_gate;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::have_object) {
                stuck_gate.enter_and_wait();
                return RpcMessage{MessageType::bool_reply, Bytes{1}};
            }
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Stuck: the handler never answers; the deadline turns that into a
    // transient error naming the message and the elapsed silence.
    auto started = Clock::now();
    std::string error;
    try {
        (void)client.call(endpoint, MessageType::have_object, Bytes{1}, 50ms, 300ms);
    } catch (const std::exception& e) {
        error = e.what();
    }
    const auto waited = Clock::now() - started;
    CHECK(!error.empty());
    CHECK(error.find("no progress") != std::string::npos);
    CHECK(error.find("have_object") != std::string::npos);
    CHECK(error.find("cancelled for retry") != std::string::npos);
    CHECK(waited >= 250ms);
    CHECK(waited < 2s);

    // Slow-but-answering: finishes inside the deadline, no error.
    std::thread releaser([&] {
        REQUIRE(stuck_gate.wait_for_entries(2, 5s));
        std::this_thread::sleep_for(150ms);
        stuck_gate.open();
    });
    auto reply = client.call(endpoint, MessageType::have_object, Bytes{2}, 50ms, 2s);
    CHECK(reply.message.type == MessageType::bool_reply);
    releaser.join();

    // Zero deadline: healthy replies still come back straight away.
    auto pong = client.call(endpoint, MessageType::ping, {}, 50ms, 0ms);
    CHECK(pong.message.type == MessageType::ok);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_metadata_mutations_use_bounded_isolated_executor) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate metadata_gate;
    std::atomic_uint32_t metadata_calls{};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_metadata_history_entry ||
                request.type == MessageType::put_metadata_commit ||
                request.type == MessageType::accept_metadata_commit) {
                ++metadata_calls;
                metadata_gate.enter_and_wait();
                return RpcMessage{MessageType::bool_reply, Bytes{1}};
            }
            if (request.type == MessageType::members)
                return RpcMessage{MessageType::members_reply, {}};
            if (request.type == MessageType::have_object)
                return RpcMessage{MessageType::bool_reply, Bytes{1}};
            if (request.type == MessageType::get_object)
                return RpcMessage{MessageType::object_reply, Bytes{0x46}};
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {}, 256 * 1024,
        RpcServerExecutionLimits{
            .metadata_workers = 1,
            .metadata_pending_jobs = 2,
            .metadata_pending_bytes = 8,
            .fast_control_pending_bytes = 1,
            .control_pending_bytes = 1,
            .data_pending_bytes = 1});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // One metadata mutation executes while two wait in its bounded queue,
    // covering every mutation RPC routed to the executor.
    auto history =
        client.call_async(endpoint, MessageType::put_metadata_history_entry, Bytes{0x01, 0x02});
    REQUIRE(metadata_gate.wait_for_entries(1));
    auto commit = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{0x03, 0x04});
    auto acceptance =
        client.call_async(endpoint, MessageType::accept_metadata_commit, Bytes{0x05, 0x06});
    REQUIRE(wait_until(
        [&] {
            const auto stats = server.work_stats();
            return stats.metadata_active_jobs == 1 && stats.metadata_pending_jobs == 2 &&
                   stats.metadata_pending_bytes == 4;
        },
        2s));

    // A full queue answers with an ordinary RPC error; the session stays open.
    auto queue_full = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{0x07});
    REQUIRE(queue_full.wait_for(1s) == std::future_status::ready);
    CHECK(queue_full.get().message.type == MessageType::error);

    // Other classes of work complete while the metadata worker and queue are blocked.
    CHECK(client.call(endpoint, MessageType::ping, {}, 100ms).message.type == MessageType::ok);
    CHECK(client.call(endpoint, MessageType::members, {}, 100ms).message.type ==
          MessageType::members_reply);
    CHECK(client.call(endpoint, MessageType::have_object, Bytes{0x08}, 100ms).message.type ==
          MessageType::bool_reply);
    CHECK(client.call(endpoint, MessageType::get_object, Bytes{0x09}, FrameType::foreground, 100ms)
              .message.type == MessageType::object_reply);

    metadata_gate.open();
    for (auto* rpc : {&history, &commit, &acceptance}) {
        REQUIRE(rpc->wait_for(2s) == std::future_status::ready);
        CHECK(rpc->get().message.type == MessageType::bool_reply);
    }
    REQUIRE(wait_until(
        [&] {
            const auto stats = server.work_stats();
            return stats.metadata_active_jobs == 0 && stats.metadata_pending_jobs == 0 &&
                   stats.metadata_pending_bytes == 0;
        },
        2s));

    // The payload-byte limit applies with no other metadata work queued; the
    // rejected job never reaches the handler.
    auto too_large =
        client.call_async(endpoint, MessageType::accept_metadata_commit, Bytes(9, 0x0a));
    REQUIRE(too_large.wait_for(1s) == std::future_status::ready);
    CHECK(too_large.get().message.type == MessageType::error);
    CHECK(metadata_calls.load() == 3);
    // Every executor class has its own byte limit; a rejected payload never
    // enters a queue.
    auto fast_too_large = client.call_async(endpoint, MessageType::ping, Bytes(2, 0x11));
    auto validation_too_large =
        client.call_async(endpoint, MessageType::have_object, Bytes(2, 0x12));
    auto data_too_large = client.call_async(endpoint, MessageType::get_object, Bytes(2, 0x13),
                                            FrameType::foreground);
    for (auto* rejected : {&fast_too_large, &validation_too_large, &data_too_large}) {
        REQUIRE(rejected->wait_for(1s) == std::future_status::ready);
        CHECK(rejected->get().message.type == MessageType::error);
    }
    const auto final_stats = server.work_stats();
    CHECK(final_stats.metadata_rejected_jobs == 2);
    CHECK(final_stats.rejected_jobs == 3);
    CHECK(final_stats.fast_control_pending_bytes == 0);
    CHECK(final_stats.control_pending_bytes == 0);
    CHECK(final_stats.data_pending_bytes == 0);
    REQUIRE(final_stats.message_timings.contains(MessageType::put_metadata_history_entry));
    REQUIRE(final_stats.message_timings.contains(MessageType::put_metadata_commit));
    REQUIRE(final_stats.message_timings.contains(MessageType::accept_metadata_commit));
    CHECK(final_stats.message_timings.at(MessageType::put_metadata_history_entry).requests == 1);
    CHECK(final_stats.message_timings.at(MessageType::put_metadata_commit).requests == 1);
    CHECK(final_stats.message_timings.at(MessageType::accept_metadata_commit).requests == 1);
    CHECK(final_stats.message_timings.at(MessageType::put_metadata_history_entry).handler_us_total >
          0);
    CHECK(final_stats.message_timings.at(MessageType::put_metadata_commit).queue_wait_us_total > 0);
    CHECK(final_stats.message_timings.at(MessageType::accept_metadata_commit).queue_wait_us_total >
          0);
    REQUIRE(final_stats.frame_timings.contains(FrameType::control));
    REQUIRE(final_stats.frame_timings.contains(FrameType::foreground));
    REQUIRE(final_stats.frame_timings.contains(FrameType::speculative));
    CHECK(final_stats.frame_timings.at(FrameType::control).requests >= 4);
    CHECK(final_stats.frame_timings.at(FrameType::foreground).requests == 1);
    CHECK(final_stats.frame_timings.at(FrameType::speculative).requests == 1);

    // Both CONTROL and DATA sessions remain usable after backpressure replies.
    CHECK(client.call(endpoint, MessageType::ping, {}, 100ms).message.type == MessageType::ok);
    CHECK(client.call(endpoint, MessageType::get_object, Bytes{0x0b}, FrameType::foreground, 100ms)
              .message.type == MessageType::object_reply);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    NodeInfo first_client;
    first_client.id = random_node_id();
    first_client.host = "127.0.0.1";
    first_client.port = free_port();
    first_client.failure_domain = "first-client-site";

    NodeInfo second_client;
    second_client.id = random_node_id();
    second_client.host = "127.0.0.1";
    second_client.port = free_port();
    second_client.failure_domain = "second-client-site";

    TestGate first_job_gate;
    std::atomic_bool first_peer_second_started{};
    std::atomic_bool second_peer_started{};
    std::mutex order_mutex;
    std::vector<uint8_t> first_peer_order;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo& peer, FrameType, const RpcMessage& request) {
            REQUIRE(request.type == MessageType::put_metadata_commit);
            REQUIRE(!request.payload.empty());
            const auto marker = request.payload.front();
            if (peer.id == first_client.id) {
                {
                    std::lock_guard lock(order_mutex);
                    first_peer_order.push_back(marker);
                }
                if (marker == 1)
                    first_job_gate.enter_and_wait();
                else if (marker == 2)
                    first_peer_second_started = true;
            } else if (peer.id == second_client.id) {
                second_peer_started = true;
            }
            return RpcMessage{MessageType::bool_reply, Bytes{1}};
        },
        [](const NodeInfo&) {}, 256 * 1024,
        RpcServerExecutionLimits{
            .metadata_workers = 2, .metadata_pending_jobs = 8, .metadata_pending_bytes = 64});
    server.start();

    RpcClient client_a(
        keys, [first_client] { return first_client; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 100ms, 2s);
    RpcClient client_b(
        keys, [second_client] { return second_client; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    auto first = client_a.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(first_job_gate.wait_for_entries(1));
    auto same_peer_next = client_a.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    auto other_peer = client_b.call_async(endpoint, MessageType::put_metadata_commit, Bytes{3});

    // The second worker may serve another peer, but never overtakes this
    // peer's blocked sequence.
    REQUIRE(wait_until([&] { return second_peer_started.load(); }, 2s));
    CHECK(!first_peer_second_started.load());

    first_job_gate.open();
    for (auto* rpc : {&first, &same_peer_next, &other_peer}) {
        REQUIRE(rpc->wait_for(2s) == std::future_status::ready);
        CHECK(rpc->get().message.type == MessageType::bool_reply);
    }
    CHECK(first_peer_second_started.load());
    {
        std::lock_guard lock(order_mutex);
        CHECK(first_peer_order == std::vector<uint8_t>({1, 2}));
    }

    client_a.stop();
    client_b.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_metadata_executor_cancellation_and_disconnect_boundaries) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info{random_node_id(), "127.0.0.1", "server-site", port};
    TestGate before_durability;
    TestGate after_durability;
    std::atomic_uint32_t handler_calls{};
    std::atomic_uint32_t durable_jobs{};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            REQUIRE(request.type == MessageType::put_metadata_commit);
            ++handler_calls;
            before_durability.enter_and_wait();
            ++durable_jobs;
            after_durability.enter_and_wait();
            return RpcMessage{MessageType::bool_reply, Bytes{1}};
        },
        [](const NodeInfo&) {}, 256 * 1024,
        RpcServerExecutionLimits{
            .metadata_workers = 1, .metadata_pending_jobs = 4, .metadata_pending_bytes = 64});
    server.start();

    NodeInfo client_info{random_node_id(), "127.0.0.1", "client-site", free_port()};
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    auto running = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(before_durability.wait_for_entries(1));
    auto queued = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    REQUIRE(wait_until([&] { return server.work_stats().metadata_pending_jobs == 1; }, 2s));

    // A queued job is removed by cancellation without entering the handler.
    queued.cancel();
    REQUIRE(wait_until([&] { return server.work_stats().metadata_pending_jobs == 0; }, 2s));
    CHECK(handler_calls.load() == 1);

    // A running job is not cancelled by losing its reply route; it finishes
    // and the reply is discarded.
    before_durability.open();
    REQUIRE(after_durability.wait_for_entries(1));
    CHECK(durable_jobs.load() == 1);
    running.abort();
    after_durability.open();
    REQUIRE(wait_until([&] { return server.work_stats().metadata_active_jobs == 0; }, 2s));
    CHECK(handler_calls.load() == 1);
    CHECK(durable_jobs.load() == 1);

    client.stop();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_metadata_executor_shutdown_finishes_owner_and_drops_queue) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info{random_node_id(), "127.0.0.1", "server-site", port};
    TestGate running_gate;
    std::atomic_uint32_t handler_calls{};
    std::atomic_uint32_t durable_jobs{};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            REQUIRE(request.type == MessageType::put_metadata_commit);
            ++handler_calls;
            running_gate.enter_and_wait();
            ++durable_jobs;
            return RpcMessage{MessageType::bool_reply, Bytes{1}};
        },
        [](const NodeInfo&) {}, 256 * 1024,
        RpcServerExecutionLimits{
            .metadata_workers = 1, .metadata_pending_jobs = 4, .metadata_pending_bytes = 64});
    server.start();

    NodeInfo client_info{random_node_id(), "127.0.0.1", "client-site", free_port()};
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    auto running = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(running_gate.wait_for_entries(1));
    auto queued = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    REQUIRE(wait_until([&] { return server.work_stats().metadata_pending_jobs == 1; }, 2s));

    auto stopping = std::async(std::launch::async, [&] { server.stop(); });
    // Closing the session detaches both replies; the queued request never executes.
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    REQUIRE(queued.wait_for(2s) == std::future_status::ready);
    running_gate.open();
    REQUIRE(stopping.wait_for(2s) == std::future_status::ready);
    stopping.get();

    CHECK(handler_calls.load() == 1);
    CHECK(durable_jobs.load() == 1);
    const auto stats = server.work_stats();
    CHECK(stats.metadata_active_jobs == 0);
    CHECK(stats.metadata_pending_jobs == 0);
    CHECK(stats.metadata_pending_bytes == 0);

    client.stop();
}

MACHA_TEST("rpc_cluster", test_service_shutdown_cancels_pending_outbound_rpc_before_join) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "shutdown-client", cluster.keyfile(), free_port());
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;

    Service service(config, keys);
    service.start();
    (void)service.filesystem();

    const auto peer_port = free_port();
    NodeInfo peer{random_node_id(), "127.0.0.1", "blocked-peer", peer_port};
    peer.metadata_write_replicas_required = 1;
    TestGate blocked;
    RpcServer server(
        "127.0.0.1", peer_port, keys, peer,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::get_object) {
                blocked.enter_and_wait();
                return RpcMessage{MessageType::object_reply, {}};
            }
            return RpcMessage{MessageType::error, {}};
        },
        [](const NodeInfo&) {}, 256 * 1024);
    server.start();
    struct GateOpener {
        TestGate& gate;
        ~GateOpener() { gate.open(); }
    } open_on_exit{blocked};

    const Endpoint endpoint{"127.0.0.1", peer_port};
    auto pending = std::async(std::launch::async, [&] {
        try {
            Bytes object_id(32, 0x7b);
            (void)service.node().call(endpoint, MessageType::get_object, object_id,
                                      FrameType::speculative);
            return false;
        } catch (...) {
            return true;
        }
    });
    REQUIRE(blocked.wait_for_entries(1));

    // Shutdown closes outbound routes first, so an outstanding synchronous RPC
    // fails without waiting for its stall deadline.
    const auto started = Clock::now();
    auto stopping = std::async(std::launch::async, [&] { service.stop(); });
    REQUIRE(stopping.wait_for(2s) == std::future_status::ready);
    stopping.get();
    CHECK(Clock::now() - started < 2s);
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    CHECK(pending.get());

    // Cancellation persists: no new route or synchronous RPC after the close.
    const auto retry_started = Clock::now();
    bool retry_rejected = false;
    try {
        (void)service.node().call(endpoint, MessageType::ping, {});
    } catch (const std::exception&) {
        retry_rejected = true;
    }
    CHECK(retry_rejected);
    CHECK(Clock::now() - retry_started < 250ms);

    blocked.open();
    server.stop();
}

MACHA_TEST("rpc_cluster", test_rpc_foreground_not_starved_by_busy_data_workers) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info;
    server_info.id = random_node_id();
    server_info.host = "127.0.0.1";
    server_info.port = port;
    server_info.failure_domain = "server-site";

    TestGate lower_priority_gate;
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType frame_type, const RpcMessage& request) {
            if (request.type == MessageType::put_object && frame_type != FrameType::foreground)
                lower_priority_gate.enter_and_wait();
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(
        keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 500ms,
        100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Loader work leaves DATA execution capacity for a later playback read.
    std::vector<AsyncRpc> bulk;
    for (int i = 0; i < 8; ++i)
        bulk.push_back(client.call_async(endpoint, MessageType::put_object, Bytes{0x42},
                                         FrameType::loader));
    REQUIRE(lower_priority_gate.wait_for_entries(6));

    auto started = Clock::now();
    auto foreground =
        client.call(endpoint, MessageType::get_object, Bytes{0x46}, FrameType::foreground, 100ms);
    CHECK(foreground.message.type == MessageType::ok);
    CHECK(Clock::now() - started < 200ms);

    lower_priority_gate.open();
    for (auto& rpc : bulk) {
        REQUIRE(rpc.wait_for(2s) == std::future_status::ready);
        CHECK(rpc.get().message.type == MessageType::ok);
    }

    client.stop();
    server.stop();
}

MACHA_FAST_TEST("rpc_cluster", test_data_credit_wait_gives_up_when_nothing_is_moving) {
    // A DATA credit wait with no deadline gives up only when the arbiter is
    // wholly stalled; any release resets the window, so contention still waits.
    constexpr uint64_t capacity = 4 * 1024 * 1024;
    constexpr uint64_t reserve = 1024 * 1024;
    constexpr uint64_t chunk = 1024 * 1024;

    // background_concurrency 2: a third loader acquire cannot be admitted
    // while the first two are held.
    DataResourceArbiter arbiter(capacity, reserve, 2, 200ms);
    auto context = DataWorkContext(FrameType::loader, chunk);
    auto first = arbiter.acquire(context, chunk);
    auto second = arbiter.acquire(context, chunk);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    const auto started = Clock::now();
    auto third = arbiter.acquire(context, chunk);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started);
    // It gives up after the no-progress window instead of hanging.
    CHECK(!third.has_value());
    CHECK(waited >= 200ms);
    CHECK(waited < 5s);

    // Releasing makes room again: giving up once does not poison the arbiter.
    first.reset();
    auto fourth = arbiter.acquire(context, chunk);
    CHECK(fourth.has_value());
}

MACHA_FAST_TEST("rpc_cluster", test_data_credit_wait_survives_genuine_contention) {
    // A waiter does not give up while work flows: a release inside the
    // no-progress window resets it, and the waiter is admitted.
    constexpr uint64_t capacity = 4 * 1024 * 1024;
    constexpr uint64_t reserve = 1024 * 1024;
    constexpr uint64_t chunk = 1024 * 1024;

    DataResourceArbiter arbiter(capacity, reserve, 2, 400ms);
    auto context = DataWorkContext(FrameType::loader, chunk);
    auto first = arbiter.acquire(context, chunk);
    auto second = arbiter.acquire(context, chunk);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    std::jthread releaser([&] {
        std::this_thread::sleep_for(150ms);
        first.reset();
    });

    auto third = arbiter.acquire(context, chunk);
    CHECK(third.has_value());
}

MACHA_TEST("rpc_cluster", test_a_namespace_node_below_the_floor_is_metadata_not_ready) {
    // Missing the metadata durability floor is transient cluster state and
    // must carry the type callers retry on.
    TestService fixture("namespace-node-below-floor", ConfigProfile::isolated);
    fixture.config().replication = 1;
    fixture.config().metadata_min_write_replicas = 1;
    auto& service = fixture.start();
    auto nodes = ControlNamespaceNodeStore::for_commit(service.node(), service.filesystem().store(), 2);
    const auto bytes = pattern(512, 17);
    bool not_ready = false;
    try {
        (void)nodes.put(bytes);
    } catch (const MetadataNotReady&) {
        not_ready = true;
    }
    CHECK(not_ready);
    CHECK(nodes.written().empty());
}

MACHA_TEST("rpc_cluster", test_a_local_control_object_is_found_while_data_credit_is_exhausted) {
    // The control store is not on the DATA device: validating a local control
    // object never waits for DATA credit.
    TestService fixture("control-local-without-data-credit", ConfigProfile::isolated);
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    const auto extent = config.extent_size;
    // One extent of non-viewer credit in all, held below by a loader.
    config.data_inflight_bytes = 2 * extent;
    config.data_viewer_reserve_bytes = extent;
    config.maintenance.background_concurrency = 1;
    auto& service = fixture.start();

    const auto bytes = pattern(18 * 1024, 91);
    const auto id = object_id(bytes);
    REQUIRE(service.node().control_store().put(id, bytes));

    auto held = service.resources().data.acquire(DataWorkContext(FrameType::loader, extent), extent);
    REQUIRE(held.has_value());
    auto found = std::async(std::launch::async,
                            [&] { return service.filesystem().store().ensure_control_local(id); });
    const bool prompt = found.wait_for(2s) == std::future_status::ready;
    held.reset(); // lets a regressed build finish rather than hang the suite
    CHECK(prompt);
    CHECK(found.get());
}

MACHA_TEST("rpc_cluster", test_storage_data_credit_reserves_viewer_headroom_and_control) {
    TestNode fixture("data-resource-viewer-reserve", ConfigProfile::functional);
    auto& config = fixture.config();
    const auto extent = config.extent_size;
    config.data_inflight_bytes = 4 * extent;
    config.data_viewer_reserve_bytes = extent;
    // Fix the ceiling: its default, hardware_concurrency() / 2, would block the
    // three loader acquires below on hosts with fewer than six cores.
    config.maintenance.background_concurrency = 4;
    auto& node = fixture.start();

    const auto bytes = pattern(64 * 1024, 77);
    const auto id = object_id(bytes);
    REQUIRE(node.local_store().put(id, bytes));

    auto loader_context = DataWorkContext(FrameType::loader, extent);
    auto first = node.resources.data.acquire(loader_context, extent);
    auto second = node.resources.data.acquire(loader_context, extent);
    auto third = node.resources.data.acquire(loader_context, extent);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(fixture.keys(), [client_info] { return client_info; },
                     [](const NodeInfo&) {}, [](uint64_t) {}, 500ms, 100ms, 2s);
    // The node pings this client back over the same connection as a peer;
    // without an inbound handler that ping would close it and fail pending calls.
    RpcServer client_server(
        "127.0.0.1", client_info.port, fixture.keys(), client_info,
        [](const NodeInfo&, FrameType, const RpcMessage&) {
            return RpcMessage{MessageType::ok, {}};
        },
        [](const NodeInfo&) {});
    client_server.attach_client(client);
    client_server.start();
    Endpoint endpoint{"127.0.0.1", config.port};
    Writer request;
    request.fixed(id.bytes);

    auto blocked_loader = client.call_async(endpoint, MessageType::get_object, request.data(),
                                            FrameType::loader);
    REQUIRE(wait_until([&] { return node.resources.data.stats().loader_waits >= 1; }, 1s));

    // The viewer request uses the reserved DATA credit; fast CONTROL is outside the arbiter.
    auto viewer_started = Clock::now();
    auto viewer = client.call(endpoint, MessageType::get_object, request.data(),
                              FrameType::foreground, 500ms);
    CHECK(viewer.message.type == MessageType::object_reply);
    CHECK(Clock::now() - viewer_started < 200ms);

    auto control_started = Clock::now();
    auto control = client.call(endpoint, MessageType::ping, {}, 500ms);
    CHECK(control.message.type == MessageType::ok);
    CHECK(Clock::now() - control_started < 200ms);

    // Presence validation reads the whole object, so it goes through
    // speculative DATA admission, not a CONTROL worker.
    auto blocked_validation =
        client.call_async(endpoint, MessageType::have_object, request.data());
    REQUIRE(blocked_validation.wait_for(1s) == std::future_status::ready);
    CHECK(blocked_validation.get().message.type == MessageType::error);
    auto second_control = client.call(endpoint, MessageType::ping, {}, 500ms);
    CHECK(second_control.message.type == MessageType::ok);
    CHECK(blocked_loader.wait_for(20ms) == std::future_status::timeout);

    first.reset();
    REQUIRE(blocked_loader.wait_for(1s) == std::future_status::ready);
    CHECK(blocked_loader.get().message.type == MessageType::object_reply);
    CHECK(client.call(endpoint, MessageType::have_object, request.data(), 500ms).message.type ==
          MessageType::bool_reply);

    const auto stats = node.resources.data.stats();
    CHECK(stats.viewer_admissions >= 1);
    CHECK(stats.loader_waits >= 1);
    CHECK(stats.peak_used_bytes == config.data_inflight_bytes);

    client.stop();
}

MACHA_TEST("rpc_cluster", test_early_replication_quorum) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto pf = free_port();
    auto ps = free_port();

    // Object PUT quorum latency only: metadata consensus is kept out, and the
    // two peers are injected rather than discovered.
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    c1.dead_after = 3s;
    c1.metadata_min_write_replicas = 1;
    // Keep background membership exchange out of the measurement.
    c1.heartbeat = 10s;
    Service s1(c1, keys);
    s1.start();
    // start() returns while local storage is still recovering, and put()
    // refuses until it has.
    REQUIRE(s1.node().wait_local_state_ready(10s));

    NodeInfo fast_info;
    fast_info.id = random_node_id();
    fast_info.host = "127.0.0.1";
    fast_info.port = pf;
    fast_info.failure_domain = "fast-site";
    fast_info.capacity = 1024ULL * 1024 * 1024;
    fast_info.seen_unix_ms = unix_ms();

    NodeInfo slow_info;
    slow_info.id = random_node_id();
    slow_info.host = "127.0.0.1";
    slow_info.port = ps;
    slow_info.failure_domain = "slow-site";
    slow_info.capacity = 1024ULL * 1024 * 1024;
    slow_info.seen_unix_ms = unix_ms();

    TestGate slow_replica_gate;
    auto peer_handler = [&](const NodeInfo& self, bool slow) {
        return [&, self, slow](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object) {
                if (slow)
                    slow_replica_gate.enter_and_wait();
                return RpcMessage{MessageType::ok, {}};
            }
            if (request.type == MessageType::members) {
                Writer writer;
                writer.u32(1);
                encode_node_info(writer, self);
                return RpcMessage{MessageType::members_reply, writer.take()};
            }
            return RpcMessage{MessageType::ok, {}};
        };
    };

    RpcServer fast_server("127.0.0.1", pf, keys, fast_info, peer_handler(fast_info, false),
                          [](const NodeInfo&) {});
    RpcServer slow_server("127.0.0.1", ps, keys, slow_info, peer_handler(slow_info, true),
                          [](const NodeInfo&) {});
    fast_server.start();
    slow_server.start();

    s1.node().membership().observe(fast_info, true);
    s1.node().membership().observe(slow_info, true);
    REQUIRE(s1.node().membership().active().size() == 3);

    DistributedStore store(s1.node(), s1.resources().activity, s1.resources().data, s1.resources().memory, s1.resources().events);
    auto data = pattern(256 * 1024);
    auto started = Clock::now();
    REQUIRE(store.put(data) == object_id(data));
    auto elapsed = Clock::now() - started;
    CHECK(elapsed < 500ms);

    slow_replica_gate.open();
    slow_server.stop();
    fast_server.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_put_commits_at_floor_without_waiting_for_desired_replicas) {
    TestNode fixture("local-degraded", ConfigProfile::functional);
    auto& config = fixture.config();
    const auto& keys = fixture.keys();
    auto slow1_port = free_port();
    auto slow2_port = free_port();

    config.replication = 3;
    config.metadata_min_write_replicas = 1;
    config.min_write_replicas = 1;
    config.write_stall = 100ms;
    config.heartbeat = 10s;
    config.dead_after = 5s;

    auto& node = fixture.start();

    auto slow_info = [](uint16_t port, const char* domain) {
        NodeInfo info;
        info.id = random_node_id();
        info.host = "127.0.0.1";
        info.port = port;
        info.failure_domain = domain;
        info.capacity = 1024ULL * 1024 * 1024;
        info.seen_unix_ms = unix_ms();
        return info;
    };
    auto slow1 = slow_info(slow1_port, "slow-site-1");
    auto slow2 = slow_info(slow2_port, "slow-site-2");

    TestGate stalled_owner_gate;
    auto delayed_put = [&](const NodeInfo&, FrameType, const RpcMessage& request) {
        if (request.type == MessageType::put_object)
            stalled_owner_gate.enter_and_wait();
        return RpcMessage{MessageType::ok, {}};
    };
    RpcServer slow1_server("127.0.0.1", slow1_port, keys, slow1, delayed_put,
                           [](const NodeInfo&) {});
    RpcServer slow2_server("127.0.0.1", slow2_port, keys, slow2, delayed_put,
                           [](const NodeInfo&) {});
    slow1_server.start();
    slow2_server.start();

    node.membership().observe(slow1, true);
    node.membership().observe(slow2, true);
    REQUIRE(node.membership().active().size() == 3);

    // R=3 is a convergence target, not a foreground quorum: with W=1 the local
    // copy publishes at once, and slow replicas stay off the critical path.
    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    auto data = pattern(128 * 1024);
    auto id = object_id(data);
    auto started = Clock::now();
    CHECK(store.put(id, data));
    auto elapsed = Clock::now() - started;
    CHECK(elapsed < 500ms);
    CHECK(node.local_store().has(id));

    // The prompt-replication worker pushes the second copy to a placement
    // owner without waiting for repair.
    stalled_owner_gate.open();
    REQUIRE(wait_until([&] { return stalled_owner_gate.entered() >= 1; }, 10s));
    REQUIRE(wait_until([&] { return store.prompt_replication_stats().copies >= 1; }, 10s));
    slow2_server.stop();
    slow1_server.stop();
}

MACHA_TEST("rpc_cluster", test_prompt_replication_sends_nothing_to_a_full_owner) {
    // An owner that gossips no room is not a destination; the object is left to repair.
    TestNode fixture("full-owner", ConfigProfile::functional);
    auto& config = fixture.config();
    const auto& keys = fixture.keys();
    config.replication = 2;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    auto& node = fixture.start();

    const auto full_port = free_port();
    NodeInfo full;
    full.id = random_node_id();
    full.host = "127.0.0.1";
    full.port = full_port;
    full.failure_domain = "full-site";
    full.capacity = 1024ULL * 1024 * 1024;
    full.used = full.capacity - 81;
    full.seen_unix_ms = unix_ms();
    std::atomic<int> puts{0};
    RpcServer full_server("127.0.0.1", full_port, keys, full,
                          [&](const NodeInfo&, FrameType, const RpcMessage& request) {
                              if (request.type == MessageType::put_object) ++puts;
                              if (request.type == MessageType::have_object) {
                                  Writer w;
                                  w.u8(0);
                                  return RpcMessage{MessageType::bool_reply, w.take()};
                              }
                              return RpcMessage{MessageType::ok, {}};
                          },
                          [](const NodeInfo&) {});
    full_server.start();
    node.membership().observe(full, true);
    REQUIRE(node.membership().active().size() == 2);

    DistributedStore store(node, node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    auto data = pattern(128 * 1024, 7);
    auto id = object_id(data);
    CHECK(store.put(id, data));
    REQUIRE(wait_until([&] { return store.repair_diagnostics().prompt_skipped_no_room >= 1; }, 10s));
    const auto diagnostics = store.repair_diagnostics();
    CHECK(diagnostics.prompt_dropped >= 1);
    CHECK(diagnostics.prompt_copies == 0);
    CHECK(diagnostics.prompt_failures == 0); // nothing was sent to be refused
    CHECK(puts.load() == 0);
    full_server.stop();
}

MACHA_TEST("rpc_cluster", test_put_falls_back_after_remote_launch_failure) {
    TestService fixture("local");
    auto& config = fixture.config();
    auto dead_port = free_port();

    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.connect_timeout = 100ms;
    config.heartbeat = 30s;
    config.dead_after = 60s;

    auto& service = fixture.start();

    NodeInfo unreachable;
    unreachable.id = random_node_id();
    unreachable.host = "127.0.0.1";
    unreachable.port = dead_port; // free_port() closes the listener: connect must fail.
    unreachable.failure_domain = "unreachable-site";
    unreachable.capacity = config.storage_backends.front().limit;
    unreachable.seen_unix_ms = unix_ms();
    service.node().membership().observe(unreachable, true);
    REQUIRE(service.node().membership().active().size() == 2);

    Bytes data;
    std::vector<NodeInfo> ranked;
    for (uint32_t salt = 0; salt < 4096; ++salt) {
        data = pattern(256 * 1024 + salt);
        auto id = object_id(data);
        ranked = capacity_placement_nodes(id.bytes, service.node().membership().active(), 1);
        if (ranked.size() == 2 && ranked[0].id == unreachable.id &&
            ranked[1].id == service.node().node_id())
            break;
        ranked.clear();
    }
    REQUIRE(ranked.size() == 2);

    // A synchronous call_async() failure counts as a failed replica, so the
    // quorum loop tries the fallback owner. The watchdog bounds a hang.
    std::atomic_bool cancelled{false};
    std::jthread watchdog([&](std::stop_token stop) {
        const auto deadline = Clock::now() + 2s;
        while (!stop.stop_requested() && Clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        if (!stop.stop_requested())
            cancelled.store(true, std::memory_order_relaxed);
    });

    DistributedStore store(service.node(), service.resources().activity, service.resources().data, service.resources().memory, service.resources().events);
    auto id = object_id(data);
    const bool stored = store.put(id, data, &cancelled);
    watchdog.request_stop();

    CHECK(stored);
    CHECK(!cancelled.load(std::memory_order_relaxed));
    CHECK(service.node().local_store().has(id));
    service.stop();
}

MACHA_TEST("rpc_cluster", test_joiner_cannot_form_genesis) {
    TestService fixture("joiner");
    auto& config = fixture.config();
    config.bootstrap = {{"127.0.0.1", config.port}};
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    auto& service = fixture.start();
    // A pristine bootstrap node holds genesis only as local codec material,
    // never advertised as accepted authority.
    CHECK(service.node().metadata_replica().accepted_heads().empty());
    bool rejected = false;
    try {
        (void)service.filesystem().getattr("/");
    } catch (const std::exception& error) {
        rejected =
            std::string(error.what()).find("waiting for bootstrap peer") != std::string::npos;
    }
    CHECK(rejected);
}

MACHA_TEST("rpc_cluster", test_bootstrap_joiner_requires_complete_checkpoint_survey) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto unreachable_port = free_port();
    auto config = config_for(cluster.path() / "checkpoint-survey", cluster.keyfile(), free_port(),
                             {{"127.0.0.1", unreachable_port}});
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.dead_after = 10s;

    BareNode node(config, keys);
    node.start();

    // An unreachable peer in active membership, with an identity that makes
    // this fresh node a placement candidate: both metadata surveys fail, and
    // the replication-1 joiner must not form an empty generation-2 namespace.
    static constexpr char label[] = "macha/metadata-placement/v1";
    const auto placement_key = sha256({reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1});
    NodeInfo phantom;
    phantom.host = "127.0.0.1";
    phantom.failure_domain = "unreachable-bootstrap";
    phantom.port = unreachable_port;
    phantom.capacity = 512ULL * 1024 * 1024;
    phantom.seen_unix_ms = unix_ms();
    phantom.metadata_write_replicas_required = 1;

    const auto self = node.membership().self();
    bool selected_self = false;
    for (uint32_t candidate = 1; candidate < 100000 && !selected_self; ++candidate) {
        phantom.id = {};
        phantom.id.bytes[0] = static_cast<uint8_t>(candidate >> 24U);
        phantom.id.bytes[1] = static_cast<uint8_t>(candidate >> 16U);
        phantom.id.bytes[2] = static_cast<uint8_t>(candidate >> 8U);
        phantom.id.bytes[3] = static_cast<uint8_t>(candidate);
        if (phantom.id == self.id)
            continue;
        auto ranked = rendezvous_nodes(placement_key.bytes, {self, phantom}, 1);
        selected_self = !ranked.empty() && ranked.front().id == self.id;
    }
    REQUIRE(selected_self);
    node.membership().observe(phantom, true);
    REQUIRE(node.wait_local_state_ready(10s));

    MetadataManager metadata(node);
    bool rejected = false;
    try {
        (void)metadata.snapshot_view();
    } catch (const std::exception& error) {
        rejected =
            std::string(error.what()).find("bootstrap checkpoint survey") != std::string::npos;
    }
    CHECK(rejected);
    CHECK(node.metadata_replica().current().generation <= 1);
    CHECK(node.metadata_replica().committed().generation <= 1);

    node.stop();
}

MACHA_TEST("rpc_cluster", test_metadata_write_floor_policy_mismatch_fails_closed) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.metadata_min_write_replicas = 1;
    c2.metadata_min_write_replicas = 2;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    auto rejected_for_policy = [](Service& service) {
        try {
            service.filesystem().mkdir("/must-not-form", 0755, getuid(), getgid());
        } catch (const std::exception& error) {
            return std::string(error.what()).find("write-floor policy mismatch") !=
                   std::string::npos;
        }
        return false;
    };
    CHECK(rejected_for_policy(s1));
    CHECK(rejected_for_policy(s2));
    CHECK(s1.node().metadata_replica().committed().generation <= 1);
    CHECK(s2.node().metadata_replica().committed().generation <= 1);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_established_metadata_floor_ignores_misconfigured_peer) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 2;
    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Reachability precedes metadata recovery; make both metadata planes
    // ready before establishing history.
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_metadata_writable(s1));
    REQUIRE(wait_metadata_writable(s2));
    s1.filesystem().mkdir("/established", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/established").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));

    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3,
                         {{"127.0.0.1", p1}, {"127.0.0.1", p2}});
    c3.metadata_min_write_replicas = 1; // deliberately wrong
    Service s3(c3, keys);
    s3.start();
    REQUIRE(wait_until([&] { return s1.node().membership().active().size() >= 3; }));

    // The quarantined peer cannot block the two compatible replicas that satisfy W=2.
    s1.filesystem().mkdir("/still-writable", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/still-writable").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));
    auto& m1 = s1.metadata_manager();
    m1.note_replica_validation(false, "policy mismatch test");
    const auto status = m1.cluster_status();
    CHECK(status.availability == MetadataAvailability::writable);
    CHECK(status.replicas_online == 2);
    CHECK(!status.stable);

    bool bad_peer_rejected = false;
    try {
        s3.filesystem().mkdir("/must-not-weaken-policy", 0755, getuid(), getgid());
    } catch (...) {
        bad_peer_rejected = true;
    }
    CHECK(bad_peer_rejected);

    s3.stop();
    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_two_node_mutual_bootstrap_metadata_write_floor) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 2;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 2;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    // Both peers may attempt genesis at once; competing proposals converge on one history.
    std::exception_ptr first_error;
    std::exception_ptr second_error;
    std::atomic<unsigned> ready{};
    std::atomic<bool> go{};
    std::thread first([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            s1.filesystem().mkdir("/from-node-1", 0755, getuid(), getgid());
        } catch (...) {
            first_error = std::current_exception();
        }
    });
    std::thread second([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            s2.filesystem().mkdir("/from-node-2", 0755, getuid(), getgid());
        } catch (...) {
            second_error = std::current_exception();
        }
    });
    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    if (first_error)
        std::rethrow_exception(first_error);
    if (second_error)
        std::rethrow_exception(second_error);

    CHECK(s1.filesystem().getattr("/from-node-2").type == EntryType::directory);
    CHECK(s2.filesystem().getattr("/from-node-1").type == EntryType::directory);

    s1.filesystem().mkdir("/media", 0755, getuid(), getgid());
    s1.filesystem().create_file("/media/two-replicas.bin", 0644, getuid(), getgid());
    auto input = pattern(128 * 1024);
    auto writer = s1.filesystem().open_write("/media/two-replicas.bin", true);
    REQUIRE(writer->write(0, input) == input.size());
    writer->commit();

    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/media/two-replicas.bin").size == input.size();
        } catch (...) {
            return false;
        }
    }));

    MetadataManager m1(s1.node());
    auto snapshot = m1.snapshot();
    CHECK(snapshot.metadata_voters.empty());
    CHECK(snapshot.data_replication == 2);

    auto entry = s1.filesystem().getattr("/media/two-replicas.bin");
    REQUIRE(entry.extents.size() == 1);
    CHECK(s1.node().local_store().has(entry.extents.front().id));
    CHECK(s2.node().local_store().has(entry.extents.front().id));

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_service_metadata_repair_coalesces_real_generation_burst) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "coalesced-repair-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "coalesced-repair-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;
    c1.ingest.enabled = c2.ingest.enabled = false;
    c1.torrent.enabled = c2.torrent.enabled = false;

    TestGate repair_gate;
    std::atomic_bool gate_repair{};
    std::atomic_bool gate_once{};
    Service s1(c1, keys, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repair.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel)) {
            repair_gate.enter_and_wait();
        }
    });
    Service s2(c2, keys);
    struct GateOpener {
        TestGate& gate;
        ~GateOpener() {
            gate.open();
        }
    } open_on_exit{repair_gate};

    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Equal generation is not convergence; wait for one shared head so later
    // reconciliation does not pollute the burst counters.
    REQUIRE(wait_until(
        [&] {
            const auto d1 = s1.metadata_convergence_diagnostics();
            const auto d2 = s2.metadata_convergence_diagnostics();
            return s1.node().metadata_replica().committed_generation() > 1 &&
                   s1.node().metadata_replica().committed_generation() ==
                       s2.node().metadata_replica().committed_generation() &&
                   s1.node().metadata_replica().committed().hash ==
                       s2.node().metadata_replica().committed().hash &&
                   s1.node().metadata_replica().accepted_heads().size() == 1 &&
                   s2.node().metadata_replica().accepted_heads().size() == 1 &&
                   !d1.scheduled && d1.runs_scheduled == d1.runs_completed && !d2.scheduled &&
                   d2.runs_scheduled == d2.runs_completed;
        },
        10s));

    const auto before = s1.metadata_convergence_diagnostics();
    const auto baseline_generation = s2.node().metadata_replica().committed_generation();
    const auto announcements_before = s2.node().metadata_announcements();
    gate_repair.store(true, std::memory_order_release);

    s2.filesystem().mkdir("/coalesced-0", 0755, getuid(), getgid());
    REQUIRE(repair_gate.wait_for_entries(1, 5s));
    const auto claimed = s1.metadata_convergence_diagnostics();
    CHECK(claimed.runs_scheduled == before.runs_scheduled + 1);
    CHECK(claimed.runs_completed == before.runs_completed);
    const auto metadata_events = [&] {
        return s1.resources().events.count(NodeEvent::metadata) +
               s1.resources().events.count(NodeEvent::topology);
    };
    const auto events_claimed = metadata_events();

    constexpr size_t burst = 32;
    for (size_t i = 1; i <= burst; ++i) {
        s2.filesystem().mkdir("/coalesced-" + std::to_string(i), 0755, getuid(), getgid());
    }
    const auto final_generation = s2.node().metadata_replica().committed_generation();
    CHECK(final_generation == baseline_generation + burst + 1);
    REQUIRE(
        wait_until([&] { return s1.node().known_metadata_generation() >= final_generation; }, 5s));
    CHECK(s2.node().metadata_announcements() == announcements_before + burst + 1);

    // While the run is held the node counts the burst; the pass turns it into
    // demand when it next looks, so nothing more is scheduled meanwhile.
    CHECK(metadata_events() > events_claimed);
    const auto gated = s1.metadata_convergence_diagnostics();
    CHECK(gated.runs_scheduled == before.runs_scheduled + 1);
    CHECK(gated.runs_completed == before.runs_completed);

    repair_gate.open();
    REQUIRE(wait_until(
        [&] {
            const auto diagnostics = s1.metadata_convergence_diagnostics();
            return !diagnostics.scheduled &&
                   diagnostics.runs_completed == before.runs_completed + 2 &&
                   s1.node().metadata_replica().committed_generation() == final_generation;
        },
        10s));

    const auto settled = s1.metadata_convergence_diagnostics();
    CHECK(settled.runs_scheduled == before.runs_scheduled + 2);
    CHECK(settled.runs_completed == before.runs_completed + 2);
    CHECK(settled.completed_epoch == settled.requested_epoch);
    CHECK(settled.latest_generation >= final_generation);
    CHECK(!settled.scheduled);
    CHECK(s1.filesystem().getattr("/coalesced-32").type == EntryType::directory);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_service_same_generation_sibling_notice_triggers_reconciliation) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "sibling-notice-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "sibling-notice-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;
    c1.ingest.enabled = c2.ingest.enabled = false;
    c1.torrent.enabled = c2.torrent.enabled = false;

    TestGate repair_gate1;
    TestGate repair_gate2;
    std::atomic_bool gate_repairs{};
    std::atomic_bool gate_once1{};
    std::atomic_bool gate_once2{};
    Service s1(c1, keys, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repairs.load(std::memory_order_acquire) &&
            !gate_once1.exchange(true, std::memory_order_acq_rel)) {
            repair_gate1.enter_and_wait();
        }
    });
    Service s2(c2, keys, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repairs.load(std::memory_order_acquire) &&
            !gate_once2.exchange(true, std::memory_order_acq_rel)) {
            repair_gate2.enter_and_wait();
        }
    });
    struct GateOpener {
        TestGate& first;
        TestGate& second;
        ~GateOpener() {
            first.open();
            second.open();
        }
    } open_on_exit{repair_gate1, repair_gate2};

    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_until(
        [&] {
            const auto d1 = s1.metadata_convergence_diagnostics();
            const auto d2 = s2.metadata_convergence_diagnostics();
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2 &&
                   s1.node().metadata_replica().committed_generation() > 1 &&
                   s1.node().metadata_replica().committed().hash ==
                       s2.node().metadata_replica().committed().hash &&
                   !d1.scheduled && d1.runs_scheduled == d1.runs_completed && !d2.scheduled &&
                   d2.runs_scheduled == d2.runs_completed;
        },
        10s));

    const auto base = s1.node().metadata_replica().committed();
    auto make_sibling = [&](NodeRuntime& node, const std::string& path) {
        auto snapshot = decode_snapshot(base.payload);
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        snapshot.entries[path] = entry;
        ++snapshot.mutation_sequences[node.node_id()];

        MetadataRecord sibling;
        sibling.generation = base.generation + 1;
        sibling.previous = base.hash;
        sibling.payload = encode_snapshot(snapshot);
        sibling.hash = metadata_hash(sibling.generation, sibling.previous, sibling.payload);
        REQUIRE(node.metadata_replica().store_commit(sibling));
        MetadataAcceptance acceptance;
        acceptance.generation = sibling.generation;
        acceptance.hash = sibling.hash;
        acceptance.required = 1;
        acceptance.replicas = {node.node_id()};
        REQUIRE(node.accept_metadata_commit(acceptance));
        return sibling;
    };

    gate_repairs.store(true, std::memory_order_release);
    const auto left = make_sibling(s1.node(), "/left-sibling");
    REQUIRE(repair_gate1.wait_for_entries(1, 5s));
    REQUIRE(repair_gate2.wait_for_entries(1, 5s));

    // Prime node 1 with the sibling generation, so the next notice's only news
    // is a changed accepted-head topology at the same generation.
    s2.node().announce_metadata_generation(left.generation);
    REQUIRE(
        wait_until([&] { return s1.node().remote_metadata_generation() == left.generation; }, 5s));
    // The node counts a changed accepted head as an event; the held pass turns
    // it into convergence demand once released.
    const auto convergence_events = [](Service& service) {
        return service.resources().events.count(NodeEvent::metadata) +
               service.resources().events.count(NodeEvent::topology);
    };
    const auto before_sibling_notice = convergence_events(s1);

    const auto right = make_sibling(s2.node(), "/right-sibling");
    REQUIRE(right.generation == left.generation);
    REQUIRE(right.hash != left.hash);
    REQUIRE(wait_until([&] { return convergence_events(s1) > before_sibling_notice; }, 2s));

    // Identical acceptance evidence produces no local event and no rebroadcast.
    std::this_thread::sleep_for(100ms);
    const auto before_duplicate1 = convergence_events(s1);
    const auto before_duplicate2 = convergence_events(s2);
    const auto announcements_before_duplicate = s2.node().metadata_announcements();
    MetadataAcceptance duplicate;
    duplicate.generation = right.generation;
    duplicate.hash = right.hash;
    duplicate.required = 1;
    duplicate.replicas = {s2.node().node_id()};
    REQUIRE(s2.node().accept_metadata_commit(duplicate));
    CHECK(s2.node().metadata_announcements() == announcements_before_duplicate);
    std::this_thread::sleep_for(100ms);
    CHECK(convergence_events(s1) == before_duplicate1);
    CHECK(convergence_events(s2) == before_duplicate2);

    repair_gate1.open();
    REQUIRE(wait_until(
        [&] {
            try {
                const auto heads = s1.node().metadata_replica().accepted_heads();
                return heads.size() == 1 && heads.front().generation > left.generation &&
                       s1.filesystem().getattr("/left-sibling").type == EntryType::directory &&
                       s1.filesystem().getattr("/right-sibling").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        10s));

    repair_gate2.open();
    REQUIRE(wait_until(
        [&] {
            try {
                return s2.node().metadata_replica().accepted_heads().size() == 1 &&
                       s2.filesystem().getattr("/left-sibling").type == EntryType::directory &&
                       s2.filesystem().getattr("/right-sibling").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        10s));

    // The first write after a reconciliation is a compact delta over the merge commit.
    auto merge_heads = s1.node().metadata_replica().accepted_heads();
    REQUIRE(merge_heads.size() == 1);
    const auto merge_head = merge_heads.front();
    const auto merge_entry = s1.node().metadata_replica().history_entry(merge_head.hash);
    REQUIRE(merge_entry.has_value());
    REQUIRE(merge_entry->merge_parents.size() == 1);

    s1.filesystem().mkdir("/after-merge", 0755, getuid(), getgid());
    auto after_heads = s1.node().metadata_replica().accepted_heads();
    REQUIRE(after_heads.size() == 1);
    const auto after_entry = s1.node().metadata_replica().history_entry(after_heads.front().hash);
    REQUIRE(after_entry.has_value());
    CHECK(after_entry->previous == merge_head.hash);
    CHECK(after_entry->merge_parents.empty());
    CHECK(after_entry->body == MetadataHistoryEntry::Body::delta);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_concurrent_reads_during_divergence_produce_one_reconciliation) {
    // Several readers on one node observe the same two-head divergence, and
    // reconciliation_mutex_ makes exactly one mint the merge commit. Bare
    // NodeRuntimes and one manager, no Service: a Service's maintenance would
    // reconcile in the background.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "concurrent-reconcile-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "concurrent-reconcile-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    // No cache TTL: every reader must actually read.
    c1.metadata_cache = std::chrono::milliseconds(0);

    BareNode n1(c1, keys);
    BareNode n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(n1.wait_local_state_ready(10s));
    REQUIRE(n2.wait_local_state_ready(10s));
    REQUIRE(wait_until(
        [&] {
            return n1.membership().active().size() >= 2 &&
                   n2.membership().active().size() >= 2;
        },
        10s));

    MetadataManager metadata1(n1);
    // One mutation, so the siblings fork from a real post-genesis record.
    metadata1.mutate([](MetadataSnapshot& snapshot) {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        snapshot.entries["/base-concurrent"] = entry;
    });
    REQUIRE(n1.metadata_replica().committed_generation() > 1);

    // A two-head divergence on node 1 alone, from a locally authored sibling.
    const auto base = n1.metadata_replica().committed();
    auto make_sibling = [&](NodeRuntime& node, const std::string& path) {
        auto snapshot = decode_snapshot(base.payload);
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        snapshot.entries[path] = entry;
        ++snapshot.mutation_sequences[node.node_id()];

        MetadataRecord sibling;
        sibling.generation = base.generation + 1;
        sibling.previous = base.hash;
        sibling.payload = encode_snapshot(snapshot);
        sibling.hash = metadata_hash(sibling.generation, sibling.previous, sibling.payload);
        REQUIRE(node.metadata_replica().store_commit(sibling));
        MetadataAcceptance acceptance;
        acceptance.generation = sibling.generation;
        acceptance.hash = sibling.hash;
        acceptance.required = 1;
        acceptance.replicas = {node.node_id()};
        REQUIRE(node.accept_metadata_commit(acceptance));
        return sibling;
    };

    const auto left = make_sibling(n1, "/left-concurrent");
    const auto right = make_sibling(n1, "/right-concurrent");
    REQUIRE(right.generation == left.generation);
    REQUIRE(right.hash != left.hash);
    REQUIRE(n1.metadata_replica().accepted_heads().size() == 2);

    const auto history_before = n1.metadata_replica().diagnostics().history_records;

    // Concurrent reads all observe the divergence; exactly one merge commit results.
    constexpr int reader_count = 8;
    std::vector<std::thread> readers;
    std::vector<MetadataRecord> results(reader_count);
    readers.reserve(reader_count);
    for (int i = 0; i < reader_count; ++i)
        readers.emplace_back([&, i] { results[i] = metadata1.read_record(); });
    for (auto& reader : readers)
        reader.join();

    const auto history_after = n1.metadata_replica().diagnostics().history_records;
    CHECK(history_after - history_before == 1);
    CHECK(n1.metadata_replica().accepted_heads().size() == 1);
    for (const auto& record : results)
        CHECK(record.hash == results.front().hash);

    n2.stop();
    n1.stop();
}

MACHA_TEST("rpc_cluster", test_service_startup_stall_terminates_within_configured_timeout) {
    TestCluster cluster;
    auto c1 = config_for(cluster.path() / "stalled-startup", cluster.keyfile(), free_port());
    c1.service_startup_timeout = 200ms;
    c1.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = false;
    c1.ingest.enabled = false;
    c1.torrent.enabled = false;

    // Stall local-state readiness forever; the injected StartupStallHandler
    // observes the timeout without terminating the process.
    TestGate stall_gate;
    std::atomic_bool handler_called{false};
    std::string diagnostic;
    Service service(
        c1, cluster.keys(),
        [&](std::string_view stage) {
            if (stage == "data-storage")
                stall_gate.enter_and_wait();
        },
        {},
        [&](std::string_view value) {
            diagnostic = std::string(value);
            handler_called.store(true, std::memory_order_release);
        });
    struct ReleaseGate {
        TestGate& gate;
        ~ReleaseGate() {
            gate.open();
        }
    } release{stall_gate};

    service.start();
    REQUIRE(stall_gate.wait_for_entries(1, 5s));

    const auto started = Clock::now();
    bool threw = false;
    try {
        (void)service.filesystem();
    } catch (const std::exception&) {
        threw = true;
    }
    const auto elapsed = Clock::now() - started;

    CHECK(threw);
    CHECK(handler_called.load(std::memory_order_acquire));
    // Resolves near the configured bound rather than hanging.
    CHECK(elapsed >= 150ms);
    CHECK(elapsed < 5s);
    CHECK(diagnostic.find("data_storage=recovering") != std::string::npos);

    // Release the stalled initialise_services() thread so shutdown can join it.
    stall_gate.open();
    service.stop();
}

MACHA_TEST("rpc_cluster", test_service_startup_gate_waits_while_recovery_progresses) {
    // The startup gate spares a slow but progressing recovery and still kills
    // one that has stopped.
    TestCluster cluster;
    auto c1 = config_for(cluster.path() / "progressing-startup", cluster.keyfile(), free_port());
    c1.service_startup_timeout = 0ms;            // no absolute ceiling
    c1.service_startup_no_progress = 1200ms;     // gate on silence only
    c1.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = false;
    c1.ingest.enabled = false;
    c1.torrent.enabled = false;

    TestGate stall_gate;
    std::atomic_bool handler_called{false};
    Service service(
        c1, cluster.keys(),
        [&](std::string_view stage) {
            if (stage == "data-storage")
                stall_gate.enter_and_wait();
        },
        {},
        [&](std::string_view) { handler_called.store(true, std::memory_order_release); });
    struct ReleaseGate {
        TestGate& gate;
        ~ReleaseGate() {
            gate.open();
        }
    } release{stall_gate};

    service.start();
    REQUIRE(stall_gate.wait_for_entries(1, 5s));

    // Keep "recovery" ticking from outside for 3 s: the gate must stay quiet.
    std::atomic_bool ticking{true};
    std::thread ticker([&] {
        while (ticking.load(std::memory_order_acquire)) {
            note_startup_progress();
            std::this_thread::sleep_for(100ms);
        }
    });
    std::thread waiter([&] {
        try {
            (void)service.filesystem();
        } catch (const std::exception&) {
        }
    });
    std::this_thread::sleep_for(3s);
    CHECK(!handler_called.load(std::memory_order_acquire));

    // Silence: the gate fires within roughly the no-progress window.
    ticking.store(false, std::memory_order_release);
    ticker.join();
    const auto silent_since = Clock::now();
    REQUIRE(wait_until([&] { return handler_called.load(std::memory_order_acquire); }, 10s));
    CHECK(Clock::now() - silent_since >= 1000ms);
    waiter.join();

    stall_gate.open();
    service.stop();
}

MACHA_TEST("rpc_cluster", test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();

    auto peers = [&](uint16_t self) {
        std::vector<Endpoint> out;
        for (const auto port : {p1, p2, p3})
            if (port != self)
                out.push_back({"127.0.0.1", port});
        return out;
    };
    auto c1 = config_for(cluster.path() / "lagging-burst-n1", cluster.keyfile(), p1, peers(p1));
    auto c2 = config_for(cluster.path() / "lagging-burst-n2", cluster.keyfile(), p2, peers(p2));
    auto c3 = config_for(cluster.path() / "lagging-burst-n3", cluster.keyfile(), p3, peers(p3));
    for (auto* config : {&c1, &c2, &c3}) {
        config->replication = 3;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        // Well above scheduler jitter, yet short enough for the 5 s wait below
        // to see s3 drop out.
        config->dead_after = 2s;
        config->catalogue.scanner.enabled = false;
        config->catalogue.api.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    auto s3 = std::make_unique<Service>(c3, keys);
    s1.start();
    s2.start();
    s3->start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    (void)s3->filesystem();
    REQUIRE(wait_until(
        [&] {
            return s1.node().membership().active().size() == 3 &&
                   s2.node().membership().active().size() == 3 &&
                   s3->node().membership().active().size() == 3;
        },
        10s));

    // The first write completes the replica set's formation, which a peer's
    // transient RPC failure can report as not ready; callers retry that.
    REQUIRE(wait_until(
        [&] {
            try {
                s1.filesystem().mkdir("/lagging-base", 0755, getuid(), getgid());
                return true;
            } catch (const MetadataNotReady&) {
                return false;
            }
        },
        10s));
    REQUIRE(wait_until(
        [&] {
            try {
                return s3->filesystem().getattr("/lagging-base").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        10s));
    const auto base = s3->node().metadata_replica().committed();

    s3->stop();
    s3.reset();
    REQUIRE(wait_until(
        [&] {
            return s1.node().membership().active().size() == 2 &&
                   s2.node().membership().active().size() == 2;
        },
        5s));

    constexpr size_t burst = 64;
    for (size_t index = 0; index < burst; ++index) {
        s1.filesystem().mkdir("/lagging-burst-" + std::to_string(index), 0755, getuid(), getgid());
    }
    const auto final = s1.node().metadata_replica().committed();
    CHECK(final.generation == base.generation + burst);
    REQUIRE(wait_until([&] { return s2.node().metadata_replica().committed().hash == final.hash; },
                       10s));

    s3 = std::make_unique<Service>(c3, keys);
    s3->start();
    (void)s3->filesystem();
    REQUIRE(wait_until(
        [&] {
            try {
                const auto diagnostics = s3->metadata_convergence_diagnostics();
                return s3->node().metadata_replica().committed().hash == final.hash &&
                       s3->filesystem().getattr("/lagging-burst-63").type == EntryType::directory &&
                       !diagnostics.scheduled &&
                       diagnostics.runs_scheduled == diagnostics.runs_completed;
            } catch (...) {
                return false;
            }
        },
        15s));

    const auto diagnostics = s3->metadata_convergence_diagnostics();
    CHECK(diagnostics.runs_scheduled <= 4);
    CHECK(diagnostics.runs_completed <= 4);
    CHECK(diagnostics.runs_completed < burst);
    const auto heads = s3->node().metadata_replica().accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == final.hash);
    CHECK(s3->node().metadata_replica().history_contains(final.hash));
    CHECK(s3->node().metadata_replica().history_is_ancestor(base.hash, final.hash));

    const auto transfer1 = s1.metadata_manager().history_transfer_diagnostics();
    const auto transfer2 = s2.metadata_manager().history_transfer_diagnostics();
    CHECK(std::max(transfer1.peak_in_flight, transfer2.peak_in_flight) > 1);
    CHECK(std::max(transfer1.peak_in_flight, transfer2.peak_in_flight) <= 8);

    s3->stop();
    s2.stop();
    s1.stop();
}

MACHA_HEAVY_TEST("rpc_cluster", test_an_ingest_blocked_on_unwritable_metadata_resumes_when_it_returns) {
    // An ingest that meets MetadataNotReady blocks with metadata_unavailable,
    // is retried, and completes once metadata is writable again.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
        config->catalogue.scanner.enabled = false;
        config->ingest.enabled = false;
    }
    Service s1(c1, keys);
    auto s2 = std::make_unique<Service>(c2, keys);
    s1.start();
    s2->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 && s2->node().membership().active().size() == 2;
    }));
    s2->stop();
    s2.reset();
    REQUIRE(wait_until([&] { return s1.node().membership().active().size() == 1; }));

    const auto root = cluster.path() / "source";
    std::filesystem::create_directories(root);
    const auto bytes = pattern(256 * 1024, 5);
    {
        std::ofstream out(root / "Blocked Movie 2024.mkv", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = cluster.path() / "staging";
    ingest_config.source_roots = {root};
    ingest_config.blocked_retry = 200ms;
    IngestManager ingest(s1.node(), s1.filesystem(), s1.catalogue_hints(), ingest_config);
    const auto id = ingest.submit_path(root);
    ingest.start();

    REQUIRE(wait_until([&] {
        const auto job = ingest.job(id);
        return job && job->state == IngestJobState::blocked && job->error_code == "metadata_unavailable";
    }, 30s));
    // Retried, and still waiting rather than failed.
    std::this_thread::sleep_for(1s);
    CHECK(ingest.job(id)->state != IngestJobState::failed);

    s2 = std::make_unique<Service>(c2, keys);
    s2->start();
    REQUIRE(wait_until([&] {
        const auto job = ingest.job(id);
        return job && job->files_total == 1 && job->files_completed == 1;
    }, 60s));
    CHECK(ingest.job(id)->state != IngestJobState::failed);
    ingest.stop();
    s2->stop();
    s1.stop();
}

MACHA_HEAVY_TEST("rpc_cluster", test_metadata_file_touch_requires_retention_before_acceptance) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();

    auto peers = [&](uint16_t self) {
        std::vector<Endpoint> out;
        for (auto port : {p1, p2, p3})
            if (port != self)
                out.push_back({"127.0.0.1", port});
        return out;
    };
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, peers(p1));
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, peers(p2));
    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3, peers(p3));
    for (auto* config : {&c1, &c2, &c3}) {
        config->replication = 3;
        config->min_write_replicas = 3;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    auto s3 = std::make_unique<Service>(c3, keys);
    s1.start();
    s2.start();
    s3->start();
    // The write needs every service ready and three extent-hosting members;
    // membership alone arrives before either.
    const auto hosting = [](Service& service) {
        const auto active = service.node().membership().active();
        return std::count_if(active.begin(), active.end(),
                             [](const NodeInfo& node) { return node_hosts_extents(node); });
    };
    REQUIRE(wait_until([&] {
        return s1.ready() && s2.ready() && s3->ready() && hosting(s1) == 3 &&
               s2.node().membership().active().size() == 3 &&
               s3->node().membership().active().size() == 3;
    }));

    s1.filesystem().create_file("/retained.bin", 0644, getuid(), getgid());
    auto input = pattern(128 * 1024);
    auto writer = s1.filesystem().open_write("/retained.bin", true);
    REQUIRE(writer->write(0, input) == input.size());
    writer->commit();
    const auto entry = s1.filesystem().getattr("/retained.bin");
    REQUIRE(entry.extents.size() == 1);
    const auto extent = entry.extents.front().id;
    REQUIRE(wait_until([&] {
        return s1.node().local_store().valid(extent) && s2.node().local_store().valid(extent) &&
               s3->node().local_store().valid(extent);
    }));
    // The accepted file reference has already installed a DATA claim.
    CHECK(s1.node().claims().retained(RetentionClass::data, extent));
    CHECK(s2.node().claims().retained(RetentionClass::data, extent));
    CHECK(s3->node().claims().retained(RetentionClass::data, extent));

    const auto before = s1.node().metadata_replica().committed();
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));

    // Metadata W=2 holds, but the file touch needs a fresh retention dot on
    // DATA W=3; when that barrier fails the metadata head must not advance.
    bool refused = false;
    try {
        s1.filesystem().chmod("/retained.bin", 0600);
    } catch (const MetadataNotReady&) {
        refused = true;
    } catch (...) {
        refused = true;
    }
    CHECK(refused);
    CHECK(s1.node().metadata_replica().committed().hash == before.hash);
    CHECK((s1.filesystem().getattr("/retained.bin").mode & 0777U) == 0644U);

    s3 = std::make_unique<Service>(c3, keys);
    s3->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 3 &&
               s2.node().membership().active().size() == 3 &&
               s3->node().membership().active().size() == 3;
    }));
    // Membership can precede the control worker that serves retention queries,
    // so prove that path before the W=3 mutation; each attempt is deadline-bounded.
    REQUIRE(wait_until([&] {
        try {
            const auto returning_id = s3->node().node_id();
            const auto active = s1.node().membership().active();
            const auto returning = std::find_if(active.begin(), active.end(), [&](const auto& n) {
                return n.id == returning_id;
            });
            return returning != active.end() &&
                   s1.filesystem().store().has_on(*returning, extent);
        } catch (...) {
            return false;
        }
    }, 10s));
    s1.filesystem().chmod("/retained.bin", 0600);
    CHECK((s1.filesystem().getattr("/retained.bin").mode & 0777U) == 0600U);
    CHECK(s1.node().metadata_replica().committed().hash != before.hash);

    s3->stop();
    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_commit_replicas_ordered_local_then_nearest) {
    NodeInfo local, lan, wan, unknown;
    local.id.bytes.fill(0x50);
    lan.id.bytes.fill(0x70);
    wan.id.bytes.fill(0x10); // lowest NodeId: first under the old ordering
    unknown.id.bytes.fill(0x90);
    local.host = "local";
    lan.host = "lan";
    wan.host = "wan";
    unknown.host = "unknown";
    const auto latency = [&](const NodeId& id) -> std::optional<std::chrono::milliseconds> {
        if (id == lan.id)
            return 2ms;
        if (id == wan.id)
            return 120ms;
        return std::nullopt;
    };
    const auto ordered = order_commit_replicas({wan, unknown, lan, local}, local.id, latency);
    REQUIRE(ordered.size() == 4);
    CHECK(ordered[0].host == "local");
    CHECK(ordered[1].host == "lan");
    CHECK(ordered[2].host == "wan");
    CHECK(ordered[3].host == "unknown");
    // No measurements: local first, the rest in their given order.
    const auto cold = order_commit_replicas({wan, lan, local}, local.id, {});
    CHECK(cold[0].host == "local");
    CHECK(cold[1].host == "wan");
    CHECK(cold[2].host == "lan");
}

MACHA_TEST("rpc_cluster", test_partition_delete_defers_destructive_gc_until_cluster_healthy) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();

    auto peers = [&](uint16_t self) {
        std::vector<Endpoint> out;
        for (auto port : {p1, p2, p3})
            if (port != self)
                out.push_back({"127.0.0.1", port});
        return out;
    };
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, peers(p1));
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, peers(p2));
    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3, peers(p3));
    for (auto* config : {&c1, &c2, &c3}) {
        config->replication = 2;
        config->min_write_replicas = 2;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
        config->maintenance.garbage_grace = 0ms;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    auto s3 = std::make_unique<Service>(c3, keys);
    s1.start();
    s2.start();
    s3->start();
    REQUIRE(wait_until(
        [&] {
            return s1.node().membership().all_known_reachable() &&
                   s2.node().membership().all_known_reachable() &&
                   s3->node().membership().all_known_reachable() &&
                   s1.metadata_manager().cluster_status().stable &&
                   s2.metadata_manager().cluster_status().stable &&
                   s3->metadata_manager().cluster_status().stable;
        },
        10s));

    s1.filesystem().create_file("/partition-retain.bin", 0644, getuid(), getgid());
    const auto bytes = pattern(128 * 1024 + 7);
    auto writer = s1.filesystem().open_write("/partition-retain.bin", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    const auto entry = s1.filesystem().getattr("/partition-retain.bin");
    REQUIRE(entry.extents.size() == 1);
    const auto extent = entry.extents.front().id;
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/partition-retain.bin").size == bytes.size() &&
                   s3->filesystem().getattr("/partition-retain.bin").size == bytes.size();
        } catch (...) {
            return false;
        }
    }));

    // Record the claims and copies on the cohort that stays online: none may be
    // removed while a durably known node is unreachable.
    const bool n1_claim_before = s1.node().claims().retained(RetentionClass::data, extent);
    const bool n2_claim_before = s2.node().claims().retained(RetentionClass::data, extent);
    const bool n1_copy_before = s1.node().local_store().valid(extent);
    const bool n2_copy_before = s2.node().local_store().valid(extent);
    REQUIRE(n1_claim_before || n2_claim_before);
    REQUIRE(n1_copy_before || n2_copy_before);

    // The W=2 cohort keeps accepting metadata with node 3 offline, but the
    // persisted roster fences claim release and reclamation.
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2 &&
               !s1.node().membership().all_known_reachable() &&
               !s2.node().membership().all_known_reachable();
    }));

    s1.filesystem().unlink("/partition-retain.bin");
    REQUIRE(wait_until([&] {
        try {
            (void)s2.filesystem().getattr("/partition-retain.bin");
            return false;
        } catch (...) {
            return true;
        }
    }));

    // Several zero-grace passes: neither the claim nor the bytes may go.
    std::this_thread::sleep_for(800ms);
    if (n1_claim_before)
        CHECK(s1.node().claims().retained(RetentionClass::data, extent));
    if (n2_claim_before)
        CHECK(s2.node().claims().retained(RetentionClass::data, extent));
    if (n1_copy_before)
        CHECK(s1.node().local_store().valid(extent));
    if (n2_copy_before)
        CHECK(s2.node().local_store().valid(extent));

    // The third node returns from its persistent state; metadata must converge
    // before GC may reclaim the delete.
    s3 = std::make_unique<Service>(c3, keys);
    s3->start();
    REQUIRE(wait_until(
        [&] {
            return s1.node().membership().all_known_reachable() &&
                   s2.node().membership().all_known_reachable() &&
                   s3->node().membership().all_known_reachable() &&
                   s1.metadata_manager().cluster_status().stable &&
                   s2.metadata_manager().cluster_status().stable &&
                   s3->metadata_manager().cluster_status().stable;
        },
        10s));
    REQUIRE(wait_until(
        [&] {
            try {
                (void)s3->filesystem().getattr("/partition-retain.bin");
                return false;
            } catch (...) {
                return true;
            }
        },
        10s));
    REQUIRE(wait_until(
        [&] {
            return !s1.node().claims().retained(RetentionClass::data, extent) &&
                   !s2.node().claims().retained(RetentionClass::data, extent) &&
                   !s1.node().local_store().valid(extent) && !s2.node().local_store().valid(extent);
        },
        10s));

    s3->stop();
    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_repair_progresses_while_the_loader_never_goes_quiet) {
    // Repair is paced by weight against busier classes, never stopped: with
    // the loader active throughout, the missing copy still comes back.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 1;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 500ms;
        config->maintenance.no_progress_backoff = 500ms;
        config->maintenance.busy_bandwidth_fraction = 0.0;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));
    REQUIRE(s1.node().wait_local_state_ready(std::chrono::seconds{10}));
    REQUIRE(s2.node().wait_local_state_ready(std::chrono::seconds{10}));

    std::atomic_bool loading{true};
    std::thread loader([&] {
        while (loading.load()) {
            s2.resources().activity.note(FrameType::loader, 64 * 1024);
            std::this_thread::sleep_for(5ms);
        }
    });

    const auto bytes = pattern(96 * 1024 + 29);
    const auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    const RetentionDot claim{s1.node().node_id(), 0xbeef};
    s1.node().claims().retain(RetentionClass::data, id, claim);
    s2.node().claims().retain(RetentionClass::data, id, claim);
    REQUIRE(!s2.node().local_store().valid(id));
    s2.resources().events.notify(NodeEvent::storage);

    // The loader thread may not have run yet: wait for its first note.
    REQUIRE(wait_until([&] {
        return s2.resources().activity.idle_for(FrameType::loader) < c2.maintenance.foreground_quiet;
    }, 5s));
    const bool restored = wait_until([&] { return s2.node().local_store().valid(id); }, 10s);
    // The loader never paused: this copy came back during a busy period.
    CHECK(s2.resources().activity.idle_for(FrameType::loader) < c2.maintenance.foreground_quiet);
    loading = false;
    loader.join();
    REQUIRE(restored);
    CHECK(*s2.node().local_store().get(id) == bytes);
}

MACHA_TEST("rpc_cluster", test_retained_missing_copy_repairs_without_namespace_reachability) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();

    auto peers = [&](uint16_t self) {
        std::vector<Endpoint> out;
        for (auto port : {p1, p2, p3})
            if (port != self)
                out.push_back({"127.0.0.1", port});
        return out;
    };
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, peers(p1));
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, peers(p2));
    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3, peers(p3));
    for (auto* config : {&c1, &c2, &c3}) {
        config->replication = 1;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    auto s3 = std::make_unique<Service>(c3, keys);
    s1.start();
    s2.start();
    s3->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 3 &&
               s2.node().membership().active().size() == 3 &&
               s3->node().membership().active().size() == 3;
    }));
    // An accepted branch first, so maintenance runs against valid metadata.
    s1.filesystem().mkdir("/base", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/base").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));

    const auto bytes = pattern(96 * 1024 + 13);
    const auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    REQUIRE(s2.node().local_store().put(id, bytes));
    const RetentionDot claim{s1.node().node_id(), 0xf00d};
    s1.node().claims().retain(RetentionClass::data, id, claim);
    s2.node().claims().retain(RetentionClass::data, id, claim);

    // With a replica offline, install a claim dot the branch clock does not
    // dominate: unreachable from the namespace, it must still not be erased.
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));

    REQUIRE(s2.node().local_store().remove(id));
    CHECK(s2.node().claims().retained(RetentionClass::data, id));
    CHECK(!s2.node().local_store().valid(id));
    // A detector outside the storage wrappers must publish the mutation event;
    // heartbeats do not trigger maintenance.
    s2.resources().events.notify(NodeEvent::storage);

    // `id` is unreachable from namespace and catalogue; only its retention
    // claim tells maintenance to restore it.
    REQUIRE(wait_until([&] { return s2.node().local_store().valid(id); }, 5s));
    auto restored = s2.node().local_store().get(id);
    REQUIRE(restored.has_value());
    CHECK(*restored == bytes);
    CHECK(s2.node().claims().retained(RetentionClass::data, id));

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_held_retention_claims_cost_repair_no_credit) {
    // Claims already held cost no credit: with credit at one extent a second
    // and 64 claimed objects held, the one it lacks comes back promptly.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 1;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
        config->maintenance.idle_bandwidth_fraction = 1.0;
        config->maintenance.cpu_target = 1.0;
        config->maintenance.max_bandwidth = config->extent_size;
    }

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));
    REQUIRE(s1.node().wait_local_state_ready(std::chrono::seconds{10}));
    REQUIRE(s2.node().wait_local_state_ready(std::chrono::seconds{10}));

    const RetentionDot claim{s2.node().node_id(), 0xc0de};
    for (int i = 0; i < 64; ++i) {
        const auto held_bytes = pattern(16 * 1024, 500 + i);
        const auto held = object_id(held_bytes);
        REQUIRE(s2.node().local_store().put(held, held_bytes));
        s2.node().claims().retain(RetentionClass::data, held, claim);
    }
    const auto bytes = pattern(96 * 1024 + 7);
    const auto id = object_id(bytes);
    REQUIRE(s1.node().local_store().put(id, bytes));
    s2.node().claims().retain(RetentionClass::data, id, claim);
    REQUIRE(!s2.node().local_store().valid(id));
    s2.resources().events.notify(NodeEvent::storage);

    REQUIRE(wait_until([&] { return s2.node().local_store().valid(id); }, 5s));
    CHECK(*s2.node().local_store().get(id) == bytes);
}

MACHA_HEAVY_TEST("rpc_cluster", test_disjoint_metadata_pairs_branch_and_reconcile) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();
    const auto p4 = free_port();

    auto all_except = [&](uint16_t self) {
        std::vector<Endpoint> peers;
        for (auto port : {p1, p2, p3, p4}) {
            if (port != self)
                peers.push_back({"127.0.0.1", port});
        }
        return peers;
    };

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, all_except(p1));
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, all_except(p2));
    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3, all_except(p3));
    auto c4 = config_for(cluster.path() / "n4", cluster.keyfile(), p4, all_except(p4));
    for (auto* config : {&c1, &c2, &c3, &c4}) {
        config->replication = 1;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
    }

    Hash256 left_head{};
    NodeId node2_id{};
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        Service s3(c3, keys);
        Service s4(c4, keys);
        s1.start();
        s2.start();
        s3.start();
        s4.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 4 &&
                   s2.node().membership().active().size() >= 4 &&
                   s3.node().membership().active().size() >= 4 &&
                   s4.node().membership().active().size() >= 4;
        }));

        // One accepted base everywhere before partitioning into two writable pairs.
        s1.filesystem().mkdir("/base", 0755, getuid(), getgid());
        MetadataManager initial_repair(s1.node());
        initial_repair.repair_once();
        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/base").type == EntryType::directory &&
                       s3.filesystem().getattr("/base").type == EntryType::directory &&
                       s4.filesystem().getattr("/base").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        }));

        s3.stop();
        s4.stop();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() == 2 &&
                   s2.node().membership().active().size() == 2;
        }));

        s1.filesystem().mkdir("/left", 0755, getuid(), getgid());
        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/left").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        }));
        left_head = s2.node().metadata_replica().committed().hash;
        node2_id = s2.node().node_id();
        CHECK(s2.node().metadata_replica().acceptance(left_head).has_value());

        s2.stop();
        s1.stop();
    }

    Hash256 right_head{};
    {
        // Nodes 3 and 4 never saw /left but satisfy the floor, so stay writable.
        Service s3(c3, keys);
        Service s4(c4, keys);
        s3.start();
        s4.start();
        REQUIRE(wait_until([&] {
            return s3.node().membership().active().size() == 2 &&
                   s4.node().membership().active().size() == 2;
        }));

        bool left_absent = false;
        try {
            (void)s3.filesystem().getattr("/left");
        } catch (...) {
            left_absent = true;
        }
        CHECK(left_absent);
        s3.filesystem().mkdir("/right", 0755, getuid(), getgid());
        REQUIRE(wait_until([&] {
            try {
                return s4.filesystem().getattr("/right").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        }));
        right_head = s3.node().metadata_replica().committed().hash;
        REQUIRE(right_head != left_head);
        CHECK(s3.node().metadata_replica().acceptance(right_head).has_value());

        // With one member of the other pair back, two accepted sibling histories
        // meet; reconciliation must descend from both.
        s4.stop();
        Service s2(c2, keys);
        s2.start();
        REQUIRE(s2.node().node_id() == node2_id);
        REQUIRE(wait_until([&] {
            auto active2 = s2.node().membership().active();
            auto active3 = s3.node().membership().active();
            return active2.size() == 2 && active3.size() == 2;
        }));

        MetadataManager reconcile(s2.node());
        REQUIRE(wait_until(
            [&] {
                try {
                    reconcile.repair_once();
                    return s2.filesystem().getattr("/left").type == EntryType::directory &&
                           s2.filesystem().getattr("/right").type == EntryType::directory &&
                           s3.filesystem().getattr("/left").type == EntryType::directory &&
                           s3.filesystem().getattr("/right").type == EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            3s));

        REQUIRE(wait_until([&] {
            return s2.node().metadata_replica().accepted_heads().size() == 1 &&
                   s3.node().metadata_replica().accepted_heads().size() == 1;
        }));
        const auto merged = s2.node().metadata_replica().accepted_heads().front();
        CHECK(s2.node().metadata_replica().history_is_ancestor(left_head, merged.hash));
        CHECK(s2.node().metadata_replica().history_is_ancestor(right_head, merged.hash));
        const auto local_history = s2.node().metadata_replica().history_entry(merged.hash);
        const auto remote_history = s3.node().metadata_replica().history_entry(merged.hash);
        REQUIRE(local_history.has_value());
        REQUIRE(remote_history.has_value());
        CHECK(local_history->body == MetadataHistoryEntry::Body::delta);
        CHECK(remote_history->body == MetadataHistoryEntry::Body::delta);
        CHECK(local_history->payload.size() < merged.payload.size());
        CHECK(remote_history->payload == local_history->payload);

        s2.stop();
        s3.stop();
    }
}

MACHA_TEST("rpc_cluster", test_replication_policy_change_on_restart) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;

    ObjectId object;
    Bytes input = pattern(128 * 1024);
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        REQUIRE(wait_metadata_writable(s1));
        s1.filesystem().create_file("/policy.bin", 0644, getuid(), getgid());
        auto writer = s1.filesystem().open_write("/policy.bin", true);
        REQUIRE(writer->write(0, input) == input.size());
        writer->commit();
        auto entry = s1.filesystem().getattr("/policy.bin");
        REQUIRE(entry.extents.size() == 1);
        object = entry.extents.front().id;
        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/policy.bin").size == input.size();
            } catch (...) {
                return false;
            }
        }));
        s2.stop();
        s1.stop();
    }

    // The replica policy changes while the cluster is stopped; on restart the
    // new policy is committed and repair converges content to it.
    c1.replication = c2.replication = 2;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 2;
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        // The write floor must be re-established at the new policy first.
        REQUIRE(wait_metadata_writable(s1));
        s1.filesystem().mkdir("/after-grow", 0755, getuid(), getgid());
        MetadataManager m1(s1.node());
        auto snapshot = m1.snapshot();
        CHECK(snapshot.metadata_voters.empty());
        CHECK(snapshot.data_replication == 2);
        CHECK(snapshot.metadata_write_replicas_required == 2);
        CHECK(s2.filesystem().getattr("/after-grow").type == EntryType::directory);

        DistributedStore r1(s1.node(), s1.resources().activity, s1.resources().data, s1.resources().memory, s1.resources().events);
        DistributedStore r2(s2.node(), s2.resources().activity, s2.resources().data, s2.resources().memory, s2.resources().events);
        REQUIRE(wait_until([&] {
            r1.repair_once(1024 * 1024);
            r2.repair_once(1024 * 1024);
            return s1.node().local_store().has(object) && s2.node().local_store().has(object);
        }));

        s2.stop();
        s1.stop();
    }

    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        s2.filesystem().mkdir("/after-shrink", 0755, getuid(), getgid());
        MetadataManager m2(s2.node());
        auto snapshot = m2.snapshot();
        CHECK(snapshot.metadata_voters.empty());
        CHECK(snapshot.data_replication == 1);
        CHECK(snapshot.metadata_write_replicas_required == 1);
        CHECK(s1.filesystem().getattr("/after-shrink").type == EntryType::directory);

        auto reader = s2.filesystem().open_read("/policy.bin");
        Bytes output(input.size());
        REQUIRE(reader->read(0, output) == output.size());
        CHECK(output == input);

        s2.stop();
        s1.stop();
    }
}

MACHA_TEST("rpc_cluster", test_full_replica_fallback) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();
    uint16_t p3 = free_port();
    uint16_t p4 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3, {{"127.0.0.1", p1}});
    auto c4 = config_for(cluster.path() / "n4", cluster.keyfile(), p4, {{"127.0.0.1", p1}});
    // Equal capacities, equal shares; filling node 1 does not change its
    // weight, and the missing replica spills to the fallback.
    c1.storage_backends.front().limit = 2ULL * 1024 * 1024;
    c2.storage_backends.front().limit = 2ULL * 1024 * 1024;
    c3.storage_backends.front().limit = 2ULL * 1024 * 1024;
    c4.storage_backends.front().limit = 2ULL * 1024 * 1024;

    Service s1(c1, keys);
    Service s2(c2, keys);
    Service s3(c3, keys);
    Service s4(c4, keys);
    s1.start();
    s2.start();
    s3.start();
    s4.start();
    REQUIRE(wait_until([&] { return s2.node().membership().active().size() >= 4; }));

    auto filler = pattern(1800 * 1024);
    filler[0] ^= 0xa5;
    REQUIRE(s1.node().local_store().put(object_id(filler), filler));

    Bytes data;
    std::vector<NodeInfo> ranked;
    for (uint32_t salt = 0; salt < 1000; ++salt) {
        data = pattern(256 * 1024 + salt);
        auto id = object_id(data);
        auto active = s2.node().membership().active();
        ranked = capacity_placement_nodes(id.bytes, active, c2.replication);
        if (ranked.size() == 4 &&
            std::find_if(ranked.begin(), ranked.begin() + 3, [&](const NodeInfo& n) {
                return n.id == s1.node().node_id();
            }) != ranked.begin() + 3) {
            break;
        }
        ranked.clear();
    }
    REQUIRE(ranked.size() == 4);

    auto id = object_id(data);
    DistributedStore store(s2.node(), s2.resources().activity, s2.resources().data, s2.resources().memory, s2.resources().events);
    REQUIRE(store.put(id, data));
    CHECK(!s1.node().local_store().has(id));

    std::array<Service*, 4> services{&s1, &s2, &s3, &s4};
    auto fallback = std::find_if(services.begin(), services.end(), [&](Service* service) {
        return service->node().node_id() == ranked[3].id;
    });
    REQUIRE(fallback != services.end());

    // put() commits at quorum; repair then spills the missing replica to the fallback.
    REQUIRE(wait_until([&] {
        for (auto* service : services) {
            if (!service->node().local_store().has(id))
                continue;
            DistributedStore repair(service->node(), service->resources().activity,
                           service->resources().data, service->resources().memory, service->resources().events);
            repair.repair_once(8ULL * 1024 * 1024);
        }
        return (*fallback)->node().local_store().has(id);
    }));

    size_t copies = 0;
    for (auto* service : services)
        copies += service->node().local_store().has(id) ? 1 : 0;
    CHECK(copies == 3);

    s4.stop();
    s3.stop();
    s2.stop();
    s1.stop();
}

MACHA_HEAVY_TEST("rpc_cluster", test_replacement_node_recovers_namespace_and_replication) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;

    std::vector<ObjectId> objects;
    Bytes input = pattern(2 * 1024 * 1024 + 12345);
    NodeId old_n1;

    auto s1 = std::make_unique<Service>(c1, keys);
    auto s2 = std::make_unique<Service>(c2, keys);
    s1->start();
    s2->start();
    REQUIRE(wait_until([&] {
        return s1->node().membership().active().size() >= 2 &&
               s2->node().membership().active().size() >= 2;
    }));

    s1->filesystem().mkdir("/media", 0755, getuid(), getgid());
    s1->filesystem().create_file("/media/recovery.bin", 0644, getuid(), getgid());
    auto writer = s1->filesystem().open_write("/media/recovery.bin", true);
    REQUIRE(writer->write(0, input) == input.size());
    writer->commit();
    auto entry = s1->filesystem().getattr("/media/recovery.bin");
    REQUIRE(!entry.extents.empty());
    for (const auto& extent : entry.extents) {
        if (!extent.hole)
            objects.push_back(extent.id);
    }
    REQUIRE(!objects.empty());

    REQUIRE(wait_until([&] {
        try {
            return s2->filesystem().getattr("/media/recovery.bin").size == input.size();
        } catch (...) {
            return false;
        }
    }));
    // R=2 is convergence, W=1 the floor; drive repair directly rather than
    // waiting on the maintenance scheduler.
    DistributedStore initial_convergence(s2->node(), s2->resources().activity,
                           s2->resources().data, s2->resources().memory, s2->resources().events);
    REQUIRE(wait_until([&] {
        initial_convergence.repair_once(16ULL * 1024 * 1024, objects);
        return std::all_of(objects.begin(), objects.end(),
                           [&](const auto& id) { return s2->node().local_store().has(id); });
    }));

    // One metadata pass makes node 2 a durable checkpoint witness first.
    MetadataManager witness_repair(s2->node());
    witness_repair.repair_once();
    CHECK(s2->node().metadata_replica().committed().generation > 1);

    old_n1 = s1->node().node_id();
    s1->stop();
    s1.reset();

    // Node 1 loses everything but the cluster key and config.
    std::error_code ec;
    std::filesystem::remove_all(c1.state_path, ec);
    std::filesystem::remove_all(c1.storage_backends.front().path, ec);
    if (!c1.cache.path.empty())
        std::filesystem::remove_all(c1.cache.path, ec);
    std::filesystem::create_directories(c1.storage_backends.front().path);

    // A wiped node needs a bootstrap route to find the survivor.
    auto replacement_config = c1;
    replacement_config.bootstrap = {{"127.0.0.1", p2}};
    auto replacement = std::make_unique<Service>(replacement_config, keys);
    replacement->start();
    CHECK(replacement->node().node_id() != old_n1);

    // Until the destroyed node expires from membership, repair may rightly keep
    // it as an owner and pull nothing to the replacement.
    REQUIRE(wait_until(
        [&] {
            const auto active = replacement->node().membership().active();
            const bool old_present = std::any_of(
                active.begin(), active.end(), [&](const auto& node) { return node.id == old_n1; });
            const bool survivor_present =
                std::any_of(active.begin(), active.end(),
                            [&](const auto& node) { return node.id == s2->node().node_id(); });
            return !old_present && survivor_present;
        },
        5s));

    REQUIRE(wait_until(
        [&] {
            try {
                return replacement->filesystem().getattr("/media/recovery.bin").size ==
                       input.size();
            } catch (...) {
                return false;
            }
        },
        10s));

    // With the namespace recovered, repair repopulates the replacement from the survivor.
    DistributedStore replacement_convergence(replacement->node(), replacement->resources().activity,
                           replacement->resources().data, replacement->resources().memory, replacement->resources().events);
    REQUIRE(wait_until(
        [&] {
            replacement_convergence.repair_once(16ULL * 1024 * 1024, objects);
            return std::all_of(objects.begin(), objects.end(), [&](const auto& id) {
                return replacement->node().local_store().has(id);
            });
        },
        10s));

    Bytes output(input.size());
    auto reader = replacement->filesystem().open_read("/media/recovery.bin");
    size_t offset = 0;
    while (offset < output.size()) {
        auto n = reader->read(offset, {output.data() + offset, output.size() - offset});
        REQUIRE(n > 0);
        offset += n;
    }
    CHECK(output == input);

    // The recovered metadata replica is writable.
    replacement->filesystem().mkdir("/after-replacement", 0755, getuid(), getgid());
    REQUIRE(wait_until(
        [&] {
            try {
                return s2->filesystem().getattr("/after-replacement").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        200ms));

    replacement->stop();
    s2->stop();
}

// Three nodes whose commits are durable on every replica, so nothing below
// waits for replication. The metadata cache lasts 5 s, so a change seen through
// another node came from the replica's committed generation, not cache expiry.
struct DurableTrio {
    TestCluster cluster;
    std::vector<Config> configs;
    std::vector<std::unique_ptr<Service>> nodes;

    explicit DurableTrio(size_t metadata_floor = 3, size_t data_floor = 3) {
        const uint16_t first = free_port();
        for (size_t i = 0; i < 3; ++i) {
            const auto dir = cluster.path() / ("n" + std::to_string(i + 1));
            auto config = i == 0 ? config_for(dir, cluster.keyfile(), first)
                                 : config_for(dir, cluster.keyfile(), free_port(),
                                              {{"127.0.0.1", first}});
            config.storage_packing = StoragePackingConfig{0, 0};
            config.replication = 3;
            config.min_write_replicas = data_floor;
            config.metadata_min_write_replicas = metadata_floor;
            config.metadata_cache = 5000ms; // the longest allowed
            configs.push_back(std::move(config));
        }
        for (const auto& config : configs)
            nodes.push_back(std::make_unique<Service>(config, cluster.keys()));
        for (auto& node : nodes)
            node->start();
        REQUIRE(wait_until(
            [&] {
                for (auto& node : nodes)
                    if (node->node().membership().active().size() < 3)
                        return false;
                return true;
            },
            60s));
        for (auto& node : nodes)
            REQUIRE(wait_metadata_writable(*node, 60s));
    }
    ~DurableTrio() {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
            if (*it)
                (*it)->stop();
    }
    Service& operator[](size_t i) { return *nodes[i]; }
    void stop(size_t i) {
        nodes[i]->stop();
        nodes[i].reset();
    }
    // Writes `bytes` to `path` through node `i`; returns the file's extents.
    std::vector<ObjectId> write(size_t i, const std::string& path, const Bytes& bytes) {
        auto& fs = nodes[i]->filesystem();
        fs.create_file(path, 0644, getuid(), getgid());
        auto writer = fs.open_write(path, true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
        std::vector<ObjectId> ids;
        for (const auto& extent : fs.getattr(path).extents)
            ids.push_back(extent.id);
        return ids;
    }
};

bool is_directory(Service& node, const std::string& path) {
    try {
        return node.filesystem().getattr(path).type == EntryType::directory;
    } catch (const std::exception&) {
        return false;
    }
}

Bytes read_whole(Service& node, const std::string& path, size_t size) {
    Bytes out(size);
    auto reader = node.filesystem().open_read(path);
    size_t got = 0;
    while (got < size) {
        const auto n = reader->read(got, {out.data() + got, size - got});
        REQUIRE(n > 0);
        got += n;
    }
    return out;
}

// Any node may found a virgin namespace (here the highest NodeId), and its root
// is on every replica when the founding call returns.
MACHA_HEAVY_TEST("rpc_cluster", test_any_node_founds_the_namespace_on_every_replica) {
    DurableTrio trio;
    size_t founder = 0;
    for (size_t i = 1; i < 3; ++i)
        if (trio[i].node().node_id() > trio[founder].node().node_id())
            founder = i;
    trio[founder].filesystem().mkdir("/media", 0755, getuid(), getgid());
    for (size_t i = 0; i < 3; ++i)
        CHECK(is_directory(trio[i], "/media"));
}

// A node whose namespace view is warm sees another node's change as soon as
// the change commits: its replica's committed generation outranks the view.
MACHA_HEAVY_TEST("rpc_cluster", test_a_warm_namespace_view_sees_a_commit_from_another_node) {
    DurableTrio trio;
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    REQUIRE(is_directory(trio[1], "/media")); // warms node 2's view
    trio[2].filesystem().mkdir("/media/new", 0755, getuid(), getgid());
    CHECK(is_directory(trio[1], "/media/new"));
}

// A file written through one node reads back whole through a second and at
// an arbitrary offset through a third, as soon as the write commits.
MACHA_HEAVY_TEST("rpc_cluster", test_a_committed_file_reads_back_through_every_node) {
    DurableTrio trio;
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    const auto input = pattern(3 * 1024 * 1024 + 12345);
    (void)trio.write(0, "/media/movie.mkv", input);
    CHECK(read_whole(trio[1], "/media/movie.mkv", input.size()) == input);
    Bytes slice(333333);
    auto reader = trio[2].filesystem().open_read("/media/movie.mkv");
    REQUIRE(reader->read(987654, slice) == slice.size());
    CHECK(std::equal(slice.begin(), slice.end(), input.begin() + 987654));
}

// With the metadata floor at two of three: losing one replica leaves the
// other two committing, and the change is on both when the call returns; a
// lone survivor refuses to commit.
MACHA_HEAVY_TEST("rpc_cluster", test_metadata_commits_down_to_its_floor_and_no_further) {
    DurableTrio trio(2, 1);
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    trio.stop(2);
    trio[1].filesystem().mkdir("/after-one-loss", 0755, getuid(), getgid());
    CHECK(is_directory(trio[0], "/after-one-loss"));
    trio.stop(1);
    bool refused = false;
    try {
        trio[0].filesystem().mkdir("/below-the-floor", 0755, getuid(), getgid());
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(!is_directory(trio[0], "/below-the-floor"));
}

// A corrupt local copy (it fails authentication) is never served: the read
// comes from a peer, and the fetched bytes replace the bad copy.
MACHA_HEAVY_TEST("rpc_cluster", test_a_read_of_a_corrupt_local_copy_comes_from_a_peer_and_heals_it) {
    DurableTrio trio;
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    const auto ids = trio.write(0, "/media/file.bin", pattern(256 * 1024, 7));
    REQUIRE(ids.size() == 1);
    const auto id = ids.front();
    auto& node = trio[1].node();
    corrupt_object(trio.configs[1].storage_backends.front().path, id);
    REQUIRE(!node.local_store().get(id).has_value());

    DistributedStore store(node, trio[1].resources().activity, trio[1].resources().data, trio[1].resources().memory, trio[1].resources().events);
    const auto fetched = store.get(id);
    REQUIRE(fetched.has_value());
    CHECK(object_id(*fetched) == id);
    store.wait_local_copies_settled();
    const auto healed = node.local_store().get(id);
    REQUIRE(healed.has_value());
    CHECK(object_id(*healed) == id);
}

// Without a read, the scrub finds a corrupt copy and discards it, and repair
// restores it from a peer within a few bounded steps.
MACHA_HEAVY_TEST("rpc_cluster", test_scrub_discards_a_corrupt_copy_and_repair_restores_it) {
    DurableTrio trio;
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    const auto ids = trio.write(0, "/media/file.bin", pattern(256 * 1024, 8));
    REQUIRE(ids.size() == 1);
    const auto id = ids.front();
    auto& node = trio[1].node();
    corrupt_object(trio.configs[1].storage_backends.front().path, id);

    DistributedStore store(node, trio[1].resources().activity, trio[1].resources().data, trio[1].resources().memory, trio[1].resources().events);
    store.scrub_once(128ULL * 1024 * 1024);
    CHECK(!node.local_store().has(id));
    // Repair pulls what the node's live inventory says it should hold.
    const auto live = trio[1].filesystem().maintenance_objects().live;
    for (int step = 0; step < 4 && !node.local_store().has(id); ++step)
        store.repair_once(128ULL * 1024 * 1024, live);
    const auto restored = node.local_store().get(id);
    REQUIRE(restored.has_value());
    CHECK(object_id(*restored) == id);
}

// With its local copy gone, a node reads the file from a peer.
MACHA_HEAVY_TEST("rpc_cluster", test_a_read_falls_back_to_a_peer_when_the_local_copy_is_gone) {
    DurableTrio trio;
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    const auto input = pattern(512 * 1024, 9);
    const auto ids = trio.write(0, "/media/file.bin", input);
    for (const auto& id : ids)
        REQUIRE(trio[1].node().local_store().remove(id));
    CHECK(read_whole(trio[1], "/media/file.bin", input.size()) == input);
}

// A persistent cache enabled at runtime keeps a playback fetch: the fetched
// bytes land in the cache, and a later read with the local copy gone is
// answered from it.
MACHA_HEAVY_TEST("rpc_cluster", test_a_runtime_cache_keeps_a_playback_fetch) {
    DurableTrio trio;
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    const auto ids = trio.write(0, "/media/file.bin", pattern(256 * 1024, 11));
    REQUIRE(ids.size() == 1);
    const auto id = ids.front();
    auto& node = trio[1].node();
    auto cached = trio.configs[1];
    cached.cache.path = trio.cluster.path() / "n2-cache";
    cached.cache.max_blocks = 8;
    node.reconfigure_local(cached);
    trio[1].store().wait_local_copies_settled();
    REQUIRE(node.local_store().remove(id));
    REQUIRE(!node.block_cache().has(id));

    DistributedStore store(node, trio[1].resources().activity, trio[1].resources().data, trio[1].resources().memory, trio[1].resources().events);
    const auto fetched = store.get(id, 0, true);
    REQUIRE(fetched.has_value());
    store.wait_local_copies_settled();
    CHECK(node.block_cache().has(id));
    REQUIRE(!node.local_store().has(id));
    const auto again = store.get(id, 0, true);
    REQUIRE(again.has_value());
    CHECK(*again == *fetched);
}

// Unlinking retires the object to garbage. The cached inventory is one shared,
// sorted, duplicate-free snapshot until the namespace changes.
MACHA_TEST("rpc_cluster", test_unlink_retires_an_object_from_the_maintenance_inventory) {
    TestService fixture("inventory");
    fixture.config().replication = 1;
    fixture.config().metadata_min_write_replicas = 1;
    auto& service = fixture.start();
    auto& fs = service.filesystem();
    fs.mkdir("/media", 0755, getuid(), getgid());
    for (const char* name : {"/media/a.bin", "/media/b.bin"}) {
        fs.create_file(name, 0644, getuid(), getgid());
        auto writer = fs.open_write(name, true);
        const auto bytes = pattern(131072, static_cast<uint8_t>(name[7]));
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    }
    const auto doomed = fs.getattr("/media/a.bin").extents.front().id;
    fs.unlink("/media/a.bin");

    const auto objects = fs.maintenance_objects();
    CHECK(std::any_of(objects.garbage.begin(), objects.garbage.end(),
                      [&](const GarbageRef& garbage) { return garbage.id == doomed; }));
    CHECK(std::find(objects.live.begin(), objects.live.end(), doomed) == objects.live.end());

    const auto first = fs.maintenance_objects_cached();
    CHECK(fs.maintenance_objects_cached().get() == first.get());
    CHECK(first->metadata_generation != 0);
    CHECK(std::is_sorted(first->live.begin(), first->live.end()));
    CHECK(std::adjacent_find(first->live.begin(), first->live.end()) == first->live.end());
    CHECK(std::is_sorted(first->garbage.begin(), first->garbage.end()));
    fs.mkdir("/changed", 0755, getuid(), getgid());
    const auto after = fs.maintenance_objects_cached();
    CHECK(after.get() != first.get());
    CHECK(after->metadata_generation > first->metadata_generation);
}

MACHA_TEST("rpc_cluster", test_rename_of_a_file_onto_a_directory_is_eisdir) {
    TestService fixture("rename-eisdir");
    fixture.config().replication = 1;
    fixture.config().metadata_min_write_replicas = 1;
    auto& fs = fixture.start().filesystem();
    fs.mkdir("/media", 0755, getuid(), getgid());
    fs.create_file("/media/file.bin", 0644, getuid(), getgid());
    fs.mkdir("/media/dir", 0755, getuid(), getgid());
    int code = 0;
    try {
        fs.rename("/media/file.bin", "/media/dir", false);
    } catch (const FsError& error) {
        code = error.code();
    }
    CHECK(code == EISDIR);
    CHECK(fs.getattr("/media/file.bin").type == EntryType::file);
    CHECK(fs.getattr("/media/dir").type == EntryType::directory);
}

// Waits until the maintenance pass is parked with no wake-up for five
// consecutive looks: done with all it can at the current clock time.
void settle_maintenance(Service& service) {
    const auto deadline = Clock::now() + 20s;
    uint64_t seen = service.maintenance_wakeups();
    int quiet = 0;
    while (Clock::now() < deadline && quiet < 5) {
        std::this_thread::sleep_for(20ms);
        const auto wakeups = service.maintenance_wakeups();
        const bool parked = std::string_view(service.maintenance_stage()) == "wait";
        quiet = parked && wakeups == seen ? quiet + 1 : 0;
        seen = wakeups;
    }
    REQUIRE(quiet >= 5);
}

// A joining node pulls its objects through its own maintenance pass, run on a
// manual clock: the test steps the clock and waits only for the pass to park.
MACHA_HEAVY_TEST("rpc_cluster", test_a_joining_node_pulls_its_objects_through_maintenance) {
    TestCluster cluster;
    const uint16_t first = free_port();
    auto node_config = [&](size_t i) {
        const auto dir = cluster.path() / ("n" + std::to_string(i + 1));
        auto config = i == 0 ? config_for(dir, cluster.keyfile(), first)
                             : config_for(dir, cluster.keyfile(), free_port(),
                                          {{"127.0.0.1", first}});
        config.storage_packing = StoragePackingConfig{0, 0};
        config.replication = 3;
        config.min_write_replicas = 2;
        config.metadata_min_write_replicas = 2;
        return config;
    };
    Service n1(node_config(0), cluster.keys());
    Service n2(node_config(1), cluster.keys());
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] { return n1.node().membership().active().size() >= 2; }, 60s));
    REQUIRE(wait_metadata_writable(n1, 60s));
    n1.filesystem().mkdir("/media", 0755, getuid(), getgid());
    n1.filesystem().create_file("/media/file.bin", 0644, getuid(), getgid());
    {
        const auto bytes = pattern(1024 * 1024, 5);
        auto writer = n1.filesystem().open_write("/media/file.bin", true);
        REQUIRE(writer->write(0, bytes) == bytes.size());
        writer->commit();
    }
    std::vector<ObjectId> ids;
    for (const auto& extent : n1.filesystem().getattr("/media/file.bin").extents)
        ids.push_back(extent.id);
    REQUIRE(!ids.empty());

    auto clock = std::make_shared<ManualMaintenanceClock>();
    ServiceInstruments instruments;
    instruments.clock = clock;
    Service n3(node_config(2), cluster.keys(), NodeRuntime::StartupStageHook{},
               Service::MaintenanceStageHook{}, Service::StartupStallHandler{}, instruments);
    n3.start();
    REQUIRE(wait_until([&] { return n3.node().membership().active().size() >= 3; }, 60s));
    (void)n3.filesystem(); // services ready

    const auto holds_all = [&] {
        return std::all_of(ids.begin(), ids.end(),
                           [&](const ObjectId& id) { return n3.node().local_store().has(id); });
    };
    // Each step settles the pass, then moves its clock a second; bounded in steps.
    int steps = 0;
    for (; steps < 60 && !holds_all(); ++steps) {
        settle_maintenance(n3);
        clock->advance(1s);
    }
    settle_maintenance(n3);
    CHECK(holds_all());
    n3.stop();
    n2.stop();
    n1.stop();
}

// Needs the libmacha-torrent plugin, built only when libtorrent is found.
#ifdef MACHA_TEST_PLUGIN_DIR
MACHA_TEST("rpc_cluster", test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes) {
    // repair_once() runs its per-peer fan-out without mutation_mutex_: with
    // the has_metadata_history_entry probe to a silent peer held, a foreground
    // write does not wait.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "silent-repair-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "silent-repair-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Past the virgin generation, so repair takes the read_group()/fan-out path.
    s1.filesystem().mkdir("/warm", 0755, getuid(), getgid());

    const auto peer = s2.node().node_id();
    s1.node().stall_peer_for_tests(peer, MessageType::has_metadata_history_entry);
    std::atomic_bool repair_done{false};
    std::jthread repair([&] {
        try {
            s1.metadata_manager().repair_once();
        } catch (const std::exception&) {
            // A pass abandoned on the stalled peer is fine; it retries.
        }
        repair_done = true;
    });
    // Repair is now inside the fan-out, holding the probe to the silent peer.
    REQUIRE(wait_until([&] { return s1.node().stalled_calls_for_tests() >= 1; }, 15s));
    CHECK(!repair_done.load());

    const auto started = std::chrono::steady_clock::now();
    s1.filesystem().mkdir("/during-stall", 0755, getuid(), getgid());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Well inside the 30 s control no-progress deadline.
    CHECK(elapsed < 3s);
    CHECK(!repair_done.load());

    s1.node().release_peer_for_tests(peer);
    REQUIRE(wait_until([&] { return repair_done.load(); }, 60s));
    repair.join();

    bool converged = false;
    try {
        s1.metadata_manager().repair_once();
        converged = true;
    } catch (const std::exception&) {
    }
    CHECK(converged);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_torrent_listing_is_served_from_memory_while_a_peer_is_silent) {
    // GET /api/v1/torrents/jobs never waits on a peer, even when a peer has
    // announced a newer generation.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "listing-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "listing-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // A write before the metadata floor forms is refused; wait for it on both nodes.
    REQUIRE(wait_metadata_writable(s1));
    REQUIRE(wait_metadata_writable(s2));
    s1.filesystem().mkdir("/warm", 0755, getuid(), getgid());
    REQUIRE(s1.metadata_manager().available_snapshot_view().has_value());

    // A commit on node 2 makes node 1's cached view stale.
    const auto before = s1.node().remote_metadata_generation();
    s2.filesystem().mkdir("/elsewhere", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] { return s1.node().remote_metadata_generation() > before; }, 10s));

    // Node 2 now answers nothing, so a listing that surveyed it would block.
    // Stalled-call counts are no evidence: background work calls node 2 constantly.
    const auto peer = s2.node().node_id();
    s1.node().stall_peer_for_tests(peer);
    const auto started = std::chrono::steady_clock::now();
    (void)s1.torrent_coordinator().requests();
    (void)s1.torrent_coordinator().request("does-not-exist");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < 1s);
    s1.node().release_peer_for_tests(peer);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_extent_put_to_a_silent_peer_fails_within_the_no_progress_budget) {
    // A put whose stalled replica has no replacement fails retryably within
    // the pipeline's no-progress budget rather than spinning forever.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "silent-put-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "silent-put-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    // Both replicas are required, so the silent one cannot be replaced.
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 2;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.write_stall = c2.write_stall = 200ms;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Seeing the peer is not a formed metadata replica set; create_file needs
    // a committed generation past genesis (1).
    REQUIRE(wait_until([&] {
        return s1.node().metadata_replica().committed_generation() > 1;
    }, 10s));
    s1.filesystem().create_file("/silent.bin", 0644, getuid(), getgid());
    const auto contents = pattern(64 * 1024);
    const auto peer = s2.node().node_id();

    std::atomic_uint64_t progress{0};
    auto open = [&] {
        return s1.filesystem().open_write(
            "/silent.bin", false, false, WriteDurability::publication_generation,
            16ULL * 1024 * 1024,
            DataWorkContext(FrameType::loader, c1.extent_size, {}, nullptr, &progress, 500ms));
    };

    s1.node().stall_peer_for_tests(peer, MessageType::put_object_deferred);
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    {
        auto writer = open();
        try {
            (void)writer->write(0, contents);
            writer->commit();
        } catch (const std::exception& e) {
            error = e.what();
        }
        // The destructor relaunches a failed put; let the peer answer it.
        s1.node().release_peer_for_tests(peer);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(!error.empty());
    CHECK(error.find("quorum unavailable") != std::string::npos);
    CHECK(elapsed < 15s);

    // With the peer answering again the identical write goes through.
    bool ok = false;
    try {
        auto writer = open();
        (void)writer->write(0, contents);
        writer->commit();
        ok = true;
    } catch (const std::exception&) {
    }
    CHECK(ok);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    const auto source_dir = cluster.path() / "jobvis-n1-src";
    std::filesystem::create_directories(source_dir);
    const auto source_file = source_dir / "movie.mkv";
    {
        std::ofstream out(source_file, std::ios::binary);
        REQUIRE(out.good());
        out << "not really media, just needs to exist";
    }

    auto c1 = config_for(cluster.path() / "jobvis-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "jobvis-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    // ingest.enabled requires the scanner; provider lookups are off (no jobs
    // reach cataloguing, and they would need a TMDB token).
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = true;
    c1.catalogue.scanner.movies.enabled = c2.catalogue.scanner.movies.enabled = false;
    c1.catalogue.scanner.tv.enabled = c2.catalogue.scanner.tv.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;
    c1.ingest.enabled = c2.ingest.enabled = true;
    c1.ingest.source_roots = {source_dir};
    // Node 2 runs no torrents: it still lists node 1's, acts on them, places
    // a new one there, and refuses one pinned to itself.
    c1.torrent.enabled = true;
    c2.torrent.enabled = false;
    // Load this build's torrent plugin through the real dlopen path, from a
    // directory holding only it, so neither Service loads any other plugin.
    TempDir plugin_dir;
    {
        const std::filesystem::path torrent_plugin = MACHA_TEST_TORRENT_PLUGIN;
        std::error_code plugin_copy_error;
        std::filesystem::copy_file(torrent_plugin, plugin_dir.path() / torrent_plugin.filename(),
                                   std::filesystem::copy_options::overwrite_existing,
                                   plugin_copy_error);
        REQUIRE(!plugin_copy_error);
    }
    c1.plugin_path = c2.plugin_path = plugin_dir.path();

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
              s2.node().membership().active().size() >= 2;
    }));

    const auto node1_id = to_string(s1.node().node_id());

    // Only node 1 owns these jobs; node 2 sees them through the cluster RPC
    // survey. The dummy file is not media, so wait for the job to fail rather
    // than race a pause() that plan_job() would overwrite.
    const auto ingest_id = s1.ingest().submit_path(source_file, "filesystem");
    REQUIRE(wait_until(
        [&] {
            auto job = s1.ingest().job(ingest_id);
            return job && job->state == IngestJobState::failed;
        },
        5s));
    // The failure carries a code, emitted beside the message.
    {
        const auto failed = s1.ingest().job(ingest_id);
        REQUIRE(failed.has_value());
        CHECK(failed->error_code == "no_supported_media");
        CHECK(!failed->error.empty());
        const auto json = ingest_job_json(*failed, false);
        CHECK(json.find("error_code")->asString() == "no_supported_media");
    }

    // A non-null handle proves the plugin load path. Plugins are constructed
    // on their own lifecycle threads, so it appears shortly after ready.
    std::shared_ptr<TorrentService> s1_torrents;
    REQUIRE(wait_until([&] { return (s1_torrents = s1.torrents()) != nullptr; }, 5s));
    const auto torrent_id = s1_torrents->add(
        "magnet:?xt=urn:btih:3333333333333333333333333333333333333333&dn=Test");
    // add() only creates the session entry, so pausing at once is reliable.
    REQUIRE(s1_torrents->pause(torrent_id));

    auto get = [](AcquisitionApi& api, const std::string& path) {
        HttpRequest request;
        request.method = "GET";
        request.path = path;
        return api.handle(request);
    };
    auto post = [](AcquisitionApi& api, const std::string& path) {
        HttpRequest request;
        request.method = "POST";
        request.path = path;
        return api.handle(request);
    };
    auto body_json = [](const HttpResponse& response) {
        return Json::parse(
            std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
    };

    // Node 2's cluster view refreshes every 5 s; poll now instead of waiting.
    s2.cluster_jobs().refresh_now();

    // --- Ingest: list visibility from the non-owning node ---
    {
        const auto response = get(s2.acquisition_api(), "/api/v1/ingest/jobs");
        REQUIRE(response.status == 200);
        const auto parsed = body_json(response);
        const auto* jobs = parsed.find("jobs");
        REQUIRE(jobs != nullptr);
        bool found = false;
        for (const auto& job : jobs->asArray()) {
            const auto* id = job.find("id");
            if (!id || id->asString() != ingest_id) continue;
            found = true;
            const auto* node_id = job.find("node_id");
            REQUIRE(node_id != nullptr);
            CHECK(node_id->asString() == node1_id);
            const auto* state = job.find("state");
            REQUIRE(state != nullptr);
            CHECK(state->asString() == "failed");
        }
        CHECK(found);
    }

    // --- Ingest: single-job detail visibility from the non-owning node ---
    {
        const auto response = get(s2.acquisition_api(), "/api/v1/ingest/jobs/" + ingest_id);
        REQUIRE(response.status == 200);
        const auto parsed = body_json(response);
        const auto* node_id = parsed.find("node_id");
        REQUIRE(node_id != nullptr);
        CHECK(node_id->asString() == node1_id);
        const auto* catalogue = parsed.find("catalogue");
        REQUIRE(catalogue != nullptr);
        CHECK(catalogue->isObject());
    }

    // --- Ingest: resume from the non-owning node actually lands on node 1 ---
    {
        const auto response = post(s2.acquisition_api(), "/api/v1/ingest/jobs/" + ingest_id + "/resume");
        REQUIRE(response.status == 200);
        const auto parsed = body_json(response);
        const auto* node_id = parsed.find("node_id");
        REQUIRE(node_id != nullptr);
        CHECK(node_id->asString() == node1_id);
        const auto* state = parsed.find("state");
        REQUIRE(state != nullptr);
        // Node 1's own answer via the RPC survey: the 200 proves the action
        // landed (the route errors unless it reported `changed`). A worker may
        // already have re-failed the job, so only states resume can lead to pass.
        const auto reported = state->asString();
        CHECK((reported == "queued" || reported == "scanning" || reported == "importing" ||
               reported == "failed"));
    }

    // --- Torrent: node 1's existing job becomes a cluster request its
    // coordinator claims, visible from node 2 ---
    s1.torrent_coordinator().pass_now();
    REQUIRE(wait_until([&] { return s2.torrent_coordinator().request(torrent_id).has_value(); }, 10s));
    s2.cluster_jobs().refresh_now();
    {
        const auto response = get(s2.acquisition_api(), "/api/v1/torrents/jobs");
        REQUIRE(response.status == 200);
        const auto parsed = body_json(response);
        bool found = false;
        for (const auto& job : parsed.find("jobs")->asArray()) {
            if (job.find("id")->asString() != torrent_id) continue;
            found = true;
            CHECK(job.find("node_id")->asString() == node1_id);
            CHECK(job.find("phase")->asString() == "downloading");
            CHECK(job.find("desired")->asString() == "paused");
            CHECK(job.find("state")->asString() == "paused"); // node 1's live state, via the view
            CHECK(job.find("desired_applied")->asBool());
            CHECK(!job.find("live_as_of_unix_ms")->isNull());
        }
        CHECK(found);
    }

    // --- Torrent: resume through node 2 is intent; node 1 applies it ---
    {
        const auto response =
            post(s2.acquisition_api(), "/api/v1/torrents/jobs/" + torrent_id + "/resume");
        REQUIRE(response.status == 202);
        const auto parsed = body_json(response);
        CHECK(parsed.find("desired")->asString() == "active");
        CHECK(parsed.find("node_id")->asString() == node1_id);
        REQUIRE(wait_until([&] {
            s1.torrent_coordinator().pass_now();
            const auto job = s1_torrents->job(torrent_id);
            return job && job->state != TorrentJobState::paused;
        }, 10s));
    }

    // --- Torrent: the capable nodes, as node 2 sees them: node 1 only ---
    {
        const auto response = get(s2.acquisition_api(), "/api/v1/torrents/nodes");
        REQUIRE(response.status == 200);
        const auto nodes = body_json(response).find("nodes")->asArray();
        REQUIRE(nodes.size() == 1);
        CHECK(nodes.front().find("node_id")->asString() == node1_id);
        CHECK(!nodes.front().find("local")->asBool());
        CHECK(nodes.front().find("reachable")->asBool());
        CHECK(nodes.front().find("accepting")->asBool());
        CHECK(nodes.front().find("max_active")->asUInt64() == c1.torrent.max_active);
        CHECK(nodes.front().find("staging")->find("limit_bytes")->asUInt64() > 0);
    }

    // --- Torrent: added through node 2, which runs none; node 1 claims it ---
    {
        auto add = [&](const std::string& body) {
            HttpRequest request;
            request.method = "POST";
            request.path = "/api/v1/torrents/jobs";
            request.body = Bytes(body.begin(), body.end());
            return s2.acquisition_api().handle(request);
        };
        // Pinned to node 2 itself: it cannot run torrents.
        const auto here = add(R"({"magnet":"magnet:?xt=urn:btih:4444444444444444444444444444444444444444","node_id":")" +
                              to_string(s2.node().node_id()) + R"("})");
        CHECK(here.status == 409);
        CHECK(body_json(here).find("error")->find("reason")->asString() == "node_not_torrent_capable");

        const auto queued = add(R"({"magnet":"magnet:?xt=urn:btih:4444444444444444444444444444444444444444&dn=Queued"})");
        REQUIRE(queued.status == 202);
        const auto body = body_json(queued);
        CHECK(body.find("node_id")->isNull()); // the cluster chooses
        const auto queued_id = body.find("id")->asString();
        REQUIRE(body.find("job")->isObject());
        CHECK(body.find("job")->find("phase")->asString() == "awaiting_node");
        // Listed on node 2 at once: the 202 came after metadata acceptance.
        bool listed = false;
        const auto listing = body_json(get(s2.acquisition_api(), "/api/v1/torrents/jobs"));
        for (const auto& job : listing.find("jobs")->asArray())
            if (job.find("id")->asString() == queued_id) listed = true;
        CHECK(listed);
        // Node 1, the only capable node, claims and starts it.
        REQUIRE(wait_until([&] {
            s1.torrent_coordinator().pass_now();
            return s1_torrents->job(queued_id).has_value();
        }, 15s));
        REQUIRE(wait_until([&] {
            const auto r = s2.torrent_coordinator().request(queued_id);
            return r && r->claim && to_string(r->claim->node_id) == node1_id;
        }, 10s));

        // The same torrent again, through either node, is the one job.
        const auto again = add(R"({"magnet":"magnet:?xt=urn:btih:4444444444444444444444444444444444444444"})");
        CHECK(again.status == 409);
        CHECK(body_json(again).find("id")->asString() == queued_id);

        // Cancelled through node 2; node 1 stops it and says so.
        REQUIRE(post(s2.acquisition_api(), "/api/v1/torrents/jobs/" + queued_id + "/cancel").status == 202);
        REQUIRE(wait_until([&] {
            s1.torrent_coordinator().pass_now();
            const auto r = s2.torrent_coordinator().request(queued_id);
            return r && r->phase == TorrentPhase::cancelled;
        }, 15s));
    }

    // --- A job that exists nowhere still 404s cluster-wide, not just locally ---
    {
        const auto response = get(s2.acquisition_api(), "/api/v1/ingest/jobs/does-not-exist");
        CHECK(response.status == 404);
    }

    // --- Partial-peer-failure tolerance: node 1 alone still answers with its
    // own jobs once node 2 is unreachable, instead of erroring the request ---
    const auto node2_id = to_string(s2.node().node_id());
    s1.cluster_jobs().refresh_now(); // node 1 has now heard from node 2 at least once
    s2.stop();
    s1.cluster_jobs().refresh_now(); // and now cannot reach it
    {
        const auto response = get(s1.acquisition_api(), "/api/v1/ingest/jobs");
        REQUIRE(response.status == 200);
        const auto parsed = body_json(response);
        // Node 2 is still accounted for, as unreachable, not silently dropped.
        bool node2_listed = false;
        for (const auto& source : parsed.find("sources")->asArray())
            if (source.find("node_id")->asString() == node2_id) {
                node2_listed = true;
                CHECK(!source.find("reachable")->asBool());
                CHECK(!source.find("as_of_unix_ms")->isNull());
            }
        CHECK(node2_listed);
        const auto* jobs = parsed.find("jobs");
        REQUIRE(jobs != nullptr);
        bool found = false;
        for (const auto& job : jobs->asArray()) {
            const auto* id = job.find("id");
            if (id && id->asString() == ingest_id) found = true;
        }
        CHECK(found);
    }
    s1.stop();
}
#endif // MACHA_TEST_PLUGIN_DIR

MACHA_TEST("rpc_cluster", test_metadata_history_checkpoint_round_compacts_across_cluster) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "checkpoint-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "checkpoint-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    s1.filesystem().mkdir("/a", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/a").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));
    s2.filesystem().mkdir("/b", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s1.filesystem().getattr("/b").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));

    // The round's gate needs direct reachability, not merely gossip.
    REQUIRE(wait_until([&] {
        return s1.node().membership().all_known_reachable() &&
               s2.node().membership().all_known_reachable();
    }));

    CHECK(s1.node().metadata_replica().diagnostics().history_records >= 3);

    // s1 proposes, both ack, s1 commits and compacts in this call; s2 holds a
    // committed proof and re-roots its history.log on its own next attempt.
    s1.metadata_manager().attempt_history_checkpoint(1, 1);
    CHECK(s1.node().metadata_replica().diagnostics().history_records == 1);
    auto proof1 = s1.node().metadata_replica().checkpoint_proof();
    REQUIRE(proof1.has_value());
    CHECK(proof1->status == HistoryCheckpointProof::Status::committed);

    REQUIRE(wait_until([&] {
        s2.metadata_manager().attempt_history_checkpoint(1, 1);
        return s2.node().metadata_replica().diagnostics().history_records == 1;
    }));
    auto proof2 = s2.node().metadata_replica().checkpoint_proof();
    REQUIRE(proof2.has_value());
    CHECK(proof2->status == HistoryCheckpointProof::Status::committed);
    CHECK(proof2->floor_hash == proof1->floor_hash);
    CHECK(proof2->epoch == proof1->epoch);

    // Compaction must never be observable through ordinary reads.
    CHECK(s1.filesystem().getattr("/a").type == EntryType::directory);
    CHECK(s1.filesystem().getattr("/b").type == EntryType::directory);
    CHECK(s2.filesystem().getattr("/a").type == EntryType::directory);
    CHECK(s2.filesystem().getattr("/b").type == EntryType::directory);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_unreconstructable_accepted_head_is_repaired_live_from_a_peer) {
    // A head the local replica cannot replay is repaired from a peer while the
    // node runs: repair_unreconstructable_heads() -> the peer's
    // full_history_record() -> local reanchor_history().
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "repair-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "repair-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    s1.filesystem().mkdir("/a", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/a").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));

    auto& replica = s2.node().metadata_replica();
    auto certificates = replica.accepted_head_certificates();
    REQUIRE(certificates.size() == 1);
    const auto head = certificates.front().hash;
    CHECK(s1.node().metadata_replica().history_contains(head));

    // The replica on s2 cannot reconstruct the head, so excludes and flags it.
    replica.set_force_unreconstructable_for_tests(
        [head](const Hash256& hash) { return hash == head; });
    (void)replica.accepted_heads();
    REQUIRE(wait_until([&] {
        return replica.unreconstructable_heads() == std::vector<Hash256>{head};
    }));
    // Lift the fault; the flag's 30 s cooldown means only repair can clear it here.
    replica.set_force_unreconstructable_for_tests({});

    REQUIRE(wait_until([&] {
        (void)s2.metadata_manager().repair_unreconstructable_heads();
        return replica.unreconstructable_heads().empty();
    }));
    CHECK(replica.accepted_heads().size() == 1);
    CHECK(replica.accepted_heads().front().hash == head);
    CHECK(s2.filesystem().getattr("/a").type == EntryType::directory);

    // The wire call itself, independently of the driver.
    Writer request;
    request.fixed(head.bytes);
    auto reply = s2.node().call(s1.node().membership().self(),
                                MessageType::get_metadata_history_record, request.take(),
                                FrameType::control);
    REQUIRE(reply.message.type == MessageType::metadata_history_entry_reply);
    auto served = decode_metadata_history_entry(reply.message.payload);
    CHECK(served.hash == head);
    CHECK(served.body == MetadataHistoryEntry::Body::full);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_metadata_history_checkpoint_concurrent_proposers_converge) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "concurrent-checkpoint-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "concurrent-checkpoint-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    s1.filesystem().mkdir("/concurrent", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/concurrent").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));
    REQUIRE(wait_until([&] {
        return s1.node().membership().all_known_reachable() &&
               s2.node().membership().all_known_reachable();
    }));

    // Both nodes propose at once; the leaderless protocol is idempotent by
    // (floor_hash, epoch), so both converge on the same committed proof.
    std::thread t1([&] { s1.metadata_manager().attempt_history_checkpoint(1, 1); });
    std::thread t2([&] { s2.metadata_manager().attempt_history_checkpoint(1, 1); });
    t1.join();
    t2.join();

    // The loser's next attempt converges via the same (floor_hash, epoch).
    REQUIRE(wait_until([&] {
        s1.metadata_manager().attempt_history_checkpoint(1, 1);
        s2.metadata_manager().attempt_history_checkpoint(1, 1);
        return s1.node().metadata_replica().diagnostics().history_records == 1 &&
               s2.node().metadata_replica().diagnostics().history_records == 1;
    }));

    auto proof1 = s1.node().metadata_replica().checkpoint_proof();
    auto proof2 = s2.node().metadata_replica().checkpoint_proof();
    REQUIRE(proof1.has_value());
    REQUIRE(proof2.has_value());
    CHECK(proof1->floor_hash == proof2->floor_hash);
    CHECK(proof1->epoch == proof2->epoch);
    CHECK(s1.node().metadata_replica().committed().hash == proof1->floor_hash);
    CHECK(s2.node().metadata_replica().committed().hash == proof2->floor_hash);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_metadata_history_checkpoint_aborts_when_a_participant_is_unreachable) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "unreachable-checkpoint-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "unreachable-checkpoint-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    s1.filesystem().mkdir("/unreachable", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/unreachable").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));
    REQUIRE(wait_until([&] {
        return s1.node().membership().all_known_reachable() &&
               s2.node().membership().all_known_reachable();
    }));

    // An unreachable durably known participant aborts the round
    // (discover_accepted_heads_required(), independent of all_known_reachable()).
    s2.stop();
    s1.metadata_manager().attempt_history_checkpoint(1, 1);
    CHECK(s1.node().metadata_replica().diagnostics().history_records > 1);
    CHECK(!s1.node().metadata_replica().checkpoint_proof().has_value());

    s1.stop();
}

MACHA_TEST("rpc_cluster", test_metadata_history_checkpoint_recovers_after_crash_between_ack_and_commit) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "crash-checkpoint-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "crash-checkpoint-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    s1.filesystem().mkdir("/crash", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/crash").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));
    REQUIRE(wait_until([&] {
        return s1.node().membership().all_known_reachable() &&
               s2.node().membership().all_known_reachable();
    }));

    // A proposer that crashed after every ack but before the commit: an
    // acked-only proof on s2 for the (floor_hash, epoch) a round would produce.
    const auto floor = s1.node().metadata_replica().accepted_heads();
    REQUIRE(floor.size() == 1);
    HistoryCheckpointProof stranded;
    stranded.floor_hash = floor.front().hash;
    stranded.floor_generation = floor.front().generation;
    stranded.epoch.bytes[0] = 0x99;
    stranded.participants = {s1.node().node_id(), s2.node().node_id()};
    s2.node().metadata_replica().record_checkpoint_ack(stranded);
    CHECK(s2.node().metadata_replica().checkpoint_proof()->status ==
         HistoryCheckpointProof::Status::acked);
    // A stranded ack alone never re-roots s2's history.
    CHECK(s2.node().metadata_replica().diagnostics().history_records > 1);

    // The next maintenance cycle re-proposes and completes, superseding it.
    REQUIRE(wait_until([&] {
        s1.metadata_manager().attempt_history_checkpoint(1, 1);
        return s1.node().metadata_replica().diagnostics().history_records == 1;
    }));
    REQUIRE(wait_until([&] {
        s2.metadata_manager().attempt_history_checkpoint(1, 1);
        return s2.node().metadata_replica().diagnostics().history_records == 1;
    }));
    auto proof2 = s2.node().metadata_replica().checkpoint_proof();
    REQUIRE(proof2.has_value());
    CHECK(proof2->status == HistoryCheckpointProof::Status::committed);
    CHECK(proof2->epoch != stranded.epoch);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_a_hung_health_probe_is_retried_inside_the_liveness_budget) {
    // A hung health ping is retried within the liveness budget, not held for
    // all of dead_after. The stall fixture holds every ping unanswered and
    // stalled_calls_for_tests() counts the attempts.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "probe-budget-n1", cluster.keyfile(), p1,
                         {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "probe-budget-n2", cluster.keyfile(), p2,
                         {{"127.0.0.1", p1}});
    // A 3 s liveness budget gives 1 s attempts: three per window.
    c1.dead_after = c2.dead_after = 3s;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    const auto peer = s2.node().node_id();
    s1.node().stall_peer_for_tests(peer, MessageType::ping);
    const auto started = std::chrono::steady_clock::now();
    const bool retried =
        wait_until([&] { return s1.node().stalled_calls_for_tests() >= 3; }, 3s);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    s1.node().release_peer_for_tests(peer);

    CHECK(retried);
    CHECK(elapsed < c1.dead_after);
}

// A node that accepts no inbound connections, modelled by a black-hole
// advertised address (192.0.2.1, TEST-NET-1) and a short connect timeout.
MACHA_TEST("rpc_cluster", test_inbound_incapable_node_is_reached_only_over_its_own_sessions) {
    TestCluster cluster;
    const auto& keys = cluster.keys();

    struct FlagNode {
        NodeInfo info;
        RpcClient client;
        RpcServer server;

        FlagNode(ClusterKeys keys, NodeInfo node, std::chrono::milliseconds connect_timeout)
            : info(std::move(node)),
              client(
                  keys, [this] { return info; }, [](const NodeInfo&) {}, [](uint64_t) {},
                  connect_timeout, 100ms, 30s),
              server(
                  "127.0.0.1", info.port, keys, info,
                  [this](const NodeInfo& peer, FrameType, const RpcMessage& request) {
                      return handle(peer, request);
                  },
                  [](const NodeInfo&) {}) {
            server.attach_client(client);
            server.start();
        }
        ~FlagNode() {
            server.stop();
            client.stop();
        }
        // What NodeRuntime::handle does for the two new messages.
        RpcMessage handle(const NodeInfo& peer, const RpcMessage& request) {
            if (request.type == MessageType::dial_request) {
                Reader reader(request.payload);
                const auto lane = static_cast<TransportLane>(reader.u8());
                reader.finish();
                client.request_lane(peer, lane);
                return {MessageType::ok, {}};
            }
            return {MessageType::ok, request.payload};
        }
    };

    auto capable_info = [] {
        NodeInfo node;
        node.id = random_node_id();
        node.host = "127.0.0.1";
        node.port = free_port();
        node.failure_domain = "hub";
        return node;
    };
    auto incapable_info = [] {
        NodeInfo node;
        node.id = random_node_id();
        node.host = "192.0.2.1"; // a routing key, never an address anyone reaches
        node.port = free_port();
        node.failure_domain = "cgnat";
        node.flags = node_flags_for(false, true);
        return node;
    };

    // A peer that knows the node cannot be dialled does not try, and says
    // so at once rather than after a connect timeout.
    {
        FlagNode hub(keys, capable_info(), 2s);
        FlagNode site(keys, incapable_info(), 300ms);
        hub.client.note_peer(site.info);
        const auto started = Clock::now();
        bool refused = false;
        try {
            (void)hub.client.call(site.info, MessageType::members, Bytes{1}, 1s);
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("accepts no inbound connections") !=
                      std::string::npos;
        }
        CHECK(refused);
        CHECK(Clock::now() - started < 1s);
        CHECK(hub.client.stats().connections_created == 0);
    }

    FlagNode hub(keys, capable_info(), 2s);
    FlagNode site(keys, incapable_info(), 300ms);

    // Control flows both ways over the one session the site opened.
    CHECK(site.client.call(hub.info, MessageType::members, Bytes{1}, 1s).message.payload ==
          Bytes{1});
    CHECK(hub.client.call(site.info, MessageType::members, Bytes{2}, 1s).message.payload ==
          Bytes{2});
    CHECK(hub.client.stats().connections_created == 0);
    CHECK(site.client.stats().connections_created == 1);

    // The hub needs the DATA lane the site has not opened: it asks, the site
    // dials, the call completes -- and the hub still never dialled anything.
    CHECK(hub.client.call(site.info, MessageType::get_object, Bytes{3}, FrameType::foreground, 5s)
              .message.payload == Bytes{3});
    CHECK(hub.client.dial_requests_sent() == 1);
    CHECK(site.client.dial_requests_received() == 1);
    CHECK(hub.client.stats().connections_created == 0);
    CHECK(site.client.stats().connections_created == 2);
    CHECK(hub.client.has_route(site.info.id, TransportLane::data));
    CHECK(site.client.has_route(hub.info.id, TransportLane::data));

    // A DATA lane that dies underneath (a NAT mapping expiring) is redialled
    // by the site on its own initiative, without being asked.
    site.client.set_maintained_peers([&] { return std::vector<NodeInfo>{hub.info}; });
    site.client.close_lane_for_tests(hub.info.id, TransportLane::data);
    REQUIRE(wait_until([&] { return site.client.has_route(hub.info.id, TransportLane::data); },
                       5s));
    CHECK(hub.client.dial_requests_sent() == 1);
    CHECK(hub.client.stats().connections_created == 0);
    CHECK(hub.client.call(site.info, MessageType::get_object, Bytes{4}, FrameType::foreground, 5s)
              .message.payload == Bytes{4});
    CHECK(hub.client.dial_requests_sent() == 1);

    // The dial-back probe: a fresh connection, a handshake, nothing else.
    // The hub can be probed; the site cannot, and the failure names why.
    const auto hub_created = hub.client.stats().connections_created;
    CHECK(site.client.probe_dial(Endpoint{hub.info.host, hub.info.port}, hub.info.id).empty());
    CHECK(!hub.client.probe_dial(Endpoint{site.info.host, site.info.port}, site.info.id).empty());
    CHECK(hub.client.stats().connections_created == hub_created);
    CHECK(hub.client.stats().canonical_connections == 2);
    CHECK(site.client.stats().canonical_connections == 2);
}

MACHA_TEST("rpc_cluster", test_a_call_whose_dial_is_retired_by_a_simultaneous_connect_waits_for_the_peer) {
    // In a simultaneous connect the lower id keeps its own dial and retires the
    // other; the higher id's call may find its dial retired before the peer's
    // session registers. That gap is forced here; the call must wait it out.
    struct Node {
        NodeInfo info;
        RpcClient client;
        RpcServer server;
        Node(ClusterKeys keys, NodeInfo node)
            : info(std::move(node)),
              client(
                  keys, [this] { return info; }, [](const NodeInfo&) {}, [](uint64_t) {}, 2s,
                  100ms, 30s),
              server(
                  "127.0.0.1", info.port, keys, info,
                  [](const NodeInfo&, FrameType, const RpcMessage& request) {
                      return RpcMessage{MessageType::ok, request.payload};
                  },
                  [](const NodeInfo&) {}) {
            server.attach_client(client);
            server.start();
        }
        ~Node() {
            server.stop();
            client.stop();
        }
    };
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto info = [] {
        NodeInfo node;
        node.id = random_node_id();
        node.host = "127.0.0.1";
        node.port = free_port();
        node.failure_domain = "loopback";
        return node;
    };
    auto a = info();
    auto b = info();
    if (b.id < a.id) std::swap(a, b);
    Node low(keys, a);
    Node high(keys, b);

    // The low node's session reaches the high node but is not yet registered.
    high.client.hold_inbound_for_tests(low.info.id);
    REQUIRE(low.client.call(high.info, MessageType::members, Bytes{1}, 2s).message.payload ==
            Bytes{1});
    REQUIRE(!high.client.has_route(low.info.id, TransportLane::control));

    // The low node retires the high node's dial; the call waits for that.
    std::atomic_bool retired{false};
    high.client.set_after_dial_for_tests([&] {
        retired.store(wait_until(
            [&] { return !high.client.has_route(low.info.id, TransportLane::control); }, 5s));
    });
    auto call = std::async(std::launch::async, [&] {
        return high.client.call(low.info, MessageType::members, Bytes{2}, 5s);
    });
    REQUIRE(wait_until([&] { return retired.load(); }, 5s));
    // The low node's session now registers, as it would a moment later.
    high.client.release_inbound_for_tests(low.info.id);
    CHECK(call.get().message.payload == Bytes{2});
}

// `network.inbound_capable: auto`: resolved from a peer's dial-back, persisted,
// and reversed once the address becomes dialable.
MACHA_TEST("rpc_cluster", test_inbound_auto_resolves_from_dial_back_and_survives_restart) {
    TestCluster cluster(ConfigProfile::isolated);
    const auto& keys = cluster.keys();

    auto hub_config = cluster.node_config("hub");
    hub_config.dial_back_probe_min_interval = 100ms;

    struct RuntimeNode {
        Config config;
        const ClusterKeys& keys;
        std::unique_ptr<BareNode> node;
        RuntimeNode(Config c, const ClusterKeys& k) : config(std::move(c)), keys(k) {}
        NodeRuntime& start() {
            for (const auto& backend : config.storage_backends)
                std::filesystem::create_directories(backend.path);
            node = std::make_unique<BareNode>(config, keys);
            node->start();
            REQUIRE(node->wait_local_state_ready(10s));
            return *node;
        }
        void stop() {
            if (node)
                node->stop();
            node.reset();
        }
        ~RuntimeNode() { stop(); }
    };

    RuntimeNode hub(hub_config, keys);
    hub.start();

    auto site_config = cluster.node_config(
        "site", 0, {Endpoint{hub_config.advertise_host, hub_config.port}});
    site_config.advertise_host = "192.0.2.1";
    site_config.connect_timeout = 300ms;
    site_config.inbound_reprobe_while_incapable = 300ms;
    site_config.inbound_reprobe_while_capable = 300ms;
    site_config.dial_back_probe_min_interval = 100ms;
    RuntimeNode site(site_config, keys);
    auto& site_node = site.start();
    const auto site_id = site_node.node_id();
    CHECK(site_node.inbound_resolution().source == "default");
    CHECK(site_node.inbound_capable());

    // Two dial-backs from the hub fail against the black hole: the site now
    // says it cannot be reached, and stops hosting extents (auto follows).
    REQUIRE(wait_until([&] { return !site_node.inbound_capable(); }, 20s));
    {
        const auto resolution = site_node.inbound_resolution();
        CHECK(!resolution.hosts_extents);
        CHECK(resolution.source == "probe:" + to_string(hub.node->node_id()));
        CHECK(resolution.consecutive_probe_failures >= 2);
        CHECK(!resolution.last_probe_error.empty());
    }
    // The hub sees the gossiped flags and stops dialling.
    REQUIRE(wait_until([&] { return !hub.node->membership().inbound_capable(site_id); }, 10s));
    CHECK(!hub.node->membership().hosts_extents(site_id));
    CHECK(hub.node->membership().all_known_reachable());

    // The resolution is persisted: a restart starts from it, not from "default".
    site.stop();
    auto& restarted = site.start();
    CHECK(!restarted.inbound_capable());
    CHECK(restarted.inbound_resolution().source == "persisted");

    // Once the advertised address is genuinely dialable, one successful
    // dial-back flips it back, and hosting follows.
    site.stop();
    site.config.advertise_host = "127.0.0.1";
    auto& reachable = site.start();
    CHECK(!reachable.inbound_capable()); // persisted, until evidence says otherwise
    REQUIRE(wait_until([&] { return reachable.inbound_capable(); }, 20s));
    CHECK(reachable.hosts_extents());
    REQUIRE(wait_until([&] { return hub.node->membership().inbound_capable(site_id); }, 10s));
    CHECK(hub.node->membership().hosts_extents(site_id));

    // A founding node that cannot be dialled is refused outright.
    auto founder = cluster.node_config("founder");
    founder.inbound_capable = Tristate::no;
    RuntimeNode refused(founder, keys);
    bool threw = false;
    try {
        refused.start();
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("founding node") != std::string::npos;
    }
    CHECK(threw);
}

} // namespace

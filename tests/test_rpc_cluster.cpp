// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "fake_cluster_node.hpp"
#include "acquisition/cluster_jobs.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "torrent/torrent_coordinator.hpp"
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

MACHA_FAST_TEST("rpc_cluster", test_async_rpc_move_ownership) {
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

    NetworkLinks links;
    RpcClient local_client(
        links, keys, [local_info] { return local_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 5s, 30s, 4096);
    RpcClient remote_client(
        links, keys, [remote_info] { return remote_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 5s, 30s, 4096);
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
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 5s, 30s, 4096);
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
    REQUIRE(foreground.wait_for(scaled(2s)) == std::future_status::ready);
    CHECK(foreground.get().message.type == MessageType::ok);
    {
        std::lock_guard lock(order_mutex);
        REQUIRE(!order.empty());
        CHECK(order.front() == 0x46);
    }
    // Completion, not speed: 32 MiB takes from 5 s to tens of seconds by build
    // and load, so the bound is just inside the case's 60 s deadline.
    REQUIRE(background.wait_for(scaled(50s)) == std::future_status::ready);
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

// DistributedStore against a FakeClusterNode: placement, puts, fetches and
// repair as the store decides them, with peers answering in process.

// Objects of `size` bytes, stored locally; returns their ids sorted.
std::vector<ObjectId> local_objects(StoreBench& bench, size_t count, size_t size, uint8_t salt) {
    std::vector<ObjectId> ids;
    for (size_t i = 0; i < count; ++i) {
        const auto bytes = pattern(size, static_cast<uint8_t>(salt + i));
        const auto id = object_id(bytes);
        REQUIRE(bench.local.data().put(id, bytes));
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// A peer put's durability token, as a node answers put_object_deferred.
RpcMessage deferred_put_reply() {
    Writer writer;
    writer.fixed(random_node_id().bytes);
    writer.u64(1);
    writer.u64(1);
    writer.u64(1);
    return {MessageType::ok, writer.take()};
}

// A put is loader work: it refreshes the loader clock and leaves the viewer
// clocks, which pace maintenance and gate DATA pressure, untouched.
MACHA_FAST_TEST("rpc_cluster", test_store_put_is_loader_activity_not_viewer_activity) {
    StoreBench bench;
    auto& activity = bench.resources.activity;
    for (auto frame : {FrameType::loader, FrameType::foreground, FrameType::read_ahead})
        (void)activity.take_bytes(frame);
    auto store = bench.store();
    const auto bytes = pattern(256 * 1024, 91);
    REQUIRE(store->put(bytes) == object_id(bytes));
    CHECK(store->loader_idle_for() < 5s);
    CHECK(store->take_loader_bytes() >= bytes.size());
    CHECK(store->take_foreground_bytes() == 0);
    CHECK(store->take_interactive_bytes() == 0);
    CHECK(!activity.viewer_recently_active(30s));
}

// A put publishes at write_copies with the local copy, never waiting
// for owners beyond the floor; the prompt worker then copies it to one more
// owner, and never to an owner that gossips no room.
MACHA_FAST_TEST("rpc_cluster", test_store_put_publishes_at_the_floor_and_copies_promptly) {
    StoreBench bench([](Config& c) { c.replication = 3; });
    TestGate owners_answer;
    std::atomic_bool put_returned{};
    std::atomic_bool owners_opened{};
    const auto held_put = [&](MessageType type, const Bytes&, FrameType) {
        if (type == MessageType::put_object)
            owners_answer.enter_and_wait();
        if (type == MessageType::have_object)
            return RpcMessage{MessageType::bool_reply, Bytes{0}};
        return RpcMessage{MessageType::ok, {}};
    };
    const auto first = StoreBench::peer();
    const auto second = StoreBench::peer();
    bench.node.add_peer(first, held_put);
    bench.node.add_peer(second, held_put);
    // Opens the owners whatever happens, so a put that waits for them ends.
    std::jthread release([&] {
        (void)wait_until([&] { return put_returned.load(); }, 2s);
        owners_opened = true;
        owners_answer.open();
    });

    auto store = bench.store();
    const auto data = pattern(128 * 1024);
    const auto id = object_id(data);
    CHECK(store->put(id, data));
    CHECK(!owners_opened.load());
    put_returned = true;
    CHECK(bench.local.data().has(id));
    release.join();
    REQUIRE(wait_until([&] { return store->prompt_replication_stats().copies == 1; }));
    CHECK(bench.node.calls_of(MessageType::put_object) == 1);

    // An owner gossiping no room is no destination: nothing is sent, and the
    // object is left to repair.
    StoreBench full_bench([](Config& c) { c.replication = 2; });
    auto full = StoreBench::peer(1024ULL * 1024 * 1024);
    full.used = full.capacity - 81;
    full_bench.node.add_peer(full, [](MessageType type, const Bytes&, FrameType) {
        if (type == MessageType::have_object)
            return RpcMessage{MessageType::bool_reply, Bytes{0}};
        return RpcMessage{MessageType::ok, {}};
    });
    auto full_store = full_bench.store();
    const auto other = pattern(128 * 1024, 7);
    CHECK(full_store->put(object_id(other), other));
    REQUIRE(wait_until([&] { return full_store->repair_diagnostics().prompt_skipped_no_room == 1; }));
    const auto diagnostics = full_store->repair_diagnostics();
    CHECK(diagnostics.prompt_dropped == 1);
    CHECK(diagnostics.prompt_copies == 0);
    CHECK(diagnostics.prompt_failures == 0);
    CHECK(full_bench.node.calls_of(MessageType::put_object) == 0);
}

// A replica whose call cannot be placed counts as failed at once, so the put
// launches the next owner in its place rather than waiting on it.
MACHA_FAST_TEST("rpc_cluster", test_store_put_replaces_a_replica_that_cannot_be_launched) {
    StoreBench bench([](Config& c) {
        c.replication = 3;
        c.write_copies = 2;
    });
    bench.node.add_peer(StoreBench::peer());
    bench.node.add_peer(StoreBench::peer());

    // The peer placement tries first after the local copy cannot be reached;
    // the other stands by.
    const auto data = pattern(64 * 1024, 5);
    std::vector<NodeId> remote;
    for (const auto& node : capacity_placement_nodes(object_id(data).bytes,
                                                     bench.node.membership().active(), 3))
        if (node.id != bench.node.node_id())
            remote.push_back(node.id);
    REQUIRE(remote.size() == 2);
    const auto refused = remote[0];
    const auto standby = remote[1];
    bench.node.refuse(refused);
    // Bounds a put that never replaces the refused replica.
    std::atomic_bool cancelled{};
    std::jthread watchdog([&](std::stop_token stop) {
        const auto deadline = Clock::now() + 2s;
        while (!stop.stop_requested() && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        if (!stop.stop_requested())
            cancelled = true;
    });
    auto store = bench.store();
    CHECK(store->put(object_id(data), data, &cancelled));
    watchdog.request_stop();
    CHECK(!cancelled.load());
    CHECK(bench.node.calls_of(MessageType::put_object, standby) == 1);
}

// Claiming objects that no node present holds costs the one batched presence
// scan and nothing per object: they are reported unheld. An object the peer
// does hold is claimed there.
MACHA_FAST_TEST("rpc_cluster", test_store_claims_ask_nothing_more_about_objects_nobody_holds) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    const auto held_bytes = pattern(256, 201);
    const auto held = object_id(held_bytes);
    std::mutex mutex;
    std::map<MessageType, size_t> calls;
    bench.node.add_peer(StoreBench::peer(), [&](MessageType type, const Bytes& request,
                                                FrameType) {
        {
            std::lock_guard lock(mutex);
            ++calls[type];
        }
        if (type == MessageType::have_objects) {
            // Holds exactly `held`.
            Reader reader(request);
            const auto count = reader.u32();
            Writer writer;
            writer.u32(count);
            for (uint32_t i = 0; i < count; ++i)
                writer.u8(ObjectId{reader.fixed<32>()} == held ? 1 : 0);
            return RpcMessage{MessageType::have_objects_reply, writer.take()};
        }
        return RpcMessage{MessageType::ok, {}};
    });
    auto store = bench.store();
    std::vector<ObjectId> ids{held};
    for (uint8_t i = 0; i < 40; ++i)
        ids.push_back(object_id(pattern(128, i)));
    const auto unheld = store->retain_data(ids, RetentionDot{bench.node.node_id(), 1});
    CHECK(unheld.size() == 40);
    CHECK(!std::binary_search(unheld.begin(), unheld.end(), held));
    std::lock_guard lock(mutex);
    CHECK(calls[MessageType::have_objects] == 1);
    CHECK(calls[MessageType::get_object] == 0);
    CHECK(calls[MessageType::have_object] == 0);
    CHECK(calls[MessageType::retain_objects] == 1);
}

// A put whose second replica never answers returns on the local copy once
// the peer has stalled, well inside the work's no-progress budget, and the
// same put reaches the peer once it answers.
MACHA_FAST_TEST("rpc_cluster", test_store_put_to_a_silent_replica_returns_on_the_local_copy) {
    StoreBench bench([](Config& c) {
        c.replication = 2;
        c.write_copies = 2;
        c.write_stall = 50ms;
    });
    const auto peer_puts = [&] {
        return bench.node.calls_of(MessageType::put_object_deferred);
    };
    std::atomic_bool silent{true};
    TestGate held;
    const auto peer = StoreBench::peer();
    bench.node.add_peer(peer, [&](MessageType type, const Bytes&, FrameType) {
        if (type == MessageType::put_object_deferred && silent.load())
            held.enter_and_wait();
        return deferred_put_reply();
    });
    auto store = bench.store();
    const auto data = pattern(64 * 1024, 3);
    std::atomic_uint64_t progress{};
    const DataWorkContext work(FrameType::loader, bench.config().extent_size, {}, nullptr,
                               &progress, scaled(30s));
    DistributedStore::DurabilityBatch batch;
    const auto started = Clock::now();
    CHECK(store->put_deferred(data, batch, FrameType::loader, nullptr, &work) == object_id(data));
    const auto elapsed = Clock::now() - started;
    CHECK(elapsed >= 50ms);
    CHECK(elapsed < scaled(5s));
    CHECK(bench.local.data().has(object_id(data)));
    CHECK(!batch.empty());

    silent = false;
    held.open();
    const auto before = peer_puts();
    const auto more = pattern(64 * 1024, 4);
    DistributedStore::DurabilityBatch again;
    CHECK(store->put_deferred(more, again, FrameType::loader, nullptr, &work) == object_id(more));
    CHECK(peer_puts() == before + 1);
}

// Readers of one missing object share a single fetch and a single retained
// buffer; the reply's memory lease lives as long as the last reader's handle.
MACHA_FAST_TEST("rpc_cluster", test_store_concurrent_readers_share_one_fetch_and_buffer) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    const auto bytes = pattern(512 * 1024, 73);
    const auto id = object_id(bytes);
    auto& memory = bench.resources.memory;
    TestGate reply_gate;
    bench.node.add_peer(StoreBench::peer(), [&](MessageType type, const Bytes&, FrameType) {
        if (type != MessageType::get_object)
            return RpcMessage{MessageType::ok, {}};
        reply_gate.enter_and_wait();
        auto reply = object_reply(bytes);
        reply.retained_memory = std::make_shared<std::vector<RetainedMemoryLedger::Lease>>();
        reply.retained_memory->push_back(*memory.try_acquire(
            MemoryClass::viewer, MemoryOwner::rpc_frame, reply.payload.size()));
        return reply;
    });
    const auto held = [&] {
        return memory.stats().owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)];
    };

    auto store = bench.store();
    auto first = std::async(std::launch::async,
                            [&] { return store->get_shared(id, 0, FrameType::foreground); });
    REQUIRE(reply_gate.wait_for_entries(1));
    auto second = std::async(std::launch::async,
                             [&] { return store->get_shared(id, 0, FrameType::foreground); });
    // The second reader joins the fetch in flight; it has no signal to wait on.
    std::this_thread::sleep_for(20ms);
    reply_gate.open();
    auto a = first.get();
    auto b = second.get();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() == b.get());
    CHECK(a->bytes == bytes);
    CHECK(bench.node.calls_of(MessageType::get_object) == 1);
    CHECK(held() >= bytes.size());
    a.reset();
    CHECK(held() >= bytes.size());
    b.reset();
    CHECK(wait_until([&] { return held() == 0; }));
}

// A peer for repair: answers presence as set, counts what it was asked, and
// accepts puts, holding each at `put_gate` while `hold_puts` is set.
struct RepairPeer {
    NodeInfo info{StoreBench::peer()};
    std::atomic_bool knows_batch{true};
    std::atomic_bool holds_everything{};
    std::atomic_bool hold_puts{};
    std::atomic_uint batch_requests{};
    std::atomic_uint single_probes{};
    std::atomic_uint puts{};
    std::atomic_uint puts_in_flight{};
    std::atomic_uint max_puts_in_flight{};
    std::atomic_uint fetches{};
    std::optional<Bytes> serves;
    TestGate put_gate;

    explicit RepairPeer(StoreBench& bench) {
        bench.node.add_peer(info, [this](MessageType type, const Bytes& request, FrameType) {
            return answer(type, request);
        });
    }
    RpcMessage answer(MessageType type, const Bytes& request) {
        switch (type) {
        case MessageType::have_valid_objects:
            if (!knows_batch.load())
                return {MessageType::error, {}};
            ++batch_requests;
            return presence_reply(request, holds_everything.load());
        case MessageType::have_object:
            ++single_probes;
            return {MessageType::bool_reply, Bytes{static_cast<uint8_t>(holds_everything.load())}};
        case MessageType::put_object: {
            const auto now = ++puts_in_flight;
            for (auto seen = max_puts_in_flight.load();
                 now > seen && !max_puts_in_flight.compare_exchange_weak(seen, now);) {
            }
            if (hold_puts.load())
                put_gate.enter_and_wait();
            --puts_in_flight;
            ++puts;
            return {MessageType::ok, {}};
        }
        case MessageType::get_object:
            ++fetches;
            if (serves)
                return object_reply(*serves);
            return {MessageType::error, {}};
        default:
            return {MessageType::ok, {}};
        }
    }
};

// A push step asks about its whole window, have_valid_objects_max ids a
// request; a peer without have_valid_objects is asked one id at a time. All
// of a step's pushes are in flight together, and the bytes go at speculative
// priority.
MACHA_FAST_TEST("rpc_cluster", test_repair_push_probes_by_window_and_sends_together) {
    {
        StoreBench bench([](Config& c) { c.replication = 2; });
        RepairPeer peer(bench);
        peer.holds_everything = true;
        const auto live = local_objects(bench, 40, 8 * 1024, 30);
        auto store = bench.store();
        const auto step = store->repair_step(8ULL * 1024 * 1024, 16, live);
        CHECK(step.push_examined == live.size());
        CHECK(peer.batch_requests.load() == 3); // 16 + 16 + 8
        CHECK(peer.single_probes.load() == 0);
        CHECK(peer.puts.load() == 0);

        peer.knows_batch = false;
        auto older = bench.store();
        const auto fallback = older->repair_step(8ULL * 1024 * 1024, 16, live);
        CHECK(fallback.push_examined == live.size());
        CHECK(peer.single_probes.load() == live.size());
        CHECK(peer.puts.load() == 0);
    }
    {
        StoreBench bench([](Config& c) { c.replication = 2; });
        RepairPeer peer(bench);
        peer.hold_puts = true;
        const auto live = local_objects(bench, 8, 24 * 1024, 40);
        // Opens the puts once all eight are held, or after a bound so a
        // serial repair ends and fails the check below.
        std::jthread release([&] {
            (void)peer.put_gate.wait_for_entries(live.size(), 2s);
            peer.put_gate.open();
        });
        auto store = bench.store();
        const auto step = store->repair_step(64ULL * 1024 * 1024, 16, live);
        release.join();
        CHECK(peer.puts.load() == live.size());
        CHECK(step.push_examined == live.size());
        CHECK(step.bytes_transferred == live.size() * 24 * 1024);
        CHECK(peer.max_puts_in_flight.load() == live.size());
        for (const auto& call : bench.node.calls())
            if (call.type == MessageType::put_object)
                CHECK(call.frame_type == FrameType::speculative);
    }
}

// A peer whose advertised free space cannot take an extent is neither probed
// nor sent one; the copy goes to the next node in rank, and the local copy
// stays. Once the peer has room it is pushed to.
MACHA_FAST_TEST("rpc_cluster", test_repair_pushes_past_a_peer_with_no_room) {
    StoreBench bench([](Config& c) { c.replication = 3; });
    const auto capacity = bench.local.data().limit();
    auto full = StoreBench::peer(capacity, capacity - 81);
    const auto fallback = StoreBench::peer(capacity);
    const auto owner = StoreBench::peer(capacity);
    std::atomic_uint full_calls{};
    bench.node.add_peer(full, [&](MessageType, const Bytes&, FrameType) {
        ++full_calls;
        return RpcMessage{MessageType::ok, {}};
    });
    const auto absent = [](MessageType type, const Bytes& request, FrameType) {
        if (type == MessageType::have_valid_objects)
            return presence_reply(request, false);
        return RpcMessage{MessageType::ok, {}};
    };
    bench.node.add_peer(fallback, absent);
    bench.node.add_peer(owner, absent);

    // An object the full peer would own: among the first three in rank.
    Bytes bytes;
    for (uint32_t salt = 0;; ++salt) {
        REQUIRE(salt < 4096);
        bytes = pattern(64 * 1024 + salt, 31);
        const auto ranked = capacity_placement_nodes(object_id(bytes).bytes,
                                                     bench.node.membership().active(), 3);
        if (ranked.size() == 4 && ranked[3].id == fallback.id &&
            std::any_of(ranked.begin(), ranked.begin() + 3,
                        [&](const NodeInfo& node) { return node.id == full.id; }))
            break;
    }
    const auto id = object_id(bytes);
    REQUIRE(bench.local.data().put(id, bytes));
    const std::vector<ObjectId> live{id};
    auto store = bench.store();
    for (int pass = 0; pass < 4; ++pass)
        (void)store->repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(full_calls.load() == 0);
    CHECK(bench.node.calls_of(MessageType::put_object, fallback.id) > 0);
    CHECK(bench.node.calls_of(MessageType::put_object, owner.id) > 0);
    CHECK(bench.local.data().has(id));

    full.used = 0;
    full.seen_unix_ms += 1000;
    bench.node.membership().observe(full, true);
    auto again = bench.store();
    for (int pass = 0; pass < 4 && full_calls.load() == 0; ++pass)
        (void)again->repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(full_calls.load() > 0);
}

// Probing is paid for by the operation budget; credit pays only for a
// transfer's bytes. A push waits for credit of its object's size; a pull
// passes held objects by index, waits for an extent of credit, and the cursor
// stays on the object it could not afford.
MACHA_FAST_TEST("rpc_cluster", test_repair_probes_without_credit_and_transfers_only_with_it) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    const auto held_bytes = pattern(256 * 1024, 41);
    const auto held = object_id(held_bytes);
    const auto wanted_bytes = pattern(256 * 1024, 42);
    const auto wanted = object_id(wanted_bytes);
    peer.serves = wanted_bytes;
    REQUIRE(bench.local.data().put(held, held_bytes));

    auto store = bench.store();
    REQUIRE(store->should_own(wanted));
    const std::vector<ObjectId> held_live{held};
    auto push = store->repair_step(held_bytes.size() - 1, 16, held_live);
    CHECK(peer.batch_requests.load() == 1);
    CHECK(peer.puts.load() == 0);
    CHECK(push.credit_limited);
    CHECK(!push.complete);
    push = store->repair_step(held_bytes.size(), 16, held_live);
    CHECK(peer.puts.load() == 1);
    CHECK(!push.credit_limited);
    peer.holds_everything = true;

    std::vector<ObjectId> pull_live{held, wanted};
    std::sort(pull_live.begin(), pull_live.end());
    const auto extent = bench.config().extent_size;
    auto puller = bench.store();
    DistributedStore::RepairResult pull;
    for (int step = 0; step < 8 && !pull.credit_limited; ++step)
        pull = puller->repair_step(extent - 1, 16, pull_live);
    CHECK(pull.credit_limited);
    CHECK(peer.fetches.load() == 0);
    CHECK(!bench.local.data().has(wanted));
    for (int step = 0; step < 8 && !bench.local.data().has(wanted); ++step)
        (void)puller->repair_step(extent, 16, pull_live);
    CHECK(peer.fetches.load() == 1);
    CHECK(bench.local.data().has(wanted));
}

// The push cursor survives a new live-set generation and a restart (each
// object passed again would cost a full read on the peer). With no
// generation, a pass a different live set arrived part-way through is not
// reported settled; the same set again is.
MACHA_FAST_TEST("rpc_cluster", test_repair_pass_keeps_its_place_across_live_sets_and_restarts) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    peer.holds_everything = true;
    const auto live = local_objects(bench, 100, 4 * 1024, 50);
    const auto repair_dir = bench.config().state_path / "repair";
    {
        auto store = bench.store(DistributedStoreOptions{repair_dir / "push-position", {}});
        const auto first = store->repair_step(64ULL * 1024 * 1024, 16, live, {}, 1);
        CHECK(first.push_examined == 64);
        CHECK(peer.batch_requests.load() == 4);
        const auto second = store->repair_step(64ULL * 1024 * 1024, 16, live, {}, 2);
        CHECK(second.push_examined == 36);
        CHECK(peer.batch_requests.load() == 4 + 3);
        CHECK(!second.complete);
    }

    const auto restart_position = repair_dir / "restart-position";
    peer.batch_requests = 0;
    {
        auto store = bench.store(DistributedStoreOptions{restart_position, {}});
        CHECK(store->repair_step(64ULL * 1024 * 1024, 16, live, {}, 3).push_examined == 64);
        CHECK(peer.batch_requests.load() == 4);
    }
    peer.batch_requests = 0;
    auto restarted = bench.store(DistributedStoreOptions{restart_position, {}});
    CHECK(restarted->repair_step(64ULL * 1024 * 1024, 16, live, {}, 3).push_examined == 36);
    CHECK(peer.batch_requests.load() == 3);

    const std::vector<ObjectId> another = live;
    const auto pass_settles = [&](const std::vector<ObjectId>& second) {
        auto store = bench.store();
        REQUIRE(!store->repair_step(64ULL * 1024 * 1024, 16, live).complete);
        for (int step = 0; step < 20; ++step) {
            const auto passes = store->repair_diagnostics().passes_completed;
            const auto result = store->repair_step(64ULL * 1024 * 1024, 16, second);
            if (store->repair_diagnostics().passes_completed > passes)
                return result.complete;
        }
        CHECK(!"no repair pass ended in 20 steps");
        return false;
    };
    CHECK(pass_settles(live));
    CHECK(!pass_settles(another));
}

// Small-object pushes measure round trips and far-end writes, not bandwidth,
// so only a transfer of half an extent or more moves the link estimate.
MACHA_FAST_TEST("rpc_cluster", test_repair_bandwidth_estimate_ignores_small_transfers) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    const auto small = local_objects(bench, 1, 16 * 1024, 71);
    auto store = bench.store();
    (void)store->repair_step(64ULL * 1024 * 1024, 16, small);
    REQUIRE(peer.puts.load() == 1);
    CHECK(store->estimated_network_bps() == 0.0);

    const auto large = local_objects(bench, 1, bench.config().extent_size / 2, 72);
    auto extents = bench.store();
    (void)extents->repair_step(64ULL * 1024 * 1024, 16, large);
    REQUIRE(peer.puts.load() == 2);
    CHECK(extents->estimated_network_bps() > 0.0);
}

// Pull decides "already held" by index lookup, never by reading the extent
// (no DATA admission), and the progress shows in the diagnostics.
MACHA_FAST_TEST("rpc_cluster", test_repair_decides_already_held_without_reading_the_extent) {
    StoreBench bench;
    const auto live = local_objects(bench, 24, 64 * 1024, 200);
    auto store = bench.store();
    const auto before = bench.resources.data.stats().speculative_admissions;
    size_t examined = 0;
    for (int pass = 0; pass < 8 && examined < live.size(); ++pass)
        examined += store->repair_step(8ULL * 1024 * 1024, 16, live).pull_examined;
    CHECK(examined == live.size());
    CHECK(bench.resources.data.stats().speculative_admissions == before);
    CHECK(store->repair_diagnostics().pull_examined == examined);
}

// Repair yields between operations, never inside one: a pull whose turn ends
// while the peer is answering still lands.
MACHA_FAST_TEST("rpc_cluster", test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    const auto bytes = pattern(512 * 1024, 77);
    const auto id = object_id(bytes);
    std::atomic_uint fetches{};
    std::atomic_uint polls_in_flight{};
    std::atomic_bool fetching{};
    bench.node.add_peer(StoreBench::peer(), [&](MessageType type, const Bytes&, FrameType) {
        if (type != MessageType::get_object)
            return RpcMessage{MessageType::ok, {}};
        ++fetches;
        fetching = true;
        // Holds the reply long enough for a poll of the yield predicate.
        (void)wait_until([&] { return polls_in_flight.load() > 0; }, 100ms, 1ms);
        fetching = false;
        return object_reply(bytes);
    });
    auto store = bench.store();
    REQUIRE(store->should_own(id));
    const std::vector<ObjectId> live{id};
    const auto result = store->repair_step(8ULL * 1024 * 1024, 8, live, [&] {
        if (fetching.load())
            ++polls_in_flight;
        return fetches.load() > 0;
    });
    CHECK(fetches.load() == 1);
    CHECK(result.bytes_transferred == bytes.size());
    REQUIRE(bench.local.data().valid(id));
    CHECK(*bench.local.data().get(id) == bytes);
}

// A peer that holds `bytes` and serves them.
void add_serving_peer(StoreBench& bench, const Bytes& bytes) {
    bench.node.add_peer(StoreBench::peer(), [bytes](MessageType type, const Bytes& request, FrameType) {
        if (type == MessageType::get_object)
            return object_reply(bytes);
        if (type == MessageType::have_valid_objects)
            return presence_reply(request, true);
        return RpcMessage{MessageType::ok, {}};
    });
}

// A corrupt local copy (it fails authentication) is never served: the read
// comes from a peer and the fetched bytes replace it, as they do a copy that
// is gone. Unread, scrub finds a corrupt copy and discards it, and repair
// restores it.
MACHA_FAST_TEST("rpc_cluster", test_store_heals_a_bad_local_copy_from_a_peer) {
    StoreBench bench([](Config& c) {
        c.replication = 2;
        c.storage_packing = StoragePackingConfig{0, 0};
    });
    const auto bytes = pattern(256 * 1024, 7);
    const auto id = object_id(bytes);
    add_serving_peer(bench, bytes);
    auto& data = bench.local.data();
    const auto backend = bench.config().storage_backends.front().path;
    REQUIRE(data.put(id, bytes));
    auto store = bench.store();

    corrupt_object(backend, id);
    REQUIRE(!data.get(id).has_value());
    const auto fetched = store->get(id);
    REQUIRE(fetched.has_value());
    CHECK(*fetched == bytes);
    store->wait_local_copies_settled();
    CHECK(data.get(id) == bytes);

    REQUIRE(data.remove(id));
    CHECK(store->get(id) == bytes);
    store->wait_local_copies_settled();
    CHECK(data.get(id) == bytes);

    corrupt_object(backend, id);
    store->scrub_once(128ULL * 1024 * 1024);
    CHECK(!data.has(id));
    const std::vector<ObjectId> live{id};
    for (int step = 0; step < 4 && !data.has(id); ++step)
        store->repair_once(128ULL * 1024 * 1024, live);
    CHECK(data.get(id) == bytes);
}

// A persistent cache enabled at runtime keeps a playback fetch: the bytes land
// in the cache rather than the DATA store, and answer a later read.
MACHA_FAST_TEST("rpc_cluster", test_store_runtime_cache_keeps_a_playback_fetch) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    const auto bytes = pattern(256 * 1024, 11);
    const auto id = object_id(bytes);
    add_serving_peer(bench, bytes);
    auto cached = bench.config();
    cached.cache.path = bench.config().state_path.parent_path() / "cache";
    cached.cache.max_blocks = 8;
    bench.local.reconfigure(cached);
    REQUIRE(bench.local.cache().enabled());

    auto store = bench.store();
    const auto fetched = store->get(id, 0, true);
    REQUIRE(fetched == bytes);
    store->wait_local_copies_settled();
    CHECK(bench.local.cache().has(id));
    CHECK(!bench.local.data().has(id));
    const auto gets = bench.node.calls_of(MessageType::get_object);
    CHECK(store->get(id, 0, true) == bytes);
    CHECK(bench.node.calls_of(MessageType::get_object) == gets);
}

// The control store is not on the DATA device: finding a local control
// object never waits for DATA credit. A namespace node is written by a node
// on its own.
MACHA_FAST_TEST("rpc_cluster", test_store_control_objects_take_no_data_credit_and_commit_alone) {
    StoreBench bench([](Config& c) {
        c.data_inflight_bytes = 2 * c.extent_size;
        c.data_viewer_reserve_bytes = c.extent_size;
        c.maintenance.background_concurrency = 1;
    });
    const auto extent = bench.config().extent_size;
    const auto bytes = pattern(18 * 1024, 91);
    const auto id = object_id(bytes);
    REQUIRE(bench.local.control().put(id, bytes));
    auto store = bench.store();
    auto held = bench.resources.data.acquire(DataWorkContext(FrameType::loader, extent), extent);
    REQUIRE(held.has_value());
    auto found = std::async(std::launch::async, [&] { return store->ensure_control_local(id); });
    const bool prompt = found.wait_for(scaled(2s)) == std::future_status::ready;
    held.reset(); // lets a regressed build finish rather than hang the suite
    CHECK(prompt);
    CHECK(found.get());

    auto nodes = ControlNamespaceNodeStore::for_commit(bench.local.control(), *store);
    const auto node = nodes.put(pattern(512, 17));
    CHECK(bench.local.control().has(node));
    CHECK(nodes.written() == std::vector<ObjectId>{node});
}

// The startup gate fires on silence, never on slow progress: a reading that
// moved restarts the no-progress window; a ceiling bounds the whole startup;
// zero turns either off. Readings come at a quarter of a short window.
MACHA_FAST_TEST("rpc_cluster", test_startup_gate_fires_on_silence_not_on_slow_progress) {
    using Gate = StartupStallGate;
    const auto t0 = Gate::Clock::time_point{} + 1h;
    Gate gate(1200ms, 0ms, t0, 7);
    uint64_t progress = 7;
    for (auto at = 100ms; at <= 3000ms; at += 100ms)
        CHECK(!gate.stalled(t0 + at, ++progress));
    const auto silent_since = t0 + 3000ms;
    CHECK(!gate.stalled(silent_since + 1199ms, progress));
    CHECK(gate.stalled(silent_since + 1200ms, progress));
    CHECK(gate.last_progress_at() == silent_since);
    CHECK(gate.poll_interval() == 300ms);

    Gate ceiling(0ms, 200ms, t0, 0);
    CHECK(!ceiling.stalled(t0 + 199ms, 1));
    CHECK(ceiling.stalled(t0 + 200ms, 2));
    CHECK(ceiling.poll_interval() == 1000ms);

    Gate neither(0ms, 0ms, t0, 0);
    CHECK(!neither.stalled(t0 + 1000h, 0));
    CHECK(Gate(120s, 0ms, t0, 0).poll_interval() == 1000ms);
}

// A metadata view holding one snapshot that counts every read that would go
// to the replicas, and so could wait on a silent peer.
struct SurveyCountingView final : MetadataView {
    MetadataSnapshotView view;
    std::atomic_uint surveys{};
    std::optional<MetadataSnapshotView> current() const override { return view; }
    MetadataSnapshotView converged() override {
        ++surveys;
        return view;
    }
    MetadataSnapshotView converged(const WorkContext&) override { return converged(); }
    uint64_t current_generation() const noexcept override { return view.generation; }
    uint64_t current_namespace_revision() const noexcept override { return 0; }
    MetadataRecord record() override {
        ++surveys;
        return {};
    }
    std::optional<MetadataSnapshotView> release_head() const override { return view; }
    MetadataClusterStatus status() const noexcept override { return {}; }
    MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&, size_t) override {
        throw std::runtime_error("read-only view");
    }
    MetadataRecord mutate_delta(const std::function<void(MetadataSnapshot&, MetadataDelta&)>&,
                                size_t, std::optional<MetadataMutationIdentity>) override {
        throw std::runtime_error("read-only view");
    }
    bool resolve_conflict(const std::string&, std::string_view) override { return false; }
    uint64_t conflicts_superseded() const noexcept override { return 0; }
    uint64_t conflicts_resolved() const noexcept override { return 0; }
    MetadataHeadStanding head_standing() const noexcept override { return {}; }
    std::optional<std::optional<ObjectId>>
    common_ancestor_catalogue_root(const Hash256&, const Hash256&) const override {
        return {};
    }
    MetadataMutationTiming mutation_timing() const noexcept override { return {}; }
    Page<std::pair<std::string, FsEntry>, std::string>
    entries(const MetadataSnapshotView& v, Cursor<std::string> from, Budget& budget) override {
        return namespace_entries(*v.snapshot, nullptr, std::move(from), budget);
    }
};

// The torrent listing (GET /api/v1/torrents/jobs and one job) is served from
// the snapshot this node holds and never reads through the replicas, so a
// silent peer cannot hold an HTTP read (law 1).
MACHA_TEST("rpc_cluster", test_torrent_listing_is_served_from_memory_never_a_survey) {
    TestNode fixture("torrent-listing");
    fixture.start();
    CatalogueHintQueue hints(fixture.config().state_path / "catalogue-hints");
    IngestConfig ingest_config;
    ingest_config.staging_path = fixture.path() / "staging";
    IngestManager ingest(fixture.node(), fixture.filesystem(), hints, ingest_config);
    SubsystemRegistry registry;
    ClusterJobView cluster_jobs(fixture.node(), ingest, registry);

    SurveyCountingView metadata;
    auto snapshot = std::make_shared<MetadataSnapshot>();
    TorrentRequest listed;
    listed.id = std::string(32, 'a');
    listed.info_hash = std::string(40, '3');
    snapshot->torrent_requests[listed.id] = listed;
    metadata.view.generation = 7;
    metadata.view.snapshot = snapshot;
    TorrentCoordinator coordinator(fixture.node(), metadata, registry, cluster_jobs,
                                   fixture.config().state_path);

    const auto requests = coordinator.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().id == listed.id);
    CHECK(coordinator.request(listed.id).has_value());
    CHECK(!coordinator.request("does-not-exist").has_value());
    CHECK(metadata.surveys.load() == 0);
}

// Maintenance paces repair by weight against busier classes and never stops
// it: with a peer reporting viewer traffic throughout, and then with this
// node's loader never going quiet, each missing copy still comes back. Under
// the viewers the pacer is seen turning passes away between repair's turns.
MACHA_TEST("rpc_cluster", test_repair_is_paced_not_stopped_while_higher_classes_stay_busy) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->write_copies = 1;
        config->metadata_write_copies = 1;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 500ms;
        config->maintenance.no_progress_backoff = 500ms;
        config->maintenance.busy_bandwidth_fraction = 0.0;
    }

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));
    REQUIRE(s1.node().wait_local_state_ready(std::chrono::seconds{10}));
    REQUIRE(s2.node().wait_local_state_ready(std::chrono::seconds{10}));

    // An object s1 holds and s2 is claimed for; s2 is told storage changed.
    const auto lose_a_copy = [&](uint8_t salt) {
        const auto bytes = pattern(96 * 1024 + 31, salt);
        const auto id = object_id(bytes);
        REQUIRE(s1.local_state().data().put(id, bytes));
        const RetentionDot claim{s1.node().node_id(), 0xfeed00u + salt};
        s1.local_state().retention().retain(RetentionClass::data, id, claim);
        s2.local_state().retention().retain(RetentionClass::data, id, claim);
        REQUIRE(!s2.local_state().data().valid(id));
        s2.resources().events.notify(NodeEvent::storage);
        return std::pair{id, bytes};
    };

    // Another node, as s2's telemetry sees it, playing throughout.
    {
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
                telemetry.traffic = {TrafficClass{static_cast<uint8_t>(FrameType::foreground), 0,
                                                  0, 1'300'000, 0}};
                s2.node().telemetry().observe(telemetry, true);
                std::this_thread::sleep_for(50ms);
            }
        });
        REQUIRE(wait_until([&] { return s2.node().peer_viewers_active(3000ms); }));
        const auto share_before = s2.repair_diagnostics().gate_share;
        const auto [id, bytes] = lose_a_copy(1);
        const bool restored = wait_until([&] { return s2.local_state().data().valid(id); }, 10s);
        const auto share_after = s2.repair_diagnostics().gate_share;
        // The passes that restored it ran with the peer's viewer active.
        const auto paced_by = s2.repair_diagnostics().paced_by;
        watching = false;
        viewer.join();
        REQUIRE(restored);
        CHECK(*s2.local_state().data().get(id) == bytes);
        CHECK(share_after > share_before);
        CHECK((paced_by & DistributedStore::paced_by_peer_playback));
    }

    // This node's loader, active throughout.
    std::atomic_bool loading{true};
    std::thread loader([&] {
        while (loading.load()) {
            s2.resources().activity.note(FrameType::loader, 64 * 1024);
            std::this_thread::sleep_for(5ms);
        }
    });
    REQUIRE(wait_until([&] {
        return s2.resources().activity.idle_for(FrameType::loader) < c2.maintenance.foreground_quiet;
    }, 5s));
    const auto [id, bytes] = lose_a_copy(2);
    const bool restored = wait_until([&] { return s2.local_state().data().valid(id); }, 10s);
    CHECK(s2.resources().activity.idle_for(FrameType::loader) < c2.maintenance.foreground_quiet);
    CHECK((s2.repair_diagnostics().paced_by & DistributedStore::paced_by_loader));
    loading = false;
    loader.join();
    REQUIRE(restored);
    CHECK(*s2.local_state().data().get(id) == bytes);
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

    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms);
    Endpoint endpoint{"127.0.0.1", port};

    Bytes slow_payload{1};
    Bytes fast_payload{2};
    auto slow = client.call_async(endpoint, MessageType::ping, slow_payload);
    REQUIRE(first_slow_gate.wait_for_entries(1));
    auto fast = client.call_async(endpoint, MessageType::ping, fast_payload);

    REQUIRE(fast.wait_for(scaled(150ms)) == std::future_status::ready);
    auto fast_reply = fast.get();
    CHECK(fast_reply.message.type == MessageType::ok);
    CHECK(fast_reply.message.payload == fast_payload);
    first_slow_gate.open();
    REQUIRE(slow.wait_for(scaled(500ms)) == std::future_status::ready);
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
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
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
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
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
        NetworkLinks links;
        RpcClient client;
        RpcServer server;

        TestNode(ClusterKeys keys, NodeInfo node, RpcServer::Handler handler)
            : info(std::move(node)), client(
                                         links, keys, [this] { return info; },
                                         [](const NodeInfo&) {}, [](uint64_t) {}, 500ms, 10s, 30s),
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
        REQUIRE(slow.wait_for(scaled(1s)) == std::future_status::ready);
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
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
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

    // Liveness probes every heartbeat; a probe round that cannot complete
    // within dead_after ends the route.
    constexpr auto heartbeat = 20ms;
    constexpr auto dead_after = 300ms;
    std::unique_ptr<RpcServer> server;
    // Handlers outlive a failed assertion: they stop waiting once the case
    // ends, and never read the server while it is being destroyed.
    std::atomic_bool ending{false};
    const auto pings = [&] {
        const auto stats = server->work_stats();
        const auto found = stats.message_timings.find(MessageType::ping);
        return found == stats.message_timings.end() ? uint64_t{} : found->second.requests;
    };
    // Each slow handler answers only once the client has run more probe
    // rounds than fit in dead_after (rounds are at least a heartbeat apart,
    // up to two pings a round), so both calls outlive the window by
    // construction, however slowly the host runs.
    const auto outlive_the_window = [&] {
        const auto rounds = static_cast<uint64_t>(dead_after / heartbeat) + 2;
        const auto from = pings();
        const auto until = Clock::now() + dead_after + heartbeat;
        (void)wait_until(
            [&] {
                return ending.load() || (pings() >= from + 2 * rounds && Clock::now() >= until);
            },
            30s, 1ms);
    };
    server = std::make_unique<RpcServer>(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == MessageType::put_object || request.type == MessageType::members)
                outlive_the_window();
            return RpcMessage{MessageType::ok, request.payload};
        },
        [](const NodeInfo&) {});
    server->start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";

    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, heartbeat, dead_after);
    Endpoint endpoint{"127.0.0.1", port};
    // Declared last: runs first, with the client and the server still whole.
    struct End {
        std::atomic_bool& ending;
        RpcClient& client;
        RpcServer& server;
        ~End() {
            ending = true;
            client.stop();
            server.stop();
        }
    } end{ending, client, *server};

    // The 20 ms interval of call() is a stall notice, not a deadline: a slow
    // control call and a slow DATA call on the other lane both complete, and
    // neither ends the other's route.
    Bytes object_payload(256 * 1024, 0x5a);
    auto data = client.call_async(endpoint, MessageType::put_object, object_payload);
    const auto started = Clock::now();
    auto control = client.call(endpoint, MessageType::members, Bytes{1}, 20ms);
    CHECK(Clock::now() - started > dead_after);
    CHECK(control.message.type == MessageType::ok);
    CHECK(control.message.payload == Bytes{1});

    REQUIRE(data.wait_for(scaled(30s)) == std::future_status::ready);
    CHECK(data.get().message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 2);
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
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms);

    Bytes payload(64 * 1024, 0x5a);
    auto request = client.call_async(Endpoint{"127.0.0.1", port}, MessageType::put_object,
                                     payload, FrameType::loader);
    REQUIRE(handler_gate.wait_for_entries(1));
    const auto active = memory.stats();
    CHECK(active.owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)] >= payload.size());
    CHECK(active.used_bytes ==
          active.owner_bytes[static_cast<size_t>(MemoryOwner::rpc_frame)]);

    handler_gate.open();
    REQUIRE(request.wait_for(scaled(2s)) == std::future_status::ready);
    CHECK(request.get().message.type == MessageType::ok);
    REQUIRE(wait_until([&] { return memory.stats().used_bytes == 0; }));
    client.stop();
    server.stop();
}

// The server runs each class of work on its own executor: with every worker
// of one class held, health and membership (fast control) and a playback read
// still answer at once. Each case holds `count` calls of `held` on `frame`
// until `entries` handlers are in, then times the calls that must not wait.
MACHA_TEST("rpc_cluster", test_rpc_held_executors_do_not_delay_other_classes) {
    struct Case {
        const char* name;
        MessageType held;
        FrameType frame;
        size_t count;
        size_t entries;
        RpcMessage held_reply;
        // Calls that must answer promptly: type, frame, expected reply type.
        std::vector<std::tuple<MessageType, FrameType, MessageType>> prompt;
    };
    const std::vector<Case> cases{
        {"every DATA worker on speculative puts", MessageType::put_object, FrameType::speculative,
         8, 6, {MessageType::ok, {}},
         {{MessageType::ping, FrameType::control, MessageType::ok},
          {MessageType::members, FrameType::control, MessageType::members_reply}}},
        {"every DATA worker on retention checks", MessageType::have_objects, FrameType::loader, 8,
         6, {MessageType::have_objects_reply, Bytes{0, 0, 0, 0}},
         {{MessageType::ping, FrameType::control, MessageType::ok},
          {MessageType::members, FrameType::control, MessageType::members_reply}}},
        {"both CONTROL workers on slow handlers", MessageType::have_object, FrameType::control, 2,
         2, {MessageType::bool_reply, Bytes{1}},
         {{MessageType::ping, FrameType::control, MessageType::ok},
          {MessageType::members, FrameType::control, MessageType::members_reply}}},
        {"loader puts on the DATA workers", MessageType::put_object, FrameType::loader, 8, 6,
         {MessageType::ok, {}},
         {{MessageType::get_object, FrameType::foreground, MessageType::ok}}},
    };
    TestCluster cluster;
    const auto& keys = cluster.keys();
    for (const auto& c : cases) {
        const auto port = free_port();
        NodeInfo server_info{random_node_id(), "127.0.0.1", "server-site", port};
        TestGate gate;
        RpcServer server(
            "127.0.0.1", port, keys, server_info,
            [&](const NodeInfo&, FrameType frame_type, const RpcMessage& request) {
                if (request.type == c.held && frame_type == c.frame) {
                    gate.enter_and_wait();
                    return c.held_reply;
                }
                if (request.type == MessageType::members)
                    return RpcMessage{MessageType::members_reply, {}};
                return RpcMessage{MessageType::ok, {}};
            },
            [](const NodeInfo&) {});
        server.start();
        NodeInfo client_info{random_node_id(), "127.0.0.1", "client-site", free_port()};
        NetworkLinks links;
        RpcClient client(
            links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
            [](uint64_t) {}, 500ms, 100ms, 2s);
        const Endpoint endpoint{"127.0.0.1", port};

        std::vector<AsyncRpc> held;
        for (size_t i = 0; i < c.count; ++i)
            held.push_back(client.call_async(endpoint, c.held, Bytes{static_cast<uint8_t>(i)},
                                             c.frame));
        const auto fail = [&](const char* what) {
            std::cerr << c.name << ": " << what << '\n';
            CHECK(false);
        };
        if (!gate.wait_for_entries(c.entries)) {
            fail("the held calls never all reached the handler");
            gate.open();
            continue;
        }
        for (const auto& [type, frame, reply] : c.prompt) {
            const auto started = Clock::now();
            const auto answer = client.call(endpoint, type, {}, frame, 20ms);
            if (answer.message.type != reply || Clock::now() - started >= 150ms)
                fail(message_type_name(type));
        }
        gate.open();
        for (auto& rpc : held) {
            REQUIRE(rpc.wait_for(scaled(2s)) == std::future_status::ready);
            CHECK(rpc.get().message.type == c.held_reply.type);
        }
        client.stop();
        server.stop();
    }
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
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 100ms, 2s);
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
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 100ms, 2s);
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
    REQUIRE(queue_full.wait_for(scaled(1s)) == std::future_status::ready);
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
        REQUIRE(rpc->wait_for(scaled(2s)) == std::future_status::ready);
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
    REQUIRE(too_large.wait_for(scaled(1s)) == std::future_status::ready);
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
        REQUIRE(rejected->wait_for(scaled(1s)) == std::future_status::ready);
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

    NetworkLinks links;
    RpcClient client_a(
        links, keys, [first_client] { return first_client; }, [](const NodeInfo&) {},
        [](uint64_t) {}, 500ms, 100ms, 2s);
    RpcClient client_b(
        links, keys, [second_client] { return second_client; }, [](const NodeInfo&) {},
        [](uint64_t) {}, 500ms, 100ms, 2s);
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
        REQUIRE(rpc->wait_for(scaled(2s)) == std::future_status::ready);
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

// The metadata executor's boundaries: a queued job is removed by cancellation
// without entering the handler; a running job is not cancelled by losing its
// reply route, it finishes and the reply is discarded. A server stopping
// detaches every reply, finishes the job it owns and drops its queue.
MACHA_TEST("rpc_cluster", test_rpc_metadata_executor_cancellation_disconnect_and_shutdown) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto port = free_port();

    NodeInfo server_info{random_node_id(), "127.0.0.1", "server-site", port};
    TestGate before_durability;
    TestGate after_durability;
    TestGate last_job;
    std::atomic_uint32_t handler_calls{};
    std::atomic_uint32_t durable_jobs{};
    RpcServer server(
        "127.0.0.1", port, keys, server_info,
        [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            REQUIRE(request.type == MessageType::put_metadata_commit);
            const auto call = ++handler_calls;
            if (call == 1) {
                before_durability.enter_and_wait();
                ++durable_jobs;
                after_durability.enter_and_wait();
            } else {
                last_job.enter_and_wait();
                ++durable_jobs;
            }
            return RpcMessage{MessageType::bool_reply, Bytes{1}};
        },
        [](const NodeInfo&) {}, 256 * 1024,
        RpcServerExecutionLimits{
            .metadata_workers = 1, .metadata_pending_jobs = 4, .metadata_pending_bytes = 64});
    server.start();
    struct GateOpener {
        std::array<TestGate*, 3> gates;
        ~GateOpener() {
            for (auto* gate : gates)
                gate->open();
        }
    } open_on_exit{{&before_durability, &after_durability, &last_job}};

    NodeInfo client_info{random_node_id(), "127.0.0.1", "client-site", free_port()};
    NetworkLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};
    const auto pending_jobs = [&](size_t jobs) {
        return wait_until([&] { return server.work_stats().metadata_pending_jobs == jobs; }, 2s);
    };

    auto running = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(before_durability.wait_for_entries(1));
    auto queued = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    REQUIRE(pending_jobs(1));
    queued.cancel();
    REQUIRE(pending_jobs(0));
    CHECK(handler_calls.load() == 1);

    before_durability.open();
    REQUIRE(after_durability.wait_for_entries(1));
    CHECK(durable_jobs.load() == 1);
    running.abort();
    after_durability.open();
    REQUIRE(wait_until([&] { return server.work_stats().metadata_active_jobs == 0; }, 2s));
    CHECK(handler_calls.load() == 1);
    CHECK(durable_jobs.load() == 1);

    auto owned = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{3});
    REQUIRE(last_job.wait_for_entries(1));
    auto dropped = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{4});
    REQUIRE(pending_jobs(1));
    auto stopping = std::async(std::launch::async, [&] { server.stop(); });
    REQUIRE(owned.wait_for(scaled(2s)) == std::future_status::ready);
    REQUIRE(dropped.wait_for(scaled(2s)) == std::future_status::ready);
    last_job.open();
    REQUIRE(stopping.wait_for(scaled(2s)) == std::future_status::ready);
    stopping.get();
    CHECK(handler_calls.load() == 2);
    CHECK(durable_jobs.load() == 2);
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
    config.metadata_write_copies = 1;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;

    Service service(config, keys, test_durability_window);
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
    REQUIRE(stopping.wait_for(scaled(2s)) == std::future_status::ready);
    stopping.get();
    CHECK(Clock::now() - started < 2s);
    REQUIRE(pending.wait_for(scaled(1s)) == std::future_status::ready);
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
    NetworkLinks links;
    RpcClient client(
        links, fixture.keys(), [client_info] { return client_info; }, [](const NodeInfo&) {},
        [](uint64_t) {}, 500ms, 100ms, 2s);
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
    REQUIRE(blocked_validation.wait_for(scaled(1s)) == std::future_status::ready);
    CHECK(blocked_validation.get().message.type == MessageType::error);
    auto second_control = client.call(endpoint, MessageType::ping, {}, 500ms);
    CHECK(second_control.message.type == MessageType::ok);
    CHECK(blocked_loader.wait_for(scaled(20ms)) == std::future_status::timeout);

    first.reset();
    REQUIRE(blocked_loader.wait_for(scaled(1s)) == std::future_status::ready);
    CHECK(blocked_loader.get().message.type == MessageType::object_reply);
    CHECK(client.call(endpoint, MessageType::have_object, request.data(), 500ms).message.type ==
          MessageType::bool_reply);

    const auto stats = node.resources.data.stats();
    CHECK(stats.viewer_admissions >= 1);
    CHECK(stats.loader_waits >= 1);
    CHECK(stats.peak_used_bytes == config.data_inflight_bytes);

    client.stop();
}

MACHA_TEST("rpc_cluster", test_joiner_cannot_form_genesis) {
    TestService fixture("joiner");
    auto& config = fixture.config();
    config.bootstrap = {{"127.0.0.1", config.port}};
    config.replication = 1;
    config.metadata_write_copies = 1;

    auto& service = fixture.start();
    // A pristine bootstrap node holds genesis only as local codec material,
    // never advertised as accepted authority.
    CHECK(service.local_state().replica().accepted_heads().empty());
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
    config.metadata_write_copies = 1;
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

    MetadataManager metadata(node, node.local_state(), node.metadata_server());
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

// Nodes configured to seek different numbers of copies form one namespace,
// and a third joins it: each writes and the others read it.
MACHA_TEST("rpc_cluster", test_nodes_seeking_different_copy_counts_share_one_namespace) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto p3 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.metadata_write_copies = 1;
    c2.metadata_write_copies = 2;

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    const auto sees = [](Service& service, const std::string& path) {
        return wait_until([&] {
            try {
                return service.filesystem().getattr(path).type == EntryType::directory;
            } catch (...) {
                return false;
            }
        });
    };
    REQUIRE(retry_while_not_ready(
        [&] { s1.filesystem().mkdir("/from-one", 0755, getuid(), getgid()); }));
    REQUIRE(retry_while_not_ready(
        [&] { s2.filesystem().mkdir("/from-two", 0755, getuid(), getgid()); }));
    CHECK(sees(s2, "/from-one"));
    CHECK(sees(s1, "/from-two"));

    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3,
                         {{"127.0.0.1", p1}, {"127.0.0.1", p2}});
    c3.metadata_write_copies = 3;
    Service s3(c3, keys, test_durability_window);
    s3.start();
    REQUIRE(wait_until([&] { return s1.node().membership().active().size() >= 3; }));
    CHECK(sees(s3, "/from-one"));
    REQUIRE(retry_while_not_ready(
        [&] { s3.filesystem().mkdir("/from-three", 0755, getuid(), getgid()); }));
    CHECK(sees(s1, "/from-three"));
    CHECK(sees(s2, "/from-three"));

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
    c1.write_copies = c2.write_copies = 2;
    c1.metadata_write_copies = c2.metadata_write_copies = 2;

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
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

    MetadataManager m1(s1.node(), s1.local_state(), s1.metadata_server());
    auto snapshot = m1.snapshot();
    CHECK(snapshot.metadata_voters.empty());
    CHECK(snapshot.data_replication == 2);

    auto entry = s1.filesystem().getattr("/media/two-replicas.bin");
    REQUIRE(entry.extents.size() == 1);
    CHECK(s1.local_state().data().has(entry.extents.front().id));
    CHECK(s2.local_state().data().has(entry.extents.front().id));

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
    c1.write_copies = c2.write_copies = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;
    c1.ingest.enabled = c2.ingest.enabled = false;
    c1.torrent.enabled = c2.torrent.enabled = false;

    TestGate repair_gate;
    std::atomic_bool gate_repair{};
    std::atomic_bool gate_once{};
    Service s1(c1, keys, test_durability_window, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repair.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel)) {
            repair_gate.enter_and_wait();
        }
    });
    Service s2(c2, keys, test_durability_window);
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
            return s1.local_state().replica().committed_generation() > 1 &&
                   s1.local_state().replica().committed_generation() ==
                       s2.local_state().replica().committed_generation() &&
                   s1.local_state().replica().committed().hash ==
                       s2.local_state().replica().committed().hash &&
                   s1.local_state().replica().accepted_heads().size() == 1 &&
                   s2.local_state().replica().accepted_heads().size() == 1 &&
                   !d1.scheduled && d1.runs_scheduled == d1.runs_completed && !d2.scheduled &&
                   d2.runs_scheduled == d2.runs_completed;
        },
        10s));

    const auto before = s1.metadata_convergence_diagnostics();
    const auto baseline_generation = s2.local_state().replica().committed_generation();
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
    const auto final_generation = s2.local_state().replica().committed_generation();
    CHECK(final_generation == baseline_generation + burst + 1);
    REQUIRE(
        wait_until([&] { return s1.metadata_server().known_generation() >= final_generation; }, 5s));
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
                   s1.local_state().replica().committed_generation() == final_generation;
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
    c1.write_copies = c2.write_copies = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;
    c1.ingest.enabled = c2.ingest.enabled = false;
    c1.torrent.enabled = c2.torrent.enabled = false;

    TestGate repair_gate1;
    TestGate repair_gate2;
    std::atomic_bool gate_repairs{};
    std::atomic_bool gate_once1{};
    std::atomic_bool gate_once2{};
    Service s1(c1, keys, test_durability_window, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repairs.load(std::memory_order_acquire) &&
            !gate_once1.exchange(true, std::memory_order_acq_rel)) {
            repair_gate1.enter_and_wait();
        }
    });
    Service s2(c2, keys, test_durability_window, {}, [&](std::string_view stage) {
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
                   s1.local_state().replica().committed_generation() > 1 &&
                   s1.local_state().replica().committed().hash ==
                       s2.local_state().replica().committed().hash &&
                   !d1.scheduled && d1.runs_scheduled == d1.runs_completed && !d2.scheduled &&
                   d2.runs_scheduled == d2.runs_completed;
        },
        10s));

    const auto base = s1.local_state().replica().committed();
    auto make_sibling = [&](Service& service, const std::string& path) {
        auto snapshot = decode_snapshot(base.payload);
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        snapshot.entries[path] = entry;
        ++snapshot.mutation_sequences[service.node().node_id()];

        MetadataRecord sibling;
        sibling.generation = base.generation + 1;
        sibling.previous = base.hash;
        sibling.payload = encode_snapshot(snapshot);
        sibling.hash = metadata_hash(sibling.generation, sibling.previous, sibling.payload);
        REQUIRE(service.local_state().replica().store_commit(sibling));
        MetadataAcceptance acceptance;
        acceptance.generation = sibling.generation;
        acceptance.hash = sibling.hash;
        acceptance.required = 1;
        acceptance.replicas = {service.node().node_id()};
        REQUIRE(service.metadata_server().accept_commit(acceptance));
        return sibling;
    };

    gate_repairs.store(true, std::memory_order_release);
    const auto left = make_sibling(s1, "/left-sibling");
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

    const auto right = make_sibling(s2, "/right-sibling");
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
    REQUIRE(s2.metadata_server().accept_commit(duplicate));
    CHECK(s2.node().metadata_announcements() == announcements_before_duplicate);
    std::this_thread::sleep_for(100ms);
    CHECK(convergence_events(s1) == before_duplicate1);
    CHECK(convergence_events(s2) == before_duplicate2);

    repair_gate1.open();
    REQUIRE(wait_until(
        [&] {
            try {
                const auto heads = s1.local_state().replica().accepted_heads();
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
                return s2.local_state().replica().accepted_heads().size() == 1 &&
                       s2.filesystem().getattr("/left-sibling").type == EntryType::directory &&
                       s2.filesystem().getattr("/right-sibling").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        10s));

    // The first write after a reconciliation is a compact delta over the merge commit.
    auto merge_heads = s1.local_state().replica().accepted_heads();
    REQUIRE(merge_heads.size() == 1);
    const auto merge_head = merge_heads.front();
    const auto merge_entry = s1.local_state().replica().history_entry(merge_head.hash);
    REQUIRE(merge_entry.has_value());
    REQUIRE(merge_entry->merge_parents.size() == 1);

    s1.filesystem().mkdir("/after-merge", 0755, getuid(), getgid());
    auto after_heads = s1.local_state().replica().accepted_heads();
    REQUIRE(after_heads.size() == 1);
    const auto after_entry = s1.local_state().replica().history_entry(after_heads.front().hash);
    REQUIRE(after_entry.has_value());
    CHECK(after_entry->previous == merge_head.hash);
    CHECK(after_entry->merge_parents.empty());
    CHECK(after_entry->body == MetadataHistoryEntry::Body::delta);

    s2.stop();
    s1.stop();
}

// A second accepted head from a history this node has never seen needs no
// ancestor in common: the two heads are merged from what they hold, and
// neither's entries are lost.
MACHA_TEST("rpc_cluster", test_a_head_from_an_unknown_history_is_merged) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "unknown-history", cluster.keyfile(), free_port(), {});
    config.metadata_cache = std::chrono::milliseconds(0);
    BareNode node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));

    MetadataManager metadata(node, node.local_state(), node.metadata_server());
    const auto directory = [] {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        return entry;
    };
    metadata.mutate([&](MetadataSnapshot& snapshot) { snapshot.entries["/mine"] = directory(); });
    const auto mine = node.metadata_replica().committed();
    REQUIRE(mine.generation > 1);

    // Another author's namespace, with nothing of this node's in its clock.
    const MetadataDot theirs{random_node_id(), 9};
    auto foreign_snapshot = decode_snapshot(genesis_metadata().payload);
    foreign_snapshot.metadata_write_replicas_required = 1;
    foreign_snapshot.extent_size = decode_snapshot(mine.payload).extent_size;
    foreign_snapshot.data_replication = decode_snapshot(mine.payload).data_replication;
    foreign_snapshot.legacy_clock.emplace();
    foreign_snapshot.mutation_sequences[theirs.author] = theirs.sequence;
    auto made = directory();
    stamp_entry_provenance(made, "/theirs", nullptr, theirs);
    foreign_snapshot.entries["/theirs"] = made;
    MetadataRecord foreign;
    foreign.generation = mine.generation + 7;
    foreign.previous = sha256(pattern(64, 201));
    foreign.payload = encode_snapshot(foreign_snapshot);
    foreign.hash = metadata_hash(foreign.generation, foreign.previous, foreign.payload);
    REQUIRE(node.metadata_replica().store_commit(foreign));
    MetadataAcceptance acceptance;
    acceptance.generation = foreign.generation;
    acceptance.hash = foreign.hash;
    acceptance.required = 1;
    acceptance.replicas = {theirs.author};
    REQUIRE(node.metadata_server().accept_commit(acceptance));
    REQUIRE(node.metadata_replica().accepted_heads().size() == 2);

    const auto merged = metadata.snapshot();
    CHECK(merged.entries.contains("/mine"));
    CHECK(merged.entries.contains("/theirs"));
    CHECK(merged.mutation_sequences.at(theirs.author) == theirs.sequence);
    CHECK(node.metadata_replica().accepted_heads().size() == 1);
    CHECK(metadata.head_standing().set_aside == 0);

    metadata.mutate([&](MetadataSnapshot& snapshot) { snapshot.entries["/more"] = directory(); });
    CHECK(metadata.snapshot().entries.contains("/theirs"));
    CHECK(metadata.snapshot().entries.contains("/more"));
    node.stop();
}

// A second accepted head whose namespace no node present can supply cannot
// be merged. It is set aside: the node goes on reading, writing and
// releasing on its own head, and neither head is dropped.
MACHA_TEST("rpc_cluster", test_a_head_that_cannot_be_merged_is_set_aside) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "set-aside", cluster.keyfile(), free_port(), {});
    config.metadata_cache = std::chrono::milliseconds(0);
    Service service(config, keys, test_durability_window);
    service.start();
    REQUIRE(wait_metadata_writable(service));
    auto& metadata = service.metadata_manager();
    auto& replica = service.local_state().replica();
    service.filesystem().mkdir("/mine", 0755, getuid(), getgid());
    const auto mine = replica.committed();

    // A tree-backed head whose tree nobody here holds.
    auto foreign_snapshot = decode_snapshot(mine.payload);
    foreign_snapshot.entries.clear();
    foreign_snapshot.namespace_root = object_id(pattern(64, 77));
    foreign_snapshot.mutation_sequences.clear();
    foreign_snapshot.mutation_sequences[random_node_id()] = 9;
    MetadataRecord foreign;
    foreign.generation = mine.generation + 7;
    foreign.previous = sha256(pattern(64, 201));
    foreign.payload = encode_snapshot_v14(foreign_snapshot);
    foreign.hash = metadata_hash(foreign.generation, foreign.previous, foreign.payload);
    REQUIRE(replica.store_commit(foreign));
    MetadataAcceptance acceptance;
    acceptance.generation = foreign.generation;
    acceptance.hash = foreign.hash;
    acceptance.required = 1;
    acceptance.replicas = {random_node_id()};
    REQUIRE(service.metadata_server().accept_commit(acceptance));

    // Reads serve this node's own head.
    REQUIRE(wait_until([&] { return metadata.head_standing().set_aside == 1; }, 10s));
    CHECK(service.filesystem().getattr("/mine").type == EntryType::directory);

    // Writes extend it.
    service.filesystem().mkdir("/more", 0755, getuid(), getgid());
    CHECK(service.filesystem().getattr("/more").type == EntryType::directory);
    const auto after = metadata.read_record();
    CHECK(after.generation < foreign.generation);

    // Release has a head to follow, and the other head is still held.
    const auto release = metadata.retention_release_view();
    REQUIRE(release.has_value());
    CHECK(release->hash == after.hash);
    const auto heads = replica.accepted_heads();
    CHECK(heads.size() == 2);
    CHECK(std::any_of(heads.begin(), heads.end(),
                      [&](const MetadataRecord& head) { return head.hash == foreign.hash; }));
    service.stop();
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
    c1.write_copies = c2.write_copies = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
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

    MetadataManager metadata1(n1, n1.local_state(), n1.metadata_server());
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
    auto make_sibling = [&](BareNode& node, const std::string& path) {
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
        REQUIRE(node.metadata_server().accept_commit(acceptance));
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

// Service startup waits on local state while recovery reports progress, and
// once progress stops for service_startup_no_progress gives up through its
// stall handler with a readiness diagnostic. The gate's arithmetic is
// test_startup_gate_fires_on_silence_not_on_slow_progress; this is its wiring.
MACHA_TEST("rpc_cluster", test_service_startup_gate_spares_progress_and_ends_a_stall) {
    TestCluster cluster;
    auto c1 = config_for(cluster.path() / "stalled-startup", cluster.keyfile(), free_port());
    c1.service_startup_timeout = 0ms;
    c1.service_startup_no_progress = 400ms;
    c1.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = false;
    c1.ingest.enabled = false;
    c1.torrent.enabled = false;

    // Stall local-state readiness; the injected handler observes the stall
    // without terminating the process.
    TestGate stall_gate;
    std::atomic_bool handler_called{false};
    std::string diagnostic;
    Service service(
        c1, cluster.keys(), test_durability_window,
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
        ~ReleaseGate() { gate.open(); }
    } release{stall_gate};

    service.start();
    REQUIRE(stall_gate.wait_for_entries(1, 5s));

    // Recovery ticks for three gate windows: the gate holds.
    std::atomic_bool ticking{true};
    std::atomic_uint ticks{};
    std::thread ticker([&] {
        while (ticking.load(std::memory_order_acquire)) {
            note_startup_progress();
            ++ticks;
            std::this_thread::sleep_for(50ms);
        }
    });
    std::atomic_bool threw{};
    std::thread waiter([&] {
        try {
            (void)service.filesystem();
        } catch (const std::exception&) {
            threw = true;
        }
    });
    (void)wait_until([&] { return ticks.load() >= 24 || handler_called.load(); }, 30s);
    CHECK(!handler_called.load(std::memory_order_acquire));

    // Silence: the gate fires within a little over the window.
    ticking.store(false, std::memory_order_release);
    ticker.join();
    const auto silent_since = Clock::now();
    REQUIRE(wait_until([&] { return handler_called.load(std::memory_order_acquire); }, 10s));
    CHECK(Clock::now() - silent_since >= 350ms);
    waiter.join();
    CHECK(threw.load());
    CHECK(diagnostic.find("data_storage=recovering") != std::string::npos);

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
        config->write_copies = 1;
        config->metadata_write_copies = 2;
        config->heartbeat = 50ms;
        // Well above scheduler jitter, yet short enough for the 5 s wait below
        // to see s3 drop out.
        config->dead_after = 2s;
        config->catalogue.scanner.enabled = false;
        config->catalogue.api.enabled = false;
        config->ingest.enabled = false;
        config->torrent.enabled = false;
    }

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    auto s3 = std::make_unique<Service>(c3, keys, test_durability_window);
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

    REQUIRE(retry_while_not_ready(
        [&] { s1.filesystem().mkdir("/lagging-base", 0755, getuid(), getgid()); }));
    REQUIRE(wait_until(
        [&] {
            try {
                return s3->filesystem().getattr("/lagging-base").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        10s));
    const auto base = s3->local_state().replica().committed();

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
    const auto final = s1.local_state().replica().committed();
    CHECK(final.generation == base.generation + burst);
    REQUIRE(wait_until([&] { return s2.local_state().replica().committed().hash == final.hash; },
                       10s));

    s3 = std::make_unique<Service>(c3, keys, test_durability_window);
    s3->start();
    (void)s3->filesystem();
    REQUIRE(wait_until(
        [&] {
            try {
                const auto diagnostics = s3->metadata_convergence_diagnostics();
                return s3->local_state().replica().committed().hash == final.hash &&
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
    const auto heads = s3->local_state().replica().accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == final.hash);
    CHECK(s3->local_state().replica().history_contains(final.hash));
    CHECK(s3->local_state().replica().history_is_ancestor(base.hash, final.hash));

    const auto transfer1 = s1.metadata_manager().history_transfer_diagnostics();
    const auto transfer2 = s2.metadata_manager().history_transfer_diagnostics();
    CHECK(std::max(transfer1.peak_in_flight, transfer2.peak_in_flight) > 1);
    CHECK(std::max(transfer1.peak_in_flight, transfer2.peak_in_flight) <= 8);

    s3->stop();
    s2.stop();
    s1.stop();
}

MACHA_HEAVY_TEST("rpc_cluster", test_an_ingest_on_a_node_with_no_namespace_yet_resumes_when_it_has_one) {
    // A node still waiting for its bootstrap peer has no namespace to write:
    // an ingest there blocks with metadata_unavailable, is retried, and
    // completes once the peer has arrived and the namespace has formed.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->write_copies = 1;
        config->metadata_write_copies = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
        config->catalogue.scanner.enabled = false;
        config->ingest.enabled = false;
    }
    Service s1(c1, keys, test_durability_window);
    std::unique_ptr<Service> s2;
    s1.start();

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

    s2 = std::make_unique<Service>(c2, keys, test_durability_window);
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

// A metadata-only change to a file commits while one of its holders is away.
MACHA_HEAVY_TEST("rpc_cluster", test_a_file_touch_commits_while_a_holder_is_away) {
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
        config->write_copies = 3;
        config->metadata_write_copies = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
    }

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    auto s3 = std::make_unique<Service>(c3, keys, test_durability_window);
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

    REQUIRE(retry_while_not_ready(
        [&] { s1.filesystem().create_file("/retained.bin", 0644, getuid(), getgid()); }));
    auto input = pattern(128 * 1024);
    auto writer = s1.filesystem().open_write("/retained.bin", true);
    REQUIRE(writer->write(0, input) == input.size());
    writer->commit();
    const auto entry = s1.filesystem().getattr("/retained.bin");
    REQUIRE(entry.extents.size() == 1);
    const auto extent = entry.extents.front().id;
    REQUIRE(wait_until([&] {
        return s1.local_state().data().valid(extent) && s2.local_state().data().valid(extent) &&
               s3->local_state().data().valid(extent);
    }));
    // The accepted file reference has already installed a DATA claim.
    CHECK(s1.local_state().retention().retained(RetentionClass::data, extent));
    CHECK(s2.local_state().retention().retained(RetentionClass::data, extent));
    CHECK(s3->local_state().retention().retained(RetentionClass::data, extent));

    const auto before = s1.local_state().replica().committed();
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));

    // A holder is away: the touch still commits, with a fresh claim on the
    // holders present.
    s1.filesystem().chmod("/retained.bin", 0600);
    CHECK((s1.filesystem().getattr("/retained.bin").mode & 0777U) == 0600U);
    CHECK(s1.local_state().replica().committed().hash != before.hash);
    const auto dot = [&](Service& service) {
        const auto claims = service.local_state().retention().claims(RetentionClass::data, extent);
        const auto found = claims.adds.find(s1.node().node_id());
        return found == claims.adds.end() ? uint64_t{0} : found->second;
    };
    const auto touched = dot(s1);
    CHECK(touched > 0);
    CHECK(dot(s2) == touched);

    // The returning holder learns the change from the head.
    s3 = std::make_unique<Service>(c3, keys, test_durability_window);
    s3->start();
    REQUIRE(wait_until([&] {
        try {
            return (s3->filesystem().getattr("/retained.bin").mode & 0777U) == 0600U;
        } catch (...) {
            return false;
        }
    }, 30s));

    s3->stop();
    s2.stop();
    s1.stop();
}

MACHA_FAST_TEST("rpc_cluster", test_commit_replicas_ordered_local_then_nearest) {
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

// A delete made while a node is away is reclaimed by the nodes present
// without waiting for it, and by the absent node once it is back.
MACHA_TEST("rpc_cluster", test_a_delete_is_reclaimed_while_a_node_is_away) {
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
        config->write_copies = 2;
        config->metadata_write_copies = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
        config->maintenance.garbage_grace = 0ms;
    }

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    auto s3 = std::make_unique<Service>(c3, keys, test_durability_window);
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

    // With node 3 away the others delete the file, release their claims and
    // reclaim their copies on their own clocks.
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));
    s1.filesystem().unlink("/partition-retain.bin");
    REQUIRE(wait_until(
        [&] {
            return !s1.local_state().retention().retained(RetentionClass::data, extent) &&
                   !s2.local_state().retention().retained(RetentionClass::data, extent) &&
                   !s1.local_state().data().valid(extent) && !s2.local_state().data().valid(extent);
        },
        20s));

    // Node 3 returns still holding what it held, learns of the delete from
    // the head and reclaims its own copy.
    s3 = std::make_unique<Service>(c3, keys, test_durability_window);
    s3->start();
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
            return !s3->local_state().retention().retained(RetentionClass::data, extent) &&
                   !s3->local_state().data().valid(extent);
        },
        20s));

    s3->stop();
    s2.stop();
    s1.stop();
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
        config->write_copies = 1;
        config->metadata_write_copies = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
    }

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    auto s3 = std::make_unique<Service>(c3, keys, test_durability_window);
    s1.start();
    s2.start();
    s3->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 3 &&
               s2.node().membership().active().size() == 3 &&
               s3->node().membership().active().size() == 3;
    }));
    // An accepted branch first, so maintenance runs against valid metadata.
    REQUIRE(retry_while_not_ready([&] { s1.filesystem().mkdir("/base", 0755, getuid(), getgid()); }));
    REQUIRE(wait_until([&] {
        try {
            return s2.filesystem().getattr("/base").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));

    const auto bytes = pattern(96 * 1024 + 13);
    const auto id = object_id(bytes);
    REQUIRE(s1.local_state().data().put(id, bytes));
    REQUIRE(s2.local_state().data().put(id, bytes));
    const RetentionDot claim{s1.node().node_id(), 0xf00d};
    s1.local_state().retention().retain(RetentionClass::data, id, claim);
    s2.local_state().retention().retain(RetentionClass::data, id, claim);

    // With a replica offline, install a claim dot the branch clock does not
    // dominate: unreachable from the namespace, it must still not be erased.
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));

    REQUIRE(s2.local_state().data().remove(id));
    CHECK(s2.local_state().retention().retained(RetentionClass::data, id));
    CHECK(!s2.local_state().data().valid(id));
    // A detector outside the storage wrappers must publish the mutation event;
    // heartbeats do not trigger maintenance.
    s2.resources().events.notify(NodeEvent::storage);

    // `id` is unreachable from namespace and catalogue; only its retention
    // claim tells maintenance to restore it.
    REQUIRE(wait_until([&] { return s2.local_state().data().valid(id); }, 5s));
    auto restored = s2.local_state().data().get(id);
    REQUIRE(restored.has_value());
    CHECK(*restored == bytes);
    CHECK(s2.local_state().retention().retained(RetentionClass::data, id));

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
        config->write_copies = 1;
        config->metadata_write_copies = 1;
        config->maintenance.interval = 50ms;
        config->maintenance.foreground_quiet = 10ms;
        config->maintenance.no_progress_backoff = 500ms;
        config->maintenance.idle_bandwidth_fraction = 1.0;
        config->maintenance.cpu_target = 1.0;
        config->maintenance.max_bandwidth = config->extent_size;
    }

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
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
        REQUIRE(s2.local_state().data().put(held, held_bytes));
        s2.local_state().retention().retain(RetentionClass::data, held, claim);
    }
    const auto bytes = pattern(96 * 1024 + 7);
    const auto id = object_id(bytes);
    REQUIRE(s1.local_state().data().put(id, bytes));
    s2.local_state().retention().retain(RetentionClass::data, id, claim);
    REQUIRE(!s2.local_state().data().valid(id));
    s2.resources().events.notify(NodeEvent::storage);

    REQUIRE(wait_until([&] { return s2.local_state().data().valid(id); }, 5s));
    CHECK(*s2.local_state().data().get(id) == bytes);
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
        config->write_copies = 1;
        config->metadata_write_copies = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
    }

    Hash256 left_head{};
    NodeId node2_id{};
    {
        Service s1(c1, keys, test_durability_window);
        Service s2(c2, keys, test_durability_window);
        Service s3(c3, keys, test_durability_window);
        Service s4(c4, keys, test_durability_window);
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
        REQUIRE(retry_while_not_ready(
            [&] { s1.filesystem().mkdir("/base", 0755, getuid(), getgid()); }));
        MetadataManager initial_repair(s1.node(), s1.local_state(), s1.metadata_server());
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
        left_head = s2.local_state().replica().committed().hash;
        node2_id = s2.node().node_id();
        CHECK(s2.local_state().replica().acceptance(left_head).has_value());

        s2.stop();
        s1.stop();
    }

    Hash256 right_head{};
    {
        // Nodes 3 and 4 never saw /left but satisfy the floor, so stay writable.
        Service s3(c3, keys, test_durability_window);
        Service s4(c4, keys, test_durability_window);
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
        right_head = s3.local_state().replica().committed().hash;
        REQUIRE(right_head != left_head);
        CHECK(s3.local_state().replica().acceptance(right_head).has_value());

        // With one member of the other pair back, two accepted sibling histories
        // meet; reconciliation must descend from both.
        s4.stop();
        Service s2(c2, keys, test_durability_window);
        s2.start();
        REQUIRE(s2.node().node_id() == node2_id);
        REQUIRE(wait_until([&] {
            auto active2 = s2.node().membership().active();
            auto active3 = s3.node().membership().active();
            return active2.size() == 2 && active3.size() == 2;
        }));

        MetadataManager reconcile(s2.node(), s2.local_state(), s2.metadata_server());
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
            return s2.local_state().replica().accepted_heads().size() == 1 &&
                   s3.local_state().replica().accepted_heads().size() == 1;
        }));
        const auto merged = s2.local_state().replica().accepted_heads().front();
        CHECK(s2.local_state().replica().history_is_ancestor(left_head, merged.hash));
        CHECK(s2.local_state().replica().history_is_ancestor(right_head, merged.hash));
        const auto local_history = s2.local_state().replica().history_entry(merged.hash);
        const auto remote_history = s3.local_state().replica().history_entry(merged.hash);
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
    c1.metadata_write_copies = c2.metadata_write_copies = 1;

    ObjectId object;
    Bytes input = pattern(128 * 1024);
    {
        Service s1(c1, keys, test_durability_window);
        Service s2(c2, keys, test_durability_window);
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
    c1.metadata_write_copies = c2.metadata_write_copies = 2;
    {
        Service s1(c1, keys, test_durability_window);
        Service s2(c2, keys, test_durability_window);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        REQUIRE(wait_metadata_writable(s1));
        s1.filesystem().mkdir("/after-grow", 0755, getuid(), getgid());
        MetadataManager m1(s1.node(), s1.local_state(), s1.metadata_server());
        auto snapshot = m1.snapshot();
        CHECK(snapshot.metadata_voters.empty());
        CHECK(snapshot.data_replication == 2);
        CHECK(s2.filesystem().getattr("/after-grow").type == EntryType::directory);

        DistributedStore r1(s1.node(), s1.local_state(), s1.resources().activity, s1.resources().data, s1.resources().memory, s1.resources().events);
        DistributedStore r2(s2.node(), s2.local_state(), s2.resources().activity, s2.resources().data, s2.resources().memory, s2.resources().events);
        REQUIRE(wait_until([&] {
            r1.repair_once(1024 * 1024);
            r2.repair_once(1024 * 1024);
            return s1.local_state().data().has(object) && s2.local_state().data().has(object);
        }));

        s2.stop();
        s1.stop();
    }

    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    {
        Service s1(c1, keys, test_durability_window);
        Service s2(c2, keys, test_durability_window);
        s1.start();
        s2.start();
        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 2 &&
                   s2.node().membership().active().size() >= 2;
        }));

        REQUIRE(retry_while_not_ready(
            [&] { s2.filesystem().mkdir("/after-shrink", 0755, getuid(), getgid()); }));
        MetadataManager m2(s2.node(), s2.local_state(), s2.metadata_server());
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

MACHA_HEAVY_TEST("rpc_cluster", test_replacement_node_recovers_namespace_and_replication) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    uint16_t p1 = free_port();
    uint16_t p2 = free_port();

    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;

    std::vector<ObjectId> objects;
    Bytes input = pattern(2 * 1024 * 1024 + 12345);
    NodeId old_n1;

    auto s1 = std::make_unique<Service>(c1, keys, test_durability_window);
    auto s2 = std::make_unique<Service>(c2, keys, test_durability_window);
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
    DistributedStore initial_convergence(s2->node(), s2->local_state(), s2->resources().activity,
                           s2->resources().data, s2->resources().memory, s2->resources().events);
    REQUIRE(wait_until([&] {
        initial_convergence.repair_once(16ULL * 1024 * 1024, objects);
        return std::all_of(objects.begin(), objects.end(),
                           [&](const auto& id) { return s2->local_state().data().has(id); });
    }));

    // One metadata pass makes node 2 a durable checkpoint witness first.
    MetadataManager witness_repair(s2->node(), s2->local_state(), s2->metadata_server());
    witness_repair.repair_once();
    CHECK(s2->local_state().replica().committed().generation > 1);

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
    auto replacement = std::make_unique<Service>(replacement_config, keys, test_durability_window);
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
    DistributedStore replacement_convergence(replacement->node(), replacement->local_state(), replacement->resources().activity,
                           replacement->resources().data, replacement->resources().memory, replacement->resources().events);
    REQUIRE(wait_until(
        [&] {
            replacement_convergence.repair_once(16ULL * 1024 * 1024, objects);
            return std::all_of(objects.begin(), objects.end(), [&](const auto& id) {
                return replacement->local_state().data().has(id);
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
            config.write_copies = data_floor;
            config.metadata_write_copies = metadata_floor;
            config.metadata_cache = 5000ms; // the longest allowed
            configs.push_back(std::move(config));
        }
        for (const auto& config : configs)
            nodes.push_back(
                std::make_unique<Service>(config, cluster.keys(), test_durability_window));
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

// Any node may found a virgin namespace (here the highest NodeId), and its
// root is on every replica when the founding call returns. A node whose view
// is warm sees another node's later commit as soon as it commits. A file
// written through one node reads back whole through a second and at an
// arbitrary offset through a third, as soon as the write commits.
MACHA_HEAVY_TEST("rpc_cluster", test_a_durable_trio_founds_commits_and_reads_through_every_node) {
    DurableTrio trio;
    size_t founder = 0;
    for (size_t i = 1; i < 3; ++i)
        if (trio[i].node().node_id() > trio[founder].node().node_id())
            founder = i;
    trio[founder].filesystem().mkdir("/media", 0755, getuid(), getgid());
    for (size_t i = 0; i < 3; ++i)
        CHECK(is_directory(trio[i], "/media")); // and warms every view

    const auto writer = (founder + 1) % 3;
    trio[writer].filesystem().mkdir("/media/new", 0755, getuid(), getgid());
    for (size_t i = 0; i < 3; ++i)
        CHECK(is_directory(trio[i], "/media/new"));

    const auto input = pattern(3 * 1024 * 1024 + 12345);
    (void)trio.write(0, "/media/movie.mkv", input);
    CHECK(read_whole(trio[1], "/media/movie.mkv", input.size()) == input);
    Bytes slice(333333);
    auto reader = trio[2].filesystem().open_read("/media/movie.mkv");
    REQUIRE(reader->read(987654, slice) == slice.size());
    CHECK(std::equal(slice.begin(), slice.end(), input.begin() + 987654));
}

// Three nodes seeking two copies: losing one leaves the other two
// committing, with the change on both when the call returns; a lone survivor
// goes on committing.
MACHA_HEAVY_TEST("rpc_cluster", test_metadata_commits_with_whichever_nodes_remain) {
    DurableTrio trio(2, 1);
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    trio.stop(2);
    trio[1].filesystem().mkdir("/after-one-loss", 0755, getuid(), getgid());
    CHECK(is_directory(trio[0], "/after-one-loss"));
    trio.stop(1);
    trio[0].filesystem().mkdir("/alone", 0755, getuid(), getgid());
    CHECK(is_directory(trio[0], "/alone"));
}

// Unlinking retires the object to garbage. The cached inventory is one shared,
// sorted, duplicate-free snapshot until the namespace changes.
MACHA_TEST("rpc_cluster", test_unlink_retires_an_object_from_the_maintenance_inventory) {
    TestService fixture("inventory");
    fixture.config().replication = 1;
    fixture.config().metadata_write_copies = 1;
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
    fixture.config().metadata_write_copies = 1;
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
        config.write_copies = 2;
        config.metadata_write_copies = 2;
        return config;
    };
    Service n1(node_config(0), cluster.keys(), test_durability_window);
    Service n2(node_config(1), cluster.keys(), test_durability_window);
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
    Service n3(node_config(2), cluster.keys(), test_durability_window,
               NodeRuntime::StartupStageHook{}, Service::MaintenanceStageHook{},
               Service::StartupStallHandler{}, instruments);
    n3.start();
    REQUIRE(wait_until([&] { return n3.node().membership().active().size() >= 3; }, 60s));
    (void)n3.filesystem(); // services ready

    const auto holds_all = [&] {
        return std::all_of(ids.begin(), ids.end(),
                           [&](const ObjectId& id) { return n3.local_state().data().has(id); });
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

// RpcLinks over the real network, with the faults these tests inject: a peer
// that never answers, sessions that have not reached this node yet, a call
// whose route is retired before it is placed, and a lane that dies underneath.
class FaultyLinks final : public RpcLinks {
    struct Stall {
        std::optional<MessageType> message;
        std::optional<TransportLane> lane;
    };
    struct Dialled {
        size_t count{};
        // Duplicates of the dialled sockets: sever() reaches the socket even
        // after the transport has closed its own descriptor, never a reused one.
        std::vector<int> sockets;
    };

    NetworkLinks network_;
    mutable Mutex mutex_;
    std::map<NodeId, Stall> stalled_ MACHA_GUARDED_BY(mutex_);
    std::vector<std::shared_ptr<std::promise<RpcReply>>> stalled_calls_ MACHA_GUARDED_BY(mutex_);
    std::set<NodeId> held_ MACHA_GUARDED_BY(mutex_);
    std::vector<std::pair<NodeId, std::function<void()>>> held_installs_ MACHA_GUARDED_BY(mutex_);
    std::set<NodeId> retire_before_send_ MACHA_GUARDED_BY(mutex_);
    size_t retirement_waits_ MACHA_GUARDED_BY(mutex_){};
    bool sent_after_retirement_ MACHA_GUARDED_BY(mutex_){};
    std::map<std::pair<std::string, TransportLane>, Dialled> dialled_ MACHA_GUARDED_BY(mutex_);

    AsyncRpc stalled_call_locked() MACHA_REQUIRES(mutex_) {
        auto promise = std::make_shared<std::promise<RpcReply>>();
        auto future = promise->get_future();
        stalled_calls_.push_back(promise);
        const auto started = Clock::now();
        std::weak_ptr<std::promise<RpcReply>> weak = promise;
        auto fail = [weak] {
            if (auto held = weak.lock()) {
                try {
                    held->set_exception(std::make_exception_ptr(
                        std::runtime_error("RPC cancelled: peer is stalled by FaultyLinks")));
                } catch (const std::future_error&) {
                    // Already released or cancelled.
                }
            }
        };
        return AsyncRpc(std::move(future), fail, fail, {}, [started] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        });
    }

    std::map<NodeId, std::pair<MessageType, std::chrono::milliseconds>> slow_ MACHA_GUARDED_BY(mutex_);
    size_t slow_sends_ MACHA_GUARDED_BY(mutex_){};
    std::map<std::pair<NodeId, TransportLane>, size_t> admitted_ MACHA_GUARDED_BY(mutex_);
  public:
    ~FaultyLinks() override {
        Lock lock(mutex_);
        for (auto& [_, dialled] : dialled_)
            for (int socket : dialled.sockets)
                ::close(socket);
    }

    int connect(const Endpoint& endpoint, TransportLane lane,
                std::chrono::milliseconds timeout) override {
        const int fd = network_.connect(endpoint, lane, timeout);
        const int kept = ::dup(fd);
        Lock lock(mutex_);
        auto& dialled = dialled_[{endpoint_identity_key(endpoint), lane}];
        ++dialled.count;
        if (kept >= 0)
            dialled.sockets.push_back(kept);
        return fd;
    }

    std::optional<AsyncRpc> send(const NodeId& peer, TransportLane lane, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type,
                                 RpcRoute& route) override {
        bool retire_first = false;
        {
            Lock lock(mutex_);
            if (auto found = stalled_.find(peer);
                found != stalled_.end() &&
                (!found->second.message || *found->second.message == type) &&
                (!found->second.lane || *found->second.lane == lane))
                return stalled_call_locked();
            retire_first = held_.contains(peer) && retire_before_send_.contains(peer);
            if (retire_first)
                ++retirement_waits_;
        }
        if (retire_first) {
            const bool retired = wait_until([&] { return !route.usable(); }, 5s, 1ms);
            Lock lock(mutex_);
            sent_after_retirement_ = sent_after_retirement_ || retired;
        }
        std::optional<std::chrono::milliseconds> hold;
        {
            Lock lock(mutex_);
            if (auto found = slow_.find(peer); found != slow_.end() && found->second.first == type) {
                hold = found->second.second;
                slow_.erase(found);
            }
        }
        auto placed = network_.send(peer, lane, type, payload, frame_type, route);
        if (hold) {
            std::this_thread::sleep_for(*hold);
            Lock lock(mutex_);
            ++slow_sends_;
        }
        return placed;
    }

    // The next `message` sent to `peer` is placed on its route at once, and
    // its sender held for `hold` before getting the call back: a sender that
    // runs late while the peer answers on time.
    void slow_send(const NodeId& peer, MessageType message, std::chrono::milliseconds hold) {
        Lock lock(mutex_);
        slow_[peer] = {message, hold};
    }
    size_t slow_sends() const {
        Lock lock(mutex_);
        return slow_sends_;
    }

    void admit(const NodeInfo& peer, TransportLane lane, std::function<void()> install) override {
        {
            Lock lock(mutex_);
            if (held_.contains(peer.id)) {
                held_installs_.emplace_back(peer.id, [this, id = peer.id, lane,
                                                      install = std::move(install)] {
                    install();
                    Lock counted(mutex_);
                    ++admitted_[{id, lane}];
                });
                return;
            }
        }
        network_.admit(peer, lane, std::move(install));
        Lock lock(mutex_);
        ++admitted_[{peer.id, lane}];
    }
    // Sessions `peer` opened on `lane` that are installed as routes here.
    size_t admitted(const NodeId& peer, TransportLane lane) const {
        Lock lock(mutex_);
        const auto found = admitted_.find({peer, lane});
        return found == admitted_.end() ? 0 : found->second;
    }

    // Calls to `peer` (every one, or only `message`, or only on `lane`) are
    // held unanswered, with idle_for() advancing as for a dead link.
    void stall(const NodeId& peer, std::optional<MessageType> message = {},
               std::optional<TransportLane> lane = {}) {
        Lock lock(mutex_);
        stalled_[peer] = {message, lane};
    }
    // Ends the stall on `peer` and fails every held call with a transport
    // error, as a timed-out link would.
    void release(const NodeId& peer) {
        std::vector<std::shared_ptr<std::promise<RpcReply>>> held;
        {
            Lock lock(mutex_);
            stalled_.erase(peer);
            held.swap(stalled_calls_);
        }
        for (auto& promise : held) {
            try {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("RPC failed: FaultyLinks released the stalled peer")));
            } catch (const std::future_error&) {
            }
        }
    }
    // Calls held since the last release().
    size_t stalled_calls() const {
        Lock lock(mutex_);
        return stalled_calls_.size();
    }

    // Sessions `peer` opens authenticate and serve requests but are not
    // installed as routes until release_sessions(), as if they had not
    // reached this node yet.
    void hold_sessions(const NodeId& peer) {
        Lock lock(mutex_);
        held_.insert(peer);
    }
    void release_sessions(const NodeId& peer) {
        std::vector<std::function<void()>> installs;
        {
            Lock lock(mutex_);
            held_.erase(peer);
            for (auto it = held_installs_.begin(); it != held_installs_.end();) {
                if (it->first == peer) {
                    installs.push_back(std::move(it->second));
                    it = held_installs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& install : installs)
            install();
    }
    // While `peer`'s sessions are held, a send to it first waits (bounded)
    // for its route to be retired: a call that loses its dial before it is
    // placed on it.
    void retire_before_send(const NodeId& peer) {
        Lock lock(mutex_);
        retire_before_send_.insert(peer);
    }
    // Sends that have waited for retirement, and whether one saw it.
    size_t retirement_waits() const {
        Lock lock(mutex_);
        return retirement_waits_;
    }
    bool sent_after_retirement() const {
        Lock lock(mutex_);
        return sent_after_retirement_;
    }

    size_t dials(const Endpoint& endpoint, TransportLane lane) const {
        Lock lock(mutex_);
        auto found = dialled_.find({endpoint_identity_key(endpoint), lane});
        return found == dialled_.end() ? 0 : found->second.count;
    }
    // Shuts down every socket dialled to `endpoint` for `lane`, both
    // directions, as a NAT mapping expiring under it would end it.
    void sever(const Endpoint& endpoint, TransportLane lane) {
        Lock lock(mutex_);
        auto found = dialled_.find({endpoint_identity_key(endpoint), lane});
        if (found == dialled_.end())
            return;
        for (int socket : found->second.sockets) {
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
        }
        found->second.sockets.clear();
    }
};

MACHA_TEST("rpc_cluster", test_a_ping_answered_while_the_health_pass_ran_late_keeps_its_route) {
    // The health pass is held past the peer-death window with its ping
    // already placed and answered. An answered ping is proof of life however
    // late it is read: the route stays, and nothing is redialled.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto port = free_port();
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
    const auto pings = [&] {
        const auto stats = server.work_stats();
        const auto found = stats.message_timings.find(MessageType::ping);
        return found == stats.message_timings.end() ? uint64_t{} : found->second.requests;
    };

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    constexpr auto dead_after = 80ms;
    FaultyLinks links;
    RpcClient client(
        links, keys, [client_info] { return client_info; }, [](const NodeInfo&) {}, [](uint64_t) {},
        500ms, 20ms, dead_after);
    const Endpoint endpoint{"127.0.0.1", port};
    CHECK(client.call(endpoint, MessageType::members, Bytes{1}, 1s).message.payload == Bytes{1});
    REQUIRE(client.stats().connections_created == 1);

    links.slow_send(server_info.id, MessageType::ping, dead_after * 3);
    REQUIRE(wait_until([&] { return links.slow_sends() == 1; }, 5s));
    // The pass reads the late ping and goes on to place more.
    const auto answered = pings();
    REQUIRE(wait_until([&] { return pings() >= answered + 2; }, 5s));
    CHECK(client.has_route(server_info.id, TransportLane::control));
    CHECK(client.call(endpoint, MessageType::members, Bytes{2}, 1s).message.payload == Bytes{2});
    CHECK(client.stats().connections_created == 1);

    client.stop();
    server.stop();
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
    c1.metadata_write_copies = c2.metadata_write_copies = 1;

    auto links = std::make_shared<FaultyLinks>();
    ServiceInstruments instruments;
    instruments.links = links;
    Service s1(c1, keys, test_durability_window, {}, {}, {}, instruments);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    // Past the virgin generation, so repair takes the read_group()/fan-out path.
    REQUIRE(
        retry_while_not_ready([&] { s1.filesystem().mkdir("/warm", 0755, getuid(), getgid()); }));

    const auto peer = s2.node().node_id();
    links->stall(peer, MessageType::has_metadata_history_entry);
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
    REQUIRE(wait_until([&] { return links->stalled_calls() >= 1; }, 15s));
    CHECK(!repair_done.load());

    const auto started = std::chrono::steady_clock::now();
    s1.filesystem().mkdir("/during-stall", 0755, getuid(), getgid());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Well inside the 30 s control no-progress deadline.
    CHECK(elapsed < scaled(3s));
    CHECK(!repair_done.load());

    links->release(peer);
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

// The claims barrier before a commit: the commit's control objects are
// claimed on this node and on the peer when it answers. A peer that never
// answers the claim does not hold the commit back; once it answers, a later
// commit's claim lands on both.
MACHA_TEST("rpc_cluster", test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "claims-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "claims-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;

    auto links = std::make_shared<FaultyLinks>();
    ServiceInstruments instruments;
    instruments.links = links;
    Service s1(c1, keys, test_durability_window, {}, {}, {}, instruments);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_metadata_writable(s1));
    REQUIRE(wait_metadata_writable(s2));
    REQUIRE(retry_while_not_ready(
        [&] { s1.filesystem().mkdir("/established", 0755, getuid(), getgid()); }));

    CatalogueItem item;
    item.id = "movie:claimed";
    item.kind = CatalogueKind::movie;
    item.title = "Claimed";

    const auto peer = s2.node().node_id();
    links->stall(peer, MessageType::retain_objects);
    (void)s1.catalogue().upsert(item);
    CHECK(links->stalled_calls() >= 1);
    const auto alone = s1.metadata_manager().snapshot().catalogue_root;
    REQUIRE(alone.has_value());
    CHECK(s1.local_state().retention().retained(RetentionClass::control, *alone));
    CHECK(!s2.local_state().retention().retained(RetentionClass::control, *alone));

    links->release(peer);
    item.title = "Claimed twice";
    (void)s1.catalogue().upsert(item);
    const auto root = s1.metadata_manager().snapshot().catalogue_root;
    REQUIRE(root.has_value());
    CHECK(*root != *alone);
    CHECK(s1.local_state().retention().retained(RetentionClass::control, *root));
    CHECK(s2.local_state().retention().retained(RetentionClass::control, *root));

    s2.stop();
    s1.stop();
}

// A peer that stops answering commit calls costs one commit the stall time
// and the next ones nothing: it is not asked again until it has had time to
// be dropped or to recover, and then repair brings it up to date.
MACHA_TEST("rpc_cluster", test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "stall-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "stall-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->write_stall = 150ms;
        config->heartbeat = 50ms;
        config->dead_after = 1500ms;
    }

    auto links = std::make_shared<FaultyLinks>();
    ServiceInstruments instruments;
    instruments.links = links;
    Service s1(c1, keys, test_durability_window, {}, {}, {}, instruments);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_metadata_writable(s1));
    REQUIRE(wait_metadata_writable(s2));
    REQUIRE(retry_while_not_ready(
        [&] { s1.filesystem().mkdir("/established", 0755, getuid(), getgid()); }));
    const auto sees = [](Service& service, const std::string& path) {
        return wait_until([&] {
            try {
                return service.filesystem().getattr(path).type == EntryType::directory;
            } catch (...) {
                return false;
            }
        }, 20s);
    };
    REQUIRE(sees(s2, "/established"));

    const auto peer = s2.node().node_id();
    links->stall(peer, MessageType::put_metadata_commit);
    const auto timed = [&](const std::string& path) {
        const auto started = Clock::now();
        s1.filesystem().mkdir(path, 0755, getuid(), getgid());
        return Clock::now() - started;
    };
    const auto first = timed("/while-stalled");
    CHECK(first >= 150ms);
    CHECK(first < scaled(5s));
    const auto asked = links->stalled_calls();
    CHECK(asked >= 1);
    const auto second = timed("/not-asked");
    CHECK(second < first);
    CHECK(links->stalled_calls() == asked);
    CHECK(s1.filesystem().getattr("/not-asked").type == EntryType::directory);

    links->release(peer);
    CHECK(sees(s2, "/while-stalled"));
    CHECK(sees(s2, "/not-asked"));

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
    c1.write_copies = c2.write_copies = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
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

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
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

// A node truncates its own history while its peer is away, and goes on
// committing. The peer returns holding the head from before the truncation,
// takes the newer head, and the two serve one namespace.
MACHA_TEST("rpc_cluster", test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "truncate-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "truncate-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    Service s1(c1, keys, test_durability_window);
    auto s2 = std::make_unique<Service>(c2, keys, test_durability_window);
    s1.start();
    s2->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2->node().membership().active().size() >= 2;
    }));
    const auto sees = [](Service& service, const std::string& path) {
        return wait_until([&] {
            try {
                return service.filesystem().getattr(path).type == EntryType::directory;
            } catch (...) {
                return false;
            }
        });
    };
    const auto one_shared_head = [&] {
        return wait_until(
            [&] {
                const auto mine = s1.local_state().replica().accepted_heads();
                const auto theirs = s2->local_state().replica().accepted_heads();
                return mine.size() == 1 && theirs.size() == 1 &&
                       mine.front().hash == theirs.front().hash;
            },
            30s);
    };
    REQUIRE(retry_while_not_ready([&] { s1.filesystem().mkdir("/a", 0755, getuid(), getgid()); }));
    REQUIRE(sees(*s2, "/a"));
    s2->filesystem().mkdir("/b", 0755, getuid(), getgid());
    REQUIRE(sees(s1, "/b"));
    REQUIRE(one_shared_head());
    const auto before = s1.local_state().replica().accepted_heads().front().hash;

    s2->stop();
    s2.reset();

    // Two commits the peer never sees, so that no record it holds is the
    // previous of the one this node keeps.
    auto& r1 = s1.local_state().replica();
    REQUIRE(retry_while_not_ready([&] { s1.filesystem().mkdir("/c", 0755, getuid(), getgid()); }));
    REQUIRE(retry_while_not_ready([&] { s1.filesystem().mkdir("/d", 0755, getuid(), getgid()); }));
    REQUIRE(r1.accepted_heads().size() == 1);
    REQUIRE(r1.diagnostics().history_records > 1);
    s1.metadata_manager().truncate_history(1, 1);
    CHECK(r1.diagnostics().history_records == 1);
    CHECK(!r1.history_contains(before));
    for (const auto* path : {"/a", "/b", "/c", "/d"})
        CHECK(s1.filesystem().getattr(path).type == EntryType::directory);

    REQUIRE(retry_while_not_ready([&] { s1.filesystem().mkdir("/e", 0755, getuid(), getgid()); }));
    CHECK(r1.diagnostics().history_records == 2);
    const auto ahead = r1.committed().hash;

    {
        MetadataReplica stopped(c2.state_path, keys.storage, {}, false);
        const auto held = stopped.accepted_heads();
        REQUIRE(held.size() == 1);
        CHECK(held.front().hash == before);
    }
    s2 = std::make_unique<Service>(c2, keys, test_durability_window);
    s2->start();
    REQUIRE(sees(*s2, "/e"));
    REQUIRE(one_shared_head());
    // The peer took this node's head as it stood: nothing was merged.
    CHECK(s2->local_state().replica().accepted_heads().front().hash == ahead);
    for (auto* service : {&s1, s2.get()})
        for (const auto* path : {"/a", "/b", "/c", "/d", "/e"})
            CHECK(service->filesystem().getattr(path).type == EntryType::directory);

    REQUIRE(retry_while_not_ready([&] { s2->filesystem().mkdir("/f", 0755, getuid(), getgid()); }));
    REQUIRE(sees(s1, "/f"));
    REQUIRE(one_shared_head());
    s2->stop();
    s1.stop();
}

// History is truncated only while the node holds one head.
MACHA_TEST("rpc_cluster", test_history_is_not_truncated_while_two_heads_are_held) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config = config_for(cluster.path() / "two-heads", cluster.keyfile(), free_port(), {});
    config.metadata_cache = std::chrono::milliseconds(0);
    BareNode node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));

    MetadataManager metadata(node, node.local_state(), node.metadata_server());
    auto& replica = node.metadata_replica();
    const auto directory = [] {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        return entry;
    };
    metadata.mutate([&](MetadataSnapshot& snapshot) { snapshot.entries["/mine"] = directory(); });
    const auto mine = replica.committed();

    // Another author's head, which this node's clock does not cover.
    const MetadataDot theirs{random_node_id(), 9};
    auto foreign_snapshot = decode_snapshot(genesis_metadata().payload);
    foreign_snapshot.metadata_write_replicas_required = 1;
    foreign_snapshot.extent_size = decode_snapshot(mine.payload).extent_size;
    foreign_snapshot.data_replication = decode_snapshot(mine.payload).data_replication;
    foreign_snapshot.legacy_clock.emplace();
    foreign_snapshot.mutation_sequences[theirs.author] = theirs.sequence;
    auto made = directory();
    stamp_entry_provenance(made, "/theirs", nullptr, theirs);
    foreign_snapshot.entries["/theirs"] = made;
    MetadataRecord foreign;
    foreign.generation = mine.generation + 7;
    foreign.previous = sha256(pattern(64, 201));
    foreign.payload = encode_snapshot(foreign_snapshot);
    foreign.hash = metadata_hash(foreign.generation, foreign.previous, foreign.payload);
    REQUIRE(replica.store_commit(foreign));
    MetadataAcceptance acceptance;
    acceptance.generation = foreign.generation;
    acceptance.hash = foreign.hash;
    acceptance.required = 1;
    acceptance.replicas = {theirs.author};
    REQUIRE(node.metadata_server().accept_commit(acceptance));
    REQUIRE(replica.accepted_heads().size() == 2);

    const auto records = replica.diagnostics().history_records;
    REQUIRE(records > 1);
    metadata.truncate_history(1, 1);
    CHECK(replica.diagnostics().history_records == records);
    CHECK(replica.accepted_heads().size() == 2);

    // Reading merges the two; the one head then becomes the root.
    CHECK(metadata.snapshot().entries.contains("/theirs"));
    REQUIRE(replica.accepted_heads().size() == 1);
    metadata.truncate_history(1, 1);
    CHECK(replica.diagnostics().history_records == 1);
    const auto after = metadata.snapshot();
    CHECK(after.entries.contains("/mine"));
    CHECK(after.entries.contains("/theirs"));
    node.stop();
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
    Service s1(c1, keys, test_durability_window);
    auto s2 = std::make_unique<Service>(c2, keys, test_durability_window);
    s1.start();
    s2->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2->node().membership().active().size() >= 2;
    }));
    REQUIRE(retry_while_not_ready([&] { s1.filesystem().mkdir("/a", 0755, getuid(), getgid()); }));
    REQUIRE(wait_until([&] {
        try {
            return s2->filesystem().getattr("/a").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }));

    auto certificates = s2->local_state().replica().accepted_head_certificates();
    REQUIRE(certificates.size() == 1);
    const auto head = certificates.front().hash;
    CHECK(s1.local_state().replica().history_contains(head));

    // s2 loses its metadata checkpoint, journal and history while stopped; its
    // acceptance certificate survives. It comes back holding a head it cannot
    // replay, which it keeps, flags and excludes from reads.
    s2->stop();
    s2.reset();
    const auto metadata = c2.state_path / "metadata";
    for (const auto* name : {"checkpoint.meta", "journal.log", "history.log"})
        std::filesystem::remove(metadata / name);
    {
        MetadataReplica damaged(c2.state_path, keys.storage, {}, false);
        REQUIRE(damaged.unreconstructable_heads() == std::vector<Hash256>{head});
    }
    s2 = std::make_unique<Service>(c2, keys, test_durability_window);
    s2->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2->node().membership().active().size() >= 2;
    }));

    // The flag's 30 s cooldown means only repair can clear it here.
    auto& replica = s2->local_state().replica();
    REQUIRE(wait_until([&] {
        (void)s2->metadata_manager().repair_unreconstructable_heads();
        return replica.unreconstructable_heads().empty();
    }));
    CHECK(replica.accepted_heads().size() == 1);
    CHECK(replica.accepted_heads().front().hash == head);
    CHECK(s2->filesystem().getattr("/a").type == EntryType::directory);

    // The wire call itself, independently of the driver.
    Writer request;
    request.fixed(head.bytes);
    auto reply = s2->node().call(s1.node().membership().self(),
                                 MessageType::get_metadata_history_record, request.take(),
                                 FrameType::control);
    REQUIRE(reply.message.type == MessageType::metadata_history_entry_reply);
    auto served = decode_metadata_history_entry(reply.message.payload);
    CHECK(served.hash == head);
    CHECK(served.body == MetadataHistoryEntry::Body::full);

    s2->stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_a_hung_health_probe_is_retried_inside_the_liveness_budget) {
    // A hung health ping is retried within the liveness budget, not held for
    // all of dead_after. FaultyLinks holds every CONTROL-lane ping unanswered
    // and counts the attempts.
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

    auto links = std::make_shared<FaultyLinks>();
    ServiceInstruments instruments;
    instruments.links = links;
    Service s1(c1, keys, test_durability_window, {}, {}, {}, instruments);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    const auto peer = s2.node().node_id();
    links->stall(peer, MessageType::ping, TransportLane::control);
    const auto started = std::chrono::steady_clock::now();
    const bool retried = wait_until([&] { return links->stalled_calls() >= 3; }, 3s);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    links->release(peer);

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

        FlagNode(RpcLinks& links, ClusterKeys keys, NodeInfo node,
                 std::chrono::milliseconds connect_timeout)
            : info(std::move(node)),
              client(
                  links, keys, [this] { return info; }, [](const NodeInfo&) {}, [](uint64_t) {},
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

    NetworkLinks network;
    FaultyLinks site_links;
    FaultyLinks hub_links;

    // A peer that knows the node cannot be dialled does not try, and says
    // so at once rather than after a connect timeout.
    {
        FlagNode hub(network, keys, capable_info(), 2s);
        FlagNode site(network, keys, incapable_info(), 300ms);
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

    FlagNode hub(hub_links, keys, capable_info(), 2s);
    FlagNode site(site_links, keys, incapable_info(), 300ms);

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
    // The site lists the route once its dial is installed; the hub can use
    // the session, as it just has, a moment before that.
    REQUIRE(wait_until([&] { return site.client.has_route(hub.info.id, TransportLane::data); },
                       5s));

    // A DATA lane that dies underneath (a NAT mapping expiring) is redialled
    // by the site on its own initiative, without being asked.
    site.client.set_maintained_peers([&] { return std::vector<NodeInfo>{hub.info}; });
    const Endpoint hub_endpoint{hub.info.host, hub.info.port};
    const auto data_dials = site_links.dials(hub_endpoint, TransportLane::data);
    const auto data_sessions = hub_links.admitted(site.info.id, TransportLane::data);
    site_links.sever(hub_endpoint, TransportLane::data);
    // Redialled by the site, and installed by the hub: until the hub lists
    // the new session it has no DATA route and would ask for one.
    REQUIRE(wait_until(
        [&] {
            return site_links.dials(hub_endpoint, TransportLane::data) > data_dials &&
                   site.client.has_route(hub.info.id, TransportLane::data) &&
                   hub_links.admitted(site.info.id, TransportLane::data) > data_sessions;
        },
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
        Node(RpcLinks& links, ClusterKeys keys, NodeInfo node)
            : info(std::move(node)),
              client(
                  links, keys, [this] { return info; }, [](const NodeInfo&) {}, [](uint64_t) {},
                  2s, 100ms, 30s),
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
    NetworkLinks network;
    FaultyLinks high_links;
    Node low(network, keys, a);
    Node high(high_links, keys, b);

    // The low node's session reaches the high node but is not yet installed.
    high_links.hold_sessions(low.info.id);
    REQUIRE(low.client.call(high.info, MessageType::members, Bytes{1}, 2s).message.payload ==
            Bytes{1});
    REQUIRE(!high.client.has_route(low.info.id, TransportLane::control));

    // The low node retires the high node's dial before the call is placed on
    // it; the call waits for that. A dial retired before it was even
    // installed reaches no send, and leaves no route.
    high_links.retire_before_send(low.info.id);
    auto call = std::async(std::launch::async, [&] {
        return high.client.call(low.info, MessageType::members, Bytes{2}, 5s);
    });
    REQUIRE(wait_until(
        [&] {
            return high_links.sent_after_retirement() ||
                   (high_links.retirement_waits() == 0 &&
                    high.client.stats().connections_created == 1 &&
                    !high.client.has_route(low.info.id, TransportLane::control));
        },
        5s));
    // The low node's session now installs, as it would a moment later.
    high_links.release_sessions(low.info.id);
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

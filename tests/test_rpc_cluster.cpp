// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_FAST_TEST("rpc_cluster", test_rpc_reassembly_has_count_byte_and_message_bounds) {
    MessageAssembler assembler(2, 8, 16);
    auto fragment = [](uint64_t request, bool first, bool last, size_t bytes) {
        return WireFragment{request, FrameType::foreground, MessageType::put_object,
                            first, last, Bytes(bytes, 0x5a)};
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

MACHA_TEST("rpc_cluster", test_async_rpc_move_ownership) {
    std::atomic_int cancelled{};

    // AsyncRpc is an owning cancellation handle. Moving it must transfer that
    // ownership; destruction of the moved-from object must be inert. This is
    // particularly important on libc++, where std::function's moved-from state
    // is permitted to remain non-empty.
    std::optional<AsyncRpc> moved;
    {
        std::promise<RpcReply> promise;
        AsyncRpc original(promise.get_future(), [&] { ++cancelled; }, {});
        moved.emplace(std::move(original));
    }
    CHECK(cancelled.load() == 0);
    moved.reset();
    CHECK(cancelled.load() == 1);

    // Move assignment must also cancel any request already owned by the target,
    // while leaving the source destructor inert.
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

MACHA_TEST("rpc_cluster", test_rpc_v15_frame_priority_and_variable_length) {
    CHECK(frame_type_priority(FrameType::control) < frame_type_priority(FrameType::foreground));
    CHECK(frame_type_priority(FrameType::foreground) < frame_type_priority(FrameType::read_ahead));
    CHECK(frame_type_priority(FrameType::read_ahead) <
          frame_type_priority(FrameType::speculative));
    CHECK(default_frame_type(MessageType::ping) == FrameType::control);
    CHECK(default_frame_type(MessageType::get_object) == FrameType::foreground);
    CHECK(default_frame_type(MessageType::get_control_object) == FrameType::speculative);
    CHECK(std::string(message_type_name(MessageType::put_metadata_commit)) ==
          "put_metadata_commit");
    CHECK(std::string(message_type_name(MessageType::accept_metadata_commit)) ==
          "accept_metadata_commit");
    CHECK(std::string(message_type_name(MessageType::get_metadata_heads)) ==
          "get_metadata_heads");

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
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 5s, 30s, 4096);
    Endpoint endpoint{"127.0.0.1", port};

    // A non-multiple of max_frame_size proves that the final frame is naturally
    // short rather than padded to a fixed transport block size.
    auto odd = pattern(12'345);
    auto odd_reply = client.call(endpoint, MessageType::ping, odd, 1s);
    CHECK(odd_reply.message.payload == odd);

    auto metadata_reply = client.call(endpoint, MessageType::get_metadata, Bytes{0x4d},
                                      FrameType::read_ahead, 2s);
    CHECK(metadata_reply.message.type == MessageType::ok);
    CHECK(metadata_reply.message.payload == Bytes{0x4d});

    // Content-addressed metadata objects are potentially large and therefore
    // run at speculative worker priority, but they deliberately stay on the
    // CONTROL TCP session. Catalogue bootstrap must not require a DATA lane.
    auto control_object_reply =
        client.call(endpoint, MessageType::put_control_object, Bytes{0x43, 0x41, 0x54},
                    FrameType::speculative, 2s);
    CHECK(control_object_reply.message.type == MessageType::ok);
    CHECK(client.stats().canonical_connections == 1);

    // Start a large speculative transfer, then introduce foreground work. The
    // writer reconsiders priority after every <=4 KiB variable-length frame, so
    // foreground reaches the server before the speculative message completes.
    Bytes speculative(32 * 1024 * 1024, 0x53);
    auto background = client.call_async(endpoint, MessageType::put_object, speculative,
                                        FrameType::speculative);
    std::this_thread::sleep_for(2ms);
    auto foreground = client.call_async(endpoint, MessageType::put_object, Bytes{0x46},
                                        FrameType::foreground);
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

    // Cancellation can race with the writer while one frame is outside the
    // outbound deque. The cancelled transfer must not be requeued after that
    // frame, otherwise the peer sees continuation frames after cancel_transfer
    // discarded its assembler state and tears down the canonical connection.
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

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms);
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

    // Empty request immediately follows prior frames on the same channel. This
    // would desynchronise the old GCM framing bug.
    auto empty_reply = client.call(endpoint, MessageType::ping, {}, 1s);
    CHECK(empty_reply.message.type == MessageType::ok);
    CHECK(empty_reply.message.payload.empty());

    auto stats = client.stats();
    CHECK(stats.connections_created == 1);
    CHECK(stats.connections_reused >= 2);

    // The argument to synchronous call() is now a stall-observation interval,
    // not a request deadline. A healthy RPC may take arbitrarily longer than
    // that interval and must still complete without its connection being torn
    // down merely because wall-clock time elapsed.
    Bytes very_slow_payload{1};
    auto started = Clock::now();
    auto very_slow = client.call(endpoint, MessageType::ping, very_slow_payload, 50ms);
    CHECK(very_slow.message.type == MessageType::ok);
    CHECK(very_slow.message.payload == very_slow_payload);
    CHECK(Clock::now() - started >= 60ms);

    // The same persistent connection remains usable after a slow request; there is no
    // timeout-induced backoff/reconnect cycle.
    auto recovered = client.call(endpoint, MessageType::ping, fast_payload, 50ms);
    CHECK(recovered.message.type == MessageType::ok);
    CHECK(client.stats().connections_created == 1);

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
            : info(std::move(node)),
              client(keys, [this] { return info; }, [](const NodeInfo&) {}, [](uint64_t) {},
                     500ms, 10s, 30s),
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

    // Once A has called B, B can originate an RPC back over the accepted socket.
    // It must not create a second B->A TCP connection.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        CHECK(a.client.call(b.info, MessageType::members, Bytes{1}, 1s).message.payload == Bytes{1});
        CHECK(b.client.stats().connections_created == 0);
        CHECK(b.client.call(a.info, MessageType::members, Bytes{2}, 1s).message.payload == Bytes{2});
        CHECK(b.client.stats().connections_created == 0);

        // DATA is a second independently canonical bidirectional lane. A opens
        // it lazily; B must reuse the accepted data session rather than dial a
        // third physical connection back to A.
        CHECK(a.client.call(b.info, MessageType::put_object, Bytes{3},
                            FrameType::foreground, 1s).message.payload == Bytes{3});
        CHECK(b.client.call(a.info, MessageType::get_object, Bytes{4},
                            FrameType::foreground, 1s).message.payload == Bytes{4});
        CHECK(b.client.stats().connections_created == 0);
        CHECK(a.client.stats().canonical_connections == 2);
        CHECK(b.client.stats().canonical_connections == 2);
    }

    // Simultaneous cross-dial starts with two physical sessions. Both nodes
    // must select the same winner and all later RPCs must reuse it.
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
        CHECK(a.client.call(b.info, MessageType::members, Bytes{5}, 1s).message.payload == Bytes{5});
        CHECK(b.client.call(a.info, MessageType::members, Bytes{6}, 1s).message.payload == Bytes{6});
        CHECK(a.client.stats().connections_created == a_created);
        CHECK(b.client.stats().connections_created == b_created);
    }

    // Endpoint spelling is not peer identity. A hostname alias can cause a
    // transient second dial, but authenticated NodeId dedup leaves one route.
    {
        TestNode a(keys, node_info(), echo);
        TestNode b(keys, node_info(), echo);
        Endpoint numeric{"127.0.0.1", b.info.port};
        Endpoint alias{"localhost", b.info.port};
        CHECK(a.client.call(numeric, MessageType::members, Bytes{7}, 1s).message.payload == Bytes{7});
        CHECK(a.client.call(alias, MessageType::members, Bytes{8}, 1s).message.payload == Bytes{8});
        REQUIRE(wait_until([&] { return a.client.stats().canonical_connections == 1; }));
        auto created = a.client.stats().connections_created;
        CHECK(a.client.call(alias, MessageType::members, Bytes{9}, 1s).message.payload == Bytes{9});
        CHECK(a.client.stats().connections_created == created);
    }

    // A failed dial puts the endpoint into retry backoff, but that backoff must
    // not mask a canonical route which arrives inbound immediately afterwards.
    // This is the normal recovery shape when a peer reconnects while the other
    // side is still remembering the failed outbound attempt.
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

        // Still inside the failed dial's minimum 250-ms retry-backoff window.
        // Endpoint lookup must reuse the authenticated inbound route before
        // consulting dial backoff.
        CHECK(a.client.call(b_endpoint, MessageType::members, Bytes{12}, 1s).message.payload ==
              Bytes{12});
    }

    // Retirement is a drain, not a reset. Force the higher NodeId to have a
    // slow request outstanding on the connection which cross-dial arbitration
    // will discard, then create the canonical lower->higher connection.
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

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();

    REQUIRE(wait_until([&] {
        const auto a = n1.rpc_stats();
        const auto b = n2.rpc_stats();
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2 &&
               a.canonical_connections == 1 && b.canonical_connections == 1;
    }));

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
    RpcServer server("127.0.0.1", port, keys, server_info,
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

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 20ms, 80ms);
    Endpoint endpoint{"127.0.0.1", port};

    // The 20-ms value below is not a deadline: both 120-ms RPCs are healthy and
    // must complete without the peer transport being destroyed. They also
    // outlive the 80-ms peer-death window while priority control pings on the independent control
    // connection prove that the peer itself remains alive.
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

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Occupy every data worker. Health and membership use a separate control TCP
    // stream and must remain prompt regardless of data-lane work.
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

    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Occupy both ordinary control handlers with storage-shaped work. Health
    // and membership must use the reserved fast-control executor rather than
    // queue behind those handlers.
    auto slow1 = client.call_async(endpoint, MessageType::have_object, Bytes{1},
                                   FrameType::control);
    auto slow2 = client.call_async(endpoint, MessageType::have_object, Bytes{2},
                                   FrameType::control);
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
        RpcServerExecutionLimits{.metadata_workers = 1,
                                 .metadata_pending_jobs = 2,
                                 .metadata_pending_bytes = 8});
    server.start();

    NodeInfo client_info;
    client_info.id = random_node_id();
    client_info.host = "127.0.0.1";
    client_info.port = free_port();
    client_info.failure_domain = "client-site";
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // One metadata mutation may execute while two more wait in the dedicated
    // bounded queue. These cover every mutation RPC routed to the executor.
    auto history = client.call_async(endpoint, MessageType::put_metadata_history_entry,
                                     Bytes{0x01, 0x02});
    REQUIRE(metadata_gate.wait_for_entries(1));
    auto commit = client.call_async(endpoint, MessageType::put_metadata_commit,
                                    Bytes{0x03, 0x04});
    auto acceptance = client.call_async(endpoint, MessageType::accept_metadata_commit,
                                        Bytes{0x05, 0x06});
    REQUIRE(wait_until([&] {
        const auto stats = server.work_stats();
        return stats.metadata_active_jobs == 1 && stats.metadata_pending_jobs == 2 &&
               stats.metadata_pending_bytes == 4;
    }, 2s));

    // Job-count pressure is explicit: overload receives an ordinary RPC error
    // without closing the session or occupying a control/data worker.
    auto queue_full = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{0x07});
    REQUIRE(queue_full.wait_for(1s) == std::future_status::ready);
    CHECK(queue_full.get().message.type == MessageType::error);

    // Health, membership, ordinary control work, and foreground DATA work all
    // complete while the metadata worker and queue remain deliberately blocked.
    CHECK(client.call(endpoint, MessageType::ping, {}, 100ms).message.type == MessageType::ok);
    CHECK(client.call(endpoint, MessageType::members, {}, 100ms).message.type ==
          MessageType::members_reply);
    CHECK(client.call(endpoint, MessageType::have_object, Bytes{0x08}, 100ms).message.type ==
          MessageType::bool_reply);
    CHECK(client.call(endpoint, MessageType::get_object, Bytes{0x09}, FrameType::foreground,
                      100ms).message.type == MessageType::object_reply);

    metadata_gate.open();
    for (auto* rpc : {&history, &commit, &acceptance}) {
        REQUIRE(rpc->wait_for(2s) == std::future_status::ready);
        CHECK(rpc->get().message.type == MessageType::bool_reply);
    }
    REQUIRE(wait_until([&] {
        const auto stats = server.work_stats();
        return stats.metadata_active_jobs == 0 && stats.metadata_pending_jobs == 0 &&
               stats.metadata_pending_bytes == 0;
    }, 2s));

    // Payload-byte pressure is enforced even when no other metadata work is
    // present; the rejected job never reaches the handler.
    auto too_large = client.call_async(endpoint, MessageType::accept_metadata_commit,
                                       Bytes(9, 0x0a));
    REQUIRE(too_large.wait_for(1s) == std::future_status::ready);
    CHECK(too_large.get().message.type == MessageType::error);
    CHECK(metadata_calls.load() == 3);
    const auto final_stats = server.work_stats();
    CHECK(final_stats.metadata_rejected_jobs == 2);

    // Both CONTROL and DATA sessions remain usable after backpressure replies.
    CHECK(client.call(endpoint, MessageType::ping, {}, 100ms).message.type == MessageType::ok);
    CHECK(client.call(endpoint, MessageType::get_object, Bytes{0x0b}, FrameType::foreground,
                      100ms).message.type == MessageType::object_reply);

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
        RpcServerExecutionLimits{.metadata_workers = 2,
                                 .metadata_pending_jobs = 8,
                                 .metadata_pending_bytes = 64});
    server.start();

    RpcClient client_a(keys, [first_client] { return first_client; }, [](const NodeInfo&) {},
                       [](uint64_t) {}, 500ms, 100ms, 2s);
    RpcClient client_b(keys, [second_client] { return second_client; }, [](const NodeInfo&) {},
                       [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    auto first = client_a.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(first_job_gate.wait_for_entries(1));
    auto same_peer_next =
        client_a.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    auto other_peer =
        client_b.call_async(endpoint, MessageType::put_metadata_commit, Bytes{3});

    // The second worker may serve another peer, but it must not allow one
    // peer's acceptance/store sequence to overtake that peer's blocked owner.
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
        RpcServerExecutionLimits{.metadata_workers = 1,
                                 .metadata_pending_jobs = 4,
                                 .metadata_pending_bytes = 64});
    server.start();

    NodeInfo client_info{random_node_id(), "127.0.0.1", "client-site", free_port()};
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    auto running = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(before_durability.wait_for_entries(1));
    auto queued = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    REQUIRE(wait_until([&] { return server.work_stats().metadata_pending_jobs == 1; }, 2s));

    // A queued job has not crossed a durability boundary and is removed by an
    // ordinary transfer cancellation without ever entering the handler.
    queued.cancel();
    REQUIRE(wait_until([&] { return server.work_stats().metadata_pending_jobs == 0; }, 2s));
    CHECK(handler_calls.load() == 1);

    // Once execution owns a job, disconnecting its reply route must not cancel
    // work across an unknown durability boundary. It finishes independently;
    // the now-detached reply is simply discarded.
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
        RpcServerExecutionLimits{.metadata_workers = 1,
                                 .metadata_pending_jobs = 4,
                                 .metadata_pending_bytes = 64});
    server.start();

    NodeInfo client_info{random_node_id(), "127.0.0.1", "client-site", free_port()};
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    auto running = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{1});
    REQUIRE(running_gate.wait_for_entries(1));
    auto queued = client.call_async(endpoint, MessageType::put_metadata_commit, Bytes{2});
    REQUIRE(wait_until([&] { return server.work_stats().metadata_pending_jobs == 1; }, 2s));

    auto stopping = std::async(std::launch::async, [&] { server.stop(); });
    // Closing the session detaches both client replies before the running
    // durability owner is released. The queued request can no longer execute.
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
    RpcClient client(keys, [client_info] { return client_info; }, [](const NodeInfo&) {},
                     [](uint64_t) {}, 500ms, 100ms, 2s);
    Endpoint endpoint{"127.0.0.1", port};

    // Lower-priority work may use most of the DATA execution pool, but it must
    // leave execution capacity for a playback/seek read that arrives later.
    std::vector<AsyncRpc> bulk;
    for (int i = 0; i < 8; ++i)
        bulk.push_back(client.call_async(endpoint, MessageType::put_object, Bytes{0x42},
                                         FrameType::read_ahead));
    REQUIRE(lower_priority_gate.wait_for_entries(6));

    auto started = Clock::now();
    auto foreground = client.call(endpoint, MessageType::get_object, Bytes{0x46},
                                  FrameType::foreground, 100ms);
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

MACHA_TEST("rpc_cluster", test_early_replication_quorum) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto pf = free_port();
    auto ps = free_port();

    // This test is specifically about object PUT quorum latency. Keep metadata
    // consensus out of the test and use two deterministic RPC peers rather than
    // relying on service discovery/background maintenance to make the fast peer
    // reachable. That removes a platform/timing dependency which made this test
    // intermittently (and on macOS, consistently) fail before the assertion it
    // was intended to exercise.
    auto c1 = config_for(cluster.path() / "n1", cluster.keyfile(), p1);
    c1.dead_after = 3s;
    c1.metadata_min_write_replicas = 1;
    // Keep the node's background membership exchange out of this latency test.
    // The peers are injected directly below; the test should measure object
    // quorum completion, not race a 100ms control-plane scheduler.
    c1.heartbeat = 10s;
    Service s1(c1, keys);
    s1.start();

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
                if (slow) slow_replica_gate.enter_and_wait();
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

    RpcServer fast_server("127.0.0.1", pf, keys, fast_info,
                          peer_handler(fast_info, false), [](const NodeInfo&) {});
    RpcServer slow_server("127.0.0.1", ps, keys, slow_info,
                          peer_handler(slow_info, true), [](const NodeInfo&) {});
    fast_server.start();
    slow_server.start();

    s1.node().membership().observe(fast_info, true);
    s1.node().membership().observe(slow_info, true);
    REQUIRE(s1.node().membership().active().size() == 3);

    DistributedStore store(s1.node());
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

    // R=3 is a convergence target, not a foreground quorum. With W=1 the
    // durable local placement satisfies publication immediately; slow desired
    // replicas must not even be placed on the foreground critical path.
    DistributedStore store(node);
    auto data = pattern(128 * 1024);
    auto id = object_id(data);
    auto started = Clock::now();
    CHECK(store.put(id, data));
    auto elapsed = Clock::now() - started;
    CHECK(elapsed < 500ms);
    CHECK(stalled_owner_gate.entered() == 0);
    CHECK(node.local_store().has(id));

    stalled_owner_gate.open();
    slow2_server.stop();
    slow1_server.stop();
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

    // A synchronous call_async() failure must not increment a dead
    // "completed" counter but was not treated as a failed replica. With no
    // pending RPC, the quorum loop then slept forever instead of trying the
    // deterministic fallback owner. Keep a cancellation watchdog so this
    // regression fails boundedly rather than hanging the entire test binary.
    std::atomic_bool cancelled{false};
    std::jthread watchdog([&](std::stop_token stop) {
        const auto deadline = Clock::now() + 2s;
        while (!stop.stop_requested() && Clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        if (!stop.stop_requested())
            cancelled.store(true, std::memory_order_relaxed);
    });

    DistributedStore store(service.node());
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
    bool rejected = false;
    try {
        (void)service.filesystem().getattr("/");
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find("waiting for bootstrap peer") != std::string::npos;
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

    NodeRuntime node(config, keys);
    node.start();

    // Keep an unreachable peer in active membership and choose its identity so
    // the old placement scheme would have selected this fresh node as a genesis
    // authority. This deterministically exercises the dangerous path: both metadata
    // RPC surveys fail, yet a replication-1 joiner could previously form an
    // empty generation-2 namespace on itself.
    static constexpr char label[] = "macha/metadata-placement/v1";
    const auto placement_key =
        sha256({reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1});
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
        rejected = std::string(error.what()).find("bootstrap checkpoint survey") !=
                   std::string::npos;
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
    // Reachability deliberately precedes local metadata recovery. This test is
    // about an already-established write floor, so make both metadata planes
    // ready before creating that established history.
    (void)s1.filesystem();
    (void)s2.filesystem();
    s1.filesystem().mkdir("/established", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try { return s2.filesystem().getattr("/established").type == EntryType::directory; }
        catch (...) { return false; }
    }));

    auto c3 = config_for(cluster.path() / "n3", cluster.keyfile(), p3,
                         {{"127.0.0.1", p1}, {"127.0.0.1", p2}});
    c3.metadata_min_write_replicas = 1; // deliberately wrong
    Service s3(c3, keys);
    s3.start();
    REQUIRE(wait_until([&] { return s1.node().membership().active().size() >= 3; }));

    // The bad peer is quarantined from metadata writes; it cannot reduce the
    // availability of the two policy-compatible replicas which already satisfy W=2.
    s1.filesystem().mkdir("/still-writable", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try { return s2.filesystem().getattr("/still-writable").type == EntryType::directory; }
        catch (...) { return false; }
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

    // Both peers may attempt genesis simultaneously after symmetric discovery.
    // Competing generation-2 proposals must converge on one metadata history.
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
        if (stage == "metadata-repair-begin" &&
            gate_repair.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel)) {
            repair_gate.enter_and_wait();
        }
    });
    Service s2(c2, keys);
    struct GateOpener {
        TestGate& gate;
        ~GateOpener() { gate.open(); }
    } open_on_exit{repair_gate};

    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    REQUIRE(wait_until([&] {
        const auto d1 = s1.metadata_convergence_diagnostics();
        const auto d2 = s2.metadata_convergence_diagnostics();
        return s1.node().metadata_replica().committed_generation() > 1 &&
               s1.node().metadata_replica().committed_generation() ==
                   s2.node().metadata_replica().committed_generation() &&
               !d1.scheduled && d1.runs_scheduled == d1.runs_completed &&
               !d2.scheduled && d2.runs_scheduled == d2.runs_completed;
    }, 10s));

    const auto before = s1.metadata_convergence_diagnostics();
    const auto baseline_generation =
        s2.node().metadata_replica().committed_generation();
    const auto announcements_before = s2.node().metadata_announcements();
    gate_repair.store(true, std::memory_order_release);

    s2.filesystem().mkdir("/coalesced-0", 0755, getuid(), getgid());
    REQUIRE(repair_gate.wait_for_entries(1, 5s));
    const auto claimed = s1.metadata_convergence_diagnostics();
    CHECK(claimed.runs_scheduled == before.runs_scheduled + 1);
    CHECK(claimed.runs_completed == before.runs_completed);

    constexpr size_t burst = 32;
    for (size_t i = 1; i <= burst; ++i) {
        s2.filesystem().mkdir("/coalesced-" + std::to_string(i),
                              0755, getuid(), getgid());
    }
    const auto final_generation =
        s2.node().metadata_replica().committed_generation();
    CHECK(final_generation == baseline_generation + burst + 1);
    REQUIRE(wait_until([&] {
        return s1.node().known_metadata_generation() >= final_generation;
    }, 5s));
    REQUIRE(wait_until([&] {
        return s1.metadata_convergence_diagnostics().latest_generation == final_generation;
    }, 5s));
    CHECK(s2.node().metadata_announcements() == announcements_before + burst + 1);

    const auto gated = s1.metadata_convergence_diagnostics();
    CHECK(gated.events_received > claimed.events_received);
    CHECK(gated.latest_generation == final_generation);
    CHECK(gated.runs_scheduled == before.runs_scheduled + 1);
    CHECK(gated.runs_completed == before.runs_completed);

    repair_gate.open();
    REQUIRE(wait_until([&] {
        const auto diagnostics = s1.metadata_convergence_diagnostics();
        return !diagnostics.scheduled &&
               diagnostics.runs_completed == before.runs_completed + 2 &&
               s1.node().metadata_replica().committed_generation() == final_generation;
    }, 10s));

    const auto settled = s1.metadata_convergence_diagnostics();
    CHECK(settled.runs_scheduled == before.runs_scheduled + 2);
    CHECK(settled.runs_completed == before.runs_completed + 2);
    CHECK(settled.completed_epoch == settled.requested_epoch);
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
        if (stage == "metadata-repair-begin" &&
            gate_repairs.load(std::memory_order_acquire) &&
            !gate_once1.exchange(true, std::memory_order_acq_rel)) {
            repair_gate1.enter_and_wait();
        }
    });
    Service s2(c2, keys, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" &&
            gate_repairs.load(std::memory_order_acquire) &&
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
    REQUIRE(wait_until([&] {
        const auto d1 = s1.metadata_convergence_diagnostics();
        const auto d2 = s2.metadata_convergence_diagnostics();
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2 &&
               s1.node().metadata_replica().committed_generation() > 1 &&
               s1.node().metadata_replica().committed().hash ==
                   s2.node().metadata_replica().committed().hash &&
               !d1.scheduled && d1.runs_scheduled == d1.runs_completed &&
               !d2.scheduled && d2.runs_scheduled == d2.runs_completed;
    }, 10s));

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
        sibling.hash = metadata_hash(sibling.generation, sibling.previous,
                                     sibling.payload);
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

    // Prime node 1's remote generation to the sibling generation. The next
    // notice therefore carries no numeric advance; its only new information is
    // that node 2's accepted-head topology changed at the same generation.
    s2.node().announce_metadata_generation(left.generation);
    REQUIRE(wait_until([&] {
        return s1.node().remote_metadata_generation() == left.generation;
    }, 5s));
    const auto before_sibling_notice = s1.metadata_convergence_diagnostics();

    const auto right = make_sibling(s2.node(), "/right-sibling");
    REQUIRE(right.generation == left.generation);
    REQUIRE(right.hash != left.hash);
    REQUIRE(wait_until([&] {
        return s1.metadata_convergence_diagnostics().requested_epoch >
               before_sibling_notice.requested_epoch;
    }, 2s));

    // Installing identical acceptance evidence changes no accepted-head
    // topology and must therefore produce neither a local event nor a remote
    // rebroadcast.
    std::this_thread::sleep_for(100ms);
    const auto before_duplicate1 = s1.metadata_convergence_diagnostics();
    const auto before_duplicate2 = s2.metadata_convergence_diagnostics();
    const auto announcements_before_duplicate = s2.node().metadata_announcements();
    MetadataAcceptance duplicate;
    duplicate.generation = right.generation;
    duplicate.hash = right.hash;
    duplicate.required = 1;
    duplicate.replicas = {s2.node().node_id()};
    REQUIRE(s2.node().accept_metadata_commit(duplicate));
    CHECK(s2.node().metadata_announcements() == announcements_before_duplicate);
    std::this_thread::sleep_for(100ms);
    CHECK(s1.metadata_convergence_diagnostics().requested_epoch ==
          before_duplicate1.requested_epoch);
    CHECK(s2.metadata_convergence_diagnostics().requested_epoch ==
          before_duplicate2.requested_epoch);

    repair_gate1.open();
    REQUIRE(wait_until([&] {
        try {
            const auto heads = s1.node().metadata_replica().accepted_heads();
            return heads.size() == 1 && heads.front().generation > left.generation &&
                   s1.filesystem().getattr("/left-sibling").type == EntryType::directory &&
                   s1.filesystem().getattr("/right-sibling").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }, 10s));

    repair_gate2.open();
    REQUIRE(wait_until([&] {
        try {
            return s2.node().metadata_replica().accepted_heads().size() == 1 &&
                   s2.filesystem().getattr("/left-sibling").type == EntryType::directory &&
                   s2.filesystem().getattr("/right-sibling").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }, 10s));

    s2.stop();
    s1.stop();
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
    auto c1 = config_for(cluster.path() / "lagging-burst-n1", cluster.keyfile(), p1,
                         peers(p1));
    auto c2 = config_for(cluster.path() / "lagging-burst-n2", cluster.keyfile(), p2,
                         peers(p2));
    auto c3 = config_for(cluster.path() / "lagging-burst-n3", cluster.keyfile(), p3,
                         peers(p3));
    for (auto* config : {&c1, &c2, &c3}) {
        config->replication = 3;
        config->min_write_replicas = 1;
        config->metadata_min_write_replicas = 2;
        config->heartbeat = 50ms;
        config->dead_after = 200ms;
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
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 3 &&
               s2.node().membership().active().size() == 3 &&
               s3->node().membership().active().size() == 3;
    }, 10s));

    s1.filesystem().mkdir("/lagging-base", 0755, getuid(), getgid());
    REQUIRE(wait_until([&] {
        try {
            return s3->filesystem().getattr("/lagging-base").type == EntryType::directory;
        } catch (...) {
            return false;
        }
    }, 10s));
    const auto base = s3->node().metadata_replica().committed();

    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }, 5s));

    constexpr size_t burst = 64;
    for (size_t index = 0; index < burst; ++index) {
        s1.filesystem().mkdir("/lagging-burst-" + std::to_string(index),
                              0755, getuid(), getgid());
    }
    const auto final = s1.node().metadata_replica().committed();
    CHECK(final.generation == base.generation + burst);
    REQUIRE(wait_until([&] {
        return s2.node().metadata_replica().committed().hash == final.hash;
    }, 10s));

    s3 = std::make_unique<Service>(c3, keys);
    s3->start();
    (void)s3->filesystem();
    REQUIRE(wait_until([&] {
        try {
            const auto diagnostics = s3->metadata_convergence_diagnostics();
            return s3->node().metadata_replica().committed().hash == final.hash &&
                   s3->filesystem().getattr("/lagging-burst-63").type ==
                       EntryType::directory &&
                   !diagnostics.scheduled &&
                   diagnostics.runs_scheduled == diagnostics.runs_completed;
        } catch (...) {
            return false;
        }
    }, 15s));

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

MACHA_TEST("rpc_cluster", test_metadata_file_touch_requires_retention_before_acceptance) {
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
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 3 &&
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
        return s1.node().local_store().valid(extent) &&
               s2.node().local_store().valid(extent) &&
               s3->node().local_store().valid(extent);
    }));
    // The accepted file reference itself must already have installed physical
    // liveness evidence on the DATA durability floor.
    CHECK(s1.node().retention_store().retained(RetentionClass::data, extent));
    CHECK(s2.node().retention_store().retained(RetentionClass::data, extent));
    CHECK(s3->node().retention_store().retained(RetentionClass::data, extent));

    const auto before = s1.node().metadata_replica().committed();
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));

    // Metadata W=2 is still available, but this semantic file touch needs a
    // fresh causal retention dot on DATA W=3 so that a concurrent delete cannot
    // erase the inherited liveness claim. The metadata head must not advance
    // when that pre-publication retention barrier cannot be satisfied.
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
    s1.filesystem().chmod("/retained.bin", 0600);
    CHECK((s1.filesystem().getattr("/retained.bin").mode & 0777U) == 0600U);
    CHECK(s1.node().metadata_replica().committed().hash != before.hash);

    s3->stop();
    s2.stop();
    s1.stop();
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
    REQUIRE(wait_until([&] {
        return s1.node().membership().all_known_reachable() &&
               s2.node().membership().all_known_reachable() &&
               s3->node().membership().all_known_reachable() &&
               s1.metadata_manager().cluster_status().stable &&
               s2.metadata_manager().cluster_status().stable &&
               s3->metadata_manager().cluster_status().stable;
    }, 10s));

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

    // Placement is intentionally free to choose either active replica. Record
    // the concrete claims/copies present on the cohort that will remain online;
    // the invariant is that destructive maintenance must not remove any of
    // those pre-existing resources while a durably-known node is unreachable.
    const bool n1_claim_before =
        s1.node().retention_store().retained(RetentionClass::data, extent);
    const bool n2_claim_before =
        s2.node().retention_store().retained(RetentionClass::data, extent);
    const bool n1_copy_before = s1.node().local_store().valid(extent);
    const bool n2_copy_before = s2.node().local_store().valid(extent);
    REQUIRE(n1_claim_before || n2_claim_before);
    REQUIRE(n1_copy_before || n2_copy_before);

    // The remaining W=2 cohort may continue accepting metadata while node 3 is
    // offline, but the persisted roster must make that degraded state a hard
    // fence for claim release and physical reclamation.
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

    // Give maintenance several complete zero-grace passes. If partition GC is
    // accidentally re-enabled, this fails by observing either the causal claim
    // or the local bytes disappear while the known third node remains offline.
    std::this_thread::sleep_for(800ms);
    if (n1_claim_before)
        CHECK(s1.node().retention_store().retained(RetentionClass::data, extent));
    if (n2_claim_before)
        CHECK(s2.node().retention_store().retained(RetentionClass::data, extent));
    if (n1_copy_before) CHECK(s1.node().local_store().valid(extent));
    if (n2_copy_before) CHECK(s2.node().local_store().valid(extent));

    // Recreate the third process from its original persistent state. All three
    // sides must directly rediscover one another and metadata must converge
    // before the healthy-cluster GC epoch is allowed to reclaim the delete.
    s3 = std::make_unique<Service>(c3, keys);
    s3->start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().all_known_reachable() &&
               s2.node().membership().all_known_reachable() &&
               s3->node().membership().all_known_reachable() &&
               s1.metadata_manager().cluster_status().stable &&
               s2.metadata_manager().cluster_status().stable &&
               s3->metadata_manager().cluster_status().stable;
    }, 10s));
    REQUIRE(wait_until([&] {
        try {
            (void)s3->filesystem().getattr("/partition-retain.bin");
            return false;
        } catch (...) {
            return true;
        }
    }, 10s));
    REQUIRE(wait_until([&] {
        return !s1.node().retention_store().retained(RetentionClass::data, extent) &&
               !s2.node().retention_store().retained(RetentionClass::data, extent) &&
               !s1.node().local_store().valid(extent) &&
               !s2.node().local_store().valid(extent);
    }, 10s));

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
    // Establish normal metadata state so maintenance is running against a valid
    // accepted branch before we create the deliberately unreachable object.
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
    s1.node().retention_store().retain(RetentionClass::data, id, claim);
    s2.node().retention_store().retain(RetentionClass::data, id, claim);

    // Keep another replica offline while installing a deliberately future/
    // concurrent claim dot which the current branch clock does not dominate.
    // Namespace reachability is absent, but causal GC must not erase this claim.
    s3->stop();
    s3.reset();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));

    REQUIRE(s2.node().local_store().remove(id));
    CHECK(s2.node().retention_store().retained(RetentionClass::data, id));
    CHECK(!s2.node().local_store().valid(id));
    // This direct store mutation simulates corruption detection outside the
    // normal RPC/storage wrappers. In the event-driven scheduler that detector
    // must publish the concrete mutation event; heartbeat cadence is not a
    // maintenance trigger.
    s2.node().notify_storage_mutation();

    // `id` is deliberately absent from namespace/catalogue reachability. The
    // only reason maintenance can know it must restore this physical copy is the
    // durable local retention claim itself.
    REQUIRE(wait_until([&] { return s2.node().local_store().valid(id); }, 5s));
    auto restored = s2.node().local_store().get(id);
    REQUIRE(restored.has_value());
    CHECK(*restored == bytes);
    CHECK(s2.node().retention_store().retained(RetentionClass::data, id));

    s2.stop();
    s1.stop();
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

        // Establish one accepted base on every replica before deliberately
        // partitioning the cluster into two disjoint write-capable pairs.
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
        // Nodes 3+4 never observed /left. They must nevertheless remain
        // writable because they are an arbitrary surviving pair satisfying the
        // configured metadata durability floor.
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

        // Bring back one member of the other pair. The active pair now carries
        // two previously accepted sibling histories. Neither may be discarded:
        // reconciliation must create an accepted descendant of both.
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
        REQUIRE(wait_until([&] {
            try {
                reconcile.repair_once();
                return s2.filesystem().getattr("/left").type == EntryType::directory &&
                       s2.filesystem().getattr("/right").type == EntryType::directory &&
                       s3.filesystem().getattr("/left").type == EntryType::directory &&
                       s3.filesystem().getattr("/right").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        }, 3s));

        REQUIRE(wait_until([&] {
            return s2.node().metadata_replica().accepted_heads().size() == 1 &&
                   s3.node().metadata_replica().accepted_heads().size() == 1;
        }));
        const auto merged = s2.node().metadata_replica().accepted_heads().front();
        CHECK(s2.node().metadata_replica().history_is_ancestor(left_head, merged.hash));
        CHECK(s2.node().metadata_replica().history_is_ancestor(right_head, merged.hash));

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

    // Replica policy is deliberately changed only while the whole cluster is
    // stopped. On restart the the available metadata replicas commit the new DATA policy and
    // object repair converges existing content to the new data replica count.
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

        s1.filesystem().mkdir("/after-grow", 0755, getuid(), getgid());
        MetadataManager m1(s1.node());
        auto snapshot = m1.snapshot();
        CHECK(snapshot.metadata_voters.empty());
        CHECK(snapshot.data_replication == 2);
        CHECK(snapshot.metadata_write_replicas_required == 2);
        CHECK(s2.filesystem().getattr("/after-grow").type == EntryType::directory);

        DistributedStore r1(s1.node());
        DistributedStore r2(s2.node());
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
    // Equal configured capacities give each node an equal placement share.
    // Node 1 is then filled locally: current free space must not change its
    // placement weight, and the missing replica should spill to the fallback.
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
    DistributedStore store(s2.node());
    REQUIRE(store.put(id, data));
    CHECK(!s1.node().local_store().has(id));

    std::array<Service*, 4> services{&s1, &s2, &s3, &s4};
    auto fallback = std::find_if(services.begin(), services.end(), [&](Service* service) {
        return service->node().node_id() == ranked[3].id;
    });
    REQUIRE(fallback != services.end());

    // Foreground put() commits at quorum and does not wait for the full third
    // owner. Placement repair subsequently spills that missing replica to the
    // next deterministic capacity-aware fallback node.
    REQUIRE(wait_until([&] {
        for (auto* service : services) {
            if (!service->node().local_store().has(id))
                continue;
            DistributedStore repair(service->node());
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
    // R=2 is convergence, while the write floor remains W=1. Drive the repair
    // primitive explicitly so this lifecycle test verifies convergence itself
    // rather than depending on the production maintenance scheduler's backoff.
    DistributedStore initial_convergence(s2->node());
    REQUIRE(wait_until([&] {
        initial_convergence.repair_once(16ULL * 1024 * 1024, &objects);
        return std::all_of(objects.begin(), objects.end(),
                           [&](const auto& id) { return s2->node().local_store().has(id); });
    }));

    // Force one normal metadata maintenance pass so node 2 is demonstrably a
    // durable committed-checkpoint witness before the voter is destroyed.
    MetadataManager witness_repair(s2->node());
    witness_repair.repair_once();
    CHECK(s2->node().metadata_replica().committed().generation > 1);

    old_n1 = s1->node().node_id();
    s1->stop();
    s1.reset();

    // Simulate complete loss of node 1: identity, namespace state, cache and
    // every authoritative object are gone. The shared cluster key/config remain.
    std::error_code ec;
    std::filesystem::remove_all(c1.state_path, ec);
    std::filesystem::remove_all(c1.storage_backends.front().path, ec);
    if (!c1.cache.path.empty())
        std::filesystem::remove_all(c1.cache.path, ec);
    std::filesystem::create_directories(c1.storage_backends.front().path);

    // A wiped founder cannot discover the survivor unless it is explicitly
    // given a bootstrap route. This is the same replacement-node operation a
    // real deployment performs after losing local state.
    auto replacement_config = c1;
    replacement_config.bootstrap = {{"127.0.0.1", p2}};
    auto replacement = std::make_unique<Service>(replacement_config, keys);
    replacement->start();
    CHECK(replacement->node().node_id() != old_n1);

    // Wait until the destroyed node has expired from placement membership.
    // Repair before that point may correctly retain the old owner in the R=2
    // preferred set and therefore has no reason to pull every object to the
    // replacement yet.
    REQUIRE(wait_until([&] {
        const auto active = replacement->node().membership().active();
        const bool old_present = std::any_of(active.begin(), active.end(), [&](const auto& node) {
            return node.id == old_n1;
        });
        const bool survivor_present = std::any_of(active.begin(), active.end(), [&](const auto& node) {
            return node.id == s2->node().node_id();
        });
        return !old_present && survivor_present;
    }, 5s));

    REQUIRE(wait_until([&] {
        try {
            return replacement->filesystem().getattr("/media/recovery.bin").size == input.size();
        } catch (...) {
            return false;
        }
    }, 10s));

    // Once the committed namespace is recovered, prove the same bounded repair
    // primitive used by maintenance repopulates the replacement's R=2 ownership
    // from the survivor.
    DistributedStore replacement_convergence(replacement->node());
    REQUIRE(wait_until([&] {
        replacement_convergence.repair_once(16ULL * 1024 * 1024, &objects);
        return std::all_of(objects.begin(), objects.end(), [&](const auto& id) {
            return replacement->node().local_store().has(id);
        });
    }, 10s));

    Bytes output(input.size());
    auto reader = replacement->filesystem().open_read("/media/recovery.bin");
    size_t offset = 0;
    while (offset < output.size()) {
        auto n = reader->read(offset, {output.data() + offset, output.size() - offset});
        REQUIRE(n > 0);
        offset += n;
    }
    CHECK(output == input);

    // Recovery also reconstructs a writable metadata replica view; it is not a
    // read-only salvage mode.
    replacement->filesystem().mkdir("/after-replacement", 0755, getuid(), getgid());
    REQUIRE(wait_until(
        [&] {
            try {
                return s2->filesystem().getattr("/after-replacement").type ==
                       EntryType::directory;
            } catch (...) {
                return false;
            }
        },
        200ms));

    replacement->stop();
    s2->stop();
}

MACHA_HEAVY_TEST("rpc_cluster", test_three_node_cluster) {
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
    c1.storage_packing = c2.storage_packing = c3.storage_packing = c4.storage_packing =
        StoragePackingConfig{0, 0};
    // This lifecycle test deliberately corrupts one replica and immediately
    // requires a healthy peer copy.  Make that synchronous durability
    // requirement explicit; replicas=3 alone is only the convergence target in
    // the 0.18 storage contract.
    c1.min_write_replicas = c2.min_write_replicas = c3.min_write_replicas =
        c4.min_write_replicas = 3;

    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        Service s3(c3, keys);
        s1.start();
        s2.start();
        s3.start();

        REQUIRE(wait_until([&] {
            return s1.node().membership().active().size() >= 3 &&
                   s2.node().membership().active().size() >= 3 &&
                   s3.node().membership().active().size() >= 3;
        }));

        // Genesis has no privileged coordinator in 0.19. Deliberately initiate
        // the virgin namespace from the highest NodeId, which cannot be the
        // historical min-NodeId coordinator, and require the resulting root to
        // become visible on node 1 before continuing the lifecycle test.
        Service* genesis_writer = &s1;
        if (s2.node().node_id() > genesis_writer->node().node_id())
            genesis_writer = &s2;
        if (s3.node().node_id() > genesis_writer->node().node_id())
            genesis_writer = &s3;
        genesis_writer->filesystem().mkdir("/media", 0755, getuid(), getgid());
        REQUIRE(wait_until([&] {
            try {
                return s1.filesystem().getattr("/media").type == EntryType::directory;
            } catch (...) {
                return false;
            }
        }));
        s1.filesystem().create_file("/media/movie.mkv", 0644, getuid(), getgid());
        auto input = pattern(3 * 1024 * 1024 + 12345);
        auto writer = s1.filesystem().open_write("/media/movie.mkv", true);
        size_t offset = 0;
        while (offset < input.size()) {
            size_t n = std::min<size_t>(77777, input.size() - offset);
            CHECK(writer->write(offset, {input.data() + offset, n}) == n);
            offset += n;
        }
        writer->commit();

        // Removing a committed file records an explicit retirement. Reachability
        // GC uses the same live inventory for both tombstone pruning and orphan sweep.
        s1.filesystem().create_file("/media/delete-me.bin", 0644, getuid(), getgid());
        auto delete_data = pattern(131072);
        auto delete_writer = s1.filesystem().open_write("/media/delete-me.bin", true);
        REQUIRE(delete_writer->write(0, delete_data) == delete_data.size());
        delete_writer->commit();
        auto delete_entry = s1.filesystem().getattr("/media/delete-me.bin");
        REQUIRE(delete_entry.extents.size() == 1);
        auto deleted_id = delete_entry.extents.front().id;
        s1.filesystem().unlink("/media/delete-me.bin");
        auto maintenance = s1.filesystem().maintenance_objects();
        CHECK(std::find_if(maintenance.garbage.begin(), maintenance.garbage.end(),
                           [&](const GarbageRef& garbage) { return garbage.id == deleted_id; }) !=
              maintenance.garbage.end());
        CHECK(std::find(maintenance.live.begin(), maintenance.live.end(), deleted_id) ==
              maintenance.live.end());
        auto inventory1 = s1.filesystem().maintenance_objects_cached();
        auto inventory2 = s1.filesystem().maintenance_objects_cached();
        CHECK(inventory1.get() == inventory2.get());
        CHECK(inventory1->metadata_generation != 0);
        CHECK(inventory1->entries >= 2);
        CHECK(inventory1->extents >= 1);
        CHECK(std::is_sorted(inventory1->live.begin(), inventory1->live.end()));
        CHECK(std::adjacent_find(inventory1->live.begin(), inventory1->live.end()) ==
              inventory1->live.end());
        CHECK(std::is_sorted(inventory1->garbage.begin(), inventory1->garbage.end()));
        s1.filesystem().mkdir("/inventory-generation-change", 0755, getuid(), getgid());
        auto inventory3 = s1.filesystem().maintenance_objects_cached();
        CHECK(inventory3->metadata_generation > inventory1->metadata_generation);
        CHECK(inventory3.get() != inventory1.get());

        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/media/movie.mkv").size == input.size();
            } catch (...) {
                return false;
            }
        }));

        // Warm node 2's namespace cache, mutate through node 1, then require
        // visibility before the cache TTL can expire. The local replica update
        // and generation notice are both valid invalidation paths.
        (void)s2.filesystem().getattr("/media");
        s1.filesystem().mkdir("/cache-invalidation", 0755, getuid(), getgid());
        REQUIRE(wait_until(
            [&] {
                try {
                    return s2.filesystem().getattr("/cache-invalidation").type ==
                           EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            200ms));

        auto reader = s2.filesystem().open_read("/media/movie.mkv");
        Bytes output(input.size());
        size_t got = 0;
        while (got < output.size()) {
            size_t n = std::min<size_t>(131072, output.size() - got);
            auto r = reader->read(got, {output.data() + got, n});
            REQUIRE(r > 0);
            got += r;
        }
        CHECK(output == input);

        Bytes slice(333333);
        auto random_reader = s3.filesystem().open_read("/media/movie.mkv");
        auto n = random_reader->read(987654, slice);
        REQUIRE(n == slice.size());
        CHECK(std::equal(slice.begin(), slice.end(), input.begin() + 987654));

        s1.filesystem().mkdir("/media/not-a-file", 0755, getuid(), getgid());
        bool wrong_type_rejected = false;
        try {
            s1.filesystem().rename("/media/movie.mkv", "/media/not-a-file", false);
        } catch (const FsError& error) {
            wrong_type_rejected = error.code() == EISDIR;
        }
        CHECK(wrong_type_rejected);

        // Loss of any one metadata replica must not stop namespace mutations.
        auto failed_replica = s3.node().node_id();
        s3.stop();
        s2.filesystem().mkdir("/survives-one-node-loss", 0755, getuid(), getgid());
        CHECK(s1.filesystem().getattr("/survives-one-node-loss").type == EntryType::directory);

        // Remove a local replica from node 2; reads must transparently fall back to node 1.
        auto entry = s2.filesystem().getattr("/media/movie.mkv");
        REQUIRE(!entry.extents.empty());

        // A damaged encrypted replica must fail authentication, fall back to a
        // healthy peer, and be restored by the bounded scrub/repair path.
        auto damaged = entry.extents.front().id;
        corrupt_object(c2.storage_backends.front().path, damaged);
        DistributedStore corruption_repair(s2.node());
        auto recovered = corruption_repair.get(damaged);
        REQUIRE(recovered.has_value());
        CHECK(object_id(*recovered) == damaged);
        corruption_repair.scrub_once(128ULL * 1024 * 1024);
        for (size_t attempt = 0; attempt < entry.extents.size() + 2; ++attempt)
            corruption_repair.repair_once(128ULL * 1024 * 1024);
        REQUIRE(s2.node().local_store().get(damaged).has_value());

        // Remove a local replica entirely; reads must transparently fall back.
        s2.node().local_store().remove(damaged);
        auto failover_reader = s2.filesystem().open_read("/media/movie.mkv");
        Bytes first(1024 * 1024);
        REQUIRE(failover_reader->read(0, first) == first.size());
        CHECK(std::equal(first.begin(), first.end(), input.begin()));

        // Enable a persistent SSD-style cache on node 2 at runtime. A playback
        // fetch that node 2 should own is retained independently of DHT storage
        // and also promoted back to authoritative storage without another WAN
        // fetch.
        auto cache_config = c2;
        cache_config.cache.path = cluster.path() / "n2-cache";
        cache_config.cache.max_blocks = 8;
        s2.node().reconfigure_local(cache_config);
        s2.node().local_store().remove(damaged);
        DistributedStore playback_store(s2.node());
        auto playback_fetch = playback_store.get(damaged, 0, true);
        REQUIRE(playback_fetch.has_value());
        CHECK(object_id(*playback_fetch) == damaged);
        REQUIRE(wait_until([&] { return s2.node().block_cache().has(damaged); }));
        REQUIRE(wait_until([&] { return s2.node().local_store().has(damaged); }));
        // Prove the cache is genuinely independent: discard the DHT copy again;
        // subsequent reads can still use the persistent cache.
        s2.node().local_store().remove(damaged);
        auto cached_fetch = playback_store.get(damaged, 0, true);
        REQUIRE(cached_fetch.has_value());
        CHECK(*cached_fetch == *playback_fetch);

        // Add a replacement storage node. Once the failed node has expired, the
        // replacement is immediately an eligible metadata replica; no voter-seat
        // reconfiguration is required.
        Service s4(c4, keys);
        s4.start();
        REQUIRE(wait_until([&] {
            auto active = s1.node().membership().active();
            bool has_failed = false;
            bool has_new = false;
            for (const auto& peer : active) {
                has_failed |= peer.id == failed_replica;
                has_new |= peer.id == s4.node().node_id();
            }
            return !has_failed && has_new && active.size() >= 3;
        }));
        // A joining owner pulls its assigned live objects automatically. This
        // no longer depends on an old owner being manually prodded to scan/push.
        REQUIRE(wait_until([&] {
            if (!s4.node().readiness().data_storage_ready)
                return false;
            return s4.node().local_store().has(entry.extents.front().id);
        }));

        MetadataManager repair(s1.node());
        repair.repair_once();
        REQUIRE(wait_until([&] {
            try {
                return s4.filesystem().getattr("/media/movie.mkv").size == input.size();
            } catch (...) {
                return false;
            }
        }));

        // Node 1 can now also disappear: node 2 + replacement node 4 satisfy
        // metadata_min_write_replicas=2 regardless of which nodes they are.
        s1.stop();
        s2.filesystem().mkdir("/after-arbitrary-replica-failover", 0755, getuid(), getgid());
        CHECK(s4.filesystem().getattr("/after-arbitrary-replica-failover").type == EntryType::directory);

        // One surviving node is below metadata_min_write_replicas=2 and cannot publish.
        s4.stop();
        bool refused = false;
        try {
            s2.filesystem().mkdir("/must-not-commit", 0755, getuid(), getgid());
        } catch (...) {
            refused = true;
        }
        CHECK(refused);
        s2.stop();
    }

    // Full process-style restart from persisted state.
    {
        Service s1(c1, keys);
        Service s2(c2, keys);
        Service s3(c3, keys);
        s1.start();
        s2.start();
        s3.start();
        REQUIRE(wait_until([&] { return s2.node().membership().active().size() >= 3; }));
        REQUIRE(wait_until([&] {
            try {
                return s2.filesystem().getattr("/media/movie.mkv").size > 0;
            } catch (...) {
                return false;
            }
        }));
        auto e = s2.filesystem().getattr("/media/movie.mkv");
        CHECK(e.size == 3 * 1024 * 1024 + 12345);
        Bytes tail(65536);
        auto r = s2.filesystem().open_read("/media/movie.mkv");
        REQUIRE(r->read(e.size - tail.size(), tail) == tail.size());
        auto expected = pattern(e.size);
        CHECK(std::equal(tail.begin(), tail.end(), expected.end() - tail.size()));
        s3.stop();
        s2.stop();
        s1.stop();
    }
}

} // namespace

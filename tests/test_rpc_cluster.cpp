// SPDX-License-Identifier: GPL-3.0-or-later
#include "acquisition/cluster_jobs.hpp"
#include "cluster/placement.hpp"
#include "cluster/rpc_refusal.hpp"
#include "fake_cluster_node.hpp"
#include "metadata/namespace_control_store.hpp"
#include "stand_in_peer.hpp"
#include "startup_progress.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "test_backend_support.hpp"
#include "torrent/torrent_coordinator.hpp"

#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

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
    CHECK(store->idle_for(WorkClass::loader) < 5s);
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
    REQUIRE(
        wait_until([&] { return full_store->repair_diagnostics().prompt_skipped_no_room == 1; }));
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
    for (const auto& node :
         capacity_placement_nodes(object_id(data).bytes, bench.node.membership().active(), 3))
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
    bench.node.add_peer(StoreBench::peer(), [&](MessageType type, const Bytes& request, FrameType) {
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
    const auto peer_puts = [&] { return bench.node.calls_of(MessageType::put_object_deferred); };
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
        reply.retained_memory->push_back(
            *memory.try_acquire(WorkClass::viewer, MemoryOwner::rpc_frame, reply.payload.size()));
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
    // Answers "cannot say now" for every id, or refuses the batch as busy.
    std::atomic_bool cannot_say{};
    std::atomic_bool busy{};
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
            if (busy.load())
                return error_reply(503, "storage_busy", "DATA resource admission busy");
            if (cannot_say.load()) {
                Reader reader(request);
                const auto count = reader.u32();
                Writer writer;
                writer.u32(count);
                for (uint32_t i = 0; i < count; ++i)
                    writer.u8(2);
                return {MessageType::have_valid_objects_reply, writer.take()};
            }
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

// The pull has its own share of a repair step: a push pass with more to
// settle than a step allows does not hold the pull back until it ends.
MACHA_FAST_TEST("rpc_cluster", test_a_long_push_pass_does_not_hold_back_the_pull) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    peer.holds_everything = true;
    const auto missing_bytes = pattern(2048, 7);
    const auto missing = object_id(missing_bytes);
    peer.serves = missing_bytes;
    auto live = local_objects(bench, 100, 1024, 60);
    live.push_back(missing);
    std::sort(live.begin(), live.end());
    auto store = bench.store();
    const auto step = store->repair_step(8ULL * 1024 * 1024, 16, live);
    CHECK(step.push_examined < 100);
    CHECK(step.pull_examined > 0);
    CHECK(bench.local.data().has(missing));
}

// A peer that cannot say whether it holds a copy, per id or by refusing the
// batch as busy, is neither counted as keeping it nor sent one; once it can
// say it lacks the copy, the copy is pushed.
MACHA_FAST_TEST("rpc_cluster", test_repair_push_sends_nothing_to_a_peer_that_cannot_say) {
    for (const bool refuses : {false, true}) {
        StoreBench bench([](Config& c) { c.replication = 2; });
        RepairPeer peer(bench);
        (refuses ? peer.busy : peer.cannot_say) = true;
        const auto live = local_objects(bench, 4, 8 * 1024, 50);
        auto store = bench.store();
        const auto step = store->repair_step(8ULL * 1024 * 1024, 16, live);
        CHECK(step.push_examined == live.size());
        CHECK(peer.batch_requests.load() > 0);
        CHECK(peer.puts.load() == 0);
        for (const auto& id : live)
            CHECK(bench.local.data().has(id));

        peer.busy = false;
        peer.cannot_say = false;
        CHECK(wait_until(
            [&] {
                (void)store->repair_step(8ULL * 1024 * 1024, 16, live);
                return peer.puts.load() == live.size();
            },
            10s));
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
        const auto ranked =
            capacity_placement_nodes(object_id(bytes).bytes, bench.node.membership().active(), 3);
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
// What a peer holds, known from its holdings, replaces asking it: an object
// every owner holds settles without a probe or a send, one the peer lacks is
// sent without a probe, and only what is not known is asked about.
MACHA_FAST_TEST("rpc_cluster", test_repair_pushes_from_what_peers_hold_without_asking) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    std::vector<ObjectId> live;
    for (uint8_t i = 0; i < 3; ++i) {
        const auto bytes = pattern(64 * 1024, static_cast<uint8_t>(60 + i));
        REQUIRE(bench.local.data().put(object_id(bytes), bytes));
        live.push_back(object_id(bytes));
    }
    std::sort(live.begin(), live.end());
    constexpr uint64_t budget = 64ULL * 1024 * 1024;
    using Known = std::function<std::optional<bool>(const NodeId&, const ObjectId&)>;

    const Known all_held = [](const NodeId&, const ObjectId&) { return std::optional<bool>(true); };
    const auto settled =
        bench.store()->repair_step(budget, 16, live, {}, 0, {}, std::nullopt, all_held);
    CHECK(settled.push_examined == live.size());
    CHECK(peer.batch_requests.load() == 0);
    CHECK(peer.puts.load() == 0);

    const auto lacking = live[1];
    const Known lacks_one = [&](const NodeId&, const ObjectId& id) {
        return std::optional<bool>(id != lacking);
    };
    auto sender = bench.store();
    for (int step = 0; step < 8 && peer.puts.load() == 0; ++step)
        (void)sender->repair_step(budget, 16, live, {}, 0, {}, std::nullopt, lacks_one);
    CHECK(peer.puts.load() == 1);
    CHECK(peer.batch_requests.load() == 0);

    const Known unknown = [](const NodeId&, const ObjectId&) { return std::optional<bool>(); };
    (void)bench.store()->repair_step(budget, 16, live, {}, 0, {}, std::nullopt, unknown);
    CHECK(peer.batch_requests.load() >= 1);
}

// Given what peers lack, the push considers only that list: it sends what
// is listed and held here, examines nothing else the store holds, passes
// over a listed object this node no longer holds, and an empty list
// examines nothing.
MACHA_FAST_TEST("rpc_cluster", test_repair_pushes_only_what_the_list_names) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    std::vector<ObjectId> live;
    for (uint8_t i = 0; i < 6; ++i) {
        const auto bytes = pattern(64 * 1024, static_cast<uint8_t>(90 + i));
        REQUIRE(bench.local.data().put(object_id(bytes), bytes));
        live.push_back(object_id(bytes));
    }
    std::sort(live.begin(), live.end());
    constexpr uint64_t budget = 64ULL * 1024 * 1024;
    const auto lacking = live[2];
    const std::function<std::optional<bool>(const NodeId&, const ObjectId&)> lacks_one =
        [&](const NodeId&, const ObjectId& id) { return std::optional<bool>(id != lacking); };

    const std::vector<ObjectId> none;
    const auto idle = bench.store()->repair_step(budget, 16, live, {}, 0, {}, std::nullopt,
                                                 lacks_one, std::span<const ObjectId>(none));
    CHECK(idle.push_examined == 0);
    CHECK(peer.puts.load() == 0);

    // Listed and gone from here: passed over, nothing read or sent for it.
    const auto gone = pattern(64 * 1024, 99);
    std::vector<ObjectId> listed{lacking, object_id(gone)};
    std::sort(listed.begin(), listed.end());
    size_t examined = 0;
    for (int step = 0; step < 8 && peer.puts.load() == 0; ++step)
        examined += bench.store()
                        ->repair_step(budget, 16, live, {}, 0, {}, std::nullopt, lacks_one,
                                      std::span<const ObjectId>(listed))
                        .push_examined;
    CHECK(peer.puts.load() == 1);
    CHECK(peer.batch_requests.load() == 0);
    CHECK(examined <= listed.size());
    CHECK(bench.store()->repair_diagnostics().local_unreadable == 0);
}

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

// A settled pull is settled for the list it walked. A list that arrives after
// an empty one, or after the last one was finished while the push still ran,
// is walked by the step that first sees it.
MACHA_FAST_TEST("rpc_cluster", test_the_repair_step_that_first_sees_a_new_pull_list_walks_it) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    peer.holds_everything = true;
    const auto wanted_bytes = pattern(256 * 1024, 43);
    const auto wanted = object_id(wanted_bytes);
    peer.serves = wanted_bytes;

    auto store = bench.store();
    REQUIRE(store->should_own(wanted));
    const auto budget = 64ULL * 1024 * 1024;
    const std::vector<ObjectId> none;
    const std::vector<ObjectId> named{wanted};
    const auto step = [&](const std::vector<ObjectId>& pull, uint64_t version) {
        return store->repair_step(budget, 16, std::nullopt, {}, 0, {},
                                  std::span<const ObjectId>(pull), {},
                                  std::span<const ObjectId>(none), version, 1);
    };

    bool settled = false;
    for (int attempt = 0; attempt < 4 && !settled; ++attempt)
        settled = step(none, 1).complete;
    REQUIRE(settled);
    const auto filled = step(named, 2);
    CHECK(peer.fetches.load() == 1);
    CHECK(bench.local.data().has(wanted));
    CHECK(filled.pull_examined == 1);
}

MACHA_FAST_TEST("rpc_cluster", test_a_pull_list_new_while_the_push_runs_is_not_passed_over) {
    StoreBench bench([](Config& c) { c.replication = 2; });
    RepairPeer peer(bench);
    peer.holds_everything = true;
    const auto first_bytes = pattern(64 * 1024, 44);
    const auto second_bytes = pattern(64 * 1024, 45);
    const auto second = object_id(second_bytes);
    peer.serves = second_bytes;
    REQUIRE(bench.local.data().put(object_id(first_bytes), first_bytes));
    // More to push than one step examines, so the first list's pull finishes
    // while the pass is still open.
    const auto pushing = local_objects(bench, 100, 4 * 1024, 60);

    auto store = bench.store();
    REQUIRE(store->should_own(second));
    const auto budget = 64ULL * 1024 * 1024;
    const std::vector<ObjectId> first_list{object_id(first_bytes)};
    const std::vector<ObjectId> second_list{second};
    const auto step = [&](const std::vector<ObjectId>& pull, uint64_t version) {
        return store->repair_step(budget, 16, std::nullopt, {}, 0, {},
                                  std::span<const ObjectId>(pull), {},
                                  std::span<const ObjectId>(pushing), version, 1);
    };

    const auto opening = step(first_list, 1);
    REQUIRE(!opening.complete);
    (void)step(second_list, 2);
    CHECK(peer.fetches.load() == 1);
    CHECK(bench.local.data().has(second));
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
    bench.node.add_peer(StoreBench::peer(),
                        [bytes](MessageType type, const Bytes& request, FrameType) {
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
    std::optional<MetadataSnapshotView> current() const override {
        return view;
    }
    MetadataSnapshotView local() override {
        return view;
    }
    MetadataSnapshotView converged() override {
        ++surveys;
        return view;
    }
    MetadataSnapshotView converged(const WorkContext&) override {
        return converged();
    }
    uint64_t current_generation() const noexcept override {
        return view.generation;
    }
    uint64_t current_namespace_revision() const noexcept override {
        return 0;
    }
    MetadataRecord record() override {
        ++surveys;
        return {};
    }
    std::optional<MetadataSnapshotView> release_head() const override {
        return view;
    }
    MetadataClusterStatus status() const noexcept override {
        return {};
    }
    MetadataRecord mutate_delta(const std::function<void(MetadataSnapshot&, MetadataDelta&)>&,
                                size_t, std::optional<MetadataMutationIdentity>) override {
        throw std::runtime_error("read-only view");
    }
    bool resolve_conflict(const std::string&, std::string_view) override {
        return false;
    }
    uint64_t conflicts_superseded() const noexcept override {
        return 0;
    }
    uint64_t conflicts_resolved() const noexcept override {
        return 0;
    }
    MetadataHeadStanding head_standing() const noexcept override {
        return {};
    }
    std::optional<std::optional<ObjectId>>
    common_ancestor_catalogue_root(const Hash256&, const Hash256&) const override {
        return {};
    }
    MetadataMutationTiming mutation_timing() const noexcept override {
        return {};
    }
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
    std::filesystem::create_directories(ingest_config.staging_path);
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
        StandInPeer viewer(cluster, "viewer",
                           {.bootstrap = {{"127.0.0.1", p2}},
                            .telemetry =
                                [&](dannan::status::Telemetry& record) {
                                    if (watching.load())
                                        record.set_traffic(
                                            250, {{static_cast<uint8_t>(FrameType::foreground), 0,
                                                   0, 1'300'000, 0}});
                                },
                            .hosts_extents = false});
        viewer.start();
        REQUIRE(wait_until([&] { return s2.node().peer_viewers_active(3000ms); }));
        const auto [id, bytes] = lose_a_copy(1);
        const bool restored = wait_until([&] { return s2.local_state().data().valid(id); }, 10s);
        // The passes that restored it ran with the peer's viewer active.
        // Whether one also waited on the share depends on its budget then.
        const auto paced_by = s2.repair_diagnostics().paced_by;
        watching = false;
        REQUIRE(restored);
        CHECK(*s2.local_state().data().get(id) == bytes);
        CHECK((paced_by & DistributedStore::paced_by_peer_viewer));
    }

    // This node's loader, active throughout.
    std::atomic_bool loading{true};
    std::thread loader([&] {
        while (loading.load()) {
            s2.resources().activity.note(FrameType::loader, 64 * 1024);
            std::this_thread::sleep_for(5ms);
        }
    });
    REQUIRE(wait_until(
        [&] {
            return s2.resources().activity.idle_for(WorkClass::loader) <
                   c2.maintenance.foreground_quiet;
        },
        5s));
    const auto [id, bytes] = lose_a_copy(2);
    const bool restored = wait_until([&] { return s2.local_state().data().valid(id); }, 10s);
    CHECK(s2.resources().activity.idle_for(WorkClass::loader) < c2.maintenance.foreground_quiet);
    CHECK((s2.repair_diagnostics().paced_by & DistributedStore::paced_by_loader));
    loading = false;
    loader.join();
    REQUIRE(restored);
    CHECK(*s2.local_state().data().get(id) == bytes);
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
    REQUIRE(wait_until(
        [&] {
            const auto a0 = n1.rpc_stats().connections_created;
            const auto b0 = n2.rpc_stats().connections_created;
            std::this_thread::sleep_for(120ms);
            return n1.rpc_stats().connections_created == a0 &&
                   n2.rpc_stats().connections_created == b0;
        },
        5s, 0ms));

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

    TestGate blocked;
    StandInPeer peer(cluster, "blocked-peer", {});
    peer.routes.bind(MessageType::get_object, [&](const NodeInfo&, FrameType, const RpcMessage&) {
        blocked.enter_and_wait();
        return RpcMessage{MessageType::object_reply, {}};
    });
    peer.start();
    struct GateOpener {
        TestGate& gate;
        ~GateOpener() {
            gate.open();
        }
    } open_on_exit{blocked};

    const auto endpoint = peer.endpoint();
    auto pending = std::async(std::launch::async, [&] {
        try {
            Bytes object_id(32, 0x7b);
            (void)service.node().call(endpoint, MessageType::get_object, object_id,
                                      FrameType::speculative, call_deadline::object_transfer);
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
        (void)service.node().call(endpoint, MessageType::ping, {}, call_deadline::presence_probe);
    } catch (const std::exception&) {
        retry_rejected = true;
    }
    CHECK(retry_rejected);
    CHECK(Clock::now() - retry_started < 250ms);

    blocked.open();
    peer.stop();
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
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
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

    // Another node asking this one for the object.
    StandInPeer client(fixture.path(), fixture.keyfile(), "client", {});
    client.start();
    Endpoint endpoint{"127.0.0.1", config.port};
    Writer request;
    request.fixed(id.bytes);

    auto blocked_loader = client.call_async(endpoint, MessageType::get_object, request.data(),
                                            FrameType::loader, call_deadline::object_transfer);
    REQUIRE(wait_until([&] { return node.resources.data.stats().loader_waits >= 1; }, 1s));

    // The viewer request uses the reserved DATA credit; fast CONTROL is outside the arbiter.
    auto viewer_started = Clock::now();
    auto viewer = client.call(endpoint, MessageType::get_object, request.data(),
                              FrameType::foreground, 500ms);
    CHECK(viewer.message.type == MessageType::object_reply);
    CHECK(Clock::now() - viewer_started < 200ms);

    auto control_started = Clock::now();
    auto control = client.call(endpoint, MessageType::ping, {}, FrameType::control, 500ms);
    CHECK(control.message.type == MessageType::ok);
    CHECK(Clock::now() - control_started < 200ms);

    // Presence validation reads the whole object, so it goes through
    // speculative DATA admission, not a CONTROL worker.
    auto blocked_validation =
        client.call_async(endpoint, MessageType::have_object, request.data(),
                          FrameType::speculative, call_deadline::presence_probe);
    REQUIRE(blocked_validation.wait_for(scaled(1s)) == std::future_status::ready);
    CHECK(blocked_validation.get().message.type == MessageType::error);
    auto second_control = client.call(endpoint, MessageType::ping, {}, FrameType::control, 500ms);
    CHECK(second_control.message.type == MessageType::ok);
    CHECK(blocked_loader.wait_for(scaled(20ms)) == std::future_status::timeout);

    first.reset();
    REQUIRE(blocked_loader.wait_for(scaled(1s)) == std::future_status::ready);
    CHECK(blocked_loader.get().message.type == MessageType::object_reply);
    CHECK(
        client
            .call(endpoint, MessageType::have_object, request.data(), FrameType::speculative, 500ms)
            .message.type == MessageType::bool_reply);

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
    // The phantom meets the joiner and answers nothing it is asked about metadata.
    StandInPeer stand_in(cluster, "phantom",
                         {.bootstrap = {{"127.0.0.1", config.port}},
                          .id = phantom.id,
                          .failure_domain = phantom.failure_domain});
    stand_in.advertise_storage(phantom.capacity, 0);
    stand_in.advertise(advert::metadata_write_replicas, phantom.metadata_write_replicas_required);
    stand_in.start();
    REQUIRE(wait_until([&] { return active_on(node, phantom.id); }, 15s));
    REQUIRE(node.wait_local_state_ready(10s));

    auto metadata_store = macha::test_support::distributed_store_for(node);
    MetadataManager metadata(node, node.local_state(), node.metadata_server(), *metadata_store);
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

    // Each node answered from its own commit; the two then merge.
    const auto sees = [](Service& service, const std::string& path) {
        return wait_until(
            [&] {
                try {
                    return service.filesystem().getattr(path).type == EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            20s);
    };
    CHECK(sees(s1, "/from-node-2"));
    CHECK(sees(s2, "/from-node-1"));

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

    MetadataManager m1(s1.node(), s1.local_state(), s1.metadata_server(), s1.store());
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
                   s2.local_state().replica().accepted_heads().size() == 1 && !d1.scheduled &&
                   d1.runs_scheduled == d1.runs_completed && !d2.scheduled &&
                   d2.runs_scheduled == d2.runs_completed;
        },
        10s));

    const auto before = s1.metadata_convergence_diagnostics();
    const auto baseline_generation = s2.local_state().replica().committed_generation();
    const auto announcements_before = s2.node().metadata_announcements();
    gate_repair.store(true, std::memory_order_release);

    s2.filesystem().mkdir("/coalesced-0", 0755, getuid(), getgid());
    REQUIRE(repair_gate.wait_for_entries(1, 5s));
    // Held from here: the run that entered the gate is the baseline. Others
    // may have run before the gate was set (a topology event, say).
    const auto claimed = s1.metadata_convergence_diagnostics();
    CHECK(claimed.runs_scheduled >= before.runs_scheduled + 1);
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
    REQUIRE(wait_until([&] { return s1.metadata_server().known_generation() >= final_generation; },
                       5s));
    CHECK(s2.node().metadata_announcements() >= announcements_before + burst + 1);

    // While the run is held the node counts the burst; the pass turns it into
    // demand when it next looks, so nothing more is scheduled meanwhile.
    CHECK(wait_until([&] { return metadata_events() > events_claimed; }, 5s));
    const auto gated = s1.metadata_convergence_diagnostics();
    CHECK(gated.runs_scheduled == claimed.runs_scheduled);
    CHECK(gated.runs_completed == claimed.runs_completed);

    repair_gate.open();
    const bool settled_in_two = wait_until(
        [&] {
            const auto diagnostics = s1.metadata_convergence_diagnostics();
            return !diagnostics.scheduled && diagnostics.runs_completed > claimed.runs_completed &&
                   s1.local_state().replica().committed_generation() == final_generation;
        },
        10s);
    if (!settled_in_two) {
        const auto seen = s1.metadata_convergence_diagnostics();
        std::cerr << "convergence not settled: scheduled=" << seen.scheduled << " runs_scheduled=+"
                  << (seen.runs_scheduled - claimed.runs_scheduled) << " runs_completed=+"
                  << (seen.runs_completed - claimed.runs_completed)
                  << " committed=" << s1.local_state().replica().committed_generation()
                  << " final=" << final_generation << " metadata_events=+"
                  << (metadata_events() - events_claimed) << '\n';
    }
    REQUIRE(settled_in_two);

    // The burst coalesced: far fewer runs than commits, whatever else woke one.
    const auto settled = s1.metadata_convergence_diagnostics();
    CHECK(settled.runs_scheduled == settled.runs_completed);
    CHECK(settled.runs_completed - claimed.runs_completed < burst);
    CHECK(settled.completed_epoch == settled.requested_epoch);
    CHECK(settled.latest_generation >= final_generation);
    CHECK(!settled.scheduled);
    CHECK(s1.filesystem().getattr("/coalesced-32").type == EntryType::directory);

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_failed_convergence_retries_once_per_burst_of_events) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 =
        config_for(cluster.path() / "retry-burst-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 =
        config_for(cluster.path() / "retry-burst-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.write_copies = c2.write_copies = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;
    c1.ingest.enabled = c2.ingest.enabled = false;
    c1.torrent.enabled = c2.torrent.enabled = false;
    // A failed run's own backoff is 30 s, longer than the whole test.
    REQUIRE(maintenance_background_interval(c1.maintenance) >= 30s);
    c1.maintenance.convergence_settle = 1s;
    c1.maintenance.convergence_max_delay = 20s;

    std::atomic_bool failing{};
    std::atomic_int failed{};
    Service s1(c1, keys, test_durability_window, {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && failing.load()) {
            failed.fetch_add(1);
            throw std::runtime_error("injected convergence failure");
        }
    });
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    (void)s1.filesystem();
    (void)s2.filesystem();
    REQUIRE(wait_until(
        [&] {
            const auto d1 = s1.metadata_convergence_diagnostics();
            return s1.node().membership().active().size() >= 2 &&
                   s1.local_state().replica().committed_generation() > 1 &&
                   s1.local_state().replica().committed().hash ==
                       s2.local_state().replica().committed().hash &&
                   !d1.scheduled && d1.runs_scheduled == d1.runs_completed;
        },
        10s));

    // A stream of the peer's changes while runs fail: the first change runs
    // and fails; the rest are one burst, retried after it, whatever else
    // wakes the pass meanwhile.
    failing.store(true);
    const auto stream_start = Clock::now();
    for (int i = 0; Clock::now() - stream_start < 5s; ++i) {
        s2.filesystem().mkdir("/retry-burst-" + std::to_string(i), 0755, getuid(), getgid());
        std::this_thread::sleep_for(200ms);
    }
    CHECK(failed.load() == 1);

    // After the last change the retry runs once the settle has passed, not
    // when the 30 s backoff ends.
    failing.store(false);
    const auto head = s2.local_state().replica().committed();
    CHECK(wait_until(
        [&] {
            const auto d1 = s1.metadata_convergence_diagnostics();
            return !d1.scheduled && s1.local_state().replica().committed().hash == head.hash;
        },
        4s));
    CHECK(failed.load() == 1);

    s2.stop();
    s1.stop();
}

// A node stopping while an ingest copies: the job is interrupted, not failed,
// and resumes after the restart.
MACHA_TEST("rpc_cluster", test_an_ingest_interrupted_by_its_node_stopping_resumes) {
    TestCluster cluster(ConfigProfile::isolated);
    TempDir sources;
    const auto source_dir = std::filesystem::canonical(sources.path());
    constexpr size_t file_bytes = 96 * 1024 * 1024;
    const auto source = source_dir / "Interrupted Movie 2024.mkv";
    {
        std::ofstream out(source, std::ios::binary);
        const auto bytes = pattern(file_bytes);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    auto config = cluster.node_config("ingest-stop", free_port());
    // ingest.enabled requires the scanner; the movie root names the
    // destination, and no provider is asked.
    config.catalogue.scanner.enabled = true;
    config.catalogue.scanner.movies.enabled = true;
    config.catalogue.scanner.movies.roots = {"/Movies"};
    config.catalogue.scanner.movies.tmdb.enabled = false;
    config.catalogue.scanner.tv.enabled = false;
    config.ingest.enabled = true;
    config.ingest.source_roots = {source_dir};

    // Each step of the stop is held, so whatever the stop cancels has time to
    // fail before the next step.
    ServiceInstruments instruments;
    instruments.lifecycle = [](std::string_view event) {
        if (event.starts_with("cancel-io") || event.starts_with("request_stop"))
            std::this_thread::sleep_for(100ms);
    };
    std::string id;
    {
        Service service(config, cluster.keys(), test_durability_window, {}, {}, {}, instruments);
        service.start();
        REQUIRE(wait_until([&] { return service.ready(); }, 30s));
        id = service.ingest().submit_path(source, "filesystem");
        REQUIRE(wait_until(
            [&] {
                const auto job = service.ingest().job(id);
                return job && job->state == IngestJobState::importing && job->bytes_completed > 0;
            },
            20s));
        service.stop();
    }

    Service service(config, cluster.keys(), test_durability_window);
    service.start();
    REQUIRE(wait_until([&] { return service.ready(); }, 30s));
    const auto restored = service.ingest().job(id);
    REQUIRE(restored.has_value());
    CHECK(restored->state != IngestJobState::failed);
    CHECK(restored->error.empty());
    REQUIRE(wait_until(
        [&] {
            const auto job = service.ingest().job(id);
            return job && (job->state == IngestJobState::completed ||
                           job->state == IngestJobState::cataloguing);
        },
        60s));
    const auto done = service.ingest().job(id);
    Bytes copied(file_bytes);
    auto reader = service.filesystem().open_read(done->files.front().destination_path);
    size_t read = 0;
    while (read < copied.size()) {
        const auto got =
            reader->read(read, std::span<uint8_t>(copied.data() + read, copied.size() - read));
        if (!got)
            break;
        read += got;
    }
    copied.resize(read);
    CHECK(copied == pattern(file_bytes));
    service.stop();
}

// One ingest job read from a node that does not own it carries the owner's
// catalogue items; with the owner unreachable they are null, not empty.
MACHA_TEST("rpc_cluster", test_an_ingest_job_read_elsewhere_carries_its_owners_catalogue_items) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    TempDir sources;
    const auto source_dir = std::filesystem::canonical(sources.path());
    const auto source = source_dir / "Elsewhere Movie 2024.mkv";
    {
        std::ofstream out(source, std::ios::binary);
        const auto bytes = pattern(256 * 1024);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "detail-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "detail-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* c : {&c1, &c2}) {
        c->replication = 2;
        c->write_copies = 1;
        c->metadata_write_copies = 1;
        c->catalogue.scanner.enabled = true;
        c->catalogue.scanner.movies.enabled = true;
        c->catalogue.scanner.movies.roots = {"/Movies"};
        c->catalogue.scanner.movies.tmdb.enabled = false;
        c->catalogue.scanner.tv.enabled = false;
        c->catalogue.api.enabled = false;
        c->ingest.enabled = true;
        c->ingest.source_roots = {source_dir};
        c->torrent.enabled = false;
    }
    auto s1 = std::make_unique<Service>(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1->start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1->node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    const auto id = s1->ingest().submit_path(source, "filesystem");
    REQUIRE(wait_until(
        [&] {
            const auto job = s1->ingest().job(id);
            return job && job->catalogue_total > 0;
        },
        30s));
    s2.cluster_jobs().refresh_now();

    const auto detail = [&](Service& service) {
        HttpRequest request;
        request.method = "GET";
        request.path = "/api/v1/ingest/jobs/" + id;
        const auto response = service.acquisition_api().handle(request);
        REQUIRE(response.status == 200);
        return Json::parse(std::string(response.body.begin(), response.body.end()));
    };
    const auto own = detail(*s1);
    const auto* own_items = own.find("catalogue")->find("items");
    REQUIRE(own_items != nullptr);
    REQUIRE(own_items->isArray());
    REQUIRE(!own_items->asArray().empty());
    const auto elsewhere = detail(s2);
    CHECK(elsewhere.find("node_id")->asString() == to_string(s1->node().node_id()));
    const auto* items = elsewhere.find("catalogue")->find("items");
    REQUIRE(items != nullptr);
    CHECK(items->dump() == own_items->dump());

    s1->stop();
    s1.reset();
    REQUIRE(wait_until([&] { return s2.node().membership().active().size() == 1; }, 10s));
    const auto unreachable = detail(s2);
    const auto* unknown = unreachable.find("catalogue")->find("items");
    REQUIRE(unknown != nullptr);
    CHECK(unknown->isNull());
    s2.stop();
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
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        MetadataDelta delta;
        delta.upsert_entries[path] = entry;
        auto snapshot = macha::test_support::with_namespace_delta(
            decode_snapshot(base.payload), service.local_state().control(), service.store(), delta);
        ++snapshot.mutation_sequences[service.node().node_id()];

        MetadataRecord sibling;
        sibling.generation = base.generation + 1;
        sibling.previous = base.hash;
        sibling.payload = encode_snapshot_v14(snapshot);
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
    const auto metadata_only = [](Service& service) {
        return service.resources().events.count(NodeEvent::metadata);
    };
    const auto before_metadata1 = metadata_only(s1);
    const auto before_metadata2 = metadata_only(s2);
    const auto announcements_before_duplicate = s2.node().metadata_announcements();
    MetadataAcceptance duplicate;
    duplicate.generation = right.generation;
    duplicate.hash = right.hash;
    duplicate.required = 1;
    duplicate.replicas = {s2.node().node_id()};
    REQUIRE(s2.metadata_server().accept_commit(duplicate));
    CHECK(s2.node().metadata_announcements() == announcements_before_duplicate);
    std::this_thread::sleep_for(100ms);
    // Topology events (a peer's adverts) may arrive meanwhile; the duplicate
    // raises no metadata event.
    CHECK(metadata_only(s1) == before_metadata1);
    CHECK(metadata_only(s2) == before_metadata2);

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

    // The first write after a reconciliation extends the merge commit and does
    // not carry its topology. Under a tree either body is compact; the store
    // keeps the smaller.
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

    s2.stop();
    s1.stop();
}

// A second accepted head from a history this node has never seen needs no
// ancestor in common: the two heads are merged from what they hold, and
// neither's entries are lost.
MACHA_TEST("rpc_cluster", test_a_head_from_an_unknown_history_is_merged) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto config =
        config_for(cluster.path() / "unknown-history", cluster.keyfile(), free_port(), {});
    config.metadata_cache = std::chrono::milliseconds(0);
    BareNode node(config, keys);
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));

    auto metadata_store = macha::test_support::distributed_store_for(node);
    MetadataManager metadata(node, node.local_state(), node.metadata_server(), *metadata_store);
    const auto directory = [] {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        return entry;
    };
    metadata.mutate_delta([&](MetadataSnapshot&, MetadataDelta& delta) {
        delta.upsert_entries["/mine"] = directory();
    });
    const auto mine = node.metadata_replica().committed();
    REQUIRE(mine.generation > 1);

    // Another author's namespace, with nothing of this node's in its clock.
    const MetadataDot theirs{random_node_id(), 9};
    // Its namespace is this node's with "/mine" gone and "/theirs" added.
    auto made = directory();
    stamp_entry_provenance(made, "/theirs", nullptr, theirs);
    MetadataDelta foreign_delta;
    foreign_delta.erase_entries.push_back("/mine");
    foreign_delta.upsert_entries["/theirs"] = made;
    auto foreign_snapshot = macha::test_support::with_namespace_delta(
        decode_snapshot(mine.payload), node.local_state().control(), *metadata_store,
        foreign_delta);
    foreign_snapshot.mutation_sequences.clear();
    foreign_snapshot.legacy_clock.emplace();
    foreign_snapshot.mutation_sequences[theirs.author] = theirs.sequence;
    MetadataRecord foreign;
    foreign.generation = mine.generation + 7;
    foreign.previous = sha256(pattern(64, 201));
    foreign.payload = encode_snapshot_v14(foreign_snapshot);
    foreign.hash = metadata_hash(foreign.generation, foreign.previous, foreign.payload);
    REQUIRE(node.metadata_replica().store_commit(foreign));
    MetadataAcceptance acceptance;
    acceptance.generation = foreign.generation;
    acceptance.hash = foreign.hash;
    acceptance.required = 1;
    acceptance.replicas = {theirs.author};
    REQUIRE(node.metadata_server().accept_commit(acceptance));
    REQUIRE(node.metadata_replica().accepted_heads().size() == 2);

    const auto holds = [&](const MetadataSnapshot& snapshot, std::string_view path) {
        return macha::test_support::namespace_holds(snapshot, node.local_state().control(),
                                                    *metadata_store, path);
    };
    const auto merged = metadata.snapshot();
    CHECK(holds(merged, "/mine"));
    CHECK(holds(merged, "/theirs"));
    CHECK(merged.mutation_sequences.at(theirs.author) == theirs.sequence);
    CHECK(node.metadata_replica().accepted_heads().size() == 1);
    CHECK(metadata.head_standing().set_aside == 0);

    metadata.mutate_delta([&](MetadataSnapshot&, MetadataDelta& delta) {
        delta.upsert_entries["/more"] = directory();
    });
    CHECK(holds(metadata.snapshot(), "/theirs"));
    CHECK(holds(metadata.snapshot(), "/more"));
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
            return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
        },
        10s));

    auto metadata1_store = macha::test_support::distributed_store_for(n1);
    MetadataManager metadata1(n1, n1.local_state(), n1.metadata_server(), *metadata1_store);
    // One mutation, so the siblings fork from a real post-genesis record.
    REQUIRE(macha::test_support::retry_while_not_ready([&] {
        metadata1.mutate_delta([](MetadataSnapshot&, MetadataDelta& delta) {
            FsEntry entry;
            entry.type = EntryType::directory;
            entry.mode = 0755;
            entry.uid = getuid();
            entry.gid = getgid();
            delta.upsert_entries["/base-concurrent"] = entry;
        });
    }));
    REQUIRE(n1.metadata_replica().committed_generation() > 1);

    // A two-head divergence on node 1 alone, from a locally authored sibling.
    const auto base = n1.metadata_replica().committed();
    auto make_sibling = [&](BareNode& node, const std::string& path) {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        MetadataDelta delta;
        delta.upsert_entries[path] = entry;
        auto snapshot = macha::test_support::with_namespace_delta(
            decode_snapshot(base.payload), node.local_state().control(), *metadata1_store, delta);
        ++snapshot.mutation_sequences[node.node_id()];

        MetadataRecord sibling;
        sibling.generation = base.generation + 1;
        sibling.previous = base.hash;
        sibling.payload = encode_snapshot_v14(snapshot);
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
        ~ReleaseGate() {
            gate.open();
        }
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
    REQUIRE(
        wait_until([&] { return s2.local_state().replica().committed().hash == final.hash; }, 10s));

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
    // Bounded, not linear: a rejoin's topology events may wake a run or two
    // more, never one per commit.
    CHECK(diagnostics.runs_completed < burst);
    const auto heads = s3->local_state().replica().accepted_heads();
    REQUIRE(heads.size() == 1);
    CHECK(heads.front().hash == final.hash);
    CHECK(s3->local_state().replica().history_contains(final.hash));

    // A tree-backed record names only its root, so the head stands alone: the
    // replica takes it whole instead of replaying the burst.
    const auto transfer1 = s1.metadata_manager().history_transfer_diagnostics();
    const auto transfer2 = s2.metadata_manager().history_transfer_diagnostics();
    CHECK(std::max(transfer1.peak_in_flight, transfer2.peak_in_flight) <= 8);

    s3->stop();
    s2.stop();
    s1.stop();
}

MACHA_HEAVY_TEST("rpc_cluster",
                 test_an_ingest_on_a_node_with_no_namespace_yet_resumes_when_it_has_one) {
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
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    IngestConfig ingest_config;
    ingest_config.enabled = true;
    ingest_config.staging_path = cluster.path() / "staging";
    std::filesystem::create_directories(ingest_config.staging_path);
    ingest_config.source_roots = {root};
    ingest_config.blocked_retry = 200ms;
    IngestManager ingest(s1.node(), s1.filesystem(), s1.catalogue_hints(), ingest_config);
    const auto id = ingest.submit_path(root);
    ingest.start();

    REQUIRE(wait_until(
        [&] {
            const auto job = ingest.job(id);
            return job && job->state == IngestJobState::blocked &&
                   job->error_code == "metadata_unavailable";
        },
        30s));
    // Retried, and still waiting rather than failed.
    std::this_thread::sleep_for(1s);
    CHECK(ingest.job(id)->state != IngestJobState::failed);

    s2 = std::make_unique<Service>(c2, keys, test_durability_window);
    s2->start();
    REQUIRE(wait_until(
        [&] {
            const auto job = ingest.job(id);
            return job && job->files_total == 1 && job->files_completed == 1;
        },
        60s));
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
    // The accepted file reference installed a DATA claim here at once, and
    // on the holders present once the commit reached them.
    CHECK(s1.local_state().retention().retained(RetentionClass::data, extent));
    REQUIRE(s1.metadata_manager().wait_replicated(20s));
    CHECK(wait_until(
        [&] { return s2.local_state().retention().retained(RetentionClass::data, extent); }, 20s));
    CHECK(wait_until(
        [&] { return s3->local_state().retention().retained(RetentionClass::data, extent); }, 20s));

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
    REQUIRE(s1.metadata_manager().wait_replicated(20s));
    CHECK(wait_until([&] { return dot(s2) == touched; }, 20s));

    // The returning holder learns the change from the head.
    s3 = std::make_unique<Service>(c3, keys, test_durability_window);
    s3->start();
    REQUIRE(wait_until(
        [&] {
            try {
                return (s3->filesystem().getattr("/retained.bin").mode & 0777U) == 0600U;
            } catch (...) {
                return false;
            }
        },
        30s));

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
    REQUIRE(
        retry_while_not_ready([&] { s1.filesystem().mkdir("/base", 0755, getuid(), getgid()); }));
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
        MetadataManager initial_repair(s1.node(), s1.local_state(), s1.metadata_server(),
                                       s1.store());
        // A round in which a reachable peer did not take the head says so and
        // is run again.
        REQUIRE(retry_while_not_ready([&] { initial_repair.repair_once(); }));
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

        MetadataManager reconcile(s2.node(), s2.local_state(), s2.metadata_server(), s2.store());
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
        MetadataManager m1(s1.node(), s1.local_state(), s1.metadata_server(), s1.store());
        auto snapshot = m1.snapshot();
        CHECK(snapshot.metadata_voters.empty());
        CHECK(snapshot.data_replication == 2);
        CHECK(wait_until(
            [&] {
                try {
                    return s2.filesystem().getattr("/after-grow").type == EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            20s));

        DistributedStore r1(s1.node(), s1.local_state(), s1.resources().activity,
                            s1.resources().data, s1.resources().memory, s1.resources().events);
        DistributedStore r2(s2.node(), s2.local_state(), s2.resources().activity,
                            s2.resources().data, s2.resources().memory, s2.resources().events);
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
        MetadataManager m2(s2.node(), s2.local_state(), s2.metadata_server(), s2.store());
        auto snapshot = m2.snapshot();
        CHECK(snapshot.metadata_voters.empty());
        CHECK(snapshot.data_replication == 1);
        CHECK(snapshot.metadata_write_replicas_required == 1);
        CHECK(wait_until(
            [&] {
                try {
                    return s1.filesystem().getattr("/after-shrink").type == EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            20s));

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
                                         s2->resources().data, s2->resources().memory,
                                         s2->resources().events);
    REQUIRE(wait_until([&] {
        initial_convergence.repair_once(16ULL * 1024 * 1024, objects);
        return std::all_of(objects.begin(), objects.end(),
                           [&](const auto& id) { return s2->local_state().data().has(id); });
    }));

    // One metadata pass makes node 2 a durable checkpoint witness first.
    MetadataManager witness_repair(s2->node(), s2->local_state(), s2->metadata_server(),
                                   s2->store());
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
    DistributedStore replacement_convergence(
        replacement->node(), replacement->local_state(), replacement->resources().activity,
        replacement->resources().data, replacement->resources().memory,
        replacement->resources().events);
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
        20s));

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
            auto config =
                i == 0 ? config_for(dir, cluster.keyfile(), first)
                       : config_for(dir, cluster.keyfile(), free_port(), {{"127.0.0.1", first}});
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
    Service& operator[](size_t i) {
        return *nodes[i];
    }
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

// A commit is answered by the node that made it; the others have it once it
// reaches them.
bool becomes_directory(Service& node, const std::string& path) {
    return wait_until([&] { return is_directory(node, path); }, 20s);
}

bool file_arrives(Service& node, const std::string& path, uint64_t size) {
    return wait_until(
        [&] {
            try {
                return node.filesystem().getattr(path).size == size;
            } catch (const std::exception&) {
                return false;
            }
        },
        20s);
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

// Any node may found a virgin namespace (here the highest NodeId). The node
// that commits sees its commit when the call returns; every other node has
// it once it reaches them. A file written through one node reads back whole
// through a second and at an arbitrary offset through a third.
MACHA_HEAVY_TEST("rpc_cluster", test_a_durable_trio_founds_commits_and_reads_through_every_node) {
    DurableTrio trio;
    size_t founder = 0;
    for (size_t i = 1; i < 3; ++i)
        if (trio[i].node().node_id() > trio[founder].node().node_id())
            founder = i;
    trio[founder].filesystem().mkdir("/media", 0755, getuid(), getgid());
    CHECK(is_directory(trio[founder], "/media"));
    for (size_t i = 0; i < 3; ++i)
        REQUIRE(becomes_directory(trio[i], "/media"));

    const auto writer = (founder + 1) % 3;
    trio[writer].filesystem().mkdir("/media/new", 0755, getuid(), getgid());
    CHECK(is_directory(trio[writer], "/media/new"));
    for (size_t i = 0; i < 3; ++i)
        REQUIRE(becomes_directory(trio[i], "/media/new"));

    const auto input = pattern(3 * 1024 * 1024 + 12345);
    (void)trio.write(0, "/media/movie.mkv", input);
    REQUIRE(file_arrives(trio[1], "/media/movie.mkv", input.size()));
    REQUIRE(file_arrives(trio[2], "/media/movie.mkv", input.size()));
    CHECK(read_whole(trio[1], "/media/movie.mkv", input.size()) == input);
    Bytes slice(333333);
    auto reader = trio[2].filesystem().open_read("/media/movie.mkv");
    REQUIRE(reader->read(987654, slice) == slice.size());
    CHECK(std::equal(slice.begin(), slice.end(), input.begin() + 987654));
}

// Three nodes: losing one leaves the other two committing, each change
// reaching the other; a lone survivor goes on committing.
MACHA_HEAVY_TEST("rpc_cluster", test_metadata_commits_with_whichever_nodes_remain) {
    DurableTrio trio(2, 1);
    trio[0].filesystem().mkdir("/media", 0755, getuid(), getgid());
    REQUIRE(becomes_directory(trio[1], "/media"));
    trio.stop(2);
    trio[1].filesystem().mkdir("/after-one-loss", 0755, getuid(), getgid());
    CHECK(is_directory(trio[1], "/after-one-loss"));
    CHECK(becomes_directory(trio[0], "/after-one-loss"));
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

    const auto walked = fs.walk_namespace_references();
    CHECK(std::find(walked.extents.begin(), walked.extents.end(), doomed) == walked.extents.end());

    const auto first = fs.maintenance_objects_cached();
    CHECK(std::any_of(first->garbage.begin(), first->garbage.end(),
                      [&](const GarbageRef& garbage) { return garbage.id == doomed; }));
    CHECK(!first->referenced_extents.get(doomed).has_value());
    CHECK(fs.maintenance_objects_cached().get() == first.get());
    CHECK(first->metadata_generation != 0);
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
        auto config = i == 0
                          ? config_for(dir, cluster.keyfile(), first)
                          : config_for(dir, cluster.keyfile(), free_port(), {{"127.0.0.1", first}});
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

// Calls from a node to a peer held unanswered, with idle_for() advancing as for a dead
// link, until released: the node's call fault (ServiceInstruments::call_fault).
class StalledCalls {
    mutable Mutex mutex_;
    std::map<NodeId, std::optional<MessageType>> stalled_ MACHA_GUARDED_BY(mutex_);
    std::vector<std::shared_ptr<std::promise<RpcReply>>> held_ MACHA_GUARDED_BY(mutex_);

  public:
    NodeRuntime::CallFault fault() {
        return [this](const NodeInfo& peer, MessageType type) -> std::optional<AsyncRpc> {
            Lock lock(mutex_);
            const auto found = stalled_.find(peer.id);
            if (found == stalled_.end() || (found->second && *found->second != type))
                return std::nullopt;
            auto promise = std::make_shared<std::promise<RpcReply>>();
            held_.push_back(promise);
            const auto started = Clock::now();
            std::weak_ptr<std::promise<RpcReply>> weak = promise;
            auto fail = [weak] {
                if (auto held = weak.lock()) {
                    try {
                        held->set_exception(std::make_exception_ptr(
                            std::runtime_error("RPC cancelled: the peer is stalled")));
                    } catch (const std::future_error&) {
                    }
                }
            };
            return AsyncRpc(promise->get_future(), fail, fail, {}, [started] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                             started);
            });
        };
    }
    // Calls to `peer` (every one, or only `message`) are held from now.
    void stall(const NodeId& peer, std::optional<MessageType> message = {}) {
        Lock lock(mutex_);
        stalled_[peer] = message;
    }
    // Ends the stall and fails every held call, as a timed-out link would.
    void release(const NodeId& peer) {
        std::vector<std::shared_ptr<std::promise<RpcReply>>> held;
        {
            Lock lock(mutex_);
            stalled_.erase(peer);
            held.swap(held_);
        }
        for (auto& promise : held) {
            try {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("RPC failed: the stall was released")));
            } catch (const std::future_error&) {
            }
        }
    }
    // Calls held since the last release().
    size_t stalled_calls() const {
        Lock lock(mutex_);
        return held_.size();
    }
};

// Needs the libmacha-torrent plugin, built only when libtorrent is found.
#ifdef MACHA_TEST_PLUGIN_DIR
MACHA_TEST("rpc_cluster",
           test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes) {
    // repair_once() runs its per-peer fan-out without mutation_mutex_: with
    // the has_metadata_history_entry probe to a silent peer held, a foreground
    // write does not wait.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 =
        config_for(cluster.path() / "silent-repair-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 =
        config_for(cluster.path() / "silent-repair-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;

    auto links = std::make_shared<StalledCalls>();
    ServiceInstruments instruments;
    instruments.call_fault = links->fault();
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

    auto links = std::make_shared<StalledCalls>();
    ServiceInstruments instruments;
    instruments.call_fault = links->fault();
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
    // The commit is claimed here before the caller is answered; the peer's
    // claim is the replicator's, and it gives up on a peer that never answers.
    const auto alone = s1.metadata_manager().snapshot().catalogue_root;
    REQUIRE(alone.has_value());
    CHECK(s1.local_state().retention().retained(RetentionClass::control, *alone));
    REQUIRE(s1.metadata_manager().wait_replicated(30s));
    CHECK(wait_until([&] { return links->stalled_calls() >= 1; }, 10s));
    CHECK(!s2.local_state().retention().retained(RetentionClass::control, *alone));

    links->release(peer);
    item.title = "Claimed twice";
    (void)s1.catalogue().upsert(item);
    const auto root = s1.metadata_manager().snapshot().catalogue_root;
    REQUIRE(root.has_value());
    CHECK(*root != *alone);
    CHECK(s1.local_state().retention().retained(RetentionClass::control, *root));
    REQUIRE(s1.metadata_manager().wait_replicated(30s));
    CHECK(wait_until(
        [&] { return s2.local_state().retention().retained(RetentionClass::control, *root); },
        30s));

    s2.stop();
    s1.stop();
}

// A catalogue conflict whose alternatives merge to the root already in place
// is decided by one pass: the conflict goes, though the root does not change.
MACHA_TEST("rpc_cluster", test_a_catalogue_conflict_decided_for_the_root_in_place_is_removed) {
    TestService fixture("catalogue-conflict");
    fixture.config().replication = 1;
    fixture.config().metadata_write_copies = 1;
    auto& service = fixture.start();
    CatalogueItem item;
    item.id = "movie:standing";
    item.kind = CatalogueKind::movie;
    item.title = "Standing";
    (void)service.catalogue().upsert(item);
    const auto root = service.metadata_manager().snapshot().catalogue_root;
    REQUIRE(root.has_value());

    MetadataConflict conflict;
    conflict.kind = MetadataConflictKind::catalogue_root;
    conflict.key = "catalogue_root";
    conflict.base_catalogue_root = root;
    conflict.left_catalogue_root = root;
    conflict.right_catalogue_root = root;
    const auto id = metadata_conflict_id(conflict);
    service.metadata_manager().mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        snapshot.conflicts[id] = conflict;
        delta.replace_conflicts = snapshot.conflicts;
    });
    REQUIRE(service.metadata_manager().snapshot().conflicts.contains(id));
    const auto before = service.local_state().replica().committed_generation();

    service.catalogue().repair_once();
    const auto after = service.metadata_manager().snapshot();
    CHECK(!after.conflicts.contains(id));
    CHECK(after.catalogue_root == root);
    CHECK(service.local_state().replica().committed_generation() > before);
    // With nothing left to decide, a further pass commits nothing.
    const auto settled = service.local_state().replica().committed_generation();
    service.catalogue().repair_once();
    CHECK(service.local_state().replica().committed_generation() == settled);
}

// A peer that stops answering commit calls costs a caller nothing: the commit
// is answered from this node. The replicator waits for the peer once, for the
// stall time, and does not ask it again until it has had time to be dropped
// or to recover; then repair brings it up to date.
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

    auto links = std::make_shared<StalledCalls>();
    ServiceInstruments instruments;
    instruments.call_fault = links->fault();
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
        return wait_until(
            [&] {
                try {
                    return service.filesystem().getattr(path).type == EntryType::directory;
                } catch (...) {
                    return false;
                }
            },
            20s);
    };
    REQUIRE(sees(s2, "/established"));

    const auto peer = s2.node().node_id();
    links->stall(peer, MessageType::accept_metadata_commit);
    s1.filesystem().mkdir("/while-stalled", 0755, getuid(), getgid());
    // Answered while the peer's call has not returned.
    CHECK(s1.filesystem().getattr("/while-stalled").type == EntryType::directory);
    REQUIRE(s1.metadata_manager().wait_replicated(scaled(5s)));
    const auto asked = links->stalled_calls();
    CHECK(asked >= 1);
    s1.filesystem().mkdir("/not-asked", 0755, getuid(), getgid());
    CHECK(s1.filesystem().getattr("/not-asked").type == EntryType::directory);
    REQUIRE(s1.metadata_manager().wait_replicated(scaled(5s)));
    CHECK(links->stalled_calls() == asked);

    links->release(peer);
    CHECK(sees(s2, "/while-stalled"));
    CHECK(sees(s2, "/not-asked"));

    s2.stop();
    s1.stop();
}

MACHA_TEST("rpc_cluster", test_every_nodes_unmatched_files_are_listed_and_handled_anywhere) {
    // A scan's unmatched files stay on the node that scanned. Node 2, which
    // never scanned, lists node 1's and handles them; a file handled on one
    // node leaves both lists.
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 =
        config_for(cluster.path() / "unmatched-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 =
        config_for(cluster.path() / "unmatched-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.write_copies = c2.write_copies = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 1;
    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));
    REQUIRE(wait_metadata_writable(s1));

    auto& fs1 = s1.filesystem();
    fs1.mkdir("/Movies", 0755, getuid(), getgid());
    write_file(fs1, "/Movies/matched.mkv", pattern(4096, 1));
    write_file(fs1, "/Movies/deleted.mkv", pattern(4096, 2));
    const auto matched_media = file_media_id(fs1.getattr("/Movies/matched.mkv"));
    const auto deleted_media = file_media_id(fs1.getattr("/Movies/deleted.mkv"));
    const auto matched_id =
        s1.catalogue_hints().put_unmatched("/Movies/matched.mkv", matched_media);
    const auto deleted_id =
        s1.catalogue_hints().put_unmatched("/Movies/deleted.mkv", deleted_media);
    REQUIRE(wait_until(
        [&] {
            try {
                return s2.filesystem().getattr("/Movies/deleted.mkv").size == 4096;
            } catch (const FsError&) {
                return false;
            }
        },
        10s));

    const auto handle = [](Service& service, std::string method, std::string path,
                           std::string body = {}) {
        HttpRequest request;
        request.method = std::move(method);
        request.path = std::move(path);
        request.body = Bytes(body.begin(), body.end());
        return service.manage_api().handle(request);
    };
    const auto listed = [&](Service& service) {
        const auto response = handle(service, "GET", "/api/v1/manage/unmatched");
        REQUIRE(response.status == 200);
        const auto parsed = Json::parse(
            std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
        std::set<std::string> ids;
        for (const auto& item : parsed.find("items")->asArray())
            ids.insert(item.find("id")->asString());
        return ids;
    };

    CHECK(s2.catalogue_hints().list().empty());
    // Node 2 asks only a peer whose telemetry names a release that answers.
    REQUIRE(wait_until(
        [&] {
            return runs_at_least(s2.node().telemetry().all(), s1.node().node_id(),
                                 ClusterUnmatchedView::first_version);
        },
        10s));
    s2.cluster_unmatched().refresh_now();
    CHECK((listed(s2) == std::set<std::string>{matched_id, deleted_id}));
    CHECK((listed(s1) == std::set<std::string>{matched_id, deleted_id}));

    // Matched on node 2 to an item it holds: off both lists.
    CatalogueItem movie;
    movie.id = "manual:movie:matched";
    movie.kind = CatalogueKind::movie;
    movie.title = "Matched";
    (void)s2.catalogue().upsert(movie);
    CHECK(handle(s2, "POST", "/api/v1/manage/unmatched/" + matched_id + "/match",
                 R"({"catalogue_item_id":"manual:movie:matched"})")
              .status == 200);
    CHECK(listed(s2) == std::set<std::string>{deleted_id});
    CHECK(wait_until([&] { return listed(s1) == std::set<std::string>{deleted_id}; }, 10s));

    // Deleted through node 2: the file goes, and with it the entry.
    CHECK(handle(s2, "DELETE", "/api/v1/manage/unmatched/" + deleted_id).status == 204);
    CHECK(listed(s2).empty());
    CHECK(wait_until([&] { return listed(s1).empty(); }, 10s));

    s2.stop();
    s1.stop();
}

// An ingest job pinned to a node runs there, whichever node the API request
// reached; the path is checked against that node's own source roots.
MACHA_TEST("rpc_cluster", test_ingest_job_pinned_to_a_node_runs_there) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    const auto source_dir = cluster.path() / "pinned-n1-src";
    std::filesystem::create_directories(source_dir);
    const auto source_file = source_dir / "movie.mkv";
    {
        std::ofstream out(source_file, std::ios::binary);
        REQUIRE(out.good());
        out << "not really media, just needs to exist";
    }
    auto c1 = config_for(cluster.path() / "pinned-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "pinned-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    for (auto* config : {&c1, &c2}) {
        config->replication = 1;
        config->write_copies = 1;
        config->metadata_write_copies = 1;
        config->catalogue.scanner.enabled = true;
        config->catalogue.scanner.movies.enabled = false;
        config->catalogue.scanner.tv.enabled = false;
        config->catalogue.api.enabled = false;
        config->ingest.enabled = true;
    }
    // Only node 1 may read the source.
    c1.ingest.source_roots = {source_dir};

    Service s1(c1, keys, test_durability_window);
    Service s2(c2, keys, test_durability_window);
    s1.start();
    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() == 2 &&
               s2.node().membership().active().size() == 2;
    }));
    const auto node1 = to_string(s1.node().node_id());

    const auto submit = [](Service& service, const std::string& body) {
        HttpRequest request;
        request.method = "POST";
        request.path = "/api/v1/ingest/jobs";
        request.body = Bytes(body.begin(), body.end());
        return service.acquisition_api().handle(request);
    };
    const auto body_json = [](const HttpResponse& response) {
        return Json::parse(
            std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size()));
    };
    const auto path_field = "\"path\":\"" + source_file.string() + "\"";

    // Not pinned: the receiving node reads its own filesystem and refuses.
    CHECK(submit(s2, "{" + path_field + "}").status == 400);

    // Pinned to node 1 through node 2: the job is node 1's.
    const auto accepted = submit(s2, "{" + path_field + ",\"node_id\":\"" + node1 + "\"}");
    REQUIRE(accepted.status == 202);
    const auto parsed = body_json(accepted);
    CHECK(parsed.find("node_id")->asString() == node1);
    const auto id = parsed.find("id")->asString();
    CHECK(s1.ingest().job(id).has_value());
    CHECK(!s2.ingest().job(id).has_value());

    // Node 1's own refusal comes back as it is.
    const auto outside =
        submit(s2, "{\"path\":\"" + cluster.path().string() + "\",\"node_id\":\"" + node1 + "\"}");
    CHECK(outside.status == 400);
    CHECK(body_json(outside).find("error")->find("message")->asString().find("source_roots") !=
          std::string::npos);

    // A node that is not a member.
    const auto stranger =
        submit(s2, "{" + path_field + ",\"node_id\":\"" + to_string(random_node_id()) + "\"}");
    CHECK(stranger.status == 409);
    CHECK(body_json(stranger).find("error")->find("reason")->asString() == "node_not_member");

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

    auto c1 = config_for(cluster.path() / "jobvis-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 = config_for(cluster.path() / "jobvis-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
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
    const auto torrent_id =
        s1_torrents->add("magnet:?xt=urn:btih:3333333333333333333333333333333333333333&dn=Test");
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
            if (!id || id->asString() != ingest_id)
                continue;
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
        const auto response =
            post(s2.acquisition_api(), "/api/v1/ingest/jobs/" + ingest_id + "/resume");
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
    REQUIRE(
        wait_until([&] { return s2.torrent_coordinator().request(torrent_id).has_value(); }, 10s));
    // The claim, the phase and node 1's live state reach node 2 apart.
    CHECK(wait_until(
        [&] {
            s2.cluster_jobs().refresh_now();
            const auto response = get(s2.acquisition_api(), "/api/v1/torrents/jobs");
            if (response.status != 200)
                return false;
            const auto parsed = body_json(response);
            for (const auto& job : parsed.find("jobs")->asArray())
                if (job.find("id")->asString() == torrent_id)
                    return job.find("phase")->asString() == "downloading" &&
                           job.find("state")->asString() == "paused" &&
                           job.find("desired_applied")->asBool() &&
                           !job.find("live_as_of_unix_ms")->isNull();
            return false;
        },
        10s));
    {
        const auto response = get(s2.acquisition_api(), "/api/v1/torrents/jobs");
        REQUIRE(response.status == 200);
        const auto parsed = body_json(response);
        bool found = false;
        for (const auto& job : parsed.find("jobs")->asArray()) {
            if (job.find("id")->asString() != torrent_id)
                continue;
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
        REQUIRE(wait_until(
            [&] {
                s1.torrent_coordinator().pass_now();
                const auto job = s1_torrents->job(torrent_id);
                return job && job->state != TorrentJobState::paused;
            },
            10s));
    }

    // --- Torrent: the capable nodes, as node 2 sees them: node 1 only ---
    // Node 1's capability and staging reach node 2 as it polls them.
    CHECK(wait_until(
        [&] {
            const auto response = get(s2.acquisition_api(), "/api/v1/torrents/nodes");
            if (response.status != 200)
                return false;
            const auto nodes = body_json(response).find("nodes")->asArray();
            return nodes.size() == 1 && nodes.front().find("reachable")->asBool() &&
                   nodes.front().find("accepting")->asBool() &&
                   nodes.front().find("staging")->find("available")->asBool();
        },
        10s));
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
        // A peer's staging, as polled, says whether its directory is there.
        CHECK(nodes.front().find("staging")->find("available")->asBool());
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
        const auto here = add(
            R"({"magnet":"magnet:?xt=urn:btih:4444444444444444444444444444444444444444","node_id":")" +
            to_string(s2.node().node_id()) + R"("})");
        CHECK(here.status == 409);
        CHECK(body_json(here).find("error")->find("reason")->asString() ==
              "node_not_torrent_capable");

        const auto queued = add(
            R"({"magnet":"magnet:?xt=urn:btih:4444444444444444444444444444444444444444&dn=Queued"})");
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
            if (job.find("id")->asString() == queued_id)
                listed = true;
        CHECK(listed);
        // Node 1, the only capable node, claims and starts it.
        REQUIRE(wait_until(
            [&] {
                s1.torrent_coordinator().pass_now();
                return s1_torrents->job(queued_id).has_value();
            },
            15s));
        REQUIRE(wait_until(
            [&] {
                const auto r = s2.torrent_coordinator().request(queued_id);
                return r && r->claim && to_string(r->claim->node_id) == node1_id;
            },
            10s));

        // The same torrent again, through either node, is the one job.
        const auto again =
            add(R"({"magnet":"magnet:?xt=urn:btih:4444444444444444444444444444444444444444"})");
        CHECK(again.status == 409);
        CHECK(body_json(again).find("id")->asString() == queued_id);

        // Cancelled through node 2; node 1 stops it and says so.
        REQUIRE(
            post(s2.acquisition_api(), "/api/v1/torrents/jobs/" + queued_id + "/cancel").status ==
            202);
        REQUIRE(wait_until(
            [&] {
                s1.torrent_coordinator().pass_now();
                const auto r = s2.torrent_coordinator().request(queued_id);
                return r && r->phase == TorrentPhase::cancelled;
            },
            15s));
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
            if (id && id->asString() == ingest_id)
                found = true;
        }
        CHECK(found);
    }
    s1.stop();
}
#endif // MACHA_TEST_PLUGIN_DIR

// A node truncates its own history while its peer is away, and goes on
// committing. The peer returns holding the head from before the truncation,
// takes the newer head, and the two serve one namespace.
MACHA_TEST("rpc_cluster",
           test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    auto p1 = free_port();
    auto p2 = free_port();
    auto c1 =
        config_for(cluster.path() / "truncate-n1", cluster.keyfile(), p1, {{"127.0.0.1", p2}});
    auto c2 =
        config_for(cluster.path() / "truncate-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
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

    auto metadata_store = macha::test_support::distributed_store_for(node);
    MetadataManager metadata(node, node.local_state(), node.metadata_server(), *metadata_store);
    auto& replica = node.metadata_replica();
    const auto directory = [] {
        FsEntry entry;
        entry.type = EntryType::directory;
        entry.mode = 0755;
        entry.uid = getuid();
        entry.gid = getgid();
        return entry;
    };
    metadata.mutate_delta([&](MetadataSnapshot&, MetadataDelta& delta) {
        delta.upsert_entries["/mine"] = directory();
    });
    const auto mine = replica.committed();

    // Another author's head, which this node's clock does not cover.
    const MetadataDot theirs{random_node_id(), 9};
    // Its namespace is this node's with "/mine" gone and "/theirs" added.
    auto made = directory();
    stamp_entry_provenance(made, "/theirs", nullptr, theirs);
    MetadataDelta foreign_delta;
    foreign_delta.erase_entries.push_back("/mine");
    foreign_delta.upsert_entries["/theirs"] = made;
    auto foreign_snapshot = macha::test_support::with_namespace_delta(
        decode_snapshot(mine.payload), node.local_state().control(), *metadata_store,
        foreign_delta);
    foreign_snapshot.mutation_sequences.clear();
    foreign_snapshot.legacy_clock.emplace();
    foreign_snapshot.mutation_sequences[theirs.author] = theirs.sequence;
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
    acceptance.replicas = {theirs.author};
    REQUIRE(node.metadata_server().accept_commit(acceptance));
    REQUIRE(replica.accepted_heads().size() == 2);

    const auto records = replica.diagnostics().history_records;
    REQUIRE(records > 1);
    metadata.truncate_history(1, 1);
    CHECK(replica.diagnostics().history_records == records);
    CHECK(replica.accepted_heads().size() == 2);

    const auto holds = [&](const MetadataSnapshot& snapshot, std::string_view path) {
        return macha::test_support::namespace_holds(snapshot, node.local_state().control(),
                                                    *metadata_store, path);
    };
    // Reading merges the two; the one head then becomes the root.
    CHECK(holds(metadata.snapshot(), "/theirs"));
    REQUIRE(replica.accepted_heads().size() == 1);
    metadata.truncate_history(1, 1);
    CHECK(replica.diagnostics().history_records == 1);
    const auto after = metadata.snapshot();
    CHECK(holds(after, "/mine"));
    CHECK(holds(after, "/theirs"));
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
    auto reply =
        s2->node().call(s1.node().membership().self(), MessageType::get_metadata_history_record,
                        request.take(), FrameType::control, call_deadline::metadata);
    REQUIRE(reply.message.type == MessageType::metadata_history_entry_reply);
    auto served = decode_metadata_history_entry(reply.message.payload);
    CHECK(served.hash == head);
    CHECK(served.body == MetadataHistoryEntry::Body::full);

    s2->stop();
    s1.stop();
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
        ~RuntimeNode() {
            stop();
        }
    };

    RuntimeNode hub(hub_config, keys);
    hub.start();

    auto site_config =
        cluster.node_config("site", 0, {Endpoint{hub_config.advertise_host, hub_config.port}});
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
    // Hosting is its own advert, exchanged apart from the flag.
    CHECK(wait_until([&] { return !hub.node->membership().hosts_extents(site_id); }, 10s));
    CHECK(wait_until([&] { return hub.node->membership().all_known_reachable(); }, 10s));

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
    CHECK(wait_until([&] { return hub.node->membership().hosts_extents(site_id); }, 10s));

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

namespace {} // namespace

// The transport's behaviours as the Dannan extraction inventory lists them
// (dannan/TODO/2026-11-03-macha-function-inventory.md, "Transport and
// lanes"), each written from its specification: docs/cluster.md, the Network
// section of docs/configuration.md, and the contracts in cluster/net.hpp.
// They are the gate the transport's rewrite on Asio is held to.
namespace {

// The no-progress deadline follows the lane the message travels on: a stuck
// control call fails at the control deadline, an object transfer at the
// DATA deadline, and a lane whose deadline is 0 waits (docs/configuration.md,
// control_no_progress_deadline_ms and data_no_progress_deadline_ms).
MACHA_TEST("rpc_cluster", test_no_progress_deadline_follows_the_lane_configured) {
    TestCluster cluster;
    const auto& keys = cluster.keys();

    // One lane bounded at 300 ms, the other unbounded: `stuck`, on the
    // bounded lane, never answers and fails at the deadline; `slow`, on the
    // unbounded lane, answers well after it and succeeds.
    const auto run = [&](const char* name, std::chrono::milliseconds control_deadline,
                         std::chrono::milliseconds data_deadline, MessageType stuck,
                         FrameType stuck_frame, MessageType slow, FrameType slow_frame) {
        TestGate stuck_gate;
        TestGate slow_gate;
        StandInPeer server(cluster, std::string(name) + "-server", {});
        const auto answer = [&](const NodeInfo&, FrameType, const RpcMessage& request) {
            if (request.type == stuck)
                stuck_gate.enter_and_wait();
            else if (request.type == slow)
                slow_gate.enter_and_wait();
            if (request.type == MessageType::get_object)
                return RpcMessage{MessageType::object_reply, Bytes{1}};
            return RpcMessage{MessageType::bool_reply, Bytes{1}};
        };
        server.routes.bind(MessageType::get_object, answer);
        server.routes.bind(MessageType::have_object, answer);
        server.start();
        struct GateOpener {
            TestGate& a;
            TestGate& b;
            ~GateOpener() {
                a.open();
                b.open();
            }
        } open_on_exit{stuck_gate, slow_gate};
        const auto endpoint = server.endpoint();

        auto config = config_for(cluster.path() / name, cluster.keyfile(), free_port());
        config.control_no_progress_deadline = control_deadline;
        config.data_no_progress_deadline = data_deadline;
        BareNode node(config, keys);
        node.start();

        const auto started = Clock::now();
        std::string error;
        try {
            (void)node.call(endpoint, stuck, Bytes{1}, stuck_frame, call_deadline::control);
        } catch (const std::exception& e) {
            error = e.what();
        }
        const auto waited = Clock::now() - started;
        CHECK(error.find("no progress") != std::string::npos);
        CHECK(waited >= 250ms);
        CHECK(waited < scaled(3s));

        std::thread releaser([&] {
            if (slow_gate.wait_for_entries(1, 5s))
                std::this_thread::sleep_for(800ms); // well past the 300 ms deadline
            slow_gate.open();
        });
        bool slow_ok = false;
        try {
            slow_ok = node.call(endpoint, slow, Bytes{2}, slow_frame, call_deadline::control)
                          .message.type != MessageType::error;
        } catch (const std::exception&) {
        }
        releaser.join();
        CHECK(slow_ok);
        stuck_gate.open();
        node.stop();
        server.stop();
    };
    run("control-bound", 300ms, 0ms, MessageType::have_object, FrameType::control,
        MessageType::get_object, FrameType::foreground);
    run("data-bound", 0ms, 300ms, MessageType::get_object, FrameType::foreground,
        MessageType::have_object, FrameType::control);
}

} // namespace

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts the root builds before the node's services, each alone: the
// node-wide resources, local recovery's progress, and the control object
// fetch.
#include "cluster/control_objects.hpp"
#include "fake_cluster_node.hpp"
#include "stepped_time.hpp"
#include "storage/io_pressure.hpp"
#include "test_backend_support.hpp"

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

RpcMessage control_reply(const ObjectId& id, std::span<const uint8_t> bytes) {
    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(bytes);
    return {MessageType::control_object_reply, writer.take()};
}

// The one warning a failed pull logs, or empty.
std::string unavailable_line(const ConcurrentCapturingLogger& log) {
    for (const auto& [level, line] : log.records())
        if (level == LogLevel::warn && line.find("control object unavailable") != std::string::npos)
            return line;
    return {};
}

struct FetchBench {
    std::shared_ptr<ConcurrentCapturingLogger> log{
        std::make_shared<ConcurrentCapturingLogger>(LogLevel::debug)};
    StoreBench bench;
    explicit FetchBench(const std::function<void(Config&)>& tune = {}) : bench(tune) {
        Log::set_logger(log);
    }
    ~FetchBench() { Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info)); }
    ControlObjectFetch& fetch() { return bench.local.control_fetch(); }
    LocalStore& control() { return bench.local.control(); }
};

// A peer's answer is stored only when it is the object asked for: the right
// reply, naming the id, with bytes that hash to it. Each other answer leaves
// the store without the object and is named as the failure.
MACHA_FAST_TEST("root_parts", test_control_fetch_stores_only_the_object_it_asked_for) {
    const auto bytes = pattern(4096, 7);
    const auto id = object_id(bytes);
    const auto other = pattern(4096, 8);

    {
        FetchBench alone;
        CHECK(!alone.fetch().pull(id, {}));
        CHECK(unavailable_line(*alone.log).find("peers_asked=0 last_failure=no active peer") !=
              std::string::npos);
    }

    struct Case {
        const char* what;
        FakeClusterNode::Handler answer;
        std::string failure;
    };
    const std::vector<Case> cases{
        {"another reply type", [](MessageType, const Bytes&, FrameType) {
             return RpcMessage{MessageType::ok, {}};
         }, ": ok"},
        {"another object's id", [&](MessageType, const Bytes&, FrameType) {
             return control_reply(object_id(other), other);
         }, ": integrity failure"},
        {"bytes that do not hash to the id", [&](MessageType, const Bytes&, FrameType) {
             return control_reply(id, other);
         }, ": integrity failure"},
        {"a truncated reply", [&](MessageType, const Bytes&, FrameType) {
             return RpcMessage{MessageType::control_object_reply, Bytes(id.bytes.begin(), id.bytes.end())};
         }, ": "},
    };
    for (const auto& c : cases) {
        FetchBench f;
        const auto peer = StoreBench::peer();
        f.bench.node.add_peer(peer, c.answer);
        size_t observed = 0;
        const bool pulled = f.fetch().pull(id, [&](uint64_t, Clock::duration) { ++observed; });
        if (pulled || observed || f.control().valid(id))
            std::cerr << "control fetch accepted " << c.what << "\n";
        CHECK(!pulled);
        CHECK(observed == 0);
        CHECK(!f.control().valid(id));
        const auto line = unavailable_line(*f.log);
        CHECK(line.find("peers_asked=1 last_failure=" + peer.host + c.failure) != std::string::npos);
        // Asked once, on the control lane at speculative priority.
        const auto calls = f.bench.node.calls();
        REQUIRE(calls.size() == 1);
        CHECK(calls.front().type == MessageType::get_control_object);
        CHECK(calls.front().frame_type == FrameType::speculative);
    }
}

// A peer that cannot be reached is passed over for one that answers; the
// object is then local, the observer is told its size once, and a second pull
// asks nobody.
MACHA_FAST_TEST("root_parts", test_control_fetch_passes_over_a_failing_peer) {
    const auto bytes = pattern(8192, 9);
    const auto id = object_id(bytes);

    {
        FetchBench only_dead;
        const auto unreachable = StoreBench::peer();
        only_dead.bench.node.add_peer(unreachable);
        only_dead.bench.node.refuse(unreachable.id);
        // Through the interface namespace replay holds: no observer.
        ControlObjectSource& failing = only_dead.fetch();
        CHECK(!failing.ensure_control_local(id));
        CHECK(unavailable_line(*only_dead.log).find(unreachable.host + ": RPC to " +
                                                     unreachable.host + " cannot be placed") !=
              std::string::npos);
    }

    // Peers are asked in roster order: the first is the dead one.
    FetchBench f;
    f.bench.node.add_peer(StoreBench::peer());
    f.bench.node.add_peer(StoreBench::peer());
    const auto roster = f.bench.node.membership().active();
    REQUIRE(roster.size() == 3);
    REQUIRE(roster[0].id == f.bench.node.node_id());
    const auto dead = roster[1];
    const auto holder = roster[2];
    f.bench.node.refuse(dead.id);
    const auto second = pattern(4096, 11);
    f.bench.node.add_peer(holder, [&](MessageType type, const Bytes& request, FrameType) {
        CHECK(type == MessageType::get_control_object);
        Reader reader(request);
        const ObjectId asked{reader.fixed<32>()};
        if (asked == object_id(second))
            return control_reply(asked, second);
        CHECK(asked == id);
        return control_reply(id, bytes);
    });

    std::vector<uint64_t> observed;
    ControlObjectSource& source = f.fetch();
    REQUIRE(f.fetch().pull(id, [&](uint64_t size, Clock::duration) { observed.push_back(size); }));
    CHECK(observed == std::vector<uint64_t>{bytes.size()});
    CHECK(f.control().valid(id));
    CHECK(f.control().get(id) == bytes);
    CHECK(f.bench.node.calls_of(MessageType::get_control_object, dead.id) == 1);
    CHECK(f.bench.node.calls_of(MessageType::get_control_object, holder.id) == 1);

    const auto asked = f.bench.node.calls().size();
    CHECK(source.ensure_control_local(id));
    CHECK(f.bench.node.calls().size() == asked);

    // The same through the interface, which has no observer to tell.
    CHECK(source.ensure_control_local(object_id(second)));
    CHECK(f.control().get(object_id(second)) == second);
}

// A good answer the control store cannot take is a failure, not a success.
MACHA_FAST_TEST("root_parts", test_control_fetch_fails_when_the_store_cannot_take_the_object) {
    const auto bytes = pattern(256 * 1024, 10);
    const auto id = object_id(bytes);
    FetchBench f([](Config& c) { c.metadata_store.limit = 64 * 1024; });
    const auto holder = StoreBench::peer();
    f.bench.node.add_peer(holder, [&](MessageType, const Bytes&, FrameType) {
        return control_reply(id, bytes);
    });
    size_t observed = 0;
    CHECK(!f.fetch().pull(id, [&](uint64_t, Clock::duration) { ++observed; }));
    // It arrived, so the transfer is observed; it is not local.
    CHECK(observed == 1);
    CHECK(!f.control().valid(id));
    CHECK(unavailable_line(*f.log).find("last_failure=local control store put failed") !=
          std::string::npos);
}

DataWorkContext loader_work() {
    return DataWorkContext(FrameType::loader);
}

// Law 3 as the resources wire it: under device pressure the loader yields
// only while a viewer has been active within maintenance.foreground_quiet,
// read from the node's activity clocks on the clock the root hands in.
MACHA_FAST_TEST("root_parts", test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("resources");
    config.maintenance.foreground_quiet = 2s;
    config.maintenance.background_concurrency = 8;
    SteppedTime time;
    NodeResources resources(config, [&] { return time.now(); });

    DiskServiceMonitor monitor(DiskServiceMonitor::Thresholds{25ms, 120ms, 300, 150, 1000});
    monitor.note(2000ms, 4096);
    REQUIRE(monitor.pressured());
    resources.data.observe_device(&monitor, 1);

    // One loader lease is always admitted; the pressure gate judges the next.
    auto first = resources.data.try_acquire(loader_work(), 4096);
    REQUIRE(first.has_value());

    // No viewer: pressure alone does not hold an import back.
    {
        auto second = resources.data.try_acquire(loader_work(), 4096);
        CHECK(second.has_value());
    }
    CHECK(resources.data.stats().pressure_refusals == 0);

    // A viewer read within the window: the loader is refused.
    resources.activity.note(FrameType::foreground, 1);
    time.advance(1999ms);
    CHECK(!resources.data.try_acquire(loader_work(), 4096).has_value());
    CHECK(resources.data.stats().pressure_refusals == 1);

    // The window passes with no more viewer activity: admitted again.
    time.advance(1ms);
    CHECK(resources.data.try_acquire(loader_work(), 4096).has_value());
    CHECK(resources.data.stats().pressure_refusals == 1);

    // Stopped, the resources admit nothing.
    resources.stop();
    first.reset();
    CHECK(!resources.data.try_acquire(loader_work(), 4096).has_value());
}

// Recovery progress keeps the first failure and wakes a waiter on failure as
// on completion.
MACHA_FAST_TEST("root_parts", test_recovery_progress_keeps_the_first_failure_and_wakes_its_waiters) {
    {
        RecoveryProgress progress;
        CHECK(!progress.has(RecoveryProgress::cache));
        progress.mark(RecoveryProgress::cache);
        CHECK(progress.has(RecoveryProgress::cache));
        CHECK(!progress.has(RecoveryProgress::metadata));

        std::optional<bool> completed;
        std::jthread waiter([&] { completed = progress.wait_complete(1h); });
        progress.fail("data storage: disk gone");
        progress.fail("local state: later");
        waiter.join();
        CHECK(completed == false);
        CHECK(progress.failed());
        CHECK(!progress.complete());
        CHECK(progress.error() == "data storage: disk gone");
    }
    {
        RecoveryProgress progress;
        CHECK(!progress.wait_complete(0ms));
        std::optional<bool> completed;
        std::jthread waiter([&] { completed = progress.wait_complete(1h); });
        progress.mark_complete(1234);
        waiter.join();
        CHECK(completed == true);
        CHECK(progress.complete());
        CHECK(!progress.failed());
        CHECK(progress.error().empty());
        CHECK(progress.ready_unix_ms() == 1234);
    }
}

} // namespace

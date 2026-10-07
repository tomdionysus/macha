// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract vocabulary: work context, wait declarations and their guard,
// cursor, budget, page. Each is tested over its whole phase space.
#include "catalogue/catalogue.hpp"
#include "cluster/data_work.hpp"
#include "cluster/distributed_store.hpp"
#include "contract/published.hpp"
#include "contract/walk.hpp"
#include "contract/work.hpp"
#include "log.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace macha;
using namespace std::chrono_literals;

namespace {

constexpr std::array frame_types{FrameType::control, FrameType::foreground, FrameType::read_ahead,
                                 FrameType::speculative, FrameType::loader};

MACHA_FAST_TEST("contract", test_waits_compose_and_include_over_every_mask) {
    for (unsigned a = 0; a < 16; ++a)
        for (unsigned b = 0; b < 16; ++b) {
            const auto combined = static_cast<Waits>(a) | static_cast<Waits>(b);
            CHECK(static_cast<unsigned>(combined) == (a | b));
            for (unsigned bit : {1U, 2U, 4U, 8U})
                CHECK(includes(combined, static_cast<Waits>(bit)) == (((a | b) & bit) != 0));
        }
    CHECK(!includes(Waits::none, Waits::network));
}

MACHA_FAST_TEST("contract", test_only_control_is_refused_and_only_device_or_network_waits) {
    for (auto frame_type : frame_types)
        for (unsigned mask = 0; mask < 16; ++mask) {
            const bool data_or_network = (mask & (2U | 4U)) != 0;
            const bool expected = frame_type != FrameType::control || !data_or_network;
            CHECK(may_enter(frame_type, static_cast<Waits>(mask)) == expected);
        }
}

MACHA_FAST_TEST("contract", test_wait_guard_records_or_throws) {
    struct Counting final : Logger {
        int warnings{};
        bool enabled(LogLevel) const noexcept override {
            return true;
        }
        void log(LogLevel level, const std::string&) override {
            if (level == LogLevel::warn)
                ++warnings;
        }
    };
    auto counting = std::make_shared<Counting>();
    Log::set_logger(counting);

    WaitGuard::set_mode(WaitGuard::Mode::record);
    CHECK(WaitGuard::mode() == WaitGuard::Mode::record);
    const auto before = WaitGuard::violations();
    const WorkContext control(FrameType::control);
    const WorkContext loader(FrameType::loader);
    CHECK(WaitGuard::enter(loader, Waits::network, "op.a"));
    CHECK(WaitGuard::enter(control, Waits::state_device | Waits::locks, "op.a"));
    CHECK(WaitGuard::violations() == before);
    CHECK(!WaitGuard::enter(control, Waits::network, "op.a"));
    CHECK(!WaitGuard::enter(control, Waits::data_device, "op.a"));
    CHECK(!WaitGuard::enter(control, Waits::network, "op.b"));
    CHECK(WaitGuard::violations() == before + 3);
    // One warning per operation, however often it is reached.
    CHECK(counting->warnings == 2);

    WaitGuard::set_mode(WaitGuard::Mode::throw_on_violation);
    CHECK(WaitGuard::mode() == WaitGuard::Mode::throw_on_violation);
    bool threw = false;
    try {
        (void)WaitGuard::enter(control, Waits::network, "op.c");
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(WaitGuard::violations() == before + 4);
    CHECK(WaitGuard::enter(loader, Waits::network, "op.c"));
    WaitGuard::set_mode(WaitGuard::Mode::record);
    Log::set_logger(std::make_shared<ConsoleLogger>());
}

MACHA_FAST_TEST("contract", test_work_context_deadline_and_cancellation) {
    const auto now = WorkContext::Clock::now();
    CHECK(!WorkContext(FrameType::loader).expired(now));
    CHECK(!WorkContext(FrameType::loader, now + 1s).expired(now));
    CHECK(WorkContext(FrameType::loader, now).expired(now));
    std::atomic_bool cancelled{};
    const WorkContext context(FrameType::control, {}, &cancelled);
    CHECK(context.frame_type() == FrameType::control);
    CHECK(context.cancellation() == &cancelled);
    CHECK(!context.cancelled());
    cancelled = true;
    CHECK(context.cancelled());
    CHECK(!WorkContext().cancelled());
}

MACHA_FAST_TEST("contract", test_data_work_context_is_the_data_specialisation) {
    const DataWorkContext data(FrameType::speculative, 4096);
    const WorkContext& general = data;
    CHECK(general.frame_type() == FrameType::speculative);
    CHECK(data.quantum_bytes() == 4096);
    bool refused = false;
    try {
        (void)DataWorkContext(FrameType::control);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
}

// Every sequence of up to seven takes against every limit 0..5 and none.
MACHA_FAST_TEST("contract", test_budget_bounds_are_spent_exactly) {
    for (int limit = -1; limit <= 5; ++limit)
        for (int takes = 0; takes <= 7; ++takes) {
            Budget operations;
            Budget bytes;
            if (limit >= 0) {
                operations.operations(static_cast<size_t>(limit));
                bytes.bytes(static_cast<uint64_t>(limit) * 10);
            }
            int granted_operations = 0;
            int granted_bytes = 0;
            for (int i = 0; i < takes; ++i) {
                granted_operations += operations.take_operation() ? 1 : 0;
                granted_bytes += bytes.take_bytes(10) ? 1 : 0;
            }
            const int expected = limit < 0 ? takes : std::min(takes, limit);
            CHECK(granted_operations == expected);
            CHECK(granted_bytes == expected);
            if (limit >= 0) {
                CHECK(operations.operations_left() == static_cast<size_t>(limit - expected));
                CHECK(bytes.bytes_left() == static_cast<uint64_t>((limit - expected) * 10));
            } else {
                CHECK(!operations.operations_left());
                CHECK(!bytes.bytes_left());
            }
        }
    // A take larger than what is left spends nothing.
    Budget partial;
    partial.bytes(15);
    CHECK(!partial.take_bytes(20));
    CHECK(partial.bytes_left() == 15u);
    CHECK(partial.take_bytes(15));
    CHECK(partial.bytes_left() == 0u);
}

struct FixedYield final : YieldSource {
    bool answer{};
    bool should_yield() const override {
        return answer;
    }
};

// A yield source is owned through its interface where it is injected.
MACHA_FAST_TEST("contract", test_a_yield_source_is_destroyed_through_its_interface) {
    std::unique_ptr<YieldSource> source = std::make_unique<FixedYield>();
    CHECK(!source->should_yield());
    source.reset();
    CHECK(source == nullptr);
}

// Every combination of cancelled, budget deadline passed, context deadline
// passed and yield requested: cancellation wins, then either deadline, then
// the yield source.
MACHA_FAST_TEST("contract", test_budget_stop_precedence_over_every_combination) {
    const auto now = Budget::Clock::now();
    for (unsigned bits = 0; bits < 16; ++bits) {
        const bool cancelled_now = bits & 1U;
        const bool budget_deadline = bits & 2U;
        const bool context_deadline = bits & 4U;
        const bool yielding = bits & 8U;
        std::atomic_bool cancelled{cancelled_now};
        const WorkContext context(FrameType::speculative,
                                  context_deadline ? now - 1ms : now + 1h, &cancelled);
        FixedYield yield;
        yield.answer = yielding;
        Budget budget(context);
        budget.deadline(budget_deadline ? now - 1ms : now + 1h).yield_to(&yield);
        std::optional<Stop> expected;
        if (cancelled_now)
            expected = Stop::cancelled;
        else if (budget_deadline || context_deadline)
            expected = Stop::deadline;
        else if (yielding)
            expected = Stop::yield;
        CHECK(budget.must_stop(now) == expected);
        CHECK(budget.context().frame_type() == FrameType::speculative);
    }
    CHECK(!Budget().must_stop());
}

MACHA_FAST_TEST("contract", test_cursor_and_page) {
    Cursor<int> start;
    Cursor<int> after_three{3};
    CHECK(!start.after);
    CHECK(start != after_three);
    CHECK(after_three == Cursor<int>{3});
    Page<int, int> page;
    CHECK(page.complete());
    for (auto stop : {Stop::budget, Stop::deadline, Stop::cancelled, Stop::yield}) {
        page.stopped = stop;
        CHECK(!page.complete());
    }
}

MACHA_FAST_TEST("contract", test_published_is_empty_until_the_first_publish) {
    Published<int> empty;
    CHECK(empty.handle() == nullptr);
    empty.publish(7);
    CHECK(empty.handle() != nullptr && *empty.handle() == 7);
    Published<int> initial(std::make_shared<const int>(3));
    CHECK(*initial.handle() == 3);
}

MACHA_FAST_TEST("contract", test_published_refuses_a_missing_snapshot) {
    Published<int> published;
    bool threw = false;
    try {
        published.publish(Published<int>::Handle{});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(published.handle() == nullptr);
    published.publish(1);
    threw = false;
    try {
        published.publish(Published<int>::Handle{});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(*published.handle() == 1);
    threw = false;
    try {
        Published<int> constructed(nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// A handle held across a republish still reads its own snapshot, and that
// snapshot lives until its last holder lets go.
MACHA_FAST_TEST("contract", test_a_held_handle_keeps_its_snapshot_across_republish) {
    Published<std::string> published;
    published.publish(std::string("first"));
    auto held = published.handle();
    const std::weak_ptr<const std::string> first = held;
    published.publish(std::string("second"));
    CHECK(*held == "first");
    CHECK(*published.handle() == "second");
    CHECK(!first.expired());
    auto second_held = published.handle();
    const std::weak_ptr<const std::string> second = second_held;
    published.publish(std::string("third"));
    CHECK(*held == "first");
    CHECK(*second_held == "second");
    held.reset();
    CHECK(first.expired());
    CHECK(!second.expired());
    second_held.reset();
    CHECK(second.expired());
    // Unheld, a replaced snapshot goes with the publish that replaced it.
    const std::weak_ptr<const std::string> third = published.handle();
    published.publish(std::string("fourth"));
    CHECK(third.expired());
}

// The previous snapshot is destroyed after the publish releases its lock: a
// reader asking for a handle while that destructor runs is not held up.
MACHA_FAST_TEST("contract", test_publish_destroys_the_previous_snapshot_outside_its_lock) {
    struct Probe {
        std::function<void()> on_destroy;
        Probe(const Probe&) = delete;
        Probe& operator=(const Probe&) = delete;
        explicit Probe(std::function<void()> f) : on_destroy(std::move(f)) {}
        ~Probe() {
            if (on_destroy)
                on_destroy();
        }
    };
    Published<Probe> published;
    std::thread reader;
    std::atomic<bool> read{false};
    bool read_during_destruction = false;
    published.publish(std::make_shared<const Probe>([&] {
        reader = std::thread([&] {
            (void)published.handle();
            read = true;
        });
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!read && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        read_during_destruction = read;
    }));
    published.publish(std::make_shared<const Probe>(nullptr));
    reader.join();
    CHECK(read_during_destruction);
}

// Readers racing a writer only ever see whole snapshots, in publish order.
MACHA_FAST_TEST("contract", test_published_readers_see_whole_snapshots_in_order) {
    struct Pair {
        uint64_t a;
        uint64_t b;
    };
    constexpr uint64_t publishes = 20000;
    Published<Pair> published(std::make_shared<const Pair>(Pair{0, 0}));
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<int> backwards{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
        readers.emplace_back([&] {
            uint64_t last = 0;
            while (!done) {
                const auto snapshot = published.handle();
                if (snapshot->a != snapshot->b)
                    ++torn;
                if (snapshot->a < last)
                    ++backwards;
                last = snapshot->a;
            }
        });
    for (uint64_t i = 1; i <= publishes; ++i)
        published.publish(Pair{i, i});
    done = true;
    for (auto& reader : readers)
        reader.join();
    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(published.handle()->a == publishes);
}

// The guard on real operations: MetadataManager::snapshot_view may refresh
// from the replicas, so control work may not enter it; the catalogue's warm
// snapshot waits on nothing, so control work may.
MACHA_TEST("contract", test_the_wait_guard_on_snapshot_views) {
    macha::test_support::TestService fixture("wait-guard");
    fixture.config().replication = 1;
    fixture.config().metadata_write_copies = 1;
    auto& service = fixture.start();
    (void)service.catalogue().snapshot_view();

    WaitGuard::set_mode(WaitGuard::Mode::throw_on_violation);
    const WorkContext control(FrameType::control, {}, nullptr, "test control");
    const WorkContext loader(FrameType::loader, {}, nullptr, "test loader");
    bool threw = false;
    try {
        (void)service.metadata_manager().snapshot_view(control);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    (void)service.metadata_manager().snapshot_view(loader);
    CHECK(service.catalogue().snapshot_view(control) != nullptr);
    WaitGuard::set_mode(WaitGuard::Mode::record);
}

// A cold catalogue loads from metadata and the control store, so control
// work may not enter it; once a loader has warmed it, control work may.
MACHA_TEST("contract", test_the_wait_guard_on_a_cold_catalogue) {
    macha::test_support::TestCluster cluster;
    auto config = macha::test_support::config_for(cluster.path() / "node", cluster.keyfile(),
                                                  macha::test_support::free_port());
    config.replication = 1;
    config.metadata_write_copies = 1;
    test_support::BareNode node(config, cluster.keys());
    node.start();
    REQUIRE(node.wait_local_state_ready(10s));
    DistributedStore store(node, node.local_state(), node.resources.activity, node.resources.data, node.resources.memory, node.resources.events);
    MetadataManager metadata(node, node.local_state(), node.metadata_server());
    CatalogueManager catalogue(node, node.local_state(), node.metadata_server(), store, metadata, node.ledger());

    WaitGuard::set_mode(WaitGuard::Mode::throw_on_violation);
    const WorkContext control(FrameType::control, {}, nullptr, "test control");
    const WorkContext loader(FrameType::loader, {}, nullptr, "test loader");
    bool threw = false;
    try {
        (void)catalogue.snapshot_view(control);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(catalogue.snapshot_view(loader) != nullptr);
    CHECK(catalogue.snapshot_view(control) != nullptr);
    WaitGuard::set_mode(WaitGuard::Mode::record);
}

} // namespace

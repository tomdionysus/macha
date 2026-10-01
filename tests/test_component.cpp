// SPDX-License-Identifier: GPL-3.0-or-later
//
// The component contract and the composition root (the object ledger spec,
// A5): order from declarations, refusals, the lifecycle steps and their
// record, a failed start, faults. Primitives over fake components.
#include "component/composition_root.hpp"
#include "log.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace macha;

namespace {

using Events = std::vector<std::string>;

class Fake final : public Component {
  public:
    Fake(std::string name, Events& events, std::vector<std::string> required = {},
         std::vector<std::string> provided = {})
        : name_(std::move(name)), events_(events), required_(std::move(required)),
          provided_(std::move(provided)) {}

    std::string_view name() const noexcept override { return name_; }
    std::vector<std::string> required() const override { return required_; }
    std::vector<std::string> provided() const override { return provided_; }
    void start() override {
        events_.push_back(name_ + ".start");
        if (fail_start)
            throw std::runtime_error(name_ + " refused to start");
    }
    void request_stop() noexcept override { events_.push_back(name_ + ".request_stop"); }
    void stop() override { events_.push_back(name_ + ".stop"); }
    void attach_fault_sink(FaultSink sink) override { sink_ = std::move(sink); }

    void fault(std::string reason) { sink_(std::move(reason)); }
    bool fail_start{};

  private:
    std::string name_;
    Events& events_;
    std::vector<std::string> required_;
    std::vector<std::string> provided_;
    FaultSink sink_;
};

std::unique_ptr<Fake> fake(std::string name, Events& events, std::vector<std::string> required = {},
                           std::vector<std::string> provided = {}) {
    return std::make_unique<Fake>(std::move(name), events, std::move(required), std::move(provided));
}

template <class F> bool throws_invalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

template <class F> bool throws_logic(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return false;
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

MACHA_FAST_TEST("component", test_root_order_is_adding_order_where_the_graph_is_free) {
    Events events;
    CompositionRoot root;
    root.add(fake("a", events));
    root.add(fake("b", events));
    root.add(fake("c", events));
    CHECK((root.order() == std::vector<std::string>{"a", "b", "c"}));
    CompositionRoot empty;
    CHECK(empty.order().empty());
    empty.start();
    empty.stop();
}

MACHA_FAST_TEST("component", test_root_starts_providers_first) {
    Events events;
    CompositionRoot root;
    // a needs y (from c), b needs x (from a), d is free: c before a before b,
    // d where it was added among those free when its turn comes.
    root.add(fake("a", events, {"y"}, {"x"}));
    root.add(fake("b", events, {"x"}));
    root.add(fake("c", events, {}, {"y"}));
    root.add(fake("d", events));
    CHECK((root.order() == std::vector<std::string>{"c", "a", "b", "d"}));

    // A diamond: top needs left and right, both need base.
    CompositionRoot diamond;
    diamond.add(fake("top", events, {"l", "r"}));
    diamond.add(fake("right", events, {"base"}, {"r"}));
    diamond.add(fake("left", events, {"base"}, {"l"}));
    diamond.add(fake("base", events, {}, {"base"}));
    CHECK((diamond.order() == std::vector<std::string>{"base", "right", "left", "top"}));

    // A component providing two contracts satisfies both requirers.
    CompositionRoot two;
    two.add(fake("user", events, {"p", "q"}));
    two.add(fake("both", events, {}, {"p", "q"}));
    CHECK((two.order() == std::vector<std::string>{"both", "user"}));
}

MACHA_FAST_TEST("component", test_root_externals_satisfy_requirements) {
    Events events;
    CompositionRoot root;
    root.external("metadata");
    root.add(fake("maintenance", events, {"metadata"}));
    CHECK((root.order() == std::vector<std::string>{"maintenance"}));
}

MACHA_FAST_TEST("component", test_root_refuses_a_graph_it_cannot_order) {
    Events events;
    {
        CompositionRoot root;
        root.add(fake("a", events, {"missing"}));
        CHECK(throws_invalid([&] { (void)root.order(); }));
        CHECK(throws_invalid([&] { root.start(); }));
    }
    {
        CompositionRoot root;
        root.add(fake("a", events, {}, {"x"}));
        root.add(fake("b", events, {}, {"x"}));
        CHECK(throws_invalid([&] { (void)root.order(); }));
    }
    {
        CompositionRoot root;
        root.external("x");
        root.add(fake("a", events, {}, {"x"}));
        CHECK(throws_invalid([&] { (void)root.order(); }));
    }
    {
        CompositionRoot root;
        root.add(fake("a", events, {"y"}, {"x"}));
        root.add(fake("b", events, {"x"}, {"y"}));
        root.add(fake("c", events));
        CHECK(throws_invalid([&] { (void)root.order(); }));
    }
    {
        CompositionRoot root;
        root.add(fake("self", events, {"x"}, {"x"}));
        CHECK(throws_invalid([&] { (void)root.order(); }));
    }
    {
        CompositionRoot root;
        root.add(fake("a", events));
        CHECK(throws_invalid([&] { root.add(fake("a", events)); }));
    }
    // Nothing started for any refused graph.
    CHECK(events.empty());
}

MACHA_FAST_TEST("component", test_root_is_fixed_once_started) {
    Events events;
    CompositionRoot root;
    root.add(fake("a", events));
    root.start();
    CHECK(throws_logic([&] { root.add(fake("b", events)); }));
    CHECK(throws_logic([&] { root.external("x"); }));
    CHECK(throws_logic([&] { root.start(); }));
    CHECK((events == Events{"a.start"}));
}

MACHA_FAST_TEST("component", test_root_lifecycle_steps_and_their_record) {
    Events events;
    Events record;
    {
        CompositionRoot root([&](std::string_view event) { record.emplace_back(event); });
        root.add(fake("b", events, {"x"}));
        root.add(fake("a", events, {}, {"x"}));
        root.start();
        CHECK((events == Events{"a.start", "b.start"}));
        root.request_stop();
        root.request_stop();
        root.stop();
        // Stopped: neither asked to stop nor stopped again.
        root.request_stop();
        root.stop();
    }
    CHECK((events == Events{"a.start", "b.start", "b.request_stop", "a.request_stop",
                            "b.request_stop", "a.request_stop", "b.stop", "a.stop"}));
    CHECK((record == Events{"start a", "start b", "request_stop b", "request_stop a",
                            "request_stop b", "request_stop a", "stop b", "stop a"}));

    // stop() without a request first, and the destructor stopping what runs.
    events.clear();
    {
        CompositionRoot root;
        root.add(fake("a", events));
        root.add(fake("b", events));
        root.start();
        root.stop();
    }
    CHECK((events == Events{"a.start", "b.start", "b.stop", "a.stop"}));
    events.clear();
    {
        CompositionRoot root;
        root.add(fake("a", events));
        root.add(fake("b", events));
        root.start();
    }
    CHECK((events == Events{"a.start", "b.start", "b.stop", "a.stop"}));
    // Never started: nothing to stop.
    events.clear();
    {
        CompositionRoot root;
        root.add(fake("a", events));
        root.request_stop();
        root.stop();
    }
    CHECK(events.empty());
}

MACHA_FAST_TEST("component", test_root_unwinds_a_failed_start) {
    Events events;
    Events record;
    CompositionRoot root([&](std::string_view event) { record.emplace_back(event); });
    root.add(fake("a", events));
    auto& b = root.add(fake("b", events));
    root.add(fake("c", events));
    b.fail_start = true;
    bool threw = false;
    try {
        root.start();
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()) == "b refused to start";
    }
    CHECK(threw);
    // b never started, so it is not stopped; c was never reached.
    CHECK((events == Events{"a.start", "b.start", "a.stop"}));
    CHECK((record == Events{"start a", "start b", "stop a"}));
    root.stop();
    CHECK((events == Events{"a.start", "b.start", "a.stop"}));
}

// Service asks for a stop from another thread while its startup thread may
// still be starting the root: the request waits for that start, then
// reaches what it started.
MACHA_FAST_TEST("component", test_root_stop_request_during_a_start_reaches_the_started) {
    struct Slow final : Component {
        std::atomic<bool> starting{false};
        std::atomic<bool> release{false};
        std::atomic<bool> requested{false};
        std::string_view name() const noexcept override { return "slow"; }
        void start() override {
            starting = true;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        void request_stop() noexcept override { requested = true; }
        void stop() override {}
    };
    CompositionRoot root;
    auto& slow = root.add(std::make_unique<Slow>());
    std::thread starter([&] { root.start(); });
    while (!slow.starting)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::atomic<bool> request_returned{false};
    std::thread stopper([&] {
        root.request_stop();
        request_returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!request_returned);
    slow.release = true;
    starter.join();
    stopper.join();
    CHECK(slow.requested);
}

MACHA_FAST_TEST("component", test_root_routes_faults_by_component) {
    Events events;
    std::vector<std::pair<std::string, std::string>> faults;
    CompositionRoot root({}, [&](std::string_view component, const std::string& reason) {
        faults.emplace_back(std::string(component), reason);
    });
    auto& a = root.add(fake("a", events));
    auto& b = root.add(fake("b", events));
    root.start();
    b.fault("disk gone");
    a.fault("lost");
    CHECK((faults == std::vector<std::pair<std::string, std::string>>{{"b", "disk gone"},
                                                                       {"a", "lost"}}));

    struct Counting final : Logger {
        std::vector<std::string> errors;
        bool enabled(LogLevel) const noexcept override { return true; }
        void log(LogLevel level, const std::string& message) override {
            if (level == LogLevel::error)
                errors.push_back(message);
        }
    };
    auto counting = std::make_shared<Counting>();
    Log::set_logger(counting);
    CompositionRoot logging;
    auto& c = logging.add(fake("c", events));
    logging.start();
    c.fault("wedged");
    CHECK((counting->errors == std::vector<std::string>{"component c fault: wedged"}));
}

} // namespace

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract vocabulary (the object ledger spec, A1 and A3): work context,
// wait declarations and their guard, cursor, budget, page. Primitives, each
// tested over its whole phase space.
#include "cluster/data_work.hpp"
#include "contract/walk.hpp"
#include "contract/work.hpp"
#include "log.hpp"
#include "test_framework.hpp"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>

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

} // namespace

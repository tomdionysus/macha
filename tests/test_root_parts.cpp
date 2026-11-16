// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts the root builds before the node's services, each alone: local
// recovery's progress.
#include "test_backend_support.hpp"

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// Recovery progress keeps the first failure and wakes a waiter on failure as
// on completion.
MACHA_FAST_TEST("root_parts",
                test_recovery_progress_keeps_the_first_failure_and_wakes_its_waiters) {
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

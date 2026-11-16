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

struct FetchBench {
    std::shared_ptr<ConcurrentCapturingLogger> log{
        std::make_shared<ConcurrentCapturingLogger>(LogLevel::debug)};
    StoreBench bench;
    explicit FetchBench(const std::function<void(Config&)>& tune = {}) : bench(tune) {
        Log::set_logger(log);
    }
    ~FetchBench() {
        Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
    }
    ControlObjectFetch& fetch() {
        return bench.local.control_fetch();
    }
    LocalStore& control() {
        return bench.local.control();
    }
};

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

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fault-injection UAT for SubsystemSupervisor (see
// TODO/2026-09-05-subsystem-plugin-isolation-plan.md, Phase 0): these load
// real .so/.dylib plugins via the real dlopen path, not in-process mocks.
// The core claim under test is that a subsystem plugin whose construction or
// start() throws -- the exact failure mode that crash-looped corvus-es-1 49
// times -- degrades to a per-subsystem faulted/disabled state instead of
// taking this test process down. Each MACHA_TEST case already runs in its
// own isolated child process, so if the supervisor ever let such an
// exception escape, that would show up as this test process crashing, not as
// an ordinary CHECK failure.
#include "subsystem_supervisor.hpp"
#include "test_backend_support.hpp" // ConcurrentCapturingLogger

#include <fstream>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

void copy_plugin(const std::filesystem::path& source, const std::filesystem::path& dest_dir) {
    std::filesystem::create_directories(dest_dir);
    std::error_code ec;
    std::filesystem::copy_file(source, dest_dir / source.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);
}

} // namespace

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_loads_and_stops_a_real_plugin) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_OK, dir.path());

    auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::info);
    Log::set_logger(capture);

    SubsystemSupervisor supervisor(dir.path());
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::running;
    }));

    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].name == "test_plugin_ok");
    CHECK(statuses[0].restart_count == 0);

    // A successful load must say so at INFO, naming the file it came from:
    // it is the only evidence an operator has that a deployed plugin was
    // actually picked up (Status needs an authenticated API call).
    bool announced = false;
    for (const auto& [level, message] : capture->records()) {
        if (level != LogLevel::info) continue;
        if (message.find("test_plugin_ok") == std::string::npos) continue;
        if (message.find("loaded and running") == std::string::npos) continue;
        CHECK(message.find(dir.path().string()) != std::string::npos);
        announced = true;
    }
    CHECK(announced);

    supervisor.stop();
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
}

MACHA_TEST("subsystem_supervisor",
          test_subsystem_supervisor_disables_a_repeatedly_faulting_plugin) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_FAULTING, dir.path());

    // Fast, deterministic retry policy -- production defaults would make
    // this test wait tens of seconds for the same outcome.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 2;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    SubsystemSupervisor supervisor(dir.path(), policy);
    supervisor.start(SubsystemContext{});

    // The whole point: this must be reached without the test process
    // crashing, even though the plugin's start() always throws.
    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::disabled;
    }, 5s));

    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].name == "test_plugin_faulting");
    CHECK(statuses[0].restart_count > policy.max_failures_in_window);
    CHECK(statuses[0].last_fault.find("always fails to start") != std::string::npos);

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor",
          test_subsystem_supervisor_reports_a_declining_plugin_as_unavailable) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_DECLINING, dir.path());

    // A factory that returns no instance means "this node is configured not
    // to run this capability" (torrent.enabled: false, say). It must settle
    // on unavailable and stay there -- not `faulted`, and above all not enter
    // the retry loop, which would reconstruct nothing forever.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 2;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::info);
    Log::set_logger(capture);

    SubsystemSupervisor supervisor(dir.path(), policy);
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::unavailable;
    }, 5s));

    std::this_thread::sleep_for(100ms); // long enough for several retries.
    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].state == SubsystemState::unavailable);
    CHECK(statuses[0].restart_count == 0);
    CHECK(statuses[0].last_fault.empty());

    // Declining is also announced: silence here would leave an operator
    // unable to tell "plugin present but switched off" from "plugin missing".
    bool announced = false;
    for (const auto& [level, message] : capture->records())
        if (level == LogLevel::info && message.find("test_plugin_declining") != std::string::npos &&
            message.find("not enabled on this node") != std::string::npos)
            announced = true;
    CHECK(announced);

    supervisor.stop();
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_refuses_a_mismatched_plugin) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_MISMATCHED_ABI, dir.path());

    SubsystemSupervisor supervisor(dir.path());
    supervisor.start(SubsystemContext{});

    // Refused at discovery time, before any lifecycle thread or retry --
    // never given a chance to run at all.
    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].name == "test_plugin_mismatched_abi");
    CHECK(statuses[0].state == SubsystemState::disabled);
    CHECK(statuses[0].restart_count == 0);

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor",
          test_subsystem_supervisor_silently_skips_a_non_plugin_library) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_NO_ENTRY_SYMBOL, dir.path());

    SubsystemSupervisor supervisor(dir.path());
    supervisor.start(SubsystemContext{});

    // Not reported as a subsystem at all -- a shared library that doesn't
    // export the entry symbol was never a macha plugin, so it must not
    // pollute Status with a fake "disabled" entry (see /usr/bin/ld.so on a
    // real Linux install, where plugin_path defaults to a shared bindir).
    CHECK(supervisor.statuses().empty());

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_empty_directory_loads_nothing) {
    TempDir dir;
    SubsystemSupervisor supervisor(dir.path());
    supervisor.start(SubsystemContext{});
    CHECK(supervisor.statuses().empty());
    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_missing_directory_loads_nothing) {
    TempDir dir;
    const auto missing = dir.path() / "does-not-exist";
    SubsystemSupervisor supervisor(missing);
    supervisor.start(SubsystemContext{});
    CHECK(supervisor.statuses().empty());
    supervisor.stop();
}

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

#include <atomic>
#include <fstream>
#include <stdexcept>
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

// ---- Faults discovered after a successful start -----------------------------
//
// The half Phase 1 left open (subsystem_supervisor.hpp): until
// Subsystem::attach_fault_sink existed the supervisor parked until asked to
// stop, so a subsystem whose own threads died after start() stayed `running`
// forever. A lost FUSE mount is exactly that shape, which is why it is
// designed against a real subsystem now rather than guessed at.

MACHA_TEST("subsystem_supervisor",
          test_subsystem_supervisor_rebuilds_a_subsystem_that_faults_after_starting) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_FAULTING_AFTER_START, dir.path());

    // Room for several restarts before the budget is spent, so the cycle
    // under test is observable rather than racing straight to `disabled`.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 20;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    SubsystemSupervisor supervisor(dir.path(), policy);
    supervisor.start(SubsystemContext{});

    // It reaches `running` on its own merits -- the fault comes later.
    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::running;
    }, 5s));

    // ... and is then rebuilt, with the subsystem's own reason recorded.
    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].restart_count >= 1;
    }, 5s));

    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].last_fault.find("lost its work") != std::string::npos);

    // The replacement runs: a fault is a restart, not a stop.
    REQUIRE(wait_until([&] {
        auto current = supervisor.statuses();
        return current.size() == 1 && current[0].state == SubsystemState::running &&
               current[0].restart_count >= 1;
    }, 5s));

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor",
          test_subsystem_supervisor_disables_a_subsystem_that_keeps_faulting_after_starting) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_FAULTING_AFTER_START, dir.path());

    // A mount that comes up and immediately dies must not remount forever.
    // RetryState::succeeded() deliberately keeps the failure window, so a
    // clean start between faults resets the backoff without resetting the
    // budget -- which is what makes this terminate at all.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 3;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    SubsystemSupervisor supervisor(dir.path(), policy);
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::disabled;
    }, 10s));

    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].restart_count > policy.max_failures_in_window);
    CHECK(statuses[0].last_fault.find("lost its work") != std::string::npos);

    // Terminal: an operator has to act, and nothing retries behind them.
    const auto settled = statuses[0].restart_count;
    std::this_thread::sleep_for(100ms);
    CHECK(supervisor.statuses()[0].state == SubsystemState::disabled);
    CHECK(supervisor.statuses()[0].restart_count == settled);

    supervisor.stop();
}

// ---- Builtins ---------------------------------------------------------------

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_supervises_a_builtin) {
    // A subsystem linked into this binary gets the identical lifecycle to a
    // plugin: same retry/disable policy, same Status entry. This is what lets
    // FUSE move behind the supervisor before its adapter becomes a .so.
    TempDir dir; // no plugins in it, and no plugin directory is fine too.

    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 2;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    struct CountingSubsystem final : Subsystem {
        std::atomic_int* started;
        std::atomic_int* stopped;
        explicit CountingSubsystem(std::atomic_int* s, std::atomic_int* t)
            : started(s), stopped(t) {}
        std::string_view name() const noexcept override { return "counting"; }
        void start() override { ++*started; }
        void stop() override { ++*stopped; }
    };

    std::atomic_int started{};
    std::atomic_int stopped{};

    SubsystemSupervisor supervisor(dir.path() / "no-plugins-here", policy);
    supervisor.add_builtin("fuse", [&](const SubsystemContext&) -> std::unique_ptr<Subsystem> {
        return std::make_unique<CountingSubsystem>(&started, &stopped);
    });
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::running;
    }, 5s));
    CHECK(supervisor.statuses()[0].name == "fuse");
    CHECK(started.load() == 1);

    supervisor.stop();
    CHECK(stopped.load() >= 1);
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_retries_a_builtin_that_cannot_start) {
    // The es-1 shape, with no plugin file involved: a constructor that throws
    // must fault this subsystem and nothing else. Reaching the end of this
    // case at all is the assertion -- an escaped exception would take the
    // whole test process down, not fail a CHECK.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 2;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    SubsystemSupervisor supervisor(std::filesystem::path{}, policy);
    supervisor.add_builtin("fuse", [](const SubsystemContext&) -> std::unique_ptr<Subsystem> {
        throw std::runtime_error("journal replay failed");
    });
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::disabled;
    }, 5s));
    CHECK(supervisor.statuses()[0].last_fault.find("journal replay failed") != std::string::npos);

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_reports_a_declining_builtin) {
    // "This node is not configured to mount" is not a fault: no instance, no
    // retry, and Status says unavailable rather than disabled.
    SubsystemSupervisor supervisor(std::filesystem::path{});
    supervisor.add_builtin("fuse",
                          [](const SubsystemContext&) -> std::unique_ptr<Subsystem> { return {}; });
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::unavailable;
    }, 5s));
    CHECK(supervisor.statuses()[0].restart_count == 0);

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_stops_while_a_factory_is_blocked) {
    // A subsystem may legitimately block for a long time while being built:
    // FuseFrontend waits for this node's first namespace before it can build
    // its inode table, and a node whose metadata replica never arrives would
    // otherwise wait forever. The supervisor hands each attempt its own
    // lifecycle-thread stop token through SubsystemContext, so stopping does
    // not mean waiting out something that may never happen.
    std::atomic_bool entered{};
    std::atomic_bool cancelled{};

    SubsystemSupervisor supervisor(std::filesystem::path{});
    supervisor.add_builtin("blocking", [&](const SubsystemContext& context)
                                           -> std::unique_ptr<Subsystem> {
        entered.store(true);
        // Exactly what a bounded, cancellable wait looks like from inside a
        // factory: poll the token and give up when it is requested.
        while (!context.startup_stop.stop_requested())
            std::this_thread::sleep_for(1ms);
        cancelled.store(true);
        throw std::runtime_error("construction cancelled");
    });
    supervisor.start(SubsystemContext{});

    REQUIRE(wait_until([&] { return entered.load(); }, 5s));

    const auto started = std::chrono::steady_clock::now();
    supervisor.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(cancelled.load());
    CHECK(elapsed < 2s);
}

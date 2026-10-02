// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fault injection for SubsystemSupervisor over real plugins loaded by dlopen. A
// plugin whose construction or start() throws degrades to a per-subsystem
// faulted/disabled state; an escaped exception would crash the case's process.
#include "subsystem/subsystem_supervisor.hpp"
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

    // A successful load is logged at INFO, naming the file it came from.
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

    // A fast retry policy; production defaults would take tens of seconds.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 2;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    SubsystemSupervisor supervisor(dir.path(), policy);
    supervisor.start(SubsystemContext{});

    // Reached without crashing, though the plugin's start() always throws.
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

    // A factory that returns no instance means the capability is configured
    // off: it settles on `unavailable`, not `faulted`, and is never retried.
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

    // Declining is logged too, distinguishing "switched off" from "missing".
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

    // Refused at discovery, before any lifecycle thread or retry.
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

    // A shared library without the entry symbol is not a plugin and gets no Status entry.
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

// Faults a running subsystem reports through its fault sink after start().

MACHA_TEST("subsystem_supervisor",
          test_subsystem_supervisor_rebuilds_a_subsystem_that_faults_after_starting) {
    TempDir dir;
    copy_plugin(MACHA_TEST_PLUGIN_FAULTING_AFTER_START, dir.path());

    // Room for several restarts, so the cycle is observable before `disabled`.
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 20;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;

    SubsystemSupervisor supervisor(dir.path(), policy);
    supervisor.start(SubsystemContext{});

    // It reaches `running` first; the fault comes later.
    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::running;
    }, 5s));

    // Then it is rebuilt, with the subsystem's own reason recorded.
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

    // A subsystem that starts and immediately faults must not restart forever:
    // a clean start resets the backoff but keeps the failure window.
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

    // Terminal: nothing retries.
    const auto settled = statuses[0].restart_count;
    std::this_thread::sleep_for(100ms);
    CHECK(supervisor.statuses()[0].state == SubsystemState::disabled);
    CHECK(supervisor.statuses()[0].restart_count == settled);

    supervisor.stop();
}

MACHA_TEST("subsystem_supervisor", test_subsystem_supervisor_supervises_a_builtin) {
    // A builtin gets the same lifecycle as a plugin: retry/disable policy and Status entry.
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
    // A builtin whose constructor throws faults that subsystem only; an
    // escaped exception would crash the process rather than fail a CHECK.
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
    // A factory may block indefinitely (FuseFrontend waits for the first
    // namespace); SubsystemContext::startup_stop lets stop() cancel it.
    std::atomic_bool entered{};
    std::atomic_bool cancelled{};

    SubsystemSupervisor supervisor(std::filesystem::path{});
    supervisor.add_builtin("blocking", [&](const SubsystemContext& context)
                                           -> std::unique_ptr<Subsystem> {
        entered.store(true);
        // A cancellable wait: poll the token and give up when it is requested.
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

MACHA_TEST("subsystem_supervisor", test_service_stops_plugins_before_the_store) {
    // Plugins write into the store until they stop, so Service::stop stops
    // every plugin before it withdraws DATA admission and outbound RPC.
    TempDir plugins;
    copy_plugin(MACHA_TEST_PLUGIN_WRITES_ON_STOP, plugins.path());
    const auto result = plugins.path() / "result";
    REQUIRE(::setenv("MACHA_TEST_STOP_WRITE_RESULT", result.c_str(), 1) == 0);

    TestService fixture("plugins-stop-before-the-store");
    auto& config = fixture.config();
    config.replication = 1;
    config.min_write_replicas = 1;
    config.metadata_min_write_replicas = 1;
    config.plugin_path = plugins.path();
    auto& service = fixture.start();

    const auto outcome = [&] {
        std::ifstream in(result);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    REQUIRE(wait_until([&] { return outcome() == "started"; }, 10s));

    service.stop();
    CHECK(outcome() == "ok");
    if (outcome() != "ok") std::cerr << "write from stop(): " << outcome() << "\n";
}

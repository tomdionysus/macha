// SPDX-License-Identifier: GPL-3.0-or-later
//
// SubsystemSupervisor: what discovery makes of each kind of file it dlopens,
// and how one entry's lifecycle settles, driven by scripted builtins. A
// subsystem whose construction or start() throws degrades to a per-subsystem
// faulted/disabled state; an escaped exception would crash the case's process.
// Retry policies use a zero backoff, so no outcome waits on real time.
#include "subsystem/subsystem_supervisor.hpp"
#include "test_backend_support.hpp" // ConcurrentCapturingLogger

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
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

SubsystemRetryPolicy immediate_retries(size_t max_failures) {
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = max_failures;
    policy.failure_window = 1h;
    policy.initial_backoff = 0ms;
    policy.max_backoff = 0ms;
    return policy;
}

// The single entry's status once it reaches `state` after `restarts`
// rebuilds; nullopt if it never does.
std::optional<SubsystemStatus> settle_on(const SubsystemSupervisor& supervisor,
                                         SubsystemState state, size_t restarts) {
    std::optional<SubsystemStatus> settled;
    (void)wait_until([&] {
        const auto statuses = supervisor.statuses();
        if (statuses.size() == 1 && statuses.front().state == state &&
            statuses.front().restart_count == restarts)
            settled = statuses.front();
        return settled.has_value();
    });
    return settled;
}

bool logged(const ConcurrentCapturingLogger& capture, LogLevel level,
            std::initializer_list<std::string_view> parts) {
    for (const auto& [record_level, message] : capture.records()) {
        if (record_level != level)
            continue;
        if (std::all_of(parts.begin(), parts.end(), [&](std::string_view part) {
                return message.find(part) != std::string::npos;
            }))
            return true;
    }
    return false;
}

// A builtin subsystem whose start() may report a fault through its sink, and
// which records its lifecycle into a shared journal.
struct Journal {
    std::mutex mutex;
    std::vector<std::string> events;
    void add(std::string event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }
    std::vector<std::string> read() {
        std::lock_guard lock(mutex);
        return events;
    }
};

class ScriptedSubsystem final : public Subsystem {
    Journal& journal_;
    int number_;
    bool fault_in_start_;
    FaultSink sink_;

  public:
    ScriptedSubsystem(Journal& journal, int number, bool fault_in_start)
        : journal_(journal), number_(number), fault_in_start_(fault_in_start) {
        journal_.add("create " + std::to_string(number_));
    }
    ~ScriptedSubsystem() override { journal_.add("destroy " + std::to_string(number_)); }
    std::string_view name() const noexcept override { return "scripted"; }
    void attach_fault_sink(FaultSink sink) override { sink_ = std::move(sink); }
    void start() override {
        journal_.add("start " + std::to_string(number_));
        if (fault_in_start_ && sink_)
            sink_("instance " + std::to_string(number_) + " lost its work");
    }
    void stop() override { journal_.add("stop " + std::to_string(number_)); }
};

} // namespace

// Each kind of file in the plugin directory, loaded through dlopen as in
// production: a plugin's exceptions and fault reports cross the library
// boundary and settle the entry like a builtin's.
MACHA_FAST_TEST("subsystem_supervisor", test_plugin_discovery_settles_each_kind_of_file) {
    struct Row {
        const char* what;
        const char* plugin;      // copied into the directory; null for none
        bool directory_exists;
        std::optional<SubsystemState> settles_on; // nullopt: no Status entry
        bool retried;            // restart_count beyond the failure budget
        const char* fault;       // in last_fault; null for none
        const char* info_log;    // an INFO line naming the plugin; null for none
    };
    const std::vector<Row> rows{
        {"a working plugin runs", MACHA_TEST_PLUGIN_OK, true, SubsystemState::running, false,
         nullptr, "loaded and running"},
        {"a declining plugin is unavailable, never retried", MACHA_TEST_PLUGIN_DECLINING, true,
         SubsystemState::unavailable, false, nullptr, "not enabled on this node"},
        {"a plugin whose start() throws is disabled", MACHA_TEST_PLUGIN_FAULTING, true,
         SubsystemState::disabled, true, "always fails to start", nullptr},
        {"a plugin that keeps faulting after start is disabled",
         MACHA_TEST_PLUGIN_FAULTING_AFTER_START, true, SubsystemState::disabled, true,
         "lost its work", nullptr},
        {"a plugin built for another core is refused", MACHA_TEST_PLUGIN_MISMATCHED_ABI, true,
         SubsystemState::disabled, false, nullptr, nullptr},
        {"a library without the entry symbol is not a plugin", MACHA_TEST_PLUGIN_NO_ENTRY_SYMBOL,
         true, std::nullopt, false, nullptr, nullptr},
        {"an empty directory loads nothing", nullptr, true, std::nullopt, false, nullptr, nullptr},
        {"a missing directory loads nothing", nullptr, false, std::nullopt, false, nullptr,
         nullptr},
    };
    const auto policy = immediate_retries(2);

    for (const auto& row : rows) {
        std::cerr << "row: " << row.what << "\n";
        TempDir temp;
        const auto dir = temp.path() / "plugins";
        if (row.directory_exists)
            std::filesystem::create_directories(dir);
        if (row.plugin)
            copy_plugin(row.plugin, dir);
        const auto stem = row.plugin ? std::filesystem::path(row.plugin).stem().string() : "";

        auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::info);
        Log::set_logger(capture);
        SubsystemSupervisor supervisor(dir, policy);
        supervisor.start(SubsystemContext{});

        if (!row.settles_on) {
            CHECK(supervisor.statuses().empty());
        } else if (row.settles_on == SubsystemState::disabled && !row.retried) {
            // Refused at discovery, before any lifecycle thread or retry.
            const auto statuses = supervisor.statuses();
            REQUIRE(statuses.size() == 1);
            CHECK(statuses[0].name == stem);
            CHECK(statuses[0].state == SubsystemState::disabled);
            CHECK(statuses[0].restart_count == 0);
        } else {
            const auto settled = settle_on(supervisor, *row.settles_on,
                                           row.retried ? policy.max_failures_in_window + 1 : 0);
            REQUIRE(settled.has_value());
            CHECK(settled->name == stem);
            if (row.fault)
                CHECK(settled->last_fault.find(row.fault) != std::string::npos);
            else
                CHECK(settled->last_fault.empty());
        }
        if (row.info_log)
            CHECK(logged(*capture, LogLevel::info, {stem, row.info_log, dir.string()}));

        supervisor.stop();
        CHECK(supervisor.statuses().empty());
        Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));
    }
}

// One builtin's lifecycle from each kind of factory and instance: what Status
// settles on, how often the factory is asked, and that a faulted instance is
// stopped and destroyed before its replacement is built.
MACHA_FAST_TEST("subsystem_supervisor", test_builtin_lifecycle_settles_each_outcome) {
    struct Row {
        const char* what;
        // Whether the factory throws, declines, or builds an instance; and, per
        // instance number (1-based), whether its start() reports a fault.
        enum class Factory { builds, throws, declines } factory;
        std::function<bool(int)> faults;
        SubsystemState settles_on;
        size_t factory_calls;
        size_t restart_count;
        const char* fault;
        std::vector<std::string> journal; // up to and including stop()
    };
    const auto never = [](int) { return false; };
    const auto always = [](int) { return true; };
    const auto first_only = [](int number) { return number == 1; };
    const std::vector<Row> rows{
        {"an instance that starts cleanly runs until stopped", Row::Factory::builds, never,
         SubsystemState::running, 1, 0, nullptr, {"create 1", "start 1", "stop 1", "destroy 1"}},
        {"a factory that throws is retried, then disabled", Row::Factory::throws, never,
         SubsystemState::disabled, 3, 3, "journal replay failed", {}},
        {"a factory that declines is unavailable and never retried", Row::Factory::declines,
         never, SubsystemState::unavailable, 1, 0, nullptr, {}},
        {"an instance that faults after starting is rebuilt in place", Row::Factory::builds,
         first_only, SubsystemState::running, 2, 1, "instance 1 lost its work",
         {"create 1", "start 1", "stop 1", "destroy 1", "create 2", "start 2", "stop 2",
          "destroy 2"}},
        // A clean start resets the backoff but keeps the failure window.
        {"an instance that keeps faulting after starting is disabled", Row::Factory::builds,
         always, SubsystemState::disabled, 3, 3, "instance 3 lost its work",
         {"create 1", "start 1", "stop 1", "destroy 1", "create 2", "start 2", "stop 2",
          "destroy 2", "create 3", "start 3", "stop 3", "destroy 3"}},
    };

    for (const auto& row : rows) {
        std::cerr << "row: " << row.what << "\n";
        Journal journal;
        std::atomic_int calls{};
        SubsystemSupervisor supervisor(std::filesystem::path{}, immediate_retries(2));
        supervisor.add_builtin("fuse", [&](const SubsystemContext&) -> std::unique_ptr<Subsystem> {
            const int number = ++calls;
            switch (row.factory) {
            case Row::Factory::throws:
                throw std::runtime_error("journal replay failed");
            case Row::Factory::declines:
                return {};
            case Row::Factory::builds:
                break;
            }
            return std::make_unique<ScriptedSubsystem>(journal, number, row.faults(number));
        });
        supervisor.start(SubsystemContext{});

        const auto settled = settle_on(supervisor, row.settles_on, row.restart_count);
        REQUIRE(settled.has_value());
        CHECK(settled->name == "fuse");
        if (row.fault)
            CHECK(settled->last_fault == row.fault);
        else
            CHECK(settled->last_fault.empty());

        supervisor.stop();
        CHECK(static_cast<size_t>(calls.load()) == row.factory_calls);
        CHECK(journal.read() == row.journal);
    }
}

// A factory may block indefinitely (FuseFrontend waits for the first
// namespace); SubsystemContext::startup_stop lets stop() cancel it.
MACHA_FAST_TEST("subsystem_supervisor", test_stop_cancels_a_blocked_factory) {
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::atomic_bool cancelled{};

    SubsystemSupervisor supervisor(std::filesystem::path{});
    supervisor.add_builtin("blocking", [&](const SubsystemContext& context)
                                           -> std::unique_ptr<Subsystem> {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        entered.set_value();
        // A cancellable wait that returns only when the token is requested.
        cv.wait(lock, context.startup_stop, [] { return false; });
        cancelled.store(true);
        throw std::runtime_error("construction cancelled");
    });
    supervisor.start(SubsystemContext{});
    REQUIRE(entered_future.wait_for(scaled(5s)) == std::future_status::ready);

    // Without cancellation stop() joins a thread that never returns, and the
    // case's deadline fails it.
    supervisor.stop();
    CHECK(cancelled.load());
}

// Kept integrated: the order is Service::stop's, the composition root's, and
// the write that proves it lands in the real store.
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

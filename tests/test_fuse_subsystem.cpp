// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crash isolation for the FUSE subsystem: a mount that cannot be built, or dies
// after it was built, degrades to a per-subsystem faulted/restarting state
// while the node's metadata, RPC, HTTP API and playback keep serving.
//
// libfuse sits behind FuseMountDriver, so these run without a kernel mount.
#include "fuse/fuse_frontend.hpp"
#include "fuse/fuse_subsystem.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "subsystem/subsystem_supervisor.hpp"
#include "test_backend_support.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

using namespace macha;
using namespace macha::test_support;
using namespace std::chrono_literals;

namespace {

// Stands in for libfuse: "mounts" by returning, serves by blocking, and ends
// either cleanly (an ordinary stop) or by losing the mount (a fault).
struct MountControl {
    std::mutex mutex;
    std::condition_variable cv;
    bool lose_mount{};
    bool fail_to_mount{};
    int runs{};
    bool mounted{};

    void wait_mounted() {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 10s, [&] { return mounted; }));
    }
    void lose() {
        std::lock_guard lock(mutex);
        lose_mount = true;
        cv.notify_all();
    }
    int run_count() {
        std::lock_guard lock(mutex);
        return runs;
    }
};

class TestMountDriver final : public FuseMountDriver {
    std::shared_ptr<MountControl> control_;
    // Per instance: the supervisor stops the faulted driver while building the
    // next, so a shared flag would end the rebuilt mount at once.
    bool exit_requested_{};

  public:
    explicit TestMountDriver(std::shared_ptr<MountControl> control)
        : control_(std::move(control)) {}

    FuseMountOutcome run(const FuseMountContext& context, std::stop_token stop) override {
        CHECK(context.frontend != nullptr);
        CHECK(context.filesystem != nullptr);
        CHECK(context.config != nullptr);

        std::unique_lock lock(control_->mutex);
        ++control_->runs;
        if (control_->fail_to_mount)
            return {false, "fuse_mount failed for " + context.mount_path.string()};
        control_->mounted = true;
        control_->cv.notify_all();

        control_->cv.wait(lock, [&] {
            return exit_requested_ || control_->lose_mount || stop.stop_requested();
        });
        control_->mounted = false;
        if (control_->lose_mount) {
            control_->lose_mount = false; // one loss per mount attempt.
            return {false, "the mount disappeared"};
        }
        return {true, {}};
    }

    void request_exit() override {
        std::lock_guard lock(control_->mutex);
        exit_requested_ = true;
        control_->cv.notify_all();
    }
};

std::shared_ptr<MountControl> install_test_mount_driver() {
    auto control = std::make_shared<MountControl>();
    set_fuse_mount_driver_factory([control]() -> std::unique_ptr<FuseMountDriver> {
        return std::make_unique<TestMountDriver>(control);
    });
    return control;
}

SubsystemRetryPolicy fast_policy() {
    SubsystemRetryPolicy policy;
    policy.max_failures_in_window = 20;
    policy.failure_window = 60s;
    policy.initial_backoff = 5ms;
    policy.max_backoff = 20ms;
    return policy;
}

// A Config the test owns, so the mount and spool paths can change between attempts.
Config mountable_config(const Config& base, const std::filesystem::path& mount_path) {
    Config config = base;
    config.fuse.mount_path = mount_path;
    // Tests do not run as root, and the immutable flag is not under test.
    config.fuse.fail_closed_mountpoint = false;
    config.fuse.publication_quiet = 0ms;
    return config;
}

SubsystemContext context_for(Config& config, Service& service, SubsystemRegistry& registry) {
    SubsystemContext context;
    context.config = &config;
    context.node = &service.node();
    context.registry = &registry;
    context.filesystem = &service.filesystem();
    context.hydration = &service.hydration();
    return context;
}

std::optional<SubsystemStatus> fuse_status(const SubsystemSupervisor& supervisor) {
    for (const auto& status : supervisor.statuses())
        if (status.name == "fuse")
            return status;
    return std::nullopt;
}

} // namespace

MACHA_TEST("fuse_subsystem", test_fuse_subsystem_mounts_and_publishes_its_frontend) {
    TestService fixture("fuse-subsystem-mounts", ConfigProfile::isolated);
    auto& service = fixture.start();
    auto control = install_test_mount_driver();

    auto config = mountable_config(fixture.config(), fixture.path() / "mnt");
    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(std::filesystem::path{}, fast_policy());
    supervisor.add_builtin("fuse", make_fuse_subsystem);
    supervisor.start(context_for(config, service, registry));

    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->state == SubsystemState::running;
    }, 20s));
    control->wait_mounted();

    // Published and usable: nothing core asks the frontend for needs the kernel.
    auto frontend = registry.fuse();
    REQUIRE(frontend);
    CHECK(!frontend->blocked_namespace_operation().has_value());
    CHECK(frontend->parked_publications().empty());

    supervisor.stop();

    // Withdrawn on stop, so nothing can reach a stopped frontend.
    CHECK(!registry.fuse());
    CHECK(fuse_status(supervisor) == std::nullopt); // stop() clears the entries.
    CHECK(control->run_count() == 1);
}

MACHA_TEST("fuse_subsystem", test_fuse_subsystem_clean_stop_is_not_a_fault) {
    TestService fixture("fuse-subsystem-clean-stop", ConfigProfile::isolated);
    auto& service = fixture.start();
    auto control = install_test_mount_driver();

    auto config = mountable_config(fixture.config(), fixture.path() / "mnt");
    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(std::filesystem::path{}, fast_policy());
    supervisor.add_builtin("fuse", make_fuse_subsystem);
    supervisor.start(context_for(config, service, registry));

    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->state == SubsystemState::running;
    }, 20s));
    control->wait_mounted();

    auto before = fuse_status(supervisor);
    REQUIRE(before);
    CHECK(before->restart_count == 0);
    CHECK(before->last_fault.empty());

    supervisor.stop();
    // A requested unmount is not a fault: nothing is retried.
    CHECK(control->run_count() == 1);
}

MACHA_TEST("fuse_subsystem", test_fuse_subsystem_rebuilds_the_mount_after_losing_it) {
    TestService fixture("fuse-subsystem-mount-loss", ConfigProfile::isolated);
    auto& service = fixture.start();
    auto control = install_test_mount_driver();

    auto config = mountable_config(fixture.config(), fixture.path() / "mnt");
    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(std::filesystem::path{}, fast_policy());
    supervisor.add_builtin("fuse", make_fuse_subsystem);
    supervisor.start(context_for(config, service, registry));

    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->state == SubsystemState::running;
    }, 20s));
    control->wait_mounted();
    auto first = registry.fuse();
    REQUIRE(first);

    // The mount goes away underneath a running node (`umount -l`, a module
    // reload, a vanished mountpoint).
    control->lose();

    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->restart_count >= 1;
    }, 20s));
    CHECK(fuse_status(supervisor)->last_fault.find("disappeared") != std::string::npos);

    // It is rebuilt: a new mount attempt, a new frontend, no process restart.
    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->state == SubsystemState::running && control->run_count() >= 2;
    }, 20s));
    control->wait_mounted();

    auto second = registry.fuse();
    REQUIRE(second);
    CHECK(second.get() != first.get());

    // The rest of the node is unaffected.
    CHECK(service.filesystem().getattr("/").type == EntryType::directory);
    CHECK(service.ready());

    supervisor.stop();
}

MACHA_TEST("fuse_subsystem",
          test_fuse_subsystem_construction_fault_leaves_the_rest_of_the_node_serving) {
    TestService fixture("fuse-subsystem-construction-fault", ConfigProfile::isolated);
    auto& service = fixture.start();
    auto control = install_test_mount_driver();

    auto config = mountable_config(fixture.config(), fixture.path() / "mnt");

    // A spool directory that cannot exist (its parent is a regular file), so
    // construction fails before any FUSE request is served.
    const auto blocker = fixture.path() / "not-a-directory";
    { std::ofstream out(blocker); out << "x"; }
    config.fuse.spool_path = blocker / "spool";
    config.fuse.operation_journal_path = blocker / "spool" / "operations.log";

    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(std::filesystem::path{}, fast_policy());
    supervisor.add_builtin("fuse", make_fuse_subsystem);
    supervisor.start(context_for(config, service, registry));

    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->restart_count >= 2;
    }, 20s));

    auto status = fuse_status(supervisor);
    REQUIRE(status);
    CHECK((status->state == SubsystemState::faulted ||
           status->state == SubsystemState::restarting ||
           status->state == SubsystemState::disabled));
    CHECK(!status->last_fault.empty());
    CHECK(!registry.fuse());
    CHECK(control->run_count() == 0); // never got as far as mounting.

    // The node still serves its namespace, and the manage endpoints answer
    // "nothing to report" rather than failing.
    CHECK(service.ready());
    CHECK(service.filesystem().getattr("/").type == EntryType::directory);
    CHECK(!service.blocked_namespace_operation().has_value());
    CHECK(!service.skip_blocked_namespace_operation(1));
    CHECK(service.parked_publications().empty());
    CHECK(!service.retry_parked_publication(1));
    CHECK(!service.abandon_parked_publication(1));

    // Once the cause is fixed the next attempt succeeds, with nothing else restarted.
    std::filesystem::remove(blocker);
    config.fuse.spool_path = fixture.path() / "spool";
    config.fuse.operation_journal_path = fixture.path() / "spool" / "operations.log";

    REQUIRE(wait_until([&] {
        auto current = fuse_status(supervisor);
        return current && current->state == SubsystemState::running;
    }, 20s));
    CHECK(registry.fuse());

    supervisor.stop();
}

MACHA_TEST("fuse_subsystem", test_fuse_subsystem_without_a_mount_path_is_unavailable) {
    TestService fixture("fuse-subsystem-no-mount-path", ConfigProfile::isolated);
    auto& service = fixture.start();
    auto control = install_test_mount_driver();

    Config config = fixture.config();
    config.fuse.mount_path.reset();

    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(std::filesystem::path{}, fast_policy());
    supervisor.add_builtin("fuse", make_fuse_subsystem);
    supervisor.start(context_for(config, service, registry));

    // No mount configured is a choice, not a fault: `unavailable`, no retries.
    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->state == SubsystemState::unavailable;
    }, 10s));
    std::this_thread::sleep_for(100ms);
    CHECK(fuse_status(supervisor)->state == SubsystemState::unavailable);
    CHECK(fuse_status(supervisor)->restart_count == 0);
    CHECK(control->run_count() == 0);
    CHECK(!registry.fuse());

    supervisor.stop();
}

MACHA_TEST("fuse_subsystem", test_fuse_subsystem_without_a_mount_driver_is_unavailable) {
    TestService fixture("fuse-subsystem-no-driver", ConfigProfile::isolated);
    auto& service = fixture.start();
    // No driver: a node with no FUSE adapter (no libmacha-fuse installed).
    set_fuse_mount_driver_factory({});

    auto config = mountable_config(fixture.config(), fixture.path() / "mnt");
    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(std::filesystem::path{}, fast_policy());
    supervisor.add_builtin("fuse", make_fuse_subsystem);
    supervisor.start(context_for(config, service, registry));

    REQUIRE(wait_until([&] {
        auto status = fuse_status(supervisor);
        return status && status->state == SubsystemState::unavailable;
    }, 10s));
    // No amount of retrying links a mount adapter into a binary that has none.
    CHECK(fuse_status(supervisor)->restart_count == 0);
    CHECK(!registry.fuse());

    supervisor.stop();
}

#ifdef MACHA_TEST_FUSE_PLUGIN
MACHA_TEST("fuse_subsystem", test_fuse_plugin_loads_over_the_real_dlopen_path) {
    // The adapter is a real dlopen'd module: discovery, entry symbol and
    // build-identity check. No mount path is configured (test hosts have no
    // kernel mount), so the plugin must decline cleanly as `unavailable`.
    TestService fixture("fuse-plugin-dlopen", ConfigProfile::isolated);
    auto& service = fixture.start();

    TempDir plugin_dir;
    const std::filesystem::path plugin = MACHA_TEST_FUSE_PLUGIN;
    std::error_code ec;
    std::filesystem::copy_file(plugin, plugin_dir.path() / plugin.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);

    Config config = fixture.config();
    config.fuse.mount_path.reset();

    SubsystemRegistry registry;
    SubsystemSupervisor supervisor(plugin_dir.path(), fast_policy());
    supervisor.start(context_for(config, service, registry));

    REQUIRE(wait_until([&] {
        auto statuses = supervisor.statuses();
        return statuses.size() == 1 && statuses[0].state == SubsystemState::unavailable;
    }, 10s));

    auto statuses = supervisor.statuses();
    REQUIRE(statuses.size() == 1);
    // Named for the file it came from, so Status says which file to look for.
    CHECK(statuses[0].name == plugin.stem().string());
    CHECK(statuses[0].restart_count == 0);
    CHECK(statuses[0].last_fault.empty());
    CHECK(!registry.fuse());

    supervisor.stop();
}
#endif

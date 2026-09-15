// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crash-isolation UAT for the FUSE subsystem (see
// TODO/2026-09-14-fuse-supervised-subsystem-plan.md, Stage A). The claim under
// test is the one the whole exercise exists for: a FUSE mount that cannot be
// built, or that dies after it was built, degrades to a per-subsystem
// faulted/restarting state while this node's metadata, RPC, HTTP API and
// playback keep serving. Before this, FuseFrontend's constructor threw into
// main() -- a 0.24.3 journal-bookkeeping bug crash-looped corvus-es-1 49 times
// that way -- and a lost mount called Service::request_stop().
//
// libfuse is behind FuseMountDriver so these run without a kernel mount, which
// is what lets them run at all: no test host here has ever had one, and the
// mount lifecycle is precisely the part that was never covered.
#include "fuse_frontend.hpp"
#include "fuse_subsystem.hpp"
#include "subsystem_registry.hpp"
#include "subsystem_supervisor.hpp"
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

// Stands in for libfuse: it "mounts" by returning from the call that
// establishes the mount, serves by blocking, and can be told to end either
// cleanly (an ordinary stop) or by losing the mount (a fault).
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
    // Per instance, deliberately: each mount attempt gets a fresh driver, and
    // the supervisor stops the faulted one on its way to building the next.
    // Sharing this flag made a rebuilt mount exit the moment it came up.
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

// A Config the test owns, so a mount path (and a deliberately broken spool
// path) can be changed between one attempt and the next.
Config mountable_config(const Config& base, const std::filesystem::path& mount_path) {
    Config config = base;
    config.fuse.mount_path = mount_path;
    // Nothing here is root, and the immutable flag is not what is under test.
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

    // Published for core to find, and usable: the frontend answers before,
    // during and after a mount, because none of what core asks it for needs
    // the kernel.
    auto frontend = registry.fuse();
    REQUIRE(frontend);
    CHECK(!frontend->blocked_namespace_operation().has_value());
    CHECK(frontend->parked_publications().empty());

    supervisor.stop();

    // Withdrawn on the way out, so nothing can reach a stopped frontend.
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
    // An unmount we asked for is not a fault: nothing retried, nothing to
    // tell an operator about.
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

    // The mount goes away underneath a running node -- `umount -l`, a kernel
    // module reload, a mountpoint that vanished. Until this work that called
    // Service::request_stop() and exited the process with code 8.
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

    // The rest of the node never noticed.
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

    // A spool directory that cannot exist: its parent is a regular file. This
    // is the es-1 shape in the form it can still take after discipline 3 made
    // journal replay itself survive every frame mutation -- construction
    // fails before a single FUSE request could ever be served.
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

    // Everything this subsystem is not: the node still serves its namespace,
    // and the manage endpoints answer "nothing to report" rather than failing.
    CHECK(service.ready());
    CHECK(service.filesystem().getattr("/").type == EntryType::directory);
    CHECK(!service.blocked_namespace_operation().has_value());
    CHECK(!service.skip_blocked_namespace_operation(1));
    CHECK(service.parked_publications().empty());
    CHECK(!service.retry_parked_publication(1));
    CHECK(!service.abandon_parked_publication(1));

    // Fix what was wrong and the next attempt succeeds, with no restart of
    // anything else -- the point of retrying in place.
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

    // Not configured to mount is a deliberate operator choice, not a fault:
    // `unavailable`, no retry loop, nothing for anyone to fix.
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
    // Deliberately no driver: this is a build with no FUSE adapter linked,
    // which after Stage B is also a node with no libmacha-fuse installed.
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
    // Stage B: the adapter is a real dlopen'd module, so the load path this
    // exercises is discovery, entry symbol, and build-identity check against
    // the running core -- the part that a partial deploy gets wrong. The
    // mount itself cannot be attempted here (no kernel mount on any test
    // host), so this configures no mount path: the plugin must then decline
    // cleanly and report `unavailable`, which is also what a node that
    // installs the plugin without configuring a mount looks like.
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
    // Named for the file it came from, so Status says which file to look for
    // on disk -- the same contract libmacha-torrent has.
    CHECK(statuses[0].name == plugin.stem().string());
    CHECK(statuses[0].restart_count == 0);
    CHECK(statuses[0].last_fault.empty());
    CHECK(!registry.fuse());

    supervisor.stop();
}
#endif

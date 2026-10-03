// SPDX-License-Identifier: GPL-3.0-or-later
// The driver-agnostic half of the FUSE subsystem; nothing here links libfuse.
#include "fuse/fuse_subsystem.hpp"

#include "filesystem/filesystem.hpp"
#include "fuse/fuse_frontend.hpp"
#include "fuse/fuse_mountpoint.hpp"
#include "filesystem/hydration.hpp"
#include "log.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "supervised.hpp"

#include <stdexcept>
#include <utility>

namespace macha {
namespace {

FuseMountDriverFactory& mount_driver_factory_storage() {
    static FuseMountDriverFactory factory;
    return factory;
}

} // namespace

void set_fuse_mount_driver_factory(FuseMountDriverFactory factory) {
    mount_driver_factory_storage() = std::move(factory);
}

const FuseMountDriverFactory& fuse_mount_driver_factory() {
    return mount_driver_factory_storage();
}

FuseSubsystem::FuseSubsystem(const SubsystemContext& context,
                             std::unique_ptr<FuseMountDriver> driver)
    : registry_(*context.registry), hydration_(*context.hydration),
      filesystem_(*context.filesystem), config_(context.config->fuse),
      mount_path_(*context.config->fuse.mount_path), driver_(std::move(driver)) {
    // Per attempt, not per process: a remount after mount loss needs the
    // stale-mount recovery and the fail-closed guard on the covered directory.
    prepare_fuse_mountpoint(mount_path_, config_);
    std::filesystem::create_directories(mount_path_);

    // Journal replay and the wait for the first namespace happen here, not in
    // start(), so a failure is a construction fault the supervisor retries.
    frontend_ = std::make_shared<FuseFrontend>(filesystem_, *context.retained_memory, config_,
                                               context.startup_stop);

    // Published before start(): Status and the manage endpoints use the
    // frontend without a kernel mount. The destructor withdraws it.
    registry_.publish_fuse(frontend_);
    hydration_.add_provider(frontend_);
}

FuseSubsystem::~FuseSubsystem() {
    stop();
}

void FuseSubsystem::attach_fault_sink(FaultSink sink) {
    Lock lock(fault_mutex_);
    fault_sink_ = std::move(sink);
}

void FuseSubsystem::start() {
    if (mount_.joinable())
        return;
    mount_ = std::jthread([this](std::stop_token stop) {
        run_supervised_once("fuse-mount", [this, stop] { run_mount(stop); });
    });
}

void FuseSubsystem::run_mount(std::stop_token stop) {
    FuseMountContext context;
    context.frontend = frontend_.get();
    context.filesystem = &filesystem_;
    context.mount_path = mount_path_;
    context.config = &config_;

    FuseMountOutcome outcome;
    try {
        outcome = driver_->run(context, stop);
    } catch (const std::exception& e) {
        outcome.clean = false;
        outcome.error = e.what();
    } catch (...) {
        outcome.clean = false;
        outcome.error = "unknown exception";
    }

    if (outcome.clean || stop.stop_requested() || stopping_.load(std::memory_order_acquire))
        return;

    // An unrequested end is a subsystem fault; the rest of the node keeps serving.
    report_fault(outcome.error.empty() ? std::string("FUSE mount ended unexpectedly")
                                       : outcome.error);
}

void FuseSubsystem::report_fault(std::string reason) {
    FaultSink sink;
    {
        Lock lock(fault_mutex_);
        sink = fault_sink_;
    }
    Log::error("FUSE mount fault at " + mount_path_.string() + ": " + reason);
    if (sink)
        sink(std::move(reason));
}

void FuseSubsystem::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel))
        return;

    // Withdraw first: existing callers keep their shared_ptr; new ones see the
    // capability absent rather than racing teardown.
    registry_.withdraw_fuse(frontend_.get());
    if (frontend_)
        hydration_.remove_provider(frontend_.get());

    // An fsync waiting on a publication that cannot finish while stopping
    // would hold the mount open and block the join.
    if (frontend_)
        frontend_->interrupt_waits();
    if (mount_.joinable()) {
        mount_.request_stop();
        driver_->request_exit();
        mount_.join();
    }

    if (frontend_)
        frontend_->stop();
}

std::unique_ptr<Subsystem> make_fuse_subsystem(const SubsystemContext& context) {
    if (!context.config || !context.registry || !context.filesystem || !context.hydration)
        throw std::runtime_error("fuse subsystem requires config, registry, filesystem and "
                                 "hydration in its context");

    if (!context.config->fuse.mount_path) {
        Log::info("fuse subsystem not started: no fuse.mount_path is configured");
        return {};
    }

    const auto& factory = fuse_mount_driver_factory();
    if (!factory) {
        // Capability absence, not a fault: retrying cannot help.
        Log::error("fuse.mount_path is configured but this build has no FUSE mount adapter; "
                   "the namespace will not be mounted");
        return {};
    }

    auto driver = factory();
    if (!driver) {
        Log::error("the FUSE mount adapter declined to provide a driver; the namespace will "
                   "not be mounted");
        return {};
    }

    return std::make_unique<FuseSubsystem>(context, std::move(driver));
}

} // namespace macha

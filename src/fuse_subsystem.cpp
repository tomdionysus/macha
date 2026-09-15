// SPDX-License-Identifier: GPL-3.0-or-later
// The driver-agnostic half of the FUSE subsystem. Nothing here links libfuse;
// see fuse_subsystem.hpp and TODO/2026-09-14-fuse-supervised-subsystem-plan.md.
#include "fuse_subsystem.hpp"

#include "filesystem.hpp"
#include "fuse_frontend.hpp"
#include "fuse_mountpoint.hpp"
#include "hydration.hpp"
#include "log.hpp"
#include "subsystem_registry.hpp"
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
    // Recover a stale Macha mount left by an unclean exit and re-establish the
    // fail-closed guard over the covered directory. Done per attempt, not once
    // per process: a remount after an unexpected mount loss is exactly the
    // case the stale-mount recovery exists for, and by then the covered
    // directory has been left non-writable on purpose.
    prepare_fuse_mountpoint(mount_path_, config_);
    std::filesystem::create_directories(mount_path_);

    // Constructing the frontend is the expensive, failure-prone part: it
    // replays the durable operation journal and waits for this node's first
    // namespace. Doing it here rather than in start() is deliberate -- a
    // throw is then a construction fault the supervisor can retry, which is
    // precisely the isolation this subsystem exists to provide.
    frontend_ = std::make_shared<FuseFrontend>(filesystem_, config_, context.startup_stop);

    // Published before start() for the same reason the torrent plugin
    // publishes in its constructor: the frontend is usable the moment it
    // exists (Status diagnostics, the blocked-namespace and parked-publication
    // manage endpoints all read it) and none of that needs a kernel mount. If
    // start() throws, the supervisor destroys this object and the destructor
    // withdraws it again.
    registry_.publish_fuse(frontend_);
    hydration_.add_provider(frontend_);
}

FuseSubsystem::~FuseSubsystem() {
    stop();
}

void FuseSubsystem::attach_fault_sink(FaultSink sink) {
    std::lock_guard lock(fault_mutex_);
    fault_sink_ = std::move(sink);
}

void FuseSubsystem::start() {
    if (mount_.joinable())
        return;
    mount_ = std::jthread([this](std::stop_token stop) {
        run_supervised("fuse-mount", [this, stop] { run_mount(stop); });
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

    // The mount ended and nobody asked it to. Before this existed the same
    // event called Service::request_stop() and returned an exit code, taking
    // metadata, RPC, the HTTP API and playback down with the mount.
    report_fault(outcome.error.empty() ? std::string("FUSE mount ended unexpectedly")
                                       : outcome.error);
}

void FuseSubsystem::report_fault(std::string reason) {
    FaultSink sink;
    {
        std::lock_guard lock(fault_mutex_);
        sink = fault_sink_;
    }
    Log::error("FUSE mount fault at " + mount_path_.string() + ": " + reason);
    if (sink)
        sink(std::move(reason));
}

void FuseSubsystem::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel))
        return;

    // Withdraw first: a caller already inside a manage handler keeps the
    // shared_ptr it took and finishes against this instance, while a new
    // caller sees the capability as absent rather than racing the teardown.
    registry_.withdraw_fuse(frontend_.get());
    if (frontend_)
        hydration_.remove_provider(frontend_.get());

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
        // Not a fault: this node is simply not configured to mount. Same
        // shape as torrent.enabled being false.
        Log::info("fuse subsystem not started: no fuse.mount_path is configured");
        return {};
    }

    const auto& factory = fuse_mount_driver_factory();
    if (!factory) {
        // No amount of retrying will link a FUSE adapter into this build, so
        // this is capability absence, not a fault to back off from.
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

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "subsystem.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace macha {

class FileSystem;
class FuseFrontend;
class HydrationManager;
class SubsystemRegistry;

// What a mount driver is given to work with. Every pointer is owned by the
// FuseSubsystem and outlives the driver's run().
struct FuseMountContext {
    FuseFrontend* frontend{};
    FileSystem* filesystem{};
    std::filesystem::path mount_path;
    const FuseConfig* config{};
};

struct FuseMountOutcome {
    // True only when the mount ended because it was asked to. Anything else
    // -- the kernel mount disappearing, the event loop returning an error --
    // is a fault, and the supervisor rebuilds the subsystem.
    bool clean{};
    std::string error;
};

// The kernel-facing half of the FUSE subsystem, behind an interface so the
// rest of it can be exercised without a kernel mount. libfuse is the only
// native crash surface here and the only part that cannot run in a test
// process; separating it is also what lets the whole adapter move into
// libmacha-fuse in Stage B without anything else moving with it. See
// TODO/2026-09-14-fuse-supervised-subsystem-plan.md.
class FuseMountDriver {
  public:
    virtual ~FuseMountDriver() = default;

    // Establish the mount and serve it until `stop` is requested, the mount
    // is lost, or request_exit() is called. Returns having unmounted.
    // Throwing is equivalent to returning a non-clean outcome.
    virtual FuseMountOutcome run(const FuseMountContext&, std::stop_token) = 0;

    // Make a run() in progress return. Safe from any thread, and safe to call
    // when run() has already returned or never started.
    virtual void request_exit() = 0;
};

using FuseMountDriverFactory = std::function<std::unique_ptr<FuseMountDriver>()>;

// The libfuse driver registers itself here at static-initialisation time,
// the same runtime-registration seam macha_core already uses for the FFmpeg
// media engine. macha_core must not link libfuse: in Stage A the adapter is
// compiled into the `macha` executable, in Stage B it is inside
// libmacha-fuse, and in a test process there is no adapter at all. Nothing
// registered means this build cannot mount, which is a capability that is
// absent rather than a fault.
void set_fuse_mount_driver_factory(FuseMountDriverFactory);
const FuseMountDriverFactory& fuse_mount_driver_factory();

// One supervised FUSE mount: owns the FuseFrontend (whose construction
// replays the durable journal -- the operation that crash-looped corvus-es-1
// 49 times before any of this existed) and the mount thread, publishes the
// frontend for core to find, and reports a lost mount as a subsystem fault
// instead of taking the process down.
class FuseSubsystem final : public Subsystem {
  public:
    // Throws if the frontend cannot be constructed -- journal replay, spool
    // access, or the initial namespace never arriving. The supervisor treats
    // that as an ordinary construction fault: backoff, retry, and eventually
    // `disabled`, while the rest of the node keeps serving.
    FuseSubsystem(const SubsystemContext&, std::unique_ptr<FuseMountDriver>);
    ~FuseSubsystem() override;

    std::string_view name() const noexcept override { return "fuse"; }
    void start() override;
    void stop() override;
    void attach_fault_sink(FaultSink) override;

    // The frontend this subsystem owns. For tests; core reaches the live one
    // through SubsystemRegistry::fuse().
    const std::shared_ptr<FuseFrontend>& frontend() const noexcept { return frontend_; }

  private:
    void run_mount(std::stop_token);
    void report_fault(std::string reason);

    SubsystemRegistry& registry_;
    HydrationManager& hydration_;
    FileSystem& filesystem_;
    FuseConfig config_;
    std::filesystem::path mount_path_;
    std::unique_ptr<FuseMountDriver> driver_;
    std::shared_ptr<FuseFrontend> frontend_;

    std::mutex fault_mutex_;
    FaultSink fault_sink_;
    std::atomic_bool stopping_{};

    std::jthread mount_;
};

// Subsystem factory for FUSE, with the same contract as a plugin's entry
// point: no instance means this node is not configured to mount (or this
// build has no adapter), and throwing means the attempt failed.
std::unique_ptr<Subsystem> make_fuse_subsystem(const SubsystemContext&);

} // namespace macha

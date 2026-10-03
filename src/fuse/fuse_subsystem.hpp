// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "config.hpp"
#include "subsystem/subsystem.hpp"

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

// Every pointer is owned by the FuseSubsystem and outlives the driver's run().
struct FuseMountContext {
    FuseFrontend* frontend{};
    FileSystem* filesystem{};
    std::filesystem::path mount_path;
    const FuseConfig* config{};
};

struct FuseMountOutcome {
    // True only when the mount ended on request; anything else is a fault and
    // the supervisor rebuilds the subsystem.
    bool clean{};
    std::string error;
};

// The kernel-facing (libfuse) half of the FUSE subsystem, behind an interface
// so the rest runs without a kernel mount, in tests or a separate library.
class FuseMountDriver {
  public:
    virtual ~FuseMountDriver() = default;

    // Mounts and serves until `stop`, mount loss or request_exit(); returns
    // unmounted. Throwing equals a non-clean outcome.
    virtual FuseMountOutcome run(const FuseMountContext&, std::stop_token) = 0;

    // Makes a running run() return. Any thread; safe before or after run().
    virtual void request_exit() = 0;
};

using FuseMountDriverFactory = std::function<std::unique_ptr<FuseMountDriver>()>;

// The libfuse driver registers itself here at static initialisation, so
// macha_core never links libfuse. Nothing registered means this build cannot
// mount: an absent capability, not a fault.
void set_fuse_mount_driver_factory(FuseMountDriverFactory);
const FuseMountDriverFactory& fuse_mount_driver_factory();

// One supervised FUSE mount: owns the FuseFrontend (whose construction replays
// the durable journal) and the mount thread, publishes the frontend, and
// reports a lost mount as a subsystem fault rather than ending the process.
class FuseSubsystem final : public Subsystem {
  public:
    // Throws if the frontend cannot be built (journal replay, spool access, no
    // initial namespace): a construction fault the supervisor retries.
    FuseSubsystem(const SubsystemContext&, std::unique_ptr<FuseMountDriver>);
    ~FuseSubsystem() override;

    std::string_view name() const noexcept override { return "fuse"; }
    void start() override;
    void stop() override;
    void attach_fault_sink(FaultSink) override;

    // For tests; core uses SubsystemRegistry::fuse().
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

    Mutex fault_mutex_;
    FaultSink fault_sink_ MACHA_GUARDED_BY(fault_mutex_);
    std::atomic_bool stopping_{};

    std::jthread mount_;
};

// Plugin entry-point contract: null means not configured to mount or no
// adapter in this build; throwing means the attempt failed.
std::unique_ptr<Subsystem> make_fuse_subsystem(const SubsystemContext&);

} // namespace macha

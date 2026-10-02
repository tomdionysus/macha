// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace macha {

// Lifecycle state the supervisor tracks per subsystem; held by the supervisor,
// not the instance, which may already be destroyed when asked.
enum class SubsystemState {
    unavailable, // no plugin file present for this capability
    starting,
    running,
    faulted,     // most recent attempt threw/crashed; may retry
    restarting,
    disabled,    // too many failures in the configured window; needs an operator
};

std::string_view subsystem_state_name(SubsystemState) noexcept;

class FileSystem;
class HydrationManager;
class IngestManager;
class NodeRuntime;
class DataResourceArbiter;
class RetainedMemoryLedger;
class JobRoutes;
class SubsystemRegistry;

// The core references a subsystem may use: `node` and `ingest` for Torrent,
// `filesystem` and `hydration` for FUSE; `registry` is where a plugin publishes
// its capability. Extend only for a real need. All non-owning; Service owns
// them and stops the supervisor before destroying any.
struct SubsystemContext {
    const Config* config{};
    NodeRuntime* node{};
    DataResourceArbiter* data_resources{};
    RetainedMemoryLedger* retained_memory{};
    JobRoutes* job_routes{};
    IngestManager* ingest{};
    SubsystemRegistry* registry{};
    FileSystem* filesystem{};
    HydrationManager* hydration{};
    // Cancels a construction that may block indefinitely (FUSE waits for the
    // first namespace); the lifecycle thread's token, per attempt. A factory
    // that cannot block may ignore it.
    std::stop_token startup_stop;
};

// A subsystem run behind a SubsystemSupervisor, as a plugin or builtin. A
// faulted instance is destroyed and reconstructed in place.
class Subsystem {
  public:
    // Reports a fault found after start() returned; the reason becomes
    // SubsystemStatus::last_fault.
    using FaultSink = std::function<void(std::string reason)>;

    virtual ~Subsystem() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

    // Installed between create() and start(). Calling the sink makes the
    // supervisor destroy this instance and retry a fresh one under the
    // backoff/disable policy, as for a failed start(). Callable from any of the
    // subsystem's threads, including inside start(); the first reason wins and
    // calls once stopping has begun are ignored. Subsystems that only fail in
    // construction or start() may ignore it.
    virtual void attach_fault_sink(FaultSink sink) { (void)sink; }
};

} // namespace macha

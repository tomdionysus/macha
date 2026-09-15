// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace macha {

// Lifecycle state a SubsystemSupervisor tracks for one loaded subsystem. This
// is bookkeeping the supervisor owns, not something a Subsystem instance
// reports about itself: a faulted instance may already have been destroyed
// by the time anything asks, and "unavailable" (no plugin file present for
// this capability) is a fact about the loader, not about any instance.
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
class SubsystemRegistry;

// The narrow set of core references a subsystem actually needs, replacing
// today's practice of handing out whatever concrete internal reference
// happens to be convenient (see the old FuseFrontend/TorrentManager
// constructors). Extended only by what a migration actually needs: `node`
// and `ingest` are here because the Torrent plugin (Phase 1) needs exactly
// those two, `filesystem` and `hydration` because FUSE (Phase 2) needs
// exactly those two, and `registry` is where a plugin publishes the
// capability it provides so core can reach it without knowing the concrete
// class.
//
// Every pointer is non-owning and outlives the supervisor: Service holds all
// of them, and stops the supervisor before destroying any of them.
struct SubsystemContext {
    const Config* config{};
    NodeRuntime* node{};
    IngestManager* ingest{};
    SubsystemRegistry* registry{};
    FileSystem* filesystem{};
    HydrationManager* hydration{};
    // Cancels a construction that can legitimately block for a long time --
    // FUSE waits for this node's first namespace before it can build its
    // inode table. The supervisor supplies its own lifecycle thread's token
    // per attempt, so `Service::stop()` does not have to wait out a
    // subsystem that is still waiting for something that may never arrive.
    // A factory that cannot block may ignore it.
    std::stop_token startup_stop;
};

// Implemented by every subsystem that can run behind a SubsystemSupervisor,
// whether loaded as a plugin (FUSE, Torrent -- Phase 1/2) or, in principle,
// linked directly into core. start()/stop() must be safe to call repeatedly:
// a faulted subsystem is destroyed and reconstructed in place by its owning
// supervisor, not just logged and abandoned.
class Subsystem {
  public:
    // How a subsystem reports a fault its own threads discovered, after
    // start() has already returned successfully. The string is the operator-
    // facing reason and becomes SubsystemStatus::last_fault.
    using FaultSink = std::function<void(std::string reason)>;

    virtual ~Subsystem() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

    // Installed by SubsystemSupervisor between create() and start(). A
    // subsystem whose failure modes are all in construction or start() --
    // Torrent, and the fault-injection test plugins -- can ignore it; the
    // supervisor's retry/disable policy then only ever sees those.
    //
    // FUSE cannot: losing a kernel mount, or fuse_loop_mt returning an error,
    // happens long after start() returned, and before this hook the supervisor
    // had no way to learn of it (it parked until asked to stop, see
    // SubsystemSupervisor::run_entry). Calling the sink asks the supervisor to
    // stop and destroy this instance and then apply the ordinary backoff/
    // disable policy to a fresh one, exactly as it does for a failed start().
    //
    // Safe to call from any thread the subsystem owns, including from inside
    // start(); the supervisor never holds a lock across a call into the
    // instance. It may be called more than once -- the first reason wins --
    // and calls after the supervisor has begun stopping are ignored.
    virtual void attach_fault_sink(FaultSink sink) { (void)sink; }
};

} // namespace macha

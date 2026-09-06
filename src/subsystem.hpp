// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"

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

class IngestManager;
class NodeRuntime;
class SubsystemRegistry;

// The narrow set of core references a subsystem actually needs, replacing
// today's practice of handing out whatever concrete internal reference
// happens to be convenient (see the old FuseFrontend/TorrentManager
// constructors). Extended only by what a migration actually needs: `node`
// and `ingest` are here because the Torrent plugin (Phase 1) needs exactly
// those two, and `registry` is where a plugin publishes the capability it
// provides so core can reach it without knowing the concrete class.
//
// Every pointer is non-owning and outlives the supervisor: Service holds all
// of them, and stops the supervisor before destroying any of them.
struct SubsystemContext {
    const Config* config{};
    NodeRuntime* node{};
    IngestManager* ingest{};
    SubsystemRegistry* registry{};
};

// Implemented by every subsystem that can run behind a SubsystemSupervisor,
// whether loaded as a plugin (FUSE, Torrent -- Phase 1/2) or, in principle,
// linked directly into core. start()/stop() must be safe to call repeatedly:
// a faulted subsystem is destroyed and reconstructed in place by its owning
// supervisor, not just logged and abandoned.
class Subsystem {
  public:
    virtual ~Subsystem() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <dannan/supervise/startup.h>

#include <cstdint>

namespace macha {

// The process's one startup progress, ticked by each unit of long recovery work
// (history frame, delta, journal record, pack, readiness stage), Macha's and the
// store node's alike: the root hands it to the store node. Service::wait_services_ready
// kills the process only when this has not moved for `service_startup_no_progress_ms`,
// never merely because startup is slow.
inline dannan::supervise::StartupProgress& startup_progress_counter() {
    static dannan::supervise::StartupProgress progress;
    return progress;
}

inline void note_startup_progress(uint64_t units = 1) {
    startup_progress_counter().note(units);
}

inline uint64_t startup_progress() {
    return startup_progress_counter().value();
}

// Decides when a startup has stalled: Dannan's.
using StartupStallGate = dannan::supervise::StartupStallGate;

} // namespace macha

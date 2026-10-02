// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace macha {

// Process-wide startup progress, ticked by each unit of long recovery work
// (history frame, delta, journal record, pack, readiness stage).
// Service::wait_services_ready kills the process only when this has not moved
// for `service_startup_no_progress_ms`, never merely because startup is slow.
inline std::atomic<uint64_t>& startup_progress_counter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline void note_startup_progress(uint64_t units = 1) {
    startup_progress_counter().fetch_add(units, std::memory_order_relaxed);
}

inline uint64_t startup_progress() {
    return startup_progress_counter().load(std::memory_order_relaxed);
}

} // namespace macha

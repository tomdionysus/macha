// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace macha {

// A process-wide "recovery is doing something" counter. Every unit of
// startup work that can legitimately take a long time on a big node -- a
// history frame parsed, a delta applied, a journal record read, a pack
// scanned, a readiness stage reached -- ticks it. Service::wait_services_ready
// kills the process for its supervisor only when this has not moved for
// `service_startup_no_progress_ms`, never merely because startup is slow:
// on 2026-09-06 a 120 s elapsed-time gate turned a 5-minute (quadratic, but
// progressing) replay into an infinite crash loop. Discipline 2 of
// TODO/2026-09-06-self-healing-disciplines-plan.md.
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

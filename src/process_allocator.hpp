// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace macha {

struct ProcessAllocatorPolicy {
    bool supported{};
    size_t arena_max{};
};

// Apply before Macha creates worker or codec threads. On glibc this makes the
// allocator's process-wide arena count an explicit Macha bound instead of the
// glibc CPU-derived default. Other allocators are left untouched.
ProcessAllocatorPolicy configure_process_allocator(size_t arena_max);

// The allocator's own account of the memory it holds, summed over every arena:
// what it took from the system for its heaps and as separate mappings, what of
// that is allocated, and what is free but kept. Only glibc's malloc reports it.
struct ProcessAllocatorUsage {
    uint64_t heap_bytes{};
    uint64_t mapped_bytes{};
    uint64_t in_use_bytes{};
    uint64_t free_bytes{};
};
std::optional<ProcessAllocatorUsage> process_allocator_usage();

} // namespace macha

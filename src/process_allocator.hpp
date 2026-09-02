// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace macha {

struct ProcessAllocatorPolicy {
    bool supported{};
    size_t arena_max{};
};

// Apply before Macha creates worker or codec threads. On glibc this makes the
// allocator's process-wide arena count an explicit Macha bound instead of the
// glibc CPU-derived default. Other allocators are left untouched.
ProcessAllocatorPolicy configure_process_allocator(size_t arena_max);

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "process_allocator.hpp"

#include <limits>
#include <stdexcept>

#if defined(__linux__)
#include <features.h>
#if defined(__GLIBC__)
#include <malloc.h>
#define MACHA_HAS_GLIBC_MALLOPT 1
#endif
#endif

namespace macha {

ProcessAllocatorPolicy configure_process_allocator(size_t arena_max) {
    if (!arena_max || arena_max > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("allocator arena limit is out of range");
#if defined(MACHA_HAS_GLIBC_MALLOPT)
    if (::mallopt(M_ARENA_MAX, static_cast<int>(arena_max)) == 0)
        throw std::runtime_error("cannot apply glibc allocator arena limit");
    return {true, arena_max};
#else
    return {false, arena_max};
#endif
}

} // namespace macha

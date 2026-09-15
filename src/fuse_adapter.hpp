// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "config.hpp"
#include "filesystem.hpp"
#include "fuse_mountpoint.hpp"
#include "fuse_subsystem.hpp"

namespace macha {
class FuseFrontend;
// libfuse returns a positive signal number for an event loop deliberately
// terminated by its installed signal handlers, and a negated errno for an
// actual loop failure.
constexpr bool fuse_loop_result_is_error(int result) noexcept { return result < 0; }

// The libfuse mount driver registers itself with macha_core at static
// initialisation time (see fuse_subsystem.hpp); there is no entry point to
// call. Linking this translation unit is what gives a build the ability to
// mount, and not linking it is what makes `fuse` report `unavailable`.
} // namespace macha

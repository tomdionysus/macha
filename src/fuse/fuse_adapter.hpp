// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "config.hpp"
#include "filesystem/filesystem.hpp"
#include "fuse/fuse_mountpoint.hpp"
#include "fuse/fuse_subsystem.hpp"

namespace macha {
class FuseFrontend;
// libfuse returns a positive signal number for a signal-terminated loop and a
// negated errno for a loop failure.
constexpr bool fuse_loop_result_is_error(int result) noexcept { return result < 0; }

// The driver self-registers at static initialisation (see fuse_subsystem.hpp);
// a build without it reports `fuse` as `unavailable`.
} // namespace macha

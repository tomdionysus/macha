// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdexcept>

namespace macha {

// Thrown by a long walk's pause callback when the maintenance pass is asked to
// stop; it passes through the walk to the pass, which then sees the stop and
// ends. Nothing on the way treats it as the walk's own failure.
struct MaintenanceStopping final : std::runtime_error {
    MaintenanceStopping() : std::runtime_error("maintenance stopping") {}
};

} // namespace macha

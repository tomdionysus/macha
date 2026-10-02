// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace macha {

// The work classes, system-wide: on the wire, in the arbiters, and in every
// contract's WorkContext. Priority is frame_type_priority() (cluster/net.hpp),
// not enum order.
enum class FrameType : uint8_t {
    control = 1,
    foreground = 2,
    read_ahead = 3,
    speculative = 4,
    loader = 5, // user-requested bulk work; wire values are fixed
};

} // namespace macha

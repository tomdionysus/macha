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
    // User-requested bulk work. Keep the existing speculative wire value
    // stable; priority is defined by frame_type_priority(), not enum order.
    loader = 5,
};

} // namespace macha

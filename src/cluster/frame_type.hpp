// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace macha {

// How work travels: the wire encoding of its class, with a hint inside the
// viewer class (read_ahead behind foreground). Wire values are fixed. Order
// within a transport queue is frame_type_priority() (cluster/net.hpp).
enum class FrameType : uint8_t {
    control = 1,
    foreground = 2,
    read_ahead = 3,
    speculative = 4,
    loader = 5, // user-requested bulk work
};

// What work is, by the laws' ranking: control (law 1), viewer (law 2), loader
// (law 3), then speculative (repair, prefetch, maintenance). The one
// definition: every subsystem that admits, paces or measures work by class
// reads work_class(), and none decides class from a frame itself.
enum class WorkClass : uint8_t { control, viewer, loader, speculative };

constexpr WorkClass work_class(FrameType frame_type) noexcept {
    switch (frame_type) {
    case FrameType::control: return WorkClass::control;
    case FrameType::foreground:
    case FrameType::read_ahead: return WorkClass::viewer;
    case FrameType::loader: return WorkClass::loader;
    case FrameType::speculative: return WorkClass::speculative;
    }
    return WorkClass::speculative;
}

// The frame a class's work travels on when it moves DATA: a viewer's is
// foreground. Control has none to move (DATA admission refuses it).
constexpr FrameType frame_for(WorkClass work) noexcept {
    switch (work) {
    case WorkClass::control: return FrameType::control;
    case WorkClass::viewer: return FrameType::foreground;
    case WorkClass::loader: return FrameType::loader;
    case WorkClass::speculative: return FrameType::speculative;
    }
    return FrameType::speculative;
}

} // namespace macha

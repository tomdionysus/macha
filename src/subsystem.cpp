// SPDX-License-Identifier: GPL-3.0-or-later
#include "subsystem.hpp"

namespace macha {

std::string_view subsystem_state_name(SubsystemState state) noexcept {
    switch (state) {
    case SubsystemState::unavailable: return "unavailable";
    case SubsystemState::starting: return "starting";
    case SubsystemState::running: return "running";
    case SubsystemState::faulted: return "faulted";
    case SubsystemState::restarting: return "restarting";
    case SubsystemState::disabled: return "disabled";
    }
    return "unknown";
}

} // namespace macha

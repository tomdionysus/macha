// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "subsystem/subsystem.hpp"

#include <memory>
#include <string_view>

namespace macha {

// The contract every subsystem plugin (.so/.dylib) must satisfy. A plugin
// exports exactly one symbol with this name and signature:
//
//   extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry();
//
// Core and plugins are always built together from one commit, so a C++
// virtual interface across dlopen is safe; only this entry point is flat C.
inline constexpr const char* kSubsystemEntrySymbol = "macha_subsystem_entry";

struct SubsystemPluginEntry {
    // Must equal the loading core's macha::kBuildIdentity; a mismatch (e.g. a
    // partial deploy) is refused, as the ABI may differ.
    std::string_view build_identity;
    // nullptr: this node is configured not to run the capability (reported
    // `unavailable`, not retried). Throwing: failed, subject to backoff/disable.
    std::unique_ptr<Subsystem> (*create)(const SubsystemContext&);
};

using SubsystemEntryFunction = const SubsystemPluginEntry* (*)();

} // namespace macha

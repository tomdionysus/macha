// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "subsystem.hpp"

#include <memory>
#include <string_view>

namespace macha {

// The contract every subsystem plugin (.so/.dylib) must satisfy. A plugin
// exports exactly one symbol with this name and signature:
//
//   extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry();
//
// Core and its plugins are always built from the same source tree, same
// commit, in the same CMake invocation -- never distributed or versioned
// independently (see TODO/2026-09-05-subsystem-plugin-isolation-plan.md) --
// so a plain C++ virtual interface across the dlopen boundary is safe; the
// only part that must cross as a flat C symbol is this one bootstrap entry
// point.
inline constexpr const char* kSubsystemEntrySymbol = "macha_subsystem_entry";

struct SubsystemPluginEntry {
    // Must equal macha::kBuildIdentity (version.hpp.in) of the core that
    // loads this plugin. A mismatch means this plugin was built against a
    // different revision of macha_core than the one currently running -- for
    // example a partial deploy -- and must be refused rather than loaded:
    // the ABI is not guaranteed compatible.
    std::string_view build_identity;
    // Returning no instance (rather than throwing) means "this node is
    // configured not to run this capability" -- the supervisor reports
    // `unavailable` and does not retry. Throwing means the attempt failed and
    // is subject to the backoff/disable policy.
    std::unique_ptr<Subsystem> (*create)(const SubsystemContext&);
};

using SubsystemEntryFunction = const SubsystemPluginEntry* (*)();

} // namespace macha

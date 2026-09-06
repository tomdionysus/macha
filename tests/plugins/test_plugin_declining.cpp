// SPDX-License-Identifier: GPL-3.0-or-later
//
// A real plugin whose factory declines to produce an instance, the way the
// torrent plugin does when torrent.enabled is false. That is an operator
// choice, not a fault: the supervisor must report `unavailable` and stop,
// never enter the backoff/retry path it uses for a plugin that threw.
#include "subsystem.hpp"
#include "subsystem_abi.hpp"
#include "macha_version.hpp"

namespace {

std::unique_ptr<macha::Subsystem> create(const macha::SubsystemContext&) {
    return {};
}

const macha::SubsystemPluginEntry kEntry{macha::kBuildIdentity, &create};

} // namespace

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &kEntry;
}

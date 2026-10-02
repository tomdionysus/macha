// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin whose factory declines to produce an instance (as torrent does when
// disabled). The supervisor must report `unavailable`, not enter backoff/retry.
#include "subsystem/subsystem.hpp"
#include "subsystem/subsystem_abi.hpp"
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

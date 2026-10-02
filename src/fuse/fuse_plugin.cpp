// SPDX-License-Identifier: GPL-3.0-or-later
// libmacha-fuse's one exported symbol (kSubsystemEntrySymbol). dlopen has
// already run fuse_adapter.cpp's static initialiser, registering the driver.
// The plugin boundary is libfuse: the frontend, journal and mountpoint helpers
// stay in macha_core.
#include "fuse/fuse_adapter.hpp"
#include "fuse/fuse_subsystem.hpp"
#include "macha_version.hpp"
#include "subsystem/subsystem_abi.hpp"

namespace macha {
namespace {

const SubsystemPluginEntry kEntry{kBuildIdentity, &make_fuse_subsystem};

} // namespace
} // namespace macha

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &macha::kEntry;
}

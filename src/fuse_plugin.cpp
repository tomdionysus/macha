// SPDX-License-Identifier: GPL-3.0-or-later
// The libmacha-fuse plugin's boundary with core: the one exported C symbol
// (kSubsystemEntrySymbol). Everything it needs to do beyond that has already
// happened by the time this is called -- dlopen ran fuse_adapter.cpp's static
// initialiser, which registered the libfuse mount driver with macha_core.
//
// The plugin boundary here is libfuse, not FuseFrontend: the frontend, the
// journal and the mountpoint helpers are core's own code and stay in
// macha_core, so core keeps its concrete types and the FUSE tests keep
// linking it directly. See TODO/2026-09-14-fuse-supervised-subsystem-plan.md,
// decision 2.
#include "fuse_adapter.hpp"
#include "fuse_subsystem.hpp"
#include "macha_version.hpp"
#include "subsystem_abi.hpp"

namespace macha {
namespace {

const SubsystemPluginEntry kEntry{kBuildIdentity, &make_fuse_subsystem};

} // namespace
} // namespace macha

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &macha::kEntry;
}

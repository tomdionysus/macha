// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include <cstdint>
#include <filesystem>
#include <string>

namespace macha {

enum class MountTableState : uint8_t {
    macha_fuse,
    other,
    missing,
    probe_error,
};

struct MountTableProbe {
    MountTableState state{MountTableState::probe_error};
    int error{};
};

// The covered directory under the mount path at startup, and how it is guarded
// while unmounted.
struct MountpointPreparation {
    // Host entries under the mount path, hidden by the mount; logged as an
    // error and reported as filesystem.mountpoint_stray_entries.
    uint64_t stray_entries{};
    // Immutable flag set: nothing, root included, can create entries while
    // the mount is absent.
    bool immutable{};
    // Why the requested flag could not be set.
    std::string immutable_error;
    // Permission bits as first observed in this process, before any guard;
    // what a clean unmount restores.
    uint32_t covered_mode{};
    bool covered_mode_known{};
};

MountTableProbe probe_macha_mountpoint(const std::string& mount);
// Recovers a stale Macha mount, then inspects and guards the covered directory.
void prepare_fuse_mountpoint(const std::filesystem::path& mount_path, const FuseConfig& config);
// The inspect/guard step alone; creates the directory when missing.
MountpointPreparation guard_covered_mountpoint(const std::filesystem::path& mount_path,
                                               bool fail_closed);
// Result of the most recent prepare_fuse_mountpoint() in this process.
const MountpointPreparation& fuse_mountpoint_preparation();

} // namespace macha

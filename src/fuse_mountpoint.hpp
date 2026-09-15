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

// What the covered directory under the mount path looked like at startup and
// how it is guarded while no Macha mount covers it.
struct MountpointPreparation {
    // Entries already present under the mount path on the host filesystem.
    // They are not in Macha; the mount will hide them. Logged as an error and
    // exposed as filesystem.mountpoint_stray_entries in the status API.
    uint64_t stray_entries{};
    // The covered directory carries the immutable flag, so nothing (root
    // included) can create entries under it while the mount is absent.
    bool immutable{};
    // Why the flag could not be set, when it was requested and is not set.
    std::string immutable_error;
    // The covered directory's own permission bits as first observed in this
    // process, before anything protected it. The fail-closed guard restores
    // THIS on a clean unmount rather than whatever it happened to see when it
    // was constructed: after an unexpected mount loss the directory is left
    // deliberately non-writable, and a later mount attempt must not record
    // that as the original and restore it as such.
    uint32_t covered_mode{};
    bool covered_mode_known{};
};

MountTableProbe probe_macha_mountpoint(const std::string& mount);
// Recover a stale Macha mount, then inspect and guard the covered directory.
void prepare_fuse_mountpoint(const std::filesystem::path& mount_path, const FuseConfig& config);
// The inspection/guard step alone (creates the directory when missing).
MountpointPreparation guard_covered_mountpoint(const std::filesystem::path& mount_path,
                                               bool fail_closed);
// Result of the most recent prepare_fuse_mountpoint() in this process.
const MountpointPreparation& fuse_mountpoint_preparation();

} // namespace macha

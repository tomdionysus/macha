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

MountTableProbe probe_macha_mountpoint(const std::string& mount);
void prepare_fuse_mountpoint(const std::filesystem::path& mount_path, const FuseConfig& config);

} // namespace macha

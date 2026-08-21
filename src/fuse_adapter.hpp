// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "config.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"
#include <filesystem>
#include <functional>

namespace macha {
int run_fuse(FileSystem& fs, CacheHydrator& hydrator, const std::filesystem::path& mount_path,
             const FuseConfig& config, std::function<void()> request_shutdown = {});
}

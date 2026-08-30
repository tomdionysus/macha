// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "config.hpp"
#include "filesystem.hpp"
#include "fuse_mountpoint.hpp"
#include "hydration.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace macha {
class FuseFrontend;
int run_fuse(FileSystem& fs, CacheHydrator& hydrator, const std::filesystem::path& mount_path,
             const FuseConfig& config, std::function<void()> request_shutdown = {},
             std::function<void(std::weak_ptr<FuseFrontend>)> frontend_observer = {});
} // namespace macha

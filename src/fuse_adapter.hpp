// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filesystem.hpp"
#include <filesystem>
#include <functional>

namespace macha {
int run_fuse(FileSystem& fs, const std::filesystem::path& mount_path, bool allow_other = false,
             std::function<void()> request_shutdown = {});
}

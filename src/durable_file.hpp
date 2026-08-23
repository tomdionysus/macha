// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string_view>

namespace macha {

// Atomically replace a small local state file and make both its contents and
// directory entry durable before returning. Intended for accepted/resumable
// control state, not rebuildable caches or scheduling hints.
void durable_replace_file(const std::filesystem::path& path, std::string_view contents);

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

namespace macha {

// Syncs a directory, so the entries in it survive a crash. Throws on failure.
void durable_sync_directory(const std::filesystem::path& directory);

// How the durable writes below sync a directory: `durable_sync_directory`, or an
// observer standing in for it.
using DirectorySync = std::function<void(const std::filesystem::path&)>;

// Makes each missing directory of `directory`, outermost first, and syncs each
// into its parent, so a crash cannot lose a directory a durable file is then
// written into. Existing directories are left as they are.
void durable_create_directories(const std::filesystem::path& directory);
void durable_create_directories(const std::filesystem::path& directory, const DirectorySync& sync);

// Atomically replace a small local state file and make both its contents and
// directory entry durable before returning, along with every directory it
// made. Intended for accepted/resumable control state, not rebuildable caches
// or scheduling hints.
void durable_replace_file(const std::filesystem::path& path, std::string_view contents);
void durable_replace_file(const std::filesystem::path& path, std::string_view contents,
                          const DirectorySync& sync);

} // namespace macha

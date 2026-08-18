// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace macha {

// macFUSE expects names returned from readdir() in Unicode Normalization Form D.
// This is an adapter/presentation rule only: Macha's persisted namespace strings
// remain byte-for-byte unchanged for compatibility with existing stores.
std::string macos_fuse_decomposed_name(std::string_view name);

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace macha {

// macFUSE expects readdir() names in NFD. Presentation only: persisted
// namespace strings are never rewritten.
std::string macos_fuse_decomposed_name(std::string_view name);

// Canonical form for runtime path equivalence on macOS; never persisted.
std::string macos_fuse_composed_name(std::string_view name);

} // namespace macha

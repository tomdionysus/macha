// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

// Coverage hooks for a test process that forks a child per case
// (tests/test_framework.cpp). Under Clang every image -- this library and
// each executable -- carries its own private copy of the profile runtime,
// so the library's counters can only be named, reset and written from
// inside the library. Empty unless the library is built for coverage under
// Clang; GCC's runtime already reaches the library from the executable.
namespace macha::coverage {

// `pattern` with every "%p" replaced by `pid` followed by `tag`: the name a
// forked child's profile goes to. The runtime expands %p once, in the
// parent, so without this every child writes the parent's file.
std::string child_profile_name(std::string_view pattern, long pid, std::string_view tag);

// In a forked child: point this library's profile at the child's own file
// (from LLVM_PROFILE_FILE, tagged "-core") and drop the counts inherited
// from the parent.
void begin_child();
// Writes this library's counters, for a child that leaves without exit
// handlers.
void dump();

} // namespace macha::coverage

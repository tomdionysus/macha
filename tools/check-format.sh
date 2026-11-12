#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every tracked C and C++ source must be as clang-format leaves it. The version
# is pinned (CLANG_FORMAT, default clang-format-19), since another version
# formats differently.
set -eu
cd "$(git rev-parse --show-toplevel)"
clang_format=${CLANG_FORMAT:-clang-format-19}
command -v "$clang_format" >/dev/null || { echo "check-format: $clang_format not found" >&2; exit 2; }
git ls-files -z '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' |
    xargs -0 "$clang_format" --dry-run --Werror
echo "format: ok"

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every tracked source, script and build file names its licence in an SPDX line
# near its top.
set -eu
cd "$(git rev-parse --show-toplevel)"
missing=0
# TODO/ holds the record, archived evidence among it, and is not source.
for file in $(git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.sh' 'CMakeLists.txt' '*.cmake' '*.cmake.in' ':!:TODO/'); do
    if ! head -n 5 "$file" | grep -q 'SPDX-License-Identifier:'; then
        echo "no SPDX line: $file"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1
echo "SPDX: ok"

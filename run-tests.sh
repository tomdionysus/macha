#!/bin/sh
# Run every Macha test executable that was built in the selected build tree.
set -u

build_dir=${1:-build}
if [ "$#" -gt 0 ]; then
    shift
fi

found=0
failed=0

for test_binary in "$build_dir"/macha-tests "$build_dir"/macha-tests-*; do
    if [ ! -f "$test_binary" ] || [ ! -x "$test_binary" ]; then
        continue
    fi
    found=1
    printf '\n==> %s\n' "$test_binary"
    if ! "$test_binary" "$@"; then
        failed=1
    fi
done

if [ "$found" -eq 0 ]; then
    printf 'No Macha test executables found in %s\n' "$build_dir" >&2
    exit 1
fi

exit "$failed"

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The merge gate: format and SPDX checks, the release build with every test
# binary, clang-tidy over the sources, the ASan and UBSan build with the same
# tests, and the coverage build with its report (coverage/coverage.info in
# build-coverage). Dannan must be installed where CMake finds it, or named by
# CMAKE_PREFIX_PATH.
#
#   tools/ci.sh                  run here
#   tools/ci.sh --remote HOST    copy this working tree to HOST over SSH and
#                                run there; HOST is [user@]address
#   tools/ci.sh --local          run here even if MACHA_CI_HOST is set
#
# MACHA_CI_HOST may stand in for --remote HOST. The remote copy lives in
# ~/macha-ci on the host; its build trees are kept between runs.
set -eu

usage() {
    sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'
}

remote=${MACHA_CI_HOST:-}
while [ $# -gt 0 ]; do
    case "$1" in
        --remote)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            remote=$2
            shift 2
            ;;
        --local)
            remote=""
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

top=$(git rev-parse --show-toplevel)
cd "$top"

if [ -n "$remote" ]; then
    dest=macha-ci
    echo "== copying working tree to $remote:$dest"
    # The copy is cleared first, its build trees kept, so a file removed here
    # is gone there too.
    ssh -o BatchMode=yes "$remote" \
        "mkdir -p $dest && find $dest -mindepth 1 -maxdepth 1 ! -name 'build*' -exec rm -rf {} +"
    git ls-files -co --exclude-standard -z |
        rsync -a --no-owner --no-group --from0 --files-from=- ./ "$remote:$dest/"
    rsync -a --no-owner --no-group .git/ "$remote:$dest/.git/"
    exec ssh -o BatchMode=yes "$remote" \
        "cd $dest && git update-index -q --refresh >/dev/null 2>&1; CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH:-}' sh tools/ci.sh --local"
fi

jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
step() { echo; echo "== $*"; }

step "format"
sh tools/check-format.sh

step "SPDX headers"
sh tools/check-spdx.sh

step "release build and tests"
cmake --preset default >/dev/null
cmake --build --preset default -j "$jobs"
sh run-tests.sh build --jobs "$jobs"

if command -v run-clang-tidy-19 >/dev/null; then
    step "clang-tidy"
    run-clang-tidy-19 -p build -j "$jobs" -quiet '^.*/(src|tools)/.*\.cpp$' >build/clang-tidy.log 2>&1 ||
        { grep -E "(warning|error):" build/clang-tidy.log | sort -u; exit 1; }
    echo "clang-tidy: ok"
else
    step "clang-tidy: skipped, run-clang-tidy-19 not installed"
fi

step "ASan and UBSan build and tests"
cmake --preset asan >/dev/null
cmake --build --preset asan -j "$jobs"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    sh run-tests.sh build-asan --jobs "$jobs"

step "coverage"
[ -d build-coverage ] && find build-coverage -name '*.gcda' -delete
cmake --preset coverage >/dev/null
cmake --build --preset coverage -j "$jobs"
cmake --build --preset coverage --target coverage

echo
echo "== ci: all checks passed ($(git describe --tags --always --dirty 2>/dev/null || echo unknown))"

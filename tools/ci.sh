#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The merge gate: format and SPDX checks, the release build with every test
# binary, clang-tidy over the sources, the ASan and UBSan build with the same
# tests, and the coverage build with its report (coverage/coverage.info in
# build-coverage). Dannan and Dagda must be installed where CMake finds them,
# or named by CMAKE_PREFIX_PATH. The ASan build compiles both again from source,
# instrumented: MACHA_DANNAN_SRC and MACHA_DAGDA_SRC name their source trees.
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
        "cd $dest && git update-index -q --refresh >/dev/null 2>&1; CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH:-}' MACHA_DANNAN_SRC='${MACHA_DANNAN_SRC:-}' MACHA_DAGDA_SRC='${MACHA_DAGDA_SRC:-}' sh tools/ci.sh --local"
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
# Dannan and Dagda are built from source with the same flags into their own
# prefixes: a sanitizer build never links a plain-built library.
[ -n "${MACHA_DANNAN_SRC:-}" ] && [ -n "${MACHA_DAGDA_SRC:-}" ] || {
    echo "ci: MACHA_DANNAN_SRC and MACHA_DAGDA_SRC must name Dannan's and Dagda's source trees" >&2
    exit 1
}
san_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
deps="$top/build-asan-deps"
san_lib() {
    src=$1 prefix=$2
    shift 2
    cmake -S "$src" -B "$prefix-build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS="$san_flags" -DCMAKE_CXX_FLAGS="$san_flags" \
        -DCMAKE_EXE_LINKER_FLAGS="$san_flags" -DCMAKE_SHARED_LINKER_FLAGS="$san_flags" \
        -DCMAKE_INSTALL_PREFIX="$prefix" "$@" >/dev/null
    cmake --build "$prefix-build" -j "$jobs"
    cmake --install "$prefix-build" >/dev/null
}
san_lib "$MACHA_DANNAN_SRC" "$deps/dannan" -DDANNAN_BUILD_TESTING=OFF
san_lib "$MACHA_DAGDA_SRC" "$deps/dagda" -DDAGDA_BUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$deps/dannan"
for so in "$deps/dannan/lib/libdannan.so" "$deps/dagda/lib/libdagda.so"; do
    n=$(nm -D "$so" | grep -c __asan_ || true)
    echo "$(basename "$so"): $n __asan_ symbols"
    [ "$n" -gt 0 ] || { echo "ci: $so is not instrumented" >&2; exit 1; }
done
cmake --preset asan -U dannan_DIR -U dagda_DIR -DCMAKE_PREFIX_PATH="$deps/dannan;$deps/dagda" >/dev/null
cmake --build --preset asan -j "$jobs"
ldd build-asan/macha-tests | grep -E "libdannan|libdagda"
LD_LIBRARY_PATH="$deps/dannan/lib:$deps/dagda/lib" \
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    sh run-tests.sh build-asan --jobs "$jobs"

step "coverage"
[ -d build-coverage ] && find build-coverage -name '*.gcda' -delete
cmake --preset coverage >/dev/null
cmake --build --preset coverage -j "$jobs"
cmake --build --preset coverage --target coverage

echo
echo "== ci: all checks passed ($(git describe --tags --always --dirty 2>/dev/null || echo unknown))"

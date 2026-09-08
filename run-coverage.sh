#!/bin/sh
# Build Macha's tests instrumented for coverage, run them, and report.
#
# Works on both toolchains this project is built with, which need entirely
# different machinery: AppleClang/Clang emits a profile per process and is
# read back with llvm-profdata/llvm-cov, while GCC emits .gcda counters beside
# the objects and is read back with gcov. Neither lcov nor gcovr is installed
# on the cluster nodes, so the GCC summary is aggregated here from plain gcov
# output rather than depending on a tool that would have to be installed
# first.
#
# Usage: ./run-coverage.sh [build_dir] [-- test args...]
#   ./run-coverage.sh                       # build-coverage, whole suite
#   ./run-coverage.sh build-cov             # a different build directory
#   ./run-coverage.sh build-cov -- --filter test_retained_memory
set -u

build_dir=build-coverage
if [ "$#" -gt 0 ] && [ "$1" != "--" ]; then
    build_dir=$1
    shift
fi
[ "$#" -gt 0 ] && [ "$1" = "--" ] && shift

# Coverage instrumentation is slower than an ordinary build, and every case
# deadline was chosen against an ordinary build. Scale them rather than let a
# spurious timeout hide the report the run existed to produce -- the same
# reasoning the sanitizer build already uses.
: "${MACHA_TEST_TIMEOUT_SCALE:=3}"
export MACHA_TEST_TIMEOUT_SCALE
# -O0 keeps line and region counts attributable to the source that produced
# them; at -O3 inlining makes a coverage report describe the optimiser's view
# rather than the code's.
: "${MACHA_COVERAGE_BUILD_TYPE:=Debug}"

printf 'Configuring %s (coverage, %s)\n' "$build_dir" "$MACHA_COVERAGE_BUILD_TYPE"
cmake -S . -B "$build_dir" \
      -DCMAKE_BUILD_TYPE="$MACHA_COVERAGE_BUILD_TYPE" \
      -DMACHA_TEST_COVERAGE=ON >/dev/null || exit 1
cmake --build "$build_dir" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" || exit 1

# CMAKE_CXX_COMPILER_ID is not a cache entry; CMake records it here.
compiler_id=$(sed -n 's/^set(CMAKE_CXX_COMPILER_ID "\(.*\)")$/\1/p' \
    "$build_dir"/CMakeFiles/*/CMakeCXXCompiler.cmake 2>/dev/null | head -1)
case "$compiler_id" in
    AppleClang|Clang) flavour=llvm ;;
    GNU)              flavour=gcc ;;
    *) printf 'Unsupported compiler for coverage: %s\n' "$compiler_id" >&2; exit 1 ;;
esac

profile_dir="$build_dir/coverage-profiles"
rm -rf "$profile_dir"
mkdir -p "$profile_dir"

# GCC accumulates counters into .gcda across runs, so a stale set from an
# earlier (perhaps filtered) run would be counted as though this run had
# executed it. Reset them: a coverage report should describe the run that
# produced it.
if [ "$flavour" = gcc ]; then
    find "$build_dir" -name '*.gcda' -delete 2>/dev/null
fi

# The test runner executes every case in an isolated child process, so the
# profile filename must be per-process or the children overwrite each other
# and the report describes whichever one happened to exit last.
if [ "$flavour" = llvm ]; then
    LLVM_PROFILE_FILE="$profile_dir/%p.profraw"
    export LLVM_PROFILE_FILE
fi

status=0
for test_binary in "$build_dir"/macha-tests "$build_dir"/macha-tests-*; do
    [ -f "$test_binary" ] && [ -x "$test_binary" ] || continue
    printf '\n==> %s\n' "$test_binary"
    "$test_binary" "$@" || status=1
done

printf '\n===== coverage =====\n'
if [ "$flavour" = llvm ]; then
    profdata="$build_dir/coverage.profdata"
    # shellcheck disable=SC2046
    set -- $(ls "$profile_dir"/*.profraw 2>/dev/null)
    if [ "$#" -eq 0 ]; then
        printf 'No profiles were produced.\n' >&2
        exit 1
    fi
    (xcrun llvm-profdata merge -sparse -o "$profdata" "$@" 2>/dev/null ||
     llvm-profdata merge -sparse -o "$profdata" "$@") || exit 1

    objects=""
    for lib in "$build_dir"/libmacha_core.dylib "$build_dir"/libmacha_core.so; do
        [ -f "$lib" ] && objects="$objects -object $lib"
    done
    # shellcheck disable=SC2086
    (xcrun llvm-cov report "$build_dir/macha-tests" $objects \
        -instr-profile="$profdata" \
        -ignore-filename-regex='(tests/|/build|_deps/|/usr/)' 2>/dev/null ||
     llvm-cov report "$build_dir/macha-tests" $objects \
        -instr-profile="$profdata" \
        -ignore-filename-regex='(tests/|/build|_deps/|/usr/)')
else
    # GCC: .gcda counters land beside the objects. Aggregate plain gcov output
    # ourselves, since neither lcov nor gcovr is present on the nodes.
    gcov_dir="$build_dir/coverage-gcov"
    rm -rf "$gcov_dir"
    mkdir -p "$gcov_dir"
    # gcov's -o names the directory holding the objects, not an output
    # directory; the .gcno paths are passed directly instead. -n keeps it from
    # writing a .gcov file per translation unit, since only the summary it
    # prints is wanted here.
    find "$build_dir" -name '*.gcno' -print0 2>/dev/null |
        xargs -0 -r -n 32 gcov -n -b >"$gcov_dir/gcov.log" 2>&1
    python3 - "$gcov_dir/gcov.log" <<'PY'
import re, sys
rows, current = {}, None
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"File '(.+)'", line.strip())
    if m:
        current = m.group(1)
        continue
    m = re.match(r"Lines executed:([\d.]+)% of (\d+)", line.strip())
    if m and current:
        # Only our own sources; system headers and the tests themselves are
        # noise in a report about what the server code exercises.
        if ("/src/" in current or current.startswith("src/")) and "/usr/" not in current:
            pct, total = float(m.group(1)), int(m.group(2))
            name = current[current.rfind("/src/") + 1:] if "/src/" in current else current
            covered = round(pct * total / 100.0)
            prev = rows.get(name)
            # The same file can be reported more than once; keep the run with
            # the most lines attributed to it.
            if prev is None or total > prev[1]:
                rows[name] = (covered, total)
        current = None
if not rows:
    print("No coverage data found.")
    sys.exit(1)
tc = sum(c for c, _ in rows.values())
tt = sum(t for _, t in rows.values())
print(f"{'FILE':<52}{'LINES':>9}{'COVER':>9}")
for name, (c, t) in sorted(rows.items(), key=lambda kv: (kv[1][0] / kv[1][1]) if kv[1][1] else 1.0):
    print(f"{name:<52}{t:>9}{(100.0 * c / t if t else 0.0):>8.1f}%")
print("-" * 70)
print(f"{'TOTAL':<52}{tt:>9}{(100.0 * tc / tt if tt else 0.0):>8.1f}%")
PY
fi

exit "$status"

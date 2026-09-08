# Backend testing

Macha deliberately separates the portable behavioural suite from the small set of tests that require build-time runtime adapters.

- `macha-tests` is the default dependency-light suite. It exercises storage, metadata, RPC, clustering, MachaDFS/FUSE ordering and recovery, hydration, catalogue policy, playback planning, ingest, GC and other core behaviour without requiring yaml-cpp or FFmpeg. FUSE kernel mounting is not required: the FUSE journal/frontend tests exercise the adapter-independent filesystem contract directly.
- `macha-tests-runtime` is built only when both yaml-cpp and the required FFmpeg/libav development libraries are available. It owns concrete YAML parsing, the libav log bridge and embedded libav media/tag/artwork extraction, and the A/V timeline harness that drives the real transcode pipeline end to end.

Both executables use the same `test_framework.hpp` runner. Cases self-register; the parent runner executes them in isolated child processes and schedules independent cases concurrently. Architecture regressions remain ordinary cases in `test_invariants.cpp` (or in the owning subsystem where the invariant is narrower), rather than becoming a separate testing framework.

The test implementation is split by the subsystem that owns the invariant:

- `test_foundations.cpp` — codecs, dependency-free configuration policy, diagnostics, placement and small pure policies;
- `test_storage_metadata.cpp` — local/pool storage, persistent cache and metadata durability;
- `test_rpc_cluster.cpp` — transport, admission, quorum, membership and distributed repair;
- `test_filesystem_fuse.cpp` — MachaDFS semantics, FUSE ordering, durability and recovery;
- `test_hydration_catalogue.cpp` — hydration, replica selection, dependency-free media recognition and catalogue state;
- `test_media_playback.cpp` — HTTP streaming, VOD planning, timestamps and playback negotiation using an injected media engine;
- `test_invariants.cpp` — cross-component regressions whose failure requires a real topology/lifecycle;
- `test_models.cpp` — cheap state/property coverage over production algorithms and durable formats;
- `test_runtime_dependencies.cpp` — concrete yaml-cpp and FFmpeg/libav adapter behaviour;
- `test_transcode_timeline.cpp` — A/V timeline behaviour of the real transcode pipeline,
  measured from the fragments it actually publishes rather than through an injected engine.

Shared deterministic infrastructure lives in `test_support.hpp` and `test_backend_support.hpp`. Prefer `TestService`, `TestNode`, `TestCluster`, `TestGate`, declarative case tables and exhaustive boundary loops over rebuilding keys, ports, state directories or ad-hoc sleeps in individual tests.

## Build and run

A normal server build still requires yaml-cpp and FFmpeg. On a constrained development host, build only the portable core and tests:

```sh
cmake -S . -B build-light \
  -DMACHA_BUILD_SERVER=OFF \
  -DMACHA_BUILD_TESTS=ON \
  -DMACHA_WARNINGS_AS_ERRORS=ON
cmake --build build-light -j
./run-tests.sh build-light
```

On a fully provisioned host, the normal build creates both test binaries and the same script runs both automatically:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMACHA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
./run-tests.sh build
```

The runner accepts normal test-runner arguments after the build directory, for example `./run-tests.sh build --serial` or `./run-tests.sh build --filter catalogue`.

## Sanitizer builds

`MACHA_SANITIZE` builds the whole tree -- `macha_core`, the executables, every
subsystem plugin and both test binaries -- under one sanitizer. It has to be
whole-tree rather than per-target: core is a shared library the executables
link and the plugins `dlopen`, so instrumenting only part of it would leave
the interposed allocator and the shadow memory disagreeing across that
boundary.

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMACHA_WARNINGS_AS_ERRORS=ON -DMACHA_SANITIZE=address,undefined
cmake --build build-asan -j
./run-tests.sh build-asan
```

`thread` is the other useful setting, and is deliberately run separately:
ThreadSanitizer keeps its own shadow state and cannot share a process with
AddressSanitizer's, so the configure step refuses the combination rather than
letting the compiler reject it later and less legibly.

Case deadlines are scaled automatically in a sanitizer build (3x under ASan,
10x under TSan), because the declared 30/60/120 second deadlines were chosen
against an ordinary build and a spurious timeout would hide the sanitizer
report the run existed to produce. `--timeout-scale N` or
`MACHA_TEST_TIMEOUT_SCALE` overrides it.

Leak checking works despite the runner's `_Exit()` isolation: each case calls
`__lsan_do_recoverable_leak_check()` after its local owners have unwound. See
the sanitizer-proof section of `docs/ownership.md`.

## Runtime model

A single suite executable does not mean serial execution. By default each runner uses up to twelve hardware-derived resource slots and gives integration/heavy cases larger weights. Each case runs after `fork()` in its own process, so global loggers, environment, signal state, temporary files and background threads cannot leak into another test. `free_port()` assigns each case a separate loopback-port block before it starts real RPC/HTTP listeners.

Useful runner options:

```text
macha-tests --list
macha-tests --filter fuse
macha-tests --jobs 8
macha-tests --serial
macha-tests --verbose
```

`MACHA_TEST_JOBS=N` supplies the default slot budget for CI. Every result prints its elapsed time; the final line reports both wall time and the sum of individual case times, making accidental serialization or a newly slow test visible immediately.

Synchronization sleeps are not an acceptable way to make concurrency deterministic. Use an observable production state or a `TestGate` so the test proves the competing operation has actually entered the state being tested. Deliberate time-policy tests may scale the policy interval while keeping the same ordering/ratio invariant.

## Regression ownership

The suites are organized by current invariants rather than by historical release migrations. A test belongs in `macha-tests-runtime` only when the assertion genuinely needs yaml-cpp or a concrete FFmpeg/libav API. Policy and state-machine behaviour stays in `macha-tests` and uses injected/test adapters where appropriate.

When a test is consolidated or moved, preserve the underlying failure situation in the owning subsystem and retain process-shaped restart/network coverage wherever a pure state test cannot reproduce the lifecycle.

State/property coverage complements integration tests for metadata deltas, placement, hydration scheduling, durable replacement, ingest safety and enum/state round trips. The purpose is to prove production algorithms over broad state spaces without replacing the crash, restart and network tests that validate real wiring.

## Quantitative coverage

Configure with `MACHA_TEST_COVERAGE=ON` to instrument `macha_core` and the test executables available in that build. The normal build remains uninstrumented.

With Clang/AppleClang, run the tests with `LLVM_PROFILE_FILE` set and inspect the merged profile with `llvm-profdata`/`llvm-cov`; with GCC, use gcov/lcov. Branch coverage is the required comparison when consolidating scenarios. A future consolidation is acceptable only when:

1. every situation it replaces has an explicit owning invariant or case-table row;
2. branch coverage does not decrease; and
3. process-shaped restart/network tests are retained wherever a pure state test cannot reproduce the relevant lifecycle.

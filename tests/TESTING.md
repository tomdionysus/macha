# Backend testing

The backend has one test executable: `macha-tests`.

There is no separate architecture-test binary or assertion framework. Every case self-registers
with `test_framework.hpp`; the parent runner executes cases in isolated child processes and schedules
independent cases concurrently. Architecture regressions are ordinary cases in `test_invariants.cpp`
(or in the owning subsystem where the invariant is narrower).

The test implementation is split only by the subsystem that owns the invariant:

- `test_foundations.cpp` — codecs, configuration, diagnostics, placement and small pure policies;
- `test_storage_metadata.cpp` — local/pool storage, persistent cache and metadata durability;
- `test_rpc_cluster.cpp` — transport, admission, quorum, membership and distributed repair;
- `test_filesystem_fuse.cpp` — filesystem semantics, FUSE ordering, durability and recovery;
- `test_hydration_catalogue.cpp` — hydration, replica selection, media recognition and catalogue state;
- `test_media_playback.cpp` — HTTP streaming, VOD planning, timestamps and playback negotiation;
- `test_invariants.cpp` — cross-component regressions whose failure requires a real topology/lifecycle;
- `test_models.cpp` — cheap state/property coverage over production algorithms and durable formats.

Shared deterministic infrastructure lives in `test_support.hpp` and `test_backend_support.hpp`.
Prefer `TestService`, `TestNode`, `TestCluster`, `TestGate`, declarative case tables and exhaustive
boundary loops over rebuilding keys, ports, state directories or ad-hoc sleeps in individual tests.

## Runtime model

A single executable does not mean serial execution. By default the runner uses up to twelve hardware-derived resource
slots and gives integration/heavy cases larger weights. Each case runs after `fork()` in its own
process, so global loggers, environment, signal state, temporary files and background threads cannot
leak into another test. `free_port()` assigns each case a separate loopback-port block before it
starts real RPC/HTTP listeners.

Useful runner options:

```text
macha-tests --list
macha-tests --filter fuse
macha-tests --jobs 8
macha-tests --serial
macha-tests --verbose
```

`MACHA_TEST_JOBS=N` supplies the default slot budget for CI. Every result prints its elapsed time;
the final line reports both wall time and the sum of individual case times, making accidental
serialization or a newly slow test visible immediately.

Synchronization sleeps are not an acceptable way to make concurrency deterministic. Use an
observable production state or a `TestGate` so the test proves the competing operation has actually
entered the state being tested. Deliberate time-policy tests may scale the policy interval while
keeping the same ordering/ratio invariant.

## Regression ownership

The suite is organized by current invariants rather than by historical release migrations. When a test is consolidated or moved, preserve the underlying failure situation in the owning subsystem and retain process-shaped restart/network coverage wherever a pure state test cannot reproduce the lifecycle.

State/property coverage complements integration tests for metadata deltas, placement, hydration scheduling, durable replacement, ingest safety and enum/state round trips. The purpose is to prove production algorithms over broad state spaces without replacing the crash, restart and network tests that validate real wiring.

## Quantitative coverage

Configure with `MACHA_TEST_COVERAGE=ON` to instrument both `macha_core` and `macha-tests`. The normal
build remains uninstrumented.

With Clang/AppleClang, run the tests with `LLVM_PROFILE_FILE` set and inspect the merged profile with
`llvm-profdata`/`llvm-cov`; with GCC, use gcov/lcov. Branch coverage is the required comparison when
consolidating scenarios. A future consolidation is acceptable only when:

1. every situation it replaces has an explicit owning invariant or case-table row;
2. branch coverage does not decrease; and
3. process-shaped restart/network tests are retained wherever a pure state test cannot reproduce the
   relevant lifecycle.

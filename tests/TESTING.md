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

## 0.15.0 coverage-preservation contract

The pre-0.15 suite contained 83 functional cases in `test_main.cpp` and 14 separate architecture
regressions: 97 named scenarios in total. All were inventoried before the old sources were removed.

95 legacy scenario names remain directly represented in the unified suite. The only two removed
architecture implementations are exact duplicates of stronger owning tests:

| Removed architecture case | Owning replacement |
| --- | --- |
| `test_unreferenced_fuse_spool_is_preserved_without_killing_frontend` | `test_fuse_durable_journal_preserves_unreferenced_spool` exercises startup, quarantine and continued frontend operation |
| `test_local_store_put_repairs_corrupt_existing_object_before_ack` | `test_local_store` performs corrupt-existing -> verified re-put -> verified read as part of the LocalStore contract |

The two historical FUSE torn-tail lifecycle tests are deliberately retained. They still prove real
file truncation/restart wiring. Additional exhaustive coverage is cheap: the runtime journal-frame
scanner is a production component and `test_fuse_journal_frame_scanner_exhaustive_tail_model`
exercises every possible cut inside the next valid frame, full-length invalid-checksum EOF recovery,
mid-journal corruption rejection, and fully valid framing without repeating a frontend restart.

The unified suite additionally adds state/property coverage for:

- metadata delta encode/decode/apply across 96 successive namespace/catalogue/garbage/idempotency
  transitions, with full-snapshot round-trip agreement at every state;
- hydration scheduler ordering, reinforcement, fairness and blocked-prefix semantics;
- deterministic capacity placement across 256 keys, every replica count, failure-domain diversity,
  and the R=1 monotonic-addition property over 1024 keys;
- durable atomic replacement with empty, binary and large payloads;
- torrent fetch URL safety and magnet sanitisation; and
- every `TorrentJobState`, `IngestJobState` and `CatalogueHintState` name/parse round trip.

This is a coverage-preserving migration by *situation*, not by test-file shape or raw assertion count.
Expensive process-shaped tests prove real wiring; combinatorial state-space coverage calls the actual
production state machine/policy directly instead of replaying the complete server lifecycle for every
row.

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

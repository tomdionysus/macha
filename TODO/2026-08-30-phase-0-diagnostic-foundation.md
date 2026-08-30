# Phase 0 checkpoint: diagnostic foundation

Date: 2026-08-30

Status: completed implementation slice; Phase 0 remains open

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope completed

This slice adds deterministic measurement seams for the two amplification paths
observed on the live three-node cluster. It intentionally does not change
publication, recovery, metadata authority, scheduling, or on-disk formats.

### FUSE namespace and journal diagnostics

`FuseFrontendStatus` now reports monotonic counters for:

- namespace operations admitted by this frontend instance;
- pending namespace operations loaded during recovery;
- distributed namespace publication attempts;
- operations whose `namespace_published` marker was durably appended;
- operations whose `namespace_done` marker was durably appended;
- successful operation-journal append groups;
- records contained in those append groups;
- successful fsync durability barriers for those append groups.

The journal counters increment only after the append and fsync succeed. A failed
append or rollback fsync is not reported as successful durable work. Counters are
per `FuseFrontend` instance and are not themselves persisted.

The recovered-operation counter counts only unfinished namespace operations. It
does not include pending data operations in the same journal.

### Metadata reconstruction diagnostics

`MetadataReplica::diagnostics()` returns monotonic counters for:

- calls entering `historical_locked`;
- calls that find a history entry and begin reconstruction;
- individual metadata deltas applied while reconstructing historical records.

The counters are observational only and use relaxed atomics. They do not imply
authority, acceptance, or cache state.

## Tests added

### `filesystem_fuse/test_fuse_recovery_characterizes_one_publication_per_namespace_operation`

Creates eight durable namespace operations behind the publication quiet boundary,
restarts the frontend, drains recovery, and proves the current baseline:

- eight recovered operations;
- eight publication attempts;
- eight published operations;
- eight confirmed operations;
- sixteen journal append groups and durability barriers: one published marker
  and one done marker per operation.

Phase 2 must update this test from exact per-operation equalities to bounded batch
counts.

### `storage_metadata/test_metadata_delta_chain_characterizes_repeated_head_reconstruction`

Creates a twelve-delta linear metadata history and reads the same head twice. It
proves that each lookup currently replays all twelve deltas independently.

Phase 1 must update the second-lookup expectation to demonstrate reuse of a
single validated materialization.

## Verification performed

Exact commands and observed results:

```text
cmake --build build -j 4
```

Result: successful; `macha`, `macha-tests`, and `macha-tests-runtime` built.

```text
build/macha-tests --filter test_metadata_delta_chain_characterizes_repeated_head_reconstruction --serial --verbose
```

Result: 1/1 passed.

```text
build/macha-tests --filter test_fuse_recovery_characterizes_one_publication_per_namespace_operation --serial --verbose
```

Result: 1/1 passed.

```text
build/macha-tests --filter storage_metadata
```

Result: 27/27 passed.

```text
build/macha-tests --filter filesystem_fuse
```

Result: 34/34 passed.

The complete all-subsystem suite was not run in this slice. No claim is made that
it passed.

## Files changed

- `src/fuse_frontend.hpp`
- `src/fuse_frontend.cpp`
- `src/metadata.hpp`
- `src/metadata.cpp`
- `tests/test_filesystem_fuse.cpp`
- `tests/test_storage_metadata.cpp`
- `TODO/namespace-publication-and-metadata-efficiency.md`
- this checkpoint document

## Next safe resume point

Finish the remaining Phase 0 observation seams before changing behaviour:

1. Count accepted-head persistence operations.
2. Count metadata generation notices and coalesced/scheduled convergence runs.
3. Decide whether existing RPC trace timing is sufficient or add bounded
   aggregate diagnostics by frame/message class.
4. Document a reproducible non-CI benchmark recipe.
5. Run the relevant focused tests and update the parent plan.

After those measurements exist, begin Phase 1 with a design note covering cache
ownership, pinning, eviction, concurrent same-hash deduplication, and stale-input
installation checks. Do not start by adding an unbounded `hash -> snapshot` map.

## Important continuation cautions

- The two characterization tests intentionally assert inefficient current
  behaviour. Their expected values must change when Phases 1 and 2 land.
- Cache presence must never imply metadata acceptance or authority.
- Do not perform reconstruction outside the replica lock until the install path
  has an explicit immutable-input revalidation rule.
- Do not use Git unless the user explicitly reauthorises it in that session.
- Do not claim the complete suite passes based on the two owning subsystem runs.

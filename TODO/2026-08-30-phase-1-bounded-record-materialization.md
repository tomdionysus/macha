# Phase 1 checkpoint: bounded record materialization

Date: 2026-08-30

Status: first Phase 1 slice complete; Phase 1 remains open

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Problem addressed

`MetadataReplica::historical_locked` previously began at the last full history
anchor for every lookup. Repeated acceptance, repair, and catalogue consumers of
the same head therefore replayed and re-encoded the same delta chain repeatedly.

This slice makes a validated record materialization reusable by commit hash.
It changes no on-disk format, wire protocol, acceptance rule, or branch policy.

## Implementation

`MetadataReplica` now owns a process-local materialization cache keyed by
`Hash256`. Each entry contains:

- the immutable, previously validated `MetadataRecord`;
- a monotonically increasing last-use stamp.

The cache has a soft limit of 64 entries. Eviction chooses the least recently
used reconstructible entry while pinning:

- `cur_`;
- `committed_`;
- every accepted head.

If a pathological accepted-head set itself exceeds the limit, correctness wins:
pinned entries may temporarily exceed the soft bound rather than being evicted.

History lookup now behaves as follows:

1. Confirm the requested hash still exists in durable in-memory history. Cache
   presence alone can never manufacture history or authority.
2. Return a direct cache hit when available.
3. Otherwise walk backwards only until the nearest cached ancestor or full root.
4. Apply forward deltas, validating each resulting record exactly as before.
5. Cache each validated intermediate and the requested target.

`load_history()` clears the process-local cache. `ensure_history_root()` seeds a
known materialized current/committed root. Safe history compaction clears the old
cache and reseeds the new full committed root.

Diagnostics now also report cache hits, misses, evictions, and current entry
count.

## Correctness boundaries

- Cache entries are derived only from records that pass the existing hash,
  generation, parent, delta, and merge-parent validation.
- A cached record is returned only while its hash remains present in `history_`.
- Cache state is never persisted and never treated as acceptance evidence.
- Recovery-required state and accepted-head authority rules are unchanged.
- All cache access currently remains under `MetadataReplica::m_`, matching the
  original synchronization model.

## Test changed

`storage_metadata/test_metadata_delta_chain_reuses_bounded_materialized_head`
now builds and reopens an 80-delta chain so construction-time cache state cannot
affect the assertion.

It verifies:

- the first head lookup reconstructs once and applies all 80 deltas;
- the second lookup performs no reconstruction and applies no deltas;
- the second lookup records a cache hit;
- cache entries remain at or below 64 in the ordinary one-head case;
- eviction occurs;
- an evicted early record remains reconstructible with a byte-identical payload.

Existing recovery-cache tests continue to prove that materialized state does not
self-promote to accepted authority.

## Verification performed

```text
cmake --build build -j 4
```

Result: successful; all configured targets built.

```text
build/macha-tests --filter test_metadata_delta_chain_reuses_bounded_materialized_head --serial --verbose
```

Result after the final test assertions: 1/1 passed.

```text
build/macha-tests --filter storage_metadata
```

Result after the final implementation/test changes: 27/27 passed.

```text
build/macha-tests --filter rpc_cluster
```

Result: 27/27 passed, including three-node convergence, branch reconciliation,
RPC priority, metadata floor, and replacement-node recovery coverage.

```text
build/macha-tests --filter test_catalogue_sync_search_and_artwork_gc --serial --verbose
```

Result: `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` 1/1 passed.

The complete all-subsystem suite was not run. No claim is made that it passed.

## Files changed in this slice

- `src/metadata.hpp`
- `src/metadata.cpp`
- `tests/test_storage_metadata.cpp`
- `TODO/namespace-publication-and-metadata-efficiency.md`
- this checkpoint document

Phase 0 diagnostic files remain modified from the previous documented slice.

## Remaining Phase 1 work

This slice removes repeated record reconstruction on the hot path, but it does
not complete Phase 1:

1. Cache/share the decoded immutable `MetadataSnapshot`, not only its encoded
   `MetadataRecord`. Acceptance policy and snapshot consumers still decode the
   cached payload independently.
2. Audit and convert acceptance-policy validation, accepted-head refresh,
   metadata manager snapshot views, repair, catalogue, and retention consumers
   to reuse that decoded snapshot.
3. Move expensive cache-miss reconstruction, decoding, hashing, and encoding
   outside `MetadataReplica::m_` using captured immutable inputs and an explicit
   revalidation/install step.
4. Deduplicate concurrent cache misses for the same hash once reconstruction is
   allowed outside the lock.
5. Add merge-head, concurrent lookup, stale-input race, and decoded-snapshot
   identity/reuse tests.
6. Consider byte-based cache accounting if payload/snapshot sharing no longer
   makes the 64-entry bound predictable.

## Next safe resume point

Design the decoded-snapshot cache value and consumer API before editing call
sites. The recommended value is an immutable shared materialization containing
both record and decoded snapshot. Keep cache presence separate from acceptance.

Do not move work outside `m_` in the same unreviewed step. First make consumers
share the immutable value under the existing lock model and prove merge,
recovery, and authority behaviour. Then introduce the capture/compute/revalidate
state machine as a separate checkpoint.

Do not use Git unless the user explicitly reauthorises it in that session.

# Phase 1 checkpoint: shared decoded materialization

Date: 2026-08-30

Status: shared-snapshot slice complete; Phase 1 lock-scope work remains

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope completed

The bounded metadata cache now stores a shared immutable
`MetadataMaterialization` containing both:

- the validated `MetadataRecord`;
- the decoded `MetadataSnapshot` for that exact immutable hash.

This removes a second layer of repeated work after the previous checkpoint
eliminated repeated anchor-to-head history traversal.

## Reconstruction behaviour

On a cache miss, reconstruction now:

1. Walks backwards to the nearest cached materialization or full history root.
2. Copies the parent's decoded snapshot.
3. Applies the next canonical delta.
4. Encodes and validates the resulting record exactly as before.
5. Stores the validated record and decoded snapshot together.

Later lookups share the same immutable materialization and snapshot pointers.
Eviction, pinning, history-presence checks, and the 64-entry soft bound retain
the previous checkpoint's semantics.

## Consumers converted

- Metadata acceptance policy validation, including parent transition floors.
- Node-level configured write-floor fencing before acceptance.
- Metadata manager single-head selection and decoded cache installation.
- Merge base, left-head, and right-head decoding.
- Metadata repair's selected snapshot.
- Retention release fallback views.
- Local filesystem snapshot views when the current record is in durable history.
- General `MetadataManager::cache_record` installation when the replica already
  owns the matching immutable materialization.

Proposed records not yet present in local durable history still decode directly.
The cache therefore remains an optimization over known history, not a source of
authority.

## Tests

`storage_metadata/test_metadata_delta_chain_reuses_bounded_materialized_head`
now uses a 200-delta history and verifies:

- first lookup applies 200 deltas;
- second lookup applies zero deltas;
- repeated materialization calls return the same shared materialization and
  decoded snapshot pointers;
- re-encoding the shared decoded snapshot exactly matches the committed payload;
- an unaccepted materialized head remains absent from acceptance and accepted
  heads;
- the ordinary cache remains at or below 64 entries and evicts;
- an evicted old record reconstructs byte-identically.

Existing merge, policy-transition, recovery-required, corrupt-history,
accepted-head, and three-node convergence tests provide broader regression
coverage.

## Verification performed

```text
cmake --build build -j 4
```

Result: successful.

```text
sh run-tests.sh build
```

Run after the production changes and shared-snapshot identity/authority
assertions, while the chain length was 80:

- `macha-tests`: 185/185 passed.
- `macha-tests-runtime`: 3/3 passed.
- This included
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passing.

The test was then strengthened from 80 to 200 deltas without another production
change:

```text
build/macha-tests --filter test_metadata_delta_chain_reuses_bounded_materialized_head --serial --verbose
```

Result: 1/1 passed at 200 deltas.

No claim is made that the complete suite was rerun after that test-only constant
change.

## Remaining Phase 1 work

- Expensive first-time reconstruction still occurs while holding
  `MetadataReplica::m_`.
- Cache misses from concurrent callers are serialized by that mutex rather than
  computed outside it and deduplicated through explicit in-flight state.
- Applying a delta copies the decoded snapshot's namespace map. Persistent or
  copy-on-write namespace structures are deferred and may not be necessary once
  batching reduces generation count.
- A few proposed-record paths correctly decode snapshots which are not yet in
  durable replica history.

The next implementation slice should design and test a
capture/compute/revalidate/install state machine. It must not simply unlock the
existing function around references into `history_`.

## Short UAT assessment

A short UAT is beneficial now, but only for Phase 1.

Recommended scope:

1. Run the same build on all three nodes.
2. Let a modest existing namespace backlog drain, or use a disposable test tree
   containing a few hundred entries.
3. Observe for two to five minutes while generations advance.
4. Sample each process and compare CPU/RPC behaviour with the original evidence.

Expected improvement:

- follower nodes should no longer spend most samples replaying long chains in
  `historical_locked -> encode_snapshot_for_delta -> encode_snapshot`;
- repeated consumers of the same head should hit the shared materialization;
- follower CPU and metadata RPC handler duration should drop materially;
- RPC stall notices caused by repeated follower validation should reduce.

Expected remaining limitation:

- every POSIX operation still creates its own distributed metadata generation;
- the author still encodes/publishes one new snapshot per operation;
- published/done journal markers still fsync individually;
- deletion throughput may therefore remain poor even if replicated CPU and RPC
  responsiveness improve.

Do not interpret unchanged deletion throughput alone as a Phase 1 failure. The
useful pass/fail signal is removal of duplicated follower reconstruction and a
drop in non-author CPU/RPC contention. Throughput is principally a Phase 2
namespace-batching result.

## Files changed in this slice

- `src/metadata.hpp`
- `src/metadata.cpp`
- `src/metadata_manager.hpp`
- `src/metadata_manager.cpp`
- `src/cluster.cpp`
- `src/filesystem.cpp`
- `tests/test_storage_metadata.cpp`
- `TODO/namespace-publication-and-metadata-efficiency.md`
- this checkpoint document

Do not use Git unless the user explicitly reauthorises it in that session.

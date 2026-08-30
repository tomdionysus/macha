# Phase 5 physical-object GC separation proof

Date: 2026-08-30

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope

Close the remaining Phase 5 proof that namespace publication completion does
not synchronously perform physical DATA reclamation, and that later destructive
GC remains bounded and subordinate to foreground work.

## Implementation audit

No production-code change was required.

- Namespace deletion publishes metadata and confirms its operation journal
  independently of physical object removal.
- Physical DATA GC owns a resumable `StoragePool` cursor and examines at most 64
  objects per Service maintenance slice.
- Every slice checks for foreground playback or mounted-filesystem activity and
  yields immediately when either becomes active.
- Destructive GC requires direct reachability of all durably known nodes,
  stable metadata, a complete catalogue live set, a retention-release baseline,
  and an inventory at least as new as the known metadata generation.
- Tombstoned objects remain protected until their exact configured grace
  deadline. Untombstoned orphan bytes receive at least the no-progress retry
  window so an in-flight DATA write cannot race its later metadata claim.
- A complete no-progress physical pass parks until an event. An immature
  tombstone arms one exact deadline rather than a maintenance polling cadence.

The focused storage test was strengthened to require more than one GC cursor
slice, in addition to its existing assertion that no slice examines more than
its configured three-object budget.

## Deterministic verification

- `storage_metadata/test_storage_pool_and_persistent_cache` passed before the
  assertion change. It proves resumable physical traversal, the per-slice
  operation bound, preservation of live/protected/young objects, reclamation of
  an aged orphan, foreground yielding, and age refresh when an old hash is
  reaffirmed.
- `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`
  passed. It proves namespace deletion has already published while the physical
  objects still exist, no object disappears before the 750 ms grace deadline,
  the exact event-driven deadline reclaims them without an unrelated event, and
  convergence returns to a parked state.

The strengthened storage test is rerun in the verification section below.

## Operational interpretation

Physical reclamation may intentionally trail a large namespace deletion. The
64-object slice is an examination bound rather than a promise of immediate
unlink throughput, and packed-object space is recovered later by bounded pack
compaction. This lag must not be interpreted as failure of namespace batching:
the Phase 5 live UAT separately measured 130 namespace operations completing in
six publications and under 0.3 seconds per half.

## Verification after strengthening

`build/macha-tests --filter test_storage_pool_and_persistent_cache --serial`
passed 1/1 in 1.384 seconds with the new multi-slice assertion.

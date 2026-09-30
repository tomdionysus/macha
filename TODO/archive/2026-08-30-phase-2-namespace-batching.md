# Phase 2 checkpoint: bounded namespace publication batches

Date: 2026-08-30

Status: deletion/backlog batching slice complete; Phase 2 remains open for
general mixed-operation batch identity and the full crash-injection matrix.

## Outcome

Recovered namespace work is no longer necessarily published one syscall at a
time. The FUSE namespace worker drains a bounded ordered batch and the
filesystem applies its valid prefix in one metadata mutation.

The deterministic deletion-backlog test now recovers 1,000 independent
`unlink` operations with a 256-operation limit in:

- 4 metadata publications rather than 1,000;
- 4 grouped `published` marker appends and 4 grouped `done` marker appends;
- 8 successful journal durability barriers rather than 2,000.

This directly addresses the publication, replicated materialization, network,
and journal-fsync amplification seen in the live `rm -rf` incident.

## Implemented design

### Bounded, event-driven formation

- Added `fuse.namespace_batch_operations`, defaulting to 256.
- Added `fuse.namespace_batch_bytes`, defaulting to 256 KiB.
- Both limits are validated at startup. Operation count cannot exceed the
  configured pending-operation bound; the byte limit is capped at 64 MiB.
- An operation larger than the byte bound is permitted as the first and only
  member of a batch, so it cannot permanently block the queue.
- No polling or coalescing timer was introduced. A lone live operation begins
  immediately; a known recovery or sustained backlog fills the batch directly.

### Ordered filesystem transaction

- Added a filesystem namespace-batch API supporting `mkdir`, `rmdir`, `create`,
  `unlink`, `rename`, `chmod`, `chown`, and `utimens`.
- Operations are applied in journal order to one mutable snapshot and one
  `MetadataDelta`.
- The largest valid prefix is committed when a later operation has a semantic
  failure. The blocking operation and everything after it remain queued in
  original order.
- An already-achieved replay effect advances journal state without creating a
  redundant metadata generation.
- Erased paths are canonicalised before publication, and garbage retirement is
  accumulated in the single batch delta.
- `rmdir` emptiness detection now uses an ordered-map descendant lookup instead
  of scanning the complete namespace.
- Successful renames update path-backed open-write handles while the batch lock
  is held. Existing no-op rename behaviour remains unchanged.

### Crash-compatible journal completion

The existing journal records operations individually and has no durable batch
commit identity. To preserve restart proof without changing the on-disk format,
the worker currently groups only operation families whose individual final
effects remain provable from one final snapshot:

- `unlink` and `rmdir` may share a batch, retaining child-before-parent order;
- distinct-path `mkdir` and `create` operations may share a batch;
- same-kind `chmod`, `chown`, or `utimens` operations on distinct paths may
  share a batch;
- rename and incompatible mixed families form singleton boundaries.

All operations in a published safe prefix receive individual `published`
markers in one append and durability barrier. Confirmation uses one available
snapshot, then individual `done` markers are written in one append and barrier.
Recovery can still assess any unconfirmed operation independently. No journal,
metadata, or wire-format version changed.

### Diagnostics

FUSE status now reports:

- `namespace_publication_batches`;
- `namespace_operations_batched`.

In-flight operations are included in the pending count, so status remains
accurate while one batch is publishing.

## Test coverage added or changed

- Recovery batching and grouped marker durability with an operation-count
  boundary.
- A 1,000-independent-unlink backlog proving 4 publications at a limit of 256.
- Hard encoded-size boundaries, including forward progress for one oversized
  operation.
- Child unlinks followed by parent `rmdir` in one publication.
- An all-idempotent recovery batch that creates no metadata generation.
- A partially idempotent sequence.
- Largest-valid-prefix publication and exact requeue order after a deterministic
  semantic failure.
- Runtime configuration parsing for both batch bounds.

During development, the delete/rmdir test exposed that accumulated erase paths
were in syscall order rather than the canonical order required by a metadata
delta. Sorting and deduplicating the final erase list fixed the cause; the test
was not weakened or removed.

## Verification

Against the final source and the scale case using 1,000 unlinks:

- `sh run-tests.sh build` passed core 190/190 and runtime 3/3;
- the full run included
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`, which passed;
- the exact 1,000-unlink focused test also passed 1/1 before the final full run;
- focused `filesystem_fuse`, `storage_metadata`, and `rpc_cluster` suites passed
  during implementation.

## Remaining Phase 2 work

- Add a durable batch commit identity before grouping mixed dependency chains
  such as create/rename/unlink into one publication.
- Inject restart at admission, distributed commit, grouped `published`,
  confirmation, and grouped `done` boundaries.
- Test concurrent live admission while recovery publication is gated.
- Decide whether an accepted commit hash/generation needs an explicit in-memory
  association with every prefix member.
- Retain the immediate event-driven rule unless measured interactive workload
  justifies a bounded coalescing delay; do not introduce a maintenance loop.

## Recommended short UAT

Yes. A short three-node UAT is particularly valuable at this checkpoint because
Phase 1's UAT measured idle convergence, whereas this slice changes the exact
deletion-backlog workload that originally saturated all three nodes.

Suggested exercise:

1. Run the same newly built binary on all three nodes and allow them to reach a
   quiet connected state.
2. Use a disposable directory containing 1,000 to 5,000 already committed
   files, then delete it through the mounted filesystem or replay its durable
   deletion backlog.
3. Observe metadata generation growth, namespace batch/operation counters,
   journal drain time, per-thread CPU and RSS, and health/status RPC latency.
4. Expect generation growth to be approximately `ceil(operations / 256)`, plus
   a small number of explicitly explainable compatibility boundaries.
5. Confirm that CPU returns to event-driven idle when the queue drains. If it
   does not, capture a short stack sample while it remains hot.

Namespace completion and physical object garbage collection are separate. A
paced post-grace GC tail should not be mistaken for failure of namespace
batching; report both separately.

# Phase 3 retained-memory governance checkpoint

Status: substantial local checkpoint; Phase 3 remains active

Date: 2026-09-02

## Completed in this checkpoint

- Added one configurable process-wide retained-memory ledger with explicit
  `control`, `viewer`, `loader`, and `speculative` classes, protected
  control/viewer headroom, a loader floor, work-conserving reclaimable cache
  borrowing, event-driven waits, cancellation/deadline handling, recovery
  overcommit, exact RAII release, and Status diagnostics.
- Attached ownership to FUSE request copies, durable operation/checksum
  history, publication staging and pending extents, RPC request jobs, inbound
  reassembly, outbound frames, opportunistic object persistence, immutable
  read extents, and playback segment-store capacity.
- Replaced `SharedFetch`'s copied result plus one copy per waiter with one
  immutable reference-counted `ObjectBuffer`. Reassembly ownership now follows
  the object through RPC completion, shared-fetch publication and every reader;
  the final reader releases the charge. FUSE uses this path directly.
- Preserved the old byte-returning object API as a compatibility wrapper for
  short-lived callers; long-lived internal object consumers use shared
  ownership.

## Deterministic evidence

- Priority/reserve, reclaimable shedding, recovery-overcommit and shutdown
  ownership tests pass.
- RPC reassembly is charged while partial, across completed-message/future
  handoff, and rejects loader admission before protected headroom is consumed.
- Two concurrent object readers perform one remote fetch, receive the same
  retained allocation, and release its memory only after the last reader.
- Latest relevant suites pass: RPC/cluster 43/43, filesystem 64/64,
  storage-v18 21/21, storage-metadata 37/37, invariants 41/41, and
  hydration/catalogue 24/24. One timing-sensitive catalogue convergence case
  failed during the first loaded suite run, then passed alone and in the full
  suite rerun; it is recorded rather than hidden.
- The complete process-isolated repository verification then passed 275/275
  core tests and 4/4 runtime-dependency tests at the default 12-slot setting.

## Remaining before Phase 3 exit

- Charge or bound remaining durability and metadata/catalogue/media-profile
  retained structures, including retry/queued states and cache shedding.
- Audit transient materialisation/readback buffers which still allocate around
  the governed owners, and remove any remaining full-payload compatibility
  copies from asynchronous production paths.
- After the remaining owner integrations, run a guarded loaded 4 GiB node UAT
  proving a stable RSS plateau, no swap growth/lost work, and no viewer
  regression. Loaded UAT is not yet authorised by this checkpoint.

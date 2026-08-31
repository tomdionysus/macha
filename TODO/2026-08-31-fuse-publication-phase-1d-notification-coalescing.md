# Phase 1D.4 checkpoint: publication notification coalescing

Date: 2026-08-31

Status: notification/request-storm cut complete locally; Phase 1D.4 remains
open for retirement selection and metadata-generation coalescing.

## Defect addressed

Loaded rsync observation showed 841,119 data-publication requests while only 55
publications had started. Most requests were reported as merged, but reaching
that decision still required repeated inode locking and, under spool pressure,
repeated all-inode candidate sweeps.

The pressure path kept a node-wide `spool_drain_requested` flag, but every FUSE
admission had its own `drain_requested_at_revision`. A new blocked write
therefore rescanned all dirty inodes even when another writer had already
asserted the same node-wide drain demand. Repeated flush/release notifications
also re-entered publication request handling for an unchanged durable prefix.

## Implementation

- The atomic false-to-true transition of `spool_drain_requested` now owns the
  all-inode pressure sweep. While drain demand remains asserted, further writes
  do not repeat that sweep.
- Drain demand is still cleared only by the existing real occupancy transition
  below the pressure threshold. No polling loop, timer owner or idle worker was
  added.
- Writes which become durable while pressure is already asserted are notified
  directly by the existing durability-batch completion path, so transition
  coalescing cannot strand newly durable data.
- `request_data_publication()` now treats the durable data sequence and required
  namespace sequence as one monotonic notification watermark.
- If a queued, running, deferred or unconfirmed publication already owns that
  exact watermark, flush/release/pressure duplicates are discarded before any
  shared queue work.
- A per-inode enqueue-owner bit closes the inode-lock to queue-lock handoff
  race. Concurrent requests cannot both believe they own the same queue insert.
- A genuinely newer durable watermark still merges into the existing owner and
  is published after its current bounded snapshot.

## Operational diagnostics

Status now exposes:

- `data_publication_notifications_suppressed`: unchanged-watermark signals
  discarded before queue work; and
- `spool_pressure_publication_sweeps`: node-wide pressure candidate sweeps.

`data_publication_requests` now counts effective initial or advanced-watermark
requests rather than every duplicate signal. The existing queued/running/
unconfirmed coalescing counters continue to count real watermark advances into
an existing owner.

## Deterministic proof

The new notification-watermark regression suspends publication after one inode
is queued, then emits 1,002 flush notifications around two durable write
watermarks. It proves exactly:

- two effective publication requests;
- one queued newer-watermark merge;
- 1,000 unchanged-watermark notifications suppressed;
- one pending queue owner; and
- zero publications started while the loader is deliberately suspended.

The three existing spool backpressure/bootstrap/stall regressions now also prove
that one pressure episode performs exactly one publication sweep while
preserving progress, hard bounds, shutdown wake-up and non-`ENOSPC` semantics.

## Verification

- Build: passed.
- Notification-watermark regression: passed.
- Spool pressure regressions: 3/3 passed.
- `filesystem_fuse/`: 56/56 passed.
- Complete suite with four isolated workers: 227/227 passed, including both
  catalogue regressions and sparse changed-range coverage.
- Runtime dependency suite: 3/3 passed.

No default-12-worker complete-suite result is claimed for this cut. The prior
sparse-range checkpoint separately records its 225/226 default run and clean
226/226 controlled run.

## UAT boundary

Combine this cut with the sparse changed-range UAT. During loaded
`rsync --append-verify` plus real playback/seeks, verify:

- pressure sweeps rise only on distinct drain-demand episodes, not in
  proportion to FUSE writes;
- effective publication requests track durable watermark advances and remain
  far below suppressed duplicate notifications;
- completed-cohort source reads show unchanged extent reuse;
- loader retirement remains non-zero; and
- viewer waits, control failures and filesystem timeouts remain zero.

This cut removes redundant notification and sweep work. It does not yet combine
compatible durable append ranges into fewer publication generations, choose the
largest retirement return under full-spool pressure, or reserve a retirement
ticket. Those remain the next Phase 1D.4 work.

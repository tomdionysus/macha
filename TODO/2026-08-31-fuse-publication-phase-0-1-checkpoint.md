# FUSE publication throughput: Phase 0/1 first implementation checkpoint

Date: 2026-08-31

Parent plan: `TODO/2026-08-31-fuse-publication-throughput-plan.md`

Status: deterministic implementation checkpoint complete; deployment/UAT and
the remaining Phase 0/1 bounds are pending

## Implemented

- Corrected foreground classification in the FUSE adapter. Read-only open and
  read operations signal viewer demand; creates, writes, flushes, fsyncs,
  releases, and unrelated metadata operations no longer manufacture a viewer
  quiet window.
- Removed the scheduler rule which treated any open writer or recent mount
  operation as a reason to cap all live publication at
  `foreground_commit_workers`. When there is no viewer demand, live ingest is
  now work conserving up to `commit_workers`.
- Added per-inode writable-handle accounting and select closed live files ahead
  of open live loader files. Recovery retains its independent bounded worker
  budget and remains behind live work.
- Preserved the existing event-driven viewer gate before every bounded replay
  chunk. Added a second viewer-demand check after acquiring the serialized
  metadata/commit boundary, preventing several publishers which passed an
  earlier check from forming a queued commit train in front of a newly arrived
  viewer.
- Split the misleading merged-publication total into explicit request,
  queued/running/unconfirmed coalescing, actual start/completion, peak active,
  closed-priority selection, and useful byte counters. These are lock-free
  process-lifetime diagnostics exposed through Status.
- Retained `foreground_commit_workers` as a parsed compatibility setting and
  documented that it no longer classifies loader writes as viewer activity.

## Deterministic evidence

New tests prove:

- four durable files whose writable handles remain open reach more than one
  active publisher despite `foreground_commit_workers: 1`;
- peak activity never exceeds `commit_workers`;
- repeated demand for already queued inodes coalesces into one publication per
  file;
- bytes read, committed, and confirmed reconcile exactly;
- a closed file queued behind an open loader inode is selected first; and
- the existing playback-yield contract remains intact.

Verification on the final source:

- build completed successfully;
- all 47 `filesystem_fuse` tests passed after the commit-boundary viewer recheck;
- all three runtime dependency tests passed;
- a complete 213-test run passed on the final source in 72.149 seconds,
  including
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` and
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`.

## Deliberately not claimed complete

- Phase 0 still needs raw disk/network baselines and fuller per-stage latency
  accounting.
- Phase 1 still needs an explicit in-flight-byte bound and resumable/fair
  publication quanta. Worker count currently bounds concurrent `WriteHandle`
  state, but a closed file arriving after every worker has entered a very large
  open-file generation cannot yet pre-empt one safely.
- No deployed throughput improvement is claimed until three-node UAT.
- No claim is made that the current whole-file completion EWMA is sufficient;
  continuous progress feedback remains later plan work.

## UAT checkpoint

The active rsync plus active Ted Lasso playback is a particularly useful first
deployment test:

1. Deploy the same build to all three nodes without stopping playback merely to
   make the test easier.
2. Confirm read-only open/read demand causes queued publication to yield and
   that playback startup, seeks, and sustained reads show no new wait or buffer.
3. While playback is quiet, confirm `data_publication_peak_active` rises above
   one, actual publication bytes advance, and complete files become
   authoritative/catalogued during the continuing rsync.
4. Confirm repeated-demand counters may rise but actual starts/completions
   remain proportional to file generations rather than write callbacks.
5. Record spool occupancy, throttle rate/waits, per-node CPU, disk/network work,
   RPC latency/timeouts, completed files, and catalogue latency.
6. Stop or seek playback during the observation and confirm ingest promptly
   returns to available capacity after the viewer quiet interval.

The UAT is useful now, but this checkpoint must remain active afterward for the
explicit byte bound, fair quanta, and physical baseline work.

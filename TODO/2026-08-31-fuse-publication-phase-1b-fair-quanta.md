# Phase 1B checkpoint: fair resumable publication quanta

Date: 2026-08-31

## Objective

Prevent a multi-gigabyte spool generation from monopolising a publication
worker while independently bounding concurrently admitted publication work.
This must preserve the two scheduling laws: viewers never wait for new loader
work, and loaders do not wait when they can run without delaying viewers.

## Implemented

- Added `fuse.publication_quantum_bytes`, defaulting to 32 MiB.
- Added `fuse.publication_inflight_bytes`, defaulting to 256 MiB.
- Validated that a quantum is an extent-size multiple and that the aggregate
  budget is a whole number of quanta.
- Replaced whole-generation worker ownership with a process-lifetime cursor per
  inode. It retains the provisional `WriteHandle`, operation index, spool
  offset, checksum position, and recovery provenance across scheduler turns.
- A worker consumes at most one byte quantum, releases its admitted-byte token,
  and requeues unfinished work at the tail. One inode still has at most one
  active generation.
- Viewer activity stops new quantum admission. Running work checks the viewer
  gate between 256 KiB durable spool chunks and yields without discarding its
  cursor.
- Metadata publication remains a final whole-generation operation under the
  existing serial boundary. No partial file becomes visible.
- Crash semantics remain journal-authoritative. A process loss discards only
  provisional cursor state; the durable spool generation replays normally.
- Publication rate accounting now measures active service time rather than
  queue/viewer pauses, avoiding false throttling from fair yields.
- Added diagnostics for total quanta, yields, and peak admitted bytes.

## Deterministic verification

`test_fuse_publication_quanta_are_fair_and_byte_bounded` configures four workers
but permits one 1 MiB quantum in flight. It queues an 8 MiB closed file before a
64 KiB closed file and proves:

- the small file becomes visible after the large file's first quantum;
- the large file is still invisible at that point;
- peak active workers and admitted bytes remain at the configured bound;
- the generation yields repeatedly; and
- total spool bytes read equal useful input bytes, demonstrating that a yield
  does not restart or restage the prefix.

Verification after the final change:

- clean incremental build: passed;
- `filesystem_fuse`: 48/48 passed;
- runtime/dependency suite: 3/3 passed; and
- complete suite at four parallel slots: 215/215 passed.

An earlier complete-suite run at twelve slots had one unrelated catalogue test
time out (`test_catalogue_uses_final_state_after_coalesced_metadata_burst`). It
then passed alone in 883 ms and passed in the complete four-slot run in 799 ms.
This is recorded rather than describing that earlier run as green.

## Three-node UAT result

The 2026-08-31 mixed Phase 1A/1B UAT passed loader classification, fair quantum,
byte-bound, useful-work, cluster-health and control-queue checks, but failed the
real viewer-pre-emption check. The FUSE adapter records actual reads on the
interactive activity clock while publication watches the foreground clock. The
local deterministic test called the latter directly and therefore missed the
adapter mismatch. Phase 1B remains active. Full observations are in
[2026-08-31-fuse-publication-phase-1ab-uat.md](2026-08-31-fuse-publication-phase-1ab-uat.md).

The mismatch has since been corrected locally and the regression now enters
through the public hook used by the adapter after publication is already in
progress. FUSE 48/48, runtime 3/3, and the complete 215/215 suite pass. At that
local checkpoint a repeat live viewer probe remained before closing Phase 1B.

The repeat deployed probe passed: the 4 MiB FUSE read completed in 0.40 seconds,
the quantum count remained fixed throughout the observed viewer window after
already-running bounded work retired, and publication resumed immediately at
quiet-window expiry with no failure or timeout. Phase 1B is complete; the
separate synchronous-I/O throughput limitation continues into the next phase.

# FUSE publication Phase 1A: loader priority checkpoint

Date: 2026-08-31

Parent plan: `TODO/2026-08-31-fuse-publication-throughput-plan.md`

Status: implementation, deterministic verification, and coordinated loader
classification UAT complete; live viewer-pre-emption leg pending

## Corrected model

Journal recovery is provenance, not scheduling priority. Durable spool bytes
represent user-requested loader work before and after process restart. The
priority order is now:

1. independent control execution;
2. viewer foreground;
3. viewer read-ahead;
4. user loader publication; and
5. speculative maintenance.

## Implemented

- Added `FrameType::loader` with a new wire value. Existing wire values remain
  unchanged, and `frame_type_priority()` explicitly places loader below
  read-ahead and above speculative traffic.
- Added a dedicated loader queue to inbound DATA execution. Viewer foreground
  retains reserved worker capacity; read-ahead, loader, and speculative queues
  are selected in that order.
- Added loader frame timing diagnostics, encoding/decoding, queue cancellation,
  promotion traversal, and human-readable status naming.
- Routed distributed file object puts and publication durability barriers as
  loader traffic. Loader activity does not update foreground or read-ahead
  activity accounting and therefore cannot manufacture its own viewer gate.
- Kept catalogue artwork writes explicitly speculative.
- Replaced the FUSE queue's overloaded recovery scheduling flag with recovered
  provenance. Recovered provenance still controls spool validation, corruption
  handling, cache bypass, and diagnostics.
- Journal-restored closed and open spool files now occupy the same loader lanes
  as files accepted in the current process. They may use `commit_workers` and
  are no longer capped by `recovery_commit_workers`.
- Retained `recovery_commit_workers` parsing as a compatibility setting. It has
  no effect on user spool publication and may later be removed or assigned only
  to genuinely background recovery work.

## Verification

A clean rebuild completed successfully.

Focused evidence:

- loader wire encode/decode, priority ordering, and frame diagnostics passed;
- loader publication did not increment viewer/read-ahead activity;
- foreground RPC completed while loader workers were saturated;
- journal-restored spool started without new FUSE activity;
- four restored user files exceeded `recovery_commit_workers: 2` while staying
  within `commit_workers: 4`; and
- restored loader concurrency exceeded `recovery_commit_workers: 1` while all
  final file contents remained exact.

Affected groups:

- all 36 `rpc_cluster` tests passed;
- all 47 `filesystem_fuse` tests passed; and
- all 3 runtime/configuration tests passed.

Final complete verification on the same source passed all 214 main tests in
66.465 seconds. This included
`hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` and
`hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`.

## Deployment and UAT requirement

This cut introduces loader wire value 5. Older binaries reject unknown frame
types rather than silently interpreting them as another class. Until feature
negotiation is added, all three nodes must be deployed together; a rolling
mixed-version UAT is not valid.

After coordinated deployment, restart the import node with a non-empty durable
spool and confirm:

- Status exposes a `loader` RPC timing bucket for publication puts/barriers;
- recovered-provenance active publishers can rise above the configured legacy
  recovery worker value and remain at or below `commit_workers`;
- confirmed and retired bytes resume without a restart-induced two-worker cap;
- a viewer start/seek promptly gates new publication quanta and receives
  foreground execution capacity without RPC timeout;
- loader work resumes immediately after the viewer quiet interval; and
- control/status remains responsive.

This checkpoint corrects priority but does not itself satisfy the throughput
contract. Phase 1B must add the explicit in-flight-byte bound and fair resumable
quanta. End-to-end acceptance remains confirmed/retired throughput, usable-file
latency, and catalogue visibility relative to the direct physical baseline.

## 2026-08-31 coordinated deployment observation

All three nodes were deployed together and formed a healthy, writable
three-voter cluster. The two Linux installed binaries had identical SHA-256
`283d3257904987e7aad6f10098a3ccbb27c0b100fbd06d413e8fea0716e1c7d0`;
the macOS platform build had SHA-256
`fd668211ce6dc501571813c369d6bcc47b20337b5b2296d06b1c2ba21d4418a7`.
Every node reported version 0.21.0 and the cluster advanced from metadata
generation 267 to 268 during the sample.

The loader class worked end to end. Node 51's loader requests advanced from 95
to 263, including 263 deferred object puts and three durability barriers. Loader
queue wait remained at or below 100 microseconds. The longest barrier handler
was 1.151 seconds, but it remained on the loader executor: ping queue maxima
were 149 microseconds on node 50 and 146 microseconds on node 51. There were no
filesystem timeouts, backend failures, rejected metadata jobs, or metadata job
backlogs.

Node 50's recovered publication read advanced from 1,701,576,704 to
3,004,170,240 bytes over 144.907 seconds, about 8.99 MB/s internal replay. One
990,904,320-byte generation completed, committed, and confirmed, giving about
6.84 MB/s end-to-end confirmed throughput over that observation. The rsync
receiver was directly observed blocked in a 256 KiB FUSE write and later
advanced from 62,390,272 to 333,971,456 physical write bytes, proving loader
admission resumed when publication created room.

Peak publication remained two for a legitimate workload-shape reason: the
16 GiB spool contained exactly two files, approximately 13 GiB and 3.1 GiB.
There were no additional independent inodes with which to exceed two workers.
This does not indicate that `recovery_commit_workers: 2` still caps the
scheduler; the loader frame class and deterministic four-inode restart test
prove the removed cap directly.

The live result rejects throughput, not priority correctness. Spool occupancy
rose from 16,744,500,753 to 17,076,375,057 bytes (about 99.4% of its limit),
throttle waits reached 1,267 with 124.645 seconds of aggregate delay, and a
completed generation did not reclaim the spool file because newer operations
for the same inode remained. Two large inodes, serial extent puts, and
whole-file/generation capacity reclamation therefore dominate this workload.
Phase 1B fair resumable quanta and Phase 2 incremental durable extent staging
are required before performance can approach direct copy.

No live viewer was started during this observation. Deterministic viewer
priority and worker-reserve tests pass, but the deployment checkpoint remains
active until playback startup/seek is sampled under loader saturation. That leg
can be combined with the next Phase 1B UAT rather than delaying implementation.

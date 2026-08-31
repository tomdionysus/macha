# FUSE publication throughput: Phase 0/1 first implementation checkpoint

Date: 2026-08-31

Parent plan: `TODO/2026-08-31-fuse-publication-throughput-plan.md`

Status: first implementation checkpoint complete; three-node correctness UAT
passed, but end-to-end throughput UAT failed; Phase 1 remains active

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

## 2026-08-31 deployment observation: partial UAT

All three running processes contain the checkpoint:

- node 50's mapped `/proc/44890/exe`, installed `/usr/bin/macha`, and
  `/root/macha/build/macha` all had SHA-256
  `f7253279a526dbfbe60e7e9310aedc9a3d58018f714d5af22b0ed3c5560ac1a8`;
- node 51's mapped `/proc/70687/exe`, installed `/usr/bin/macha`, and build
  output had the identical SHA-256;
- the macOS process mapped inode `13421689` from
  `/Users/tom/devroot/macha/build/macha`; the current file had that inode and
  SHA-256 `d9a1775b7c671b7a26cbbc1fa16da59e1d3ec847eacc004dff3c209194b73921`;
  and
- every live Status endpoint exposed the new publication counters and reported
  version 0.21.0.

The cluster was healthy and writable with all three nodes online at metadata
generation 226. Node 50 recovered 13,769,098,980 spool bytes (about 80.1% of the
16 GiB limit). It started three publication generations, reached peak
concurrency two, and completed/confirmed two generations totalling
1,130,354,405 dirty bytes. The third generation read 32 MiB beyond that total
before viewer demand held it at a safe replay boundary. The early whole-file
EWMA was 6,641,907 bytes/s, but two completions are not enough to call that a
sustained throughput result.

Across an observation of more than 100 seconds, node 50's publication byte and
generation counters remained unchanged while Ted Lasso playback continued to
cause remote object reads on node 51 (`get_object` requests advanced from 123
to 132). This is positive evidence for the first scheduling law: ingest work
did not continue competing while the viewer remained active. Node 50 briefly
used about two cores during viewer-facing HTTP/media work, then returned to
about 1.6% process CPU; node 51 was about 1.4%. Control RPC queues remained
empty, ping queue maxima stayed in the microsecond-to-low-millisecond range,
and filesystem timeout/backend-failure counters remained zero.

Node 50's resident set was about 1.2-1.43 GiB during the sample. It was stable,
not growing over the observation, but should be tracked when publication
resumes because the remaining Phase 1 work includes an explicit in-flight-byte
bound.

This does **not** complete the loader half of UAT. No `rsync` process existed on
node 50, node 51, or the macOS node, and node 50 reported zero new durability
writes since its restart. The current work is recovered-spool drain, not a live
rsync feed. A later sample must observe publication resuming after playback
becomes quiet and a real loader accepting new writes before the checkpoint can
be called complete.

## 2026-08-31 deployment observation: loader/publication correctness UAT

After playback stopped and rsync was restarted, publication resumed promptly.
Before the measured loader interval, node 50 had advanced from metadata
generation 226 to 232, completed eight recovered publication generations, and
confirmed 3,374,402,691 bytes. The active publisher count reached two, matching
the two currently runnable recovered inodes rather than the old foreground cap
of one.

The rsync receiver was directly observed blocked in Linux syscall 64,
`write(fd=3, ..., 262144)`, where fd 3 was the destination Reservoir Dogs file
on `/mnt/machamedia`. This was intentional proportional backpressure, not an
erroneous viewer gate or failed request: the same process had already completed
612,106,240 bytes of physical writes, Macha's throttle counters were advancing,
and `timed_out_requests` remained zero.

Across a 53.658-second endpoint-to-endpoint interval:

- rsync admitted another 279,183,360 bytes, about 5.20 MB/s;
- recovered publication read another 617,086,976 bytes, about 11.50 MB/s;
- spool occupancy therefore rose by only 292,028,416 bytes, from
  12,122,214,726 to 12,414,243,142 bytes (about 72.3% of the 16 GiB limit);
- 1,113 FUSE durability writes completed while throttle waits rose by 1,114
  and aggregate throttle delay rose by 52,352 ms;
- two recovered publications remained active; no whole file crossed its final
  confirmation boundary during this short interval, so confirmed bytes and the
  whole-file EWMA correctly remained unchanged; and
- node 51 accepted another 81 deferred object puts while playback `get_object`
  traffic remained unchanged at 132 requests.

All three nodes remained online, healthy, writable, and converged at cluster
generation 232. Control responsiveness was preserved: ping queue maxima
remained below one millisecond on the receiving nodes during this interval,
with no filesystem timeout, backend failure, RPC failure, or metadata job
backlog. This completes the first checkpoint's live correctness proof: loader
input and recovered publication make simultaneous progress, input is slowed
rather than rejected, and neither path occupies critical communication queues.
It does not constitute an end-to-end throughput pass.

The result does not complete the parent performance plan. Continuous replay was
about 11.5 MB/s and peak publication concurrency was two; resident memory on
node 50 remained about 1.43 GiB. The explicit byte bound, fair resumable quanta,
and stage-level physical profiling remain required before claiming the intended
massive throughput improvement. Status also continued to show the already
tracked client/API symptom where a stale per-node sample can transiently report
metadata generation 0 or an older generation even though the endpoint's
cluster-level generation and quorum state are current.

## 2026-08-31 throughput rejection

The operator observed only two movies becoming usable in approximately one
hour, whereas copying the same material directly into Plex takes minutes. That
is the relevant acceptance criterion, and it rejects this checkpoint as a
performance result.

A later live sample made the discrepancy explicit: node 50 read another
3,024,093,184 spool bytes while committed and confirmed bytes remained fixed at
3,374,402,691. Two publications remained active, and 14,906 requests had
coalesced behind running generations. The replay-read rate therefore measures
internal effort, not useful end-to-end completion, and must not be presented as
throughput success.

The immediate concurrency ceiling exposed a classification defect. The backlog
was user-requested loader work reconstructed from its journal, but the deployed
code treated recovered provenance as background priority and applied
`recovery_commit_workers: 2` despite `commit_workers: 8`. Each resulting worker
also performed extent puts serially. The roughly 11.5 MB/s aggregate replay
rate was consequently only two serial publication streams followed by a
whole-file commit boundary.

Phase 1A now corrects that classification: journal-restored spool remains loader
work, while recovered provenance controls only replay validation, cache policy,
crash handling, and diagnostics. The compatibility setting no longer caps user
publication. Phase 1B still needs an explicit in-flight-byte bound and fair
quanta; Phase 2/4 must then eliminate repeated whole-generation work and
pipeline immutable extent transfer. Future UAT is judged primarily by
confirmed/retired bytes, usable-file latency, and catalogue visibility—not
bytes reread.

# Phase 1A/1B three-node UAT

Date: 2026-08-31

Result: **passed after viewer-signal correction and repeat deployment**

## Workload

- Three healthy, writable nodes: `10.44.1.50`, `10.44.1.51`, and
  `10.44.1.200`.
- Rsync was writing a movie tree through the FUSE mount on node 50.
- Node 50 had two durable spool generations: approximately 13 GiB and 2.4 GiB.
- Live configuration used eight commit workers, a 5 second publication quiet
  window, 4 MiB extents, and the new defaults of a 32 MiB quantum and 256 MiB
  aggregate admitted-byte budget.

## Passed observations

- Cluster health remained healthy, metadata writable, generation 277, with all
  three nodes online.
- Node 50 started exactly two publications. Peak activity was two workers and
  peak admitted work was 64 MiB, consistent with two 32 MiB quanta and below
  the 256 MiB limit.
- Counters advanced from 41 to 119 quanta and from 1,350,828,032 to
  3,967,025,152 useful bytes read. The delta is 78 exact 32 MiB quanta:
  2,616,197,120 bytes. There is no prefix reread/amplification in this sample.
- Over 323.2 seconds that delta is about 7.72 MiB/s aggregate. Fair sharing
  explains why neither whole-file generation had committed during the sample;
  both retained provisional cursors and remained atomically invisible.
- Node 51 received loader-class traffic. Loader requests rose from 95 to 404;
  maximum loader queue wait remained 83 microseconds.
- Node 50 control queue maximum was 755 microseconds and node 51's was 153
  microseconds. No FUSE request timeouts or backend failures were reported.
- Node 50 was not continuously CPU-bound. A per-thread snapshot found all Macha
  threads asleep with the host at 25.6% I/O wait. This identifies synchronous
  storage/durability work, not control-thread starvation or a continuous CPU
  spin, as the remaining throughput limitation in this interval.

## Failed viewer-pre-emption observation

A bounded 4 MiB read through `/mnt/machamedia` took roughly three seconds of the
3.5 second SSH command wall time while publication was active. Publication
continued instead of observing the configured five-second quiet window.

The code path explains the result:

- `FuseLatency` marks FUSE `open`/`read` callbacks as viewer-critical;
- it calls `FuseFrontend::note_interactive_activity()`;
- that records `DistributedStore::interactive_activity()`; but
- `State::playback_quiet()` gates publication using
  `DistributedStore::foreground_idle_for()`.

The deterministic playback-yield test stimulates `foreground_activity()`
directly, so it proves the scheduler gate but not that the real FUSE adapter
drives that gate. This is a test coverage gap and a production signal mismatch.

## Required correction before repeat UAT

1. Route actual viewer-critical FUSE open/read demand to the same foreground
   clock used by publication gating, without reclassifying loader writes.
2. Add a regression test which enters through the adapter-equivalent activity
   hook rather than directly calling the distributed-store foreground clock.
3. Prove a running generation yields at its next bounded chunk and admits no new
   quantum during the configured quiet interval.
4. Repeat the bounded FUSE read while rsync publication is active.

## Correction implemented locally

The first three items are complete:

- `FuseLatency` now calls the explicitly named
  `FuseFrontend::note_viewer_activity()` hook for viewer-critical open/read
  callbacks.
- That hook records foreground activity through `FileSystem`, which is the
  exact clock consumed by `State::playback_quiet()`, and wakes parked data
  workers so they immediately observe the changed gate.
- Loader writes and read-ahead retain their existing interactive/loader
  classifications; they do not refresh the viewer clock.
- `test_fuse_publication_yields_to_playback` now enters through the public
  adapter hook after an 8 MiB publication has already yielded. It proves the
  active bounded quantum retires, no new quantum starts during the quiet
  interval, partial metadata remains invisible, and the retained cursor later
  resumes and commits successfully.

Verification after this correction:

- targeted viewer-pre-emption regression: passed;
- complete `filesystem_fuse` group: 48/48 passed;
- runtime/dependency suite: 3/3 passed; and
- complete suite at four slots: 215/215 passed, including
  `test_catalogue_sync_search_and_artwork_gc` and the coalesced catalogue burst
  test.

At this local correction point, the only remaining item was redeployment and
repetition of the bounded live FUSE viewer probe; that result follows below.

## Repeat deployed viewer UAT

After redeployment, node 50 recovered with a newly restarted rsync and a new
spool workload. The cluster was healthy with all three nodes online. Seven
independent generations initially reached seven active 32 MiB quanta, with a
224 MiB peak below the 256 MiB configured budget.

The same bounded 4 MiB FUSE read completed in 0.40 seconds including SSH
overhead, versus roughly 3.5 seconds before the correction. Immediately after
the viewer signal:

- the admitted-quantum count remained exactly 28 across five samples;
- 13.6 MiB of an already-running bounded operation retired, then useful-byte
  progress stopped;
- a publisher yielded and no replacement quantum was admitted; and
- partial file metadata remained unpublished.

After the configured five-second quiet window, publication resumed immediately
and advanced from 28 to 36 quanta. The cluster remained healthy, node 50's
control queue maximum was 259 microseconds, and FUSE backend failures and
request timeouts both remained zero. This closes the Phase 1A/1B UAT.

## Performance conclusion

Fair resumable scheduling and loader RPC isolation work as designed, but they do
not by themselves deliver the required ingest throughput. The observed
7.72 MiB/s aggregate path is dominated by synchronous extent/durability I/O on
node 50. After viewer signalling is corrected, the next throughput work remains
within-file pipelining and narrowing/grouping synchronous durability operations.

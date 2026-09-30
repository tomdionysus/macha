# Spool progress-bootstrap admission checkpoint

Date: 2026-08-31

Status: implementation, native verification and resumed-writer deployment UAT complete.

## Failure corrected

The loaded three-node UAT left node 50 at 8,590,147,750 spool bytes, only
213,158 bytes above the 50% soft threshold. Publication was making resumable
partial progress, but no complete spool file had retired and
`spool_publish_rate_bytes_per_second` was still zero. `reserve_spool_bytes()`
therefore put the FUSE writer to sleep with no deadline. Only a later
whole-file retirement could change the admission revision and wake it.

## Implementation

- A publication generation now tracks spool input replayed since its previous
  successfully drained quantum.
- Only after `drain_staging()` completes does that input grant one-for-one
  bootstrap admission credit. A scheduling turn, buffered input, failed
  staging, materialisation work and rebuild work grant no credit.
- While occupancy is at or above 50% and the physical retirement-rate estimate
  is still zero, a writer may consume those progress credits.
- Credits are capped at the configured soft-to-hard spool headroom. The hard
  byte limit and physical free-space reserve remain unconditional.
- Each credited progress event advances the existing admission revision and
  wakes blocked writers. There is no polling loop or maintenance timer.
- Falling below 50% clears unused progress credit together with the pressure
  epoch.
- Once a whole-file retirement establishes a sustainable aggregate rate, the
  existing occupancy/rate deadline pacing remains authoritative.

This deliberately does not release spool accounting early. Atomic metadata
visibility and restart recovery still retain source spool ranges until the
complete generation is confirmed. Safe incremental range retirement remains a
later versioned-staging phase.

## Deterministic proof

`test_fuse_spool_threshold_bootstraps_from_partial_publication`:

1. crosses a reduced spool's 50% threshold with a multi-quantum open file;
2. starts a second write before any retirement-rate sample exists;
3. proves that write initially blocks;
4. waits for a quantum which has actually replayed one MiB of spool input;
5. proves the second write wakes while the first file is still incomplete and
   the retirement rate remains zero; and
6. proves occupancy remains within the hard bound.

The regression also distinguished a zero-byte setup yield from useful
publication progress. Zero-byte scheduler turns correctly grant no capacity.

## Verification

- Build passed.
- Focused progress-bootstrap regression passed in 0.5 seconds.
- All three spool-pressure regressions passed together.
- Complete filesystem/FUSE group: 54/54 passed.
- The complete project suite was not run at this checkpoint.

## Related scheduler finding

The first Linux test formulation exposed that slow publication setup could
consume the entire weighted loader slice before producing bytes.
`WeightedLoaderService` then charged that zero-byte setup duration as active
loader service and applied the full proportional cooldown. Admission is now
tracked as active immediately, but its weighted service clock begins only when
the resumable writer is ready to perform bounded useful work. A scheduler-level
regression proves a ten-second cold setup does not enlarge the subsequent
25 ms service slice or its 475 ms cooldown at 95:5.

## Deployment UAT

Deploy the same binary to all three nodes and restart the real
`rsync --append-verify`. Before starting playback, verify that a writer above
50% occupancy advances after each drained publication quantum even while
`spool_publish_rate_bytes_per_second` remains zero. Then combine with genuine
playback/seek. This cut addresses the rsync freeze only; it does not yet make
viewer priority non-bypassable at remote disk/object-transfer granularity.

### 2026-08-31 deployment checkpoint

- Nodes 50 and 51 built the corrected source natively and passed the weighted
  loader regression plus all three spool-pressure regressions.
- Both installed/running executables matched SHA-256
  `b41498418d40113c117798d0be8df804b7e84c546cc98d0c7186c41e497bd2ff`.
- After sequential restart, the cluster was healthy, writable and 3/3 online;
  node 200's restarted API was reachable from node 50.
- Node 50 recovered 12,632,680,880 durable spool bytes. Across a 20-second
  loaded sample, replay advanced 311,951,360 bytes through ten bounded quanta
  and ten clean yields (about 14.9 MiB/s), with zero backend failures and no
  control/RPC-thread ownership of the publication work.
- Stack inspection also found the pre-existing 1 ms completion poll in
  `DistributedStore::put_impl` active in each deferred extent task. It did not
  stop forward progress in this sample, but is avoidable polling and is now an
  explicit Phase 4B item in `ACTIVE.md`.
- The pre-restart rsync exited when FUSE was remounted. The decisive live
  writer-above-50% admission check therefore still requires the operator to
  restart rsync; do not mark the deployment UAT complete from recovery replay
  alone.

### Resumed rsync plus playback result

The operator restarted `rsync --append-verify` and genuine direct playback on
node 50. The spool-specific deployment gate passed:

- Direct playback of *A Beautiful Mind* probed in 200 ms and created its
  session in 211 ms, with no pipeline, transcode-limit or media-read failure.
- Remote 4 MiB viewer reads observed during playback completed in 818, 885,
  1,251 and 1,726 ms. This is materially better than the previous 9.273-second
  seek failure, but resource-level viewer latency remains Phase 1D work rather
  than being claimed complete here.
- During a sustained 30-second overlap, rsync advanced approximately
  506,132,224 input bytes and 510,786,351 output bytes while Macha replayed
  441,188,352 spool bytes through 14 bounded quanta and 14 clean yields.
- Occupancy moved from 10,326,648,294 to 10,833,110,502 bytes. Admission
  recorded 1,930 additional waits and 28,016 aggregate wait-ms, but continued
  to accept data rather than freezing or returning ENOSPC.
- The cluster remained healthy, writable and 3/3 with zero filesystem backend
  failures. The cumulative control queue-wait maximum remained 352 us while
  control request count continued increasing.

This closes the zero-rate/progress-bootstrap and weighted loader non-starvation
cut. It does not close the separate invariant that viewer priority must remain
non-bypassable through physical disk/object-transfer resources.

# Aggregate spool retirement-rate deployed UAT

Date: 2026-08-31

Result: paused on a scheduler prerequisite. The aggregate estimator produced a
correct first live sample; the workload then exposed incorrect FUSE/viewer
classification and an exclusive viewer gate. Do not mark this UAT passed until
Phase 1C is deployed and the two-retirement check is repeated.

## Environment

- Three nodes online, healthy, version 0.21.0, metadata generation 416.
- Node 50 owned the FUSE spool and publication frontend.
- Spool hard limit: 17,179,869,184 bytes.
- Live workload: `rsync --recursive --append-verify` from `/mnt/diskA/Movies`
  into `/mnt/machamedia/Movies`.

## Evidence obtained

- Initial spool occupancy was 11,952,851,317 bytes (69.6% of the hard limit).
- Fourteen publications started, eight were concurrently active at peak, and
  the bounded extent pipeline peaked at two extents.
- Useful publication reads advanced from 1,017,643,008 to 4,921,176,939 bytes
  before the exclusive viewer gate suppressed further quanta.
- One 3,115,266,923-byte spool file retired physically. The shared estimator
  reported a 444,906 ms window and 7,002,065 B/s, equal to cumulative retired
  bytes divided by shared elapsed time to integer precision.
- Spool occupancy fell to 8,838,108,682 bytes and remained below the hard bound.
- All three cluster APIs remained healthy at generation 416. No RPC request was
  rejected and no FUSE timeout occurred during the observation.
- Control queue maxima remained below one millisecond on nodes 50 and 200. A
  read-only thread snapshot showed condition/future waits rather than CPU spin.

## Scheduler defect exposed

The destination-side rsync child held the existing file open for both reading
and writing. It was reading the FUSE destination prefix for `--append-verify`:

- source size: 13,425,757,713 bytes;
- existing destination size: 7,676,887,040 bytes;
- observed destination read position: 2,642,259,968 bytes; and
- observed progress: 33,587,200 bytes in five seconds, about 6.4 MiB/s.

These FUSE convenience/verification reads refreshed the viewer-activity clock.
The binary `playback_quiet()` gate consequently stopped new publication quanta
and held commit work behind a quiet interval. This was not an rsync deadlock:
the read position advanced, but slowly, while publication was starved.

The mount is explicitly for ingest and convenience access. Therefore all FUSE
traffic is loader class; only the real viewer/streaming path may emit viewer
demand. Viewer priority itself is dominant weighted service, not indefinite
exclusion: configurable relative weights default to 95 viewer and 5 loader,
with unused capacity borrowed by whichever class is runnable.

## Resume criteria

Implement and locally verify Phase 1C in
[the throughput plan](2026-08-31-fuse-publication-throughput-plan.md#phase-1c-weighted-viewerloader-scheduling-and-fuse-classification),
then repeat this UAT. Require:

1. at least two physical retirements while occupancy remains above 50%;
2. cumulative window bytes and elapsed-time arithmetic remain exact;
3. FUSE append verification does not emit viewer demand or stop publication;
4. real streaming playback receives the configured dominant share while loader
   quanta continue to complete;
5. idle-viewer capacity is borrowed fully by the loader; and
6. the hard spool bound, control latency, failure count and timeout count remain
   healthy.

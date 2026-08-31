# Phase 1D loaded UAT failure — 2026-08-31

## Scope and result

The three-node cluster was tested with a real
`rsync --recursive --append-verify` ingest through node 50's FUSE mount while
starting and seeking real playback. The cluster remained healthy and 3/3, but
the UAT **failed** both the viewer-latency invariant and continuous-loader
progress requirements. No application code was changed during this
observation.

## Viewer evidence

- A transcoded playback session started in 4,369 ms. Its remote 4 MiB probe
  read took 1,160 ms and its first fragment took 2,938 ms after pipeline start.
- A seek at 13:12:33 took 9,273 ms to produce its first fragment.
- During the same interval, append-verify FUSE reads for Apollo 13 continued at
  roughly 1.1–1.8 seconds each, with outliers of 3.7, 5.7 and 8.1 seconds.
- Playback later improved, but eventual cache/resource warming does not satisfy
  “Thou Shalt Not Make The Viewer Wait”.

RPC queue wait was not the bottleneck. Node 51 reported loader and foreground
queue waits in tens of microseconds, while `get_object` handling and full object
transfer were already in progress. FUSE correctly labels its reads `loader`,
but the 95:5 publication scheduler does not govern FUSE reads or the
synchronous local-store/remote-object service beneath the RPC queue. A loader
request can therefore occupy disk and transfer service until a complete extent
finishes; foreground can win only at the next admission boundary. Priority is
still bypassable below the scheduler.

## Loader freeze evidence

The rsync terminal stopped updating on an Apollo 13 progress line, but process
inspection showed that rsync had subsequently moved to an Avatar file. A
12-second sample then proved a genuine write-side stall:

- all three rsync processes had unchanged byte counters;
- writer PID 2532 was in kernel wait `request_wait_answer` on the Avatar FUSE
  destination;
- Macha FUSE worker LWP 2674 was blocked in
  `FuseFrontend::write()` waiting for its broker future;
- the broker-side write was waiting in `reserve_spool_bytes()`;
- spool occupancy was 8,590,147,750 bytes against a 17,179,869,184-byte limit,
  only 213,158 bytes above the exact 50% throttle threshold;
- publication had read 3,426,222,080 bytes, confirmed 580,124,672 bytes,
  completed one file, and yielded 115 times, but the measured publish rate was
  still zero.

The throttle has a zero-rate dead zone. At or above 50% occupancy,
`reserve_spool_bytes()` admits no bytes when
`spool_publish_rate_bytes_per_second == 0`. It then waits without a timer for a
spool-admission revision. The rate is learned only from whole-file retirement,
and partial resumable publication neither releases spool accounting nor
updates the rate. A large-file ingest can consequently stop for whole-file
publication intervals even though publication is making steady partial
progress. In this run, the first retirement left occupancy barely above 50%
and did not yield a usable rate sample, so the writer remained asleep until a
later full-file retirement.

## Required corrections and proof

1. Extend viewer/loader arbitration to distributed reads and every shared
   physical resource: byte credits, per-peer sends, local-store reads and
   outstanding remote extents. Bound the maximum loader bytes already admitted
   ahead of a newly runnable viewer; retain the configured non-zero loader
   share and work-conserving borrowing.
2. Remove the zero-rate throttle dead zone. Crossing the soft threshold before
   a trustworthy drain sample must use a bounded bootstrap admission policy,
   not an indefinite condition-variable wait.
3. Feed admission with continuous confirmed/retirable publication progress, or
   otherwise make safe incremental spool-range retirement available. Do not
   require whole-file completion merely to establish forward progress.
4. Add deterministic tests for threshold crossing with no prior rate sample,
   a first retirement that remains just above the threshold, large partial
   publication with no whole-file retirement, and viewer arrival during
   saturated FUSE loader reads.
5. Repeat this exact three-node loaded UAT. Pass requires prompt playback start
   and seek, bounded loader progress rather than a freeze, responsive control
   RPC, and no loss of durability or atomic visibility.

The first correction is now implemented locally for item 2: successfully
drained partial publication grants bounded progress credits before the first
whole-file retirement. Verification is recorded in
[the bootstrap-admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md).
The loaded deployment UAT remains failed until the new binary is deployed and
the viewer/resource-arbitration correction is complete.

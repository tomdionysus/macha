# FUSE spool rate backpressure

Date: 2026-08-30

## Outcome

FUSE write admission now treats `fuse.max_spool_bytes` as a bounded durable
backlog budget rather than an `ENOSPC` threshold. The default remains 16 GiB;
the YAML value is parsed as a byte size and takes effect when the server starts.
`fuse.spool_reserve_free` remains an independent physical-filesystem safety
floor.

## Admission policy

- Below 50% occupancy, writes may burst at local spool speed.
- Crossing the pressure threshold, or waiting for a request which cannot fit,
  raises publication demand for every durable dirty inode. Publication can
  therefore begin while the source file is still open.
- Completed end-to-end data publications update an exponential moving average
  of sustainable bytes per second.
- From 50% through 90% occupancy, exact admission deadlines progressively move
  from at most eight times the measured rate to the measured rate itself.
- At the hard bound, writers wait for publication/retirement notifications.
  There is no polling loop and no logical-capacity `ENOSPC`.
- A single write larger than the entire configured bound is impossible to
  admit and returns `EFBIG`. Exhausting the physical free-space reserve remains
  `ENOSPC` so backpressure cannot consume the host filesystem.
- Shutdown explicitly wakes blocked admissions with `EINTR`.

Admission waits run only on FUSE write broker workers. They do not occupy RPC,
metadata, telemetry, or other control-plane execution threads. Mutating FUSE
requests retain their existing exactly-once result rule once execution begins.

## Operational diagnostics

`GET /api/v1/status` exposes these O(1) fields beneath
`diagnostics.filesystem`:

- `spool_bytes`;
- `spool_limit_bytes`;
- `spool_publish_rate_bytes_per_second`;
- `spool_throttle_waits`; and
- `spool_throttle_wait_ms`.

## Verification

The capacity regression uses a 384 KiB spool. A second write which cannot fit
blocks, raises publication demand for a first prefix below the ordinary
pressure threshold, wakes after retirement, and succeeds without `ENOSPC`.
Assertions prove occupancy never exceeds the configured limit, a publication
rate is learned, and a throttle wait is recorded.

The stalled-publisher regression holds playback/publication quiet for 30
seconds, proves a saturated writer remains blocked rather than failing, and
proves frontend shutdown wakes it with `EINTR` without a polling owner.

Verification results:

- filesystem/FUSE group: 45/45 passed;
- complete backend suite: 211/211 passed; and
- runtime dependency suite: 3/3 passed.

## Remaining UAT and follow-up

A mounted-FUSE rsync UAT is useful now. Observe the five Status fields while
copying enough data to cross 50% and approach 90% of a deliberately reduced
test limit. Confirm the initial burst, convergence toward actual publication
speed, bounded occupancy, and clean speed recovery after drain.

Concurrent-writer fairness and restart while already above the throttle
threshold remain explicit follow-up tasks in `ACTIVE.md`.

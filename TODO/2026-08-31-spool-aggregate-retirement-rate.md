# Aggregate spool retirement-rate correction

Date: 2026-08-31

Status: implementation and local verification complete; short deployed UAT
recommended.

## Problem reproduced

The Phase 4A UAT had several concurrent file publishers making 19.5--40.2 MiB/s
aggregate useful progress, but `spool_publish_rate_bytes_per_second` settled at
5.95 MB/s. The old estimator calculated `file bytes / that file's active time`
for every completed generation and fed those per-file samples into an EMA.
Parallel 6 MB/s publishers therefore continued to report approximately 6 MB/s
instead of their aggregate retirement capacity.

At high spool occupancy this underestimates sustainable drain and paces rsync
below the capacity which the concurrent publishers can actually reclaim.

## Correction

- All concurrent publishers now share one cumulative retirement epoch.
- The epoch begins when the first publication generation starts.
- Only successful physical spool retirement adds bytes to the numerator.
  Provisional extent progress, metadata acceptance alone, and rolled-back write
  reservations cannot manufacture capacity.
- Every retirement divides cumulative retired bytes by the shared monotonic
  wall-clock interval. Concurrent completions therefore add rates naturally.
- Falling below the 50% burst threshold resets the internal epoch for the next
  pressured run. The last measured rate remains available so admission does not
  forget a useful prior observation before the next completion.
- Existing event-driven admission deadlines, pressure hysteresis, hard byte
  bound, physical free-space reserve and viewer priority are unchanged. No
  timer, polling loop or idle worker was added.

Status retains `spool_publish_rate_bytes_per_second` and now also exposes:

- `spool_publish_rate_window_bytes`; and
- `spool_publish_rate_window_ms`.

These make the aggregate calculation directly auditable during UAT.

## Deterministic proof

`test_spool_retirement_rate_is_aggregate` uses an injected monotonic timeline:
one 100-byte completion at ten seconds reports 10 B/s; a concurrent second
completion at the same timestamp reports 20 B/s over the same denominator.
Later bytes remain cumulative, and an explicit reset begins a clean epoch.

The existing spool-capacity regression now also proves that a successful
retirement populates the rate-window numerator/time and wakes blocked admission.
The stalled-publisher test continues to prove event-driven blocking and shutdown
cancellation without `ENOSPC` or polling.

## Verification

- Build passed.
- New aggregate-rate regression passed.
- Filesystem/FUSE suite: 50/50 passed.
- Foundations suite: 15/15 passed.
- Runtime dependency suite: 3/3 passed.
- Complete default suite: 218/218 passed, including
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` and
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`.

## UAT checkpoint

Deploy node 50, restart/continue the rsync, and observe at least two physical
spool retirements while occupancy remains above 50%. Verify:

1. window bytes are the sum of retired concurrent generations;
2. reported rate equals window bytes divided by window milliseconds;
3. admission accelerates relative to the former 5.95 MB/s estimate without
   exceeding the hard spool limit;
4. viewer activity still freezes new publication quanta; and
5. control latency, failures and timeouts remain healthy.

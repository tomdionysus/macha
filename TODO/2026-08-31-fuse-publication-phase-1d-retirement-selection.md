# Phase 1D.4 checkpoint: pressure-aware retirement selection

Date: 2026-08-31

Status: implementation and complete verification are green locally; deployment
and loaded UAT remain pending.

## Live UAT trigger

The three-node loaded UAT reproduced the remaining full-spool collapse on node
50 after rsync restarted:

- the spool reached 17,179,764,682 bytes against a 17,179,869,184-byte limit;
- rsync slept under event-driven backpressure rather than receiving `ENOSPC`;
- publication performed 334 bounded quanta and 329 yields, reading
  7,111,061,825 spool bytes, but committed zero publication bytes and retired
  no spool file;
- five transient object-durability failures caused publication retries;
- individual 4 MiB object writes commonly took 0.5--8 seconds;
- the spool contained independently completable closed generations from about
  614--842 MiB alongside generations up to about 3.89 GiB; and
- eight workers advanced multiple files without producing near-term atomic
  completion or admission capacity.

Viewer admission remained protected during this sample: viewer waits and FUSE
request timeouts stayed at zero, and two loaded transcode starts produced first
fragments in 889 and 897 ms. A separate playback cache read took 3,312 ms,
confirming that physical storage latency is still outside the complete priority
bound and remains active Phase 1D.3 work.

Node 51 also exposed a separate shutdown defect during rolling deployment of
the preceding cut: shutdown stopped at `service maintenance joining`, exceeded
systemd's one-minute stop timeout, and was killed before the new process
started. The replacement process joined cleanly, but the stop was not graceful.

## Implementation

Above the existing 50% spool-pressure threshold, closed queued generations are
now ranked by retirement return divided by estimated remaining spool replay
work. A generation that cannot retire its spool because a later generation or
durability admission exists receives no retirement return. Equal-return work is
ordered by least remaining work and then greatest releasable bytes, producing
the nearest useful retirement without changing ordinary FIFO/fair scheduling
below pressure.

The selector:

- considers only closed generations before any open loader;
- preserves the existing one-inode/one-quantum ownership and viewer gate;
- retains finite per-inode progress because each completed candidate leaves the
  pressure queue;
- performs no polling or periodic scan; it runs only at an existing queue
  admission event; and
- exposes `data_retirement_priority_selections`, incremented only when a worker
  actually takes a closed candidate ahead of an earlier closed candidate.

This cut implements retirement selection only. The planned reserved retirement
ticket, resumable transient-failure boundary, per-device/per-peer admission and
durability completion tickets remain active.

## Deterministic proof

`filesystem_fuse/test_fuse_spool_pressure_selects_nearest_retirement` fills a
small spool to its pressure threshold with:

1. an open pathological generation;
2. a large closed generation queued first; and
3. a smaller closed generation queued later.

An additional writer blocks at the threshold. The test proves the smaller
closed generation becomes atomically visible while the earlier large
generation remains unpublished, the blocked writer wakes from real publication
progress, exactly one pressure sweep occurs, and the retirement-selection
counter advances.

## Verification

- Build: passed.
- New focused regression: 1/1 passed.
- Filesystem/FUSE group: 57/57 passed with two requested worker slots.
- Runtime dependencies: 3/3 passed.
- The initial complete controlled suite was **227/228**. The sole failure was
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`.
- Failure-only diagnostics proved the catalogue was already correct: cached,
  known and available generation were 18; the writer-side captured generation
  was 15; and the exact final title and artwork were present. The equality
  assertion incorrectly rejected the newer reconciled generation.
- The same test could also observe convergence totals after zero-grace artwork
  retirement had generated later legitimate metadata events. It now captures
  its bounded two-or-three-run integration proof at the first catalogue-repair
  boundary: the gated owner plus follow-up, optionally one reconciliation-owned
  accepted-metadata edge, and strictly fewer runs than requested epochs. The
  dedicated state-machine test retains exact one-follow-up proof for a pure
  burst.
- The timeout and final title/search/artwork/GC assertions are unchanged, and
  detailed failure diagnostics remain in the test.
- Corrected regression: 10/10 repeated serial runs passed.
- Final controlled complete suite: **228/228 passed**.

No deployment of this retirement-selection cut has been performed.

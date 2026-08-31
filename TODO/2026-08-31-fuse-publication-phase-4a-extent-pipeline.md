# FUSE publication Phase 4A: bounded within-file extent pipeline

Date: 2026-08-31

Status: implementation checkpoint complete; deployed three-node UAT recommended
before this slice is moved to `COMPLETED.md`.

## Purpose

The Phase 1 UAT measured about 7.72 MiB/s while each retained publication
writer synchronously waited for every extent put. This slice overlaps the
independent provisional object puts within one foreground loader generation.
It does not change whole-file visibility, spool authority, viewer priority, or
the final durability/metadata ordering.

## Implementation

- `WriteHandle` gives every asynchronous extent put a private
  `DistributedStore::DurabilityBatch`; completed evidence is merged in extent
  order before the existing generation barrier.
- Only `WriteDurability::publication_generation` opts into the pipeline.
  Ordinary filesystem writes retain their previous synchronous behaviour.
- `fuse.publication_pipeline_bytes` is configurable. When omitted, the
  effective bound is two extents capped at one publication quantum. Explicit
  values must be an extent multiple and may not exceed one quantum or eight
  extents. The documented 4 MiB extent configuration therefore defaults to an
  8 MiB/two-extent pipeline.
- A publication drains every admitted extent before a fairness yield or viewer
  yield releases its global quantum reservation. Viewer arrival can therefore
  encounter only the already-running bounded pipeline, never an unaccounted
  tail of loader work.
- Commit drains the pipeline, performs the existing aggregate durability
  barrier, and only then publishes metadata. A failed or abandoned generation
  joins its bounded tasks and remains invisible; the spool+journal stays the
  replay authority.
- The implementation uses transient asynchronous tasks only while extent I/O
  exists. It adds no idle worker or polling loop. A shared bounded data executor
  remains Phase 4 follow-up work.
- Status now exposes `data_publication_pipeline_limit_bytes` and
  `data_publication_peak_pipeline_extents` for tuning and UAT proof.

## Deterministic tests

`test_publication_extent_pipeline_is_bounded_and_atomic` proves that:

- two extents may be admitted at a two-extent bound;
- admitting later extents first retires older work rather than exceeding the
  bound;
- draining provisional work does not expose a partial manifest; and
- final commit exposes the exact complete bytes.

`test_publication_extent_pipeline_failure_stays_invisible` constrains DATA
capacity so a pipelined generation fails after some provisional work and proves
the namespace file remains at its prior visible size.

Existing fair-quantum and viewer-pre-emption tests run with the effective
pipeline enabled. The status contract test covers both new operational fields.

## Verification

- Build: passed.
- New pipeline tests: 2/2 passed.
- `filesystem_fuse`: 50/50 passed.
- Runtime dependency/configuration suite: 3/3 passed.
- Complete default suite: 216/217 passed. The sole failure was the already
  tracked concurrent-only
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
  final-state timeout. It passed immediately in isolation in 1.030 seconds.
  This is not recorded as a green complete suite.

## UAT checkpoint

Deploy all three nodes with the same extent and pipeline configuration, restart
the rsync import, and sample:

1. useful publication bytes and completed files over a fixed interval;
2. `data_publication_peak_pipeline_extents` (expected at least 2 under load and
   no more than the configured byte limit divided by extent size);
3. loader/control RPC queue and service latency;
4. a real FUSE viewer read during import, confirming no new quanta start inside
   the viewer quiet window;
5. CPU, I/O wait, RSS, spool occupancy, failures and timeouts.

If throughput does not materially improve, the next measurement should split
local pack-lock/write time from remote extent RPC time. That decides whether to
proceed first with Phase 3 local range/journal batching or the Phase 4 shared
data executor and per-peer pipeline.

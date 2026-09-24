# Apply the governing laws to disk I/O

Status: proposed 2026-09-19, from
[loader I/O starves control](2026-09-19-loader-io-starves-control-incident.md).
In progress now, ahead of the standing backlog.

## Why this exists

An ordinary ingest on es-1 drove the node to load 11 and 91% iowait, made
`/api/v1/health` and `/api/v1/catalogue/*` take eight seconds, and the client
timed out. macha's own loader work did it: 34 MB/s write, 9 MB/s read, against
co-tenants doing effectively nothing.

The three governing laws are enforced in memory, in HTTP threads and in RPC
execution. They are not enforced on the disk, which is underneath all three.

## The conceptual error

`DataResourceArbiter` (`src/data_work.hpp:95`) carries the comment *"CONTROL
does not enter this object"*, and `DataWorkContext` (`:20`) says *"CONTROL
deliberately remains outside this pool on its independently reserved
transport/executors."*

That reasoning is sound for every resource the arbiter actually models — queue
slots, worker threads, bytes in flight — and false for the one it does not.
Keeping control out of a pool protects control **only when the pool is the
contended resource.** The disk is contended, is shared by every class, and is
modelled nowhere. Control is not protected by exclusion from the arbiter; it is
merely invisible to it.

Two corollaries worth stating plainly, because both were assumed otherwise:

- **A byte budget is not a latency bound.** `data_inflight_bytes` (128 M) and
  `data_viewer_reserve_bytes` (32 M) bound how many bytes are in flight. A node
  can sit inside both and have a disk queue seconds deep. Adding a
  `data_control_reserve_bytes` would do nothing at all, because control never
  takes a lease — that is the wrong shape and should not be built.
- **A concurrency ceiling is not a throughput bound.**
  `background_concurrency` resolves to `max(1, nproc/2)` — two on this node.
  Two concurrent extent writes are sufficient to saturate the disk.

## The primitive that does not exist

Nothing in the tree measures disk service time. `grep -niE
'service_time|io_latency|write_latency|disk_ms' src/` returns nothing, and the
store path carries no timing at all beyond `DurabilityDomain`'s batch deadline.
Every bound is declared up front; none is derived from what the disk is
actually doing.

This is discipline 1 in a resource it was never applied to: bookkeeping
(byte budgets) is trusted over ground truth (measured completion latency) that
is cheap to re-derive. Build the measurement first; every stage below consumes
it.

## What the harness measured, and how it re-aimed this plan (2026-09-20)

**Stage 1 as first written targeted law 1 and the control lane. That was wrong,
and the harness proved it before a line of it was built.**

Three runs on es-1 with `./run-io-pressure.py`, an idle control and two 8 GB
FUSE ingests driving the real loader path (2,048 durable extent puts, a 4.8 s
durability barrier, iowait 70-86%):

| vantage | lane | disk | p99 under load |
|---|---|---|---|
| `/api/v1/health` on-box | control | none | **1.2-1.6 ms** |
| `/api/v1/health` at haproxy | control | none | **1.0-2.0 ms** |
| a `web.root` asset on-box | **data** | **NVMe** | **3.3 ms** |
| `/api/v1/manage/unmatched` | data | **sdb1** | **7,808 ms, client aborted** |

The web asset is the decisive one: it runs on the **same data lane and the same
workers** as the route that timed out, and differs only in which disk it reads.
`web.root` is on `/dev/nvme0n1p2`; the DATA backend is `/dev/sdb1`. The NVMe
route answered in 3.3 ms while the sdb1 route took 7.8 seconds, in the same
window.

So this is **not** worker starvation, **not** lane contention, and **not** the
reactor — 0.43.0 already closed those, and five days of zero `reactor stall`
lines plus these numbers confirm it. It is contention for one physical device,
between loader writes and interactive reads, with nothing arbitrating.

**Law 1 is already enforced everywhere it can be**, because control touches no
disk. The law that is broken is **law 2**, on the DATA backend.

## Stage 1 — Law 2: an interactive read does not queue behind a bulk write

**Rule: loader and speculative writes yield the DATA backend to reads a person
is waiting for. No other class of work may be the reason a viewer's read
waits.**

- New `src/io_pressure.{hpp,cpp}`: `DiskServiceMonitor`, one per durability
  domain, recording completion latency of local store operations as an EWMA
  plus a p95 over a short rolling window. Lock-free sampling on the completion
  path; no timer thread. This is still the missing primitive and nothing in the
  tree measures it today.
- Instrument `LocalStore`/`StoragePool` completion points to feed it. Two
  `steady_clock` reads and an atomic update, nothing more: if measuring service
  time measurably costs service time, this has failed on its own terms.
- `DataResourceArbiter` takes a `const DiskServiceMonitor*` and gains a pressure
  term. When observed service time on a backend exceeds
  `io_pressure_target_ms`, **loader and speculative admission for that backend**
  is progressively refused even where bytes are free. Viewer and interactive
  classes are never gated by pressure: if the disk is slow, they get all of it.
- The existing `data_viewer_reserve_bytes` stays. It reserves viewer *bytes* at
  admission; this reserves viewer *latency* at the device. They compose, and
  neither substitutes for the other — a viewer holding byte credit still queued
  behind a 17-second write, which is the whole finding.

Config, under `dht:` beside the existing data knobs:

```yaml
dht:
  io_pressure_target_ms: 50      # service time the backend defends
  io_pressure_release_ms: 20     # hysteresis floor; resume below this
  io_pressure_min_background: 1  # never fewer than this many background leases
```

**Acceptance.** Re-run the 8 GB ingest with the harness probing a route that
reads the DATA backend — artwork, a playback segment, or `manage/unmatched`,
all three of which aborted on 2026-09-19. That route's p99 stays under 250 ms
and no request exceeds 1 s, with zero `CD--` terminations, for the whole
ingest. The ingest still completes, at reduced throughput. Control and the
NVMe-backed data route must not regress from the figures in the table above.

**The acceptance probe needs credentials** and the harness does not have them:
`anonymous` holds no roles, so every DATA-backed route it could reach is a 403.
Either a probe account with `media_viewer`, or a signed artwork URL, has to be
supplied before this stage can be proved. The `web.root` probe cannot do it —
being on the other disk is exactly what made it useful here.

## Stage 2 — Law 2: the viewer keeps its service time

**Rule: no other class may be the reason a viewer waits on the disk.**

Today `data_viewer_reserve_bytes` reserves viewer *bytes*. It does not reserve
viewer *latency*, so a viewer holding credit still queues behind a loader's
17-second write. The incident shows this directly: a playback segment and a
session `PATCH` are in the `CD--` list beside the control routes.

- Extend the stage 1 pressure term to gate loader and speculative admission
  against a second, tighter target whenever a viewer is present
  (`waiting_viewers_ > 0` or any viewer lease is live).
- Viewer work itself is never gated by pressure. If the disk is slow, the
  viewer gets all of it.
- The existing byte reserve stays. It solves a different problem — admission
  when the budget is full — and the two compose.

**Acceptance.** With one transcode session and a concurrent ingest, viewer
extent reads hold p95 under a stated bound and `viewer_waits` does not rise
with ingest depth. The bounded-exceptions contract in `docs/streaming.md`
still holds: the only waits a viewer sees are its own frontier and its own
holds.

## Stage 3 — Law 3: the loader is bounded, never stopped

**Rule: loader work yields, and keeps going. "Not yet" must not become
"forever".**

The danger in stages 1 and 2 is the obvious one: a throttle that reaches zero
is a wedge, and a node that never finishes an ingest is a different outage.

- `io_pressure_min_background` is a hard floor. Below it the arbiter admits
  regardless of pressure, so loader progress cannot reach zero however bad the
  disk looks.
- Throttling is admission pacing only. It must never cancel work in flight,
  fail a publication, or interact with the retry/park budgets: a paced
  publication is progressing, not failing, and must not consume
  `publication_retry_max_failures`.
- `DataWorkContext`'s no-progress counter must be re-armed by paced progress,
  so pacing cannot be mistaken for a stall by the existing deadline.

**Acceptance.** A sustained ingest under continuous viewer load completes.
`parked_publications` stays zero throughout and no publication retry budget is
consumed by pacing. A node under maximum pressure still retires spool bytes at
a measurable non-zero rate.

## Stage 4 — Make it legible

Nothing above is trustworthy if an operator cannot see it working.

- `diagnostics.data_resources` gains observed service time (EWMA and p95),
  current pressure state, and cumulative `loader_admissions_paced` /
  `speculative_admissions_paced`.
- One `INFO` line per pressure-state transition, naming the observed and
  target service time. Transition-only, following the
  `metadata availability changed` precedent — never per-decision.
- Document in `docs/configuration.md` beside the existing reserves, and in
  `docs/operations.md` as the law-1 mechanism on disk. The
  [governing laws](../ARCHITECTURE.md#governing-laws) table gains its fourth
  row.

## Ordering and sizing

**Re-ordered 2026-09-20 on the measurements above.** Law 2 first, because it
is the law actually broken and the one the incident's aborts belong to. Law 1
needs nothing: control touches no disk and measured 1.2-1.6 ms p99 through an
ingest at 86% iowait. Law 3's floor ships alongside stage 1 rather than after
it, because a throttle that can reach zero is a different outage and must not
be able to exist even briefly. Diagnostics last, describing whatever the first
two settle on. Stage 1 is the only one that adds a new primitive.

Stage 1 is roughly a day with the reproduction harness, which is most of the
work; 2 and 3 are small once the monitor exists; 4 is an afternoon.

## Out of scope, deliberately

- **P-1.** Each metadata commit shipping the whole serialised namespace is a
  separate structural problem with its own plan. It makes this worse and is
  not caused by it.
- **fi-1's slow `put_metadata_commit`.** Whether fi-1 is slow or es-1 is too
  busy to send is unseparated; do that before treating it as a WAN problem.
- **Co-tenancy on es-1.** Plex and qbittorrent share the disk. They were
  measured and exonerated here, but a node cannot defend a service-time target
  against a neighbour that ignores it. If pressure is observed with macha's own
  I/O low, that is the cause and the remedy is operational, not code.
- **cgroup / ionice.** Real, and a blunter instrument than a closed loop that
  knows which class each operation belongs to.

## Risks

- **A latency target is a policy, not a fact.** 50 ms is a guess until measured
  on the Pi-class nodes and on es-1's disk. Tune before enabling widely, and
  expose it rather than burying it.
- **Feedback loops oscillate.** Hence hysteresis (`release_ms` below `target_ms`)
  and an EWMA rather than an instantaneous sample. A flapping throttle is worse
  than none, and the transition log will show it.
- **The hot-path instrumentation must stay trivial.** If measuring service time
  measurably costs service time, this has failed on its own terms.
- **Throttling hides slow disks.** A node quietly pacing itself to defend
  latency looks healthy while achieving little. Stage 4's counters are what
  stop that becoming invisible, and are not optional.

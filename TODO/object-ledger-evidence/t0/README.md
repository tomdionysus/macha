# T0 evidence: measuring the existing system

Branch `experiment/object-ledger-t0`, version 0.74.0: the code of 0.73.2
with observation added (`src/observation.hpp`, `src/observation.cpp`) and
probes at the sites the plan lists. Nothing a response body carries has
changed; Status `threads` lists the recorder's supervised thread,
`observation`.

## What each probe measures, and where

| series | site | what |
|---|---|---|
| `maintenance.claim_walk.examine_us`, `.examined`, `.missing` | `Service::loop`, `repair_retained` | one claim: `next_retained` plus `has` on its class's store |
| `maintenance.inventory.build_us`, `.live_objects` | `Service::loop`, inventory rebuild | the maintenance inventory, when rebuilt |
| `maintenance.release_horizon.build_us` | `Service::loop`, release horizon | the retention release horizon, when its head changes |
| `maintenance.repair.step_us.{idle,loaded}`, `.bytes.*`, `.push_examined.*`, `.pull_examined.*`, `.retained_repairs` | `Service::loop`, after `repair_step` | a repair step, split by whether a higher class was active as it ended |
| `maintenance.gc.step_us`, `.per_object_ns`, `.examined`, `.reclaimed_bytes` | `Service::loop`, after `gc_step` | a DATA sweep step; `gc.objects` counts objects examined |
| `retention.released.{data,control}`, `retention.pruned.{data,control}`, `catalogue.control_gc.removed`, `maintenance.tombstones.collected` | `Service::loop`, the three destructive gates | release and GC rates |
| `claim.data_barrier_us`, `.ids`, `.failed` | `DistributedStore::retain`'s `report` | the DATA retention barrier a quantum commit waits on |
| `api <METHOD> <route>` | `HttpServer::Impl::run_handler` | handler latency per route (`observation_route_label`), deferred responses excluded |
| `playback.create_us`, `.start_ready_us`, `.update_ready_us`, `.first_fragment_us`, `playback.seek_fastpath.{taken,declined}` | `src/playback/playback.cpp` | session creation, async start and update (seek) to ready, first fragment |
| gauges `rss_bytes`, `*_idle_ms`, `repair_*`, `fuse_*`, `maintenance_wakeups` | `Service::observation_gauges` | sampled at each window's end; `repair_*` and `fuse_*` are cumulative |
| events `start`, `services_ready`, `cluster_stable`, `shutdown`, `backend_offline`, `backend_online` | `Service`, `StoragePool` | startup, recovery after a restart, shutdown, a backend's drop and return |

## Probe cost and microbenchmark baselines

fi-1 (GCC, Release, 4 cores, load average 1-2), five runs
(`bench-fi-1.txt`). This is the baseline the threshold rule uses: worse
means above 110% of the median.

| operation | ns per op, five runs | median |
|---|---|---|
| `LatencyHistogram::record` | 21 21 22 24 24 | 22 |
| counter add | 5 5 6 6 8 | 6 |
| `elapsed_us` (one `Clock::now`) | 38 38 39 39 45 | 39 |
| the whole HTTP probe (label, lookup under the registry mutex, record) | 232 234 234 245 256 | 234 |
| the claim walk, per object (`next_retained` + `has`) | 930 935 950 1007 1039 | 950 |
| `has` on the DATA pool, object present | 892 896 897 922 967 | 897 |
| `has` on the DATA pool, object absent (disk check on a miss) | 4100 4128 4129 4216 4331 | 4129 |

On fi-1 the claim walk probe (two clock reads and a record, about 0.1 us)
is about 10% of an object's in-memory walk cost; the walk is bounded at 16
objects a maintenance step, so it adds about 1.6 us to a step. The HTTP
probe is about 0.23 us against handlers measured in milliseconds.

Laptop, in-suite (`--filter baseline --serial --verbose`, `BENCH` lines), laptop,
Apple Clang, Release, 2026-09-30, under a load average of 110-370 from other
projects' builds (figures are upper bounds):

| operation | ns per op |
|---|---|
| `LatencyHistogram::record` | 30 |
| counter add | 9 |
| `elapsed_us` (one `Clock::now`) | 107 |
| the whole HTTP probe (label, lookup under the registry mutex, record) | 1,045 |
| the claim walk, per object (`next_retained` + `has`) | 4,213 |
| `has` on the DATA pool, object present | 3,795 |
| `has` on the DATA pool, object absent (disk check on a miss) | 20,062 |

The claim walk probe adds two clock reads and a record, about 0.25 us, to a
4.2 us object; the walk is bounded at 16 objects a step. The HTTP probe is
about 1 us against handlers measured in milliseconds.

## Files

- `observation_report.py`: merges a node's windows and prints the
  baselines the spec's kill criteria table takes.
- `mutations.md`: each mutation of the observation component and the tests
  that killed it.

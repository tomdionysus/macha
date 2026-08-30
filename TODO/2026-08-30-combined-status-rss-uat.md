# Combined Status availability and repeated-burst UAT

Date: 2026-08-30

## Scope

This deployment UAT combined two checkpoints:

- verify that missing remote disk telemetry is represented as unavailable,
  rather than as a plausible numeric zero; and
- exercise repeated bounded namespace bursts while observing batching,
  materialization, convergence, queues, CPU, and resident memory.

The three deployed nodes were online and healthy before the workload. The
cluster began at generation 1439 with no queued namespace or metadata work and
no scheduled convergence pass.

## Status availability result

The corrected contract passed on every Status endpoint before and after the
workload:

- each endpoint's self card had live, numeric disk measurements;
- remote cards without telemetry reported `available: false`;
- remote storage `used` and `free` were null rather than zero;
- remote cache capacity, used, and free were null;
- remote `storage_backends_online` was null;
- membership-known storage capacity could remain numeric; and
- cluster storage aggregates with incomplete observations reported
  `available: false`, with null used and free values.

This distinguishes an unavailable observation from a genuine measured zero.

## Workload and batching

Eight rounds were run. Each round created one root directory and 128 child
directories, then removed all 129 directories. The exact fixtures were
`/.macha-uat-rss-round-1-20260830` through
`/.macha-uat-rss-round-8-20260830`.

The workload comprised 2,064 successful POSIX namespace operations. It used 48
metadata publications, advancing the cluster from generation 1439 to 1487:

- 2,064 admitted, batched, published, and confirmed operations;
- 48 publication attempts and 48 successful publication batches;
- 43 operations per publication across the complete workload;
- 7,224 journal records and 3,192 journal durability barriers; and
- zero timeouts, failures, rejected operations, or residual queued bytes.

Each round completed in approximately one to two seconds. The journal totals
match the live-path durability model exactly: 399 barriers and 903 records per
round.

One initial attempt to start round 2 was interrupted before any operation was
admitted. The fixture was confirmed absent and counters unchanged before the
round was rerun normally.

## Materialization result

| Node | Baseline entries | Final entries | Evictions | Baseline reconstructions | Final reconstructions | Baseline applied deltas | Final applied deltas | New requests | New misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| local | 25 | 64 | 9 | 1 | 1 | 23 | 23 | 1,996 | 0 |
| node 50 | 44 | 64 | 28 | 1 | 1 | 42 | 42 | 860 | 0 |
| node 51 | 39 | 64 | 23 | 1 | 1 | 37 | 37 | 1,104 | 0 |

Every new materialization request was a cache hit. No node performed another
chain reconstruction or applied another historical delta. All caches remained
bounded at 64 entries and evicted entries after reaching the limit.

## Convergence and final state

At generation 1487 all nodes agreed, all requested convergence epochs had
completed, scheduling was parked, and all namespace and metadata queues were
empty. Final convergence event/run totals were:

| Node | Events | Runs |
| --- | ---: | ---: |
| local | 154 | 74 |
| node 50 | 155 | 19 |
| node 51 | 156 | 19 |

After a 15-second drain, Status reported approximately 0.260%, 0.142%, and
0.066% CPU. Every exact fixture lookup returned not found. The UAT fixtures
were permanently deleted and are not recoverable.

## RSS measurement defect

The repeated-burst functional result passed, but the RSS-ceiling question is
not closed. `src/telemetry.cpp` implements `resident_bytes()` using
`getrusage(RUSAGE_SELF).ru_maxrss`. That value is the lifetime maximum resident
set size, not current resident memory, despite being exposed as
`runtime.rss_bytes`.

The final reported high-water values were 123,609,088, 158,023,680, and
159,514,624 bytes. A subsequent operating-system sample showed current RSS of
approximately 123.55 MB, 148.00 MB, and 161.51 MB. In particular, node 50's
current value was about 10 MB below the Status value, directly demonstrating
the peak/current mismatch. Sampling times also differed, so these figures are
not a sound plateau series.

Only one round followed the local cache first reaching its 64-entry limit.
Even with a correct current-RSS metric, that would not be enough post-cap data
to claim a stable ceiling.

## Result and continuation boundary

- **Pass:** Status unavailable-versus-zero semantics on all three endpoints.
- **Pass:** bounded publication, exact journal accounting, cache bound and hit
  behaviour, convergence drain, queue drain, fixture removal, and idle CPU.
- **Inconclusive:** current-RSS decay or a stable materialization-cache memory
  ceiling.

Before repeating the memory UAT, correct the Status metric to report current
RSS, or expose separately named current and peak values. Add deterministic
contract coverage, deploy it, then run enough rounds after every node's cache
has reached its bound to measure a genuine post-cap series.

## Aggregation addendum

This UAT also exposed that connected remote nodes remained unavailable on each
endpoint. The null representation was correct for missing data, but missing
steady-state peer telemetry was not the intended API contract: one endpoint
must aggregate all known nodes without client fan-out.

The subsequent correction found that RPC request-ID-zero telemetry
notifications were silently ignored in both receive directions. The fix and
tests are recorded in
[Cluster Status telemetry aggregation correction](2026-08-30-cluster-status-telemetry-aggregation.md).
Its deployment UAT remains active.

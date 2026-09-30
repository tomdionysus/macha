# Phase 3 three-node active-burst UAT

Date: 2026-08-30

## Purpose

Exercise the integrated event-driven convergence work on the deployed three-node cluster and verify:

- bounded metadata publication and convergence during a real namespace burst;
- identical final namespace views on all three nodes;
- responsive HTTP/control service while convergence work is active;
- no RPC connection churn; and
- prompt return to stable idle operation with no delayed maintenance loop.

This was an operational UAT, not a deletion-throughput benchmark. Exact service convergence-run counts are proved by the deterministic integrated tests but are not currently exposed by the status API; the live checks therefore use publication count, generation agreement, namespace agreement, responsiveness, CPU decay, and absence of delayed work as operational evidence.

## Cluster and fixture

- Nodes: `10.44.1.200`, `10.44.1.50`, and `10.44.1.51`.
- Deployed server version reported by each node: `0.20.0`.
- Mounted client path: `/Volumes/machamedia`.
- Disposable namespace: `/.macha-uat-phase3-20260830-2009`.
- Workload: create the fixture root and 80 child directories, then remove the 80 children and root.
- Child operations were paced by 40 ms so notices crossed multiple real generations rather than collapsing into a single synthetic publication.
- The fixture was checked absent before creation and checked absent after cleanup.

## Baseline

All three nodes reported:

- cluster health `healthy`;
- metadata availability `writable`;
- generation `1375`;
- 3/3 metadata replicas and voters online; and
- exactly two canonical RPC peer connections, with zero created/reused connection churn since startup.

Baseline status latency was 1.0 ms locally, 8.4 ms on node 50, and 12.8 ms on node 51. Reported Macha CPU was 0.22%, 0.118%, and 0.065%; an instantaneous local process sample was 0.0%.

## Create burst

The 81 namespace mutations advanced the cluster from generation 1375 to 1395: 20 publications, or 4.05 operations per publication. During the burst:

- all three nodes reported generation 1395 at the same active checkpoint;
- health remained `healthy`, writable, and 3/3;
- status latency was 1.1 ms, 8.0 ms, and 14.3 ms;
- canonical RPC connections remained exactly two with no churn; and
- elevated CPU represented bounded active work and had already disappeared in the simultaneous instantaneous process samples.

After the burst, the management API on every node independently returned the fixture with exactly 80 entries. Browse latency was 30.0 ms, 37.9 ms, and 43.1 ms.

## Delete burst

The symmetric 81-operation delete advanced the cluster from generation 1395 to 1415: again exactly 20 publications and 4.05 operations per publication. At the active checkpoint:

- all three nodes already agreed at generation 1415;
- health remained `healthy`, writable, and 3/3;
- status latency was 1.0 ms, 5.6 ms, and 11.4 ms; and
- canonical RPC connections remained exactly two with no churn.

The uniquely named fixture was absent after the workload completed.

## Idle return and delayed-work check

Three post-cleanup checkpoints, including checkpoints approximately 30 and 60 seconds later, all reported generation 1415 on every node. No delayed publication or maintenance activity appeared.

At the final checkpoint:

| Node | Health | Generation | Replicas | Macha CPU | Status latency | Canonical RPC | Connection churn |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10.44.1.200 | healthy/writable | 1415 | 3/3 | 0.253% | 1.0 ms | 2 | 0 |
| 10.44.1.50 | healthy/writable | 1415 | 3/3 | 0.135% | 5.7 ms | 2 | 0 |
| 10.44.1.51 | healthy/writable | 1415 | 3/3 | 0.067% | 10.7 ms | 2 | 0 |

## Result

Pass.

The live cluster batched both halves identically, converged all three nodes during each burst, exposed identical namespace state, kept control/status service responsive, did not create or recycle peer connections, removed the fixture completely, and returned promptly to stable idle operation without a later maintenance wake.

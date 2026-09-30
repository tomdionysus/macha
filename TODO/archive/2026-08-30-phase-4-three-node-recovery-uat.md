# Phase 4 three-node recovery UAT

Date: 2026-08-30

## Purpose

Exercise the deployed bounded metadata RPC executor and history-transfer pipeline
against a real lagging replica, while checking that control/status service remains
responsive and the cluster returns to event-driven idle.

## Environment and fixture

- Nodes: `10.44.1.200`, `10.44.1.50`, and `10.44.1.51`.
- Baseline generation: 1415.
- Linux nodes 50 and 51 had identical deployed executable hashes. The local hash
  differed as expected for the macOS build.
- Node 51's existing Macha process (PID 64859) was paused with `SIGSTOP` and later
  resumed with `SIGCONT`; it was not terminated or replaced.
- Disposable namespace: `/.macha-uat-phase4-20260830-2046`.
- Workload: create the fixture root plus 64 empty child directories while node 51
  was unavailable, verify all three views after recovery, then remove the exact
  fixture.

## Baseline

All three nodes were healthy, writable, converged at generation 1415, and reported
three metadata replicas online. Each live self record had two canonical RPC
connections and zero created/reused connection churn.

Direct status latency was 2.7 ms locally, 23.7 ms on node 50, and 13.1 ms on node
51. Reported process CPU was 0.207%, 0.130%, and 0.067%.

The previously recorded status-telemetry defect remained visible: remote node
cards intermittently showed unavailable runtime data and zero storage, while each
node's own live record and cluster totals were plausible. This is tracked in
`ACTIVE.md` and is not attributed to Phase 4.

## Offline backlog

Node 51 was confirmed intact in Linux stopped state `Tl`. Membership initially
transitioned through an inconsistent 2/3 then 3/3 observation while the peer was
still frozen, before converging correctly to degraded 2/3. This transient is worth
retaining with the separate status/liveness investigation; it did not alter the
metadata write-floor result.

The 65 namespace operations were admitted through the mounted FUSE filesystem in
126 ms. The surviving pair remained writable and converged from generation 1415
to 1418: three publications for 65 operations. With node 51 correctly classified
offline, direct status latency was 2.4 ms locally and 8.0 ms on node 50. The local
fixture contained exactly 64 children.

## Resume and recovery

Node 51 resumed as the same process. Samples begun concurrently with the remote
`SIGCONT` naturally timed out while the process was still stopped. The first
successful node-51 status response was 946 ms; subsequent recovery-boundary
responses were 7-74 ms and no later request timed out.

All three nodes converged at generation 1418, healthy and writable with 3/3
metadata replicas. Every node's management API independently returned the same 64
fixture children. Browse latency was 39.9 ms, 46.8 ms, and 52.7 ms. Canonical RPC
connections returned to two on every node with zero created/reused churn.

Post-recovery direct status latency was 2.3 ms, 7.0 ms, and 12.5 ms.

## Cleanup and delayed idle checkpoint

Removing the 64 children and fixture root was admitted in 217 ms and advanced the
cluster by another three generations, from 1418 to 1421. The fixture was confirmed
absent locally.

Generation remained fixed at 1421 through the delayed checkpoint. All nodes were
healthy, writable, and 3/3 with two canonical RPC connections and no churn.
Reported CPU was 0.204%, 0.145%, and 0.065%. Final direct status latency was 2.5 ms,
7.2 ms, and 9.9 ms.

Instantaneous thread snapshots showed:

- local macOS Macha process and every listed thread at 0.0% CPU;
- node 50: 90/90 Macha threads sleeping, 0 running, process 0.0% CPU;
- node 51: 88/88 Macha threads sleeping, 0 running, process 0.0% CPU.

Final RSS was approximately 116.3 MB, 99.8 MB, and 91.8 MB.

## Result

Pass.

The cluster retained a writable two-replica floor while one peer was unresponsive,
batched the live namespace workload, recovered the lagging history to an identical
namespace on all three nodes, kept survivor status responsive, restored canonical
connections without churn, and returned to a fully sleeping idle state with no
delayed metadata generation. The only anomalous observations were the already
tracked remote status telemetry zeros and a transient membership classification
during failure detection.

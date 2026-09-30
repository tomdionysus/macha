# Phase 1 three-node UAT — 2026-08-30

## Purpose

Observe the three-node cluster after the Phase 1 shared bounded metadata-materialization changes. The specific acceptance question was whether an otherwise idle, converged cluster still spent cores repeatedly reconstructing and encoding historical metadata.

This was an observational UAT only. No tests were run and no application code or cluster state was changed.

## Environment

Observation window: approximately 17:36–17:42 BST on 2026-08-30.

| Node | Address | PID | Version |
| --- | --- | ---: | --- |
| Local Mac | `10.44.1.200` | 95784 | 0.20.0 |
| Linux node 50 | `10.44.1.50` | 35271 | 0.20.0 |
| Linux node 51 | `10.44.1.51` | 40298 | 0.20.0 |

## Result

**Pass for the Phase 1 idle/convergence acceptance question.**

The cluster performed a short, finite startup tail and then became quiet. It did not reproduce the previous sustained near-core saturation, repeated historical snapshot reconstruction, or memory growth seen before Phase 1.

At every successful status sample:

- cluster health was `healthy` with no conditions;
- all three nodes and all three metadata voters were online;
- metadata quorum was validated and writable (two writes required);
- every node agreed on the same metadata generation;
- each node had three active peers and exactly two canonical RPC connections;
- connection counters showed no creation/reuse churn (`created=0`, `reused=0` after startup).

## Timeline and measurements

### Startup tail

The first sample, at roughly 135–140 seconds uptime, showed generation 1219 on every node. Process CPU telemetry was:

| Node | CPU | RSS |
| --- | ---: | ---: |
| `.200` | 0.287% | 96 MB |
| `.50` | 6.235% | 186 MB |
| `.51` | 4.087% | 232 MB |

Linux `ps` lifetime averages were initially higher (`.50` 33.8%, `.51` 23.3%), reflecting work already completed during startup rather than the instantaneous load.

Generation advanced three times, from 1219 to 1222, during the early observation window. It then remained at 1222 through the rest of the UAT.

### Settled state

At roughly 315 seconds uptime:

| Node | CPU telemetry | RSS |
| --- | ---: | ---: |
| `.200` | 0.492% | 97 MB |
| `.50` | 0.491% | 203 MB |
| `.51` | 0.073% | 232 MB |

The Linux lifetime averages had fallen to 12.8% and 8.4%, respectively, as the finite startup cost was amortized. A local two-sample `top` measurement reported 0.0% then 0.2% for Macha.

At roughly 345–350 seconds uptime, generation was still 1222 and CPU telemetry remained low:

| Node | CPU telemetry |
| --- | ---: |
| `.200` | 0.585% |
| `.50` | 0.220% |
| `.51` | 0.077% |

Per-thread `top` snapshots on both Linux nodes reported 100% host CPU idle, zero Macha threads running, and all 89 Macha threads sleeping.

## Stack evidence

A five-second, 1 ms interval sample of the local process captured the system in the desired event-driven state:

- HTTP workers were waiting on their work condition variable;
- RPC fast-control, control, and data workers were waiting on their queues;
- the hydration worker was waiting for an event;
- FUSE broker and durability workers were waiting for queued work;
- the maintenance/service thread was in its interruptible timed wait;
- the RPC health thread was almost entirely waiting, with only its small expected health-check activity.

The sample contained no `MetadataManager::historical_locked`, `encode_snapshot`, `accept_metadata_commit`, or metadata commit stack. This directly contrasts with the pre-Phase 1 profiles in which historical reconstruction and snapshot encoding repeatedly occupied followers.

## RPC/API observations

No RPC timeout, quorum loss, peer churn, or non-canonical connection growth was observed. Status requests that completed did so immediately.

Two scheduled polling batches received simultaneous `connection refused` results from all three status APIs. After the second batch, `lsof` verified that the local process still owned a listening `*:7438` socket, and direct retries succeeded with the same PID, healthy cluster, and stable generation. The synchronized three-host result plus the contradictory live local listener makes this an indeterminate observer/tooling-path anomaly rather than evidence of an application RPC timeout. It is recorded here for completeness and should be investigated only if it can be reproduced independently from the normal shell or client.

## Interpretation

Phase 1 appears to have removed the pathological repeated materialization work in the tested idle, converged state. The residual work was bounded startup/convergence activity, after which CPU usage became negligible and worker threads slept.

This UAT does **not** validate later plan phases such as batching large POSIX mutation streams or throughput under a fresh deletion backlog. Those require a separate deliberately generated workload and latency/operation counters. It does show that the cluster no longer burns three cores merely by being connected and converged.

## Recommended checkpoint

Proceed with the next planned phase. Preserve this idle three-node profile as the regression baseline, and use a separate deletion-burst UAT after batching is implemented to measure operations per publication, generations per burst, RPC latency, and total drain time.

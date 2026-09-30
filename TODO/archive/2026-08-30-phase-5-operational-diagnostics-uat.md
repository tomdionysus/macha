# Phase 5 operational diagnostics UAT

Date: 2026-08-30

## Purpose

Validate the deployed Phase 5 accepted-head persistence and RPC execution
diagnostics during a real three-node lagging-replica workload. The UAT checks
bounded namespace publication, recovery traffic, isolation of metadata queueing
from CONTROL work, convergence, responsiveness, and return to event-driven idle.

This fixture contains directories only. It validates namespace deletion but
does not exercise physical DATA-object GC; that remains a separate active item.

## Environment and fixture

- Nodes: `10.44.1.200`, `10.44.1.50`, and `10.44.1.51`.
- Baseline generation: 1421.
- Local process: PID 7293; node 50: PID 38255; node 51: PID 65360.
- Disposable namespace: `/.macha-uat-phase5-20260830-2113` on the mounted
  `/Volumes/machamedia` filesystem.
- Node 51 was temporarily paused with `SIGSTOP` and resumed as the same process
  with `SIGCONT`.
- Workload: create the fixture plus 256 empty child directories while node 51
  was paused, verify recovery on all nodes, then remove the exact 257-operation
  fixture while all three nodes were active.

The fixture was confirmed absent before creation. Cleanup permanently removed
only this uniquely named UAT namespace; it is not recoverable and was confirmed
absent through every node's management API afterward.

## Settled baseline

All nodes were healthy, writable, at generation 1421, with 3/3 metadata replicas
online, two canonical RPC connections, and zero connection creation/reuse churn.
All metadata queues were empty and rejected-job and accepted-head persistence
failure counters were zero.

Across the baseline window, generation, accepted-head persistence,
reconstruction/delta application, connections, and RSS remained stable. Only
expected `ping` and `members` traffic advanced. Node 50 performed two additional
historical cache-hit lookups without reconstruction or delta application.

The existing remote Status defect reproduced: each node's own live card was
plausible, while remote cards intermittently reported unavailable runtime data
and zero storage/cache values. This UAT therefore uses each node's self card.

## Offline create burst

Node 51 was confirmed in Linux stopped state `Tl`. Creating 257 directories took
1.46 seconds. The surviving write-floor pair advanced from generation 1421 to
1428: seven publications, or 36.7 operations per publication.

Both publishing nodes persisted exactly seven accepted-head updates (896 bytes,
128 bytes per encrypted accepted-head file) with zero failures. Their metadata
queues were empty at the active sample and no work was rejected. Node 50 was
correctly degraded to 2/3 while remaining writable; the local Status view briefly
retained the already-known transient 3/3 classification for the stopped peer.

Both survivor management APIs independently returned all 256 children. Node 50
reported 12.8% process CPU during useful publication work, then fell to 0.2%
within seconds.

## Resume and recovery

Node 51 resumed as the same PID and was converged at generation 1428 in the first
successful three-node sample. Every node independently returned all 256 child
directories.

For the missed seven-generation create burst, node 51:

- imported six additional history entries;
- persisted one final maximal accepted-head state rather than every
  intermediate accepted head;
- performed no new whole-chain reconstruction or delta replay;
- reported no queue rejection or persistence failure.

This is the desired lagging-replica behaviour: transfer sufficient immutable
history, then persist the final accepted authority without repeating every
intermediate head-file replacement.

## Cleanup burst

Removing the exact fixture took 1.19 seconds and advanced generation from 1428
to 1433: five publications, or 51.4 operations per publication. Across create
and cleanup, 514 POSIX operations therefore used 12 metadata generations, or
42.8 operations per publication.

The local node and node 50 each recorded 12 accepted-head writes/1,536 bytes for
the 12 publications. Node 51 recorded six writes/768 bytes: one final recovered
create head plus the five cleanup publications. All failure counters remained
zero.

Node 51 briefly reported generation 1428 immediately after cleanup, then reached
1433 at the next sample. All three management APIs returned `not found` for the
fixture.

## RPC execution evidence

Counter deltas from the settled baseline through final convergence were:

| Node | `put_metadata_commit` | `put_metadata_history_entry` | `accept_metadata_commit` | Accepted-head writes |
| --- | ---: | ---: | ---: | ---: |
| 200 | 0 | 0 | 7 | 12 |
| 50 | 7 | 4 | 27 | 12 |
| 51 | 5 | 6 | 16 | 6 |

Local publication owns its local durable mutation directly, so it is expected
not to appear as an inbound server handler. Repeated acceptance evidence was
idempotent: 50 inbound acceptance handlers across the cluster produced only the
30 head-file replacements required by actual local accepted-head changes.

The largest process-lifetime timings after the UAT were:

| Node | Message | Max queue wait | Max handler time |
| --- | --- | ---: | ---: |
| 200 | acceptance | 0.097 ms | 17.732 ms |
| 50 | commit | 0.138 ms | 14.004 ms |
| 50 | history | 0.178 ms | 10.652 ms |
| 50 | acceptance | 0.056 ms | 22.866 ms |
| 51 | commit | 0.068 ms | 54.972 ms |
| 51 | history | 50.630 ms | 20.523 ms |
| 51 | acceptance | 5.566 ms | 23.238 ms |

The recovering node's speculative/history class accumulated the expected
bounded wait behind its single metadata owner. Its CONTROL-frame queue maximum
was only 2.567 ms during the same recovery, so long metadata work did not occupy
the critical communications lane. No metadata job was rejected and every
sampled pending/active queue returned to zero.

Across the cluster there were 940 additional historical lookups for 514 POSIX
operations, but zero additional reconstructions and zero additional applied
deltas. Those lookups therefore hit already validated shared materializations;
the former repeated whole-chain work did not recur.

## Memory and idle return

| Node | Baseline RSS | Final RSS | Increase | Final reported CPU |
| --- | ---: | ---: | ---: | ---: |
| 200 | 118,161,408 B | 132,472,832 B | 14,311,424 B | 0.286% |
| 50 | 76,038,144 B | 105,316,352 B | 29,278,208 B | 0.135% |
| 51 | 109,379,584 B | 115,376,128 B | 5,996,544 B | 0.063% |

Each materialization cache gained exactly 12 entries and remained below its
64-entry bound with no eviction. RSS and entry counts were stable across the
delayed samples. This single burst does not establish the repeated-burst RSS
ceiling; that active item remains open.

At the final instantaneous process samples:

- local Macha was sleeping at 0.0% CPU;
- node 50 had 88/88 threads sleeping and 0.0% host CPU;
- node 51 had 88/88 threads sleeping and 0.0% host CPU.

Between the final delayed Status samples, generation, accepted-head writes,
commit/history/accept RPC counts, reconstruction/delta counts, cache entries,
queues, and RSS were unchanged. Only expected heartbeat CONTROL traffic moved.
Canonical connections were two on every node with zero churn.

Final direct Status latency was 1.2 ms locally, 11.7 ms on node 50, and 12.7 ms
on node 51. All ordinary sequential probes completed without timeout. One
parallel observer-tool invocation produced simultaneous local and remote
connection refusals while all processes/listeners remained present; fresh
direct probes succeeded immediately. This matches the previously documented
observer-shell artefact and is not treated as a cluster outage.

## Result

Pass for Phase 5 operational diagnostics and lagging-replica recovery.

The workload batched 514 namespace operations into 12 publications, persisted
accepted-head authority exactly when local head state changed, recovered the
lagging replica without intermediate head-file churn or chain replay, kept
metadata queueing away from CONTROL service, produced no rejection/failure/RPC
timeout, converged identical namespace state, and returned all processes to
event-driven idle.

Physical DATA-object GC and repeated-burst RSS ceiling measurements remain
separate active work and are not claimed by this directory-only UAT.

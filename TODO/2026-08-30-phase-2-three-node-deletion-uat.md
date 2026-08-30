# Phase 2 three-node deletion UAT — 2026-08-30

## Purpose

Validate the Phase 2 bounded namespace-publication implementation against the
original failure shape: a live burst of many POSIX namespace operations on one
mounted node, replicated through a three-voter cluster.

The acceptance questions were:

- Does a 1,000-file delete use bounded metadata publications rather than one
  generation per syscall?
- Do foreground namespace admission and cluster status remain responsive?
- Does all replicated work finish and return to event-driven idle?

## Environment

Observation window: approximately 18:50–18:55 BST on 2026-08-30.

| Node | Address | PID | Version |
| --- | --- | ---: | --- |
| Local Mac | `10.44.1.200` | 97726 | 0.20.0 |
| Linux node 50 | `10.44.1.50` | 36110 | 0.20.0 |
| Linux node 51 | `10.44.1.51` | 48234 | 0.20.0 |

The workload used the uniquely named disposable mounted path
`/.macha-uat-phase2-20260830-1851`. Its absence was checked before creation and
after cleanup.

## Result

**Pass for the Phase 2 live deletion-batching acceptance question.**

The decisive delete comprised 1,000 `unlink` operations followed by one
`rmdir`. The shell admitted and completed it in 0.77 seconds. Metadata advanced
from generation 1301 to 1306: five publications for 1,001 ordered namespace
operations, rather than 1,001 publications.

With a 256-operation bound, the theoretical minimum for a completely prefilled
queue is four publications. One extra live publication is consistent with the
event-driven worker immediately starting before the admission burst had fully
entered the queue. This is the intended latency/throughput tradeoff: no polling
or coalescing delay was introduced merely to force every live batch to be full.

The disposable directory was absent after the delete. All three nodes agreed
on generation 1306, retained healthy/writable quorum, and returned to idle.

## Workload timeline

### Baseline

The three nodes were healthy and converged at generation 1293. All had three
active peers and two canonical RPC connections with no connection churn.

| Node | CPU telemetry | RSS |
| --- | ---: | ---: |
| `.200` | 0.481% | 126.8 MB |
| `.50` | 0.121% | 111.1 MB |
| `.51` | 0.064% | 110.0 MB |

### Fixture creation

- The disposable directory produced one generation: 1293 to 1294.
- Admission of 1,000 empty files completed in 1.56 seconds.
- The create burst settled at generation 1301, seven publications for 1,000
  creates.
- Direct status latency after settling was 1.0 ms locally, 8.5 ms on node 50,
  and 29.5 ms on node 51.
- Process samples already showed the local process and all Linux threads
  sleeping.

The create phase was fixture preparation, but it also demonstrated bounded live
batching for independent creates.

### Deletion

- `rm -rf` of the exact verified disposable path completed in 0.77 seconds.
- The 1,001 namespace operations advanced generation by five: 1301 to 1306.
- The exact path no longer existed after completion.
- The first valid post-delete status latencies were 1.2 ms locally, 5.8 ms on
  node 50, and 7.9 ms on node 51.
- CPU telemetry at that point was 0.515%, 0.123%, and 0.065%, respectively.

### Settled observation

Generation stayed fixed at 1306 for the remaining approximately 95-second
observation window.

- Local process samples were normally 0.3–0.5% CPU and sleeping. One bounded
  three-second sample reported 9.6%, after which it immediately returned below
  1%; there was no sustained spin.
- Final Linux thread snapshots showed all 89 Macha threads sleeping on both
  nodes. Node 50 was 97.6% host-idle and node 51 was 100% host-idle.
- Final CPU telemetry was 0.521% on `.200`, 0.134% on `.50`, and 0.068% on `.51`.
- Final direct status latencies were 1.0 ms, 6.2 ms, and 17.3 ms.
- Cluster health remained `healthy`, metadata remained `writable`, and all
  three voters remained online throughout every valid status sample.

No RPC timeout, quorum loss, peer churn, or persistent CPU work was observed.

## Publication interpretation

The public status API does not expose the new in-process FUSE batch counters, so
the UAT used the agreed metadata generation before and after a uniquely isolated
workload. No unrelated generation movement occurred during the settled window.
The five-generation deletion delta is therefore strong live evidence of bounded
publication, while the deterministic tests remain the exact counter-level
proof.

## Memory observation

Final reported RSS was approximately:

| Node | Final RSS | Increase from baseline |
| --- | ---: | ---: |
| `.200` | 154.9 MB | 28.0 MB |
| `.50` | 146.2 MB | 35.1 MB |
| `.51` | 170.1 MB | 60.1 MB |

This did not continue growing during the short settled window and did not
coincide with CPU work. A plausible contributor is the bounded immutable
metadata materialization cache retaining several snapshots that briefly
contained 1,000 additional entries. That attribution was not proven by this
UAT. It is not a failure at this scale, but RSS should be compared before and
after a larger or repeated burst before declaring the memory side fully closed.

## Observer/tooling caveat

One attempted high-frequency status loop wrapped `curl` inside an unauthorised
sandboxed shell and every network call was rejected immediately. The same
observer restriction rejected multi-sample SSH commands. These results are not
application RPC failures: direct authorised calls immediately before and after
the loop succeeded with millisecond latency, and process/listener health stayed
intact. Only direct successful status measurements are used above.

## Recommendation

Proceed with the next plan phase. The original deletion amplification and
three-core sustained-spin symptom did not reproduce; live deletion was fast,
publication count was bounded, RPC status stayed responsive, and the cluster
returned to event-driven idle.

Before making general mixed create/rename/unlink batches, complete the remaining
Phase 2 crash-boundary and concurrent-admission tests. A larger repeated UAT is
optional for throughput, but useful specifically for establishing a steady RSS
ceiling.


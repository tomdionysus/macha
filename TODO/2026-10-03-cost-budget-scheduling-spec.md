# Cost-budgeted scheduling: replacing slot counts and presence gates

Proposal, 2026-10-03. Nothing here is built. It follows the object ledger
experiment (operator: "we'll finish this experiment first") and is itself an
experiment, judged by kill criteria (the last section but two).

Line references are to the working tree at 0.82.0. The survey behind
"What exists today" was made by reading the code; where something was not
established it says so.

## Purpose

Macha decides who may use a disk, a CPU or a link by counting things and by
asking who is around. A node admits background DATA work by a slot count
(`maintenance.background_concurrency`), paces publication and repair by
fixed 95:5 duty cycles, and stops maintenance whenever any class moved a
byte in the last two seconds. None of these looks at the resource. The
results are the ones the operator named (2026-10-03):

- an idle node that does little, until somebody raises a knob;
- loader work derated when nothing is contending with it;
- one viewer using a fraction of one node stopping loading and maintenance
  outright.

Each past incident of this kind was fixed with another mechanism or another
knob (the incident table below has sixteen). This proposal replaces the
family with one model: **work is charged in the resource it uses, contention
is measured, and a lower class runs whenever the measured cost to the class
above it is within a stated bound.**

## The laws this serves

From `docs/principles-and-laws.md`; the design is an implementation of
these and adds no principle of its own.

- **Law 1**: control has "independently reserved admission and execution
  capacity which lower classes never occupy ... a floor, not a share".
- **Law 2**: no viewer-visible delay from lower classes, and "every
  per-viewer resource needs a per-account bound as well as a component-wide
  one".
- **Law 3**: the loader "must use the available spool, storage, network,
  CPU, and publication capacity. It may be paced for hard capacity,
  durability, bounded-memory, fairness, or genuine downstream throughput
  limits, but **not by an artificial quiet period** or the mere existence
  of another open writer."
- **Law 4** and **discipline 5**: no budget that cannot admit one unit of
  its own work; no loop that will not finish.
- **Priority, not polling; priority, not exclusion**: "Capacity is
  work-conserving where safe, but a lower class may have only a bounded
  amount of non-pre-emptible work outstanding when a higher class arrives.
  The non-zero loader share must still prove continued progress under
  sustained viewing."
- **Work is bounded and event-driven**: "Polling, artificial quiet periods
  and unbounded hidden work are not substitutes for explicit ownership."
- **The end-to-end invariant**: priority accompanies a request through
  "server admission -> executor -> lock -> buffer/byte credit -> CPU work
  -> physical I/O -> RPC", and a lower class "may not hold a shared lock,
  executor slot, buffer reservation, network allowance or media resource
  while waiting for slow work".

The sentence in bold is the centre of it. A quiet window is what most of
today's pacing is made of.

## What exists today

### Budgets, their units, and what each stands in for

| budget | unit | real resource it stands in for | where |
|---|---|---|---|
| DATA in-flight capacity and viewer reserve | bytes of leases, mostly one `extent_size` per read whatever the object's size | concurrent disk operations and buffers; does not bound device time | `data_work.hpp:217-251` |
| `background_concurrency` | count of loader + speculative leases (default half the cores) | CPU (hash, seal) and disk | `data_work.hpp:241`, `node_resources.cpp:12-14` |
| device pressure gate | actual / expected service time, EWMA; expected = 25 ms + 120 ms/MiB, fixed | device time; the timed interval also includes hashing, sealing and lock waits | `io_pressure.hpp:74-117` |
| retained memory ledger | bytes of heap, with control, viewer, loader and reassembly reserves | RAM (a space, not a rate) | `retained_memory.hpp:190-231` |
| publication and repair duty cycles | wall time, 95:5, 25 ms slice | nothing measured | `fuse_frontend.hpp:68-197`, `maintenance.cpp:230` |
| maintenance credits | bytes per second derived from a network-rate estimate, also spent on local disk work | link bandwidth | `maintenance.cpp:339-369` |
| RPC worker pools | 2 fast-control, 2 control, 1 metadata, 8 data (2 reserved for foreground); compile-time | CPU and blocking I/O per class | `net.cpp:39-45` |
| RPC queues | bytes and job counts per queue; per-connection outbound 256 messages / 128 MiB shared by every class | memory, queueing latency | `net.hpp:209-215`, `net.cpp:46-50` |
| HTTP lanes | 16 data workers, 2 control workers, 256 queued per lane | handler time | `http.cpp:1438-1441` |
| playback | `max_sessions` 64, per account 32; `max_video_transcodes` 1, audio 4; 64 MiB reserved per pipeline | CPU and memory per viewer | `playback.cpp:1831-1900` |
| FUSE publication | `commit_workers` 8, quantum 32 MiB, in-flight 256 MiB, open-writer cap | loader CPU, disk, memory | `fuse_frontend.cpp:3628-3636` |

### Where a lower class stops because a higher class is merely present

1. A waiting viewer refuses all loader and speculative DATA leases
   (`data_work.hpp:225-226`); a waiting loader refuses all speculative
   ones, whatever the loader is waiting for (`:248-249`).
2. Under device pressure a loader is refused if a viewer is "present",
   which includes any foreground or read-ahead byte in the last
   `foreground_quiet` (2 s) (`data_work.hpp:233-235`). That clock is also
   moved by a peer's foreground request served here
   (`storage_server.cpp:185, 219`), by catalogue artwork and index reads
   issued at foreground (`catalogue.cpp:1007, 1628`), and by a zero-byte
   note at the start of every foreground read (`distributed_store.cpp:1707`).
3. Retained memory: any control or viewer waiter refuses all loader and
   speculative memory; any loader waiter refuses speculative
   (`retained_memory.hpp:201-202, 213-214`).
4. FUSE publication drops to a fixed 95:5 duty cycle (a cooldown nineteen
   times the turn) whenever the foreground clock moved in the last
   `publication_quiet` (5 s) (`fuse_frontend.cpp:3236-3273`).
5. The maintenance pass is `busy` when any of foreground, read-ahead **or
   loader** moved a byte this pass or within 2 s. Busy shuts tombstones,
   GC, rebalance, compaction, history checkpoint and scrub, and sets the
   rebalance and scrub rate to `busy_bandwidth_fraction` (0)
   (`maintenance.cpp:316-337, 491-492, 942-1010`).
6. Repair runs on a 95:5 duty cycle keyed on the same recency, or on any
   peer reporting a non-zero viewer byte rate in gossip within 30 s
   (`maintenance.cpp:334-336, 482, 605-620`; `cluster.cpp:679-691`).
7. GC is pushed two seconds out by every metadata event, node event and
   inventory rebuild (`maintenance.cpp:279, 287, 555`).

Outside the arbiters **no mechanism reacts to measured contention between
classes**: every cross-class signal is presence or recency.

### What is limited by a fixed number whatever the load

`background_concurrency`; the non-lendable 32 MiB DATA viewer reserve; the
fixed read charge of one extent; the RPC and HTTP worker counts; repair's
16 operations and 8 sends per step; 64 objects per GC, rebalance and scrub
step; `commit_workers`; `max_video_transcodes`; the 25 ms slice; the 2 s
and 5 s windows; a 30-day scrub at 2% of a rate; polling slices of 200 ms,
500 ms and 1 s in three admission loops.

### What is already measured

- DATA device service time per pool: EWMA, worst, slowdown ratio
  (`io_pressure.hpp`). The only measurement that drives admission, and
  only to refuse.
- RPC server queue wait and handler time per class and message type
  (`net.cpp:4377-4438`), diagnostic only.
- Per-class traffic bytes per second, per-peer control round trip
  (`net.cpp:1966-1973`), process CPU, load, RSS (`telemetry.cpp:513-556`).
- Per-thread CPU for the RPC pools (`ThreadCpuReporter`), log only.
- FUSE spool retirement rate (drives spool admission; the one place a
  measured rate already paces work, `fuse_frontend.cpp:1147-1168`).
- Transcode production rate against real time, per session and as medians
  per codec and height (`TranscodeRateBook`), reporting only.
- The maintenance pass's own CPU share and the catalogue scanner's
  (`cpu_target`).

Not measured at all: state-device I/O time; CPU, bytes or I/O time per
viewer, per account or per class.

### The incidents this must not bring back

| date | what happened | what was added |
|---|---|---|
| 08-31 | publication gated on an exclusive playback quiet window | the 95:5 weighted loader |
| 09-09 | publication livelock: 123 memory leases held by writers that were not running | reassembly reserve, no-progress deadline, open-writer cap |
| 09-19 | one ingest drove a disk to 91% iowait; viewers aborted. "No available setting would have prevented this." | the device pressure gate |
| 09-22 | maintenance outranked a 36 GB import (read 51.6 MB/s while the import got 2) | a loader clock in `busy` |
| 09-23 | torrent writes tripped pressure; the single remaining slot was held across a peer call on both nodes; 120 s abandons | a torrent disk backend at loader class |
| 09-25 | repair switched off whenever a node ingested; 0.9 TB short when es-1 left | the repair duty cycle |
| 09-28 | repair moved almost nothing; first fix *held* repair while a peer served viewers, rejected ("pace, never gate") | peer viewers feed the pacer |
| 10-01 | repair completes no pass: one to three months at the measured rate | open (`repair_weight` 5 to 20?) |
| 10-03 | two nodes writing at once deadlock on each other's slots | leases only for local work (0.82.0) |

The pattern: a presence gate starves something, a counter-gate or a knob is
added, and the next workload finds the next gap. Three structural faults
recur:

1. **The budgets are not denominated in the resource.** Bytes in flight and
   slot counts stand in for device time; a duty cycle stands in for
   nothing.
2. **Presence stands in for contention.** A viewer reading 2 MB/s from a
   disk that can do 80 is treated as a viewer who needs the disk.
3. **Each mechanism is local.** Priority is re-derived at each stage by a
   different rule (the transport treats read-ahead as background, the
   arbiters as viewer), so the end-to-end invariant holds only where
   somebody has checked.

## The model

### Principles

- **R1. A budget is denominated in the resource it protects.** Device time
  on a named device, CPU time, bytes on a named link, bytes of memory.
- **R2. Contention is measured, never inferred.** A class is slowed only
  by evidence that it is costing the class above it.
- **R3. A lease is held only while consuming what it names.** Never across
  a wait on another node, another device or a lock (the invariant; the
  2026-09-23 and 2026-10-03 deadlocks were both this).
- **R4. Every class always progresses.** Each has a floor of at least one
  unit of its own work on each resource (discipline 5, law 3's last
  sentence).
- **R5. Work-conserving.** Entitlement a class is not using is used by
  whoever has work; nothing is held idle for someone who might arrive.
- **R6. One mechanism everywhere.** As with locks: if it works it works
  everywhere, and it can be tested once.
- **R7. Every refusal has a reason a person can read.** ACTIVE's "say why a
  node counts itself busy" becomes a property of the mechanism.

### Resources

A node has a small, enumerable set, found at startup and on reconfigure:

- each **device**: every distinct `st_dev` behind a DATA backend, and the
  state device (control store, FUSE spool and journal, ingest staging,
  playback temp), which today is unmeasured and may be the same disk;
- **CPU**: the cores;
- each **link**: one per peer;
- **memory**: the retained-memory ledger, which is a space and stays a
  ledger (below).

### Cost

Work is charged in resource time: milliseconds of a device, milliseconds
of CPU, bytes on a link. Two numbers per operation:

- an **estimate** at admission, from a model learned per device and
  operation kind (read, write, validate, remove; by size). It replaces the
  fixed `25 ms + 120 ms/MiB`, which is right for no device in particular;
- the **actual** at completion, measured around the device call alone
  (today's timer also covers hashing, sealing and lock waits, which belong
  to CPU and to nothing). The actual corrects the model.

A device's **baseline** is what an operation costs when nothing contends:
the low end of recent actuals for that kind and size. **Inflation** is
actual over baseline. A slow USB disk has a slow baseline and is not, by
that alone, congested.

### The bound that replaces the slot count

The laws already say what the bound is: *a lower class may have only a
bounded amount of non-pre-emptible work outstanding when a higher class
arrives.* Userspace cannot pre-empt an I/O it has issued, so what a viewer
arriving at a device waits behind is exactly the lower-class work
outstanding there. So:

> On each device, lower classes may have outstanding at most **B
> milliseconds of estimated device time**.

- With no viewer, `B` outstanding keeps the device continuously busy: the
  device is the bottleneck, not the count. An idle node does as much as its
  disks can do, on any number of cores, with no knob.
- A viewer's first operation waits at most about `B`. That is the
  viewer-visible cost of background work, stated in the unit the viewer
  feels.
- The same bound per link (bytes outstanding in a peer's outbound queue
  below viewer class) and for CPU (lower-class CPU work outstanding).

`B` is not a constant. It is controlled.

### The controller

Per resource, one small controller sets `B` for the classes below viewer:

- **Signal**: the inflation of *viewer* operations on that resource, over
  a short window. For a device, viewer read service time against
  baseline. For CPU, a transcode's production rate against real time (it
  must stay above 1.0 with margin; already measured). For a link, the wait
  of viewer-class fragments in the outbound queue.
- **Rule**: while viewer inflation is within the operator's target, `B`
  grows additively up to a ceiling (the latency a viewer may be made to
  wait, the one number configured). When it exceeds the target, `B` shrinks
  multiplicatively. With no viewer operations there is no signal, and `B`
  sits at the ceiling.
- **Floor**: `B` never falls below one operation's estimate (R4). A node
  under sustained viewing still publishes and repairs, slowly, and says so.
- **Damping**: the signal is a windowed quantile, not a mean; changes to
  `B` are rate-limited; a single outlier does not move it. (Today's gate
  trips on one operation ten times over expected and latches until the
  average falls below 150%.)

This is the difference from today in one line: a viewer using a quarter of
a node inflates nothing, so nothing is taken from the loader.

### Classes below viewer share by weight, without starvation

Loader and speculative share `B` by weight (one configured ratio, default
favouring the loader). The share is deficit-based over resource time: a
class with no work lends its share (R5), and a class that has been short is
repaid. No class waits because another is merely waiting; today a loader
blocked on anything at all shuts out all speculative work, and the reverse
gap let maintenance outrank an import.

### Viewers, and one viewer against another

Viewer work is admitted ahead of everything below it, as now. What is new
is law 2's second clause, which today has only session counts behind it:
viewer consumption is accounted **per account** in resource time on each
resource, and when viewers contend with each other the resource is shared
by deficit round robin across accounts. Segment holds, pipeline memory and
transcode slots become per-account as well as per-node.

### Control

Control keeps a reserved floor that is not a share of anything (law 1):

- its own executors, as now;
- a reserved fraction of every outbound queue (today lower-class traffic
  can fill a connection's queue and a control message is refused,
  `net.cpp:1293-1297`);
- on the **state device**, the same outstanding-work bound, measured,
  with control as the protected class: loader work that lands on the state
  device (spool, journal, staging) is bounded by what it costs control I/O
  there. This is the missing disk-I/O reserve for law 1.

### One lease, one shape

```text
lease = budget.acquire(context, {resource, estimate})   // may wait; never across another wait
...do the work on that resource...
lease.complete(actual)                                   // charges, updates the model
```

- `context` is the work context that already travels in `Budget`
  (object-ledger spec A1): class, account, deadline, cancellation, origin.
- A lease names one resource. Work that uses two takes two, in a fixed
  order, and holds neither while waiting for something else (R3).
- A refusal returns a reason: which resource, which class's bound, what the
  inflation was (R7).
- The same call is what the RPC and HTTP executors, the FUSE publisher,
  the maintenance stages, ingest, torrent and playback use. Their private
  pacers go.

### What the maintenance pass becomes

The pass has no `busy` flag, no quiet windows and no duty cycle. Each stage
asks for budget for its next unit on the resource that unit uses, and runs
it if admitted. Repair's network work is bounded by the link and by the
*receiving* node's device, measured there; it is not slowed because some
peer somewhere has a viewer.

Two delays that look like pacing are not, and stay, named as what they are:
GC's wait after a metadata change is a safety margin for a destructive
decision, and scrub's campaign period is a policy about how often to read
the library.

### Across nodes

A peer's viewer reading from this node is viewer-class work on this node's
device, charged here to that viewer's account. That is all a serving node
needs: the gossip presence flag (`peer_viewers`) and the practice of a
peer's request moving the local viewer clock both go.

### Memory

Memory is a space, and the retained-memory ledger is the right kind of
thing. Three repairs bring it under the same principles:

- waiter gates become reserves: loader and speculative memory is refused
  when the control and viewer reserves would be eaten, not because a viewer
  is waiting for something;
- per-account viewer bounds;
- the shedding path, which no caller uses, is removed or used.

### Playback

Transcode admission is by measured capacity, not `max_video_transcodes`:
the rate book already records what this node sustains per codec and height
at each concurrency. A transcode is admitted if the predicted production
rate, given the CPU already committed, stays above real time with margin.
A Pi that can run one 1080p transcode still runs one; a node that can run
six runs six, without a knob.

## Configuration

What the operator sets:

| key | meaning |
|---|---|
| `scheduling.viewer_delay_ms` | the most a viewer operation may be made to wait behind lower-class work on any one resource: the ceiling on `B` |
| `scheduling.viewer_inflation_percent` | how much slower than baseline viewer operations may run before lower classes are cut back |
| `scheduling.loader_share` | loader against speculative when both have work |
| `scheduling.control_reserve_percent` | control's floor on queues and the state device |
| optional hard caps | a link's bandwidth, a device's rate, for a metered link or a disk the operator wants spared |

What goes: `maintenance.background_concurrency`, `foreground_quiet_ms`,
`busy_bandwidth_fraction`, `idle_bandwidth_fraction`, `foreground_weight`,
`repair_weight`, `cpu_target`, `initial_bandwidth`;
`fuse.publication_quiet_ms`, `viewer_weight`, `loader_weight`;
`dht.io_pressure_*`, `data_viewer_reserve_bytes`; the transcode counts.
`data_inflight_bytes` remains as a memory bound if it is still needed once
buffers are charged to the memory ledger.

A node refuses unknown keys at startup (0.65.0), so each stage that retires
a key edits both nodes' `macha.yaml` in the same deploy.

## Observability

Per resource, in Status diagnostics: the baseline model, current `B`,
viewer inflation, resource time consumed per class and per account over the
window, and the last refusals with their reasons. Two questions that today
need a debugger become a read:

- why is this node not doing background work? (which resource, whose
  bound, what was measured);
- what is this viewer costing? (per account, per resource).

## How each law is held

| law | today | under this design |
|---|---|---|
| 1 | separate executors; no reserve in outbound queues or on the state device | a reserved floor on every resource control uses, measured on the state device |
| 2, first clause | presence gates stop lower classes whether or not the viewer is affected | lower classes are cut back on measured viewer inflation, and a viewer waits at most `B` |
| 2, second clause | session counts only | per-account accounting and fair sharing in resource time |
| 3 | quiet windows and duty cycles hold the loader back with no viewer contending | the loader takes whatever the viewer is not using; nothing waits for an absent class |
| 4, discipline 5 | budgets in bytes chosen when units were smaller; a slot count that two nodes can deadlock | floors of one unit; leases never held across another wait |
| priority, not polling | three admission loops poll on fixed slices | waits are on the budget's own events |
| the invariant | re-derived per stage, inconsistently | one context, one lease shape, at every stage |

## Stages

Each stage is deployable alone and judged before the next starts.

- **C0. Measure.** Device time per class on every device including the
  state device, CPU time per class, queue wait per class, per-account
  viewer consumption; baselines learned and reported. No behaviour changes.
  This fills the baseline the kill criteria need, and may by itself show
  where the time goes.
- **C1. One lease shape.** The existing arbiters behind `acquire` /
  `complete` with today's rules, every caller moved onto it, estimates and
  actuals recorded. Decision traces identical.
- **C2. The device controller.** The outstanding-work bound and controller
  replace the slot count, the waiter gates and the pressure gate for DATA
  devices. The first stage that changes behaviour.
- **C3. The pacers go.** The pass's `busy`, quiet windows and repair duty
  cycle; FUSE's publication duty cycle; maintenance credits derived from a
  network estimate.
- **C4. The state device and control's reserve.**
- **C5. Transport and HTTP.** Executor pools sized by measured cost,
  per-class queue reservations, read-ahead classed consistently.
- **C6. CPU and playback.** Transcode admission by measured capacity;
  per-account viewer bounds.
- **C7. Memory ledger repairs.**

## Kill criteria

The object ledger's K table is the before. A stage is withdrawn if, taken
as an operating system on the cluster:

- viewer start, seek or segment latency (K7) is worse under the same load;
- control latency (K6, health, ping) is worse;
- the controller oscillates: `B` on any resource swings by more than a
  factor of two more than a few times an hour under steady load;
- any class makes no progress for longer than its floor implies.

And it has failed to do its job if these do not improve:

- **idle work**: a repair pass on the live library completes in days, not
  months (K4, today 19 to 21 MB/min);
- **a light viewer**: with one direct-play viewer on a node, loader
  throughput (K8) stays above a stated large fraction of its idle rate;
- **knobs**: the two nodes run the same scheduling configuration with no
  per-node tuning.

## Testing

- The controller and the share accounting are pure functions of (signal,
  state): deterministic primitives, tested exhaustively against a simulated
  device with a stated service model, including a noisy one.
- The lease shape is a contract with a conformance suite; fakes at it let
  every caller's admission behaviour be tested without a disk.
- The incident table is a list of fixtures: each row becomes a scenario the
  design must pass, in the trace harness where it is a pass decision and
  in-process otherwise.
- No test waits on real time: the clock and the device model are injected.

## Risks

- **Baseline drift.** A disk that is always contended has no uncontended
  samples, and its baseline creeps up until congestion looks normal. The
  baseline needs a floor from the device's own best observations and
  possibly a periodic uncontended probe.
- **Noise.** A Pi's USB disk has heavy-tailed latency. The signal must be a
  quantile over enough operations to mean something, and a viewer making
  few reads gives few samples.
- **Oscillation.** Additive-increase, multiplicative-decrease loops ring
  when the delay between a change and its effect is long. Device queues are
  short, so this should be mild; it is the first thing C2 measures.
- **Estimates are wrong before they are learned.** A restart begins with
  no model. It starts conservatively and learns within seconds of real
  work, but the first minute after a restart is the worst-scheduled.
- **Userspace cannot reorder what the kernel has queued.** The bound on
  outstanding work is the whole of the control available; if it is not
  enough on a shared state device, the remaining tools are I/O priority
  classes or separate devices, and that is a deployment matter.
- **CPU is harder to attribute than disk.** Threads are shared and work is
  short; per-class CPU accounting is approximate.
- **Scope.** This touches every admission site in the server. C1 is what
  makes the rest tractable, and is the largest stage.

## Non-goals

- Changing the classes or their order.
- Cluster-wide scheduling: each node schedules its own resources; nodes
  cooperate only by serving each other's requests at their class.
- Placement, replication or repair *policy* (what to move); only when it
  may be moved.

## Open questions for the operator

1. **The viewer bound.** Is "a viewer operation waits at most N ms behind
   background work" the right thing to configure, and what is N: 100, 250?
2. **Sustained viewing.** Law 3 says the loader share is non-zero under
   sustained viewing. Is one unit at a time the floor, or a stated minimum
   share?
3. **Hard caps.** Do link and device caps stay as operator settings, or is
   everything measured?
4. **Per-account fairness.** Equal shares between accounts, or weighted?
5. **The state device.** Is sharing a physical disk between state and DATA
   a supported configuration to be scheduled, or one to warn about?
6. **Order.** C2 (the disk) first gives the most; is CPU and playback (C6)
   wanted sooner because of transcodes?

## Decision log

- **2026-10-03.** Proposed after the 0.81.0 measurement run found two
  nodes deadlocked on each other's background slots. Operator: "would a
  better idea, given these threads spend a lot of time IO waiting, not be
  to have some manner of abstract configurable budget? The 'number of
  slots' is a bit of a blunt instrument ... having to tinker with knobs to
  get maintenance to do useful work even though the nodes are idle, or
  derating loader work when again, idle, and a few problems we've had to
  sort involving a single viewer who isn't using even 1/4 of one node
  clobbering loading and maintenance completely." Then: "Write a full
  document on rewriting the balancing system for credit with the laws in
  docs, and we'll finish this experiment first."

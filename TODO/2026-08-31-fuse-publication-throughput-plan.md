# FUSE publication throughput plan

Date: 2026-08-31

Status: Phase 4A and the aggregate retirement-rate implementation are locally
complete. Phase 1C corrected FUSE classification and exclusive viewer gating,
but deployed UAT proved that priority can still be bypassed inside synchronous
materialisation, rebuild, commit and physical I/O. Phase 1D below is now the
mandatory invariant cut before Phase 1C UAT or later throughput work continues.

The 2026-08-31 loaded Phase 1D UAT subsequently proved two remaining failures:
the viewer/loader distinction still does not arbitrate already-admitted FUSE
distributed reads at disk/transfer granularity, and spool admission can wait
indefinitely at the 50% soft threshold while its publication-rate estimate is
still zero. The evidence and required regression matrix are recorded in
[the loaded UAT failure](2026-08-31-phase-1d-loaded-uat-failure.md). These are
mandatory Phase 1D corrections, not later throughput refinements.

The spool zero-rate dead zone now has a locally verified event-driven
correction: drained bounded publication work grants capped bootstrap admission
credit until the first physical retirement supplies the ordinary pacing rate.
See
[the progress-bootstrap checkpoint](2026-08-31-spool-progress-bootstrap-admission.md).
Its deployment UAT remains pending. The same regression exposed zero-byte
publication setup being charged as weighted loader service and amplified into a
long proportional cooldown; that accounting belongs to the still-active
resource-arbitration correction.

## Scheduling laws

1. **Thou Shalt Not Make Control Wait.** Cluster membership, health, metadata
   coordination, cancellation, shutdown and the bounded control work needed to
   admit viewer operations must never queue behind or execute inline with bulk
   data work. Control has independently reserved admission and execution
   capacity which lower classes never occupy. Shared physical capacity may be
   used work-conservingly only while an independent control submission credit
   and a bounded completion path remain available.
2. **Thou Shalt Not Make The Viewer Wait.** Playback startup, reads, seeks, and
   the control work required to serve them have overwhelming priority. No ingest
   throughput improvement is acceptable if it introduces viewer-visible delay,
   buffering, starvation, or latency spikes.
3. **Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
   Viewer Wait.** In the absence of viewer contention, ingest must use the
   available spool, storage, network, CPU, and publication capacity. It may be
   paced for hard capacity, durability, bounded-memory, fairness, or genuine
   downstream throughput limits, but not by an artificial quiet period or the
   mere existence of another open writer.

These laws define priority, not polling. Viewer demand, resource availability,
durability completion, queue transitions, and pressure thresholds must wake or
pace work through events.

They also define priority, not exclusion. The strict order is
`control > viewer >> loader > speculative`. The hard control admission/executor
reservation is outside the viewer/loader ratio and is never borrowed. While
viewer and loader work are both runnable, their
default service ratio is 95:5. The weights are configurable and work conserving:
any class may borrow unused capacity, but a lower class may have only a bounded
amount of non-pre-emptible work outstanding when a higher class arrives.
Bounded quanta limit that delay, while the non-zero loader share proves
continued progress under sustained viewing.

### Non-bypassable end-to-end invariant

Classification at the API, FUSE or RPC boundary is necessary but insufficient.
Priority must accompany a request through every resource it can wait for or
occupy:

```text
admission -> executor -> lock -> buffer/byte credit -> CPU/hash work
          -> physical I/O -> RPC -> durability barrier -> metadata commit
```

No function called from a bounded quantum may hide an unbounded subordinate
operation. In particular, `materialize()`, `rebuild()`, object hashing, extent
puts, durability waits and metadata publication must be resumable at bounded
byte/time checkpoints. A lower-priority operation may not hold a shared lock,
executor slot, buffer reservation, disk queue allowance or communications
thread while waiting for slow I/O.

Application scheduling must bound physical I/O before submission. Kernel I/O
already issued cannot be recalled portably, so the application must reserve an
independent, non-borrowable control admission/executor path and physical
submission credit, retain viewer headroom, cap lower-class outstanding
bytes/operations, and submit the next loader unit only after the scheduler
grants a credit. Non-control pooled capacity is borrowed work-conservingly.

Priority inversion is a correctness failure, not merely a poor benchmark.
Every shared mutex must have a bounded non-waiting critical section; no disk,
network, durability or metadata wait may occur while holding a cross-class or
cross-inode lock. Pure control requests must remain memory-only or enqueue a
bounded data ticket and return; they must never execute bulk work inline.

These are software scheduling invariants, not a promise that failed hardware
has zero latency. If a required peer/device/control store is unavailable, the
higher class must complete from published memory where its contract permits or
fail/degrade within its explicit deadline. It must never wait indefinitely
while Macha continues issuing lower-class work to that resource.

### Priority classes and provenance

Durable spool publication is user-requested loader work, including after a
process restart. Restart changes its provenance and replay requirements; it
does not demote it to background recovery. The schedulers must represent these
as separate dimensions:

1. control traffic, which must remain independently responsive;
2. viewer foreground reads and viewer read-ahead;
3. user-requested loader/ingest publication;
4. speculative repair, hydration, garbage collection, and maintenance.

The FUSE mount is an ingest and convenience interface, not a viewer interface.
All FUSE reads and writes are loader class and must not refresh viewer activity
or create a playback quiet window. Viewer demand is emitted only by the actual
viewer/streaming path. This is an interface contract; do not infer intent from
process names, PIDs, open flags, or per-inode writer state.

The RPC transport therefore needs a loader frame class between viewer
read-ahead and speculative work. FUSE queue items need a recovered-provenance
flag for checksum, cache-admission, and crash-proof behaviour, independently of
their loader scheduling class. `recovery_commit_workers` must not permanently
throttle user spool data merely because it was reconstructed from the journal.

## Outcome

Make sustained bulk import through the FUSE mount limited by useful storage or
network bandwidth, rather than by a single publication slot, repeated demand
work, whole-file completion barriers, or work performed on communications
threads.

The change must retain these invariants:

- a successful FUSE write is recoverable according to the existing local
  durability contract;
- a file becomes authoritative atomically at a complete generation, never as a
  partially published file;
- restart can distinguish locally durable, remotely staged, committed, and
  confirmed work without guessing;
- playback and control RPCs remain responsive under bulk ingest;
- control admission and completion have reserved capacity above viewer work,
  and viewer work has reserved capacity above loader work, at every shared
  executor and physical resource boundary;
- the maximum lower-class work admitted ahead of a newly runnable higher class
  is explicitly byte/time bounded and observable;
- when playback is absent, ingest is work conserving and uses all safe available
  publication capacity;
- spool occupancy never exceeds the configured logical limit and physical free
  space reserve; and
- all scheduling remains event driven. No maintenance polling loop is added.

## Baseline and diagnosis

The 2026-08-31 three-node rsync observation provides the initial baseline:

- node 50 accepted an rsync into `/mnt/machamedia` while nodes 51 and 200 were
  connected and healthy;
- the 16 GiB spool reached 17,179,869,073 of 17,179,869,184 bytes;
- completed-publication EWMA fell from about 3.7 MB/s to about 1.9 MB/s;
- writers accumulated about 2,185 seconds over 46,551 throttle waits;
- one 3.286 GB file eventually became authoritative and the catalogue matched
  it within about two seconds;
- other complete local spool files remained size zero in the authoritative
  namespace while waiting for publication;
- node 50's source and destination disks showed only a few MB/s and low
  utilisation, and sustained process CPU was low; and
- status and RPCs remained responsive with no observed backend failures or
  timeouts during this sample.

This rules out the catalogue as the primary bottleneck. It also shows that the
machine was not limited by disk, CPU, or the cluster's ability to answer control
requests.

The immediate scheduler defect is in
`FuseFrontend::State::data_global_slot_available()`. Any open FUSE writer marks
the mount busy, so continuous rsync restricts data publication to
`foreground_commit_workers` (one on the observed node) although
`commit_workers` is eight. Rsync normally opens the next file before the system
gets a quiet interval, so publication remains serial indefinitely. Large files
therefore impose head-of-line blocking on later complete files.

The wider data path is:

```text
FUSE writes
    -> spool data + per-write journal descriptors
    -> local group durability
    -> per-inode publication request
    -> reread spool and form 4 MiB extents
    -> distributed object puts
    -> per-file durability barrier
    -> serial metadata commit and confirmation
    -> retire the whole inode's spool
    -> catalogue event/scan after authoritative visibility
```

Several secondary effects make the serial bottleneck worse:

- Under pressure, every local durability batch requests publication again for
  each touched inode. The request is coalesced, but still takes locks, updates a
  misleading `merged_publications` counter, and wakes scheduling machinery.
  The observed value of about 661,000 is demand coalescing, not 661,000 useful
  publications.
- Publication rate is sampled only when a complete file finishes. Multi-GB
  files therefore leave the throttle using old information for many minutes.
- Spool capacity is released only when a complete inode is retired. Occupancy
  falls in multi-GB steps even while useful extents are being made durable.
- A file is reread from the spool and content work is repeated during
  publication. The exact byte-copy and hash amplification must be measured
  before altering it.
- Each file owns a separate distributed durability batch. Parallel publishers
  without barrier aggregation could merely turn the bottleneck into excess
  fsync work.
- Data-object handlers can take much longer than control handlers. Saturated
  data traffic must never execute storage waits on an RPC reader/control lane.

## Performance contract

Before making absolute throughput claims, measure raw sequential spool read,
object-store write, and peer-transfer ceilings on the same nodes. The completed
system should meet all of the following:

1. Sustained publication reaches at least 70% of the slowest relevant measured
   physical baseline. On the observed topology, the practical goal is at least
   a tenfold improvement over 1.9 MB/s unless the baseline proves that impossible.
2. At least four independent closed files can make data progress concurrently
   with the observed eight-worker configuration, subject to an explicit
   in-flight-byte bound.
3. A closed small or medium file is not queued behind the full publication of a
   multi-GB file. It becomes authoritative within two times its isolated
   publication time plus five seconds under sustained bulk ingest.
4. Control/status RPC latency remains within its idle-order envelope and has no
   data-load-induced timeout. Exact latency targets belong in the UAT benchmark,
   not timing-sensitive unit tests.
5. CPU consumption follows useful bytes and operations. An idle or blocked
   publisher consumes no core, and raising concurrency does not create a spin.
6. Throttling follows continuously measured sustainable progress, holds the
   spool below its configured limit, and recovers promptly when backlog drains.
7. A committed file reaches catalogue processing within the configured event
   debounce. Catalogue work must never inspect a mutable local overlay as if it
   were authoritative.
8. With no viewer demand and no genuine resource bound, queued ingest work keeps
   the permitted workers and byte window busy. Viewer arrival promptly reduces
   loader service to its configured weighted share at safe yield boundaries,
   without starving it; viewer departure promptly releases full capacity back
   to ingest.
9. Restarted spool work retains the same useful-throughput target as continuous
   ingest. Process lifetime boundaries must not reduce it to a background worker
   allowance.

## Phase 0: establish useful-byte telemetry

Add low-cardinality, O(1) diagnostics before tuning the pipeline:

- queued and active publication inodes, split into open loader, closed loader,
  recovered provenance, genuine background work, and confirmation-waiting
  states; recovered provenance is an orthogonal label, not a queue class;
- active publication workers and peak active workers;
- spool bytes read, object bytes submitted, remotely durable bytes, committed
  bytes, confirmed bytes, and retired bytes;
- extent/object counts and bytes, plus retry and deduplication counts;
- cumulative time in spool read/hash, object submit, remote durability barrier,
  metadata commit, confirmation, and spool retirement;
- publication queue age and closed-file completion age;
- durability group bytes, records, touched inodes, and barriers; and
- separate counters for new publication demand, demand coalesced while queued,
  demand coalesced while running, and actual generations published.

Update the throttle's rate input from useful byte milestones rather than only
whole-file retirement. Preserve a separate end-to-end confirmed/retired rate so
staging cannot masquerade as freed capacity.

Create a reproducible benchmark command outside the default test suite. Record
raw disk/network baselines, single-file publication, eight-file publication,
status latency, CPU, memory, and spool occupancy. Deterministic tests should
assert counters and gates, not wall-clock speed.

Checkpoint: telemetry reconciles exactly for a small deterministic workload and
adds negligible idle work.

## Phase 1A: establish loader priority independently of provenance

This correction precedes concurrency tuning because every later measurement
depends on classifying the work correctly.

1. Add an explicit transport `loader` class below viewer foreground/read-ahead
   and above speculative maintenance. Preserve control as an independent lane.
2. Send publication object puts, durability requests, and their replies as
   loader traffic. Loader activity must not refresh either viewer-activity
   clock or manufacture a viewer quiet window.
3. Replace the FUSE queue's overloaded `recovery` scheduling boolean with two
   independent properties: user-loader priority and recovered-journal
   provenance. Provenance continues to control checksum/corruption handling,
   cache bypass, and restart proof only.
4. Schedule closed user-loader files before open user-loader files regardless
   of which process accepted their durable spool records.
5. Stop applying `recovery_commit_workers` to user-requested spool publication.
   Retain it temporarily as a documented compatibility setting only for genuine
   background recovery work; deprecate it if no such work remains in this
   subsystem.
6. Assign a new, non-conflicting wire value to `loader`, audit every exhaustive
   frame-type switch and priority queue, and define mixed-version behaviour.
   An older peer must never reinterpret loader traffic as viewer traffic; use
   negotiated downgrade to speculative or require a coordinated deployment
   until feature negotiation exists.

Deterministic tests:

- spool accepted before restart resumes in the loader lane and can use the same
  bounded capacity as spool accepted after restart;
- setting `recovery_commit_workers: 1` does not cap four journal-restored user
  files to one publisher;
- viewer traffic wins over loader traffic, loader wins over a saturated
  speculative-maintenance queue, and control completes independently;
- loader sends and replies do not update viewer activity or self-throttle;
- recovered checksum, cache-bypass, corruption, and restart semantics remain
  unchanged after priority is separated; and
- frame encoding, decoding, priority order, diagnostics, and mixed-version
  fallback/rejection behave exactly as documented.

Checkpoint: deploy all three nodes together if loader-frame negotiation is not
yet available. Prove restart does not reduce confirmed/retired throughput, then
continue immediately into the bounded concurrency cut; priority correctness
alone is not a throughput pass.

## Phase 1B: remove artificial serialization and head-of-line blocking

This is the first implementation cut and is expected to produce the largest
immediate gain.

1. Stop treating an open writer anywhere on the mount as a reason to limit all
   publication to one foreground worker. Write-only ingest activity is not
   interactive foreground work.
2. Track writable handles per inode and maintain distinct runnable lanes:
   closed dirty files, pressure-triggered stable prefixes of open files, and
   background work. Prefer closed loader files, then serve open loader files
   fairly. Journal-restored spool remains in these loader lanes; recovered
   provenance is not a scheduling lane.
3. Permit up to `commit_workers` independent inode publications when there is
   spool pressure or closed work. Preserve explicit playback priority using
   signals from the viewer/streaming path, never ordinary FUSE reads.
4. Enforce both a worker limit and an in-flight-byte semaphore. Worker count
   alone is unsafe because each publisher holds extent buffers, RPC payloads,
   and durability state.
5. Keep at most one publication generation active per inode and retain sequence
   ordering within that inode.
6. Initially keep final metadata mutation serial if required, but allow extent
   preparation and object transfer to overlap. Audit and narrow
   `publication_mutex` to the smallest state transition that truly requires it.
7. Use bounded fair service, for example 16-64 MiB quanta, so a large open file
   cannot continuously win over closed files.
8. Make publication demand edge triggered: signal on transition from no useful
   pending work to useful pending work, on close, on pressure-threshold crossing,
   and on confirmation. Repeated writes to an already queued/running inode only
   advance its target sequence.

Deterministic tests:

- four gated closed inodes prove more than one and no more than the configured
  number of publishers are active;
- a large blocked inode does not prevent a later small closed inode completing;
- repeated writes to one queued inode create one runnable entry and no wakeup
  storm;
- no two workers publish the same inode concurrently and sequence order is
  preserved;
- weighted viewer/loader scheduling, loader bounds, recovered-provenance handling,
  stop/cancellation, and confirmation requeue semantics remain correct; and
- with playback absent, ready ingest fills the permitted worker and byte budget;
  injected playback demand receives the configured dominant share without
  corrupting work or starving ingest, and ingest returns immediately to full
  service when that demand clears; and
- the byte semaphore bounds memory even when worker count is high.

Implementation checkpoint (2026-08-31): the resumable cursor, extent-aligned
byte quantum, aggregate admitted-byte budget, fair tail requeue, original
viewer gate, and operational counters are implemented. Phase 1C supersedes the
gate while retaining the cursor, bounds and yield points. The deterministic test queues a
large file before a small file, restricts four workers to one byte quantum, and
proves the small file becomes atomically visible first while the large file
remains invisible. It also proves the large prefix is not reread, and that peak
worker/byte admission remains within the configured bound. Local FUSE coverage
passes; three-node UAT remains before Phase 1B is complete.

UAT checkpoint: strongly recommended. Repeat the current rsync workload before
deeper WAL changes. Require visible concurrent progress, prompt completion of
closed files, rising disk/network utilisation, responsive status, and catalogue
activity as each file becomes authoritative.

## Phase 1C: weighted viewer/loader scheduling and FUSE classification

This corrective cut precedes further throughput phases. The aggregate-rate UAT
showed `rsync --append-verify` reading an existing destination through FUSE at
about 6.4 MiB/s. Those convenience/verification reads were incorrectly recorded
as viewer demand, and the binary `playback_quiet()` gate then stopped all new
publication quanta and serialized commits behind a quiet window.

1. Classify every FUSE operation, including read-only open and read, as loader
   traffic. FUSE must not update foreground/read-ahead viewer clocks.
2. Tag the actual streaming/viewer path explicitly as viewer foreground or
   viewer read-ahead. Classification belongs at the caller boundary, not in a
   heuristic inside the scheduler.
3. Replace the binary viewer quiet-window publication gate with an event-driven,
   byte-quantum weighted scheduler. Add `fuse.viewer_weight` and
   `fuse.loader_weight`, defaulting to 95 and 5. Require both to be positive and
   use relative weights rather than requiring a sum of 100.
4. Make service work conserving. Viewer-only and loader-only workloads each use
   all safe capacity. When both are continuously runnable, accumulated service
   debt selects approximately 95 viewer bytes/quanta for every 5 loader bytes/
   quanta without allowing either class to starve.
5. Preserve immediate viewer admission through reserved viewer executor/RPC
   capacity. Weighting must not put a viewer behind a loader queue; it controls
   shared storage/network service after independently responsive admission.
6. Retain bounded loader quanta and the byte-bounded extent pipeline. A viewer
   can wait only for already-admitted bounded work, not a whole file or a global
   quiet interval.
7. Remove `wait_for_playback_quiet()` from the serialized commit boundary.
   Long durability/commit work must run on the loader/data executor and must not
   occupy communications threads. If a physical commit cannot be pre-empted,
   admission must bound its size and preserve viewer headroom before it starts.
8. Expose per-class admitted/completed bytes or quanta, runnable time, and
   starvation/debt counters so the configured ratio and borrowing behaviour are
   auditable without polling.
9. Deprecate `fuse.publication_quiet_ms` and
   `fuse.foreground_commit_workers` after compatibility parsing is documented;
   they must no longer implement normal scheduling policy.

Deterministic tests:

- configuration defaults to 95:5, accepts other positive relative weights, and
  rejects zero/overflow values;
- sustained FUSE `--append-verify` analogue reads do not refresh viewer activity
  and publication continues making bounded progress;
- genuine viewer reads receive prompt service and approximately the configured
  share while a saturated loader remains runnable;
- the loader completes repeated quanta under continuous viewer demand, proving
  non-starvation;
- either class borrows full capacity when the other queue is empty, including
  immediate loader recovery when viewer demand ends;
- different-process convenience reads, read-only FUSE handles, and FUSE reads
  of an inode also open for writing all remain loader class;
- viewer admission, control RPC latency, in-flight byte bounds, cancellation,
  restart provenance, and atomic whole-file visibility remain unchanged; and
- no scheduler timer, maintenance loop, or busy wait is introduced.

UAT checkpoint: rerun the live `rsync --append-verify` workload and real
viewer/streaming playback together. Prove FUSE verification no longer suppresses
publication, the loader continues at its configured residual share during
sustained viewing, the viewer remains responsive, and loader throughput returns
to full safe capacity immediately after playback. Only then resume the paused
aggregate-retirement-rate UAT.

## Phase 1D: make control and viewer priority non-bypassable

This phase is a correctness prerequisite, not throughput tuning. The deployed
overlapping-writer UAT showed that a loader grant can call `materialize()` or
`rebuild()` and synchronously process an entire file. The outer weighted
scheduler cannot observe or pre-empt that work. Ordinary kernel block I/O then
has no knowledge of Macha's priority classes, and `publication_mutex` serialises
other commits behind it. The detailed live evidence is in
[the rebuild-stall diagnosis](2026-08-31-overlapping-fuse-writer-rebuild-stall.md).

### 1D.1: propagate a data-work budget and audit existing control isolation

Macha already has separate CONTROL/DATA transport lanes, a closed fast-control
allow-list, a dedicated metadata executor, and separate CONTROL storage. Keep
and strengthen that design rather than replacing it with one universal
scheduler.

1. Audit the existing fast-control allow-list and dedicated metadata executor.
   Retain the rule that fast control is bounded published-memory work only and
   prove it remains responsive with every DATA executor blocked.
2. Introduce an immutable `DataWorkContext` containing class (`viewer`,
   `loader`, `speculative`), cancellation/deadline, granted byte/CPU budget, and
   accounting ticket. Require it at potentially blocking DATA object, DATA RPC,
   hashing and durability boundaries.
3. Carry the originating class into metadata work caused by DATA publication.
   A loader-originated manifest/namespace commit must not become high-priority
   merely because it uses a metadata RPC; the dedicated metadata executor needs
   bounded origin-aware admission behind genuine control metadata work.
4. A child DATA operation inherits or lowers its parent's priority; it may never
   raise itself. Only an explicit streaming entry point may originate viewer
   DATA work.
5. Record admitted, started, yielded, completed and cancelled bytes/operations,
   plus maximum uninterrupted service and queue delay, per class and resource.

Checkpoint: existing control-isolation tests remain green, compile-time/API
tests prove bulk DATA I/O cannot be submitted without a context, and
deterministic accounting proves priority is retained through nested DATA calls
and loader-originated metadata publication.

Partial implementation checkpoint (2026-08-31): FUSE publication now creates
an immutable loader `DataWorkContext` containing its admitted byte quantum, and
`WriteHandle` preserves that class through nested extent fetch/put, rebuild and
durability operations. Loader writes no longer manufacture viewer activity, and
CONTROL is rejected at the DATA context boundary. Filesystem/FUSE coverage is
51/51 green and the existing foreground/control isolation tests are green. This
does **not** complete 1D.1: mandatory context APIs, deadline/cancellation and
accounting tickets, origin-aware metadata admission, and per-resource service
accounting remain. See
[the checkpoint](2026-08-31-fuse-publication-phase-1d-data-work-context.md).

### 1D.2: turn unbounded file work into resumable state machines

1. Replace synchronous whole-file `materialize()` and `rebuild()` calls with
   resumable cursors over changed ranges/extents. Each step consumes at most the
   granted byte/time budget and returns `complete`, `yielded`, `cancelled`, or
   `failed`.
2. Preserve immutable references for unchanged extents. Compatible interleaved
   append ranges are ordered and coalesced from the durable operation journal;
   they do not force base-file materialisation.
3. Detect genuinely conflicting overwrites explicitly. Reconstruct only the
   changed extent and its boundary extents; never reread/re-hash the full file
   merely because writes arrived from two handles.
4. Keep the Phase 1D cursor resumable in memory and preserve the current durable
   spool/journal as restart authority. Restart may redo a bounded, idempotent
   unit but must not re-enter an unbounded whole-file call. Durable staged-range
   reuse belongs to Phase 2's versioned journal work.
5. Add a higher-class check before every read, hash, object put and durability
   submission. A loader yields without discarding proven staged work.

Checkpoint: a large-base/two-writer deterministic test shows bounded unchanged
base reads, repeated loader yields, exact final content, atomic visibility and
restart at every cursor boundary.

Partial implementation checkpoint (2026-08-31): old-generation materialisation
is now an in-memory resumable cursor. FUSE charges each preparation step to its
existing loader byte quantum and retains both staging and WAL cursors across a
clean event-driven requeue. Direct and end-to-end FUSE tests prove exact
one-extent steps, repeated yields, no repeated source reads, partial-tail
handling, atomic visibility and exact final content; filesystem/FUSE coverage is
53/53 green. This is not the complete 1D.2 checkpoint: rebuild and commit remain
synchronous and restart-boundary injection has not yet been added. See
[the materialisation checkpoint](2026-08-31-fuse-publication-phase-1d-resumable-materialization.md).

Second partial implementation checkpoint (2026-08-31): rebuild is also a
per-handle resumable cursor and FUSE charges complete canonical extents to fresh
loader grants before final publication. The redundant frontend-wide
`publication_mutex` has been removed; metadata mutation, write-handle rename
ordering and content-version conflict checks retain their narrower authority.
Tests prove old-generation visibility at every rebuild boundary, exact extent
reuse/put counts and at least nine end-to-end yields for the four-extent
overwrite analogue; filesystem/FUSE coverage remains 53/53 green. Changed-range
reconstruction, restart-boundary injection, asynchronous durability completion
and origin-aware metadata admission remain. See
[the rebuild checkpoint](2026-08-31-fuse-publication-phase-1d-resumable-rebuild.md).

Third partial implementation checkpoint (2026-08-31): canonical manifests now
use a sparse changed-range overlay. Untouched extents are reused by immutable
reference without fetch or hashing; touched extents alone are seeded and
reconstructed under the existing resumable quantum. Completed-cohort counters
make source/spool reads, reused extents and put extents directly comparable to
completed bytes. The deterministic eight-extent regression proves two touched
extent reads, six untouched reuses, two puts, exact bytes and atomic visibility.
Filesystem/FUSE is 55/55 green, the controlled complete suite is 226/226, and
runtime is 3/3. Restart-boundary injection, asynchronous durability completion
and origin-aware metadata admission remain. See
[the sparse changed-range checkpoint](2026-08-31-fuse-publication-phase-1d-sparse-changed-ranges.md).

### 1D.3: reserve end-to-end resources and remove priority inversion

1. Keep the existing independent fast-control, ordinary-control and metadata
   RPC admission/execution capacity. Fast-control handlers perform bounded
   published-memory work only; ordinary control/metadata handlers enqueue
   bounded work on their existing isolated executors and never run bulk DATA
   work inline.
2. Add priority-aware, byte-bounded credits within the DATA plane for object
   reads/writes, hashing, per-peer RPC, per-storage-domain I/O and durability
   barriers. Preserve separate CONTROL storage/quota and the hard
   non-borrowable control executors. Viewer has reserved DATA headroom;
   loader/speculative work may borrow unused DATA capacity only while their
   bounded outstanding work leaves the viewer bound intact.
3. Retain configurable 95:5 viewer/loader service over the non-control capacity.
   A viewer arrival stops new loader submissions immediately, but already-issued
   loader I/O is bounded so viewer delay has a deterministic ceiling. Loader
   continues receiving its non-zero share during sustained viewing.
4. Remove `publication_mutex`. Use per-inode generation ownership and a shared
   priority-aware commit executor. No cross-inode lock spans materialisation,
   hashing, object I/O, a durability barrier or metadata observation.
5. Split durability into submission and completion tickets. A waiting loader
   releases executor and byte credits that are not required to preserve the
   submitted operation; completion events requeue it at its original class.
6. Audit metadata locks and store locks for priority inversion. Critical
   sections may update bounded in-memory state only and must expose measured
   maximum hold time.

Checkpoint: with loader I/O saturated and deliberately delayed, control requests
complete using their reserved path and viewer reads begin within the declared
bound. Neither waits for a loader-owned mutex or executor slot.

First partial implementation checkpoint (2026-08-31): a node-wide event-driven
byte arbiter now sits below RPC admission at blocking DATA object/local-store
boundaries. Loader/speculative work cannot consume configurable viewer
headroom; CONTROL remains outside the arbiter; lower-class saturation tests
prove foreground object reads and CONTROL ping complete before a blocked loader
is released. Direct repair placement, incoming/outgoing transfers, ordinary
local/cache reads and asynchronous fetched-object persistence are covered.
Status exposes exact capacity/use/wait counters. The clean complete default
suite passed 225/225 and runtime dependencies 3/3. Per-peer/storage-domain,
hashing, durability completion tickets and origin-aware metadata admission
remain active. See
[the DATA resource checkpoint](2026-08-31-phase-1d-data-resource-headroom.md).

### 1D.4: guarantee retirement progress under full-spool pressure

1. Schedule closed or otherwise completable generations that release the most
   spool per bounded unit, while retaining per-inode fairness. One conflicting
   inode cannot occupy every publication/commit credit.
2. Reserve at least one loader retirement ticket while the spool is above its
   pressure threshold. It remains below viewer/control priority and participates
   in the loader's configured share, but speculative work cannot consume it.
3. Coalesce publication notification at durable sequence/byte boundaries.
   Eliminate the observed per-write request storm; repeated notification of an
   already-running inode updates one target watermark and performs no queue work.
4. Keep the hard spool bound and proportional acceptance pacing. Backpressure
   may stop an ingester at the bound, but an available completable generation
   must continue retiring without requiring new writes or polling.

Checkpoint: fill a small test spool with one pathological overlapping-writer
inode and independent closed files. Closed files commit, occupancy falls, and
the blocked writer resumes while control and viewer bounds continue to hold.

First partial implementation checkpoint (2026-08-31): publication notification
is now coalesced at durable data/namespace watermarks, an explicit enqueue owner
closes the inode-to-queue handoff race, and unchanged flush/release notifications
perform no shared queue work. One false-to-true spool-drain transition owns the
all-inode pressure sweep; later durable batches notify only their affected
inodes. A deterministic test reduces 1,002 notifications to two effective
requests and 1,000 suppressed duplicates, the three pressure tests prove one
sweep per episode, filesystem/FUSE is 56/56, the controlled complete suite is
227/227 and runtime is 3/3. Retirement selection/reservation and fewer metadata
generations remain. See
[the notification-coalescing checkpoint](2026-08-31-fuse-publication-phase-1d-notification-coalescing.md).

Second partial implementation checkpoint (2026-08-31): above the spool-pressure
threshold, closed generations are ranked by releasable spool bytes per estimated
remaining replay byte, then nearest completion. Normal below-pressure FIFO and
open-loader fallback are unchanged. A deterministic small-spool regression
proves a later small closed generation retires ahead of an earlier large closed
generation and wakes a blocked writer without polling. The new test and all
57 filesystem/FUSE tests pass. Failure diagnostics corrected the pre-existing
catalogue final-state test's invalid exact-generation and asynchronous
run-sampling assumptions without changing its timeout or content/GC proof; it
then passed 10/10 isolated repetitions and the final controlled complete suite
passed 228/228. Deployment and loaded UAT remain. Reserved retirement capacity,
transient-failure cursor preservation and metadata-generation coalescing remain. See
[the retirement-selection checkpoint](2026-08-31-fuse-publication-phase-1d-retirement-selection.md).

Third partial implementation checkpoint (2026-08-31): retryable backend errors
now preserve the exact process-lifetime publication, WAL and
materialisation/rebuild cursor rather than reopening the writer and replaying
the generation from byte zero. Pipelined extent futures retain their bounded
payload and manifest offset on failure, retry in place at the queue head, and
cannot let later results create a manifest hole. A deterministic one-shot
failure after the first extent proves one publication start, one backend
failure, exact one-pass spool reads, atomic final content and successful
completion. Filesystem/FUSE is 58/58 green, the controlled complete suite is
229/229 and runtime is 3/3. Reserved retirement capacity and
metadata-generation coalescing remain. See
[the transient-failure cursor checkpoint](2026-08-31-fuse-publication-phase-1d-transient-failure-cursor.md).

### Phase 1D invariant tests and UAT gate

Deterministic tests must cover:

- control arrival during every loader checkpoint and every injected slow
  resource, with a bounded completion assertion;
- viewer startup, streaming reads and repeated seeks during materialisation,
  rebuild, object put, durability and metadata delay;
- continuous viewer demand retaining approximately the configured 95:5
  viewer/loader share without loader starvation;
- immediate work-conserving borrowing after control/viewer demand ends;
- two compatible appenders, conflicting overwrite/append, truncate, cancellation
  and restart at every resumable cursor state;
- a full spool with a pathological inode plus independently retireable files;
- no lower-class disk/network wait on control/RPC threads and no I/O while a
  cross-class/cross-inode mutex is held; and
- assertions on maximum outstanding lower-class bytes, uninterrupted work,
  lock hold time and per-class queue delay, rather than timing-only sleeps.

Deployed UAT must run overlapping `rsync --append-verify`, force the spool above
its pressure threshold, start real playback with repeated seeks, and exercise
status/membership/metadata control requests concurrently. Pass only if:

1. control remains within its declared latency envelope with no timeouts;
2. playback starts and continues without a loader-induced stall;
3. loader progress remains non-zero during playback and promptly becomes
   work-conserving afterward;
4. spool occupancy retires below pressure without restarting any process;
5. unchanged file regions are not reread/rehashed; and
6. counters prove every lower-class uninterrupted unit and outstanding-I/O
   bound was respected.

Phase 1C is not UAT-complete until this gate passes. Phase 2 may reuse the
resumable range/cursor machinery, but it must not be used to postpone these
invariants.

## Phase 2: stage immutable extents incrementally

Decouple useful object publication from whole-file closure while retaining
atomic namespace visibility.

1. Once a full extent is locally durable and no longer mutable for the target
   sequence, queue it to a loader staging pool. FUSE request threads only
   copy and journal local data; they never perform network publication.
2. Store a durable mapping from file generation/range to staged object identity
   and durability state. On restart, reuse proven staged extents instead of
   rereading and resending them.
3. Publish manifest/metadata only when every range required by the requested
   generation is durable. The namespace still changes atomically from the old
   complete generation to the new complete generation.
4. Coalesce adjacent sequential dirty ranges before staging. Measure first, then
   remove duplicate copies or hashes where an existing checksum can safely
   provide the required content identity.
5. Allow fair extent-level scheduling between inodes so small closed files can
   finish while large files continue to stage.
6. Do not reclaim a spool range until the journal contains sufficient durable
   proof to reconstruct and commit the generation without it. If range hole
   punching is portable and correct, reclaim proven prefixes; otherwise retain
   whole-file retirement and count staged versus actually free bytes separately.

This phase requires a versioned journal record and an explicit compatibility
story. Restart tests must cover every transition: local data durable, stage
queued, object stored, remote barrier complete, manifest committed, confirmation
received, and spool range retired. Also cover overwrite, truncate, append,
deduplication, cancellation, and a peer disappearing between stages.

Checkpoint: a restart at every injected boundary completes exactly once, never
exposes a partial file, and never needs data that was reclaimed.

## Phase 3: reduce local journal and fsync amplification

The observed 61,696 writes formed 27,580 durability batches, only about 2.24
writes per batch. Improve useful bytes per durability epoch without adding an
arbitrary long delay:

1. Aggregate contiguous sequential writes for an inode into bounded range
   descriptors before the durable journal boundary.
2. Drive group commit by a byte target and short maximum delay. `fsync`, release,
   spool pressure, and shutdown provide immediate flush triggers.
3. Sync each touched spool file once per epoch, append its journal records as a
   group, and issue one journal durability barrier per epoch.
4. Use `fdatasync` for payload files where metadata durability is not required,
   while preserving the ordering proof that journal descriptors cannot become
   recoverable before their bytes.
5. Expose bytes and descriptors per epoch so improvement is measured directly.

Tests use an injected durability backend and explicit gates to prove grouping,
flush triggers, ordering, error fan-out, restart recovery, and bounded latency.

## Phase 4: pipeline data RPC and aggregate durability barriers

Implementation note (2026-08-31): Phase 4A now pipelines a configurable,
byte-bounded set of provisional extent puts within each file and drains that
set at every fairness/viewer boundary. Atomic visibility and the final aggregate
durability barrier are preserved. Local verification is recorded in
[the Phase 4A checkpoint](2026-08-31-fuse-publication-phase-4a-extent-pipeline.md).
The shared data executor, per-peer/domain limits and cross-publication physical
barrier aggregation below remain open.

1. Pipeline bounded extent puts across files and, where beneficial, within a
   file. Bound queued and active work by bytes per peer and per storage domain.
2. Execute data-object storage and durability waits on a dedicated data executor.
   RPC readers and control/status lanes may parse and enqueue bounded work but
   must not wait for disk or remote durability.
3. Aggregate compatible object durability across publishers into shared storage
   epochs. A single physical durability barrier may satisfy many extent puts;
   its completion is fanned out to the owning publication tickets.
4. Avoid assembling repeated full 4 MiB copies between spool read, hashing,
   object payload, and 256 KiB RPC frames. Prefer bounded reusable buffers or
   scatter/gather after profiling confirms the copy cost.
5. Apply explicit byte backpressure before accepting more data work so queues
   cannot consume unbounded RAM during a slow disk or disconnected peer.

Tests saturate the data executor while proving ping/status and metadata control
complete, byte limits hold, disconnects resolve all tickets, shared barriers
cover exactly their members, and no request receives success before durability.

UAT checkpoint: recommended. Saturate object publication while sampling control
latency, per-stage throughput, CPU, memory, peer queues, and physical disk work.

## Phase 5: integrate admission, progress, and catalogue events

1. Feed admission control with separate local-ingest, staging, remote-durable,
   confirmed, and retired rates. Only retired/reclaimable bytes increase free
   spool capacity.
2. Use occupancy hysteresis and measured drain capacity to make admission smooth:
   burst below the low watermark, progressively pace in the control band, and
   wait at the hard bound. Never busy-wait.
3. Increase or reduce publication concurrency only on useful completion/latency
   events and with stable hysteresis. A conservative fixed bounded concurrency
   is acceptable before adaptive control is justified by measurements.
4. Allocate fair admission and publication shares across concurrent writers so
   one import cannot monopolise the spool.
5. Emit a direct catalogue hint when a file generation becomes authoritative.
   Retain the existing event-driven safety scan/coalescing path. The observed
   catalogue latency was already good, so this is lower priority than the data
   pipeline.

Configuration should expose the spool byte limit, occupancy watermarks,
publication worker bounds, in-flight byte bound, extent scheduling quantum, and
local durability byte/delay bounds. Defaults must be safe on small machines and
validated as a coherent set.

## Final verification matrix

Run these cuts after their corresponding deterministic tests pass:

1. Four closed 4 GiB files with a 16 GiB spool: prove parallel progress and no
   head-of-line blocking.
2. A mixed-size 100 GiB rsync: prove initial burst, smooth convergence to drain
   speed, prompt small-file visibility, and catalogue progress during import.
3. Concurrent writers: prove admission and publication fairness.
4. Saturated data publication plus continuous status/ping/metadata operations:
   prove communications isolation and absence of timeouts.
5. Restart at greater than 90% spool occupancy and at every durable staging
   state: prove recovery, bounded occupancy, and resumed loader-class throughput
   without a process-lifetime priority demotion.
6. Peer loss and return during publication: prove progress with the configured
   write quorum and later convergence without foreground replication
   amplification.
7. Idle soak after drain: prove zero scheduler spin, no maintenance loop, no
   delayed work, and stable memory.
8. Viewer-under-load test: begin playback and perform repeated seeks during a
   saturated import, prove viewer startup/read latency stays within its defined
   envelope, prove ingest retains its configured non-zero share throughout,
   then prove ingest promptly returns to full safe capacity when the viewer
   becomes idle.

Each UAT record must include the raw physical baseline and the new useful-byte
counters. A throughput number without an identified limiting stage is not an
acceptable result.

## Sequencing and stop points

- Phase 0, Phase 1A, Phase 1B, corrective Phase 1C and invariant Phase 1D are one
  safe delivery sequence: measure, establish the loader class independently of
  restart provenance, remove false worker caps, add fairness/bounds, replace
  exclusion with weighted service, propagate priority through every resource,
  make hidden whole-file work resumable, then UAT. Do not tune concurrency
  against the old `recovery` lane or use the earlier two-worker UAT as a
  performance baseline.
- Phase 1D is a release gate. Do not claim viewer/control priority, resume the
  aggregate-rate UAT, or begin throughput tuning while any lower-class call can
  issue unbounded work or hold an unreserved shared resource.
- Phase 2 remains a large correctness change and should be delivered separately
  behind versioned recovery records and exhaustive fault injection. It should
  build on Phase 1D's resumable range state rather than reintroducing a second
  publication mechanism.
- Phase 3 can proceed independently after Phase 0 if local durability is shown
  to limit the new pipeline.
- Phase 4 should follow Phase 1 telemetry; add concurrency before barrier
  aggregation so the measurement demonstrates whether aggregation is needed.
- Phase 5 is tuning and integration, not a substitute for fixing the earlier
  structural bottlenecks.

At every pause, update `TODO/ACTIVE.md` with the last completed checkpoint and
the next exact implementation step. Move a phase to `TODO/COMPLETED.md` only
after its deterministic tests and stated UAT checkpoint have both passed.

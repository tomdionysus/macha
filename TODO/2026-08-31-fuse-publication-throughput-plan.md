# FUSE publication throughput plan

Date: 2026-08-31

Status: Phase 0/1 first implementation checkpoint complete; deployment UAT and
remaining telemetry/byte-bound/fair-quantum work pending

## Scheduling laws

1. **Thou Shalt Not Make The Viewer Wait.** Playback startup, reads, seeks, and
   the control work required to serve them have absolute priority. No ingest
   throughput improvement is acceptable if it introduces viewer-visible delay,
   buffering, starvation, or latency spikes.
2. **Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
   Viewer Wait.** In the absence of viewer contention, ingest must use the
   available spool, storage, network, CPU, and publication capacity. It may be
   paced for hard capacity, durability, bounded-memory, fairness, or genuine
   downstream throughput limits, but not by an artificial quiet period or the
   mere existence of another open writer.

These laws define priority, not polling. Viewer demand, resource availability,
durability completion, queue transitions, and pressure thresholds must wake or
pace work through events.

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
   or pauses competing work at safe cancellation/yield boundaries, and viewer
   departure promptly releases that capacity back to ingest.

## Phase 0: establish useful-byte telemetry

Add low-cardinality, O(1) diagnostics before tuning the pipeline:

- queued and active publication inodes, split into open, closed, recovery, and
  confirmation-waiting states;
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

## Phase 1: remove artificial serialization and head-of-line blocking

This is the first implementation cut and is expected to produce the largest
immediate gain.

1. Stop treating an open writer anywhere on the mount as a reason to limit all
   publication to one foreground worker. Write-only ingest activity is not
   interactive foreground work.
2. Track writable handles per inode and maintain distinct runnable lanes:
   closed dirty files, pressure-triggered stable prefixes of open files, and
   recovery. Prefer closed files, then serve other lanes fairly.
3. Permit up to `commit_workers` independent inode publications when there is
   spool pressure or closed work. Preserve explicit playback priority using
   actual playback/read signals rather than the existence of a writer.
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
- playback gating, recovery limits, stop/cancellation, and confirmation requeue
  semantics remain correct; and
- with playback absent, ready ingest fills the permitted worker and byte budget;
  injected playback demand pre-empts new ingest quanta without corrupting work,
  and ingest resumes immediately when that demand clears; and
- the byte semaphore bounds memory even when worker count is high.

UAT checkpoint: strongly recommended. Repeat the current rsync workload before
deeper WAL changes. Require visible concurrent progress, prompt completion of
closed files, rising disk/network utilisation, responsive status, and catalogue
activity as each file becomes authoritative.

## Phase 2: stage immutable extents incrementally

Decouple useful object publication from whole-file closure while retaining
atomic namespace visibility.

1. Once a full extent is locally durable and no longer mutable for the target
   sequence, queue it to a background staging pool. FUSE request threads only
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
   state: prove recovery, bounded occupancy, and resumed throughput.
6. Peer loss and return during publication: prove progress with the configured
   write quorum and later convergence without foreground replication
   amplification.
7. Idle soak after drain: prove zero scheduler spin, no maintenance loop, no
   delayed work, and stable memory.
8. Viewer-under-load test: begin playback and perform repeated seeks during a
   saturated import, prove viewer startup/read latency stays within its defined
   envelope, then prove ingest promptly returns to full safe capacity when the
   viewer becomes idle.

Each UAT record must include the raw physical baseline and the new useful-byte
counters. A throughput number without an identified limiting stage is not an
acceptable result.

## Sequencing and stop points

- Phase 0 and Phase 1 are one safe delivery sequence: measure, remove the false
  single-worker cap, add fairness/bounds, test, then UAT.
- Phase 2 is the largest correctness change and should be delivered separately
  behind versioned recovery records and exhaustive fault injection.
- Phase 3 can proceed independently after Phase 0 if local durability is shown
  to limit the new pipeline.
- Phase 4 should follow Phase 1 telemetry; add concurrency before barrier
  aggregation so the measurement demonstrates whether aggregation is needed.
- Phase 5 is tuning and integration, not a substitute for fixing the earlier
  structural bottlenecks.

At every pause, update `TODO/ACTIVE.md` with the last completed checkpoint and
the next exact implementation step. Move a phase to `TODO/COMPLETED.md` only
after its deterministic tests and stated UAT checkpoint have both passed.

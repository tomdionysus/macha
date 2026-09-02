# Structural runtime remediation — first implementation checkpoint

Status: local checkpoint complete; guarded UAT failed the retained-memory gate

Date: 2026-09-02

Parent plan:
[structural ingest and runtime remediation](2026-09-02-structural-ingest-runtime-remediation.md)

## Implemented

### Indexed FUSE pending-data reads

- The append-only `DataOp` journal remains the crash-recovery and publication
  authority.
- Each live inode now maintains a derived non-overlapping interval overlay which
  records only the newest visible spool/zero source for each dirty range.
- Adjacent sequential writes with contiguous spool storage coalesce in the
  runtime index.
- FUSE reads query only ranges intersecting the requested interval and copy
  compact descriptors, rather than deep-copying every `DataOp` and checksum
  vector and replaying the whole history.
- Shrink, regrow, sparse gaps and overwrites preserve POSIX zero/last-write-wins
  semantics. Recovery and publication retirement rebuild the same derived index
  from the authoritative remaining operations.
- O(1) Status counters expose read queries, ranges examined and descriptors
  copied:
  - `.diagnostics.filesystem.data_overlay_read_queries`
  - `.diagnostics.filesystem.data_overlay_ranges_examined`
  - `.diagnostics.filesystem.data_overlay_descriptors_copied`

### Object-scoped loose storage

- Loose-object reads, encryption and physical writes no longer run beneath the
  store-wide index/accounting mutex.
- Exact per-object weak lock ownership coalesces operations for the same
  immutable ID while unrelated objects proceed independently. Expired lock
  entries are removed, so the registry does not grow with historical objects.
- Concurrent loose writes reserve physical/logical capacity before allocating
  ciphertext or issuing I/O. Every failure path releases the reservation.
- Reaffirmation retains the strong existing contract: a successful PUT proves
  the named replica contains the supplied bytes and repairs external
  corruption.
- A bounded 4,096-entry verification cache avoids repeated full read/decrypt/hash
  work when the exact loose file's device, inode, size, mtime and ctime are
  unchanged. External modification invalidates the fast path and forces strong
  validation/repair. The cache's recency queue is also compacted at a fixed
  bound.
- Status exposes aggregate backend counters:
  - `.diagnostics.data_store.loose_reaffirmation_fast_paths`
  - `.diagnostics.data_store.loose_reaffirmation_full_validations`

Packed small-object work remains store-serialised. Media extents are loose
objects under the normal configuration; packed concurrency remains part of the
parent Phase 1 rather than being silently claimed complete here.

## Tests added

- `filesystem_fuse/test_fuse_pending_overlay_reads_only_intersecting_ranges`
  creates 256 pending sequential writes, proves a small read examines and copies
  one range, then verifies overwrite and shrink/regrow zero semantics.
- `storage_v18/test_unrelated_loose_object_read_bypasses_blocked_loader_write`
  freezes a loader write after capacity admission and proves an unrelated viewer
  read completes while a second same-ID write remains single-flight.
- `storage_metadata/test_local_store` now proves unchanged reaffirmation takes
  the verified fast path while external corruption forces full validation and
  successful repair.
- Existing recovery, ordered mutation, corruption repair, replica repair,
  rebalance and durability-generation regressions remain passing.

## Verification

- Complete default suite: **260/260 passed**.
- Runtime-dependency suite: **3/3 passed**.
- Filesystem/FUSE focused suite: **62/62 passed**.
- Storage v18 focused suite: **17/17 passed**.

The test runner remained process-isolated and parallel; no serial fallback was
used.

The first exact-tree run caught a startup Status regression introduced by the
new store counters: Status attempted to obtain the storage pool while it was
still recovering. The diagnostics are now explicitly availability-aware and
observational; they return `available: false` without blocking or failing the
API. Both startup-order regressions pass, followed by the clean 260/260 and 3/3
run reported above.

## Useful UAT now

A short guarded four-node deployment UAT is useful because physical disk,
network, FUSE kernel request shape and Pi allocator/RSS behaviour are not fully
represented by deterministic tests.

1. Synchronise the complete source and build the three identical Pi nodes in
   parallel; rebuild/start node 200 normally. Verify versions and identical Pi
   hashes before load.
2. Record idle RSS, swap, CPU, metadata generation, spool occupancy, control/RPC
   latency and the new counters.
3. Start one `rsync --append-verify` on node 50 and ordinary playback from node
   50. Sample all four nodes at approximately 1, 3, 5 and 10 minutes.
4. Verify overlay ranges examined per read remain proportional to intersections
   (normally near one for sequential append verification), not pending journal
   record count.
5. Verify control remains responsive and playback can read unrelated extents
   while loader writes are active. Record start/seek behaviour and RPC timeouts.
6. Verify RSS growth is materially different from the previous roughly
   200 MiB/minute climb. Stop all Macha services if node 50 exceeds 1.25 GiB RSS,
   grows by more than 128 MiB/minute across two consecutive samples, begins
   sustained swap growth, or control/playback becomes unavailable.

This UAT is directional, not the final structural acceptance test. Do not leave
rsync overnight. Process-wide ownership accounting, physical work-class
propagation, metadata amplification and causally stable profile GC remain open.

## Continuation after UAT

The 2026-09-02 guarded run failed safely: GBNI-1 grew from 498,794,496 bytes to
992,722,944 bytes RSS in about 65 seconds across the confirming samples. The
overlay fix passed with exactly one range examined and descriptor copied per
read, so the remaining growth belongs to other retained owners. All services
were stopped. See
[`2026-09-02-structural-runtime-first-uat.md`](2026-09-02-structural-runtime-first-uat.md)
for measurements and the shutdown exception exposed by the stop.

- If the two counters and resource curves behave as predicted, continue parent
  Phase 1 physical work-class/RPC isolation and Phase 3 process-wide retained
  memory governance.
- If RSS still climbs rapidly while overlay intersection counts remain bounded,
  use the existing owner diagnostics to attribute the remaining live bytes; do
  not add another generic concurrency limit.
- If playback still blocks while unrelated local store reads proceed, trace the
  next physical/durability/RPC owner outside the corrected store mutex rather
  than weakening viewer priority or increasing timeouts.

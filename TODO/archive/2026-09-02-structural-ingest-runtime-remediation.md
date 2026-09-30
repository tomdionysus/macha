# Structural ingest and runtime remediation

Status: primary active programme

Date established: 2026-09-02

This plan supersedes throughput tuning and isolated ingest-memory remediation as
the primary line of work. The architecture remains: durable local FUSE WAL,
immutable content-addressed extents, replicated metadata, ownerless playback,
and priority-aware work. The defect is that several runtime implementations turn
incremental work into whole-history copies, whole-library scans, globally
serialised storage, or uncharged retained memory.

The two project laws remain the acceptance authority:

2. Thou Shalt Not Make The Viewer Wait.
3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.

No throughput result is acceptable if it bypasses control/viewer priority,
allows unbounded retained memory, or relies on longer timeouts.

## Confirmed structural findings

- `LocalStore` holds one store-wide mutex across filesystem I/O, decryption,
  hashing/comparison, encryption and writes. Viewer reads therefore contend with
  already-admitted loader work outside the priority scheduler.
- Reaffirming an existing content-addressed extent reads, decrypts and compares
  the complete stored extent even though its immutable object ID already names
  the expected content.
- FUSE reads deep-copy the complete base manifest and pending `DataOp` history,
  then replay every write/truncate. `rsync --append-verify` drives this towards
  quadratic allocation and scan work as a partial file grows.
- Spool payload bytes are bounded, but per-inode operation/checksum metadata and
  copied publication snapshots are not bounded by their real retained cost.
- Subsystem-local byte limits do not form a process-wide memory bound. Payloads,
  RPC frames, publication cursors, operation histories, metadata/catalogue
  snapshots and caches have independent or incomplete accounting.
- Small metadata changes still decode, copy, encode and hash whole namespace
  snapshots; concurrent authors amplify this into expensive branch/merge work.
- Every metadata notice can request a full-library media-profile prune. Pruning
  computes immutable identities from every file manifest and can remove a valid
  profile using a temporarily incomplete local branch, creating further
  catalogue and metadata churn.
- Work class currently governs only selected admission points. Store locks,
  crypto, physical I/O, metadata work, GC, checksum work and diagnostics can
  bypass `control > viewer >> loader > speculative`.
- Per-extent slow-path logging grows when the system is already overloaded.

Deferred durability was inspected and does not retain file payload bytes until
whole-file commit. Its batch retains compact replica/generation requirements,
although that requirement list still scales once per extent and needs bounded
coalescing.

## Required invariants

- Expensive work has an explicit owner, lifetime, retained-byte charge and work
  class from admission until release.
- No store-global lock is held across disk I/O, crypto, RPC waits or durability
  waits. Contention for the same immutable object is single-flight; unrelated
  objects proceed independently.
- A FUSE read examines only the base extents and pending ranges intersecting its
  requested interval. Journal history is recovery authority, not the runtime
  query index.
- Spool admission accounts for payload and operation metadata. Pathological
  small-write workloads cannot create unbounded in-memory descriptors beneath a
  bounded byte spool.
- Viewer and control work can overtake loader work at every bounded scheduling
  boundary. Loader retains its configured non-zero share and may borrow idle
  capacity.
- Small namespace/catalogue mutations have cost proportional to changed state,
  except for explicit bounded checkpoints.
- Immutable-media profile removal requires causally stable proof of zero live
  references, not absence from one currently available branch.
- Diagnostics are aggregate and rate-limited on hot paths.

## Phase 0 — deterministic proof and ownership accounting

- [x] Add an append-verify analogue which builds a large pending write history
  and repeatedly reads the growing destination. Record operations examined,
  descriptor bytes copied, allocations and elapsed CPU per read.
- [x] Add duplicate-object put tests measuring reads, decryptions, hashes,
  physical writes and allocations on local and remote reaffirmation.
- [x] Add a mixed loader/viewer test which blocks a loader store operation and
  proves a viewer operation is not trapped behind its store-wide critical
  section.
- [ ] Extend retained-memory diagnostics to identify FUSE operation histories,
  publication snapshots/cursors, extent payloads, RPC frames, durability
  requirements, metadata/catalogue snapshots and media-profile work.
- [ ] Establish a reproducible safe loaded benchmark. Tests must fail on work
  amplification or ownership bounds; default CI must not depend on wall-clock
  performance thresholds.

Exit gate: the current implementation fails the targeted amplification and
priority tests for the expected reasons, and all material retained owners are
visible in diagnostics.

## Phase 1 — object-store concurrency and idempotence

- [x] Split store metadata/index protection from physical I/O and crypto. Use
  short critical sections, immutable index snapshots where useful, and
  per-object single-flight coordination for mutations of the same ID.
  - 2026-09-02 checkpoint: loose and packed reads/writes, loose removal,
    filesystem-capacity inspection, and bounded pack compaction no longer hold
    the store index mutex across payload I/O, hashing, AES-GCM or filesystem
    waits. Packed appends/compaction use a separate physical stream domain;
    mutations of one immutable ID retain exact per-object single-flight.
    Deterministic blocked-operation tests cover loose write, packed read,
    packed write and compaction. Logical pack-reader leases also remove
    `open(2)` from the index lock while preventing compaction/unlink races.
- [x] Make existing-object reaffirmation an indexed/idempotent fast path. Define
  the separate scrub/integrity-validation policy rather than validating the
  entire payload synchronously on every put.
- [x] Ensure remote put/get handlers do not perform long storage work on
  critical communications threads. Completion, cancellation and deadlines must
  be event driven and bounded.
  Storage validation, retention and deletion are now classified DATA-executor
  work rather than CONTROL-worker work. Their inner physical admission is
  non-blocking so a saturated bounded executor cannot form a cross-node wait
  cycle; callers receive an error and retry/reconcile.
- [x] Propagate work class into store and physical-I/O admission so viewer/control
  work can overtake loader work without starving loader progress.
  Distributed reads/writes, cache promotion, retention validation, repair and
  remote physical handlers now enter the common priority-aware DATA resource
  boundary. The final whole-program audit remains in Phase 7.
- [x] Coalesce durability requirements by physical replica/domain generation as
  they are accumulated rather than retaining one full requirement per extent.
  Each exact replica-set/quorum retains only its non-dominated cumulative
  generation frontier; incomparable alternatives remain separate so the
  original per-object durability formula is not strengthened or weakened.

Exit gate: unrelated object operations run concurrently; duplicate puts do no
payload read/decrypt/rewrite; viewer/control latency remains bounded under
loader saturation; crash durability and integrity tests remain unchanged.

## Phase 2 — indexed FUSE overlay and bounded operation metadata

- [x] Keep the append-only journal as crash authority, but build a compact
  per-inode interval overlay for runtime reads and publication.
- [x] Coalesce compatible adjacent sequential writes in the in-memory overlay
  while retaining sufficient journal sequence provenance for exact recovery and
  acknowledgement.
- [x] Make reads query `O(log n + intersecting ranges)` and copy only immutable
  descriptors needed after dropping the inode lock.
- [x] Bound and charge `DataOp`, checksum and publication-snapshot metadata.
  Backpressure or compact before the bound is exceeded.
  - 2026-09-02 checkpoint: each accepted operation reserves a conservative
    heap charge covering authoritative history, checksum-vector capacity and
    one concurrent publication snapshot. The configurable 64 MiB aggregate
    bound wakes publication and waits on durability/retirement events; recovery
    preserves acknowledged history even if a lowered limit is initially
    exceeded, while blocking new admissions until it drains.
- [x] Rebuild the same compact overlay deterministically from the journal after
  crash/restart and prove truncate/overwrite/rename/unlink semantics.

Exit gate: append-verify work grows linearly with bytes transferred rather than
quadratically with historical write count; pending metadata reaches a stable
bound; restart replay produces the identical visible file.

## Phase 3 — process-wide retained-memory governance

- [ ] Introduce one ownership ledger/budget for actual retained heap across
  loader/viewer payloads, RPC frames, extent tasks, publication cursors,
  operation indexes and metadata/catalogue caches.
- [ ] Charge memory for the complete lifetime of each retained object, including
  queued and retry states; release exactly once on every success, error,
  cancellation, timeout and shutdown path.
- [ ] Reserve non-borrowable control/viewer headroom, permit work-conserving
  borrowing when idle, and preserve a non-zero loader share.
- [ ] Add pressure-triggered shedding only for reconstructible caches. Durable
  and acknowledged work must backpressure rather than disappear.

2026-09-02 checkpoint: the common ledger, priority reserves, recovery
overcommit, event-driven admission and first concrete owner lifetimes are
implemented. FUSE/RPC/publication/object/playback owners are substantially
wired, and concurrent remote object readers now share one charged immutable
buffer rather than copying one complete extent per waiter. Phase 3 remains open
for metadata/catalogue/profile ownership, remaining transient materialisation
buffers, complete-suite verification and the stable-RSS loaded exit gate. See
[the retained-memory checkpoint](2026-09-02-phase-3-retained-memory-checkpoint.md).

Exit gate: deterministic saturation tests and a multi-file loaded run reach a
stable RSS plateau on a 4 GiB node without swap growth, OOM, lost work or viewer
regression.

## Phase 4 — mutation and publication amplification

- [ ] Add a publication commit coordinator which combines ready compatible file
  manifest updates into bounded metadata deltas without weakening per-file WAL,
  durability or atomic visibility.
- [ ] Preserve immediate publication where a viewer requires a newly completed
  file; ordinary ingest may use a short bounded micro-batch.
- [ ] Make namespace batching byte/urgency/dependency aware and retain safe
  singleton boundaries where required.
- [ ] Eliminate repeated active-owner notifications: advance per-inode durable
  high-water marks and enqueue only on an ownership edge.

Exit gate: many completed files require substantially fewer metadata generations
and convergence cycles, with exact crash/restart and dependency-chain tests.

## Phase 5 — incremental metadata representation

- [ ] Replace whole-snapshot mutation copies with persistent/copy-on-write state
  and incremental canonical hashing/encoding, or an equivalent representation
  whose ordinary cost is proportional to changed keys.
- [ ] Carry shared decoded parent/proposed state and exact retention deltas
  through publication so callbacks do not decode the parent again.
- [ ] Define checkpoint/anchor, corruption-validation and rolling-upgrade rules
  before changing wire identity.
- [ ] Keep explicit full checkpoints bounded and below viewer/control priority.

Exit gate: tiny mutations and ordinary sibling reconciliation have bounded
incremental CPU/memory cost independent of total library size; compatibility,
partition and recovery matrices pass.

## Phase 6 — causally safe media-profile lifecycle

- [ ] Maintain immutable-media reference changes incrementally from accepted
  file additions/removals rather than scanning the complete library on every
  metadata notice.
- [ ] Require reconciled causal proof, a stability fence and a grace period
  before deleting the final profile reference.
- [ ] Keep profile computation and GC below loader priority. A profile miss or
  failed job must never block playback negotiation.
- [ ] Coalesce profile/catalogue publication and prevent prune/publication
  oscillation across concurrent branches.

Exit gate: branch lag cannot delete a live profile; metadata notices cause work
proportional to relevant file changes; profile lifecycle remains asynchronous,
deduplicated and restart safe.

## Phase 7 — global priority and diagnostics closeout

- [ ] Audit every executor, mutex, disk/crypto operation, RPC wait, durability
  barrier, metadata mutation, catalogue job and maintenance job for explicit
  work-class propagation and bounded pre-emption points.
- [ ] Replace per-extent debug floods with counters, latency histograms and
  sparse rate-limited examples.
- [ ] Prove `control > viewer >> loader > speculative` end to end, including
  poor network, slow/spinning disk, peer loss, retry and concurrent metadata
  mutation.
- [ ] Preserve configurable 95:5 viewer/loader service under contention,
  work-conserving borrowing and loader non-starvation.

Exit gate: loaded four-node UAT demonstrates responsive control APIs, ordinary
playback start/seek without avoidable loader delay, smooth sustained ingest,
bounded resources, automatic restart/rejoin and a quiet converged idle state.

## Superseded or paused work

- Throughput/backpressure tuning is paused until Phases 1–3 establish truthful
  storage concurrency, overlay cost and retained-memory bounds. Tuning a
  controller against the current amplified publication rate would encode the
  defect into policy.
- Earlier DATA-resource and weighted-loader UAT remains useful evidence for the
  admission layer it tested, but no longer counts as proof of the project laws.
  The live code walk established multiple lower layers which bypass it.
- The proposed shared per-extent executor is absorbed into Phases 1 and 3. It
  must not be implemented as another independently bounded queue.
- Earlier descriptor batching, metadata-generation coalescing, delta-native
  identity and copy-on-write namespace concepts are absorbed into Phases 2, 4
  and 5 rather than pursued as disconnected optimisations.
- Loaded rsync/overnight and throughput UAT is paused. Resume only after the
  relevant deterministic phase gates pass and the service has an enforced
  process-wide memory ceiling.

## Safe continuation boundary

All Macha services were deliberately stopped after node 50 grew from roughly
218 MiB RSS to over 1 GiB in about five minutes while publication completed only
one file. The rsync ended when the FUSE mount disappeared. No code was changed
during the diagnosis. Resume with Phase 0 tests and diagnostics; do not restart
loaded ingest merely to reproduce the already captured unsafe behaviour.

The first local implementation checkpoint is complete and a short guarded UAT
is now useful. See
[the first structural runtime checkpoint](2026-09-02-structural-runtime-first-checkpoint.md).
This does not reopen overnight ingest: Phase 3 process-wide memory governance
and the remaining Phase 1 priority/RPC work are still active.

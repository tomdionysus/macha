# Namespace publication and metadata efficiency

Status: Phase 1 in progress; bounded shared record/snapshot materialization implemented

Last updated: 2026-08-30

## Purpose

Remove the pathological cost of replaying a large POSIX namespace backlog while preserving Macha's durability, ordering, crash-recovery, and distributed-convergence guarantees.

The observed failure mode is a recovered `rm -rf` from node `10.44.1.50`. Each recorded `unlink` or `rmdir` becomes a separate distributed metadata generation. Every generation is then reconstructed, validated, persisted, announced, and reacted to on all three nodes. This consumes approximately one core per node while making slow forward progress and can stall unrelated RPC work.

This plan is intentionally split into independently reviewable phases so work can continue across multiple sessions.

## Non-negotiable invariants

- Every POSIX namespace operation acknowledged by the FUSE frontend remains durably recoverable.
- Operations from one journal retain their sequence order.
- Recovery never silently skips an acknowledged operation.
- A crash at any point may cause idempotent replay, but must not lose or duplicate a visible effect.
- A published metadata head is not authoritative until the configured write floor has durably stored the commit and its acceptance evidence.
- Concurrent metadata branches and policy transitions retain the existing protocol-20 semantics.
- Foreground and health traffic must remain serviceable during namespace recovery.
- Polling is not introduced. Scheduling and convergence remain event-driven.
- Existing data-write batching and recovery concurrency must not regress.

## Diagnosis and baseline

Relevant current paths:

- `src/fuse_frontend.cpp`: namespace admission, recovery queue, single-operation namespace worker, and operation-journal markers.
- `src/filesystem.cpp`: `unlink` and `rmdir` each call one `mutate_delta`; `rmdir` scans all namespace entries.
- `src/metadata_manager.cpp`: every mutation materializes a complete snapshot and executes a distributed publication.
- `src/metadata.cpp`: delta history is anchored every 256 generations; `historical_locked` reconstructs and re-encodes every delta from the anchor.
- `src/net.cpp`: metadata acceptance runs synchronously in the general RPC execution pool.
- `src/cluster.cpp`: every accepted-head change announces a generation and wakes service convergence.
- `src/service.cpp`: metadata and catalogue repair react to those generation events.

Live evidence collected before this plan:

- Replayed commits contained exactly one erased entry per generation.
- The local namespace worker was asleep while the process consumed a core accepting metadata commits authored elsewhere.
- Hot stacks were dominated by `accept_metadata_commit`, `historical_locked`, `encode_snapshot_for_delta`, and `encode_snapshot`.
- Metadata and catalogue repair also reconstructed the same evolving state.
- Node 50 and node 51 were each consuming most of one core; the local node was approximately one full core.
- Node 50 also had unusually high RSS. Treat that as a separate item to measure; it was not proven to be the primary cause.

## Target behaviour

For a durable backlog of 10,000 independent unlinks and a publication batch limit of 256:

- Approximately 40 metadata publications, not 10,000.
- Approximately 40 grouped `published` durability barriers and 40 grouped `done` barriers, not one of each per operation.
- Each accepted commit is materialized at most once per replica in the normal linear-head path.
- Intermediate generation notifications are coalesced to a latest-generation high-water mark.
- Health RPCs remain immediately serviceable and foreground RPC capacity remains available.
- The namespace backlog drains at a rate governed primarily by storage durability and network round trips per batch, not by repeated full-snapshot serialization.

These are architectural targets. CI tests should use deterministic counters and execution gates rather than fragile wall-clock performance assertions.

## Phase 0: measurement seams and characterization

Goal: make duplicated work and batching behaviour observable before changing it.

- [x] Add test-visible diagnostic counters for namespace operations admitted,
  recovered, attempted, published, and confirmed.
- [x] Add test-visible diagnostic counters for operation-journal append groups,
  records appended, and successful durability barriers.
- [x] Add metadata-replica diagnostic counters for historical requests,
  reconstructions, and deltas applied during reconstruction.
- [ ] Add counters for namespace publication batches and operations per batch
  when the batch abstraction is introduced.
- [ ] Add accepted-head persistence counters.
- [ ] Add generation notices received and convergence runs scheduled/completed.
- [ ] Add RPC queue time and handler time summaries by message and frame class.
- [x] Ensure implemented counters do not add production polling or
  high-cardinality logging.
- [x] Add a characterization test proving that the current implementation publishes one generation per recovered namespace operation. Mark it as a baseline expected to change in Phase 2.
- [x] Add a characterization test showing repeated reconstruction of a linear delta chain.
- [ ] Record a reproducible local benchmark recipe, but keep timing thresholds out of the default suite.

Likely tests: `tests/test_filesystem_fuse.cpp`, `tests/test_storage_metadata.cpp`, and `tests/test_rpc_cluster.cpp`.

Exit criteria:

- Tests can count publications, reconstruction work, journal barriers, and convergence scheduling without sampling a live process.

## Phase 1: materialize a metadata head once

Goal: eliminate repeated anchor-to-head replay and full-snapshot encoding for the same hash.

### Design

- [x] Add a bounded materialization cache keyed by commit hash for immutable,
  validated `MetadataRecord` and decoded `MetadataSnapshot` values.
- [x] Seed the cache from the committed/current materialized head on startup.
- [x] Construct normal linear history validation from its cached materialized parent.
- [x] When an uncached chain must be reconstructed, begin at the nearest cached materialized ancestor, apply deltas in memory, and cache validated intermediates and the target.
- [ ] Avoid encoding every intermediate snapshot solely to proceed to the next delta. Encode only where required to validate the historical commit identity; retain validated intermediate materializations when doing so is cheaper than recreating them.
- [x] Reuse the same immutable materialization in:
  - acceptance-policy validation;
  - accepted-head pruning and ancestry handling;
  - `committed()` and snapshot-view paths;
  - metadata repair;
  - catalogue and retention consumers.
- [x] Bound the initial cache to 64 entries. Pin current, committed, and accepted
  heads; evict only reconstructible non-head entries. Byte accounting remains a
  possible refinement if snapshot payload sharing changes.
- [ ] Do reconstruction, decoding, encoding, and hashing outside the main `MetadataReplica` mutex.
- [ ] Use a short lock/CAS-style install step: capture immutable history inputs, calculate outside the lock, reacquire the lock, verify that assumptions still hold, then publish the cached result.
- [ ] Deduplicate concurrent requests for the same hash so only one materialization is computed and waiters share the result.
- [ ] Preserve exact behaviour for merge commits, same-generation siblings, recovery-required state, policy transitions, corrupt history, and cache eviction.

### Tests

- [x] A 200-entry linear delta chain materializes its head with bounded work and a second lookup performs no delta replay.
- [ ] Concurrent requests for one uncached hash perform one materialization.
- [x] Eviction followed by reconstruction returns a byte-identical record payload.
- [x] Existing branch-merge and three-node reconciliation coverage passes with
  cached and startup-uncached materializations.
- [ ] Corrupt deltas are never made valid by the cache.
- [x] Acceptance policy changes and accepted-head pruning retain existing semantics.
- [x] Run the complete default and runtime suites, including
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`.

Exit criteria:

- Normal acceptance of a new linear child does not replay the chain from the 256-generation anchor.
- Repeated consumers of the same head share one immutable materialization.
- Expensive materialization work is not performed while holding the global replica mutex.

## Phase 2: batch namespace publication

Goal: retain per-operation journal durability while publishing an ordered group in one distributed metadata mutation.

### Batch formation

- [ ] Replace the one-operation namespace publication loop with an adaptive ordered batch drain.
- [ ] Start with configurable bounds such as:
  - maximum operations per batch;
  - maximum encoded delta bytes;
  - maximum coalescing delay for an interactive/lightly loaded queue.
- [ ] If only one operation is available, publish it without an unnecessary delay unless a recovery backlog is already known.
- [ ] During recovery or sustained backlog, fill batches immediately up to a bound.
- [ ] Never mix journal epochs or otherwise incompatible publication contexts.
- [ ] Preserve operation sequence numbers and original order inside the batch.

### Ordered application and failure semantics

- [ ] Add a filesystem API that applies a sequence of namespace operations to one mutable snapshot and one `MetadataDelta`.
- [ ] Apply operations in exact journal order so rename/create/unlink dependencies retain POSIX behaviour.
- [ ] Publish the largest valid prefix when an operation fails:
  - commit all preceding valid operations as one batch;
  - leave the failing operation at the head of the durable queue;
  - do not execute later operations past it;
  - retain the current bounded retry/error reporting behaviour for the failing operation.
- [ ] Treat an already-achieved replay effect as success using the existing idempotence rules.
- [ ] Define no-op handling explicitly. A batch containing only already-achieved operations must still advance their journal state without inventing a metadata generation.
- [ ] Ensure one metadata mutation sequence can represent the entire ordered batch while FUSE journal sequence numbers remain individually recoverable.

### Publication and journal completion

- [ ] Publish one metadata commit for the valid batch prefix.
- [ ] Associate the accepted commit hash/generation with all operations in that prefix in memory.
- [ ] Append all `published` markers using the existing multi-record journal append facility and one durability barrier.
- [ ] Confirm the batch against one available snapshot view.
- [ ] Append all confirmed `done` markers with one grouped durability barrier.
- [ ] If only a subset can be confirmed after restart, check effects individually but group the resulting markers.
- [ ] Consider a versioned batch marker or sequence-range marker only if individual marker grouping is insufficient. Prefer no journal-format change for the first implementation.
- [ ] Keep journal compaction/recovery compatible with journals written by the current release.

### Filesystem efficiency

- [ ] Avoid copying/encoding the complete snapshot once per operation inside a batch.
- [ ] Replace the `rmdir` full-map emptiness scan with the existing namespace/directory index or an equivalent indexed child lookup.
- [ ] Accumulate erased-file garbage retirement into the batch delta without changing grace-period semantics.
- [ ] Ensure rename subtree behaviour and path shadowing remain correct across a batch.

### Tests

- [ ] Recover 1,000 independent unlinks and assert publications are bounded by `ceil(operations / batch_limit)` plus explicitly justified boundary cases.
- [ ] Assert grouped journal records use one barrier per marker group, not one per operation.
- [ ] Test unlink children followed by parent `rmdir` in one batch.
- [ ] Test create/rename/unlink dependencies in one batch and across batch boundaries.
- [ ] Inject a deterministic failure at operation N and verify largest-valid-prefix publication.
- [ ] Restart after every durability boundary: admission, distributed commit, published-marker group, confirmation, and done-marker group.
- [ ] Verify no acknowledged operation is lost, reordered, or reported done without its effect.
- [ ] Test an all-idempotent recovery batch and a partially idempotent batch.
- [ ] Test concurrent new FUSE admissions while a recovery batch is publishing.
- [ ] Verify batch encoded-size limits with large renames or metadata entries.

Exit criteria:

- A recovered `rm -rf` produces bounded batches rather than a generation per syscall.
- Crash recovery and failure-prefix tests demonstrate the original durability and ordering contract.

## Phase 3: coalesce generation-driven convergence

Goal: process the newest required state without repeating obsolete intermediate work.

### Design

- [ ] Represent metadata convergence demand as an atomic/latest generation or epoch high-water mark plus an edge-triggered scheduled flag.
- [ ] Receiving a notice advances the high-water mark and signals the worker only when no run is already scheduled.
- [ ] A convergence run reads the latest accepted-head topology, performs necessary work, and checks the high-water mark again before becoming idle.
- [ ] Multiple notices received during one run must result in at most one additional pass over the latest state.
- [ ] Catalogue, retention, and GC consumers should consume a stable immutable snapshot of the latest relevant head, not every intermediate linear generation.
- [ ] Preserve same-generation sibling notifications by including the existing metadata epoch/topology signal, not generation alone.
- [ ] Do not coalesce away a branch-topology change, policy transition, retention deadline, or explicit catalogue rescan request.
- [ ] Prevent acceptance rebroadcast storms: installing already-known acceptance evidence must not announce again, and followers should not make every intermediate linear head a service-wide event when catching up to a newer head.
- [ ] Allow a lagging third replica to import required history and accept the newest valid linear head without running full service convergence for every ancestor.

### Tests

- [ ] Deliver hundreds of increasing generation notices while a convergence run is gated; assert bounded run count and final high-water processing.
- [ ] Deliver a same-generation sibling notice and verify reconciliation still occurs.
- [ ] Verify catalogue search/artwork/GC sees the final state after a coalesced burst.
- [ ] Verify retention deadlines and garbage grace are calculated from the correct accepted state.
- [ ] Verify no busy loop appears when notices stop or peers disconnect.

Exit criteria:

- A linear burst causes constant or near-constant convergence passes per burst rather than one pass per generation.
- Event-driven idle behaviour is preserved.

## Phase 4: isolate long metadata work from critical RPC service

Goal: prevent metadata validation and durability from occupying shared RPC execution capacity or blocking unrelated work through long lock holds.

### Design

- [ ] Introduce a dedicated, bounded metadata executor for history import, commit storage, and acceptance.
- [ ] Keep wire reading, frame assembly, basic decoding/size checks, ping, and membership independent of that executor.
- [ ] Permit an RPC request to enqueue metadata work and complete its reply asynchronously after the job finishes.
- [ ] Bound the queue by jobs and bytes. Apply explicit backpressure rather than unbounded memory growth.
- [ ] Coalesce or pipeline consecutive linear commits from one peer where correctness permits.
- [ ] Maintain separate foreground capacity. Metadata recovery must not consume the workers reserved for foreground object or filesystem operations.
- [ ] Ensure cancellation/session retirement safely detaches the reply without cancelling durability work that has already crossed an irreversible boundary.
- [ ] Ensure stopping drains or safely abandons executor jobs according to their durability state.
- [ ] Remove filesystem I/O, fsync, snapshot reconstruction, and large encoding from global metadata critical sections.
- [ ] Retain the existing fast-control worker isolation and document which RPC messages are allowed to execute there.

### Tests

- [ ] Gate a deliberately slow metadata acceptance and prove ping/membership complete independently.
- [ ] Prove foreground work can begin while all permitted background metadata slots are occupied.
- [ ] Verify bounded-queue backpressure and recovery after capacity becomes available.
- [ ] Disconnect a peer while an acceptance job is queued, running before durability, and running after durability.
- [ ] Stop the service with queued jobs and verify clean shutdown and recoverable state.
- [ ] Use deterministic executor gates rather than wall-clock timeout assertions.

Exit criteria:

- No RPC execution lane intended for health or foreground responsiveness performs long metadata reconstruction or durability work.
- A namespace-recovery storm cannot exhaust all foreground service capacity.

## Phase 5: integrated validation and operational proof

- [ ] Run the complete default suite via the repository's documented test command.
- [ ] Run relevant heavy FUSE recovery and multi-node RPC tests.
- [ ] Run `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` explicitly and report it explicitly; do not infer its result from another suite.
- [ ] Exercise a three-node recovery backlog analogous to the original node-50 `rm -rf`.
- [ ] Compare before/after counters:
  - operations per metadata generation;
  - materializations and applied deltas per accepted head;
  - journal durability barriers;
  - convergence runs;
  - RPC queue/handler latency by class;
  - CPU and RSS per node;
  - deletions completed per second.
- [ ] Confirm the cluster becomes quiet after the backlog drains: no maintenance spin, no repeated notices, and no unexplained metadata RPC traffic.
- [ ] Confirm physical-object GC remains deliberately rate-limited and distinct from namespace publication completion.
- [ ] Update `docs/durability.md`, `docs/metadata.md`, and `docs/operations.md` with the final batching and recovery semantics.

Exit criteria:

- The three-node reproduction drains in bounded batches, foreground RPCs remain responsive, and all nodes return to event-driven idle.

## Deferred protocol-level option

Do not begin here unless the protocol-compatible phases above remain insufficient.

- [ ] Consider delta-native commit identity: hash the parent identity plus canonical delta and commit to a state-tree root, rather than requiring a serialized full namespace payload for every commit hash.
- [ ] Consider a persistent or copy-on-write namespace tree with incremental subtree hashing.
- [ ] Define rolling-upgrade compatibility and protocol-version negotiation.
- [ ] Provide checkpoint/anchor migration and independent corruption validation.

This would further reduce full-state serialization, but it is a substantially larger compatibility and correctness project.

## Decisions to record during implementation

Add short dated notes here rather than leaving important choices only in chat history.

- Batch limits and whether they are configurable or internal constants.
- Exact interactive coalescing-delay rule.
- Whether grouped individual journal markers are sufficient or a new batch marker is needed.
- Materialization cache ownership, byte accounting, eviction, and pinning rules.
- How concurrent materialization work is deduplicated without deadlock.
- Which generation/epoch events may be coalesced and which require distinct processing.
- Metadata executor queue bounds, priority policy, shutdown semantics, and backpressure error behaviour.
- Any on-disk or wire-format change and its compatibility story.

## Risks and review focus

- Batching can accidentally make later operations visible if an earlier operation fails; prefix semantics must be explicit and tested.
- Grouped journal completion can falsely retire operations after a partial failure; crash injection is mandatory.
- Cached materializations can become false authority during recovery or accepted-head divergence; cache presence must never imply acceptance.
- Moving work outside the metadata lock introduces stale-input races; installation must revalidate immutable inputs/topology.
- Notice coalescing can lose same-generation sibling changes if it tracks generation without epoch/topology.
- Executor isolation can become an unbounded queue disguised as responsiveness; bounded admission is required.
- Faster namespace deletion can produce a larger garbage-retirement burst. Object GC must remain paced and foreground-aware.
- Avoid solving throughput by increasing concurrency around a single contended metadata mutex.

## Suggested session order

Each future session should:

1. Read this document and the latest entries in **Session log** below.
2. Inspect the working tree without using Git unless the user explicitly reauthorises Git in that session.
3. Select one unchecked, dependency-ready group of tasks.
4. State the exact scope before editing.
5. Implement and run the narrowest relevant tests first.
6. Run broader tests only when the change is ready and credits/time permit.
7. Update checkboxes, decisions, test results, and the session log before stopping.
8. Never claim a suite passed unless that exact command completed successfully in the current run and its result was observed.

## Session log

### 2026-08-30 — planning

- Confirmed the live cost is replicated metadata reconstruction/acceptance rather than physical pathname deletion.
- Defined protocol-compatible work in five phases: measurement, materialization reuse, namespace batching, event coalescing, and RPC isolation, followed by integrated validation.
- No production code changed and no tests were run while creating this plan.

### 2026-08-30 — Phase 0 diagnostic foundation

- Added monotonic FUSE status counters for namespace admission/recovery/publication
  and successful operation-journal append durability work.
- Added read-only metadata-replica diagnostics for historical reconstruction and
  delta application counts.
- Added deterministic characterization tests for one-publication-per-recovered-op
  and repeated whole-chain materialization.
- Full build passed.
- All 27 `storage_metadata` tests passed.
- All 34 `filesystem_fuse` tests passed.
- Detailed implementation and continuation notes:
  `TODO/2026-08-30-phase-0-diagnostic-foundation.md`.

### 2026-08-30 — Phase 1 bounded record materialization

- Added a bounded LRU-style cache of validated historical `MetadataRecord`
  materializations, keyed by immutable commit hash.
- Current, committed, and accepted heads are pinned; reconstructible non-head
  entries are eviction candidates.
- History lookup now starts from the nearest cached ancestor and retains each
  validated intermediate, eliminating repeated anchor-to-head replay on the hot
  linear acceptance path.
- History reload and safe history compaction explicitly reset/reseed the cache.
- Added cache hit/miss/eviction/entry diagnostics.
- Updated the delta-chain test: an 80-delta first lookup applies 80 deltas; the
  second applies zero; an evicted old record reconstructs byte-identically.
- Full build passed; `storage_metadata` 27/27, `rpc_cluster` 27/27, and
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` 1/1 passed.
- Detailed implementation and continuation notes:
  `TODO/2026-08-30-phase-1-bounded-record-materialization.md`.

### 2026-08-30 — Phase 1 shared decoded materialization

- Extended each bounded cache value to hold one immutable shared record and its
  decoded snapshot.
- Delta reconstruction now copies/applies from the nearest cached decoded
  snapshot rather than decoding every intermediate encoded payload.
- Acceptance policy, node configuration fencing, metadata-manager head
  selection/merge/repair/retention, and local filesystem snapshot views reuse
  the shared decoded value.
- Added pointer-identity and non-authority assertions; strengthened the chain
  test to 200 deltas.
- With the production changes and shared-snapshot assertions in place, the full
  core suite passed 185/185 and runtime suite passed 3/3. The subsequent
  test-only increase from 80 to 200 deltas passed its focused test 1/1.
- A short three-node UAT is now useful for measuring Phase 1 CPU/RPC improvement;
  it will not yet demonstrate batched deletion throughput.
- Detailed implementation, continuation, and UAT notes:
  `TODO/2026-08-30-phase-1-shared-decoded-materialization.md`.

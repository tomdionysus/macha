# Completed and tested

Last updated: 2026-09-01

This is the completed-work ledger for the current session. An item belongs here only after implementation and its stated verification are complete. Detailed design notes, exact test results, and UAT measurements remain in the linked records.

## Exact metadata reconciliation recovery — 0.22.2

- [x] Reproduced and corrected compact-delta selection for reconciliation which
  reorders an append-ordered garbage/tombstone set. Unrepresentable ordering now
  selects a full immutable record instead of emitting a byte-inexact delta.
- [x] Added one bounded local full-record fallback after compact-body rejection,
  matching remote-replica safety without retrying or loading full history.
- [x] Added regressions for both ordering rejection and local fallback; all
  250/250 core and 3/3 runtime tests passed.
- [x] Deployed complete source as 0.22.2 to all four nodes. The three identical
  Linux builds have SHA-256
  `1fc6c8473a64525da48d6fead630780615f525f22ae136dd2ff9bdf5731144d0`.
- [x] Live generation-2128 branches reconciled without reset or library loss.
  Metadata became writable/validated, ordinary queued publication advanced the
  cluster through generation 2155, and every catalogue API returned the same
  331 items and 374 artwork objects. Linux services remained active with zero
  restarts.

Evidence: [metadata reconciliation recovery](2026-09-01-metadata-reconciliation-recovery.md)

## Metadata-history bounded-memory and delta-reconciliation checkpoint

- [x] Replaced retained decrypted history payloads with a compact authenticated
  on-disk frame index and on-demand reconstruction.
- [x] Added a configurable `128M` default materialisation byte budget, retained
  the secondary 64-entry guard, and exposed history/cache byte diagnostics.
- [x] Made deterministic merges store DLT6 deltas when smaller, including
  merge-parent/conflict replacement, while preserving the immutable full record
  hash and safely supplying missing parents before full fallback.
- [x] Corrected `runtime.rss_bytes` to report current RSS on Linux/macOS and
  moved stale-Macha-mount recovery before mount-path filesystem access.
- [x] Added disk-backed large-history and four-node merge-body regressions.
  The authoritative serial suite passed 246/246 and runtime dependencies passed
  3/3. Live bounded-RSS/restart UAT remains active before loaded ingest resumes.

Evidence: [P0 metadata-history remediation](2026-09-01-metadata-history-memory-remediation.md)

## Bounded shutdown and clean cluster rejoin

- [x] Reproduced the deployed shutdown timeout and captured both maintenance
  owners that could remain blocked after stop: a synchronous outbound control
  RPC and pack compaction waiting for startup storage accounting.
- [x] Made shutdown close pending outbound routes before joining service
  maintenance, permanently reject new outbound calls once cancellation begins,
  and make the accounting wait stop-token-aware.
- [x] Correctly classified libfuse's positive signal return as an intentional
  shutdown rather than a frontend failure, while retaining negative errno and
  watchdog mount loss as fail-closed errors.
- [x] Added regressions for pending RPC cancellation, post-cancellation retry
  rejection, accounting-wait cancellation, and FUSE exit-result semantics.
- [x] Passed the final 245/245 core and 3/3 runtime suites. Live UAT on
  node 50 stopped Macha in 215 ms with systemd `Result=success`, then restarted
  it as the same node at generation 1643; node 51 independently reported it
  online at the same writable generation.

Evidence: [bounded shutdown and rejoin record](2026-09-01-bounded-shutdown-and-rejoin.md)

## Dedicated immutable media-information engine

- [x] Added an event-driven, durably hinted profiling service used by ingest,
  cataloguing, the profile API, and playback; it performs no idle polling.
- [x] Ordered and deduplicated optional scans while keeping every background
  media read in the speculative class below loader work.
- [x] Removed the client-visible playback profiling gate. A stored immutable
  profile feeds the unchanged negotiation path with zero probe reads; a genuine
  miss continues normal viewer-priority media-engine negotiation.
- [x] Coalesced concurrent scans by immutable media ID. Viewer-required
  negotiation cancels/takes over speculative ownership, and successful results
  are asynchronously persisted regardless of which path produced them.
- [x] Pruned profiles only when their immutable media ID has no live namespace
  copy, retaining one shared profile across identical aliases/copies.
- [x] Added separated profile-lookup, fallback-inspection, selection and total
  admission timing diagnostics and corrected the streaming/catalogue docs.
- [x] Passed all 242 core tests and all 3 runtime/libav tests, then built and
  deployed the source on all four nodes. Each node reported ready, writable and
  online at metadata generation 1633.

Evidence: [media-information checkpoint](2026-09-01-media-information-engine.md)

## Manual causal metadata repair and ancestry safety

- [x] Proved all three replicas held accepted generations 1552 and 1516 but no
  retained common ancestor because local history compaction had discarded the
  required bridge.
- [x] Proved generation 1552 strictly causally dominated every mutation in 1516,
  retained the same catalogue root, and contained all 622 older namespace
  entries plus 12 additions.
- [x] Added a dry-run/two-phase offline repair tool which refuses automatic,
  mergeable, concurrent, equal-clock, unstaged or under-witnessed repairs.
- [x] Backed up all three metadata replicas, staged one identical generation
  1553 two-parent record everywhere, then accepted it with nodes 50 and 51 as
  actual durable witnesses. No branch or object was discarded.
- [x] Disabled unsafe automatic history compaction until exact cluster-wide
  accepted-head identity can be proven durably.
- [x] Passed 29/29 storage/metadata tests, 234/234 core tests and 3/3 runtime
  tests. Live UAT converged all nodes, replayed queued operations, and remained
  healthy/writable overnight at generation 1569; later ordinary divergent
  branches reconciled automatically with zero conflicts.

Evidence: [manual causal repair record](2026-09-01-metadata-manual-causal-repair.md)

## Phase 1D.4 partial: transient publication cursor preservation

- [x] Retained the provisional writer, WAL cursor and resumable
  materialisation/rebuild state across retryable backend failures.
- [x] Kept failed pipelined extent payloads at their exact manifest offset and
  retried them in place, preventing both whole-generation replay and manifest
  holes.
- [x] Added a deterministic one-shot failure regression proving one publication
  start, exact one-pass spool reads, atomic visibility and exact final content.
- [x] Preserved the existing pipeline-failure invisibility contract.
- [x] Passed focused tests, filesystem/FUSE 58/58, the complete suite 229/229
  and runtime dependencies 3/3.

Evidence: [transient-failure cursor checkpoint](2026-08-31-fuse-publication-phase-1d-transient-failure-cursor.md)

## Phase 1D.4 partial: pressure-aware retirement selection

- [x] Ranked closed generations above the pressure threshold by releasable
  spool bytes per remaining replay work, with nearest completion as tie-break.
- [x] Preserved ordinary below-pressure FIFO, open-loader fallback, bounded
  quanta, viewer priority and event-driven scheduling.
- [x] Added `data_retirement_priority_selections`, counted only when a worker
  actually reorders closed work.
- [x] Proved a later small closed generation retires ahead of an earlier large
  generation and wakes a blocked writer in the pathological/full-spool test.
- [x] Passed the focused regression, filesystem/FUSE 57/57, runtime 3/3 and the
  final controlled complete suite 228/228.

Evidence: [retirement-selection checkpoint](2026-08-31-fuse-publication-phase-1d-retirement-selection.md)

## Catalogue coalesced-final-state regression correction

- [x] Retained failure-only diagnostics and proved the timed-out cache already
  held the exact final title and artwork at reconciled generation 18 while the
  test incorrectly required equality with writer-side generation 15.
- [x] Accepted newer generations only with exact final content and captured the
  integration run bound at catalogue-repair entry, before zero-grace artwork GC
  can add unrelated metadata events.
- [x] Retained the 10-second timeout, exact final search/artwork/GC assertions,
  and the separate exact one-follow-up ConvergenceDemand state-machine test.
- [x] Passed 10/10 isolated repetitions and the controlled complete suite
  228/228.

Evidence: [retirement-selection checkpoint verification](2026-08-31-fuse-publication-phase-1d-retirement-selection.md#verification)

## Phase 1D.4 partial: publication notification coalescing

- [x] Made one false-to-true spool-drain transition own the all-inode pressure
  sweep instead of rescanning for every blocked FUSE write.
- [x] Kept new durable work event-driven through durability-batch notification;
  no polling or idle owner was introduced.
- [x] Coalesced publication demand at monotonic data/namespace watermarks and
  suppressed unchanged flush/release/pressure notifications before queue work.
- [x] Added a single per-inode enqueue owner across the inode-to-queue lock
  handoff, preventing concurrent duplicate queue ownership.
- [x] Added Status counters for suppressed notifications and pressure sweeps.
- [x] Proved 1,002 notifications become two effective requests and 1,000
  suppressed duplicates; all three pressure regressions prove one sweep per
  episode.
- [x] Passed build, filesystem/FUSE 56/56, controlled complete suite 227/227 and
  runtime 3/3.

Evidence: [publication notification checkpoint](2026-08-31-fuse-publication-phase-1d-notification-coalescing.md)

## Phase 1D.2 partial: sparse changed-range reconstruction

- [x] Replaced canonical whole-file materialisation with a sparse overlay of
  sorted, merged changed byte ranges.
- [x] Reused untouched immutable extents without fetching or hashing them and
  reconstructed only touched/boundary extents under resumable loader quanta.
- [x] Preserved append-then-overwrite data, truncate/re-extension zero fill,
  exact final bytes and old-generation visibility until atomic commit.
- [x] Added completed-publication cohort counters for spool/source reads,
  immutable extent reuse and extent puts.
- [x] Added an eight-extent deterministic regression proving two touched extent
  reads, six untouched reuses, two puts and no whole-file materialisation.
- [x] Passed build, filesystem/FUSE 55/55, controlled complete suite 226/226 and
  runtime 3/3. The default 12-worker complete run is accurately retained as
  225/226 due to the resource-sensitive catalogue burst test; it passed alone
  and in the complete four-worker run.

Evidence: [sparse changed-range checkpoint](2026-08-31-fuse-publication-phase-1d-sparse-changed-ranges.md)

## Phase 1D.3 partial: node-wide DATA resource headroom

- [x] Added event-driven byte-bounded admission below RPC scheduling for
  blocking DATA object, local-store and cache work.
- [x] Reserved configurable non-borrowable foreground/read-ahead capacity while
  retaining work-conserving lower-class use of all non-reserved capacity.
- [x] Kept CONTROL completely outside DATA admission and ordered waiting loader
  work ahead of speculative work.
- [x] Covered incoming/outgoing transfer, direct repair placement, ordinary
  local reads and asynchronous fetched-object persistence; impossible
  oversized class work fails instead of waiting forever.
- [x] Added Status counters, configuration validation/documentation, isolated
  saturation/deadline/fairness tests and a real RPC/local-store saturation test.
- [x] Passed the clean complete default suite 225/225 and runtime dependencies
  3/3. The first eight-slot run was accurately retained as 223/225; both
  load-sensitive failures passed isolated and in the clean four-slot rerun.

Evidence: [DATA resource headroom checkpoint](2026-08-31-phase-1d-data-resource-headroom.md)

Deployment/UAT evidence: [three-node loaded UAT](2026-08-31-phase-1d-data-resource-headroom-uat.md)

## Aggregate spool retirement-rate estimator

- [x] Replaced the per-generation throughput EMA with cumulative physically
  retired spool bytes over one shared wall-clock epoch, so concurrent publisher
  completions contribute their aggregate capacity.
- [x] Kept provisional progress and failed reservation rollback out of the
  capacity signal and reset the internal epoch below the 50% burst threshold.
- [x] Preserved event-driven admission, hard capacity/free-space bounds,
  occupancy hysteresis and viewer priority without adding a loop or idle worker.
- [x] Exposed the rate-window numerator and elapsed milliseconds for operational
  audit and added deterministic aggregate-rate plus integrated retirement tests.
- [x] Passed filesystem/FUSE 50/50, foundations 15/15, runtime 3/3 and complete
  default 218/218 suites, including both catalogue regressions.

Evidence: [aggregate retirement-rate checkpoint](2026-08-31-spool-aggregate-retirement-rate.md)

## FUSE publication Phase 4A: bounded within-file extent pipeline

- [x] Added a configurable byte-bounded provisional extent pipeline for durable
  spool publication while leaving ordinary writes synchronous.
- [x] Preserved whole-file atomic visibility and the final aggregate durability
  barrier by merging private extent durability evidence in order.
- [x] Drained admitted work before every fairness/viewer yield so the pipeline
  is the hard bound on already-running loader I/O.
- [x] Added deterministic bound, atomicity and failure-invisibility tests plus
  operational pipeline diagnostics.
- [x] Passed build, new tests 2/2, filesystem/FUSE 50/50 and runtime 3/3. The
  complete run was accurately recorded as 216/217 because the separately
  tracked concurrent catalogue final-state test failed; it passed immediately
  in isolation.
- [x] Passed three-node UAT: 19.5 MiB/s over the complete observation window,
  40.2 MiB/s in a clean loaded interval, pipeline peak exactly two, viewer read
  gating exact, a 4 MiB direct read in 0.37 seconds, protected communications,
  and zero failures/timeouts.

Evidence: [Phase 4A implementation](2026-08-31-fuse-publication-phase-4a-extent-pipeline.md)
and [Phase 4A UAT](2026-08-31-fuse-publication-phase-4a-uat.md)

## FUSE publication Phase 0/1: diagnostics, loader priority and fair quanta

- [x] Added useful-byte, coalescing, publication, quantum/yield, admitted-byte,
  spool-pressure and RPC class/timing diagnostics.
- [x] Separated user-requested loader traffic from crash-recovery provenance and
  placed it below viewer/read-ahead work but above speculative maintenance.
- [x] Removed the false single-publisher cap for open loaders and stopped using
  the legacy recovery-worker value as a cap on restarted user ingest.
- [x] Added extent-aligned resumable publication quanta, a global admitted-byte
  budget, one retained cursor per inode, fair tail requeueing and atomic final
  metadata visibility.
- [x] Added bounded mid-generation yield/resumption machinery and its original
  FUSE-read signalling test. The later aggregate-rate UAT established that the
  FUSE classification and exclusive gate policy were wrong; Phase 1C now
  supersedes that policy while retaining the bounded-yield mechanism.
- [x] Added deterministic coverage for loader RPC ordering, recovery provenance,
  concurrent/open/closed loaders, coalescing, byte bounds, fair small-file
  progress, exact useful bytes, mid-generation viewer pre-emption, resumption,
  crash replay and confirmation.
- [x] Passed the final local build, `filesystem_fuse` 48/48, runtime 3/3 and
  complete 215/215 suites.
- [x] Passed the Phase 0/1 bounded-quantum three-node UAT: two live loader
  generations used fair bounded quanta with no amplification; loader/control
  queue waits remained sub-millisecond; a FUSE read demonstrated the old gate
  and clean resumption; and failures/timeouts remained zero. That FUSE leg is
  not accepted as viewer UAT evidence after the Phase 1C classification decision.

Evidence: [Phase 0/1 checkpoint](2026-08-31-fuse-publication-phase-0-1-checkpoint.md),
[Phase 1A loader priority](2026-08-31-fuse-publication-phase-1a-loader-priority.md),
[Phase 1B fair quanta](2026-08-31-fuse-publication-phase-1b-fair-quanta.md), and
[Phase 1A/1B UAT](2026-08-31-fuse-publication-phase-1ab-uat.md)

## Phase 0: diagnostic foundation

- [x] Added namespace admission, recovery, attempt, publication, confirmation, journal-group, record, and durability-barrier diagnostics.
- [x] Added metadata historical-request, reconstruction, and applied-delta diagnostics.
- [x] Added namespace batch/operation and convergence event/run diagnostics without introducing polling or high-cardinality logging.
- [x] Added characterization tests for one-publication-per-recovered-operation baseline behaviour and repeated linear-chain reconstruction.
- [x] Verified the Phase 0 implementation with its focused suites and recorded the continuation boundary.

Evidence: [Phase 0 diagnostic foundation](2026-08-30-phase-0-diagnostic-foundation.md)

## Phase 1: bounded shared metadata materialization

- [x] Added a bounded cache for immutable validated records and decoded snapshots, keyed by commit hash and seeded from materialized heads.
- [x] Reconstructed uncached chains from their nearest cached ancestor and retained validated intermediates.
- [x] Reused shared immutable materializations across policy validation, pruning/ancestry, committed and snapshot views, repair, catalogue, and retention consumers.
- [x] Bounded the initial cache to 64 entries while protecting current, committed, and accepted heads.
- [x] Added coverage for a 200-entry linear chain, zero replay on repeat lookup, eviction/reconstruction identity, branches, reconciliation, policy changes, and accepted-head pruning.
- [x] Ran the complete default/runtime suites for this slice, including the catalogue search/artwork/GC regression.
- [x] Performed a three-node UAT: the cluster converged, remained responsive, and returned to idle without sustained metadata work.

Evidence: [bounded record materialization](2026-08-30-phase-1-bounded-record-materialization.md), [shared decoded materialization](2026-08-30-phase-1-shared-decoded-materialization.md), and [Phase 1 three-node UAT](2026-08-30-phase-1-three-node-uat.md)

## Phase 1: materialization follow-up closeout

- [x] Audited every remaining intermediate encode and confirmed it is required
  to validate the current full-snapshot successor hash, including exact legacy
  delta-version encodings; removing it requires the deferred identity redesign.
- [x] Added a corrupt-delta regression proving a cached parent cannot validate
  a false successor, rejection cannot poison the cache, and a corrupt duplicate
  cannot displace a valid cached target.
- [x] Completed the merge, same-generation sibling, recovery authority, policy
  transition, corruption, and eviction audit against explicit deterministic
  tests.
- [x] Passed the new regression, `storage_metadata` 28/28, and the live
  same-generation sibling reconciliation test.
- [x] Passed the expanded complete default suite 205/205 and runtime
  dependencies 3/3, including both catalogue regressions.

Evidence: [Phase 1 materialization closeout](2026-08-30-phase-1-materialization-closeout.md)

## Phase 2: bounded namespace publication and durability

- [x] Replaced one-operation publication with bounded ordered, event-driven batch draining governed by configurable operation-count and encoded-byte limits.
- [x] Added ordered batch application to one mutable snapshot and metadata delta, including largest-valid-prefix failure handling and idempotent/no-op recovery.
- [x] Grouped `published` and `done` journal markers into one durability barrier per group while preserving the existing crash-compatible journal format.
- [x] Batched confirmation-safe operation families, retained unsafe mixed operations and rename as boundaries, and preserved sequence order and journal epochs.
- [x] Avoided per-operation full-snapshot work, replaced `rmdir` full-map scans with descendant lookup, and retained garbage-grace semantics.
- [x] Proved 1,000 recovered unlinks publish in four batches at a 256-operation limit rather than 1,000 generations.
- [x] Added deterministic coverage for count/byte boundaries, oversized progress, unlink-plus-parent-rmdir, idempotence, partial idempotence, semantic failure prefixes, crash prefixes, and concurrent live admission during recovery publication.
- [x] Verified partial grouped `published` and `done` crash images recover without loss, duplication, or unnecessary publication.
- [x] Ran the complete suite after durability closeout: default 194/194 and runtime-dependency 3/3, including the explicit catalogue regression.
- [x] Performed a three-node deletion UAT: 1,001 deletion operations used five publications, completed in 0.77 seconds, kept status responsive, and returned all nodes to idle.

Evidence: [Phase 2 namespace batching](2026-08-30-phase-2-namespace-batching.md), [durability closeout](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md), and [Phase 2 three-node deletion UAT](2026-08-30-phase-2-three-node-deletion-uat.md)

## Phase 3: event-driven convergence foundation

- [x] Added `ConvergenceDemand`, using a semantic epoch, latest-generation diagnostic high-water mark, and edge-triggered scheduled state.
- [x] Coalesced event bursts so events arriving during a run schedule at most one follow-up pass over the newest state.
- [x] Kept metadata/topology signals distinct from storage-only maintenance and preserved same-generation sibling/topology events.
- [x] Deferred catalogue convergence when newer metadata arrives during repair so it does not consume an obsolete intermediate view.
- [x] Added scheduler-level burst coverage proving hundreds of notices produce two runs and process the final high-water state.
- [x] Added an integrated service-level gated burst around real metadata repair. A peer publishes exactly 33 generations while the follower's repair owner is held after claiming its run; the held burst schedules no extra runs, release produces exactly one latest-state follow-up, and the follower installs the final generation and namespace.
- [x] Corrected real same-generation sibling signaling. Explicit metadata notices at the current numeric generation now advance the semantic epoch and wake convergence because they represent an accepted-head topology change; stale lower-generation notices and unchanged membership heartbeats remain non-events.
- [x] Added a two-service gated sibling test that creates distinct accepted children of one parent at the same generation, proves the remote notice advances demand without a generation increase, and verifies automatic reconciliation preserves both namespace changes in one accepted descendant.
- [x] Added an integrated catalogue burst test. While follower metadata repair is gated, a peer publishes intermediate title and poster replacements; catalogue repair performs no intermediate run, then runs exactly once against the final metadata state, exposes only the final search result and artwork bytes, and reclaims every superseded artwork object.
- [x] Corrected event-driven garbage-grace scheduling. After evaluating an immature final tombstone set, maintenance now arms one steady-clock wake at the earliest recorded wall-clock retirement expiry instead of becoming indefinitely quiescent or introducing a polling cadence.
- [x] Added a gated coalesced-delete test proving bytes survive immediately before their 750 ms grace deadline, are reclaimed after it without an unrelated event, tombstones are removed, cleanup convergence remains bounded, and the scheduler returns to a parked state.
- [x] Removed the redundant unconditional metadata announcement at the end of local publication. `NodeRuntime::accept_metadata_commit` is now the single owner of accepted-head change detection and notification.
- [x] Added a monotonic announcement diagnostic and proved 33 real publications emit exactly 33 announcements, while re-installing identical acceptance evidence emits neither a local event nor a remote rebroadcast.
- [x] Added a persisted three-replica catch-up test. A third voter stops at a shared base, two surviving voters publish 64 linear generations at W=2, and the restarted voter imports the required history, accepts the sole newest head, exposes the final namespace, and settles in at most four service convergence runs rather than one per ancestor.
- [x] Verified disconnected maintenance sleeps until a peer event and retained the catalogue search/artwork/GC regression.
- [x] Performed a three-node idle UAT at generation 1375. All three nodes remained healthy/writable with stable RPC connections, all convergence-related workers were parked, and post-torrent-pause instantaneous process CPU was 0.0% on every node.
- [x] Isolated the local pre-pause CPU use to libtorrent peer/UTP work rather than metadata, catalogue, hydration, FUSE, HTTP, or RPC work.
- [x] Performed a three-node active-burst UAT using 81 paced creates followed by 81 paced deletes. Each half used 20 metadata publications, all three nodes agreed on both the 80-entry intermediate namespace and the final empty state, status remained responsive during active work, RPC connections did not churn, and every node returned immediately to the previously proven idle state.

Evidence: [Phase 3 scheduler checkpoint](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md), [Phase 3 three-node idle UAT](2026-08-30-phase-3-three-node-idle-uat.md), and [Phase 3 three-node active-burst UAT](2026-08-30-phase-3-three-node-active-burst-uat.md)

Integrated burst verification: `rpc_cluster/test_service_metadata_repair_coalesces_real_generation_burst` passed six isolated executions; the complete `rpc_cluster` suite passed 28/28, `invariants` passed 36/36, `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` passed, and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed.

Same-generation sibling verification: `rpc_cluster/test_service_same_generation_sibling_notice_triggers_reconciliation` first reproduced the missing event, then passed eight consecutive isolated executions after the fix. The burst test passed another eight consecutive executions; the complete `rpc_cluster` suite passed 29/29, `invariants` passed 36/36, and the explicit inactivity and catalogue regressions passed.

Catalogue burst verification: `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` passed six isolated executions; the complete `hydration_catalogue` suite passed 23/23, and the real repair burst, same-generation sibling, and scheduler primitive regressions all passed explicitly.

Retention/grace verification: the new test first reproduced objects remaining indefinitely after grace, then `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace` passed five consecutive isolated executions after the fix. The complete `filesystem_fuse` suite passed 43/43, and both catalogue tests plus all three convergence regressions passed explicitly.

Acceptance rebroadcast verification: the strengthened real-repair burst first reproduced duplicate publication announcements, then passed eight consecutive isolated executions after the fix. The repeated-evidence sibling test passed five consecutive executions. The complete `rpc_cluster` suite passed 29/29 and `invariants` passed 36/36; the catalogue burst and exact garbage-grace tests also passed explicitly.

Lagging-replica verification: `rpc_cluster/test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` passed six isolated executions. The expanded complete `rpc_cluster` suite passed 30/30.

Phase 3 repository-wide checkpoint: `sh run-tests.sh build` passed the complete default suite 199/199 and runtime-dependency suite 3/3. The run explicitly included `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`, the real burst and sibling tests, the exact garbage-grace test, and the lagging-third catch-up test.

## Phase 4: bounded metadata RPC executor

- [x] Routed history import, commit storage, and acceptance RPCs to a dedicated executor rather than ordinary control or data workers.
- [x] Kept socket reading, frame assembly, queue admission, ping, membership, ordinary control handlers, and foreground DATA work independent of metadata execution.
- [x] Preserved asynchronous RPC completion: executor owners finish work and queue replies without occupying the session reader.
- [x] Bounded pending metadata work by both job count and payload bytes, returning an immediate in-band error on overload without closing the peer session.
- [x] Preserved per-peer FIFO across history/store/accept operations while permitting explicitly configured workers to execute different peers concurrently.
- [x] Defined and tested lifecycle boundaries: queued work is cancellable; running work owns its durability boundary and survives reply-route disconnect; shutdown completes a running owner and abandons queued work whose session has closed.
- [x] Kept metadata recovery from consuming fast-control, ordinary-control, or foreground DATA capacity.
- [x] Added deterministic gated tests for executor isolation, count/byte backpressure, session survival, per-peer order, cross-peer concurrency, cancellation, disconnect, and shutdown.
- [x] Set the production default to one metadata owner, 64 queued jobs, and 256 MiB of queued payload. This avoids multiplying idle workers while retaining bounded multi-worker support and per-peer FIFO for explicit configurations.
- [x] Replaced stop-and-wait history transfer with a dependency-first window of at most eight asynchronous uploads; sender diagnostics and the lagging-third test prove more than one and no more than eight requests are in flight.
- [x] Moved history-entry reconstruction, delta application, snapshot encoding/hash validation, encryption, and durable history append out of the global replica-state critical section. A separate durability owner preserves append order and a short state lock installs the validated immutable entry afterward.
- [x] Added single-flight off-lock materialization. Eight concurrent readers of a cold 200-delta chain now share exactly one reconstruction and install through a validated short lock step.
- [x] Prewarmed acceptance-policy materializations outside the replica lock and moved accepted-head public reconstruction onto the off-lock path.
- [x] Documented the closed fast-control allow-list (`ping` and `members` on CONTROL frames only), its handler constraints, and the proof required before extending it.
- [x] Fixed the event-driven follow-up edge exposed by the complete suite: a coalesced convergence run which leaves one follow-up pending now continues immediately without requiring an unrelated external wake, while disconnected settled maintenance remains parked.
- [x] Performed the Phase 4 three-node recovery UAT: a paused third replica missed a 65-operation workload, the surviving pair remained writable and responsive, the resumed replica converged to the identical 64-entry namespace, and all nodes returned to sleeping idle at the same generation without RPC churn.

Evidence: [Phase 4 bounded metadata RPC executor](2026-08-30-phase-4-bounded-metadata-rpc-executor.md) and [Phase 4 three-node recovery UAT](2026-08-30-phase-4-three-node-recovery-uat.md)

Verification: the 27-test `storage_metadata` suite passed; the strengthened cold-chain test proved one reconstruction for eight concurrent callers; and the lagging-third test proved a bounded transfer peak in `(1, 8]`. The first repository-wide run correctly exposed the lost follow-up wake in `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`; after correcting that scheduler edge, the catalogue burst and disconnected-idle regressions passed explicitly. The final repository-wide run passed the default suite 203/203 plus runtime dependencies 3/3, including both catalogue burst/search/artwork/GC regressions and the lagging-third recovery test.

## Phase 5: bounded operational diagnostics

- [x] Added accepted-head persistence write, byte, and failure counters with
  success recorded only after durable replacement completes.
- [x] Added fixed-size atomic RPC execution timing summaries by message and
  frame class, including count and total/max queue and handler microseconds.
- [x] Exposed local aggregates through Status without a sampler, polling loop,
  per-request history, metadata mutation, or gossip expansion.
- [x] Added deterministic metadata, RPC isolation/backpressure, and Status API
  assertions for the new diagnostics.
- [x] Documented bounded FUSE recovery batching, shared metadata
  materialization, metadata RPC isolation, and operational interpretation of
  the new counters.
- [x] Observed and retained one initial catalogue-burst suite timeout; the
  immediate isolated run passed, and a second complete run passed 203/203.
  Runtime dependencies passed 3/3.
- [x] After formatting and rebuilding, explicitly passed the 1,000-operation
  FUSE recovery test, lagging-third multi-node RPC recovery test, and catalogue
  sync/search/artwork/GC regression.
- [x] Verified the final handler-only timing implementation with the complete
  `rpc_cluster` group, 34/34.

Evidence: [Phase 5 operational diagnostics](2026-08-30-phase-5-operational-diagnostics.md)

## Phase 5: three-node operational diagnostics UAT

- [x] Paused one deployed replica, admitted 257 directory operations through
  MachaDFS, resumed the same process, verified identical 256-child views, and
  removed the exact fixture with all three nodes active.
- [x] Measured 12 publications for 514 create/delete operations, zero rejected
  metadata jobs, zero accepted-head persistence failures, and bounded RPC queue
  separation from CONTROL work.
- [x] Proved the lagging replica imported required history and persisted one
  final create head rather than every intermediate accepted head, with no new
  reconstruction or delta replay on any node.
- [x] Verified stable generation/counters/RSS, two canonical connections with
  zero churn, direct Status responsiveness, and fully sleeping processes after
  drain.
- [x] Preserved physical DATA GC and repeated-burst RSS as explicit remaining
  work rather than over-claiming them from a directory-only fixture.

Evidence: [Phase 5 operational diagnostics UAT](2026-08-30-phase-5-operational-diagnostics-uat.md)

## Phase 5: journal and convergence Status diagnostics

- [x] Exposed existing namespace publication and operation-journal append/barrier
  totals through an O(1), lock-free FUSE diagnostic snapshot.
- [x] Exposed convergence event/run/epoch totals and scheduled state from the
  existing edge-triggered demand object.
- [x] Kept startup and non-mounted nodes explicit through `available`, and used
  weak frontend ownership so Status cannot extend mount lifetime.
- [x] Proved exact live-path journal semantics: inode descriptor admission,
  operation admission, publication, and completion are four durable append
  groups; recovered operations retain the existing two-group expectation.
- [x] Passed the focused production-shaped API test, `filesystem_fuse` 44/44,
  `invariants` 36/36, the complete default suite 204/204, and runtime 3/3.

Evidence: [Phase 5 journal and convergence Status diagnostics](2026-08-30-phase-5-journal-convergence-status.md)

## Phase 5: journal and convergence Status UAT

- [x] Exercised 65 creates followed by 65 removals on the deployed three-node
  cluster; each half completed in three publications and under 0.3 seconds.
- [x] Accounted exactly for 520 journal records and 272 durability barriers:
  two admission barriers per operation plus grouped publication and completion
  barriers per metadata batch.
- [x] Measured 130 confirmed namespace operations in six publications, or 21.7
  operations per publication, with no retries, failures, timeouts, rejected
  metadata work, or residual queue.
- [x] Observed 23 convergence events on every node, with 8 local and 12 remote
  completed runs; all epochs drained, scheduling parked, and all nodes agreed
  at generation 1439.
- [x] Verified the fixture was absent, direct Status latency was 1-12 ms, and
  delayed process CPU returned to approximately 0.21%, 0.12%, and 0.07%.

Evidence: [Phase 5 journal and convergence Status UAT](2026-08-30-phase-5-journal-convergence-uat.md)

## Phase 5: physical-object GC separation proof

- [x] Confirmed namespace deletion completion does not wait for physical DATA
  removal; metadata publication and grouped journal confirmation remain the
  foreground durability boundary.
- [x] Confirmed physical GC uses a resumable cursor capped at 64 examined
  objects per Service slice and yields immediately to foreground playback or
  mounted-filesystem activity.
- [x] Confirmed destructive reclamation remains fenced by cluster reachability,
  stable/current metadata, catalogue liveness, retention claims, and grace.
- [x] Strengthened deterministic coverage to require a multi-slice GC pass, and
  retained the exact-deadline test proving bytes survive namespace deletion
  until grace before event-driven reclamation.
- [x] Documented the operational distinction and bounded cursor semantics in
  the durability and operations guides.

Evidence: [Phase 5 physical-object GC separation proof](2026-08-30-phase-5-physical-object-gc-proof.md)

## Plan-ledger reconciliation

- [x] Removed stale active entries for off-lock materialization, validated
  short-lock installation, concurrent single-flight deduplication, and its
  deterministic eight-reader proof; these were completed and verified in
  Phase 4.
- [x] Confirmed the final batching/recovery, convergence, RPC-isolation, and
  physical-GC semantics are recorded across `docs/durability.md`,
  `docs/metadata.md`, and `docs/operations.md`.

## Status disk-usage availability correction

- [x] Reproduced the alternating plausible/zero presentation from raw Status
  responses on all three live nodes and located the ambiguity in the API rather
  than the client.
- [x] Made unavailable per-node storage usage/free, cache measurements, and
  online-backend count structurally null instead of fabricated-looking zeros.
- [x] Made incomplete cluster storage/cache aggregates unavailable rather than
  summing missing observations as empty disks.
- [x] Preserved genuine measured zero usage as numeric with `available: true`.
- [x] Added deterministic regression coverage and passed the focused test,
  `invariants` 35/35, and the FUSE/convergence Status integration test.
- [x] Passed the complete default suite 204/204 and runtime dependencies 3/3,
  including the explicit catalogue search/artwork/GC regression.

Evidence: [Status disk-usage availability correction](2026-08-30-status-disk-usage-availability.md)

## Combined Status and repeated-burst UAT

- [x] Verified the unavailable-versus-zero disk contract before and after work
  on every deployed Status endpoint: live self measurements remained numeric,
  while unavailable remote usage, free space, cache measurements, and online
  backend count remained null.
- [x] Completed 2,064 create/remove operations in 48 publications, advancing
  all nodes from generation 1439 to 1487 with exact journal accounting, no
  failure or timeout, and no residual queue.
- [x] Filled every materialization cache to its 64-entry bound. All 3,960 new
  requests were hits; reconstructions, misses, and applied-delta totals did not
  increase, and bounded evictions occurred on every node.
- [x] Verified all convergence epochs drained, scheduling parked, exact UAT
  fixtures were absent, and delayed CPU returned to approximately 0.260%,
  0.142%, and 0.066%.
- [x] Identified that `runtime.rss_bytes` is lifetime-peak `ru_maxrss`, not
  current RSS. The memory-ceiling claim was therefore kept active pending a
  correctly defined metric and repeated post-cap observations.

Evidence: [Combined Status availability and repeated-burst UAT](2026-08-30-combined-status-rss-uat.md)

## Cluster Status telemetry aggregation correction

- [x] Found that authenticated telemetry notifications were transmitted with
  request ID zero but silently ignored by both RPC notification receive paths.
- [x] Routed telemetry notifications from both canonical connection directions
  into the existing bounded speculative executor, keeping decode/store work off
  critical socket-reader threads.
- [x] Added an edge-triggered telemetry wake on authenticated peer observation,
  so connection formation disseminates a coalesced current sample without
  waiting for the periodic local sampler.
- [x] Proved that one two-node Status request contains the connected peer's live
  numeric storage/cache/backend telemetry and a complete online aggregate; no
  client fan-out or HTTP-time network call is required.
- [x] Passed both focused regressions, `rpc_cluster` 35/35, and `invariants`
  36/36. The repository-wide run was accurately retained as 206/207 because
  the tracked catalogue-burst test timed out under suite load; it passed an
  immediate isolated rerun in 0.890 seconds, and runtime passed 3/3. A second
  complete run at four-way concurrency again finished 206/207 with the same
  test timing out at 10.415 seconds.

Evidence: [Cluster Status telemetry aggregation correction](2026-08-30-cluster-status-telemetry-aggregation.md)

## Cluster Status telemetry aggregation UAT

- [x] Queried each of the three deployed Status endpoints independently and
  confirmed that every single response contained all three connected nodes.
- [x] Verified every node was live with numeric storage/cache/backend telemetry
  and that all endpoints returned identical available online/known aggregates.
- [x] Verified the cluster was healthy and writable at generation 1487, all
  convergence demand was drained, and all metadata executor queues were empty.
- [x] Confirmed deployed telemetry notifications were executing under the
  speculative RPC class in both canonical route distributions.

Evidence: [Cluster Status telemetry aggregation UAT](2026-08-30-cluster-status-telemetry-aggregation-uat.md)

## Linux systemd install and uninstall targets

- [x] Added a Linux-default systemd install with binary, unit, configuration,
  example, and documentation paths derived consistently from CMake settings.
- [x] Added first-install-only creation of `/etc/macha/macha.yaml`; upgrades
  preserve operator changes and clearly print the operational config path.
- [x] Added a manifest-based `make uninstall` target which removes managed
  artifacts while deliberately preserving configuration, keys, state, cache,
  spool, mounts, and media data.
- [x] Replaced the legacy EnvironmentFile indirection with a direct generated
  `ExecStart` and documented service enable/disable/reload steps.
- [x] Verified a staged `/usr` install, both generated unit paths, complete
  uninstall, configuration preservation, and checksum-stable reinstall.
- [x] Corrected Debian multiarch systemd installation: cached
  `lib/<architecture>/systemd/system` defaults migrate to
  `lib/systemd/system`, while custom paths remain configurable; also removed
  the generated CMake CMP0012 warning.

Evidence: [Linux systemd install and uninstall targets](2026-08-30-linux-systemd-install.md)

## FUSE spool rate backpressure — first checkpoint

- [x] Retained the safe 16 GiB default while documenting and testing
  `fuse.max_spool_bytes` and `fuse.spool_reserve_free` as configurable policy.
- [x] Replaced logical-capacity `ENOSPC` with event-driven blocking admission;
  only an impossible single request or the independent physical reserve can
  fail for capacity.
- [x] Start publication from spool pressure even while a writer remains open,
  and pace admission from measured end-to-end publication throughput: local
  burst below 50%, progressive slowdown, and publish-rate admission by 90%.
- [x] Added O(1) spool occupancy/rate/wait diagnostics to Status.
- [x] Added focused drain-recovery and stalled-publisher regressions. The FUSE
  group passed 45/45, the complete backend suite passed 211/211, and runtime
  dependencies passed 3/3.

Evidence: [FUSE spool rate backpressure](2026-08-30-fuse-spool-rate-backpressure.md)

## Version 0.21.0

- [x] Bumped the project minor version from 0.20.0 to 0.21.0 across CMake,
  generated server metadata, the release heading, and shipped configuration and
  legacy systemd-wrapper examples.
- [x] Reconfigured and rebuilt every target successfully; generated
  `kServerVersion` is `0.21.0`, and runtime dependencies passed 3/3.

## Linux miniupnpc API 18 compatibility

- [x] Removed the compile-time dependency on `UPNP_CONNECTED_IGD` and
  `UPNP_PRIVATEIP_IGD`, which were added after miniupnpc first shipped API 18
  without changing its API-version number.
- [x] Added a version-aware compatibility classifier for the documented
  `UPNP_GetValidIGD()` ABI values, retaining private-WAN support for API 18+
  and rejecting disconnected/unknown devices.
- [x] Rebuilt successfully with UPnP enabled; the focused compatibility test
  passed and the complete `foundations` group passed 13/13.

## Weighted loader cold-setup accounting

- [x] Separated loader admission/activity tracking from the start of useful
  weighted service, so cold distributed-writer setup cannot consume the loader
  slice before producing work or manufacture a ratio-amplified cooldown.
- [x] Added a deterministic scheduler regression proving that a ten-second cold
  setup is excluded from the 25 ms service slice and 475 ms cooldown at the
  default 95:5 weighting.
- [x] Built and passed the scheduler regression on macOS and natively on both
  Linux nodes 50 and 51, together with all three spool-pressure regressions.

Evidence: [Spool progress-bootstrap admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md)

## Spool progress-bootstrap admission

- [x] Removed the zero-rate dead zone above 50% spool occupancy by granting
  bounded one-for-one admission credit only after useful partial publication
  has successfully drained.
- [x] Preserved the configured hard limit, physical reserve, whole-file atomic
  visibility and event-driven wakeup contract; zero-byte or failed work grants
  no capacity.
- [x] Passed the focused regression and all spool-pressure tests on macOS and
  both Linux nodes, then deployed the identical binary to nodes 50 and 51.
- [x] Passed loaded UAT with rsync and real playback: rsync accepted roughly
  506 MB while publication replayed 441 MB in 30 seconds above the soft
  threshold, with bounded waits, no freeze, no ENOSPC, no backend failure and a
  healthy 3/3 cluster.

Evidence: [Spool progress-bootstrap admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md)

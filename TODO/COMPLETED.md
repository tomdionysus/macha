# Completed and tested

Last updated: 2026-08-30

This is the completed-work ledger for the current session. An item belongs here only after implementation and its stated verification are complete. Detailed design notes, exact test results, and UAT measurements remain in the linked records.

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

Evidence: [Phase 3 scheduler checkpoint](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md) and [Phase 3 three-node idle UAT](2026-08-30-phase-3-three-node-idle-uat.md)

Integrated burst verification: `rpc_cluster/test_service_metadata_repair_coalesces_real_generation_burst` passed six isolated executions; the complete `rpc_cluster` suite passed 28/28, `invariants` passed 36/36, `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` passed, and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed.

Same-generation sibling verification: `rpc_cluster/test_service_same_generation_sibling_notice_triggers_reconciliation` first reproduced the missing event, then passed eight consecutive isolated executions after the fix. The burst test passed another eight consecutive executions; the complete `rpc_cluster` suite passed 29/29, `invariants` passed 36/36, and the explicit inactivity and catalogue regressions passed.

Catalogue burst verification: `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` passed six isolated executions; the complete `hydration_catalogue` suite passed 23/23, and the real repair burst, same-generation sibling, and scheduler primitive regressions all passed explicitly.

Retention/grace verification: the new test first reproduced objects remaining indefinitely after grace, then `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace` passed five consecutive isolated executions after the fix. The complete `filesystem_fuse` suite passed 43/43, and both catalogue tests plus all three convergence regressions passed explicitly.

Acceptance rebroadcast verification: the strengthened real-repair burst first reproduced duplicate publication announcements, then passed eight consecutive isolated executions after the fix. The repeated-evidence sibling test passed five consecutive executions. The complete `rpc_cluster` suite passed 29/29 and `invariants` passed 36/36; the catalogue burst and exact garbage-grace tests also passed explicitly.

Lagging-replica verification: `rpc_cluster/test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` passed six isolated executions. The expanded complete `rpc_cluster` suite passed 30/30.

Phase 3 repository-wide checkpoint: `sh run-tests.sh build` passed the complete default suite 199/199 and runtime-dependency suite 3/3. The run explicitly included `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`, the real burst and sibling tests, the exact garbage-grace test, and the lagging-third catch-up test.

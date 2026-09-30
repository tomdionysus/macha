# Phase 2 durability closeout and Phase 3 scheduler checkpoint

Date: 2026-08-30

## Completed in this checkpoint

1. Added deterministic namespace-journal crash-prefix coverage.
   - A partial grouped `published` append is replayed without creating another metadata generation.
   - A partial grouped `done` append is reconciled and compacted without invoking the publication worker.
   - Every acknowledged directory remains visible after restart and the operation journal returns to its header-only state.

2. Added deterministic concurrent-admission coverage.
   - Recovery publication is gated inside the metadata commit path.
   - A new live FUSE `mkdir` is admitted and becomes locally visible while that publication is blocked.
   - Releasing the gate preserves order and produces exactly one recovery batch plus one live batch.

3. Began Phase 3 convergence coalescing.
   - Added `ConvergenceDemand`, a single-consumer/multi-producer semantic epoch high-water state.
   - Metadata and topology events are now distinct from storage-only maintenance events.
   - A burst does not repeatedly signal an already-scheduled convergence owner.
   - Events received during a run schedule at most one follow-up pass.
   - Same-generation sibling/topology events advance the semantic epoch and cannot be lost by generation-only coalescing.
   - Catalogue convergence is skipped when a newer epoch arrived during metadata repair, preventing work on an obsolete intermediate view.
   - Diagnostic counts expose events received and runs scheduled/completed.

## Verification

- `filesystem_fuse`: 42/42 passed after the durability tests were added.
- `invariants/test_convergence_demand_coalesces_burst_and_keeps_same_generation_event`: passed.
- `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event`: passed.
- `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`: passed.
- Complete default suite after the Phase 3 wiring: 194/194 passed.
- Complete runtime-dependency suite: 3/3 passed.

## Next work

1. Add an integrated service-level gated burst test around real metadata repair, using the new diagnostics to prove the bounded run count.
2. Test retention deadlines and garbage grace across a coalesced burst.
3. Audit acceptance propagation so installing already-known evidence never rebroadcasts it.
4. Test a lagging third replica importing a linear history burst while service convergence observes only the newest necessary state.
5. Complete the deliberately deferred mixed create/rename/unlink batching identity work separately; rename remains a safe singleton boundary today.

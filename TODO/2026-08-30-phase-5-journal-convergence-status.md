# Phase 5 journal and convergence Status diagnostics

Date: 2026-08-30

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope

Expose the two remaining live operational counter families needed for the final
Phase 5 comparison:

- FUSE operation-journal durability and namespace publication totals;
- event-driven metadata convergence events, runs, epochs, and scheduled state.

No counters were newly sampled or generated. The implementation publishes
existing process-lifetime diagnostics only when Status is requested.

## Implementation

- Added `FuseFrontendDiagnostics`, an O(1), lock-free snapshot of existing
  atomic operation, publication, DATA durability, and journal counters.
- Kept it separate from `FuseFrontendStatus`; the latter can inspect queues and
  inode state and is therefore inappropriate for a frequently viewed API.
- Added optional Status providers for FUSE and convergence diagnostics. Status
  copies provider handles under a short mutex, releases it, and then obtains the
  snapshots.
- Wired the Service-owned `ConvergenceDemand` for the full Service lifetime, so
  convergence diagnostics remain available during startup recovery.
- Wired the externally created mounted `FuseFrontend` through weak ownership.
  Filesystem diagnostics are unavailable before mount completion, after
  teardown, and on nodes without a mount; Status never extends frontend
  lifetime.
- Added `diagnostics.filesystem` and `diagnostics.convergence` to
  `GET /api/v1/status`.

There is no timer, polling loop, persistence, metadata mutation, network fan-out,
or gossip expansion in this change.

## Semantics recorded by the test

A fresh live `mkdir` produces four successful operation-journal append/barrier
groups:

1. inode descriptor admission;
2. namespace-operation admission;
3. accepted publication marker;
4. completion marker.

The first test expectation incorrectly assumed three and then reproduced the
actual value of four. This is not the same path as restart recovery: admission
is already durable before a recovery frontend starts, so the existing recovery
tests correctly expect only the grouped publication and completion barriers.

## Verification

- `filesystem_fuse/test_status_exposes_filesystem_and_convergence_counters`
  passed after proving exact live journal totals, exact namespace publication
  totals, drained convergence run/epoch state, and weak-provider detachment.
- Startup Status proves convergence is available and filesystem diagnostics are
  unavailable before a mount exists.
- Direct node-only Status proves both optional Service/frontend providers remain
  safely unavailable when no Service owns them.
- Complete `filesystem_fuse` selector passed 44/44.
- Complete `invariants` selector passed 36/36.
- Complete default suite passed 204/204, including the catalogue burst and
  catalogue sync/search/artwork/GC regressions.
- Runtime dependency suite passed 3/3.

## UAT completion

The deployed three-node UAT passed. A 65-create/65-remove burst used six
metadata publications, produced exact admission and grouped-completion journal
accounting, converged all nodes at generation 1439, and returned every demand
epoch and metadata queue to a drained parked state. See
`TODO/2026-08-30-phase-5-journal-convergence-uat.md`.

This closes the remaining Phase 5 live counter comparison. Physical DATA GC and
the repeated-burst RSS ceiling remain separate measurements.

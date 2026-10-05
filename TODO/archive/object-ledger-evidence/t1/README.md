# T1 evidence: the instrument

Branch `experiment/object-ledger-t1`, cut from `experiment/object-ledger-t0`
(both edit `src/service/service.cpp`). No version: T1 is judged in-process
and is not deployed.

## What was built

- **The clock seam.** `MaintenanceClock` (`src/service/maintenance_clock.hpp`)
  is the maintenance pass's first injected dependency. Every time read a
  decision depends on goes through it: quiet windows, garbage grace and the
  tombstone retirement stamp, back-offs, credit accrual, repair's weighted
  share, the scrub schedule, and the pass's own wait. Measurements (slow-stage
  logs, observation durations, the CPU reporter) stay on the real clock.
  `SystemMaintenanceClock` is production; `ManualMaintenanceClock` moves only
  when advanced. `Service` takes `ServiceInstruments` (clock, trace hook,
  lifecycle hook); production passes none of them.
- **The decision trace.** The pass reports every gate with its inputs on
  each pass that evaluates it, and every action: claims walked and not
  present, tombstones erased and stamped, releases, control GC, prunes,
  reclaimed bytes, and the inventory and release horizon it builds.
  `DistributedStore::repair_step` reports each copy pushed, each pull and
  each local copy dropped, in the order it decides them.
- **The trace harness** (`tests/test_maintenance_trace.cpp`): a traced node
  on a manual clock, driven through scripted steps. Per step it compares the
  outcome (store and claim counts, named objects held and claimed), the
  actions since the previous step (grouped by kind in first-taken order,
  each distinct action once) and, for every gate, its current node
  conditions and its verdict from the last pass where it was due and its
  inventory not rebuilt. Scheduling inputs are excluded (`due`, `rebuilt`,
  repair's share and quiescence): which passes run between two steps is
  timing, not a decision.
  Ids are normalised to #n by first appearance.
- **Seven fixtures** (`tests/fixtures/maintenance-traces/`): a quiet node;
  tombstones maturing; a backend offline and back; claimed objects lost;
  a peer unreachable and back (two nodes); an incomplete catalogue, which
  also leaves the release horizon incomplete.
- **The lifecycle recorder** (`tests/test_lifecycle_record.cpp`, in
  `macha-tests-runtime`, which parses YAML): gbni-1's and fi-1's deployed
  configurations, templated (`tests/fixtures/node-configs/`), run through
  start and stop; the order is committed in `tests/fixtures/lifecycle/`.

## Decisions taken building it

- **What a trace compares.** A first version recorded gate transitions and
  every action as they happened; it was 3-41% non-deterministic, and each
  variation was traced to its cause before the design changed: whether a
  startup event lands before the first pass; real-time idleness (the store's
  `idle_for`, not yet on the clock) arming wake-ups in fake time; a pass
  rebuilding a view at an intermediate generation mid-write; a stray wake-up
  after GC completed; a verification of an object that needed nothing; a
  push racing a peer's pull; a claim barrier choosing between two present
  nodes; rejoin traffic arriving after a step. Each fix removed timing from
  the comparison, never a decision.
- **Repair across nodes is not compared.** Which of two nodes restores a
  copy first is a race by design. Repair's order is compared on one node.
- **Incomplete release horizon has no separate fixture.** A fresh node has
  no namespace tree root to remove; the lost catalogue manifest makes the
  horizon incomplete, and that fixture shows a deleted file keeping its
  claim.
- **Lifecycle in process has no FUSE mount and no plugins.** Both nodes'
  configurations therefore give the same Service-level order; the subsystem
  steps that distinguish them are first measured on the cluster at T5.

## What the fixtures record about 0.73.2 (findings, not fixed)

- **A lost object does not wake repair.** A backend going offline, or
  claimed objects vanishing from the store, leaves `held=0 claimed=1` with
  no claim walk until an unrelated event wakes repair
  (`backend-offline`, `claimed-objects-lost`). Discipline 1 (self-healing)
  says repair should notice.
- **Stop runs twice.** `main()` calls `Service::stop()` and the destructor
  calls it again; every component's stop is invoked a second time
  (`tests/fixtures/lifecycle/`). Idempotent today, recorded.

## Acceptance (plan, T1)

- Determinism: see `determinism.md`.
- A deliberate change to a gate or to repair's order changes the trace:
  see `mutations.md`.
- Lifecycle order recorded for every real configuration: both, 40/40
  repeated runs identical.
- Q: maintenance tests that wait on real time, with their share: see
  `q-real-time-maintenance-tests.md`.

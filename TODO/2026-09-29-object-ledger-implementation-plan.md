# The object ledger and the component model: stage 0 implementation plan

Status: EXPERIMENTAL, on `experiment/object-ledger`. Implements the most
recent version of
[the object ledger and the component model](2026-09-29-object-ledger-and-components-spec.md),
which is canonical; where this plan and the spec disagree, the spec wins and
this plan is corrected. Written 2026-09-29 against the tree at `51ed982`
(0.73.2 code), ordered by critical path analysis: measure the existing
system, build the instrument, prove the design on one thin slice through
every layer, then widen.

## What the structure is for

Macha is to be Dijkstra-provable and Knuth-legible before mass peer review,
and one reason for this experiment is to make independent testing easy.
That sets the standard of evidence for every step:

- **Proof is structure plus coverage.** Components small and composed
  fractally, each with contracts stated as preconditions, postconditions
  and invariants beside what it may wait on and whether it is thread-safe;
  100% line and branch coverage of every component a step creates or
  converts, each covered path's test mutation-proven. Where complexity
  outruns what can be enumerated, the functional and behavioural suite
  covers the rest.
- **Legibility is a requirement, not polish.** A component reads top-down
  as one idea; names say what things are; comments say why, never what the
  code already says; a reviewer who did not write it can follow each
  contract to its implementation and its tests.
- **Sanitizers are used, not canonical.** The ASan and TSan builds
  (`build-asan`, `build-tsan`) run, and anything they find is fixed, but
  their silence is not evidence of correctness.
- **No CI.** Every accepted stage and substage is committed and pushed to
  GitHub on `experiment/object-ledger` or a branch cut from it. The pushed
  history is the record.

## How every step is judged

Each step has its own goal and acceptance criteria. On top of those, every
step, without exception:

- **Builds and passes on both toolchains.** Laptop (Apple Clang):
  `cmake --build build -j8`, `./build/macha-tests`. fi-1 (GCC):
  `nice -n 10 cmake --build build -j3` and `macha-tests`,
  `macha-tests-runtime`, `macha-tests-torrent` in one ssh (595 + 15 + 21
  at 0.73.2). Warnings are errors on both.
- **Every new test is mutation-proven**: break the code it guards, see it
  fail, restore, remove the `.o`.
- **Covers what it builds**: 100% line and branch coverage
  (`./run-coverage.sh`) of every component the step creates or converts,
  with each contract's preconditions, postconditions and invariants
  written down and tested.
- **Keeps operational and functional fidelity**, and meets none of the kill
  criteria, judged holistically against T0's thresholds: no loss of
  fidelity to the user, nothing more brittle, no breach of the Laws and
  Guidelines, nothing less performant or less resilient than before. A step
  that fails this is withdrawn, not patched forward.
- **Records its decisions** in the spec's decision log, dated, with the
  reasoning.
- **Is pushed when accepted**, stage and substage alike.
- **States the laws it touches** and how each holds.
- **Changes nothing the API sends**, or announces the change to Core and
  every client before it ships.
- **Does not make the suites slower** (Q track, below): summed case time
  and wall time are reported at every step against the step before.
- **Merges to `experiment/object-ledger` only when accepted** (see
  Branches, below).

This is an experiment: steps are built to find out, and the criteria above
are how each is evaluated, not preconditions for starting it.

## Branches, versions and deploys

- **`develop` is never modified** by this work, and it is the only
  development stream: nothing merges into it until the experiment ends,
  and nothing needs merging from it.
- **Work happens on branches cut from `experiment/object-ledger`**, one per
  step or part of a step. A branch merges back only when its step is
  accepted: working, tested, cohesive, and behaviourally faithful to the
  main project. `experiment/object-ledger` holds nothing else.
- **On success** the whole experiment commit tree merges into `develop`, so
  the development history is unbroken. **On failure** development continues
  from `develop` as it stands (`75e6f98`).
- **Versions are plain semver, no extensions**, bumped in `CMakeLists.txt`
  and the README's version line together (configure enforces the match),
  as on `develop`. A version is taken when a build is deployed: first by
  T0's instrumented build, then from T5.
  On failure the experiment's version line ceases to exist, as if it never
  happened, and `develop` continues its own numbering.
- **The cluster**: gbni-1 and fi-1 are the only Macha cluster in the world,
  and it is a disposable test cluster. Dropping the library costs a great
  deal of time and is avoided where possible, but not at the expense of the
  experiment.
- **Nothing deploys between T0 and T5.** T0 deploys instrumentation only,
  because the baseline has to come from the real cluster; T1 to T4 are
  judged in-process (next section). Every deploy is built on fi-1 (never on
  gbni-1), installed with `install-guarded.sh`, viewers checked first,
  nodes restarted 5-10 minutes apart.

## Until T5: in-process testing

`TestService` and `TestCluster` run real nodes in one process over
loopback, which is stronger than mocking, and fakes at the contracts isolate
a component further. That is sufficient for functional fidelity:
decisions and traces, gates, cursor order and resumption, handle lifetime,
lifecycle order for each real node configuration, the waiting guard and the
Clang lock analysis, and the presence index's logic under fault injection.

It cannot establish physical or performance fidelity: cost on real
hardware under load (the 2026-09-07 cold disk checks at ~5 ms each on a
disk saturated by an import), the kernel's behaviour when a device drops
(USB resets, ext4 remounting read-only, an emptied mountpoint), the real
process lifecycle under systemd with real plugins and a FUSE kernel mount,
and timing under real contention. Those are first measured at T5, so a
problem found there in P or in the locking work may force rework of what
T2 to T4 built on it; that is the risk the experiment accepts.

In-suite microbenchmarks narrow it while staying in-process: lock wrapper
cost against `std::mutex`, `has()` against today's, and the claim walk's
per-object cost, each reported at every step that touches them. The fi-1
suite runs put the code on real hardware under GCC as well.

## Why this order

The layer-by-layer order this replaces had seven costs, each removed here:

| cost | cause | removed by |
|---|---|---|
| kill criteria not decidable | criteria stated qualitatively | T0 measures the existing system and sets thresholds |
| fidelity checked by cluster soak (days) until late | the decision-trace harness came in the ledger stage | T1 builds it first; from then on fidelity is a suite comparison (minutes) |
| design flaws found at ~80% of the effort | every foundation built in full before any contract used it | T2 proves every layer on one path at ~15% |
| every file opened twice; merge conflicts everywhere | annotating 173 mutexes in 59 files as a separate stage | L: annotate as each step touches a file |
| service-wide wiring done twice | concrete wiring, then rewiring on contracts | T5: each component moves into the root once, onto its contracts |
| the one semantic change shares a soak with refactors | authoritative presence inside the store-contract stage | P: its own step, deploy and soak |
| mapping done too early to stay true | one up-front survey of everything | M: each step maps what it is about to move |

## Critical path

Sizes are relative estimates for comparing steps, not a schedule.

```text
T0 measure [3] ─► T1 harness+clock [3] ─► T2 slice [5] ─► T3 ledger [5] ─► T4 metadata [6] ─► T5b conversions [~5]
                          │                    │
                          ├─► P presence [3] ──┘ (cluster measurement at T5)
                          │
                          └── T5a conversions needing no new contract [~5]: any time after T2
                              L annotate-as-touched, M map-just-in-time, Q test consolidation: inside every step
                              S final sweep [~4]: after T5
```

Critical path T0 → T1 → T2 → T3 → T4 → T5b, about 27 units of about 43.
Only P and T5a run beside it; T1 to T4 all edit `src/service/service.cpp`,
so parallel work there only produces conflicts. T0's cluster soak runs
while T1 is built, so it adds little wall time.

## Tracks that run inside every step

### L. Annotate as you touch

T2 lands the annotated wrapper types for mutex, shared mutex and their
guards (Clang thread-safety attributes; I/O modelled as a capability, with
negative capabilities for "must not hold") and turns on `-Wthread-safety`
with warnings as errors in the Clang build; GCC ignores the attributes.
Clang checks only annotated code, so partial adoption is valid. From then
on every file a step edits has its locks annotated in the same change. S
sweeps the files no step touched.

### M. Map just in time

Each step begins by surveying exactly what it moves, with file and line,
each site read rather than inferred, and appends it to the spec's Inventory
section. Known totals for sizing (basemind, 2026-09-29; call sites exclude
the accessors' own declarations in `src/cluster/cluster.hpp`):
`local_store()` 63 call sites in 8 files, `control_store()` 34 in 5,
`membership()` 31 in 10, `data_resources()` 18 in 3, `retention_store()` 15
in 4, `config()` about 145; 173 `std::mutex` in 59 files, 3
`std::shared_mutex`, 22 `std::condition_variable`, 34
`std::condition_variable_any`, 1,047 lock sites in 56 files. Known correction already
made: the DATA object store is `StoragePool` over `LocalStore` backends.

### Q. Test consolidation

Baseline (laptop, `build/claude-full-5.log`, 2026-09-29): 541 cases, 50.3 s
wall, 268.5 s summed, 5.3x parallel; median case 280 ms, p90 1.06 s, seven
cases at 5 s or more. Most time is in tests that stand up whole services or
clusters: `filesystem_fuse` 80.6 s over 79 run cases (74 `TestService`
constructions across its 80 declared cases), `rpc_cluster` 59.5 s over 68,
`hydration_catalogue` 29.6 s over 36. `tests/` holds 513 wait sites
(`sleep_for`, `wait_until`, `eventually`). Of 631 declared cases across all suites, 446 are
integration cost, 166 fast, 19 heavy.

The plan makes consolidation possible in three ways, and Q does it:

- **An injected clock** (T1) turns real-time waits for grace periods,
  backoffs and quiet windows into stepped time: faster, and deterministic
  instead of timing-dependent.
- **Fakes at the contracts** (T2 onwards) let a component's own logic run
  alone, in-process, with no sockets, peer threads or service startup,
  where today it needs a `TestService` or `TestCluster`.
- **Conformance suites and decision-trace fixtures** replace duplicated
  per-implementation tests and hand-built maintenance scenarios with one
  suite per contract and table-driven fixtures.

Rules:

- When a step converts a component, its tests that exercise only that
  component's logic move to isolated tests against fakes. The integration
  test is deleted only after the isolated test is mutation-proven against
  the same fault.
- Integration tests stay where the behaviour is genuinely cross-component:
  real topology, lifecycle, transport, and law 4 recovery. Fewer, not none.
- Production test hooks (`set_before_loose_write_for_tests` and the other
  `*_for_tests` setters on `LocalStore`) are replaced by injected fakes and
  removed from the production classes.
- Every step reports case count, summed case time, wall time and wait
  sites against the step before; none may rise without a stated reason.
  Consolidation targets are set once T1 has measured how much of the
  summed time is waiting.

## T0. Measure the existing system

**Goal.** Turn the kill criteria from qualitative to quantitative: every
"less performant, less resilient, more brittle" becomes a named metric with
a baseline measured on 0.73.2 behaviour and a threshold recorded in the
spec.

**Work.**
- Instrument the existing code, adding observation only. Measurements go
  to the node's logs and local files, not to Status or any API response,
  so nothing the clients receive changes. Where a figure already exists
  (`nodes[].traffic`, `status/diagnostics` `repair`, the stage timings of
  `log_slow_stage`), it is used as it is.
- The metrics, each with how it is measured, over what window, and on
  which node:
  - claim walk and GC sweep cost per object; quantum-commit claim latency,
    idle and under import load;
  - repair bytes per minute and objects examined, idle and loaded;
  - GC and retention release rates;
  - API latency percentiles on the busy endpoints (`GET /api/v1/torrents/jobs`,
    the catalogue, Status);
  - playback start and seek latency; FUSE publication throughput;
  - resident memory; startup time to `wait_services_ready`; shutdown time;
    recovery time after a restart and after a backend drops and returns.
- In-suite baselines on the laptop and fi-1: the Q figures above, the
  microbenchmarks, and line and branch coverage per component
  (`./run-coverage.sh`).
- Deploy the instrumented build (plain semver, the experiment's first
  version) to both nodes and soak it under normal load: viewers, torrents,
  imports, repair.

**Acceptance.**
- The instrumented build is behaviourally identical to 0.73.2: all suites
  green, the only diff is observation, and each probe's cost is measured
  and negligible.
- Every kill criterion has a metric, a baseline with its variance over the
  soak, and a threshold (what difference counts as worse), written into the
  spec's kill criteria.
- The coverage baseline per component recorded, as the starting point for
  the 100% bar on each component a later step converts.

## T1. The instrument: trace harness, clock seam, lifecycle recorder

**Goal.** From here on, fidelity is checked in the suites, not by soak.

**Work.**
- An injected clock for the maintenance pass: the grace periods,
  `foreground_quiet`, `no_progress_backoff` and quiescence deadlines read
  `Clock::now()` and `wall_time_ns()` directly in `src/service/service.cpp`
  today. The clock is the first injected dependency.
- A decision-trace recorder on the existing `maintenance_stage_hook_`,
  driven through `TestService` / `TestCluster` (`tests/test_support.hpp`):
  objects repair visits and in what order, claims walked, each gate's
  result, claims released, objects deleted, tombstones erased.
- Trace fixtures: a quiet node; tombstones maturing; an incomplete
  catalogue; an incomplete release horizon; an unreachable peer; metadata
  not stable; a backend offline.
- A lifecycle recorder: construction, start, `initialise_services`,
  `wait_services_ready`, `request_stop` and stop order, captured for each
  real node configuration (gbni-1 and fi-1 configs as fixtures, secrets
  removed) and committed as data.

**Acceptance.**
- Each fixture's trace is deterministic across 100 runs (`--repeat`).
- A deliberate change to a gate or to repair's order changes the trace;
  mutation-proven.
- Lifecycle order recorded for every real configuration.
- Q: the maintenance tests that wait on real time are identified, with
  their share of summed time.

## P. Authoritative presence

**Goal.** `has()` waits on nothing after warm-up; the only step that
changes a semantic, isolated so any fault is attributable.

**Work.** Inside `LocalStore` and `StoragePool` only, no contract yet:
- presence published when a put completes;
- the index authoritative once `warm_presence_index` finishes, with `has()`
  waiting on the device only before that;
- a backend going offline invalidates its entries, re-adoption restores
  them, through the existing re-adoption path;
- the per-object lock and disk fallback removed from `has()`;
  `pruned_loose_` handling kept;
- tests that write object files directly (`test_invariants.cpp`,
  `test_storage_metadata.cpp`, `test_storage_v18.cpp`, `test_support.hpp`)
  moved to fault injection through the store.

**Acceptance.**
- `has()` never reports a partially written object (a put interrupted at
  each point); never touches the device after warm-up (a filesystem hook
  that fails any access); a backend taken offline and re-adopted leaves
  presence exact. Mutation-proven.
- T1 traces identical.
- In-suite microbenchmarks: `has()`, the claim walk and the GC sweep cost
  the same or less per object.
- On the cluster at T5 (below): a backend unmounted under load and
  remounted, as on fi-1 2026-09-29, recovers without a restart and without
  false presence; quantum-commit claim latency on a disk saturated by an
  import the same or better (the 2026-09-07 regression the cache fixed must
  not return).

## T2. The thin vertical slice: the claim walk through every layer

**Goal.** Prove spec A1 to A5, B1 and B3 end to end on one real path, at
about 15% of the effort, where the kill criteria are cheapest to apply.

The path: `repair_retained` (`src/service/service.cpp:1516-1547`), which
walks retention claims with `next_retained`, checks presence with `has()`,
and restores missing claimed objects.

**Work.**
- `Cursor`, `Budget` (values plus an injected yield source carrying the
  work context) and `Page`, enough for `next_retained`; `SnapshotHandle`.
- The work context for every class, `DataWorkContext` as its DATA
  specialisation; the waiting declaration and runtime guard, applied to
  this path and to `snapshot_view` (`network`) and
  `available_snapshot_view` (`none`), so the ACTIVE item 2 list falls out
  of the suites.
- L begins: wrappers, `-Wthread-safety`, and the locks this path takes
  annotated.
- `ObjectStore` contract with `has()` (after P, declared `none`).
- `ObjectLedger` with `claimed` (forwarding to `RetentionStore`) and `held`.
- The component contract generalised from `Subsystem`: requires and
  provides, construction with dependencies, `start`, non-blocking
  `request_stop`, joining `stop`, fault sink.
- A composition root owning one component: maintenance, extracted from
  `Service`, taking only the contracts it uses. Everything else still built
  by `Service`.
- Q: the claim walk's tests run against fakes of the store and ledger.

**Acceptance.**
- T1 traces identical; lifecycle order equals the recording for every
  configuration.
- A control context entering a `network`-declared operation fails a test;
  the Clang analysis rejects `has()` if it takes a lock held across I/O.
  Both mutation-proven.
- A handle held across a republish still reads its own snapshot.
- The claim walk's per-object cost the same or better.
- A written assessment against the kill criteria: whether any contract,
  the budget, the handle or the component contract proved brittle or
  wrongly shaped, and what changes before widening. The spec is corrected
  where it was wrong.

## T3. The ledger, widened

**Goal.** One owner for everything maintenance asks about an object.

**Work.**
- Horizons `inventory` and `release` as snapshot handles, with
  `refresh_inventory` and `refresh_release` under today's conditions.
- The three gates exactly as the spec's table, returning today's reason
  strings; the predicate queries implemented, tested, unused;
  `everywhere` in place of `universal`.
- Consumers moved: inventory and release builds, repair, tombstone
  collection, control release and GC, DATA release and sweep, rebalance
  keep checks (`src/cluster/distributed_store.cpp` 2300, 2764), publication
  claims (847, `src/cluster/cluster.cpp` 1528), peer remove refusal (1535),
  catalogue staging GC (`src/catalogue/catalogue.cpp` 1872), retention
  compaction.
- `Service`'s reachability members (`src/service/service.hpp:103-114`)
  deleted.
- Metadata is reached through the declared views from T2; the full
  metadata contract is T4.
- Q: GC, release and repair scenarios move to trace fixtures and
  ledger-level tests against fakes.

**Acceptance.**
- T1 traces identical over every fixture.
- Each gate: a table test over every condition in its row; mutation-proven.
- No `retention_store()` call and no reachability vector outside the ledger.
- The retention store's existing tests pass through the ledger.
- On the cluster at T5: repair bytes and examined counts in range under the
  same load; GC and release proceed once deletion is resumed; memory
  unchanged.

## T4. The metadata contract

**Goal.** Edges by referrer behind one contract, blocking made explicit.

**Work.**
- `MetadataView` over `MetadataManager`, `MetadataReplica` and the
  namespace primitives: `current()` and `converged()`, every call site
  mapped one-to-one (M), none changing which it calls.
- `entries(view, cursor, budget)`, a resumable, budgeted tree walk (new
  code); the callback walk kept and declared for whole passes.
- Commit application declared and wrapped, unchanged.
- The catalogue's hidden repair split out of `maintenance_objects()` into
  an explicit step, called in the same place.
- `set_namespace_store` removed: metadata takes the object store contract
  at construction.

**Acceptance.**
- `entries` visits exactly the set and order of `for_each_namespace_entry`
  on the fixtures and on a copy of each live node's namespace, across
  budget boundaries.
- T1 traces identical, including across the catalogue split.
- The guard's list of control paths into `converged()` handed to ACTIVE
  item 2, not fixed here.

## T5. Component conversions, strangler-style

**Goal.** Every component moves into the root once, directly onto its
contracts; nothing reaches a collaborator through `NodeRuntime` or
`Service`.

**Work.** One component per change. For each: survey (M), declare requires
and provides, construct in the root, remove its locator calls and setters
(ports where a cycle remains), annotate its locks (L), move its isolated
tests to fakes (Q), and confirm lifecycle order.

- **T5a**, needing no new contract, any time after T2: cluster status, the
  session, users and web APIs, the HTTP servers, the subsystem registry and
  supervisor (`SubsystemContext` becomes a view of the graph).
- **T5b**, after T3 and T4: `DistributedStore`, `FileSystem`, hydration,
  ingest, scanner, media information, catalogue and its hints and API,
  torrent coordinator and search, cluster jobs, acquisition and manage
  APIs, playback, and the FUSE subsystem.

Core supervision is unchanged: no core component is newly restarted.

**The cluster enters here.** The first deploy of experiment code, and every
one after it, follows these rules:

- **Returning to `develop` is always possible.** Stage 0 adds no state on
  disk: the retention journal and checkpoints, the store layout and the
  metadata formats are untouched, so either node can go back to the 0.73.2
  binary at any point without migration. Before the first experiment
  install, each node's running 0.73.2 is saved as its install backup
  (`BACKUP=/root/macha-0.73.2-installed.tgz`), the rollback artifact.
- **Deletion is paused until the ledger's decisions have been watched on
  real data.** T3 moves every destructive gate, so a gate that permits too
  much deletes library data. A large `garbage_grace` in each node's config
  defers tombstone collection (matured tombstones become immature again),
  the orphan sweep (its grace is the larger of `garbage_grace` and
  `no_progress_backoff`) and control GC (`control_gc_step` takes
  `garbage_grace`); retention release only removes claims, and the bytes
  still wait for the grace. Storage grows meanwhile. Grace is restored once
  the deletion decisions logged at debug match the trace fixtures'
  expectations on the real library.
- **Old and new side by side.** Stage 0 changes neither the cluster
  protocol nor any wire format, so the two nodes can run different builds:
  first fi-1 on the experiment and gbni-1 on 0.73.2 under the same library
  and load, a direct behavioural comparison; then both.
- **Physical and performance measurements** deferred from earlier steps run
  here: P's backend loss and remount, the claim latency under import load,
  lock-heavy paths (FUSE publication, playback start and seek, API latency),
  and restarts under load.
- **The library is dropped only if the experiment requires it**, and the
  operator is told before any step that risks it.

**Acceptance, per conversion.**
- Lifecycle order equals the recording for every configuration; bounded
  shutdown holds.
- T1 traces identical.
- Changes that alter a node's lifecycle are deployed and soaked one node at
  a time under that node's configuration; others batch into one deploy.

**Acceptance, at the end.**
- No locator call or post-construction setter left, or each listed in the
  spec with its reason.
- Every contract operation carries its waiting and thread-safety
  declaration, checked against its implementation; the spec's A4 audit
  complete.

## S. Final sweep

**Goal.** One locking mechanism everywhere, and the suites at their new
shape.

**Work.** Annotate the files no step touched; retire any remaining
`*_for_tests` hooks.

**Acceptance.**
- The Clang build clean under `-Wthread-safety -Werror`; GCC and all three
  suites green; the ASan and TSan builds run and anything they find is
  fixed (used, not canonical).
- 100% line and branch coverage of every component in the root.
- No measurable change in lock-heavy paths (FUSE publication throughput,
  playback start and seek, API latency) against the same load.
- Q: summed case time, wall time and wait sites below the baseline, with
  the integration tests that remain each justified as cross-component.
- A day on the cluster under normal load (viewers, torrents, imports,
  repair): fidelity kept, no law breached, latency, throughput, memory and
  restart behaviour the same or better.

## Open questions for the operator

1. The spec's open questions are decided before the step that needs them:
   context in `Budget` before T2; core supervision and where the component
   model lives before T2; the control gate and `universal` before T3;
   release over pinned roots and map-backed snapshots before any later
   storage stage.

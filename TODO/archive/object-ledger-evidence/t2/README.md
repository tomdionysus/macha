# T2a: the contract vocabulary

`src/contract/work.hpp` (WorkContext, Waits, may_enter, WaitGuard),
`src/contract/walk.hpp` (Cursor, Stop, Page, YieldSource, Budget); `FrameType`
moved to `src/cluster/frame_type.hpp`; `DataWorkContext` is now the DATA
specialisation of WorkContext (same interface, still refuses control).

Tests (`tests/test_contract.cpp`): every work class against every wait mask
(5 x 16); every Waits pair; the guard in both modes, logging once per
operation; every sequence of up to seven takes against every limit 0-5 and
none; must_stop's precedence over all 16 combinations of cancelled, budget
deadline, context deadline and yield.

## Mutation record (laptop, 2026-09-30)

- control may wait on the network -> KILLED by contract/test_only_control_is_refused_and_only_device_or_network_waits, contract/test_wait_guard_records_or_throws
- control may wait on the DATA device -> KILLED by contract/test_only_control_is_refused_and_only_device_or_network_waits, contract/test_wait_guard_records_or_throws
- the epoch is a deadline -> KILLED by contract/test_budget_stop_precedence_over_every_combination, contract/test_work_context_deadline_and_cancellation
- includes tests the wrong bits -> KILLED by contract/test_only_control_is_refused_and_only_device_or_network_waits, contract/test_wait_guard_records_or_throws, contract/test_waits_compose_and_include_over_every_mask
- violations not counted -> KILLED by contract/test_wait_guard_records_or_throws
- throw mode ignored -> KILLED by contract/test_wait_guard_records_or_throws
- logged every time -> KILLED by contract/test_wait_guard_records_or_throws
- an exhausted operation bound still grants -> KILLED by contract/test_budget_bounds_are_spent_exactly
- bytes bound off by one -> KILLED by contract/test_budget_bounds_are_spent_exactly
- yield outranks the deadline -> KILLED by contract/test_budget_stop_precedence_over_every_combination
- context deadline ignored -> KILLED by contract/test_budget_stop_precedence_over_every_combination
- a yield counts as complete -> KILLED by contract/test_cursor_and_page
- every class refused (rewritten to compile: `frame_type == static_cast<FrameType>(0) &&`) -> KILLED by contract/test_wait_guard_records_or_throws, contract/test_only_control_is_refused_and_only_device_or_network_waits

13 of 13 killed. Full suite: 642/642 (laptop).

# T2b: I/O as a capability; has()'s index path proven to wait on no I/O

`src/contract/thread_safety.hpp`: Clang thread-safety attributes (ignored by
GCC), `-Wthread-safety` on for Clang builds of the core. "This code waits on
no I/O" is a capability, `no_io`, held by a scoped `NoIoRegion`; waiting on
I/O -- so far, taking a per-object lock a writer holds across its device
I/O, now always through `ObjectLock` (all 12 sites in `LocalStore`) -- is
annotated `MACHA_EXCLUDES(no_io)`.

`LocalStore::has()` is split: `presence_from_index()` (the pack index and
the presence index, holding a `NoIoRegion`), then the pre-warm-up device
check. Adding `ObjectLock waits_for_a_writer(object_mutex(id));` to
`presence_from_index` fails the Clang build:

    local_store.cpp: error: cannot call function 'ObjectLock' while no-I/O
    region 'no_io' is held [-Werror,-Wthread-safety-analysis]

A first attempt annotated the function `EXCLUDES(io_locks)` with object locks
acquiring `io_locks`; that compiled with the lock inside, because EXCLUDES
restricts the caller, not the body. The capability was inverted.

Scope, honestly: the analysis is per function. A call inside the region to
another function that takes an object lock is caught only once that function
is itself annotated `MACHA_EXCLUDES(no_io)`; that annotation spreads with
the L track, file by file.

# T2c (part): the wait guard on the snapshot views

`MetadataManager::snapshot_view(const WorkContext&)` enters the guard always
(it may refresh from the replicas: state device and network);
`CatalogueManager::snapshot_view(const WorkContext&)` only when cold (warm it
serves the cached snapshot and waits on nothing). `WorkContext` names its
origin. The seven HTTP call sites ACTIVE item 2 lists now pass a control
context naming their route.

- A control context entering `MetadataManager::snapshot_view` throws in the
  guard's test mode; a loader context does not; a control context on a warm
  catalogue does not. Removing the guard fails the test (mutation-proven).
- **ACTIVE item 2 corrected:** the four `catalogue_api.cpp` sites call
  `CatalogueManager::snapshot_view()`, which is memory-only when warm and
  reaches metadata only when cold; one `manage_api.cpp` site (730) calls
  `MetadataManager::snapshot_view()` directly.
- **The audit, from the suites:** a full verbose run (643/643) in the guard's
  record mode logged no control path into a guarded operation. The catalogue
  is always warm by the time a test's API read arrives, and the direct
  metadata call (POST `/api/v1/manage/nodes/{id}/identity-association/reset`
  with no host, for a node absent from live membership) is not exercised by
  any test. The guard sees only what the suites drive: the cold-catalogue
  case and that route need cases of their own before the list can be called
  complete.

# T2c (part two): the claim walk on the contracts

`ObjectStore` (`src/contract/object_store.hpp`, B1: `has()` so far, waits on
nothing) is implemented by `LocalStore` and `StoragePool`. `ObjectLedger`
(`src/contract/object_ledger.hpp`, B3 as far as the walk needs:
`claimed(class, cursor, budget)` pages and `held(class, id)`) is implemented
at stage 0 by `RetentionLedger` over the `RetentionStore` and the two object
stores. The claim walk left the maintenance lambda for `ClaimWalk`
(`src/service/claim_walk.{hpp,cpp}`): one 16-claim page per class per step,
a cursor it keeps, and a `ClaimRestorer` for each claim not held. The
maintenance pass supplies the restorer (network credit, the fetch, the trace
line) and reads the step's counts; nothing else in the pass changed.

- **Equivalence, by construction of the test:** the walk is run side by side
  with a reference copy of the 0.73 lambda (next_retained one claim at a
  time, 16 per step) over fakes, comparing every step's counts, every
  restorer call and the cursor: every held pattern over 0..8 claims, credit
  0..3 with and without refill, with and without a refused restore; 15, 16,
  17, 32, 33 and 50 claims across the step bound; and a ledger changing
  between steps. `RetentionLedger` is checked against the store it pages
  (0..9 claims with released ones skipped, page bounds 1..10, zero budget,
  cancellation, class routing of `held`).
- **Mutation:** 16/16 mutants killed (credit wait position, unfinished on
  bound and on credit wait, resume, cursor advance, restored and missing
  counts, held claims restored, bound 15 and 17; ledger wrap, next, budget,
  cancellation, class swap). One first-cut mutant did not compile and was
  rewritten (`!held || true`).
- **Traces:** the seven maintenance trace fixtures pass unchanged; two of
  them (`claimed-objects-lost`, `incomplete-catalogue`) contain claim-walk
  actions in both classes. Full suite 649/649 (Clang, laptop).
- **Cost:** `baseline/test_baseline_claim_walk_on_the_ledger_per_object`
  walks the same 2000 held claims as the T0 per-object baseline. Ten serial
  runs each, same binary: old walk median ~3.05 us/claim (1.4..4.5), ledger
  walk median ~3.2 us/claim (1.8..5.0). Both distributions are bimodal
  (laptop frequency scaling); the difference is inside the spread. The cost
  per claim is the presence check, not the paging.
- **One definition moved:** `maintenance.claim_walk.examine_us` timed
  next_retained plus has(); it now times `held()` alone, the paging being
  done once per page. Examined and missing counters are unchanged.

# T2c (part three): `Published<T>`, the snapshot handle

`src/contract/published.hpp` (spec A2, published immutable state; the plan's
`SnapshotHandle`): a mutex-guarded `shared_ptr<const T>`. `handle()` copies
the pointer under the lock and nothing else; `publish(Handle)` and
`publish(T)` swap under the lock and require a snapshot (a published state is
never withdrawn); the previous snapshot is released after the lock, so a
reader never waits on its destructor. Empty (null handle) until the first
publish. Not yet used: the ledger's horizons adopt it at T3.

- **Tests** (`tests/test_contract.cpp`): empty until the first publish and
  both constructors; a missing snapshot refused by both publish and the
  constructor, the current one kept; a handle held across two republishes
  reads its own snapshot, which lives exactly until its last holder lets go,
  and an unheld one goes with the publish that replaced it (the plan's
  acceptance line); a reader asking for a handle from inside the previous
  snapshot's destructor completes during it; four readers racing 20,000
  publishes see only whole snapshots, never going backwards. Every line and
  both sides of the one branch are reached.
- **Mutation:** 9/9 killed (no swap; previous destroyed under the lock;
  missing snapshot accepted; value overload ignores its value; handle
  returns nothing; handle does not own; constructor bypasses the
  precondition; constructor ignores its snapshot; handle reads without the
  lock -- the last killed by the race test in a plain build, no sanitizer).
- Full suite 655/655 (Clang, laptop).

## Mutation record (laptop, 2026-10-01)

- publish never swaps -> KILLED by contract/test_a_held_handle_keeps_its_snapshot_across_republish, contract/test_published_is_empty_until_the_first_publish, contract/test_published_readers_see_whole_snapshots_in_order, contract/test_published_refuses_a_missing_snapshot
- previous destroyed under the lock -> KILLED by contract/test_publish_destroys_the_previous_snapshot_outside_its_lock
- a missing snapshot accepted -> KILLED by contract/test_published_refuses_a_missing_snapshot
- the value overload ignores its value -> KILLED by contract/test_a_held_handle_keeps_its_snapshot_across_republish, contract/test_published_is_empty_until_the_first_publish, contract/test_published_readers_see_whole_snapshots_in_order, contract/test_published_refuses_a_missing_snapshot
- handle returns nothing -> KILLED by contract/test_a_held_handle_keeps_its_snapshot_across_republish, contract/test_published_is_empty_until_the_first_publish, contract/test_published_readers_see_whole_snapshots_in_order, contract/test_published_refuses_a_missing_snapshot
- handle does not own -> KILLED by contract/test_a_held_handle_keeps_its_snapshot_across_republish, contract/test_published_readers_see_whole_snapshots_in_order
- constructor bypasses the precondition -> KILLED by contract/test_published_refuses_a_missing_snapshot
- constructor ignores the initial snapshot -> KILLED by contract/test_published_is_empty_until_the_first_publish, contract/test_published_readers_see_whole_snapshots_in_order, contract/test_published_refuses_a_missing_snapshot
- handle reads without the lock -> KILLED by contract/test_published_readers_see_whole_snapshots_in_order

# T2d: the component contract, the composition root, maintenance in it

- `src/component/component.hpp`: `Component` -- name, `required()` and
  `provided()` contracts by name, `start()`, a `request_stop()` that never
  blocks, a joining `stop()` that includes the request, a fault sink.
  `Subsystem` is unchanged; plugins meet the root at T5.
- `src/component/composition_root.{hpp,cpp}`: owns components, derives the
  start order from the declarations (providers first; adding order where
  the graph is free), refuses a requirement nothing provides, a contract
  with two providers (an external one included) and a cycle, naming them;
  `external()` declares what Service still hands in. Starts once; a failed
  start stops what started, in reverse, and rethrows; `request_stop()` and
  `stop()` run in reverse and touch only running components; the
  destructor stops what runs. Each step is recorded through the lifecycle
  hook ("start maintenance", ...) before it is taken. One lock over the
  steps: Service's stop can ask for a stop while its startup thread is
  still starting the root, and the request then waits for that start and
  reaches what it started (the old `jthread` had the same race, unguarded).
- `src/service/maintenance.{hpp,cpp}`: the maintenance pass left Service
  as the `Maintenance` component: the loop (now `run`), `collect_garbage`,
  `maintain_garbage_metadata` and all of the pass's state, moved verbatim
  but for the renames the move needs and the ledger, now injected (built
  once by Service when the stores exist, where the pass built one per
  pass). Dependencies are concrete at stage 0 (decision log, 2026-10-01).
  `MaintenancePort` (owned by Service) holds what precedes and outlives the
  component: the event counter and wait, the convergence demand, the
  diagnostics. Two dead members (`retention_*_repair_cursor_`) went.
- **Lifecycle:** `test_lifecycle_of_gbni_1` and `test_lifecycle_of_fi_1`
  pass against the fixtures committed at T1, unchanged. Moving the root's
  stop ahead of status, or dropping its stop request, fails both
  (mutation-proven).
- **Traces:** the maintenance trace group 1100/1100 over `--repeat 100`.
- **Root tests** (`tests/test_component.cpp`, 9 cases): order where free
  and where constrained (chain, diamond, one provider of two contracts),
  externals, every refusal, fixed once started, every lifecycle step and
  its record, repeated requests and stops, the destructor, a failed start,
  a stop request during a start, faults routed by component and the
  default handler. Mutation: 22/22 killed (one first-cut mutant did not
  compile and was rewritten: `(void)waiting[dependant]`).
- **Suites:** macha-tests 663/663, macha-tests-runtime 17/17 (Clang,
  laptop).
- **Coverage** (laptop, Clang, whole suite, after the coverage fix below):
  every T2 primitive at 100% lines and 100% branches --
  `component/composition_root.cpp` 138/138 and 64/64, `component.hpp`,
  `contract/published.hpp` 15/15 and 2/2, `walk.hpp` 48/48 and 20/20,
  `work.{hpp,cpp}`, `claim_walk.{hpp,cpp}`, `retention_ledger.cpp`,
  `maintenance_clock.{hpp,cpp}`. `service/maintenance.cpp`, a functional
  component (decision log, two classes), 1040/1178 lines (88.3%), 484/608
  branches (79.6%). Whole library 82.4% lines. Two cases were added to
  reach the last lines (a yield source destroyed through its interface;
  `ClaimWalk::type()`) and one to reach the root's destructor catch; each
  mutation-proven.
- **The coverage fix.** The laptop's Clang coverage had never measured the
  library from a test case. Three defects, each proven before the change:
  (1) the runtime expands `%p` once, in the parent, so every forked case
  wrote the parent's file, overwriting and corrupting it (a probe: one file
  without the fix, the child's own with it); (2) each image carries a
  private copy of the profile runtime (`nm`: `__llvm_profile_*` private
  in both `libmacha_core.dylib` and `macha-tests`), so the framework reset
  and dumped only the test binary's counters and `macha_core` read 0% --
  `src/coverage.{hpp,cpp}` now lets the library name, reset and write its
  own (`<pid>-core.profraw`); (3) counters were not atomic, and in-process
  cluster tests run several nodes' threads through the same code, so lost
  increments became negative derived branch counts (`False: 18.4E`) that
  wrapped real counts to zero (bisected to the merge; the case's own
  profile alone had them) -- `-fprofile-update=atomic`. GCC untouched (T0
  measured the library through it).
- **Two cases fail only in the laptop's coverage build** (Debug, Clang,
  instrumented), both P0, reported, not fixed:
  `hydration_catalogue/test_a_torrent_is_held_by_one_job_and_a_second_add_names_it`
  failed 50/50 there and passed 50/50 in Release. **Fixed:** libtorrent's
  exported CMake target adds `$<$<CONFIG:Debug>:TORRENT_USE_ASSERTS>`
  (`LibtorrentRasterbarTargets.cmake`), which changes class layouts in about
  twenty headers; the installed library was built without it, so the Debug
  plugin read `torrent_status` at the wrong offsets, `job.info_hash` came
  back empty, and only libtorrent's duplicate backstop refused the second
  add. CMake now drops the generator expression from the imported target;
  5/5 in Debug after. `rpc_cluster/test_three_node_cluster`
  failed once in three whole-suite coverage runs (load 15): a joining node's
  pull through background repair overran `wait_until`'s fixed 5 s. **Deleted
  and rewritten from its concepts** (operator, 2026-10-01: a test that needs
  a scaled wait is a faulty test). It was one scenario making thirteen
  claims, most waiting on background work. Claims proven elsewhere (metadata
  repair visibility, restart from disk) were dropped; the rest became ten
  tests, each driving its step: a `DurableTrio` whose commits are durable on
  every replica, so a change is on the others when the call returns, and
  only forming the cluster is waited for. Any node founds the namespace; a
  warm view sees another node's commit; a file reads back through every
  node; metadata commits down to its floor and no further; a corrupt local
  copy is read from a peer and healed by the write-back (the old test
  attributed that to repair); scrub discards a corrupt copy and repair
  restores it; a read falls back to a peer; a runtime cache keeps a
  playback fetch; unlink retires an object from the inventory; rename of a
  file onto a directory is `EISDIR`. `NodeRuntime::wait_local_copies_settled()`
  is new: it waits until queued write-backs are written, so the write-back
  claims need no timing. 20/20 each. Mutation: 10 killed; three
  single-guard mutants survived because the guarded property has redundant
  guards (the metadata floor is enforced at four points; the property-level
  mutant, floor = 1, is killed), and the warm view's freshness held with
  both staleness checks in `cached_snapshot_view` removed -- the mechanism
  that keeps it fresh there is **not yet traced**; the FileSystem-level
  mutant is killed. **Not covered by the rewrite:** a joiner pulling its
  objects with no explicit step, and the cache-to-store promotion by idle
  maintenance. Both are the background pass's pacing, which needs the
  store's activity clock (`idle_for`) on the injected clock to test without
  real time (T1's later work).

## Mutation record (laptop, 2026-10-01)

- ties go to the last added -> KILLED by component/test_root_lifecycle_steps_and_their_record, component/test_root_order_is_adding_order_where_the_graph_is_free, component/test_root_starts_providers_first, component/test_root_unwinds_a_failed_start
- externals ignored -> KILLED by component/test_root_externals_satisfy_requirements
- two providers allowed -> KILLED by component/test_root_refuses_a_graph_it_cannot_order
- an external may also be provided -> KILLED by component/test_root_refuses_a_graph_it_cannot_order
- a missing provider skipped -> KILLED by component/test_root_refuses_a_graph_it_cannot_order
- a cycle ends the order early -> KILLED by component/test_root_refuses_a_graph_it_cannot_order
- duplicate names allowed -> KILLED by component/test_root_refuses_a_graph_it_cannot_order
- add after start allowed -> KILLED by component/test_root_is_fixed_once_started
- external after start allowed -> KILLED by component/test_root_is_fixed_once_started
- start twice allowed -> KILLED by component/test_root_is_fixed_once_started
- request_stop in start order -> KILLED by component/test_root_lifecycle_steps_and_their_record
- stop in start order -> KILLED by component/test_root_lifecycle_steps_and_their_record
- a stopped component stays running -> KILLED by component/test_root_lifecycle_steps_and_their_record, component/test_root_unwinds_a_failed_start
- request_stop reaches stopped components -> KILLED by component/test_root_lifecycle_steps_and_their_record
- a failed start not unwound -> KILLED by component/test_root_unwinds_a_failed_start
- the failing component counted as started -> KILLED by component/test_root_unwinds_a_failed_start
- no fault sink attached -> KILLED by component/test_root_routes_faults_by_component
- start not recorded -> KILLED by component/test_root_lifecycle_steps_and_their_record, component/test_root_unwinds_a_failed_start
- the destructor leaves components running -> KILLED by component/test_root_lifecycle_steps_and_their_record
- no default fault handler -> KILLED by component/test_root_routes_faults_by_component
- providers never counted down -> KILLED by component/test_root_lifecycle_steps_and_their_record, component/test_root_starts_providers_first
- request_stop takes no lock -> KILLED by component/test_root_stop_request_during_a_start_reaches_the_started
- root stopped before status -> KILLED by lifecycle_record/test_lifecycle_of_fi_1, lifecycle_record/test_lifecycle_of_gbni_1
- root never asked to stop -> KILLED by lifecycle_record/test_lifecycle_of_fi_1, lifecycle_record/test_lifecycle_of_gbni_1

# T2: the wait guard's two unseen paths (assessment point 6)

- **A cold catalogue:** `contract/test_the_wait_guard_on_a_cold_catalogue`
  builds a fresh `CatalogueManager` over a real node: a control context
  entering its `snapshot_view` throws in the guard's test mode, a loader
  context loads it, and control may then enter the warm catalogue. Removing
  the cold-path guard fails it (mutation-proven); 10/10 repeated.
- **The manage route** (`POST /api/v1/manage/nodes/{id}/identity-association/reset`
  without a host, for a node absent from live membership,
  `src/api/manage_api.cpp:730`) passes a control context into
  `MetadataManager::snapshot_view`, which is declared to wait on the state
  device and the network. Whenever that branch runs it is a violation:
  control work that may wait on a metadata refresh from the replicas. Not
  fixed: the candidate (`available_snapshot_view()`, waits on nothing, may
  read an older snapshot for this fallback lookup) changes behaviour, and
  is the operator's decision.

## fi-1 (2026-10-02)

Built and run on fi-1 as part of the `-t3` tip (`dac5098`), which contains
T2 unchanged: GCC, no warnings; suites 700/700, 17/17, 21/21;
`src/component/composition_root.cpp` 100%, `dependencies.hpp` 100%,
`contract/published.hpp` 91.8% (one translation unit's figure; the refusal
it misses is tested in `test_contract`). Details in the T3 README.

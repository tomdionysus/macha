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

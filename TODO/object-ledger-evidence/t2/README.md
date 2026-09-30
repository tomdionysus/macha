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

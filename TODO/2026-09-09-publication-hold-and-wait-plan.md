# Plan: publication hold-and-wait on the retained-memory ledger

Date: 2026-09-09 (evening, after the continuation note)

Supersedes the "where to look next" section of
`2026-09-09-publication-livelock-continuation.md`. That note's hypothesis
("each failed-then-retried publication leaves its leases behind, so the retry
loop *is* the accumulation") was checked against the code and is wrong in
emphasis; the corrected diagnosis is below and drives a different fix.

## Diagnosis (corrected)

Every byte of `owners.publication` is a `WriteHandle` extent lease: the only
two `MemoryOwner::publication` acquires in the tree are in
`WriteHandle::ensure_buffer_memory` (`src/filesystem.cpp:314`, `:341`). A lease
lives in `buffer_memory_` while the extent is being filled, moves into the
`PendingExtent` on `flush()` (`src/filesystem.cpp:443-449`), and is released
only when that extent drains (`drain_one_extent`, `:540-541`) or the handle is
destroyed. `cleanup()` (`:1438`) releases nothing.

es-1 runs `extent_size: 4M`. Its equilibrium is exact:
durable-lower 536,870,912 − `owners.fuse_operation` 19,462,464 = 517,408,448,
floored to whole extents = **123 leases = 515,899,392**. The loader has filled
the durable-lower budget to the last whole extent, which is why the number is
byte-identical across restarts.

Those 123 leases are **not** held by the eight (on es-1, three: 96M / 32M)
running workers — they can hold at most 3 each (1 buffer + 2 pipelined). They
are held by writers retained on inodes that are *not running*:

1. `yield_quantum` (`src/fuse_frontend.cpp:3465-3475`) drains pipelined
   extents but keeps the partial-extent `buffer_memory_` on the retained
   writer. A yield that does not land on a 4 MiB boundary leaves one lease
   behind. With playback active, viewer-driven yields land at 256 KiB chunk
   granularity, and appended files are misaligned from byte one.
2. Scheduling is breadth-first. `admit_deferred` (`:2763-2803`) re-queues
   every due inode in id order, so a yielded inode goes behind the whole
   backlog. First quantum on file 1, first quantum on file 2, … ~123 files
   each holding one partial buffer before any file gets its second quantum.
3. Then every writer needs one more lease and none can be released: classic
   hold-and-wait. A retryable failure retains the writer with its leases
   (`:4012-4020`), so `EAGAIN` from the no-progress deadline does not unwind
   it either. A fresh inode admitted onto the full ledger fails holding zero
   leases; the 30 s cycle adds nothing, it just cannot recover anything.

Nothing about the ledger's gate ordering or any budget setting changes this.
Any budget fills the same way because the number of writers holding partial
state is unbounded — it is the breadth of the backlog.

Two secondary defects found on the way:

- **The no-progress deadline watches admissions, not progress.**
  `data_publication_quanta` increments when a quantum is *admitted*
  (`src/fuse_frontend.cpp:3854`) and is the counter `ensure_buffer_memory`
  polls (`:3435`). Every failure frees a slot, a fresh inode is admitted, and
  every other waiter's 30 s window re-arms. With three slots that is exactly
  one failure per 30 s, round-robin, always a fresh inode with `attempts=1` —
  the log shape the continuation note called "the strongest clue" is this
  artefact.
- **`drain_one_extent` waits forever.** `flush()` (`src/filesystem.cpp:439`)
  spins on `drain_one_extent`, which blocks in `future::get()` with no
  deadline and no cancellation check. If an extent put stalls, publication
  blocks there silently — the same "never fails, never completes" shape one
  layer down. Not what es-1 is doing now, but live.

Rejected: dropping the writer on `EAGAIN` (the continuation note's
"release-on-failure"). It frees nothing today — the failing writers hold
nothing — and when a holder is eventually re-admitted and fails, dropping it
discards the generation cursor, so a multi-GB file replays from spool byte
zero. Under contention that is strictly worse than the current state.

## Invariant the fix establishes

> The number of inodes holding a provisional writer is bounded such that every
> one of them can hold its worst-case leases (one buffer plus the pipeline)
> simultaneously within the loader's guaranteed share of the ledger.

With that, hold-and-wait is impossible by construction: a writer waiting on
the ledger is only ever waiting for control/viewer work, which releases.
The retained-cursor design (writer kept across yields and retryable failures,
no spool replay) stays exactly as it is.

## Steps

### 0. Reproduce in a test before touching anything

`tests/test_filesystem_fuse.cpp`, next to
`test_fuse_retryable_publication_failure_preserves_cursor` (`:2032`), which
is the fixture shape to copy:

`test_fuse_publication_backlog_wider_than_ledger_completes`
- `extent_size = 1 MiB`, `publication_quantum_bytes = extent_size`,
  `publication_pipeline_bytes = extent_size`, `commit_workers = 2`,
  `publication_quiet = 0ms`, `publication_no_progress_deadline` short (≈1 s).
- `runtime.retained_memory_bytes` small enough that durable-lower fits only a
  handful of writers (e.g. control 4 MiB, viewer 4 MiB, loader 4 MiB, total
  ≈ 16 MiB → durable-lower ≈ 8 MiB ≈ 8 partial buffers).
- Create and release ~16 files of `3 × extent_size + 12345` bytes each (the
  odd tail forces a partial buffer at every yield; three extents forces
  multiple quanta per file so breadth-first interleaves them).
- Assert `wait_for_idle` succeeds, `data_publications_completed == 16`,
  `parked_publications == 0`, and `owners.publication == 0` afterwards.

Expected on the current tree: hangs until the idle timeout, with
`owners.publication` pinned at the durable-lower budget. If it does **not**
hang, the diagnosis above is incomplete — stop and re-examine before building
the fix (the split of the 123 leases between yielded and failed writers was
inferred from code, not measured; gdb on es-1 can count `WriteHandle`
instances and their `buffer_.size()` / `pending_extents_.size()` if needed).

### 1. Bound open publications (the fix)

`src/fuse_frontend.cpp`:

- Add `std::atomic_size_t open_publications` to `State`, maintained under the
  inode mutex at every place `Inode::data_publication` is assigned or reset:
  created `:3885-3890`; reset at `:3947` (ESTALE replay), `:3970` (parked),
  `:4020` (completed/error/parked). Audit `abandon_data` and inode reclaim
  for any other reset. One helper `set_data_publication_locked(inode, ptr)`
  so the count cannot drift.
- Add `size_t publication_writer_cap` resolved once at startup. Recommended
  derivation (decision point, see below):
  `max(commit_workers, loader_memory_reserve_bytes / per_writer_worst)`, where
  `per_writer_worst = extent_size + publication_pipeline_bytes`. Expose it as
  `fuse.publication_max_open_writers` (0 = derive) for operators. Validation
  warns when `commit_workers × per_writer_worst > loader_memory_reserve_bytes`,
  because then the floor binds and the invariant relies on durable-lower
  headroom rather than the guaranteed loader reserve.
- Gate in `runnable_data_locked` (`:3656-3747`): an item is admissible iff
  its inode already holds a `data_publication` **or**
  `open_publications < publication_writer_cap`. Apply the predicate in the
  closed-file `find_if`, in the retirement-score candidate loop, and in the
  final `data_queue.begin()` fallback. Inodes with writers are always
  admissible, so once the cap is reached the scheduler is depth-first over
  the open set until one completes — which is the point.
- Because `runnable_data_locked` is the wait predicate for `data_loop`, an
  inadmissible queue must not spin: the existing `data_cv.wait` on
  `runnable_data_available_locked` covers it provided every
  `data_publication.reset()` site is followed by `data_cv.notify_all()`
  (the `:4045` notify already covers the worker path; check `:3947`).
- Counter `data_publications_deferred_by_writer_cap` for diagnostics.

### 2. Count progress, not admissions

- `FileSystem` gains `std::atomic_uint64_t publication_progress_` (or the
  frontend's counter is passed as a non-const pointer — pick whichever keeps
  `DataWorkContext` simple). `WriteHandle::drain_one_extent` increments it
  after `pending_extents_.pop_front()`; `commit()` increments it once more.
- `replay_data_quantum` (`:3434-3436`) passes that counter as the progress
  pointer instead of `&data_publication_quanta`. `data_publication_quanta`
  keeps its current meaning and status field.
- Test: ledger held full by a viewer lease, two workers, two files; assert
  both publications hit the no-progress `EAGAIN` within ≈ 2 × budget (on the
  current tree the second one's window is re-armed by the first one's
  replacement admission and it never fires).

### 3. Bounded wait in `drain_one_extent`

`src/filesystem.cpp:515-543`: replace `pending.result.get()` with a
`wait_for` loop in the same 500 ms slices `ensure_buffer_memory` uses,
checking `work_context_.cancelled()`, the absolute deadline, and the same
no-progress budget against the progress counter from step 2. On expiry throw
`FsError(EAGAIN, "extent put made no progress within budget")`, leaving the
`PendingExtent` at the queue head with its future still valid; the retry
path's `!pending.result.valid()` relaunch logic is unchanged and a still-valid
future is simply waited on again. Test needs a `stall_extent_put_for_tests`
hook in `DistributedStore`; if that is more than a few lines, defer this step
to its own change — it is not what es-1 is stuck on.

### 4. Diagnostics

`src/status_api.cpp` filesystem section: `open_publications`,
`publication_writer_cap`, `data_publications_deferred_by_writer_cap`. Add to
`docs/configuration.md` alongside `publication_no_progress_deadline_ms`.

### 5. Commit order

1. The uncommitted reassembly-reserve + no-progress-deadline work first, on
   its own (it is proven on the cluster and green: 380 + 8).
2. Steps 0–2 (+4) as one commit: the test, the cap, the counter.
3. Step 3 separately if done.

### 6. Deploy and verify

Check playback sessions first (rolling restarts vs viewers). Deploy to es-1
first — it is the node that reproduces.

- Revert es-1's config drift: remove `fuse.publication_inflight_bytes: 96M`
  and its comment block from `/etc/macha/macha.yaml` (backup
  `macha.yaml.bak-20260909-inflight`). The other two nodes never had it.
- After restart, watch (anonymous session, recipe in the continuation note):
  `owners.publication` must **not** climb to the durable-lower budget;
  `data_publications_completed` must become > 0 within minutes;
  `spool_publish_rate_bytes_per_second` > 0; `open_publications ≤ cap`.
  The 8.5 GB spool should visibly drain.
- Then gbni-1 and gbni-2, 5–10 min apart.

## Implementation status (2026-09-09, later the same evening)

Steps 0, 1, 2 and 4 are implemented. Step 3 is deferred, deliberately.

**Step 0 answered the gate, and the diagnosis holds.** The repro test was run
both ways against the same binary. Unbounded (bound set to 1000), the 20-file
backlog opens **20 writers at once**, drives retained memory to 15.76 MB of the
16 MB durable-lower budget, and only escapes through the no-progress deadline:
**18 retryable failures** and **25.1 s** wall clock. Bounded at 4:
`peak_open_publications` = 4, **zero** backend failures, **6.8 s**. That is the
es-1 shape reproduced and removed. (es-1 itself had no escape hatch, because
its deadline did not exist until today -- the two changes compose as designed:
the deadline turns the deadlock into a livelock, the bound removes the
livelock.)

**Step 1** is `fuse.publication_max_open_writers`, resolved in
`config_base.cpp` validation and again in `FuseFrontend::State`'s constructor
for in-process callers with an unvalidated `FuseConfig` (the same two-place
pattern `publication_pipeline_bytes` already uses). `set_data_publication_locked`
is now the only place `Inode::data_publication` changes, so the count cannot
drift; `runnable_data_locked` refuses inodes without a writer once the bound is
reached.

The `data_loop` wait restructure was the one genuinely risky part and it did
break three existing scheduling tests on the first attempt: hoisting
`runnable_data_available_locked()` above the empty-queue check meant a worker
that slept on an empty queue no longer woke when work arrived that was not
*immediately* runnable, and `test_fuse_spool_pressure_selects_nearest_retirement`
hung for its full 60 s. The landed version keeps the original empty-queue
branch and its `!data_queue.empty()` predicate exactly as they were, and only
adds the deferred due time to the *non-empty* path -- which is the case the
bound actually introduces.

**Step 2** turned out to have a better test than the one planned. The planned
timing test (K blocked workers must fail within one budget, not K) is real but
fragile in CI. Instead the counter is exposed as
`data_publication_progress_events` and the test asserts the structural property
directly: a publication yields far more often than it retires an extent, so
`data_publication_quanta > data_publication_progress_events`. If the deadline
watched admissions those would be the same counter and the assertion would be
an equality. Deterministic, 0.8 s.

**Step 3 (bounded wait in `drain_one_extent`) is not done.** `DistributedStore`
has no fault-injection hook at all today, so `stall_extent_put_for_tests` is a
new mechanism in a core class rather than the few lines the plan allowed for.
Landing the wait change untested would add a new `EAGAIN` path to publication
on the strength of inspection alone. It is the same defect class and still
live, but it is not what es-1 is stuck on. It should be its own change.

**Step 4** adds `open_publications`, `peak_open_publications`,
`publication_max_open_writers`, `data_publication_selections_under_writer_cap`
and `data_publication_progress_events` to the Status filesystem section, and
documents the setting in `docs/configuration.md`.

New tests: `test_fuse_publication_backlog_wider_than_ledger_completes`,
`test_publication_progress_counts_releases_not_admissions`,
`test_publication_open_writer_bound_fits_the_loader_reserve`.

## Decision points for Tom

- **Cap formula.** `max(commit_workers, loader_reserve / per_writer_worst)`
  gives es-1 `max(8, 64M / 12M = 5) = 8` — worst case 96 MiB against a
  536 MiB durable-lower, fine. An alternative is a fraction of durable-lower
  (e.g. half → 22 on es-1), which allows more breadth but ties the bound to
  headroom that `fuse_operation` and speculative work also draw on. I lean to
  the reserve-based bound: it is the only share the loader is actually
  guaranteed.
- **Include step 3 now or later.** It is the same defect class but not the
  live fault; I would take it now only if the test hook is cheap.
- **Measure the 123 split first?** Optional gdb pass on es-1 before step 0.
  The test in step 0 is the real gate either way.

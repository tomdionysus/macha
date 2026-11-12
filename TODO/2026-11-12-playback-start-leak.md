# The async playback start leaks its candidate session (2026-11-12)

Found by the ASan stage of the new CI gate (`tools/ci.sh`, LeakSanitizer on, Debian trixie, GCC 14) on a fresh
clone of `0fefebe`: `media_playback/test_an_async_start_answers_at_once_and_reports_its_progress` leaks 34,716
bytes in 175 allocations. Every allocation is an indirect leak, which is the signature of a `shared_ptr` cycle.
The gate stops there; the coverage stage did not run. The branch is local and unpushed because GitHub CI would
go red at the same step.

Status: **found, reported, plan checked, waiting on the operator's go.** Fixed in Macha first, as its own commit,
before the push and before 0.94.1.

## The defect

`src/playback/playback.cpp`, the async start path of `create`:

```cpp
auto start = std::make_shared<StartState>();
start->candidate = copy_for_start(*session);
start->candidate->start = start;        // the cycle
session->start = start;
```

The `StartState` owns the candidate session and the candidate owns the `StartState` back. Nothing clears either
link, so every POST with an async start that needs a transcode leaks the candidate and all it holds: plan,
probe, source lease, subtitle cache, and on failure the start's error JSON. The test makes eight such starts
and leaks eight. The link dates from `63642c8` (2026-09-28, async start stage 1), so both nodes have leaked
one session per async transcode start since.

The PATCH replacement path is clean: it sets only `start->candidate` and never the back-link, and no leak root
is in it. Every leak root is `copy_for_start` or `create`.

## Why the links exist

- `session->start` on the live session (the candidate once it is swapped in at the ready point) is read by GET
  (the long-poll choice and the `start` block of the status JSON), `start_pending`, `creation_response` and
  `tear_down_session`. All read it without a lock: it is written once before the session is published and
  treated as immutable.
- `start->candidate` is read by the start worker at its top, by `cancel_pending` (PATCH), by
  `tear_down_session` (to stop a candidate still running beside the placeholder) and by the destructor.

## The plan, checked against every reader

**Cut `start->candidate`, not the back-link.** Clearing `session->start` on a live session would race the
unlocked readers above and would drop the `start` block GET reports after ready. The worker is the last thing
that needs the start to own its candidate; every later reader is content with null:

- `tear_down_session` compares `candidate != removed`; for a live candidate that is already a no-op.
- The destructor stops the candidate; for a live one its session loop stops it anyway.
- `run_start` copies the candidate at its top.

So one statement at the end of `run_start`, after the try and catch, under `start->mutex`:
`start->candidate.reset()`. It covers success, failure and the cancelled race alike. Ownership becomes the
session map, then the live session, then the start, then nothing. On failure the placeholder has been erased
from the map, the worker's local copy is the candidate's last owner and it dies at return; the start is held
by the placeholder and the thread lambda, which die with the thread.

**What it needs.** `candidate` becomes mutable, so it is `MACHA_GUARDED_BY(mutex)` on the start and its readers
take `start->mutex` to copy it out before acting: `cancel_pending`, the two workers' first lines,
`tear_down_session` and the destructor. The two writers (`create`, the PATCH path) take the lock on a start
nobody else can yet see. Lock order is unchanged: `mutex` before `start->mutex` is the existing direction and
is never reversed; the reset in the worker happens outside `mutex`. `stop_pipeline` never reads
`session.start`, so stopping a candidate concurrently with the reset is safe.

**Rejected: a `weak_ptr` candidate.** In both paths the start is the sole owner of the candidate between its
creation and the worker's copy; the thread lambdas capture the placeholder and the start, not the candidate.
A weak candidate would die before the worker ran.

**Rejected: a session instance counter.** A repeat-cycle lifecycle test for this needs a seam that counts
live sessions, which would be a status field and so an API change for every client. The test that caught
the leak is the repeat-cycle case (eight starts), and LeakSanitizer in the gate now holds it.

## Verification

LeakSanitizer runs on Linux only, so: the `asan` preset in the CI container on a clone carrying the diff,
`media_playback` first, then the full `tools/ci.sh` on a fresh clone of the commit. Then the push, then 0.94.1.

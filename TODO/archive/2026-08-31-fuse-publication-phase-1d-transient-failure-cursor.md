# Phase 1D.4 checkpoint: transient publication cursor preservation

Date: 2026-08-31

Status: implementation and complete local verification are green; deployment
and loaded failure UAT remain pending.

## Problem

Publication already retained its provisional `WriteHandle`, WAL operation index
and byte offset across ordinary scheduler yields. Any retryable backend error,
however, reset that complete process-local cursor. The next attempt reopened a
writer and replayed the generation from byte zero. A slow or intermittently
failing object path could therefore multiply spool reads, materialisation,
hashing and object work without making the final atomic commit any closer.

Pipelined extent puts made a simple outer-state retention unsafe. The failed
future was removed from the pending queue before its result was inspected, so a
retained writer could otherwise continue with a manifest hole.

## Implementation

- A pipelined pending extent now retains its bounded immutable payload, exact
  manifest offset and cache policy until its future succeeds.
- A failed future remains at the queue head. Its content-addressed put is
  relaunched on the next publication attempt; later futures may finish in
  parallel but cannot enter the manifest ahead of the missing extent.
- Draining stops at the first failed offset instead of discarding every pending
  result and then reporting the first exception.
- Retryable publication failures now retain the same `DataPublication`, writer,
  spool descriptor, operation index/offset and resumable materialisation/rebuild
  state. Completion and terminal inode errors still discard it.
- The retry remains event-driven through the existing deferred publication
  path. No maintenance loop or new polling owner was added.
- A test-only, non-configurable one-shot injection point raises `EIO` after a
  specified amount of accepted spool input.

Durable spool and journal data remain the crash authority. This checkpoint only
preserves safe process-lifetime progress; a process crash still replays the
generation from durable input.

## Deterministic proof

`filesystem_fuse/test_fuse_retryable_publication_failure_preserves_cursor`
publishes a file larger than three extent-sized quanta and injects one retryable
failure after the first extent. It proves:

- exactly one backend failure and one publication start;
- exactly one successful publication completion;
- total spool bytes read equal the file size, with no repeated prefix;
- completed-cohort spool bytes equal the file size; and
- the final atomically visible bytes exactly match the input.

The adjacent pipeline-failure regression also remains green, preserving the
rule that a failed provisional pipeline cannot expose partial content.

## Verification

- Build: passed.
- New focused regression: passed (and passed again directly after the complete
  suite).
- Existing pipeline-failure atomicity regression: passed.
- Filesystem/FUSE group: **58/58 passed**.
- Complete backend suite: **229/229 passed** with four worker slots.
- Runtime dependencies: **3/3 passed**.

Two attempted shell-wrapped repetitions never entered the test body because
the managed sandbox denied all bind probes in the test's port namespace. The
same test invoked directly before and after the complete suite passed; these
environmental invocation failures are not recorded as product-test failures.

## Remaining Phase 1D.4 work

- Reserve at least one retirement-capable loader ticket under spool pressure so
  speculative and unrelated lower-class work cannot consume every useful DATA
  opportunity.
- Coalesce compatible append metadata generations where safe.
- Perform a loaded UAT with an injected or naturally observed transient DATA
  failure and verify the publication start/cursor counters do not reset.


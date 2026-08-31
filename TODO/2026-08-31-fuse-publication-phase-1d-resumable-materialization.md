# Phase 1D.2 checkpoint: resumable materialisation

Date: 2026-08-31

Status: loader materialisation cut complete locally; rebuild, commit and the
overall Phase 1D invariant gate remain open.

## Defect addressed

A non-sequential FUSE publication write called `WriteHandle::materialize()`
inside one outer loader quantum. That function synchronously fetched and wrote
the complete old generation before the scheduler regained control. A one-byte
overwrite of a multi-gigabyte file could consequently perform gigabytes of
lower-priority work ahead of newly arrived viewer demand.

## Implementation

- Added an in-memory materialisation cursor to `WriteHandle`, retaining the
  current durable FUSE spool and journal as restart authority.
- Added `prepare_write(offset, byte_budget)`. It advances old-generation
  staging by no more than the supplied DATA byte grant and returns both exact
  bytes processed and whether the target write is ready.
- Ordinary filesystem writers preserve their synchronous API by driving the
  same state machine to completion with an unlimited local call.
- FUSE publication now charges preparation bytes to the existing publication
  quantum before reading or consuming the next spool operation. An incomplete
  preparation cleanly retains its staging and replay cursors, drains bounded
  provisional work, and requeues through the existing event-driven scheduler.
- Immutable extents, holes, partial tails and an existing sequential buffer are
  copied incrementally. A partial immutable tail is restored to the manifest
  for non-sequential fallback rather than fetched eagerly outside the cursor.
- Added diagnostics for cumulative source bytes and materialisation steps.

Atomic visibility is unchanged: preparation only writes a private temporary
file. The authoritative manifest still changes only at the final metadata
commit.

## Tests

`filesystem_fuse/test_loader_materialization_is_resumable_and_byte_bounded`
uses a four-extent base and proves one exact extent is processed per grant,
source extents are not repeated, old authoritative size remains visible during
every step, and the final one-byte overwrite is exact.

`filesystem_fuse/test_fuse_overwrite_materialization_yields_between_quanta`
exercises the complete FUSE durable-spool path. With a one-extent grant, a
four-extent existing file yields at least four times before its one-byte WAL
operation is consumed, then publishes the exact final generation.

The earlier provenance test now uses a partial final extent and proves that its
fetch also retains loader classification and is included in materialisation
byte accounting.

Verification after the final changes:

- project build: passed;
- `filesystem_fuse/`: 53/53 passed.

The complete project suite was not run at this checkpoint. The two targeted RPC
control-isolation tests passed in the immediately preceding 1D.1 checkpoint;
this cut did not change the transport or RPC executors.

## Remaining risk and next cut

`rebuild()` is still synchronous. After materialisation and the overlay write,
`commit()` can reread and hash the complete temporary file and can issue extent
puts without returning to the scheduler. The global `publication_mutex` still
spans that rebuild, durability barrier and metadata commit. Append-tail seeding,
pending-pipeline draining, trace-level full-file diagnostics and individual
object operations also need resource-specific accounting beyond this source
copy cursor.

The next cut must make rebuild resumable and move it out of the global commit
critical section. Until that is complete, live UAT cannot establish the
end-to-end viewer/control invariant.

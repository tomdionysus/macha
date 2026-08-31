# Phase 1D.2 checkpoint: sparse changed-range reconstruction

Date: 2026-08-31

Status: changed-range reconstruction complete locally; Phase 1D remains open.

## Defect addressed

The resumable rebuild checkpoint bounded how long a loader could run before
yielding, but a one-byte overwrite still materialised, reread and hashed every
extent in the file. Interleaved or resumed FUSE writes could therefore turn a
small changed range into whole-file disk, network and hashing work.

The first loaded DATA-headroom UAT appeared to show 4.76 GB read for 697 MB
committed. Those cumulative counters did not describe the same publication
cohort: the read counter included five active publications while the committed
counter included only one completed publication. The apparent 6.8x ratio was
therefore not a valid amplification measurement, although the underlying
whole-file rebuild path was real.

## Implementation

- A canonical committed manifest can now enter sparse-overlay mode without
  copying its immutable extents into a temporary file.
- Non-sequential writes and shrinking truncates record sorted, merged changed
  byte ranges. Overlapping writes are represented once.
- Rebuild walks canonical extent boundaries under the existing resumable DATA
  quantum. An untouched extent is reused directly by immutable object ID and is
  neither fetched nor hashed.
- A touched extent is seeded from the applicable handle-local or committed
  immutable extent, then only the intersecting overlay ranges are applied. This
  preserves partial-extent bytes while bounding source work to touched extents.
- A sequential append followed by an overwrite uses the handle's provisional
  immutable extent as its seed, so changing direction cannot substitute zeros
  or stale committed data.
- Shrink followed by re-extension preserves POSIX zero-fill semantics by
  recording the truncated range as changed before the sparse overlay is
  truncated.
- Atomic visibility is unchanged: readers continue seeing the complete old
  generation through every rebuild yield, and the new manifest becomes visible
  only after durability and metadata publication succeed.
- After commit, the temporary overlay is discarded and a still-open handle
  returns to the append-capable immutable-manifest path.

## Cohort-correct diagnostics

Status now exposes counters accumulated only when a publication completes:

- `data_publication_completed_spool_bytes_read`;
- `data_publication_completed_source_bytes_read`;
- `data_publication_completed_reused_extents`; and
- `data_publication_completed_put_extents`.

These counters share the same completed-publication cohort as
`data_publication_bytes_committed`. They make the next loaded UAT an
apples-to-apples amplification measurement while retaining the existing
in-progress counters for operational progress.

## Deterministic proof

The new eight-extent regression applies two overlapping writes in one extent
and a second change in a distant extent. It proves:

- zero whole-file materialisation bytes;
- exactly two old extents plus six unique overlay bytes read;
- six untouched extents reused without source reads;
- exactly two changed extents put;
- eight extent-aligned resumable checkpoints;
- old-generation visibility at every checkpoint; and
- exact final bytes after one atomic commit.

The same test also covers append-then-overwrite direction changes. Existing
truncate/read-overlay coverage found a shrink/re-extension regression during
the complete filesystem run; marking the invalidated tail as changed corrected
it, and that coverage now passes with the sparse path.

## Verification

- Build: passed.
- `filesystem_fuse/`: 55/55 passed.
- Complete suite at the default 12 workers: accurately recorded as 225/226.
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
  hit its 10-second assertion under load; the changed-range regression passed.
- The catalogue burst test passed alone in 1.048 seconds.
- Complete suite with four isolated workers: 226/226 passed, including both the
  catalogue burst test and changed-range regression.
- Runtime dependency suite: 3/3 passed.

The default-concurrency failure is not represented as a passing suite. Its
isolated and controlled-full-run results show resource sensitivity rather than
a changed-range functional regression.

## Live observation context

The three-node observation immediately before verification used the previously
deployed binary, not this sparse-overlay implementation. During active ingest,
node 200 alternated between idle and roughly one-core metadata bursts while the
cluster advanced from generation 965 to 1002. In about three minutes it handled
37 accepted metadata heads, 111 read-ahead RPCs and 943 historical snapshot
requests. Node 50 simultaneously accumulated hundreds of thousands of
coalesced publication requests.

That observation does not validate this checkpoint, but it demonstrates that
publication amplification is cluster-wide: excessive publication generations
also make every metadata replica work. Sparse changed ranges remove whole-file
data reconstruction; Phase 1D.4 still needs to suppress redundant publication
notifications and reduce metadata-generation frequency.

## Continuation boundary and UAT

A short loaded UAT is useful after deployment. Run the existing
`rsync --append-verify` workload with real playback/seeks and compare only the
new completed-publication counters. Acceptance for this slice is that unchanged
extents are reused, completed source reads are proportional to touched extents,
viewer waits/timeouts remain zero, and loader retirement remains non-zero.

This checkpoint does not complete Phase 1D. Restart-boundary fault injection,
asynchronous durability completion, origin-aware metadata admission,
resource-specific budgets, and Phase 1D.4 request/generation coalescing remain
active.

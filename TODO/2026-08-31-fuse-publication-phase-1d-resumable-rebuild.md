# Phase 1D.2 checkpoint: resumable rebuild and commit-convoy removal

Date: 2026-08-31

Status: rebuild scheduling cut complete locally; Phase 1D remains open.

## Defects addressed

After resumable materialisation, final `WriteHandle::commit()` still reread,
hashed and classified every extent in the temporary file synchronously. The
FUSE worker then held a frontend-wide `publication_mutex` across that rebuild,
the distributed durability barrier and metadata mutation. One conflicting file
could therefore hide another complete file behind an entire generation of disk,
object and metadata work.

## Implementation

- Replaced synchronous rebuild internals with a persistent per-handle cursor.
  Each step reads and hashes complete canonical extents up to the supplied DATA
  byte grant and returns exact bytes processed plus ready/yield state.
- Avoided an O(file) reusable-extent map before the first yield. Each canonical
  extent looks up a same-offset candidate in the already sorted handle/base
  manifests, with handle-local immutable state retaining precedence.
- Preserved immutable extent reuse. Changed extents alone are submitted to the
  distributed store and accumulated in the existing generation durability
  batch.
- Added `prepare_commit(byte_budget)` so FUSE rebuild begins only on a fresh
  extent-aligned loader grant. Arbitrary WAL write lengths cannot fragment the
  rebuilt manifest.
- Ordinary writers retain synchronous `commit()` semantics by driving the same
  cursor to completion. Further writes/truncates correctly invalidate an
  already prepared rebuild.
- Exposed cumulative rebuild source bytes and rebuild steps in handle
  diagnostics.
- Removed the frontend-wide `publication_mutex`. It was not the metadata
  consistency authority: `MetadataManager::mutation_mutex_` serializes metadata
  mutations, `FileSystem::open_writes_mutex_` preserves rename/write-handle
  ordering, and content-version checks reject conflicting generations. Those
  narrower existing invariants remain.
- Trace-level pre-rebuild verification no longer performs a hidden whole-file
  diagnostic scan in bounded FUSE mode; normal per-extent diagnostics remain.

Atomic visibility remains unchanged. Prepared extents and provisional object
puts are invisible until the durability barrier and metadata mutation succeed.

## Deterministic verification

The resumable large-base test now proves separately that:

- materialisation processes one extent per loader grant;
- rebuild rereads/hashes one canonical extent per fresh grant;
- authoritative readers see the complete old byte generation at every cursor
  boundary;
- three unchanged extents are reused and only the one changed extent is put;
- no source extent is repeated; and
- final bytes are exact after one atomic commit.

The end-to-end FUSE overwrite test now requires at least nine clean scheduler
yields for four materialisation steps, WAL/rebuild alignment, and four rebuild
steps before final publication.

Verification after the final changes:

- project build: passed;
- `filesystem_fuse/`: 53/53 passed, including rename/unlink ordering, recovery,
  concurrent loader, atomic pipeline and publication fairness coverage.

The complete project suite was not run.

## Deployment checkpoint

Deployed natively built binaries to Linux nodes 50 and 51 on 2026-08-31. Both
nodes passed the focused resumable materialisation/rebuild test and end-to-end
FUSE overwrite-yield test before installation. Node 50's first FUSE test attempt
was refused by the physical spool reserve because `/tmp` is a 2 GiB tmpfs; the
unchanged test passed with `TMPDIR` on its root NVMe filesystem. The deployed,
installed and running binaries on both nodes have identical SHA-256
`0607671e9211df613640fec64f06d7f0f09ee46c5e009b1ec4f3b0987027c526`.

Node 50 required a hard host reset after an eight-way native build coincided
with the loaded machine becoming unreachable over SSH. The low-parallelism
retry completed normally. On restart, recovery discarded three unreferenced
spool files (9,011,986,432 + 3,148,587,518 + 5,019,246,248 bytes) because the
operation journal contained no history for their inodes and they exceeded the
orphan budget. Those unpublished bytes must be resent from their original
source. After deployment node 50 reported writable metadata at generation 759
with 3/3 replicas; node 51 reconnected successfully.

## Remaining work

This bounds rebuild interruption latency but does not yet eliminate the
amplification: a one-byte conflicting overwrite still rereads and hashes every
extent, now over multiple fair quanta. Phase 1D.2 still needs changed-range
tracking so only touched/boundary extents are reconstructed, plus restart fault
injection at cursor boundaries.

The distributed durability barrier and metadata mutation remain synchronous
after rebuild. `MetadataManager::mutation_mutex_` intentionally serializes
metadata publication but currently spans network/durability work and is not yet
origin-aware. Phase 1D.3 must provide bounded loader-origin metadata admission,
viewer/control headroom, and completion tickets before the end-to-end priority
invariant can be claimed.

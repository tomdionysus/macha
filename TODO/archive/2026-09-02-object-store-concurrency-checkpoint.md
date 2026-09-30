# Object-store concurrency checkpoint

Date: 2026-09-02

Status: Phase 1 locally complete; not deployed

## Result

The first Phase 1 store-global serialization tranche is removed without
changing object identity, encryption, packing, durability or recovery formats.

- Same immutable object ID: one exact per-object single-flight domain.
- Store metadata/index/accounting: short critical sections only.
- Loose payload reads, writes and removals: physical I/O and AES-GCM outside the
  index mutex.
- Packed reads: the pack inode is opened while the index is stable, then read,
  decrypted and hashed after releasing the index mutex.
- Packed writes: encryption and append I/O run outside the index mutex, with a
  distinct single-writer pack stream and capacity reserved before release.
- Pack compaction: operates from an immutable index snapshot and switches the
  live index only after the replacement is installed; unrelated reads proceed
  during the physical rewrite.
- Filesystem free-space inspection no longer executes under the index mutex.
- The first crash-accounting dirty-marker fsync is single-flight but releases
  the index mutex; unrelated viewer reads remain free while it completes.
- Physical validation/retention/deletion RPCs execute on the priority-aware
  DATA executor, not CONTROL workers. Inner resource admission is non-blocking
  to prevent bounded worker-pool inversion across peers.
- Local repair, retention and cache promotion carry speculative/loader work
  class into physical admission; viewer headroom remains reserved.
- Deferred durability batches retain an exact non-dominated frontier per
  physical replica set rather than one requirement per sequential extent.

## Deterministic regressions

The tests deliberately stall physical work and require an unrelated viewer
read to complete before releasing it:

- blocked loose loader write;
- blocked packed read;
- blocked packed loader write, while a second same-ID put remains single-flight;
- blocked pack compaction, for both packed and loose viewer reads.

Current verification:

- `storage_v18`: 21/21 within the final complete run;
- `storage_metadata`: 37/37;
- priority/repair/RPC subset: 40/40;
- complete core suite: 267/267 at 12-way process isolation;
- runtime dependencies: 4/4.

Intermediate complete-suite runs exposed, rather than waived, two ordering
problems. Retention/deletion were initially sent with a non-control frame that
the protocol had not authorised; this caused six deterministic failures and was
fixed in frame semantics. A heavy returning-node retention test then exposed a
possible bounded-worker wait cycle under suite load. Storage validation now
uses non-blocking inner physical admission. The exact heavy test, the 40-case
parallel RPC subset, and the final 267-case suite all pass.

## Remaining before UAT

- run Linux architecture-specific durability/accounting verification at the
  next deployment checkpoint;
- complete Phase 2 retained operation/checksum/publication-snapshot bounds;
- complete Phase 3 process-wide retained-memory governance.

Loaded rsync/playback UAT is not yet useful. Phase 3 retained-memory governance
remains a prerequisite for reopening a sustained load test.

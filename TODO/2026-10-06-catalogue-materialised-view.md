# The catalogue as a materialised view of the local head

Status: agreed design, 2026-10-06. Not started. The P0 stopgap (a `cache()`
that refuses an older generation) is in the tree; this replaces it.

## The rule

The catalogue view on a node is a pure function of that node's own metadata
head: `view = f(head.catalogue_root)`. Nothing else produces it. Two nodes
holding the same root hold byte-identical views, and any assertion about the
catalogue is an assertion about a root hash.

This is the law the namespace already obeys. The tree is derived from the
head, FUSE reads it, the cluster converges behind. The catalogue is the one
subsystem that keeps its own notion of "current", refreshed by polling and
a TTL and written from two paths. Both failures of
`test_catalogue_uses_final_state_after_coalesced_metadata_burst` are that
exception showing through.

## What is wrong today

`CatalogueManager` (`src/catalogue/catalogue.cpp`) has two writers of its
cached view:

- the mutation path (`upsert`, `put_artwork`, `reconcile_scanner`, ...)
  commits, then installs the snapshot it just built via
  `cache(committed.generation, ...)`, read from `metadata_.local()`;
- `refresh()` polls: driven by maintenance's `catalogue_dirty`,
  `refresh_needed()` (a `cache_until_` TTL from `metadata_cache` and the
  server's known generation), and reads `metadata_.current()`, which can lag
  this node's own head.

Consequences:

- **The view can step backwards.** A refresh reads its view before a commit
  and caches it after, and the item just written is refused on the next call
  with `catalogue item revision changed` (fi-1, 2 of 30 runs; proven by
  naming both revisions in the message: `expected 6, now 5`). An editor save
  or the scanner can get a 409 straight after a successful write.
- **Repairs are not countable.** Maintenance runs `repair_once()` on every
  pass while `catalogue_dirty` holds, whether or not the root moved, so the
  number of catalogue passes a burst costs depends on how passes interleave
  with commits (3 to 4 observed for the same burst). The test's ceiling,
  a quarter of the node events, is an invented number and fails whenever
  events come in under 16.
- **Every install is a full load.** `load_root` reads and decodes all 64
  shards and merges them, for a one-item change.

## The design

1. **One install point, keyed on the root.** `install(root)`: if `root` is
   the cached root, nothing; otherwise materialise and swap the snapshot
   pointer. No generation bookkeeping, no dirty flag, no TTL. Today's
   generation guard in `cache()` becomes unnecessary because there is only
   one writer and the latest root always wins.

2. **Driven by head changes, not polling.** When this node's head changes
   (the same event FUSE wakes on, `NodeEvent::metadata`, plus every local
   commit), the catalogue records the head's `catalogue_root` as the target
   and wakes one worker. The worker installs whatever the target is when it
   runs: a burst of twenty commits is one install of the final root.
   Readers keep the old pointer until the swap; nobody waits on the worker.
   The mutation path is the degenerate case: it already holds the decoded
   `next` snapshot, so its install is a pointer swap with no load.

3. **Incremental by construction.** The root is a manifest of 64
   content-addressed shards. The view holds the decoded shard per id;
   installing a new root decodes only the shards whose id changed and reuses
   the rest by pointer. A single edit costs one shard. This is BACKLOG
   Catalogue Stage C (shards loaded on demand) done at the same time, since
   it is the same code.

4. **Maintenance keeps only real background work.** Offering the root's
   control objects to peers (`converge_control_replicas`) runs when the root
   or membership changes, not on every pass. The catalogue stage in
   `maintenance.cpp` becomes: wake the worker if the head moved; converge
   if root or membership changed.

5. **Conflict reconciliation stays where it is.** `reconcile_catalogue_conflict`
   is a commit, so it goes through the mutation path and produces a new head
   like any other.

## What gets deleted

- `refresh(bool)`, `refresh_needed()`, `cache_until_`,
  `cached_metadata_generation_`, the "view older than known generation,
  defer" branch, and `catalogue_dirty` / `catalogue_retry_due` in
  maintenance.
- The catalogue's use of `metadata_cache`. `MetadataManager` also uses it
  for its decoded-record cache (`metadata_manager.cpp:301`); whether that
  use survives is a separate question, so the setting stays until it is
  answered.
- `test_metadata_decoded_cache_ttl_recovers_missed_notice`, if it exists
  only to cover a missed notice the catalogue can no longer miss. Check what
  it asserts first; it may be a MetadataManager test.

## Cost

- **Check on every head change:** a 32-byte root compare, after reading
  `catalogue_root` from the head. `MetadataManager::local()` keeps the
  decoded head and re-reads only on a `heads_revision` change, so the
  decode is paid once by whoever needs the head after a commit. Measure that
  decode on a FUSE-heavy head on gbni-1 before claiming it is small; if it
  is not, the root must be readable without a full decode.
- **Install on a root change:** proportional to changed shards, so one
  shard for a typical edit. Today it is all 64 every time.
- **Serial point:** one worker, latest wins. Worst case is a peer's edit
  appearing one install-time later than it could. Nothing blocks on it.

## Tests

- The burst test asserts installs, not repairs: at most one install per
  distinct root adopted, so exactly one for the gated burst. A per-event
  storm fails it; no ratio, no invented ceiling.
- A deterministic unit test on `install`: same root is a no-op; an older
  target queued behind a newer one is never installed; only changed shards
  are decoded (count decodes).
- The existing catalogue suites must pass unchanged in behaviour; the
  revision-conflict case stays as a regression guard with the named
  revisions in its message.

## Order of work

1. Commit the P0 guard in `cache()` as is (done in the tree; small and
   correct on its own).
2. Shard-level view with reuse (Stage C).
3. `install(root)` plus the head-change worker; remove the polling paths.
4. Maintenance stage reduced to wake and converge.
5. Rewrite the burst test; add the install unit test; remove the dead TTL
   test if it is the catalogue's.
6. Measure `local()` decode cost and `load_root` time on gbni-1 before and
   after; record both in COMPLETED.

Estimate: a day, not an hour.

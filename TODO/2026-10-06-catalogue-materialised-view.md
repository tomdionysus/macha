# The catalogue plan: a materialised view of the local head

Status: agreed design, 2026-10-06. Not started. Supersedes
`archive/2026-09-17-catalogue-shard-demand-load-plan.md`, whose stages are
folded in below or dropped. The P0 stopgap (a `cache()` that refuses an
older generation) shipped in 0.90.26; stage 3 replaces it.

## The rule

The catalogue view on a node is a pure function of that node's own metadata
head: `view = f(head.catalogue_root)`. Nothing else produces it. Two nodes
holding the same root hold byte-identical views, and any assertion about the
catalogue is an assertion about a root hash.

This is the law the namespace already obeys. The catalogue is the one
subsystem that keeps its own notion of "current", refreshed by polling and
a TTL and written from two paths. Both failures of
`test_catalogue_uses_final_state_after_coalesced_metadata_burst` are that
exception showing through: the view stepping backwards (fi-1, 2 of 30 runs,
`catalogue item revision changed: ... expected 6, now 5`), and a repair
count that depends on how maintenance passes interleave with commits.

## What the tree-era work already gives the catalogue

Built since the September plan, and reused here rather than rebuilt:

- **Shards arrive with the commit.** A commit ships its changed control
  objects with its claims (0.89.1); convergence is the fallback. A node
  that received the commit holds every shard, so an install fetches only
  when the node missed the commit.
- **Reachability is the horizon builder.** `NodeHorizonBuilder::release`
  asks the catalogue for each root's retained data and control
  (`node_horizon_builder.cpp:50`); `maintenance_objects` declares the
  manifest and shards of the cached and protected roots. Every object the
  catalogue references is declared through these two, or GC reclaims it.
- **`retention_objects` is already a per-shard diff** (`catalogue.cpp:1835`):
  it decodes only shards the two manifests do not share.
- **Commits carry the change set** (0.90.11): only changed shards are
  encoded and claimed. **Lists page** (0.90.18).
- **Local-first commits and `MetadataView::local()`**: this node's head is
  always readable without a peer.

## The design

### 1. The view: resident shards, shared by pointer

`CatalogueView` is immutable and handed out as `shared_ptr<const>`. It holds
the manifest and, per shard slot, a `shared_ptr<const Shard>` of decoded
content. Two views built from roots that share a shard share the `Shard`
object. Every shard stays resident: API reads are memory-only and never
wait on the control store. Derived structures (the artwork-type and
media-binding indexes `indexes()` builds today; the list and search indexes
of section 6) are per shard and merged, so a one-shard change updates one
shard's entries.

Residency is measured first (stage 1) and reported in catalogue status. If
the number says a library cannot keep its shards resident, the answer is a
prefetch-on-install policy on the `RetainedMemoryLedger`, never a fetch on
read. Not planned until the number says so.

### 2. One install point, keyed on the root

`install(root)`: if `root` is the installed root, nothing. Otherwise diff
the manifests, decode the changed shards (reusing the rest by pointer),
build the new view, swap the pointer. The manifest diff and changed-shard
decode are one function, `shard_changes(old_manifest, new_manifest)`, used
by `install`, `retention_objects` and the conflict merge (section 5).

No generation bookkeeping, no dirty flag, no TTL. The 0.90.26 generation
guard is deleted: with one writer and latest-wins there is nothing to
guard.

### 3. Driven by head changes, not polling

Every change of this node's head (the `NodeEvent::metadata` FUSE wakes on,
and every local commit) records the head's `catalogue_root` as the target
and wakes one worker. The worker installs whatever the target is when it
runs: a burst of twenty commits is one install of the final root. Readers
keep the old pointer until the swap; nothing waits on the worker.

An install that cannot read a shard (the node missed the commit and the
holder is away) parks, and the worker wakes again on membership or
storage change, not per pass. Until then the previous view serves and
status reports `catalogue_complete = false`.

### 4. Mutations copy only the shards they touch

A mutation starts from the installed view, copies the shards it changes,
builds the successor view, commits it through `mutate_delta` with
`expected_root` as now, and installs the successor view directly: no load,
no deep copy of the snapshot. If the head's root has moved past the
installed one, the mutation installs first (changed shards only) and
retries once; the second attempt is against the current root by
construction.

This removes every `*current_snapshot()` deep copy (about ten sites) and
`snapshot()` as a by-value copy (`catalogue.cpp:910`).

### 5. The conflict merge compares shards first

`reconcile_catalogue_conflict` loads base, left and right in full and
merges item by item. With per-shard views it compares the three manifests
and descends only into slots that differ; identical slots are taken as is.
It is still a commit through the mutation path, producing a new head like
any other.

### 6. Indexes behind list and search, derived not stored

`list(kind, parent)` and `search` scan every item. The index they want
(`(kind, parent)` to ordered `(sort_title, id)`; a term to ids map) is a
pure function of the view, so it is derived per shard at install and
merged, not stored as objects. That keeps the format unchanged and leaves
the horizon builder nothing new to declare. Paging already exists; a page
then walks one ordered index instead of sorting a copy of every match.
`clear_metadata_with_media`'s fixed-point descendant scan uses the same
parent index.

Only if stage 1's number says the derived index is too large to keep
resident does it become stored objects, declared through
`retention_objects` and `maintenance_objects`.

### 7. One manifest change: separate shard spaces and a growable count

Items, media profiles and media indexes share the 64 shards through one
hash, so a profile publication dirties item shards. The manifest gains a
shard vector per family, each sized from the count on the wire (so the
count can grow; raising it is one re-root, done in the background). Old
manifests decode unchanged. This is the only format change in the plan, so
the two September stages that each wanted one (F and open question 2) are
done together, once.

A node not yet restarted cannot read the new manifest; it serves its last
installed view with `catalogue_complete = false` until it is restarted.
With two nodes and rolling-restart spacing that is minutes, and nothing
wedges: the view stands, writes go to the restarted node. The new manifest
is written only by new code, so the window is bounded by the restart.

### 8. Batched profile publication

`MediaInformationService::loop` drains its pending publications into one
`put_media_profiles` call. One batch, one root, one install. Independent of
everything above; small.

### 9. Maintenance keeps only real background work

The catalogue stage in `maintenance.cpp` becomes: wake the worker if the
head moved; `converge_control_replicas` when the root or membership
changed, not every pass. `maintenance_repair()` and the second repair per
pass go. `catalogue_complete` for the inventory is a fact, installed root
equals head root, not the outcome of a repair.

### 10. Holdings for catalogue DATA

Artwork and media indexes are DATA outside the tree, carried today as the
inventory's flat `outside_namespace` list that repair covers separately and
`known_present` does not trust. Lost artwork is found only by a full walk.
The analogue of tree holdings is holdings per shard: a shard's DATA objects,
held and where, rolled up like a tree node's. Last, because it is new
mechanism rather than consolidation, and it needs stage 1's residency and
section 7's per-family shards.

## What gets deleted

- `refresh(bool)`, `refresh_needed()`, `cache_until_`,
  `cached_metadata_generation_`, the "view older than known generation,
  defer" branch, the 0.90.26 generation guard, and `catalogue_dirty` /
  `catalogue_retry_due` in maintenance.
- `load_root` as a merge of all 64 shards; `snapshot()` by value; every
  `*current_snapshot()` copy.
- `maintenance_repair()` and the inventory-time repair.
- The catalogue's use of `metadata_cache`. `MetadataManager` keeps its own
  use (`metadata_manager.cpp:301`); whether that survives is a separate
  question.
- `test_metadata_decoded_cache_ttl_recovers_missed_notice`, if it covers
  only a missed notice the catalogue can no longer miss. Check what it
  asserts first; it may be a `MetadataManager` test.

## Dropped from the September plan

- Demand-loaded shards with an LRU and fetch on read: a read could wait on
  a WAN fetch, which breaks memory-only reads; shards arrive with the
  commit now, so residency is the natural state.
- Modelling the shard cache on `ReadHandle::extent`: the tree keeps no
  resident nodes (readers construct `ControlNamespaceNodeStore::for_reading`
  per read), and the catalogue's needs are the opposite. The view is its
  own structure.
- Stored index objects as the first step: derived indexes cost no format
  change and no reachability work.
- "Agree the paging contract first": done, 0.90.18.
- Stage B's encoding half: done, 0.90.11.

## Cost

- **Check on every head change:** a 32-byte root compare after reading
  `catalogue_root` from the head. `MetadataManager::local()` keeps the
  decoded head and re-reads only on a `heads_revision` change, so the
  decode is paid once by whoever needs the head after a commit. Measure
  that decode on a FUSE-heavy head on gbni-1 (stage 1); if it is not
  small, the root must be readable without a full decode.
- **Install on a root change:** proportional to changed shards, one for a
  typical edit, against all 64 today.
- **Mutation:** copies one shard, against the whole snapshot today.
- **Serial point:** one worker, latest wins. Worst case is a peer's edit
  appearing one install-time later. Nothing blocks on it.

## Tests

- The burst test asserts installs, not repairs: at most one install per
  distinct root adopted, so exactly one for the gated burst. A per-event
  storm fails it; no ratio, no invented ceiling.
- `install` as a deterministic primitive: same root is a no-op; an older
  target queued behind a newer one is never installed; only changed shards
  are decoded (count decodes); a missing shard parks and the previous view
  serves.
- `shard_changes` exhaustively: identical manifests, one slot, every slot,
  a slot added, a slot removed, each family.
- A mutation against a moved root installs and retries once; a second move
  is a conflict.
- The conflict merge: identical slots are not decoded.
- The revision-conflict case stays as a regression guard with the named
  revisions in its message.

## Order of work

Each stage ships on its own and leaves the suite green; no stage waits on
a later one.

1. **Measure.** `catalogue_resident_bytes` in status, modelled on
   `snapshot_resident_bytes`; the `local()` decode cost on gbni-1; the
   current `load_root` time there. Numbers before shapes.
2. **The view and `shard_changes`.** Resident per-shard `CatalogueView`;
   `retention_objects` rewritten over `shard_changes`; `load_root` becomes
   the first install. Behaviour unchanged; the deep copies go.
3. **`install` and the worker.** One install point driven by head changes;
   the polling paths, the TTL and the 0.90.26 guard deleted; the burst test
   rewritten; the install primitive tests added.
4. **Maintenance reduced** to wake and converge; `maintenance_repair()`
   removed; `catalogue_complete` as a fact.
5. **Mutations over the view**: copy touched shards, install the successor
   directly, retry once on a moved root.
6. **The conflict merge per shard.**
7. **Derived indexes** for list, search and the descendant scan.
8. **The manifest change**: per-family shard vectors with a growable
   count. Announced to Core and every client before it ships: no API
   change, but the rolling-restart window is theirs to know.
9. **Batched profile publication.**
10. **Holdings for catalogue DATA**, with its own design note first.
11. **Measure again** (stage 1's three numbers) and record both in
    COMPLETED.

Stages 2 to 5 are the materialised view proper, about a day. Stages 6 to 9
are each small once the view exists.

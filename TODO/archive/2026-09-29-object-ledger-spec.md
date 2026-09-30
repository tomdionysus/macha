# The object ledger: one record per object, on disk, diffable

**Historical only.** The canonical spec is the most recent version of
[the object ledger and the component model](../2026-09-29-object-ledger-and-components-spec.md).
This document records how the design began and is not read as requirements.

Status: EXPERIMENTAL. A specification and roadmap, not a commitment to a
design. Nothing in the tree implements it. Written 2026-09-29 on the
operator's direction ("write it contract first, decide the functionals and
non-functionals and experiment with implementations"). No code is to be
written against it until the operator has read it and chosen the stages.

## Why

Three structures answer the question "what is object X to this node?", and
none of them scales with the library:

1. **The live set** (`MaintenanceObjects.live`, `src/filesystem/filesystem.cpp`
   `maintenance_objects_cached`). A flat sorted vector of every live object id
   in the namespace and catalogue, 32 bytes each, rebuilt by a full walk of
   the namespace tree whenever the known metadata generation advances, which
   on this cluster is every commit. Every node holds the whole library's ids,
   including an edge node that stores almost nothing. gbni-1's ~570k extents
   is 18 MB and a walk of a few seconds; at 4 MiB extents that is 64 MB at
   an 8 TB library, 1 GB at ~130 TB (~8k BluRay-sized titles) and 12.8 GB at
   100k titles. Repair's push and pull, GC and retention release all consume
   it.
2. **Retention claims** (`RetentionStore`, `src/storage/retention.{hpp,cpp}`).
   Per object per class: an observed-remove set of `adds` (highest
   undominated claim sequence per origin node) and `removed` (highest remove
   context per origin), so a removal can erase only the claims causally
   visible to the metadata mutation that produced it, and a concurrent
   branch's re-affirmation survives. Durable: append journal plus checkpoint
   generations in 256 encrypted shards by the first SHA-256 byte. Cursor-based
   traversal with operation budgets. But the working set is a
   `std::map<ObjectId, ObjectState>` in memory, bounded by what this node
   claims, which at `replicas: 2` on two nodes is the whole library again.
3. **Presence** (`LocalStore`, `src/storage/local_store.{hpp,cpp}`). Which ids
   this node physically holds: the packed-object index plus `present_loose_`
   (~40 B per object, warmed from directory names at start). Bounded by the
   disk, not the library; `has()` is an index lookup (2,710 in 24 ms on fi-1).

Every consumer wants a predicate over the same row: should I own it, do I
hold it, have I promised to keep it, does the namespace still want it. Today
each consumer joins the three structures itself, by scan. Replication
coverage ("how many of the copies the cluster wants exist") is computed by
nobody, and repair finds work by walking, one node at a time, with no way to
ask a peer "what do you hold that I lack" short of shipping every id.

The ledger is one record per object with those columns, kept current by the
writes that change them, stored on disk behind a bounded cache, indexed for
each consumer's predicate, and shaped so two nodes can diff theirs.

## The record

Per object id, per class (`data`, `control`), on each node:

| column | meaning | source of truth | durability |
|---|---|---|---|
| `referenced` | reference count from the namespace and catalogue at the head this ledger is current to | metadata | derived: rebuildable from the namespace tree |
| `claimed` | this node's retention claim state: `adds` and `removed` per origin, exactly `RetentionStore::ObjectState` today | this node | durable, causal: the one column that cannot be rebuilt |
| `held` | this node has a copy on its DATA (or control) store | the store | derived: rebuildable by a store scan |
| `owner` | placement names this node an owner at the current topology | placement | derived: recomputed on topology change |
| `size` | stored size, when known | the store | derived |

The ledger also carries, once per class: the metadata generation and head hash
`referenced` is current to; the topology epoch `owner` is current to; and the
running counts below.

`claimed` keeps the observed-remove semantics unchanged. That column is the
retention store, moved, not redesigned. Any implementation that cannot pass
the retention store's existing tests against it is wrong.

## Queries (the contract)

Every query is bounded: it takes a cursor and an operation budget and returns
where it stopped. None is O(ledger). Each is a predicate over the record and
is what a secondary index exists to serve.

| query | predicate | consumer |
|---|---|---|
| `to_pull` | `owner && !held && referenced > 0` | repair pull |
| `to_push_from` | `held && referenced > 0`, joined against a peer's `held` (see diff) | repair push |
| `surplus` | `held && !owner && !claimed && referenced > 0` | rebalance (removal after the owners hold it) |
| `releasable` | `claimed && referenced == 0`, removal causally after every observed add | retention release |
| `garbage` | `held && referenced == 0 && !claimed`, with the causal rule above and `garbage_grace` | GC |
| `missing_here` | count of `owner && !held` | cohesion |
| `held_owned` | count of `owner && held` | cohesion |
| `claims` | the causal state of one object | diagnostics, as today |
| `next_claimed` | cursor over `claimed` | the claim walk in repair |

Point lookups (`has`, `retained`, `should_own`) remain and answer from the
record.

## Writes

| event | columns | who |
|---|---|---|
| metadata commit accepted (own or replicated) | `referenced` for the extents added and removed by that commit's delta; head stamp | metadata replica, on the commit path, via the existing namespace delta applier |
| head switch (reconciliation moves to a sibling branch) | `referenced` replayed from the common ancestor, or rebuilt | metadata manager |
| object put / remove / discard on the store | `held`, `size` | LocalStore |
| retain / release | `claimed` | as today |
| membership or capacity change | `owner` for every record: a sequential walk of the ledger, bounded per pass and resumable | maintenance |
| catalogue root change | `referenced` for artwork, shards, media indexes | catalogue |

The counts (`missing_here`, `held_owned`, and per-column totals) are updated
by the same writes, so Status reads them in O(1).

## Invariants

1. **The OR-set rule.** A release removes only claims whose add sequence is
   causally visible to the releasing mutation's clock. This is what makes a
   concurrent branch's re-affirmation survive, and it is today's behaviour.
2. **The GC fence.** An object may be deleted only when `referenced == 0`,
   `!claimed` under rule 1, the grace has elapsed, and the ledger's
   `referenced` is current to an accepted head that every durably known node
   has been reached about (today's `cluster_gc_stable`). A ledger behind the
   head must refuse GC, never guess.
3. **`referenced` is current to one head.** The ledger records which. A read
   that finds the head moved uses the previous answer for repair (safe: extra
   work, never lost data) and refuses for GC.
4. **`claimed` is durable before metadata may reference the object**
   (`min_write_replicas` nodes must hold a claim), exactly as now.
5. **Derived columns are rebuildable and say so.** A mismatch between the
   ledger's stamp and the store or the head is repaired by rebuild, in the
   background, with the ledger serving conservative answers meanwhile:
   "held" is trusted only from the store's own index; "referenced" unknown
   means "assume referenced".
6. **No operation is O(ledger)** except the explicit rebuilds, which are
   sequential on disk, bounded per pass and resumable.

## Non-functionals

- **Memory is bounded by configuration, not by the library.** A page cache
  (`storage.ledger_cache`, default 64 MiB) is the ledger's whole resident
  footprint beyond the counts. The top levels of a 400M-record index fit in
  it; leaves stream from the state device.
- **Storage.** Lives under `state_path`, which must be on the SSD, not the
  DATA spindle; the config check refuses a ledger path on a DATA backend's
  device. Size: roughly 64 bytes a record plus index overhead; 400M records
  is in the tens of GB, and a node stores a record for every referenced
  object, not only its own. (Open question 2 asks whether that is right.)
- **Crash safety.** Journal plus checkpoint, as the retention store does
  today, for `claimed`; the derived columns may lag and be rebuilt.
  `durable_replace_file` semantics for checkpoints.
- **Canonical shape.** The index over ids must not depend on insertion order,
  so two nodes' `held` and `claimed` indexes over the same ids hash the same
  and can be diffed. This rules out a plain B+ tree as the diffable layer.
- **Bounded work under the laws.** Ledger writes on the commit path are
  control class and must cost O(delta) with no disk read of the ledger's
  leaves that a viewer could wait behind; rebuilds and `owner` recomputes are
  speculative class and paced by the maintenance share. Repair stays paced,
  never stopped, whatever the ledger says.
- **Observability.** Counts in Status per node (`ledger.referenced`,
  `held`, `owned`, `missing`, `claimed`, `head_generation`,
  `rebuilding`) and in telemetry so every node can state cluster cohesion.
- **Migration.** First start reads today's retention checkpoints and journal
  into `claimed`, scans the store into `held`, walks the namespace once into
  `referenced`, and keeps the old files until the ledger has checkpointed.
  A downgrade must find the old files intact.
- **No new wire format for existing messages.** The diff is one new RPC pair.

## The diff

With a canonical index, "what do you hold that I should own and lack" is a
descent: compare the two nodes' subtree hashes for a prefix, descend where
they differ, list the leaves. Over a radix-256 trie on id bytes (the layout
the loose store and the retention checkpoints already use), 400M uniformly
random ids sit about four levels deep, and a divergent region costs a few
round trips rather than 32 bytes per id.

Repair's pull becomes: diff my `owner && !held` prefix set against each
holder's `held`; fetch what the diff lists. Push becomes the mirror. The
walks and the `have_valid_objects` probe rounds remain as the fallback for a
peer without a ledger, and validation of a copy (read, decrypt, hash) stays
where it is: the diff says a peer's index has it, not that the bytes are
good. Scrub owns corruption.

`lost` (referenced, held by no node) falls out of the same diff: the owners'
union of `held` against `referenced`.

## Laws and disciplines, stated

- **Law 1 (control):** commit-path ledger writes are O(delta) and never read
  leaves from disk synchronously; if the cache misses, the write is journaled
  and applied by the ledger's own thread. HTTP reads of counts are O(1).
- **Law 2 (viewer):** the ledger does no I/O on the DATA device; it lives on
  the state SSD. Rebuilds and recomputes are speculative and paced.
- **Law 3 (loader):** publication's retention claims are the same operation
  as today; the ledger must not make a claim batch slower than the retention
  store's journal append.
- **Law 4 (recovers alone):** every derived column rebuilds without a peer;
  a corrupt ledger file is discarded and rebuilt, except `claimed`, whose
  journal-plus-checkpoint discipline is unchanged.
- **Re-derive rather than assert:** the stamps (head, topology epoch, store
  generation) are checked on every read that matters and trigger a rebuild.
- **Backoff and parked state:** rebuilds are resumable with a cursor.
- **Resolve rather than refuse:** a stale `referenced` degrades repair to
  "do more", never GC to "delete more".
- **Snapshot size is a function of the live namespace:** the ledger's size is
  a function of the referenced set, which is the same thing; the point of the
  work is that the resident part is not.
- **No bound smaller than one unit of work:** the cache must hold at least
  the index path for one record plus one leaf page.

## Implementation candidates

Behind one interface (`ObjectLedger`), with a conformance suite built from
the existing retention, GC, repair and rebalance tests plus the queries
above:

0. **Facade over today's structures.** `RetentionStore` + `LocalStore` index +
   the live vector, wrapped. No behaviour change. Proves the interface and the
   suite. This is stage 0 and is worth doing even if nothing else is.
1. **Radix-256 hash trie on disk.** Pages by id prefix, per-page child hashes,
   a page cache, journal for `claimed`. Canonical by construction; the diff
   is native. All in-tree code, no new dependency.
2. **Embedded B+ store (LMDB) for the records, with a Merkle layer over id
   ranges for the diff.** Mature ACID storage and mmap for free; adds a
   dependency (the project rule requires a concrete reason), and the diff
   layer is extra work because a B+ tree is not canonical.

Experiments to run before choosing, on fi-1 with a synthetic 400M-record
ledger on its NVMe: cold and warm point lookup latency; commit-path write
cost for a 3,000-extent file; full `owner` recompute time; diff cost against
a peer whose `held` differs in 1% of records; resident memory at a 64 MiB
cache; rebuild time from a store scan.

## Roadmap

Each stage ships on its own, is deployable, and has an exit criterion.

- **Stage 0: contract and facade.** The interface, the conformance suite,
  the facade over today's structures, every consumer (repair push and pull,
  GC, retention release, rebalance, the claim walk) moved onto the interface.
  Exit: the full suite green with the facade; no behaviour change measured
  on the cluster.
- **Stage 1: counts.** `missing_here`, `held_owned` and the column totals
  maintained by the facade (a one-off walk to seed, then incremental), in
  Status and telemetry; cluster cohesion computed from every node's figures.
  Exit: cohesion on the Status page; announced to Core and every client.
- **Stage 2: on-disk ledger.** Candidate 1 built and measured against the
  experiments; migration from the retention checkpoints; the flat live
  vector and the in-memory claim map gone; the namespace walk replaced by
  delta application on the commit path with rebuild as the fallback.
  Exit: resident memory independent of library size (measured with the
  synthetic ledger); every consumer unchanged in behaviour.
- **Stage 3: diff-driven repair.** The diff RPC; repair pull and push driven
  by it with the walks as fallback; `lost` reported.
  Exit: repair on a two-node cluster finds all missing copies without a
  live-set walk; measured cost per divergent region.
- **Stage 4: retire the fallbacks** once every node runs stage 3.

Stages 0 and 1 are independent of the storage choice and give the
operator the cohesion figure early. Stages 2 and 3 are weeks each.

## Open questions for the operator

1. Should the ledger be per class (data, control) as the retention store is,
   or one record with a class column? (Per class keeps the checkpoint layout;
   one record simplifies the counts.)
2. Must every node carry `referenced` for the whole library, or only for
   objects it owns or holds? The former is what GC and "lost" need; the
   latter halves the footprint on an edge node. Suggested: whole library,
   because GC correctness must not depend on placement.
3. Cache size: fixed default, or a fraction of RAM with a floor?
4. Is LMDB acceptable as a dependency if the experiments favour it?
5. Should the `owner` recompute on topology change be immediate (a walk) or
   lazy (recomputed on read, with the count corrected as reads happen)?

## Out of scope

Metadata replication, the namespace tree's own format, placement rules, the
retention claim semantics, and the transport. The ledger consumes all of
them and changes none.

# Object ledger stage 0: one interface over today's structures, no behaviour change

**Historical only.** The canonical spec is the most recent version of
[the object ledger and the component model](../2026-09-29-object-ledger-and-components-spec.md).
This document records how the design evolved and is not read as requirements.

Status: EXPERIMENTAL, on `experiment/object-ledger`. The stage 0 part of
[the object ledger spec](2026-09-29-object-ledger-spec.md), written
2026-09-29 after checking that spec against the code. It supersedes that
spec's stage 0 bullet and corrects its account of the live set (below). No
code until the operator has read it.

## The one requirement

Stage 0 puts every question maintenance asks about an object ("is it
referenced, do I hold it, have I claimed it, may I delete it") behind one
interface, `ObjectLedger`, implemented by a facade over the structures that
answer those questions now. **If it is implemented, nothing the node does
changes**: the same objects are examined in the same order, the same
decisions are taken at the same points in the maintenance pass, the same log
lines are written, Status and the API are byte-for-byte the same. Its value
is the interface and the conformance suite that the later stages are built
against, and one owner for state that is today spread over `Service`,
`FileSystem`, `RetentionStore` and `LocalStore`.

## What the code does today (corrections to the parent spec)

The parent spec describes "the live set" as one flat vector current to one
head. The code has two families of reachability sets, built at different
heads by different triggers:

1. **The maintenance inventory**, at the node's known metadata generation.
   `FileSystem::maintenance_objects_cached` (`src/filesystem/filesystem.cpp`)
   walks the namespace and the conflict roots; `Service` then adds the
   catalogue's `maintenance_objects()` and keeps
   (`src/service/service.hpp:103-114`):
   - `maintenance_live_`: DATA ids referenced by namespace, conflicts and
     catalogue;
   - `maintenance_universal_`: ids repair places on every hosting node
     (always empty: `CatalogueMaintenance::universal` is
     "intentionally empty in the 0.18 storage model");
   - `maintenance_control_live_`: catalogue control objects;
   - `maintenance_garbage_` and `maintenance_stale_garbage_`: tombstones
     split by whether the id is live again;
   - `maintenance_inventory_generation_` and
     `maintenance_catalogue_complete_`.

   Consumed by repair (`repair_step`), tombstone collection
   (`collect_garbage`), control GC (`control_gc_step`) and the DATA sweep
   (`gc_step`).
2. **The retention release horizon**, at the sole accepted head
   (`metadata_->retention_release_view()`), rebuilt when that head's hash
   changes (`src/service/service.cpp:1655-1720`). A second namespace walk
   plus conflict roots, the catalogue's `retention_objects` for every
   catalogue root, and the namespace tree's own nodes:
   - `retention_release_data_live_`, `retention_release_control_live_`;
   - `retention_release_clock_`: that head's mutation sequences;
   - `retention_release_complete_`. A horizon with an unreadable catalogue
     root or namespace node is discarded and the previous one kept.

   Consumed only by `RetentionStore::release_unreferenced`.

There are three destructive gates, not one fence, and they are not the same
condition. Each is preserved exactly:

| gate | consumer | condition (`src/service/service.cpp`) |
|---|---|---|
| tombstone collection | `collect_garbage`, `maintain_garbage_metadata` | `garbage_due && !rebuilt_inventory && cluster_gc_stable && maintenance_catalogue_complete_` (1623) |
| control | control retention release, `control_gc_step`, control `prune_unclaimed` | `gc_due && destructive_gc_enabled && maintenance_catalogue_complete_ && maintenance_control_live_ && inventory generation >= known` (1722); release additionally needs a complete horizon |
| DATA | DATA retention release, `gc_step`, DATA `prune_unclaimed` | `gc_due && !rebuilt_inventory && destructive_gc_enabled && maintenance_catalogue_complete_ && maintenance_live_ && inventory generation >= known` (1783); release additionally needs a complete horizon |

where `cluster_gc_stable` is every known node reachable and metadata stable,
and `destructive_gc_enabled` is `cluster_gc_stable` plus a release view whose
`retention_baseline_complete` is set. The skip reason logged at debug
(1751-1781) describes the DATA gate only.

The control gate does not test `!rebuilt_inventory`, although the comment at
the rebuild (1477) says a new inventory is never used by a destructive sweep
in the pass that built it. Stage 0 preserves the code, not the comment; see
open question 1.

## The interface

One object, owned by the node, replacing the service members above. Names
are provisional; the shape is the contract.

### Horizons

A horizon is a set of referenced ids stamped with the head it is current to.
There are exactly two, and every `referenced` question names one:

- `Horizon::inventory`: stamp = metadata generation; carries
  `catalogue_complete`, the tombstone lists, and the ids with `everywhere`
  set.
- `Horizon::release`: stamp = accepted head hash plus its mutation clock;
  carries `complete`.

Per horizon and class (`data`, `control`):

- `referenced(horizon, class, id) -> bool`
- `referenced_ids(horizon, class) -> const std::vector<ObjectId>&` (sorted,
  unique; the same vector the consumers binary-search today, handed out by
  reference so repair's inner loop costs what it does now)
- `stamp(horizon)`, `complete(horizon)`, `size(horizon, class)`

`everywhere(id)` is a column of the inventory horizon, false for every id
today, passed to repair as the `universal` vector is now.

### Refresh is the caller's decision

The facade never refreshes itself on a read. Rebuild timing is behaviour:

- `refresh_inventory(...)` is called where `Service` builds the inventory
  now, under the same conditions (`network_due || garbage_due || gc_due`;
  when only repair is due and an inventory exists, only once
  `metadata_ready_for_dependants`), and returns whether it rebuilt. It calls
  `catalogue_->maintenance_objects()`, which runs the catalogue's repair as a
  side effect, exactly as now.
- `refresh_release(view)` is called where the release horizon is built now,
  replaces the horizon only when the new one is complete, and keeps the
  previous one otherwise.
- The caller keeps `rebuilt_inventory` for the pass and the
  `gc_quiescent_until` follow-up it schedules.

The parent spec's invariant 5 (derived columns rebuild themselves in the
background on a stamp mismatch) begins at stage 2, when there is something
on disk to heal.

### Gates

Three predicates, one per row of the table above, each taking the pass's
`due` flags and `rebuilt_inventory` and returning `permitted` or the reason
it is not. The DATA gate's reason is the string logged today, checked in the
same order, so the debug line is unchanged. The gates read the ledger's own
stamps and the facts the service passes in (`cluster_gc_stable`, the release
view); they do not fetch cluster state themselves.

### Held

- `held(class, id)`: `LocalStore::has` for data, the control store's `has`
  for control.
- `next_held(class, cursor, budget)`: the store's `next_object` cursor, for
  the GC sweep.
- `put`, `remove` and `discard` stay on the stores. Nothing is mirrored at
  stage 0.

### Claimed

The `RetentionStore` surface, unchanged in meaning, forwarded:
`retain`, `retain_batch`, `retained`, `claims`, `next_retained`,
`release_unreferenced`, `prune_unclaimed`, `compact_if_needed`. Journal,
checkpoints and on-disk layout are untouched.

### Queries from the parent spec

`to_pull`, `to_push_from`, `surplus`, `releasable`, `garbage`,
`missing_here`, `held_owned` are implemented by the facade and covered by
the conformance suite, but **no consumer calls them at stage 0**. Routing
repair through `to_pull` would change which objects it visits and in what
order (the saved push position, `ranked()` peers, probe batching), which is a
behaviour change. They get their consumers at stages 2 and 3.

## Consumers moved onto the interface

| consumer | today | at stage 0 |
|---|---|---|
| inventory build | `Service` 1425-1483 | `refresh_inventory` |
| release horizon build | `Service` 1655-1720 | `refresh_release` |
| repair | `repair_step(..., maintenance_live_, maintenance_universal_, ..., maintenance_inventory_generation_)` | the same call with `referenced_ids(inventory, data)`, the `everywhere` ids and `stamp(inventory)` |
| claim walk | `retention_store().next_retained` + store `has` (1516-1547) | `next_retained` + `held` |
| tombstone collection | 1623-1637 | tombstone gate; tombstone lists from the inventory horizon |
| control release and GC | 1722-1741 | control gate; `referenced_ids(release, control)`, `referenced_ids(inventory, control)` |
| DATA release and sweep | 1783-1851 | DATA gate; `referenced_ids(release, data)`, `referenced_ids(inventory, data)`, `retained` as the sweep's `is_retained` |
| rebalance keep check | `distributed_store.cpp` 2300, 2764: `retained(data, id)` | `retained` |
| publication claims | `distributed_store.cpp` 847, `cluster.cpp` 1528: `retain_batch` | `retain_batch` |
| peer remove refusal | `cluster.cpp` 1535: `retained(data, id)` | `retained` |
| catalogue staging GC | `catalogue.cpp` 1872: `retained(control, id)` | `retained` |
| retention compaction | `Service` 1907 | `compact_if_needed` |

`RetentionStore`, `LocalStore` and `FileSystem::maintenance_objects_cached`
keep their own tests and stay public to the ledger only; after stage 0 no
maintenance code reaches them directly.

## Invariants (stage 0 wording)

1. **The OR-set rule**: unchanged; `claimed` is `RetentionStore`.
2. **The gates**: exactly the three conditions in the table, each tested
   against the ledger's stamps. Folding them into one is not stage 0 work.
3. **Each horizon is current to one head and says which.** The inventory to
   a metadata generation, the release horizon to an accepted head hash and
   its clock. Whether the two can ever be one stays open (question 2).
4. **Claims are durable before metadata references the object**: unchanged.
5. **Refresh happens where it happens today**, nowhere else.
6. **No new O(library) work.** The facade holds the same vectors; it builds
   no index, copies no vector, and adds no walk.

## Laws

- **Law 1 (control):** no new work on the commit path or in any HTTP
  handler; Status is not touched.
- **Law 2 (viewer):** no new I/O on any device.
- **Law 3 (loader):** `retain_batch` costs one forwarding call more.
- **Law 4 (recovers alone):** no new state on disk, so nothing new to
  recover. Repair stays paced by `repair_share`, never gated.

## Conformance suite

Built from what exists, run against the facade, and the gate every later
implementation must pass:

- the existing `RetentionStore` tests, re-pointed at the interface;
- the existing GC, repair, rebalance and maintenance-pass tests, unchanged
  and green;
- new: for each of the three gates, a table test over every condition in
  its row (each false in turn denies, all true permits), mutation-proven;
- new: refresh preserves the previous release horizon when the new one is
  incomplete, and the inventory is not rebuilt when only repair is due
  before metadata is ready;
- new: each parent-spec query against a small hand-built ledger (no
  consumer yet, so this is the only thing that tests them).

## Exit

- Full suites green on the laptop and on fi-1 (`macha-tests`,
  `macha-tests-runtime`, `macha-tests-torrent`).
- `grep` finds no `maintenance_live_`, `retention_release_*_live_` or
  direct `retention_store()` call outside the ledger.
- On the cluster, one node at a time and spaced for viewers: the same
  maintenance log lines at the same stages over a day as before the
  deploy, repair's bytes and examined counts in the same range for the same
  load, and no change in resident memory beyond noise. Nothing to announce
  to Core or the clients: nothing the API sends changes.

## Open questions for the operator

1. **The control gate and `rebuilt_inventory`.** Control GC can run against
   an inventory built in the same pass, which the comment at
   `service.cpp:1477` says never happens. Stage 0 preserves it. Is that
   intended (control objects are cheap to rebuild), or a defect to fix on
   `develop` separately, with its own test?
2. **One horizon or two, eventually.** Stage 2's on-disk `referenced` would
   need two stamps, or an argument that the release horizon can be derived
   from the inventory's deltas. Decide before stage 2, not now.
3. **`universal`.** Keep the `everywhere` column for a future placement
   rule, or remove the plumbing from repair on `develop` since nothing sets
   it?

# Plan: the catalogue that loads the shards it needs

Date: 2026-09-17

Status: not started. No code written. Verified against current `develop`
(0.43.0).

Companion to [namespace Merkle root](2026-09-17-namespace-merkle-root-plan.md).
Same disease, smaller organ, and — the important difference — **no format
change required for the core of it.**

The catalogue already has the structure the namespace is being given: a root
pointer in metadata (`std::optional<ObjectId> catalogue_root`,
`src/metadata.hpp:109`), a manifest of content-addressed shards
(`CatalogueManifest`, `src/catalogue.cpp:127-129`), a hash function selecting a
shard per id (`catalogue_shard`, `:131-136`), and a commit that replicates only
shards whose content id changed (`:1070-1071`, `:1084-1089`). That is a working
one-level Merkle structure with dirty-node detection, shipped.

It then throws the benefit away twice: `load_root` merges all 64 shards back
into one map, and `commit` re-shards and re-encodes the entire catalogue in
order to discover which single shard changed. Every read materialises
everything; every write traverses everything.

## Why this is separable from the namespace work

The namespace needs a flag day because its record payload *is* the serialised
namespace and its identity is a hash over those bytes, so changing the structure
changes every record hash and invalidates every acceptance certificate.

The catalogue has none of that. Its identity is already an `ObjectId` pointing at
a manifest. Shards are already independently addressable content objects fetched
through `ensure_control_local` and `control_store().get()` (`load_root`,
`:543-571`). Changing *which* shards a mutation touches, or *when* a shard is
decoded, changes no published bytes at all — a catalogue written by the new code
is byte-identical to one written by the old, given the same content.

Stages A through C below are therefore pure implementation, deployable by
ordinary rolling restart, with no migration and no coordination.

## What the code actually does (verified 2026-09-17)

### Reads materialise the entire catalogue

`load_root` (`:543-571`) decodes the manifest, then loops every shard, decodes
each, and emplaces every item and every media profile into one
`CatalogueSnapshot`. `cache` (`:647-665`) holds it as `cached_`.
`current_snapshot` (`:772-791`) hands out a `shared_ptr<const CatalogueSnapshot>`
to all of it.

From there:

- `get(id)` (`:837-841`) — a point lookup. `catalogue_shard(id)` already knows
  which of 64 shards holds it. It reads the fully merged map instead.
- `media_profile(media_id)` (`:843-849`) — same.
- `snapshot()` (`:829-831`) — `return *current_snapshot();`, a **full deep copy
  by value**.
- `list(kind, parent)` (`:982-999`) — iterates every item, copies each match into
  a vector, sorts by sort_title. No offset, no limit: it returns the entire
  filtered set. This is a browse request.
- `search(query, limit)` (`:1001-1020`) — scores every item on every query, with
  no index.

### Writes traverse the entire catalogue

Eight mutation sites open with `auto current = *current_snapshot();` — a full
deep copy of every item and every profile:

| Site | Line |
|---|---|
| `put_media_profile` | `:918` |
| `put_media_profiles` | `:937` |
| `prune_media_profiles` | `:960` |
| `upsert` | `:1146` |
| `upsert_many` | `:1175` |
| `erase` | `:1198` |
| `clear_metadata_with_media` | `:1251` |
| `reconcile_scanner` | `:1337` |

Each then calls `commit` (`:1022-1140`), which:

1. computes `artwork_ids(next)` (`:1040`) and `artwork_ids(current)` at the call
   site — two full walks of every item's artwork
2. `shard_catalogue(next)` (`:1062`) — copies **every** item and profile again
   into 64 shard snapshots (`:165-173`, `.emplace(id, item)` by value)
3. loops all 64 shards, `encode_catalogue(shards[i])` and `object_id(encoded)`
   (`:1065-1072`) — **serialises and SHA-256s the entire catalogue** to discover
   which one shard differs from `old_manifest`
4. replicates only the changed shards (`:1084-1089`) — correct, and the only step
   that is already O(changed)

So the network cost is already right and the CPU cost is O(catalogue) per
17-byte change. The dirty-shard detection exists but is computed the expensive
way: by redoing all the work and comparing results.

`clear_metadata_with_media` additionally carries a `while (grew)` loop over every
item to collect descendants (`:1269-1280`), which is O(n²) in tree depth.

### Publication is one mutation at a time

`MediaInformationService::publish_one` (`src/media_information.cpp:337-350`)
calls `catalogue_.put_media_profile` — the singular form — and the service loop
pulls exactly one pending publication per iteration (`:380-386`). The batch form
`put_media_profiles` (`src/catalogue.cpp:932-954`) exists and takes a whole map
for one commit; `reconcile_scanner` uses the batched path
(`:1334`, `:1346-1353`). The media-information path does not.

That matters most for any operation that touches every profile at once. Adding a
field to `MediaProfile` and backfilling 20,000 titles currently means 20,000
commits, each a full catalogue traversal and each advancing the metadata
generation and invalidating every node's cached catalogue.

### Nothing measures what a catalogue costs

The namespace has `snapshot_resident_bytes` (`src/metadata.cpp:55-91`), which
accounts map node overhead and string capacity carefully. The catalogue has no
equivalent. `profile_weight` (`src/media_information.cpp:40-46`) measures only
the pending publication queue. So catalogue residency is currently unmeasurable
from a running node, which is why the numbers below are derived rather than
observed.

### The shard count is already on the wire and merely refused

`encode_catalogue_manifest` writes it (`:142`) and `decode_catalogue_manifest`
reads it (`:155`) — and then throws unless it equals the compile-time constant:

```cpp
if (reader.u32() != catalogue_shard_count)
    throw DecodeError("unsupported catalogue shard count");
```

The format supports a variable shard count today. Only `CatalogueManifest`'s
fixed `std::array<std::optional<ObjectId>, catalogue_shard_count>` and that one
equality check prevent it. Old manifests stay readable because the count is in
their bytes.

## The arithmetic

A `CatalogueItem` (`src/catalogue.hpp:37-56`) carries id, title, sort_title,
synopsis, optional parent id, five optional int32, an aliases vector, an
external_ids map, a media_ids vector and an artwork vector — call it 800 bytes to
2 KB resident each once `std::string` and `std::map` overhead is counted. A
`MediaProfile` with 3-5 streams adds 400-800 bytes.

| Titles | Resident catalogue, every node | Deep copy per mutation | Shard size at 64 |
|---|---|---|---|
| 10,000 | ~15-25 MB | same, per mutation | ~110 KB |
| 100,000 | ~150-250 MB | ~200 MB | ~1.1 MB |
| 1,000,000 | ~1.5-2.5 GB | fatal | ~11 MB |

`decode_catalogue` already permits 1,000,000 items and 1,000,000 profiles
(`:336`, `:384`), so the format claims a scale the runtime cannot hold. At
100,000 titles a single media-profile publication deep-copies a couple of hundred
megabytes, which stops being a latency question and becomes an allocation one —
against a live P0 on ingest memory safety.

These are estimates from struct shapes, not measurements, because nothing
measures it. Stage A fixes that first.

## Stages

**Stage A — measure it.** Add a `catalogue_resident_bytes(const
CatalogueSnapshot&)` modelled directly on `snapshot_resident_bytes`
(`src/metadata.cpp:55-91`), reusing its `account_map_nodes` / `account_string`
discipline, and surface it in catalogue status (`CatalogueStatus`,
`src/catalogue.hpp:82-93`, already carries `items` and `artwork_objects`). Get
the real figure off es-1 before sizing anything else. Cheap, and it is the only
way this stops being argued from struct declarations.

**Stage B — commits carry the change set.** `commit` gains the set of changed
item ids and media ids. It computes `catalogue_shard(id)` for those, decodes and
re-encodes only those shards, and takes every other manifest entry from
`old_manifest`, which it already loads (`:1051-1059`). O(dirty shards) instead of
O(catalogue).

This is the same correction the namespace plan makes, and the same one
`repair_step` already made for the object store — its comment
(`src/distributed_store.cpp:2132-2136`) describes rebuilding a complete vector
per slice to examine a handful of objects, and the header states the resulting
contract: "they never rebuild complete object vectors"
(`src/distributed_store.hpp:238-241`). `WriteHandle` makes the same move at file
scope with `ChangedRange` / `note_changed_range` / `range_changed`
(`src/filesystem.hpp:189-193`, `src/filesystem.cpp:615-636`), which exist
precisely "so unchanged extents are never copied merely to discover they are
unchanged". Three subsystems have learned this; the catalogue has not.

`artwork_ids` needs the same treatment — a changed-artwork set rather than two
full walks.

**Stage C — demand-loaded shards.** `load_root` stops merging. `get(id)` and
`media_profile(id)` resolve `catalogue_shard(id)` and load one shard, cached with
an LRU. Model the cache on `ReadHandle::extent` (`src/filesystem.cpp:173-208`),
including the ordering discipline in its comment at `:184-186` — release the
previous buffer before reserving the replacement, so a handoff cannot hold two at
once — and register it with the `RetainedMemoryLedger`
(`src/retained_memory.hpp:20-52`) rather than inventing a private budget, so
catalogue memory participates in the same pressure accounting and shedding as
everything else.

Mutations stop deep-copying the snapshot: they read the shards they touch, mutate
those, and hand the change set to Stage B's `commit`.

**Stage D — batch publication.** `MediaInformationService::loop` drains
`pending_publications_` into a batch and calls `put_media_profiles`, not
`put_media_profile` per iteration. Small change, already-written target, and it
is what makes any future field addition to `MediaProfile` survivable — a 20,000
title backfill becomes ~200 commits instead of 20,000.

**Stage E — indexes for `list` and `search`.** The one part that adds to the
format, additively: new manifest entries alongside the shards, holding ids rather
than item bodies.

- `list(kind, parent)` wants an index keyed by `(kind, parent)` giving
  `(sort_title, id)` in order. Paging then loads only the shards a page touches.
- `search` wants at minimum a term→ids map. Bigger, less hot, and can lag.

`list` currently returns the entire filtered set with no offset or limit
(`:982-999`), so paging is an API shape change. `CatalogueApi`
(`src/catalogue_api.hpp:13-43`) is the surface, and ACTIVE.md's "What the client
sessions now depend on" section exists precisely because these contracts were
settled with client sessions that cannot be fixed from this repository. **Agree
the paging contract with the client sessions before implementing it**, and keep
the unpaged form working until they have moved.

**Stage F — a shard count that can grow.** Change `CatalogueManifest::shards` to
a vector, size it from the count already on the wire, and replace the equality
check at `:155-156` with a range check. Old manifests keep decoding unchanged.

Raising the count rehashes every id into a different shard, so it rewrites the
whole catalogue once — but that is a single new root, not a flag day, and it can
run as a background re-root during a quiet period. Pick the count so shards stay
in the low hundreds of KB at the target scale: 64 is right at 10,000 titles and
wrong at 1,000,000.

## What this does not fix

- **`clear_metadata_with_media`'s descendant walk** (`:1269-1280`) is O(n²) in
  tree depth and independent of sharding. It wants a parent→children index, which
  is the same index Stage E builds for `list`.
- **`CatalogueHintQueue` is a linear `find_if` over the whole map at 8 call
  sites** with no id→path index — already recorded under P1 scaling cliffs, and
  untouched by any of this.
- **`MediaInformationService::source_for()` linear-scans the entire filesystem
  snapshot** computing a media id per entry, per lookup — also already recorded,
  and it is really a namespace-side problem that Stage D of the namespace plan
  (persisting `file_media_id`) addresses.

## Open questions

1. **Does the catalogue want to become a real tree, or stay one level?** One
   level with a growable shard count is much less work and is probably sufficient
   — a catalogue is bounded by title count, which grows far slower than library
   bytes, and unlike the namespace it has no per-item component that scales with
   content size. Prefer flat-with-growth unless Stage A's numbers say otherwise.
2. **Should media profiles be their own shard space?** They are keyed by
   `media_id` and items by item id; they currently share a shard array via the
   same hash. Splitting them would let a profile backfill dirty only profile
   shards and leave item shards untouched — which is exactly the operation that
   started this whole line of enquiry.
3. **`merge_catalogue_snapshots` (`:397-454`) operates on fully materialised
   snapshots.** With demand-loaded shards, three-way merge should compare shard
   ids first and descend only where they differ. Cheaper, but it needs rewriting
   rather than adapting.
4. **Interaction with `resolve_media_profile`'s single-flight** (`:851-910`).
   Its in-memory `resolved_media_profiles_` cache is a second residency term
   nobody currently measures; Stage A should count it too.

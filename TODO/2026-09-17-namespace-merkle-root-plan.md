# Plan: the namespace that is not the whole library in memory

Date: 2026-09-17

Status: **Stage A complete (2026-09-17), measured on live nodes. Stage B
started the same day -- the tree substrate is written and tested, and is not
yet SM14 or reachable from a record; see "Stage B progress" below.** Stages
C-F not started. The finding below is verified against current `develop` (0.43.0);
the design is proposed and the migration is not yet designed in enough detail to
execute. The estimates in "The arithmetic" have been replaced by measurements --
see "Stage A results" below, which supersedes them. Every line reference here was
re-verified against the working tree on 2026-09-17: the Stage A edits to
`src/metadata.cpp` and `src/metadata.hpp` shifted their own citations, so
re-check them after any further change to those two files.

Macha does not do what it was designed to do. The stated invariant is tens of
thousands of files and 100 TB+ of media in a cluster. At that size the namespace
metadata is gigabytes per node, resident, and a single file write costs several
full traversals of it. Neither is a tuning problem. The record *is* the
serialised namespace and its identity *is* a hash over those bytes, so every
commit re-serialises and re-hashes the library, and nothing can be demand-loaded
because the whole structure must be materialised to produce the hash.

This is the first thing, ahead of the existing P0 list, because it is not a
defect in a feature — it is the storage model failing its own scale target.

## How this was found

It was not looked for. The session began with "do the AV libraries Macha uses
offer a cropdetect function?" — a small feature question about auto-detecting
letterbox bars. That went: cropdetect lives in libavfilter, which Macha does not
link; the cost is decode, not the filter; keyframe-only sampling makes the cost
trivial; the result belongs in the catalogue media profile; storing it means
touching every existing profile; a backfill of every profile exposed the
catalogue's per-mutation whole-snapshot copy; and asking whether that was a
pre-existing scaling problem led to the namespace, which has the same disease an
order of magnitude worse.

The crop work is unaffected by any of this and is written up separately. It is
recorded here only because it is how the P-1 surfaced, and because the path from
a five-minute feature question to the storage model is itself evidence that
these properties are not visible from where features get designed.

Two intermediate analyses in that session were wrong and are recorded because
the corrections matter:

- The catalogue was described as "one blob" whose backfill would write ~4 GB. It
  is sharded 64 ways (`src/catalogue.cpp:20`), shards are content-addressed, and
  `commit` replicates only shards whose id changed (`:1070-1071`). The figure was
  wrong by roughly 64x. The real catalogue cost is CPU — a full re-shard,
  re-encode and re-hash per mutation — not replication bytes.
- The namespace residency estimate (~1 GB at 10,000 files) was correct and was
  then retracted as overreach, on the strength of the `SharedBytes` comment at
  `src/metadata.hpp:129-133` scoping the design at "hundreds of megabytes". The
  comment and the arithmetic disagreed; the arithmetic was right. `SharedBytes`,
  `shared_ptr` snapshot views and `MetadataDelta` all eliminate *copying*. None
  reduce *residency*. Sharing a 2 GB structure rather than copying it is still
  2 GB resident, on every node, including the Pi-class ones.

## This exact pathology has been diagnosed and fixed once already, here

Before any of the design below, read the comment on `repair_step`
(`src/distributed_store.cpp:2132-2136`):

> Repair is deliberately cursor-based. The old implementation rebuilt a complete
> vector of every locally stored object and copied the complete live-object set
> on every scheduler slice, then usually examined only a handful of objects
> before the RPC operation budget was exhausted. On a media-sized store that made
> idle repair itself an O(store) hot loop.

That is this problem, named precisely, with the correct fix applied: cursors
retained across slices (`repair_push_cursor_`, `repair_pull_after_`,
`:2140-2158`), an operation budget, a `should_yield` predicate. The header
restates it as a contract: "Bounded `repair_step()` calls retain push/pull
cursors across scheduler slices; they never rebuild complete object vectors"
(`src/distributed_store.hpp:238-241`).

And yet `maintenance_objects_cached` (`src/filesystem.cpp:2392-2455`) still
builds the complete live-object vector that `repair_step` is handed — walking
every entry, pushing every non-hole extent id into a `std::vector<ObjectId>`,
then sorting and deduplicating it. The iteration was fixed; the input was not.
At 100 TB that vector is ~26 million ids, roughly **840 MB, transient, on a
schedule**.

So the discipline this plan asks for is not new to the codebase. It is already
written down, already argued, and already shipped in one subsystem. The
namespace never received it.

## What the code actually does (verified 2026-09-17)

### Every node holds the entire namespace, decoded and encoded

`MetadataReplica` is constructed per node over its own state directory and
persists `metadata/current.meta`, `committed.meta`, `checkpoint.meta`, plus
`journal.log` and `history.log` (`src/metadata.cpp:1955-1966`). It holds `cur_`
and `committed_` as `MetadataRecord` members; `MetadataRecord::payload` is
`SharedBytes` (`src/metadata.hpp:180-184`), so the full **encoded** snapshot is
resident. `MetadataManager` separately caches the **decoded** form and hands it
out as `MetadataSnapshotView::snapshot`.

`MetadataSnapshot::entries` is `std::map<std::string, FsEntry>` over the whole
namespace (`src/metadata.hpp:110`). There is no partitioning anywhere — no
per-node subset, no shard. The config comment is explicit: "Every node is
metadata-capable; this is a write durability floor, not a voter count or
convergence target." The edge node confirms it is deliberate: fi-1 runs
`hosts_extents: no`, stores no extents at all, and still serves the API and
playback, which it can only do because the namespace is entirely local.

The materialisation LRU (`materialization_cache_limit_bytes`, default 128 MiB at
`src/metadata.hpp:589`, entries weighed by `MaterializedHistoryEntry`'s
`last_used`/`bytes` at `:461-465`) bounds the *set* of decoded materialisations,
not the size of one.

### Identity is a hash over the full serialisation

`metadata_hash(generation, previous, payload)` (`src/metadata.cpp:1334-1342`) is
SHA-256 over the canonical serialised payload. `valid_metadata_record`
(`:1565-1567`) recomputes it. Acceptance certificates are signed over that hash.
`genesis_metadata` (`:1553-1564`) shows the shape: build the snapshot,
`encode_snapshot`, hash the bytes.

This is the constraint from which everything else follows. The record cannot be
produced without the whole serialisation, so the whole structure must be
materialised, so nothing can be demand-loaded.

Note that the *streaming* discipline is already present and was applied as far
as it could reach without changing the structure. `metadata_hash` says so:
"Preserve the exact canonical encoding without allocating a second copy of the
(potentially hundreds-of-megabytes) snapshot payload merely to hash it"
(`:1335-1337`). `metadata_namespace_signature` says so again: "Stream the
canonical representation into SHA-256 so a namespace containing millions of
extents never requires a second namespace-sized byte buffer" (`:4588-4590`).
Both avoid a *second* copy of something the design forces to exist at all. The
instinct was right and hit the wall of the data structure.

### A single file write costs several full traversals

Every namespace write path goes through `mutate_delta` — `commit_file`
(`src/filesystem.cpp:2250`) and `apply_namespace_batch` (`:1815`, behind
mkdir/rmdir/create/unlink/rename/chmod/chown/utimens). `mutate_delta`
(`src/metadata_manager.cpp:1872-1878`) calls `mutate_impl` with
`exact_delta = true`.

Per mutation, in `mutate_impl` (`src/metadata_manager.cpp:1622`):

| Step | Site | Cost |
|---|---|---|
| `decode_snapshot(current.payload)` | `:1669` | full parse, full allocation |
| `before.emplace(snapshot)` | `:1698-1700` | **skipped** — `exact_delta` is true on every namespace write |
| `encode_snapshot` | commit tail | full serialise |
| `metadata_hash` | `src/metadata.cpp:1334` | full SHA-256 over the payload |
| `decoded_cache_->entries != decoded->entries` | `src/metadata_manager.cpp:228` | full element-wise compare of every entry and extent vector |

Four full traversals per file write. The `before` copy is already avoided on the
paths that matter — an earlier draft of this plan listed removing it as a win
and that was wrong.

The comparison at `:228` is load-bearing, not incidental: a catalogue-only
mutation changes the record hash without changing `entries`, and consumers use
`decoded_namespace_revision_` to decide whether to rebuild indexes. It cannot
simply be deleted; it needs a cheaper witness, which is what a root gives it.

The delta machinery, by contrast, is already the right shape and shows the
intended direction. `record_entry_change` (`src/metadata.cpp:483-501`) detects
the pure-append case and ships only the new extents rather than the whole entry;
`apply_metadata_delta_in_place` (`:1242`) applies without rebuilding. Ship the
change, not the state — already established, just not for the snapshot itself.

## The arithmetic

From `snapshot_resident_bytes`' own accounting rules (`src/metadata.cpp:55-91`),
so these are the numbers the code itself would report.

`account_map_nodes` (`:37-43`) charges `sizeof(value_type) + 4*sizeof(void*)` per
entry — the comment is careful about this: "Standard tree implementations
allocate one node per value. Four pointers covers parent/children plus
allocator/alignment bookkeeping without pretending that
`sizeof(map::value_type)` describes the allocation." For
`std::map<std::string, FsEntry>` that is 32 + 72 + 32 = **136 bytes per file**.
`account_string` (`:45-48`) charges `capacity()+1`; media paths run 70-120 chars,
so **~100 bytes per file**.

`ExtentRef` is `uint64 offset` + `uint64 length` + `ObjectId` (32) + `bool hole`
= 49, padded to **56 bytes**, and `account_entry_allocations` charges
`extents.capacity() * 56`. `extent_size` defaults to 4M
(`docs/configuration.md:118`), so extent count is `library_bytes / 4 MiB`.

**File count is almost free. Library size is not.**

- fixed per-file overhead, 20,000 files: 236 bytes each, **under 5 MB total**
- extents: **~14 MB of decoded namespace per TB of library**
- encoded payload (49 bytes/extent on the wire) adds ~12 MB per TB
- **~26 MB per TB resident, on every node**

| 20,000 files, avg size | Library | Decoded | Decoded + encoded |
|---|---|---|---|
| 2 GB (TV-heavy) | 40 TB | ~560 MB | ~1.0 GB |
| 5 GB (mixed) | 100 TB | ~1.4 GB | ~2.6 GB |
| 8 GB (film-heavy) | 160 TB | ~2.2 GB | ~4.2 GB |

At ~14 MB per TB, **one decoded snapshot fills the entire 128 MiB materialisation
budget at about 9 TB of library** — roughly 1,800 films. Far below the target.

Caveats: struct sizes are derived from the declarations and should be confirmed
against `sizeof` on the actual build (Pi is 64-bit ARM, same layout expected, but
check). `capacity()` may exceed `size()` on the extent vectors after growth,
moving the real number up rather than down. And whether the head is exempt from
LRU eviction was not established by reading — `MetadataReplicaDiagnostics`
(`src/metadata.hpp:415-431`) already exposes
`materialization_cache_hits`/`misses`/`evictions`/`bytes`/`limit_bytes`, so a node
with a real library answers it in one request. Do that before sizing anything.

## What this rules out

**Putting entries on SSD, by itself, buys almost nothing.** The commit path would
still decode, re-encode and re-hash everything; moving the bytes to disk changes
where the gigabytes live, not how many are touched per write. It would also
convert an in-memory traversal into an O(namespace) disk read per commit, which
is worse.

**Fixing the remaining in-memory traversals, by itself, is not enough either.**
Removing the redundant decode and the entries comparison takes four traversals to
two, and the two that remain (`encode_snapshot` and `metadata_hash`) are the
largest. That halves a cost which needs to fall by orders of magnitude.

The identity change is not stage-two polish. It is the load-bearing piece, it
goes first, and the rest are cheap once it lands.

## The design

**Make the record a root pointer instead of an inlined namespace.** The precedent
is in the same struct, three lines above `entries`: `std::optional<ObjectId>
catalogue_root` (`src/metadata.hpp:109`). The catalogue is a root object pointing
at a manifest pointing at content-addressed shards. The namespace inlines its
entries instead. Invert that.

The catalogue side is worth reading as a working prototype of what the namespace
needs, including its bug:

- `CatalogueManifest` is a fixed array of optional `ObjectId` (`src/catalogue.cpp:127-129`)
- `catalogue_shard(id)` hashes an id to select a shard (`:131-136`)
- `shard_catalogue` partitions the snapshot (`:165-173`)
- `commit` encodes each shard, takes `object_id(encoded)`, and pushes to
  `changed_control` only when the id differs from `old_manifest` (`:1061-1072`),
  then replicates only those (`:1084-1089`)
- shards are ordinary control objects, fetched with `ensure_control_local` and
  `control_store().get()` (`load_root`, `:543-571`)

That is a one-level Merkle structure with dirty-node detection, shipped and
working. Its defect is that it re-shards and re-encodes *everything* to discover
which one shard changed — O(catalogue) per mutation to find an O(1) change. The
namespace wants this structure, deeper, with the change set carried in rather
than rediscovered.

Concretely: the record payload becomes the non-entry fields plus a
`namespace_root` `ObjectId`; entries become a content-addressed tree in the
control store, keyed by path. Consequences:

- `metadata_hash` over a few hundred bytes instead of a gigabyte
- a commit updates one path-to-root chain — O(log n) per changed path
- `metadata_namespace_signature` (`:4587-4611`) becomes a root comparison, and so
  does `cache_record`'s `entries != entries` witness (`src/metadata_manager.cpp:228`)
- the journal and `history.log` stop carrying namespace-sized payloads
- nothing forces materialisation, so demand-loading becomes possible rather than
  being a separate project
- extent lists become their own nodes, fetched when a file is opened and not
  otherwise — which is the whole of the residency problem, since extents are
  needed only for reading content and for `file_media_id`

Point lookups (`getattr`, `src/filesystem.cpp:1612-1621`) and prefix scans
(`readdir`, `:1622-1644`) read stat data and never touch `entry.extents`. A
resident stat-only cache keeps the FUSE hot path entirely in memory, so "as fast
as it is now" is not a target to hit — it is unchanged.

## Stage A results (measured 2026-09-17)

Measured, not estimated. `macha-metadata-dump --stats` was extended to report
decoded residency, extent-slot occupancy and struct sizes, and
`snapshot_resident_bytes` was exposed from `metadata.hpp` so the tool charges
exactly what the materialization cache charges. Both nodes were on 0.43.0 and
on the same head (gen=30844, `3e8dc0e936d158f9`).

**The library:** 4,808 entries (2,622 files, 2,186 directories), 424,222
extents, `extent_size` 4 MiB -- **1.618 TiB**. Encoded payload 21,425,300 bytes,
of which 20,786,878 (97.0%) is extent references and 634,388 is paths and stat
fields. 49.0 encoded bytes per extent, exactly as derived.

**Struct sizes on aarch64/GCC are as the plan assumed.** `sizeof(FsEntry)`=72,
`sizeof(ExtentRef)`=56, `sizeof(std::map<std::string,FsEntry>::value_type)`=104,
`sizeof(MetadataSnapshot)`=464. The 136-bytes-per-entry map-node figure and the
56-bytes-per-extent figure both hold; no ARM layout surprise.

**The head is pinned and exempt from the budget.** `cache_materialization_locked`
computes `incoming_pinned = record.hash == cur_.hash || record.hash ==
committed_.hash` (`src/metadata.cpp:3203`), the eviction scan skips pinned
entries (`:3210-3212`), and an entry larger than the whole budget is still
inserted when pinned (`:3230-3231`). So `materialization_cache_limit_bytes`
(128 MiB) never bounds the head -- it bounds only historical materialisations,
and there is a separate hard cap of 64 cache entries (`:3189`). When `cur_ !=
committed_`, two full snapshots are pinned at once. **Namespace residency is
unbounded by design; the LRU was never the thing holding it down.**

**The capacity-slack caveat was real and was the largest single number in the
measurement.** The decoded snapshot held **610,567 extent slots for 424,222
extents** -- 186,345 empty slots, 10.4 MB of pure allocator slack in a 36 MB
snapshot, permanent because the head is pinned. Cause: `entry(Reader&)`
(`src/metadata.cpp:119-153`) filled the extent vector with `push_back` and no
`reserve`, so every vector sat on geometric growth. Fixed in this session by
reserving exactly, bounded by what the remaining input can actually contain
(49 encoded bytes per extent) so a forged count cannot size an allocation.
Regression test: `storage_metadata/test_decoded_extent_vectors_carry_no_allocator_slack`.

| | before the reserve fix | after |
|---|---|---|
| extent slots (424,222 in use) | 610,567 | 424,222 |
| decoded bytes | 36,199,190 | **25,763,870** |
| + encoded payload | 57,624,490 | **47,189,170** |
| decoded per TiB | 22.4 MB | **15.9 MB** |
| materialisation per TiB | 35.6 MB | **29.2 MB** |

That is a **28.8% cut in decoded namespace residency for a one-line change**,
with no format change, no migration and no cluster coordination. It is not a
substitute for anything below -- the growth is still linear in extents and still
whole-library -- but it is real, it is shipped in the working tree, and at the
100 TB target it is roughly 640 MB per node.

**The projection, on measured constants** (20,000 files, 4 MiB extents):

| Library | extents | decoded | decoded + encoded, per node |
|---|---|---|---|
| 1.6 TiB (es-1 today) | 424k | 26 MB | 47 MB |
| 40 TB | 10.5M | ~640 MB | ~1.2 GB |
| 100 TB | 26.2M | ~1.6 GB | ~2.9 GB |
| 160 TB | 42M | ~2.5 GB | ~4.7 GB |

The plan's own arithmetic (~14 MB/TB decoded, ~26 MB/TB combined) was correct
and slightly conservative. Nothing in Stage A weakens the case; the residency
claim is now measured rather than derived.

**One materialisation equals the entire 128 MiB budget at ~4.6 TiB of library**,
not the ~9 TB the plan estimated. Because the head is exempt, what actually
happens past that point is not eviction of the head but the opposite: the head
stays, the cache holds nothing else, and every historical materialisation
becomes a full delta-chain replay.

**The edge node proves the thesis on its own.** fi-1 runs `hosts_extents: false`,
has no media disks and uses 8.2 GB of its root filesystem in total. It holds a
**byte-identical** 47 MB materialisation describing 424,222 extents of content it
does not store and cannot serve. Every byte of that is the namespace being
inlined in the record rather than addressed by it. Process RSS at the time of
measurement: es-1 478 MB, fi-1 295 MB.

**Not established: the live cache counters.** `MetadataReplicaDiagnostics`
(`materialization_cache_hits`/`misses`/`evictions`/`entries`/`bytes`) is only
reachable through `GET /api/v1/status`, which requires the `view_status` role;
the anonymous account lost that role and `macha-users` cannot mint a credential
without stopping the node. Accounts that hold it exist (`tom`, `root`, `webclient`
and others). This matters less than it did before Stage A -- the head is pinned
and exempt, so the counters describe historical reconstruction behaviour rather
than the residency question -- but it is the one Stage A item still open.

**What Stage A changes about the plan.** Nothing structural. The design is
unchanged, the ordering is unchanged, and the identity change is still the
load-bearing piece. Two things are now sharper: the LRU limit is not a bound on
the namespace and should never be described as one, and the crossover where a
single snapshot exhausts the historical-materialisation budget is at roughly
4.6 TiB -- close enough to es-1's present 1.6 TiB that it is a near-term event,
not a target-scale one.

## Stages

**Stage A — establish the real numbers. DONE 2026-09-17. See "Stage A results".**

## Stage B progress (2026-09-17)

**The substrate exists and is tested; it is not yet SM14 and nothing reaches
it.** `src/namespace_tree.{hpp,cpp}`, `tests/test_namespace_tree.cpp`, six cases
green. `encode_snapshot`, `decode_snapshot` and `MetadataRecord` are untouched,
so this changes no on-disk or wire format and deploys as dead code.

Three design decisions were settled in building it, and the first was not in
the plan:

- **The tree must be history-independent, and an incrementally built
  fixed-fanout B-tree is not.** The same entry set has to produce the same root
  whatever sequence of inserts, deletes and merges reached it, because
  `metadata_namespace_signature` and `cache_record`'s `entries != entries`
  witness (`src/metadata_manager.cpp:228`) become root comparisons: two nodes
  that reconcile independently to the same namespace must agree on the root or
  they report divergence that does not exist. Node boundaries are therefore
  chosen by hashing keys, not by fill factor -- the prolly-tree/Dolt structure.
- **Boundaries hash the key only, never the value.** Generic content-defined
  chunking hashes the whole serialised item, so a value change can move a
  boundary and rewrite its neighbours. Every namespace write *is* a value change
  on an existing path -- a size, an mtime, an extent -- so that would be the
  common case rather than the rare one. Key-only boundaries mean a value change
  rewrites exactly one leaf and the path to the root. The cap that stops a
  pathological key set making an oversized node is on item *count*, which is
  still a pure function of the sorted key sequence; a cap on encoded *bytes*
  would break history independence and there deliberately is none.
- **Extents are addressed from the leaf, not inlined in it**, past a small
  threshold (8, or 392 bytes at the 49 bytes/extent the snapshot already costs).
  A leaf therefore stays bounded by the key set, and Stage D becomes a change of
  *when* an extent node is fetched rather than another format change. The extent
  sequence is itself chunked, with boundaries on each extent's own content
  address, which is what makes an append stable: adding an extent cannot move
  the boundaries of the extents already written.

**Measured, on the shapes a media library actually has:**

| | |
|---|---|
| one file's mtime/size changed, 480-entry library | **<= 12 nodes rewritten**, against the whole library on every commit today |
| one extent appended to a 4,000-extent file | **3 nodes rewritten of 15** -- tail chunk, extent spine, leaf |
| largest node, with a 12,500-extent film present | **< 64 KB** (that file's extent list is 612 KB inline today) |
| point lookup | a path from the root, never the namespace |

What Stage B still owes before it can be called done: the SM14 record shape and
`decode_snapshot` dispatch alongside SM13; a fuzz case in the shape of
`test_fuse_journal_fuzz_every_frame_mutation_still_starts`; a stat-only read
path that provably fetches no extent nodes (the decoder already takes the flag,
nothing public exposes it); and the `macha-metadata-dump` mode that builds the
tree from the live es-1 head so these figures come off a real 1.618 TiB
namespace rather than a generated one.

**Stage B — the Merkle namespace, behind a new snapshot version.** Define the
tree, node encoding and root. `decode_snapshot` already carries SM5 through SM13
(`src/metadata.cpp:643-656`), `encode_snapshot_v7`/`v8` (`:283-345`) exist
specifically to reproduce historical byte-exact encodings for journal replay, and
`encode_snapshot_for_delta` (`:357-377`) dispatches on delta version to pick the
right one. Staged metadata format migration is established practice here, with a
worked pattern for keeping old encodings byte-exact. Write the tree as SM14
alongside the existing path, readable both ways, not yet authoritative.

**Stage C — the commit path.** `mutate_impl` stops decoding the whole snapshot,
mutates the tree, updates the root. `apply_metadata_delta_in_place` becomes a
tree operation. The change set is carried into the commit rather than
rediscovered — the mistake the catalogue's `commit` makes, not repeated.
`WriteHandle` already demonstrates the habit at file scope: `ChangedRange` with
`note_changed_range` and `range_changed` (`src/filesystem.hpp:189-193`,
`src/filesystem.cpp:615-636`) exists so unchanged extents are never copied merely
to discover they are unchanged. Same instinct, applied to the namespace.

**Stage D — demand-loaded extents.** The pattern already exists in
`ReadHandle::extent` (`src/filesystem.cpp:173-208`): fetch one extent on demand,
cache exactly one, and — note the comment at `:184-186` — release the previous
before reserving the replacement "so a two-buffer handoff cannot consume the
viewer headroom indefinitely". Extent-list nodes want the same treatment with a
bounded cache. That cache should be a `RetainedMemoryLedger` client
(`src/retained_memory.hpp:20-52`, `MemoryClass`/`MemoryOwner` with reserves,
leases and shedding) rather than a private LRU, so it participates in the same
pressure accounting as everything else. `file_media_id`
(`src/filesystem.cpp:2001-2017`) hashes the whole extent list and is a pure
function of immutable content — persist it rather than recomputing it, or the
media-index rebuild re-reads the library.

**Stage E — the migration.** See below.

**Stage F — the dependent O(N) work**, cheap once B-D land and pointless before.

## Migration: a re-root, not a rebuild

The namespace content is not the identity. Paths, stat fields and 32-byte
`ObjectId`s are data; those ObjectIds address content in the object store, which
no metadata format change touches. Every extent stays where it is and keeps its
id. **The library survives.** What is discarded at the cut is ancestry —
metadata history before the boundary, old acceptance certificates (signed over
old-scheme hashes), and the ability to roll back past it. History serves
reconciliation and repair, not serving files.

`recover_from_seed` (`src/metadata.cpp:2097-2131`) already performs the shape of
the operation: take a complete `MetadataRecord`, quarantine the existing
checkpoint/journal/history/heads, install the seed as both `cur_` and
`committed_`, reset the checkpoint, rebuild history from it. And both `seed()`
(`:4362`) and `remember_committed()` (`:4404`) carry
`const bool fresh = committed_.generation <= 1;`, which bypasses the ancestry
checks — so a freshly re-rooted replica accepts a record with no shared history.

**It is not a migration primitive as it stands.** `recover_from_seed` sets
`recovery_required_ = true` and the comment is explicit that a cache seed "cannot
stand in for the durable acceptance evidence which was lost". A migration needs a
variant that *grants* authority under an operator-initiated, cluster-coordinated
condition.

The codebase already has the vocabulary for exactly that kind of precondition.
`metadata_branch_floor` and `retention_baseline_complete`
(`src/metadata.hpp:93-103`) are a durably-established cluster-wide condition that
must hold before a destructive operation (retention release, reachability GC) is
permitted — "no participant may subsequently author from an ancestor/sibling
behind this floor". `mutate_impl` gates on an analogous policy transition,
refusing with "metadata write-floor transition is not durably accepted"
(`:1670-1671`). The migration authority grant should be built in that shape, not
by loosening the recovery path. This is the safety interlock of the whole
metadata system and the single most dangerous piece of this plan: design it
deliberately and rehearse it on a throwaway cluster.

**It is a flag day.** A node on the old scheme computes the old hash in
`valid_metadata_record` and cannot validate the new root. Every node cuts over
together. The redeeming property is that the new root is self-contained: a node
down during the cutover adopts it wholesale on return through the seed path
rather than replaying a chain it cannot get. With gbni-2 offsite at inverbeg and
fi-1 behind CGNAT, sequence carefully.

## What this does NOT fix

All separately O(N), all surviving the re-root untouched, all on the same 100 TB.
Several are already recorded under "P1 — Scaling cliffs" and should be read as
dependent on this work rather than independent of it:

- **`maintenance_objects_cached`** (`src/filesystem.cpp:2392-2455`) — the
  ~840 MB live-object vector described at the top of this plan. `repair_step`
  already refuses to rebuild complete vectors; this is the one still handed to
  it. A Merkle tree over extents is the primitive that makes reachability
  incremental, so it becomes tractable after Stage B and not before.
- **FUSE `readdir`** (`src/fuse_frontend.cpp:5349-5356`) walks every known inode
  and filters by parent, per listing — while `FileSystem::NamespaceIndex` already
  maintains a parent→children map the frontend does not use
  (`src/filesystem.cpp:1633-1642` shows the right shape). Already the top entry
  under P1 scaling cliffs, independent of storage format, fixable now.
- **`file_media_id`** re-hashes the full extent list per file on every media index
  rebuild. Stage D.
- **The catalogue**: `list()` (`src/catalogue.cpp:982-999`) and `search()`
  (`:1001-1020`) as full scans with per-item copies, `snapshot()` returning
  `*current_snapshot()` by value (`:829-831`), and eight mutation sites opening
  with `auto current = *current_snapshot();` then re-encoding all 64 shards.
  Already recorded under P1 scaling cliffs. It also has no equivalent of
  `snapshot_resident_bytes` — `profile_weight`
  (`src/media_information.cpp:40-46`) measures only the pending publication
  queue, so nothing currently knows what a catalogue costs in memory. The
  catalogue wants the same treatment the namespace is getting here, and is the
  smaller half of one problem.

## Open questions

1. **`extent_size` as a cheap orthogonal lever.** Everything above scales with
   extent count, which is `library_bytes / extent_size`. At 4 MiB, 100 TB is
   ~26M extents; at 16 MiB it is ~6.5M — 4x less of everything. The usual cost is
   read amplification on small random reads, which barely registers for
   sequential media. No asymptotic change, but every constant moves 4x for a
   config change. Applies only to newly written files. `extent_size` appears both
   in node config (`src/filesystem.hpp:439` reads `n_.config().extent_size`) and
   in `MetadataSnapshot` as cluster policy (`src/metadata.hpp:82`) — establish how
   those interact before touching it.
2. **Tree shape and fanout.** Path-keyed radix/B-tree versus hash-keyed.
   `catalogue_shard` (`src/catalogue.cpp:131-136`) hashes ids, which balances well
   but destroys locality; the namespace needs `readdir` to stay a prefix scan,
   which argues for path keying with an explicit fanout bound. Media libraries are
   deeply nested and unbalanced, so the bound matters.
3. **Where tree nodes live.** Reusing the existing content-addressed control
   store — the path catalogue shards already take, via `replicate_control` and
   `ensure_control_local` — means no new dependency on the Pi nodes and inherits
   replication, repair and GC. The alternative (LMDB or similar) is more
   conventional but adds a dependency and a second durability model. Prefer the
   existing store unless Stage A shows a reason not to.
4. **Conflict reconciliation over a tree.** `merge_catalogue_snapshots`
   (`src/catalogue.cpp:397-454`) and the namespace three-way merge operate on
   materialised maps. A tree makes divergence detection much cheaper (compare
   subtree roots and descend only where they differ) but the merge itself needs
   rewriting against the new structure.

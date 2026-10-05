# Audit: work proportional to the library, and writes that wait on the cluster

2026-10-05, tree at `45cba94` (0.89.1). Three read-only sweeps of `src/`, by
area. Items marked **V** were re-read by the session that commissioned the
sweeps; the rest are as reported, with file and line, and should be re-read
when their stage is implemented. Nothing here was timed except where a
measurement is quoted.

## The requirement this is measured against

Operator, 2026-10-05:

- File access and operations are fast and consistent locally, always. No
  local delay on an idle node.
- A mutation may be journalled locally, provided a crash or reset cannot lose
  it, and the peer catches up in its own time.
- All reads and writes use one mechanism, behind the interface FUSE uses.
- A failed journal write, or the loss of the disk itself, is acceptable.
  What is not: a hard failure, a node that cannot recover, or loss of data
  the cluster had. Degrade and carry on.

Stated for the code: no request path and no per-commit path does work
proportional to the namespace or the catalogue, and no caller waits on a peer.

## What exists

The FUSE frontend journals each namespace operation (fsync), applies it to an
in-memory view at once, and a worker publishes ordered batches of up to 256
operations as one metadata commit. FUSE lookups therefore see unpublished
operations. That is the intended model, and it is private to
`src/fuse/fuse_frontend.cpp`.

Its view is not an overlay on the tree. It is `paths`, a map of every path in
the namespace to an inode holding two full entries with extents (**V**,
`fuse_frontend.cpp:705`).

## Findings

n = namespace entries, C = catalogue items, H = hints, G = tombstones.

### 1. Local reads

| # | Where | What | When |
|---|---|---|---|
| 1.1 **V** | `filesystem.cpp:1530` `namespace_index()` | Whole-tree walk decoding every entry with extents into three whole-namespace maps. Cached by head generation; no single-flight. Seen live in a stack on fi-1. | First lookup after every commit. `getattr` checks it twice, `readdir` twice, `resolve_new_path` three times, `open_write` three to six times while holding `open_writes_mutex_`. |
| 1.2 **V** | `fuse_frontend.cpp:4950-5105` `refresh_namespace_if_stale` | Whole-tree walk with extents under the global `namespace_mutex`, then a second pass over `paths`. | Top of every FUSE operation once the namespace revision has moved, which every commit does, FUSE's own included. |
| 1.3 **V** | `fuse_frontend.cpp:5309` readdir; also rmdir `:5397`, rename `:5500-5517` | Scan of all of `paths`, taking every inode mutex. | Every FUSE readdir, rmdir, rename. |
| 1.4 | `fuse_frontend.cpp:2559`, `:4930` | `reclaim_inode_if_quiescent` and `confirm_data_from_snapshot` scan all paths or inodes. | Per confirmed operation; per request while a confirmation is pending. |
| 1.5 | `filesystem.cpp:2126` `find_media` | Whole-tree walk hashing every file's extent list, under `media_index_mutex_`. | First media-id miss after any namespace change: playback start, peer media RPC, hydration. |
| 1.6 | `namespace_tree.cpp:486,562,1118` | Prefix and emptiness walks decode extents for every entry and cannot stop early. | rmdir, rename, catalogue prefix scans, `readdir`. |

### 2. Local writes

| # | Where | What |
|---|---|---|
| 2.1 **V** | `metadata_manager.cpp:1509,1754,1761` | `mutation_mutex_` is held across the retention claim round trips and `publish_commit`. Every commit on a node queues behind the peer. Measured: 2 to 3 s per commit, about 1 s publish and 0.2 to 0.5 s claims. |
| 2.2 **V** | `manage_api.cpp:593` | A global mutex around every non-GET management request. Concurrent deletes reach the filesystem one at a time, so the 0.89.1 group commit never has two to share. Measured: 12 deletes, 39 to 45 s. |
| 2.3 | `ingest.cpp:1546-1921`, `filesystem.cpp:1357` | Callers outside FUSE make one commit per operation, with no journal. Ingest: one per missing parent, one create, one per 64 MiB checkpoint, one rename (a 4 GiB file is about 67 in series). Torrent adoption: parents + 3. A torrent job: about 7 coordinator commits more. FUSE batches namespace operations but each data publication is its own commit. |
| 2.4 **V** | `namespace_tree.cpp:682-778` `update_namespace_tree` | Lists every leaf (`collect_leaves`, n/32 entries) and rebuilds the whole branch spine on every commit, on the author and on every replica that imports it. Replay mode writes every spine node unconditionally. |
| 2.5 | `namespace_tree.cpp:455,249` | Changing one entry decodes and rebuilds the extent sequences of every neighbour in its leaf (about 32 files). |
| 2.6 | `metadata_manager.cpp:1232,1552,1675,1721`; `node_services.cpp:322`; `filesystem.cpp:110,1897` | The non-namespace snapshot (tombstones, conflicts, torrent requests) is decoded two or three times, scanned, sorted, copied per queued batch, re-encoded and hashed whole on every commit. |
| 2.7 | `metadata_manager.cpp:1704` | Every standing conflict is looked up with extents on every commit. |
| 2.8 **V** (fsync) | `fuse_frontend.cpp:1639,5366,1711` | The journal fsyncs per append under `namespace_mutex`: twice for mkdir and create, once per descendant for a directory rename. Lookups wait behind them. |
| 2.9 | `namespace_tree.cpp:1065-1133` | The batch working set uses linear scans; a directory rename is quadratic in its size. |

### 3. Work each commit sets off in the background

Every commit, local or remote, on every node:

| # | Where | What |
|---|---|---|
| 3.1 **V** (cache key) | `filesystem.cpp:2462` `maintenance_objects_cached`; `node_horizon_builder.cpp:13-75`; `maintenance.cpp:540-555,784-797` | Reachability inventory rebuilt by a full walk and sort, and the release horizon by two more full walks. Not gated on busy. |
| 3.2 **V** (call site) | `availability_service.cpp:288-439`; `ledger/availability.cpp` | Roll-up, peer survey and path table redone from the root; the path table file rewritten whole. Storage events also trigger it, so it is permanently due during a download. Reported defect: a deferred roll-up leaves the head marked changed, so the same survey repeats on every wake. No pause callback is passed. |
| 3.3 | `distributed_store.cpp:2307-2763` | A commit during a repair pass forces another whole-store pass; the peer check reads, decrypts and hashes each object, 16 at a time. |
| 3.4 | `retention.cpp:647-705` | Claim release scans every claim under the mutex that a commit's claim also needs. |
| 3.5 | `maintenance.cpp:185,1084` | Tombstones mature one deadline at a time, each a commit of its own, each restarting 3.1 to 3.3 on both nodes. |
| 3.6 | `media_catalogue.cpp:3961`; `media_information.cpp:121-187,376,430,442` | A full catalogue scan 10 s after any namespace commit; a whole-namespace walk per hint and per profile publication; each profile and keyframe index its own whole-catalogue commit. |

### 4. Catalogue and the management API

| # | Where | What |
|---|---|---|
| 4.1 **V** (shard loop) | `catalogue.cpp:1111-1234` | One catalogue write copies the snapshot, re-shards, re-encodes and hashes all 64 shards, decodes the metadata record twice, and re-reads newly staged artwork in full. |
| 4.2 | `catalogue.cpp:1641`; `node_services.cpp:378` | The commit's retention step loads the old and the new catalogue in full. |
| 4.3 | `catalogue.cpp` `mutation_mutex_` (nine sites) | Held across repair, shard replication and the commit; shared with the scanner and the media-information service. |
| 4.4 | `media_catalogue.cpp:3053-3148` match | Provider call, artwork downloads in series, durability wait, one catalogue commit, then a whole-namespace walk hashing every extent list to find a path it was given. Two more catalogue commits follow in the background. |
| 4.5 | `media_catalogue.cpp:3225-3312` artwork choose | Repeats the options call the client just made, downloads, stores with replication, re-reads the image, commits. |
| 4.6 | `media_catalogue.cpp:3035-3217`, `:1815` | `editor_mutex_` held across provider HTTP; the MusicBrainz gate across its rate-limit sleep, shared with the scanner. Provider failures are returned but not logged. |
| 4.7 | `manage_api.cpp:742-778` unmatched list | Per item: two `getattr` (four index checks) and two extent-list hashes; then every catalogue item scanned for bindings; no pagination. |
| 4.8 | `manage_api.cpp:999`; `media_catalogue.cpp:2839` unmatched item | Three `getattr`, two linear hint scans, and for music a tag and artwork read under `config_mutex_`. |
| 4.9 | `catalogue_hints.cpp:210-791` | No index by id; every delete, retry or rename rewrites and fsyncs the whole file under the store's mutex. |
| 4.10 | `manage_api.cpp:114,1029`; `item_availability.cpp:93`; `catalogue.cpp:530,838,1069,1088,1619` | Per-request scans of the whole catalogue: bindings per directory listing, the availability table after any commit, artwork lookup per poster, status, artist artwork, list, search. |

### 5. Smaller, for the record

Serial per-object probes with decrypt on the peer for prompt replication
(`distributed_store.cpp:1312`); one torrent extent stored at a time
(`torrent_manager.cpp:1147`); tree nodes fetched one at a time on a miss
(`namespace_control_store.cpp:40`); 2 s, 5 s and 500 ms polling loops in the
torrent coordinator, cluster job view and ingest; ingest state file rewritten
whole per checkpoint; unused whole-copy accessors (`MetadataManager::snapshot`,
`FileSystem::snap`, `mutate` without a delta).

## Not covered

`MetadataReplica` beyond import and materialisation; `metadata_merge.cpp` in
detail; the FUSE data path (write, read, data and durability loops);
`net.cpp`; metadata upkeep and catalogue repair commits; playback's use of
media profiles; the cost of a tree-node read in the local control store.

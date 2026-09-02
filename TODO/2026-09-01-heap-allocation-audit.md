# Heap allocation and ownership audit

Date: 2026-09-01

Status: source audit and application-owned lifecycle remediation complete;
large-index diagnostics and loaded RSS/external-library measurement remain.
All Macha services remained stopped during this work.

## Scope and method

This pass walked all production headers and sources under `src/` for explicit
allocation APIs, owning standard-library containers, shared ownership, worker
lifetime, queued payloads, full snapshot/payload copies, and external-library
allocation. C++ strings, vectors, maps, functions, promises and exceptions all
allocate indirectly, so a list of `new` calls would be misleading: Macha uses
almost no raw `new`/`malloc` itself. The useful unit is the long-lived owner or
the operation that creates a large allocation graph.

The findings below distinguish:

- **unbounded owner**: process-lifetime state can grow without a configured
  byte/count ceiling;
- **lifetime-bounded owner**: state is reclaimed, but only when a file,
  session, connection, or job ends;
- **bounded owner**: an explicit count or byte ceiling exists;
- **amplifier**: a complete tree/payload is copied or decoded even though it is
  not retained permanently;
- **external owner**: libav, libtorrent, curl, TLS, FUSE, or another library
  owns the allocation and its lifetime must be measured separately.

## P0 findings

### 1. Detached FUSE inodes are never reclaimed

`FuseFrontend::State` owns every inode strongly in both `paths` and `inodes`
(`src/fuse_frontend.cpp:490`). Namespace refresh, unlink and rename-over erase
the path and clear `published_path` (`src/fuse_frontend.cpp:3782` and
`src/fuse_frontend.cpp:3813`). `release()` decrements `open_handles`
(`src/fuse_frontend.cpp:4875`). There is no `inodes.erase()` or
`inodes.clear()` anywhere in production code.

Consequently, every inode identity ever created during the process lifetime is
retained after unlink/replacement and final close. Each retained inode owns two
`FsEntry` graphs (including extent vectors), paths and strings, `data_ops` and
their spool-hash vectors, optional entries, synchronization objects, and any
capacity those containers accumulated. Rsync replacement and `rm -rf` churn are
exactly the workload that makes this table grow.

This is a definite application lifetime leak. Reclamation must require all of:
no published path, no open/writable handles, no durability/data/namespace work,
no publication or unconfirmed state, and no queue/ticket references. It needs
unlink, rename-over, remote unlink, open-unlinked-file, publication failure,
journal recovery, and shutdown regressions.

### 2. Locked metadata reconstruction still builds and caches every full tree

The public historical materializer was changed to apply deltas to one working
snapshot and cache only its final result. The separate
`MetadataReplica::materialized_locked()` path still copies the complete parent
snapshot once per delta and passes every intermediate tree through the cache
(`src/metadata.cpp:2708`, especially `src/metadata.cpp:2777`). It also creates a
fresh encoded snapshot payload for every intermediate record.

This path is used inside replica-locked validation/recovery paths, so a long
delta chain can simultaneously allocate multiple complete namespace trees and
payloads before eviction catches up. It must use the same single-working-tree,
final-result-only rule as the public path. A synthetic long-chain peak-live-byte
test is required; final cache size alone cannot prove a bounded reconstruction
peak.

### 3. Hydration creates a fresh OS thread for every object

`CacheHydrator::loop()` launches every object with
`std::async(std::launch::async)` (`src/hydration.cpp:677`). Concurrent futures
are count-bounded by `max_inflight`, but completed threads are continually
destroyed and replaced for the entire lifetime of hydration. Each thread runs
the full distributed hydrate path and its object/RPC buffers.

This is not a retained C++ container leak, but it is an unbounded thread and
allocation-lifetime churn mechanism. Hydration needs a fixed, byte-bounded
executor, cancellation-aware queueing, and tests proving a fixed thread count
across many thousands of objects.

### 4. Most RPC request queues are count-bounded but not byte-bounded

`RpcServer::admit_locked()` applies an aggregate byte ceiling only to metadata
mutation requests. Fast control, ordinary control, foreground, read-ahead,
loader, and speculative queues accept up to 512 complete `RpcFrame` payloads
per queue without an aggregate byte limit (`src/net.cpp:2934`). The message
assembler itself allows up to 256 MiB aggregate and 128 MiB per message; queued
jobs then own the assembled payload.

Count bounds are insufficient for variable-sized frames. Each class needs an
aggregate byte limit, with reserved non-borrowable control/viewer capacity and
prompt payload release on cancellation/disconnect. Tests must saturate each
queue with maximum legal payloads and assert peak owned bytes, not just job
count.

## P1 unbounded or lifetime-bounded owners

### Playback

- `PlaybackManager::Impl::probe_cache` is a process-lifetime map with no erase,
  clear, entry ceiling, or byte ceiling (`src/playback.cpp:556`). Immutable media
  identity makes entries valid, but deletion/pruning does not reclaim them.
- Per-session subtitle segment text accumulates in `SubtitleCache::segments`
  (`src/playback.cpp:537`, populated at `src/playback.cpp:1549`). It is reclaimed
  with the session, but has no per-session count/byte ceiling.
- `vod_plan_cache` is correctly bounded to 64 entries. Probe flights and pending
  profile publications are erased on completion. Sessions are reaped by the
  cleanup loop.
- Each active libav session owns a worker thread, segment store, input/output
  buffers, demux/mux contexts, codec contexts, frames, packets, scalers,
  resamplers and audio FIFOs. Project-side cleanup is present, and the segment
  store has a configured memory limit plus spill, but external allocation bytes
  are not included in current memory diagnostics.

### Catalogue and media providers

- TMDB movie/show/season caches, MusicBrainz release/recording/cover caches,
  and Discogs search/release caches are process-lifetime maps without eviction
  (`src/media_catalogue.hpp:148-193`). They grow with every distinct lookup and
  may retain complete parsed JSON trees and artwork URLs.
- Catalogue profile resolved caches are pruned with live media IDs and flight
  records are removed, so those paths have lifecycle cleanup.
- The durable hint table is path-keyed and terminal/origin records are erased;
  ingest and torrent terminal jobs are also erased.

### Write handles

- When trace logging is enabled, every write handle retains one
  `DiagnosticWriteRange` per write plus a map entry per distinct offset/length
  for the entire handle lifetime (`src/filesystem.cpp:740-800`). It also scans
  all earlier ranges on each write, producing quadratic CPU work. A long-lived
  or append-verify handle can therefore consume unbounded memory and CPU solely
  because verbose diagnostics are enabled.
- Normal write state is byte/count bounded by the extent pipeline and FUSE
  pending-write admission, but a handle also owns extent manifests and changed
  ranges proportional to the file. Those are file-size bounded, not process
  byte-budgeted.

### Storage and cluster indexes

- `LocalStore::packed_` owns one index record per packed object.
- persistent cache LRU maps own one entry per cached object;
- retention state owns per-object/per-node causal maps;
- service maintenance vectors duplicate live/control/garbage object IDs;
- namespace and media indexes duplicate paths and names while holding a shared
  complete metadata snapshot (`src/filesystem.hpp:282-307`).

These are bounded by disk inventory, live namespace, or configured cache size,
not necessarily by a resident-memory budget. They need byte diagnostics before
being called harmless on a large library.

## Full-tree and full-payload amplification

These operations reclaim their temporaries but can create a very high peak and
can overlap on different workers:

- `MetadataManager::snapshot()` deep-copies the complete namespace
  (`src/metadata_manager.cpp:1263`). Callers should acquire `snapshot_view()`
  unless mutation is intended.
- Every metadata mutation decodes a complete snapshot. Non-exact mutations copy
  it again as `before`, then encode a complete payload and may encode a delta
  (`src/metadata_manager.cpp:1336-1397`).
- Background `repair_once()` copies the complete materialized tree and can copy
  it again for retention-baseline construction (`src/metadata_manager.cpp:1515`
  and `src/metadata_manager.cpp:1546`).
- `CatalogueManager::snapshot()` deep-copies the whole catalogue
  (`src/catalogue.cpp:805`). Catalogue mutation methods copy the whole current
  snapshot before changing one item/profile (`src/catalogue.cpp:894`, `913`,
  `936`, `1122`, `1151`, `1174`, `1227`, and `1314`).
- `CatalogueScanner::prepare_hint()` takes a complete catalogue copy per hint
  (`src/media_catalogue.cpp:2663`). A large ingest hint batch can repeat that
  allocation once per file.
- `catalogue_snapshot_files()` builds a second vector containing every file and
  copies every `FsEntry`, including its extent vector
  (`src/media_catalogue.cpp:2572`).
- Catalogue sharding copies every item/profile into one of 64 independent
  snapshot objects (`src/catalogue.cpp:165`).
- Metadata and catalogue encode/encrypt/RPC paths commonly coexist with decoded
  trees, output `Bytes`, encrypted `Bytes`, assembled frames, and receiver
  decode buffers. The DATA extent path is now fixed-thread and byte-admitted,
  but the metadata/control equivalents are not all byte-budgeted end to end.
- Status and catalogue HTTP endpoints construct complete JSON DOMs and serialized
  response bodies. HTTP request bodies are configured-byte-bounded, but large
  response construction is not streamed.

## Explicit allocation and worker inventory

There are no significant raw project `new`, `malloc`, `calloc`, or `realloc`
call sites. Ownership is predominantly `make_shared`, `make_unique`, STL
containers, futures and `std::jthread`.

Long-lived project threads include cluster maintenance/recovery/connectivity/
telemetry/local-writer workers; service startup/maintenance; RPC accept,
health, per-connection reader/writer and class workers; HTTP accept/workers;
FUSE namespace/durability/data/broker workers; fixed filesystem extent workers;
catalogue, media-information, ingest, torrent and hydration workers; playback
cleanup/profile publishing and one worker per active media pipeline; local-store
scan and durability workers; and status/management persistence workers.

Notable external allocation APIs are:

- libav `av_malloc`, AVIO/format/codec/frame/packet/FIFO/scaler/resampler
  allocation in `media_engine.cpp` and `media_metadata_ffmpeg.cpp`;
- curl handles and response buffers in catalogue/public-connectivity HTTP;
- miniupnpc device/URL/data structures;
- libtorrent session, piece/cache and alert state;
- TLS/crypto contexts and socket buffers;
- kernel FUSE request/page cache state.

The inspected libav project wrappers generally have paired cleanup, including
exception paths. That establishes intended lifetime, not a byte ceiling and not
proof that the library has no internal growth.

## Remediation order and proof

1. Reclaim detached, quiescent FUSE inodes and expose current/peak inode count
   and deep estimated bytes.
2. Unify both metadata materialization paths around one mutable working tree;
   cache only the requested result.
3. Replace hydration `std::async` with a fixed byte-bounded executor.
4. Add aggregate payload-byte admission to every RPC class while preserving
   control/viewer reservations.
5. Bound or prune playback probe/subtitle and provider JSON caches; make
   immutable-profile pruning follow media deletion.
6. Remove per-hint catalogue snapshots and use immutable shared views; batch
   catalogue mutations/profiles rather than copying the catalogue per item.
7. Bound trace-only write diagnostics with a ring/sample and an indexed overlap
   structure.
8. Add allocation-site diagnostics: current/peak counts and bytes for each
   owner above, plus process RSS/swap and external media-session estimates.
9. Run deterministic create/unlink/replace, long-delta reconstruction,
   hydration, max-frame queue, catalogue-hint, playback-session and sustained
   rsync workloads. The gate is a stable plateau over repeated fill/drain
   cycles, not merely a small logical cache reading after the burst.

This audit invalidates any conclusion that allocator behavior alone explains
the multi-gigabyte RSS. It finds a definite FUSE lifetime leak, multiple other
unbounded caches, a second full-history-tree reconstruction amplifier, and
several byte-unbounded queues. Their individual contributions to the captured
Linux RSS still require counters or allocation profiling at the owning call
sites.

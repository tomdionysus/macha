# Storage, disks and filesystem behaviour

## How files are stored

A file is namespace metadata plus an ordered list of immutable extents.

```text
pathname
   |
   v
metadata manifest
   |
   +--> extent A --+
   +--> extent B --+--> deterministic DHT owners
   +--> extent C --+          |
                              +--> node A / local disk
                              +--> node B / local disk
                              +--> node C / local disk
```

Extents are addressed by SHA-256. Sequential writes publish completed extents as they are filled; the whole file is not held in memory. Reopening an existing file for append/resume preserves every complete committed extent by reference. If EOF is inside the final extent, only that one tail extent is fetched lazily to seed the append buffer; aligned appends fetch no old extent data. Repeated flush/fsync on an open append handle re-arms only the committed partial tail, so later writes remain extent-native. Random writes use a temporary file under `state_path/tmp`; commit rebuilds the extent manifest from staged bytes, but unchanged extents are matched by offset/length/content hash and their existing `ExtentRef` is reused instead of retransmitted.

Reads try, in order:

1. authoritative local storage;
2. persistent local cache;
3. another DHT node.

Sequential readers retain the current decrypted extent. Speculative reads are scheduled through the cache hydrator rather than by per-handle futures. Read-ahead, the remaining extents of the current file, and catalogue-predicted next media all submit ordered hints with independent priorities.

Remote extent retrieval is replica-aware. Independent foreground reads choose among the configured replica set using current load, recent transfer latency and failures. Speculative hydration uses the same measurements but prefers idle replicas and yields to foreground work. Concurrent requests for the same object are coalesced; if playback needs an extent already being hydrated, that transfer is promoted rather than duplicated. A successful remote foreground fetch is still persisted asynchronously, and if this node should own the extent the same bytes can become the authoritative replica.

## Nodes and disks

A DHT node may have several local storage backends. The cluster sees one node whose placement weight is the aggregate configured capacity of its adopted backends. A second capacity-weighted shard layer chooses the local authoritative disk.

Each adopted backend has a `.macha.backend` marker and a matching identity under `state_path/backend-identities`.

If an adopted disk disappears temporarily, it goes offline but keeps its placement weight. Reads and writes fall through to surviving disks without making a transient unmount redefine the whole placement map. If the disk returns with the expected marker, local rebalance converges objects back to their intended proportional placement.

Adding or removing a backend in YAML and sending `SIGHUP` changes capacity without changing node identity. A newly adopted backend gains a proportional share of local placement; removing it from configuration removes that share. Current free space is never used as a placement weight.

Each authoritative backend keeps a small two-slot checksummed `.macha.accounting` journal. A normal restart restores the exact encrypted bytes-used count from that journal without walking `objects/`. Ordinary authoritative puts/removes retain the strict single-mutation transaction: Macha durably records the pending content-addressed mutation and the object/pathname before returning success. Spool-backed publication instead uses the already-durable FUSE spool+journal as its WAL: the backend writes one durable DIRTY accounting marker for a deferred generation, lets the kernel batch immutable-object writes, and checkpoints CLEAN only after a generation-qualified stable-storage barrier. A crash in such a deferred generation therefore triggers the existing object-tree reconciliation rather than trusting stale accounting.

Deferred durability is represented by monotonic generations, not a global dirty boolean. `LocalStore` records both its current mutation generation and the highest generation covered by an actual filesystem cut; `StoragePool` maps node-wide generations to the exact backend/store generation which accepted each provisional replica. One `syncfs()` may consequently group-commit many independent writers. A later barrier for an already-covered generation returns immediately even if newer unrelated writes are dirty, while the per-process durability epoch still prevents any pre-restart acknowledgement from being reused after a node restart.

The first 0.8.4 startup of an older backend, or a missing/corrupt accounting journal, falls back to the former background object-tree reconciliation once and writes a trusted checkpoint when the scan completes. Reads remain available while this migration/recovery scan runs and mutations wait for exact capacity accounting. If that reconciliation is interrupted, its partial byte count is discarded rather than made authoritative. Placement weight remains configured capacity and never depends on the transient scan result.

Backend state locks never cover filesystem I/O. `StoragePool` snapshots the backend state and takes a `shared_ptr<LocalStore>`, releases the backend mutex, then performs the disk operation. A backend can therefore be refreshed, removed or marked offline without a long `get`, directory walk or accounting-thread shutdown blocking health/control RPCs. An in-flight operation may finish against the old `LocalStore`; the shared pointer keeps it alive safely until that operation returns.

Scrub, local rebalance, reachability garbage collection and distributed push repair use independent persistent filesystem cursors. They advance a bounded number of physical objects per scheduler slice instead of rebuilding a complete object list for every small maintenance budget. Distributed pull repair advances the cached ordered live-object index directly rather than copying it into a complete vector for each slice. Rebalance/repair declare quiescence only after a complete pass finds no work. A completed GC pass pauses for `maintenance.no_progress_backoff_ms`; the default settled backoff is five minutes. Control-plane metadata/catalogue verification remains at most 30 seconds apart.

Proactive physical integrity scrub is not part of the ordinary idle loop. Every normal `LocalStore::get()` already authenticates AES-GCM and verifies the plaintext `ObjectId` SHA-256. Scrub adds cold-data coverage and therefore runs as a low-frequency campaign: `maintenance.scrub_interval_ms` defaults to 30 days, its next due time is stored in `state_path/maintenance/scrub.next`, and `scrub_fraction` controls the campaign rate once due. A new schedule file is anchored one interval into the future, so upgrading or restarting a healthy large node does not immediately launch a complete-store read/hash pass.

## Filesystem limits

The implemented filesystem operations cover ordinary media-library use: files and directories, create/open/read/write/truncate/unlink, mkdir/rmdir, rename, chmod/chown, timestamps, stat/statfs, directory enumeration, flush and fsync.

From 0.13.0 those operations enter an inode-based `FuseFrontend` first. Path resolution happens once and open handles retain stable frontend inode identity across rename. Namespace changes commit to the local overlay in kernel order and are then published FIFO; file writes are appended to a local operation spool and overlapping/adjacent dirty ranges are coalesced for scheduling. `flush` and writable-handle `release` request publication rather than performing quorum work, while read-only `release` never publishes another handle's dirty data and `fsync` durably flushes the local spool before queueing publication. Per-inode data publication preserves truncate/write order and waits for the inode's accepted namespace sequence, so rsync-style create/write/rename/close sequences cannot publish data through stale paths. The durable spool+journal remains authoritative until the required replica generations cross their storage barriers and metadata commits; completed/abandoned spools are then unlinked immediately without a directory durability barrier, because a crash can at worst resurrect an already-completed pathname which startup cleanup safely removes. Publication concurrency is reduced while mounted-filesystem traffic is active, keeping local spool acceptance ahead of asynchronous extent and metadata convergence under sustained bulk writes. FUSE namespace synchronisation is demand-driven rather than timer-driven and is strictly local: kernel metadata requests compare a process-local namespace revision and may adopt only a snapshot already decoded by `MetadataManager`; they never perform quorum I/O. The namespace revision advances only when filesystem entries change, so catalogue/GC/control metadata churn does not rebuild the FUSE graph. Kernel `getattr`/`readdir` also use compact attribute records and never copy file extent vectors merely to answer stat information.

Foreground FUSE reads never force a dirty writer to commit. They overlay pending local write/truncate operations on the immutable committed manifest and enforce a hard read deadline for any required remote extent. The same demand becomes an ordered high-priority hint in the existing hydration/cache scheduler; it is not a second cache hierarchy.

0.9.0 does **not** implement symlinks, hard links, extended attributes, distributed advisory locks, full sparse-file semantics, or stable POSIX inode identity across every rename case. Access time is not tracked. Concurrent appenders use file-version CAS rather than a globally serialized append stream.

A failed or interrupted upload may leave immutable extents whose data put completed but whose metadata commit did not. 0.10.0 collects these as ordinary unreachable objects: each node compares its physical authoritative objects with the combined committed filesystem+catalogue live set and removes an unreferenced object only after its local file has remained untouched for `maintenance.garbage_grace_ms` (24 hours by default). The sweep is bounded and yields to playback/mounted-filesystem work. Reaffirming an existing content hash refreshes that age, and the final age-check/remove is atomic with respect to `LocalStore::put()`.

Committed deletions still create retirement tombstones so recently dropped objects remain protected while metadata converges. Tombstones are no longer permanent: after their grace expires they are pruned from metadata, and a node which was offline long enough to miss one still discovers the dead object by reachability when it rejoins. Pre-0.10 tombstones are retained and stamped with a new retirement time on first 0.10.0 maintenance, giving an upgraded store a full grace period.

## On-disk layout

```text
<state_path>/
    .macha.lock
    node.id
    backend-identities/
    metadata/
        checkpoint.meta
        journal.log
        # after first 0.9 migration from 0.8.x:
        current.meta.v10
        committed.meta.v10
    tmp/

<storage backend>/
    .macha.backend
    .macha.accounting
    objects/ab/cd/<sha256>.obj

<cache path>/
    objects/ab/cd/<sha256>.obj
    metadata/current.meta
```

Object writes use unique temporary names, `fsync`, and atomic rename. Metadata journal records are individually encrypted/authenticated and fsynced before a CAS vote is acknowledged; commit markers are fsynced before the generation becomes a recovery witness. Idle maintenance periodically replaces the journal prefix with one durable full checkpoint. The state path is exclusively locked so two processes cannot use one node identity at once.

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

Extents are addressed by SHA-256. Sequential writes publish completed extents as they are filled; the whole file is not held in memory. Random writes use a temporary file under `state_path/tmp`; commit rebuilds the extent manifest from staged bytes, with unchanged content deduplicating by object ID.

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

Each authoritative backend keeps a small two-slot checksummed `.macha.accounting` journal. A normal restart restores the exact encrypted bytes-used count from that journal without walking `objects/`. Before each local put/remove, Macha durably records the one pending content-addressed mutation; after the object rename/removal it advances the clean checkpoint. A crash can therefore be reconciled by checking at most that one object path.

The first 0.8.4 startup of an older backend, or a missing/corrupt accounting journal, falls back to the former background object-tree reconciliation once and writes a trusted checkpoint when the scan completes. Reads remain available while this migration/recovery scan runs and mutations wait for exact capacity accounting. If that reconciliation is interrupted, its partial byte count is discarded rather than made authoritative. Placement weight remains configured capacity and never depends on the transient scan result.

Backend state locks never cover filesystem I/O. `StoragePool` snapshots the backend state and takes a `shared_ptr<LocalStore>`, releases the backend mutex, then performs the disk operation. A backend can therefore be refreshed, removed or marked offline without a long `get`, directory walk or accounting-thread shutdown blocking health/control RPCs. An in-flight operation may finish against the old `LocalStore`; the shared pointer keeps it alive safely until that operation returns.

Scrub, local rebalance and distributed push repair use persistent filesystem cursors. They advance a bounded number of physical objects per scheduler slice instead of rebuilding a complete object list for every small maintenance budget. Distributed pull repair advances the cached ordered live-object index directly rather than copying it into a complete vector for each slice. Rebalance/repair declare quiescence only after a complete pass finds no work. Scrub pauses for `maintenance.no_progress_backoff_ms` after completing an integrity pass before starting at the beginning again.

## Filesystem limits

The implemented filesystem operations cover ordinary media-library use: files and directories, create/open/read/write/truncate/unlink, mkdir/rmdir, rename, chmod/chown, timestamps, stat/statfs, directory enumeration, flush and fsync.

0.8.5 does **not** implement symlinks, hard links, extended attributes, distributed advisory locks, full sparse-file semantics, or stable POSIX inode identity across every rename case. Access time is not tracked. Concurrent appenders use file-version CAS rather than a globally serialized append stream.

A failed upload may leave unreachable immutable extents. Online garbage collection only removes objects known to have been dropped from committed metadata after a conservative grace period.

## On-disk layout

```text
<state_path>/
    .macha.lock
    node.id
    backend-identities/
    metadata/
        current.meta
        committed.meta
    tmp/

<storage backend>/
    .macha.backend
    .macha.accounting
    objects/ab/cd/<sha256>.obj

<cache path>/
    objects/ab/cd/<sha256>.obj
    metadata/current.meta
```

Object writes use unique temporary names, `fsync`, and atomic rename. The state path is exclusively locked so two processes cannot use one node identity at once.

# Operations

## Repair and maintenance

Media-object placement uses a virtual 32-bit shard space derived from the content-addressed object ID. No 2^32-entry map is materialised. Each node receives an exact shard-slot quota from its advertised usable capacity, subject to the configured replica count. The first replica choices therefore follow capacity rather than node count: a 10 TiB node receives roughly 1,280 times the ownership of an 8 GiB node when topology permits it. `failure_domain` remains a stronger placement constraint: replicas prefer distinct domains before additional nodes in the same domain.

Capacity is based on adopted configured backend limits, not live free space. Ordinary writes therefore do not move placement boundaries. Adding/removing storage changes the capacity topology and background repair/rebalance converges the affected shard ownership. Within a node, the single local authoritative copy uses capacity-weighted rendezvous over the stable shards, so adding a backend moves only shards won by that backend. Deterministic capacity-aware fallbacks are used when a preferred node or disk cannot currently store an object.

For `R` replicas, reported logical capacity is the largest `L` satisfying `sum(min(C_i, L)) >= R*L`, where `C_i` is node capacity (or failure-domain aggregate capacity when enough distinct domains exist). This correctly reports about 10.004 TiB for 10 TiB + 10 TiB + 8 GiB at `R=2`, but only 8 GiB for 10 TiB + 8 GiB at `R=2`.

Repair works both ways:

- existing owners push toward the current owner set;
- new or replacement nodes pull live objects they should own.

Background work is budgeted by bytes, remote operations and objects examined. The scheduler uses observed transfer rate, interactive activity and CPU load. Network repair, local disk rebalance and scrub have separate credits. A distributed repair slice examines at most 64 objects total across push and pull and issues at most 16 remote operations, even when every probe transfers zero bytes.

The FUSE frontend is intentionally fail-bounded. Its semantic operation deadlines and queue limits are configured under `fuse:`; exceeding them returns an error such as `ETIMEDOUT`/`EAGAIN` to the kernel rather than allowing a macFUSE/libfuse worker to block without limit. From 0.14.9, successful local namespace mutations and file writes also have recoverable operation history in `state_path/fuse-spool/operations.log`: write bytes are synced to their inode spool before the corresponding descriptor is synced to the journal, and namespace operations are journalled before their optimistic local view is exposed. A restart rebuilds that local overlay over the node's committed metadata snapshot and resumes asynchronous publication. This is local FUSE recovery, not evidence that the operation has already reached metadata quorum or its final replica set.

Treat FUSE recovery errors as data-safety faults. If startup reports a missing/short journal-referenced spool or an unreferenced non-empty `inode-*.spool`, Macha refuses to silently discard or reinterpret the bytes. Preserve `state_path/fuse-spool` for diagnosis/recovery. Before upgrading from a pre-0.14.9 build, allow FUSE publication to become idle so no legacy non-empty spool remains without operation-journal metadata. The journal resets when its durable pending-operation count returns to zero; a permanently blocked publication backlog can therefore make `operations.log` grow until convergence resumes.

The directory hidden underneath an active mount is made non-writable when `fuse.fail_closed_mountpoint` is enabled. An OS mount-table watchdog treats unexpected disappearance of the FUSE mount as a service-failure condition, leaves the naked directory protected, and requests node shutdown. Clean unmount restores the original directory mode.

Macha has three explicit I/O priorities: viewer playback/probe/seek is `foreground`; mounted-filesystem traffic and useful read-ahead are `read_ahead`; maintenance/hydration speculation is `speculative`. DATA scheduling preserves that order on both ends of the connection. Either foreground class pauses background maintenance with the default `busy_bandwidth_fraction: 0.0`, but mount traffic never outranks the viewer. FUSE namespace operations such as `getattr` and `readdir` count as read-ahead/interactive activity even when they move no extent payload. Serving a remote foreground/read-ahead object request marks the same activity on that node, so maintenance yields where the disk/CPU work actually occurs.

A settled complete pass that moves no bytes is treated as **quiescent**, not as permission to rescan immediately. Its credit is cleared and that maintenance class backs off for `no_progress_backoff_ms` (five minutes by default). Local rebalance, reachability GC and distributed push repair do not build whole-store vectors: each keeps a persistent physical-object cursor and examines at most a bounded number of objects per scheduler slice. Distributed pull repair advances the immutable ordered live-object index directly rather than copying it into a new vector on every slice. An incomplete slice is not mistaken for quiescence. A completed GC pass also pauses before starting again.

Physical integrity scrub is deliberately different from repair. Objects are already AES-GCM authenticated and content-hash verified whenever they are read; proactive scrub exists to discover latent corruption in cold data. It runs as a scheduled campaign (`scrub_interval_ms`, 30 days by default), not as permanent idle work. The next campaign due-time is persisted across daemon restarts. When a campaign is due it advances the same bounded physical-object cursor and yields to foreground activity; after a complete pass it becomes dormant until the next due time.

The filesystem live/garbage object index is immutable, stored as compact vectors and cached by known metadata generation. Service maintenance unions filesystem liveness with the current catalogue root/artwork set. Distributed repair uses that same live index. Garbage accounting splits committed retirements into still-live stale tombstones and genuinely non-live candidates; stale and matured tombstones are removed from metadata, while pre-0.10 tombstones are first stamped into the new grace lifecycle.

Physical GC walks each authoritative backend with its own cursor. A candidate must be absent from the combined live set, not covered by an unmatured retirement, and older than `garbage_grace_ms`. The local age check and removal are atomic against object writes, and each slice examines at most 64 objects before returning to the scheduler. This sweep is what removes upload/crash orphans that never had a tombstone. Metadata repair, catalogue verification and garbage accounting use the control-plane cap of 30 seconds even though data-plane no-progress backoff defaults to five minutes. Destructive maintenance consequently does not advance while metadata quorum repair is unavailable, and all background passes defer while foreground I/O is active.

## Performance diagnostics

`DEBUG` is intended for low-volume operational diagnosis. In 0.9.0 the broad per-thread CPU, lock, maintenance-stage, RPC queue/handler and aggregated storage timing telemetry remains `ALL`-only. `DEBUG` retains slow FUSE operations, compact slow metadata-mutation timings, slow foreground media reads, and slow write-stage timings so viewer and mount latency can be diagnosed without flooding the log. Long-lived worker threads remain named (`macha-maint`, `macha-rpc-health`, `macha-rpc-ctl`, `macha-rpc-data`, `macha-hydrator`, and related names) for `top -H`, `perf` or macOS `sample`.

`ALL` enables the old hot-path trace stream, including individual backend/object transfers, complete FUSE request/result records, read-payload hashes and detailed extent/write diagnostics. Disabled trace-level checks are lock-free. `ALL` is intentionally expensive and should not be used for throughput measurements.

Useful DEBUG records include:

```text
DIAG slow-fuse op=write path=... elapsed_ms=...
metadata mutate total_ms=... base_ms=... decode_ms=... encode_ms=... cas_ms=... commit_ms=... payload_bytes=...
playback object read source=owned|cache|remote stripe=... id=... bytes=... elapsed_ms=...
playback source read media=... purpose=probe|playback|subtitle offset=... wanted=... got=... elapsed_ms=...
object write quorum id=... bytes=... local_ms=... remote_max_ms=... total_ms=...
write stage id=... path=... stage=write lock_wait_ms=... extent_put_ms=... total_ms=...
write stage id=... path=... stage=append-tail-fetch offset=... bytes=...
write stage id=... path=... stage=rebuild extents=... reused_extents=... put_extents=... staging_read_ms=... object_put_ms=... total_ms=...
write stage id=... path=... stage=commit lock_wait_ms=... data_ms=... metadata_ms=... total_ms=...
```

## Cache and hydration

The persistent cache is deliberately not part of DHT ownership.

Object-cache eviction is maintained incrementally in memory. The on-disk cache is enumerated once when it is opened/reconfigured to reconstruct the block set; normal insertions do not rescan the complete cache tree. Foreground cache reads do not hold the cache mutation lock while encrypted object I/O is performed. LRU order is process-local and intentionally disposable: restarting may change which old block is evicted first, but never changes cluster durability or object integrity.

- It survives restart.
- It does not count toward replica quorum.
- It is bounded by block count and evicted approximately LRU.
- It can live on SSD while authoritative storage lives on HDD.
- Cached blocks can later be promoted if placement makes this node an owner.
- With `prefer_metadata: true`, the latest valid namespace snapshot is also kept outside the media-block limit.

The cache hydrator consumes ordered hints from independent engines. The built-in engines are:

- `read_ahead`: the immediate sequential window after the current read position;
- `current_file`: the rest of the file currently being read;
- `catalogue`: the next TV episode, crossing a season boundary when required, or the next movie in the same collection.

Hints that refer to the same ordered run are merged and their priorities reinforce each other. Scheduling uses weighted virtual time across runs, so the current file normally advances fastest without starving a predicted next item. A speculative run is sequential: the hydrator will not fetch a later extent while an earlier missing extent in that run is unavailable. Hydration keeps up to `max_inflight` extent requests active and rebuilds the hint set continuously. Read-ahead/current-file transfers use the read-ahead transport class; catalogue prediction uses speculative transport. Either yields immediately to foreground frames at the transport scheduler.

Hydration requires the persistent cache to be enabled. If the cache is disabled, foreground reads continue normally but speculative hints do not trigger network fetches. The catalogue-sequence hint provider also returns before taking a catalogue snapshot when no playback is active, so an idle hydrator does not continuously repair/copy catalogue state.

`dht.read_ahead` remains the size of the immediate read-ahead hint window. Engine enablement and priority are configured separately under `hydration`:

```yaml
hydration:
  enabled: true
  interval_ms: 100
  active_timeout_ms: 30000
  catalogue_lookahead: 1
  engines:
    read_ahead:   { enabled: true, priority: 1000 }
    current_file: { enabled: true, priority: 700 }
    catalogue:    { enabled: true, priority: 300 }
```

Catalogue media bindings may use the stable `macha:<sha256>` media identity derived from file size plus the ordered extent manifest. That identity survives a namespace rename. Raw paths and `path:/...` remain accepted as a practical fallback for manually-created catalogue records.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The integration/unit suite covers transport/crypto, bidirectional RPC and connection deduplication, metadata quorum and replacement recovery, legacy SM7/DLT1 storage replay, tombstone mutation, bounded reachability/orphan GC, catalogue join synchronisation/search/artwork GC, filename probing/provider resolution/scanner reconciliation, multi-node placement, disk loss/return, cache persistence, automatic new-owner pull, playback-assisted promotion, corruption repair and restart.

## Service files

- `systemd/macha.service`
- `systemd/macha.conf.example`
- `macos/macha.plist.example`

The systemd unit supports `systemctl reload macha`, which sends `SIGHUP`.

## Security

Anyone with the cluster key is a trusted cluster member. Keep it secret and back it up separately. There is no online key rotation or per-node revocation in 0.7.0. See `SECURITY.md`.


See [`SECURITY.md`](../SECURITY.md) for the threat model and key handling.

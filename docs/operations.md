# Operations

## Repair and maintenance

Media-object placement uses a virtual 32-bit shard space derived from the content-addressed object ID. No 2^32-entry map is materialised. Each node receives an exact shard-slot quota from its advertised usable capacity, subject to the configured replica count. The first replica choices therefore follow capacity rather than node count: a 10 TiB node receives roughly 1,280 times the ownership of an 8 GiB node when topology permits it. `failure_domain` remains a stronger placement constraint: replicas prefer distinct domains before additional nodes in the same domain.

Capacity is based on adopted configured backend limits, not live free space. Ordinary writes therefore do not move placement boundaries. Adding/removing storage changes the capacity topology and background repair/rebalance converges the affected shard ownership. Within a node, the single local authoritative copy uses capacity-weighted rendezvous over the stable shards, so adding a backend moves only shards won by that backend. Deterministic capacity-aware fallbacks are used when a preferred node or disk cannot currently store an object.

For `R` replicas, reported logical capacity is the largest `L` satisfying `sum(min(C_i, L)) >= R*L`, where `C_i` is node capacity (or failure-domain aggregate capacity when enough distinct domains exist). This correctly reports about 10.004 TiB for 10 TiB + 10 TiB + 8 GiB at `R=2`, but only 8 GiB for 10 TiB + 8 GiB at `R=2`.

Repair works both ways:

- existing owners push toward the current owner set;
- new or replacement nodes pull live objects they should own.

Background work is budgeted in bytes, not a fixed number of extents. The scheduler uses observed transfer rate, foreground activity and CPU load. Network repair, local disk rebalance and scrub have separate credits. With the default `busy_bandwidth_fraction: 0.0`, foreground I/O pauses background WAN repair.

A settled complete pass that moves no bytes is treated as **quiescent**, not as permission to rescan immediately. Its credit is cleared and that maintenance class backs off for `no_progress_backoff_ms` (30 seconds by default). Local rebalance, scrub and distributed push repair do not build whole-store vectors: each keeps a persistent physical-object cursor and examines at most a bounded number of objects per scheduler slice. Distributed pull repair advances the immutable ordered live-object index directly rather than copying it into a new vector on every slice. An incomplete slice is not mistaken for quiescence. Scrub also pauses after reaching the end of a complete integrity pass before it starts again.

The filesystem live/garbage object index is immutable, stored as a compact sorted vector and cached by known metadata generation. Maintenance may ask for that index when network repair has spendable budget or garbage accounting is due, but an unchanged namespace reuses the cached index instead of walking every file and extent again. Metadata repair, catalogue verification and garbage accounting use the larger of five seconds and `no_progress_backoff_ms`; the default is therefore 30 seconds for settled background verification. These background verification passes defer while foreground I/O is active. A metadata generation change invalidates the live-object cache on demand.

## Performance diagnostics

`DEBUG` is intended for low-volume diagnosis. 0.8.3 reports slow maintenance stages, thread CPU consumption, RPC queue/handler latency, selected contended metadata/catalogue/RPC/storage locks, slow FUSE operations and aggregated local-storage GET latency. Long-lived worker threads are named (`macha-maint`, `macha-rpc-health`, `macha-rpc-ctl`, `macha-rpc-data`, `macha-hydrator`, and related names) so `top -H`, `perf` or macOS `sample` output can be correlated with Macha logs.

`ALL` enables the old hot-path trace stream, including individual backend/object transfers, complete FUSE request/result records, read-payload hashes and detailed extent/write diagnostics. Disabled trace-level checks are lock-free. `ALL` is intentionally expensive and should not be used for throughput measurements.

Useful 0.8.3 DEBUG records include:

```text
DIAG thread name=macha-maint wall_ms=5000 cpu_ms=... cpu_pct=... iterations=...
DIAG node-stage stage=storage-refresh elapsed_ms=...
DIAG maintenance-stage stage=network-repair elapsed_ms=... bytes=... push_examined=... pull_examined=... remote_ops=... complete=... yielded=...
DIAG maintenance-inventory generation=... entries=... extents=... live=... garbage=... elapsed_ms=...
DIAG rpc-server peer=... frame=control message=members queue_wait_ms=... handler_ms=...
DIAG lock-wait lock=metadata.mutation wait_ms=...
DIAG slow-fuse op=write path=... elapsed_ms=...
DIAG storage-get window_ms=5000 gets=... bytes=... avg_ms=... max_ms=...
```

## Cache and hydration

The persistent cache is deliberately not part of DHT ownership.

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

The integration suite covers transport/crypto, bidirectional RPC and connection deduplication, metadata quorum and replacement recovery, catalogue join synchronisation/search/artwork GC, filename probing/provider resolution/scanner reconciliation, multi-node placement, disk loss/return, cache persistence, automatic new-owner pull, playback-assisted promotion, corruption repair and restart.

## Service files

- `systemd/macha.service`
- `systemd/macha.conf.example`
- `macos/macha.plist.example`

The systemd unit supports `systemctl reload macha`, which sends `SIGHUP`.

## Security

Anyone with the cluster key is a trusted cluster member. Keep it secret and back it up separately. There is no online key rotation or per-node revocation in 0.7.0. See `SECURITY.md`.


See [`SECURITY.md`](../SECURITY.md) for the threat model and key handling.

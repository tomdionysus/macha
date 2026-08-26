# Current release

## 0.18.1 — acquisition recovery

- Add an explicit torrent retry action for completed downloads whose linked ingest job failed, reusing the existing persisted ingest job and staging payload without redownloading.
- Keep an operator-paused torrent paused until explicit resume instead of allowing a stale libtorrent status sample to overwrite the Macha job state.

## 0.18.0 — storage correctness reset

0.18.0 defines a fresh on-disk/storage contract and intentionally does not migrate an existing Macha namespace.

- Separate authoritative DATA from priority CONTROL/METADATA storage.
- Rework DATA placement so configured capacity participates in stable deterministic placement and full/offline preferred owners use deterministic fallbacks.
- Define `min_write_replicas` as the synchronous DATA publication floor and `replicas` as the convergence target repaired in the background.
- Add node-local encrypted small-object packing with restart index reconstruction, torn-tail recovery, tombstones and copy-on-write compaction.
- Store catalogue structure as content-addressed shards plus a manifest in CONTROL storage; publish references only after metadata-voter quorum durability.
- Store artwork as ordinary DATA, using the same placement, fallback, replication, repair and GC rules as media extents.
- Add catalogue control-object convergence across current metadata voters and defer scanner work during infrastructure/quorum outages without consuming semantic failure attempts.
- Preserve physical filesystem headroom with per-DATA-backend `reserve_free` admission.
- Retain the metadata memory-amplification corrections: shared immutable record payloads, streaming record hashes, compact/in-place deltas and reduced transient RPC copies.
- Add configurable stale Macha FUSE mount recovery before service startup with `fuse.unmount_if_mounted`.
- Bump the incompatible cluster transport/storage contract to protocol 18.
- Replace migration-era documentation with the current storage, durability and recovery invariants.

Older development history remains in Git history rather than this operational document.

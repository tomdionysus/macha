# Current release

## 0.18.2 — cluster status telemetry

- Add optional UPnP IGD public port mapping, AWS external-IP fallback, boot/manual public endpoint self-probing, runtime advertised endpoint updates, and Status API diagnostics.

- Add live node telemetry on the authenticated peer protocol, gossiped across the cluster with boot-incarnation sequencing, freshness expiry and rolling-upgrade compatibility.
- Persist a bounded coalesced last-known telemetry cache independently on each node; routine status persistence never mutates the namespace, enters metadata quorum/CAS, or replays historical telemetry. Legacy SM9/DLT3 status records remain readable for compatibility.
- Add durable SM10/DLT4 cluster-wide endpoint→NodeId reset tombstones under `/api/v1/manage`, including endpoint-, NodeId-, and IP-only scopes, live membership/telemetry/RPC eviction, stale-gossip suppression, propagation, confirmation metadata, and audit logging.
- Improve wrong-node RPC diagnostics with endpoint, expected NodeId, and the actually authenticated NodeId.
- Add `/api/v1/status` cluster/node status surfaces with authoritative live membership, explicit metadata availability (`unavailable`, `read-only`, `writable`), known-versus-online storage/cache totals, optional runtime/load/peer/RPC observations, and explicit connectivity re-check actions. Metadata availability is logged only on state transitions.
- Preserve maintenance dependency ordering while publishing metadata availability: when metadata repair cannot establish its required state, that maintenance pass stops before catalogue/GC/repair work rather than continuing against a failed metadata validation.
- Make authenticated metadata-journal replay reproduce the same deterministic same-generation sibling-seed arbitration accepted by the live replica, preventing valid replacement-node convergence state from failing restart with `seed generation conflict`.
- Fence catalogue CONTROL-store GC to the metadata generation used to build its reachability inventory, so a concurrent catalogue commit cannot have a newly-published manifest/shard deleted by a stale maintenance live-set (including with zero garbage grace).
- Protect the data-before-metadata catalogue publication window on every metadata voter: CONTROL GC will not reclaim objects written after that voter observed its current catalogue root, so newly staged manifests/shards survive until the successor root is committed and observed even when `garbage_grace` is zero.

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

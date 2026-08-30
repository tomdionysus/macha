# Current release

## 0.21.0 — asynchronous node startup (development)

- Bring the RPC control plane and status API online before local backend recovery.
- Report startup/readiness independently from cluster reachability and storage availability.
- Make metadata-history cold-start linear and add periodic full-history anchors.
- Build UPnP support against miniupnpc API 18 headers which predate the symbolic
  `UPNP_GetValidIGD()` return-value macros, while preserving private-WAN and
  disconnected-gateway classification.
- Install Linux systemd units in systemd's unit search path rather than a
  Debian multiarch library directory, including migration of the old cached
  default and warning-free generated install scripts.
- Make endpoint identity reset an independent recovery primitive: apply,
  persist, and propagate it before attempting the cluster-metadata audit, so a
  stale association cannot prevent its own reset.

## 0.19.0 — metadata replica availability

- Replace fixed metadata-voter placement with all-node metadata replication. Every active node is eligible to durably store namespace and catalogue-control metadata.
- Add `dht.metadata_min_write_replicas`: the literal number of distinct active metadata replicas which must durably store an immutable commit before it may be accepted. Legacy `dht.metadata_replicas` remains a migration-only alias and maps its old voter count to the equivalent former majority write floor.
- Replace the live metadata CAS/PREPARE/COMMIT state machine with immutable commit storage plus durable acceptance certificates. A replica stores a valid commit without comparing it to its current head; acceptance records the distinct durable store witnesses and survives their later absence.
- Persist a maximal accepted-head set on each replica. Independent partition branches coexist instead of competing for a single current slot; accepted ancestors fall out of the head set only when a descendant/merge commit supersedes them.
- Add protocol-20 `put_metadata_commit`, `accept_metadata_commit`, and accepted-head discovery RPCs; reject the legacy network CAS/PREPARE/COMMIT RPC family so live code cannot accidentally re-enter the old consensus model.
- Add encrypted compact metadata ancestry/history, exchange it between arbitrary replicas, collapse stale ancestor heads, and reconcile divergent accepted heads through deterministic two-parent merge commits. N simultaneous heads converge by repeated deterministic DAG merges rather than winner selection.
- Merge non-conflicting namespace changes automatically and preserve incompatible namespace/catalogue-root alternatives as durable first-class conflict records instead of silently choosing a winner.
- Invalidate metadata caches on an independent peer-observation epoch so same-generation sibling heads are discovered promptly; numeric generation alone is no longer a complete freshness signal.
- Let any active replica initiate virgin-cluster genesis. All virgin founders construct the same deterministic generation-2 root and publish it through the ordinary commit-store/acceptance path; there is no genesis coordinator, voter or PREPARE arbitration. Bootstrap-configured joiners remain fenced until their peer survey completes.
- Publish catalogue CONTROL objects against the same any-node metadata write floor and converge them opportunistically to active replicas.
- Fence catalogue CONTROL GC by catalogue-root epoch as well as object age: data-before-metadata staging survives the root epoch in which it is first observed/re-affirmed and cannot be reclaimed by a stale maintenance live-set during root publication.
- Replace metadata voter/quorum status with replica/write-floor fields while retaining deprecated 0.18 JSON aliases for client compatibility.
- Keep non-destructive DATA repair running from an accepted local branch while metadata reconciliation/validation is pending.
- Separate write availability from convergence validation: a reachable `metadata_min_write_replicas` cohort remains writable while reconciliation is pending, with validation reported independently as stability telemetry.
- Add durable causal DATA/CONTROL retention claims on physical copies. Metadata publication re-affirms referenced immutable objects before acceptance; local/remote deletion, placement eviction and GC refuse to remove a claimed copy. Missing claimed copies are repaired in bounded background slices even when the claiming branch is absent from the node's current namespace view.
- Keep GC useful during partitions: unclaimed staging/extra copies remain reclaimable, while inherited claims are released only after every currently known metadata replica is online and the accepted-head set is validated coherent. This prevents one partition from erasing an ancestor object still required by another accepted branch.
- Keep the 0.18 on-disk storage layout readable; bump the peer protocol to 20 because the metadata publication contract is intentionally incompatible with protocol 18/19.
- Fix protocol-20 compact metadata deltas so ordinary mutations retain SM12/SM13 write-floor and retention-governance state, persist completed retention baselines even with an empty migration roster, and preserve mutation-sequence counters across restart.
- Keep legacy accepted-head certificates readable during upgrade while preventing a protocol-20 branch from downgrading to legacy authority, and route background maintenance through the same virgin-cluster policy fencing as foreground discovery.
- Split dependency-free backend tests from concrete yaml-cpp/FFmpeg runtime-adapter tests so storage, cluster, filesystem, catalogue and playback policy coverage can build and run on constrained development hosts; add `run-tests.sh` to execute every test binary present in a build tree.

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

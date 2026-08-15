# Architecture

Macha keeps the filesystem, metadata, inter-node placement and local storage layers separate. The point is not abstraction for its own sake: disk loss, node loss, network failure and cache eviction have different semantics and should fail independently.

```text
FuseAdapter / FileSystem
          |
          +---- MetadataManager ---- voter quorum + committed checkpoints
          |
          +---- DistributedStore --- inter-node placement and repair
                     |
                     +---- StoragePool -------- authoritative local replica
                     |       |  |  |
                     |      HDD HDD ...
                     |
                     +---- PersistentBlockCache - non-DHT cache
                     |
                     +---- RpcClient/RpcServer
                             one framed priority connection per peer

NodeRuntime
   +---- state_path: node.id, metadata, backend identities, temp writes
   +---- Membership
   +---- fetched-block persistence worker
   +---- membership/storage refresh worker

Service
   +---- CatalogueManager / optional catalogue JSON API
   +---- repair / local rebalance / scrub / GC scheduler
```

## StoragePool: one node, many disks

A node has one stable `NodeId` regardless of local disk count. Adding a disk changes the node's advertised capacity and therefore its proportional inter-node ownership without changing identity.

`StoragePool` maps object IDs onto a virtual 32-bit stable shard space and uses capacity-weighted rendezvous hashing over those shards for its single authoritative node-local copy. No shard table is materialised. Adding a backend only steals the shards it wins; existing backends do not exchange unrelated shards. Unavailable/full preferred disks fall through to deterministic alternatives. Rebalance copies before deleting.

Every adopted backend has a `.macha.backend` marker. `state_path/backend-identities` records the marker expected at each configured path. A disappeared marker means offline/unmounted storage; it is not silently recreated. A different marker at the same path is rejected. This avoids writing DHT data into a bare mountpoint after a disk failed to mount.

Membership advertises current used bytes and stable placement capacity. Placement capacity is the aggregate configured limit of adopted backends, including a temporarily offline known disk. This prevents an unmount/remount from causing a cluster-wide ownership oscillation. Removing a backend from configuration removes its capacity; a never-adopted missing path contributes nothing. Missing local objects can be restored from other replicas.

## State is not storage

`state_path` contains the durable control plane:

- process lock;
- `node.id`;
- metadata working state and committed checkpoint;
- expected backend identities;
- temporary random-write staging files.

Put it on reliable local system storage. Losing a bulk backend is disk failure. Losing `state_path` creates a replacement node with a new identity.

`state_path` is mandatory and is never inferred from a storage backend.

## PersistentBlockCache

The cache uses the same encrypted content-addressed object representation but is not authoritative. Cached objects are excluded from membership capacity, `have_object`, placement and replica quorum.

Reads resolve in this order:

1. `StoragePool`;
2. `PersistentBlockCache`;
3. remote DHT owner/fallback.

A remote foreground fetch is returned immediately and copied into a bounded persistence queue. The worker later caches it and, if placement says this node is an owner, promotes the same bytes into `StoragePool`. Queue overflow drops the caching opportunity rather than delaying playback.

Metadata cache has its own encrypted `metadata/current.meta` slot and does not consume a media-block entry.

## Cache hydration

Speculative caching is separated into hint generation and fetch scheduling. `HydrationHintProvider` implementations describe ordered runs of object IDs and assign priorities; they do not perform network I/O. `CacheHydrator` merges overlapping hints, schedules the runs, and asks `DistributedStore` to place selected objects in `PersistentBlockCache`.

The built-in providers are deliberately independent:

- read-ahead: a bounded window immediately after the sequential playback position;
- current-file: every remaining non-hole extent in the current file;
- catalogue sequence: the next episode in a season, the first episode of the next season, or the next movie in the same collection.

Read handles no longer own asynchronous prefetch futures. A sequential read reports its most recently consumed extent to `PlaybackTracker`; providers derive fresh hints from that observation. Closing the handle removes the observation, and inactive observations expire. This keeps prediction outside filesystem read semantics.

Hints for one file are ordered. The scheduler skips already-local/cache objects but never jumps over an earlier missing object in order to fetch a later extent from the same run. Overlapping hints reinforce priority. Weighted virtual-time scheduling interleaves different runs, so a high-priority current file advances more quickly while a lower-priority next episode or film still receives service before the current file is necessarily complete.

The hydrator keeps at most `hydration.max_inflight` speculative fetches active (default four) and rebuilds the hint set before each dispatch. In-flight objects count as an already-dispatched prefix, so an ordered run can advance without jumping over an undispatched or failed earlier extent. The window is deliberately small: when playback changes, stale runs stop producing new work quickly without requiring a cancellation protocol. Speculative fetches are non-foreground and go only into the persistent cache; they do not alter authoritative DHT placement. With no configured persistent cache, hydration is inactive and foreground reads are unchanged.

## Replica-aware retrieval

`DistributedStore` owns one replica selector shared by foreground filesystem reads and speculative hydration. Hint engines never select peers. For each remote object, deterministic DHT owners remain the preferred source group and later HRW-ranked nodes remain fallback sources.

The selector tracks per-peer in-flight foreground/speculative work, recent transfer latency and consecutive failures. Equivalent unknown replicas are deterministically striped by extent index. Foreground reads optimise likely completion latency; speculative work strongly prefers peers not carrying foreground traffic and spreads independent extents over otherwise suitable replicas.

Remote requests for the same `ObjectId` are coalesced inside `DistributedStore`. If foreground playback asks for an object already being hydrated, the existing transfer is promoted from speculative to foreground accounting and both callers share the result. The same object is not raced against several replicas by default; a failed source is retried against the next suitable peer. Hedged duplicate reads remain a possible later optimisation.

Catalogue matching uses the stable `macha:<sha256>` media identity derived from file size and the ordered immutable extent manifest. Namespace renames therefore do not change the identity. Path bindings remain supported for manual/legacy catalogue records. Movie sequence prediction can use a shared `collection` or `tmdb_collection` external ID, or an existing common parent relationship, without changing the catalogue binary format.

## Metadata state

Metadata is a whole versioned encrypted CAS snapshot. This is intentionally simple: media data is large and immutable; namespace mutation is comparatively rare.

Each node holds two durable metadata records under `state_path`:

- `metadata/current.meta`: local voter/working state, which may contain a CAS generation not yet known committed;
- `metadata/committed.meta`: the latest generation known to have reached quorum.

Both files are required for existing state. A `current.meta`-only state from an older build is rejected rather than upgraded implicitly.

Only the configured voter set participates in consensus. After a committed generation is known, maintenance distributes that checkpoint to every active node as a **recovery witness**. Witnesses are not extra votes.

Replica counts are mutable cluster policy. A coordinated whole-cluster restart may change data replication, metadata voter count, or both. The existing voter majority serialises the policy change; a resized voter group is seeded before normal metadata mutation continues, and object repair subsequently converges the immutable extent set to the new data replica count. Transport v7 does not advertise a node's desired replica policy, so rolling changes with mixed configurations are deliberately unsupported. `extent_size` is intentionally immutable for an existing namespace.

Read-only metadata also has a short in-memory TTL cache with generation invalidation. Mutations bypass it and start from a fresh quorum read.

## Catalogue metadata

The catalogue is a complete cluster-wide metadata dataset. Its current snapshot is an immutable content-addressed `DistributedStore` object; `MetadataSnapshot::catalogue_root` is the quorum-committed pointer to that object. Catalogue mutations therefore acquire the same serialization and durability boundary as filesystem metadata without storing catalogue bytes inside the metadata record itself.

Every referenced artwork object is also content-addressed. The current catalogue root and all live artwork are marked **universal** during maintenance: every active node must hold them, regardless of normal `dht.replicas`. A joining node pulls the root and all artwork before its catalogue status becomes ready. Catalogue synchronisation is attempted before ordinary media repair. Search/list operate from the complete local snapshot.

Superseded catalogue roots and artwork that loses its final live reference are appended to the existing committed garbage tombstones. Each node removes its authoritative/cache copy after `maintenance.garbage_grace_ms`; a rejoining node observes the same tombstone and converges deletion. Combined filesystem+catalogue reachability is checked before deletion, so an object still referenced anywhere remains live.

This first implementation intentionally shares the filesystem metadata generation for the small `catalogue_root` pointer. If catalogue mutation volume later makes that a contention point, the root can move to an independent quorum namespace without changing catalogue object or artwork representation.

### Catalogue scanner

The scanner is filesystem-driven. It first classifies a path as movie, TV episode or music track, then asks a provider capable of that class to resolve metadata. Provider results never decide the local media class. Built-in providers are TMDB for movies/TV and MusicBrainz for music; Cover Art Archive supplies album art. The provider boundary is virtual and deliberately independent of scanner scheduling.

A successful provider match contributes catalogue hierarchy plus remote artwork descriptors. Artwork bytes are fetched before commit, staged as immutable objects and included in the same catalogue durability boundary. No provider URL is required for playback clients after the scan.

The scanner uses stable `macha:<sha256>` file identities. An already-bound identity is skipped on later scans, avoiding repeated provider traffic and artwork downloads. Reconciliation removes vanished bindings only from scanner-owned leaves and prunes scanner-created empty parents; manual catalogue records remain untouched. If any configured root cannot be traversed, the pass fails before reconciliation so temporary namespace/storage loss cannot masquerade as deletion.

The lowest active `NodeId` is the scanner coordinator. Scanner enablement is therefore intended to be consistent across cluster nodes; membership change moves the role without a permanent catalogue master. Catalogue CAS still protects against overlapping scans during membership transitions.

## Voter failure and replacement

Normal voter replacement requires a majority of the old voter set and therefore preserves quorum intersection.

If the old voter quorum has been permanently destroyed, replacement recovery uses committed checkpoints instead. Recovery is allowed only when:

- every node still considered active participates in the checkpoint survey;
- surviving highest-generation committed checkpoints agree;
- the old voter quorum is no longer available;
- enough fresh, genesis-state nodes exist to fill the missing voter seats;
- the checkpoint extent size matches local configuration.

The new voter set is installed as the next metadata generation and committed by a quorum of that new set before it becomes a recovery checkpoint.

This deliberately does not let an isolated old cohort promote itself around a network partition. Stale active membership delays recovery until `dead_after` expires.

After namespace recovery, ordinary object maintenance walks the recovered live-object set and repopulates replacement nodes.

## Offline behaviour

Namespace mutation always requires quorum.

If quorum is unavailable, a node may use its last valid post-genesis snapshot for read-only namespace access. It can read files only where the required extents exist in authoritative storage or cache. It cannot invent missing blocks or mutate the namespace.

## Transport v7

Each peer uses one persistent, authenticated, bidirectional TCP connection. It is multiplexed with 64-bit request IDs, a bounded outbound queue and pending-request maps. The TCP dialler owns odd request IDs and the acceptor owns even request IDs, so either end can originate work without request/reply ambiguity. Requests may complete out of order. Cancelling one request does not tear down unrelated work.

The authenticated v7 handshake negotiates `max_frame_size`; the lower configured ceiling wins. Logical messages are split into variable-length frames no larger than that ceiling. Storage extents remain storage objects and are not transport framing units. Each frame is independently AES-256-GCM protected.

Frame type defines priority completely: control, foreground, read-ahead, then speculative. No independent wire priority exists. The outbound scheduler selects the most urgent runnable transfer for every frame and returns to scheduling immediately afterwards. This makes lower-priority object transfers pre-emptible at frame boundaries rather than committing an entire extent to TCP before foreground demand can run. A promotion control frame can raise an existing transfer; subsequent frames cannot be demoted. Cancellation stops queued remainder frames without disturbing other request IDs.

A connection is canonical by authenticated `NodeId`, not hostname or socket direction. Simultaneous cross-dial keeps the connection dialled by the lower `NodeId`. Same-direction duplicates use the authenticated client nonce as tie-break. A losing connection exchanges retirement notices, stops admitting new RPCs atomically with retirement, and closes only after admitted work in both directions has drained.

RPCs have no wall-clock completion deadline. Control/data stall intervals are DEBUG observability thresholds only. Health probes are ordinary highest-priority control RPCs on the same transport that carries real work; sustained inability to establish liveness within `dead_after` marks the peer dead.

Server execution has dedicated control workers plus data workers. Data workers choose foreground before read-ahead before speculative queued work. This preserves the same absolute ordering after a request has reached the peer; speculative work may make progress only when more urgent runnable work is absent.

Every connection uses an HMAC-authenticated ephemeral X25519 handshake, HKDF-SHA256 directional keys and AES-256-GCM variable-length frames.

## Concurrency

Long-lived execution contexts are:

- RPC accept/session workers plus control and priority-data request pools;
- membership/gossip and backend refresh;
- fetched-block persistence;
- bounded cache-hydration fetch workers;
- one optional catalogue-scanner worker on the elected node;
- service maintenance;
- FUSE/client request threads.

Foreground filesystem requests do not perform local rebalance or scrub synchronously.

## Inter-node placement

Data objects use a fixed virtual 32-bit placement-shard space. The first 32 bits of the uniformly distributed object ID select one of 2^32 shards; no per-shard table is materialised. For a replica count `R`, capacity water-filling computes each node's inclusion probability subject to the rule that a node can hold at most one replica of an object; those probabilities are quantised into exact shard-slot quotas. This avoids the small-node over-selection produced by naive weighted sampling without replacement.

When there are at least `R` configured failure domains, the same calculation is performed over aggregate domain capacities first, guaranteeing distinct-domain replicas. A selected domain then chooses a node within it by capacity. If there are fewer domains than replicas, one node from each domain is preferred before remaining slots are filled from unused nodes. Failure domains are operator-supplied topology, not discovered security boundaries.

Current free bytes never participate in the weight. Configured adopted capacity changes only when storage topology changes. Full or temporarily unavailable preferred owners use deterministic capacity-aware fallback nodes until repair can restore the preferred shard ownership.

Local disk placement uses capacity-weighted rendezvous over the same stable shard space, using stable backend tokens and configured backend limits. Because the local replica count is one, adding a backend has the usual rendezvous monotonicity: only shards won by the new backend move. A known temporarily offline disk remains in that placement map; a backend removed from configuration does not.

## Data writes

Foreground object writes launch requests to preferred owners concurrently. Success returns once a majority of the effective target has acknowledged durable storage. The effective target is bounded by the number of active nodes.

Full configured replication is convergence, not foreground latency. Repair fills missing replicas later and uses deterministic fallback nodes when preferred owners cannot store the object.

## Joining and convergence

Push-only repair cannot populate an empty node. There are therefore two convergence paths:

1. existing owners push live objects toward the current owner set;
2. every node walks the committed live-object set and pulls objects it should own.

A replacement node first recovers namespace state. Catalogue maintenance then pulls the complete catalogue root and artwork set before ordinary media repair; media extents subsequently use the normal owner pull path. Foreground playback can accelerate media convergence because a fetched block may be promoted directly into authoritative storage.

## Adaptive maintenance

Maintenance is budgeted in bytes rather than extents. Each interval derives usable background rate from observed throughput or the configured initial estimate, then scales it by busy/idle fractions and process CPU load.

Separate credits exist for:

- inter-node repair;
- local backend rebalance;
- integrity scrub.

Foreground I/O marks the store busy. During `foreground_quiet_ms`, local rebalance and scrub pause. With the default busy network fraction of zero, inter-node repair pauses as well. Credits are burst-capped.

## Garbage collection

Committed filesystem and catalogue changes record dropped object IDs as tombstones. A node removes a tombstoned authoritative/cache object only after `maintenance.garbage_grace_ms` (24 hours by default). Catalogue artwork is collected only after its final live reference disappears. A live reference wins.

Uncommitted upload orphans are not guessed safe. They require a future explicit orphan collector.

## Scaling boundary

Metadata is still a whole replicated snapshot. That is the current scaling boundary. The immutable extent layer, storage pool and cache do not depend on that representation, so metadata can be replaced later without redesigning media storage.

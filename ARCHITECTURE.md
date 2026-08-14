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
                             health / control / data lanes

NodeRuntime
   +---- state_path: node.id, metadata, backend identities, temp writes
   +---- Membership
   +---- fetched-block persistence worker
   +---- membership/storage refresh worker

Service
   +---- repair / local rebalance / scrub / GC scheduler
```

## StoragePool: one node, many disks

A node has one stable `NodeId` regardless of local disk count. Adding a disk changes local placement, not inter-node DHT placement.

`StoragePool` ranks online `LocalStore` backends by rendezvous hashing over `(ObjectId, backend-token)` and keeps one authoritative node-local copy on the highest-ranked usable backend. Rebalance copies before deleting.

Every adopted backend has a `.macha.backend` marker. `state_path/backend-identities` records the marker expected at each configured path. A disappeared marker means offline/unmounted storage; it is not silently recreated. A different marker at the same path is rejected. This avoids writing DHT data into a bare mountpoint after a disk failed to mount.

Membership advertises aggregate used/capacity across online backends. Loss of one backend removes capacity, not node identity. Missing local objects can be restored from other replicas.

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

## Metadata state

Metadata is a whole versioned encrypted CAS snapshot. This is intentionally simple: media data is large and immutable; namespace mutation is comparatively rare.

Each node holds two durable metadata records under `state_path`:

- `metadata/current.meta`: local voter/working state, which may contain a CAS generation not yet known committed;
- `metadata/committed.meta`: the latest generation known to have reached quorum.

Both files are required for existing state. A `current.meta`-only state from an older build is rejected rather than upgraded implicitly.

Only the configured voter set participates in consensus. After a committed generation is known, maintenance distributes that checkpoint to every active node as a **recovery witness**. Witnesses are not extra votes.

Replica counts are mutable cluster policy. A coordinated whole-cluster restart may change data replication, metadata voter count, or both. The existing voter majority serialises the policy change; a resized voter group is seeded before normal metadata mutation continues, and object repair subsequently converges the immutable extent set to the new data replica count. Transport v4 does not advertise a node's desired replica policy, so rolling changes with mixed configurations are deliberately unsupported. `extent_size` is intentionally immutable for an existing namespace.

Read-only metadata also has a short in-memory TTL cache with generation invalidation. Mutations bypass it and start from a fresh quorum read.

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

## Transport v4

Each peer uses three independent authenticated transport lanes:

- health;
- control;
- bulk data.

Each lane is persistent, bidirectional and multiplexed with 64-bit request IDs, a bounded outbound queue and a pending-request map. The TCP dialler owns odd request IDs and the acceptor owns even request IDs, so either end can originate work without request/reply ambiguity. Requests may complete out of order. Cancelling one request does not tear down unrelated work.

The lane is declared inside the authenticated v4 handshake, so both ends can register and arbitrate the session before the first RPC. A connection is canonical by authenticated `(NodeId, lane)`, not by hostname or socket direction. Simultaneous cross-dial keeps the connection dialled by the lower `NodeId`. Same-direction duplicates use the authenticated client nonce as the tie-break. A losing connection exchanges retirement notices, stops admitting new RPCs atomically with the retirement marker, and closes only after requests already admitted in both directions have drained.

RPCs have no wall-clock completion deadline. Control/data stall intervals are DEBUG observability thresholds only. The health lane probes liveness independently; only sustained inability to establish liveness within `dead_after` marks the peer dead and tears down outstanding lanes.

Server health, control and data requests use separate worker queues, so bulk object work cannot starve liveness or metadata traffic.

Every connection uses an HMAC-authenticated ephemeral X25519 handshake, HKDF-SHA256 directional keys and AES-256-GCM frames.

## Concurrency

Long-lived execution contexts are:

- RPC accept/session workers and health/control/data request pools;
- membership/gossip and backend refresh;
- fetched-block persistence;
- service maintenance;
- FUSE/client request threads.

Foreground filesystem requests do not perform local rebalance or scrub synchronously.

## Inter-node placement

Objects use rendezvous hashing by `(ObjectId, NodeId)`. Placement first prefers distinct configured failure domains, then fills remaining replica positions in ordinary HRW order.

Failure domains are operator-supplied topology, not discovered security boundaries.

Local disk placement is a second independent rendezvous layer below the node.

## Data writes

Foreground object writes launch requests to preferred owners concurrently. Success returns once a majority of the effective target has acknowledged durable storage. The effective target is bounded by the number of active nodes.

Full configured replication is convergence, not foreground latency. Repair fills missing replicas later and uses deterministic fallback nodes when preferred owners cannot store the object.

## Joining and convergence

Push-only repair cannot populate an empty node. There are therefore two convergence paths:

1. existing owners push live objects toward the current owner set;
2. every node walks the committed live-object set and pulls objects it should own.

A replacement node first recovers namespace state, then uses the same pull path. Foreground playback can accelerate convergence because a fetched block may be promoted directly into authoritative storage.

## Adaptive maintenance

Maintenance is budgeted in bytes rather than extents. Each interval derives usable background rate from observed throughput or the configured initial estimate, then scales it by busy/idle fractions and process CPU load.

Separate credits exist for:

- inter-node repair;
- local backend rebalance;
- integrity scrub.

Foreground I/O marks the store busy. During `foreground_quiet_ms`, local rebalance and scrub pause. With the default busy network fraction of zero, inter-node repair pauses as well. Credits are burst-capped.

## Garbage collection

Committed namespace changes record dropped object IDs as tombstones. A node removes a tombstoned authoritative/cache object only after the local 24-hour grace period. A live reference wins.

Uncommitted upload orphans are not guessed safe. They require a future explicit orphan collector.

## Scaling boundary

Metadata is still a whole replicated snapshot. That is the current scaling boundary. The immutable extent layer, storage pool and cache do not depend on that representation, so metadata can be replaced later without redesigning media storage.

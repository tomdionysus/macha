# Cluster and recovery

## Membership

Each node has a persistent random node ID, advertised endpoint, configured failure domain and DATA capacity. Bootstrap endpoints are discovery seeds, not masters. Once connected, peers exchange membership and maintain separate CONTROL and DATA transport lanes.

The cluster protocol version is 21. Mixed-version operation is rejected at the
handshake rather than negotiated down, so every node in a cluster runs the same
version and a protocol change is a rolling upgrade of the whole cluster.

Each node's gossiped record carries two self-declared bits, `inbound_capable`
and `hosts_extents` (see `network.inbound_capable` and
`storage.hosts_extents`). A peer that accepts no inbound connections is never
dialled: it is reached over the sessions it opened, and asked over the CONTROL
session to open a lane it has not. Extents are placed only on nodes that host
them.

## Metadata replicas and write floor

Every node is a metadata replica. There is no configured voter subset, witness role, leader or permanent metadata authority.

`dht.metadata_min_write_replicas` is the publication durability floor. A namespace commit becomes accepted after the exact immutable commit has been durably stored on that many distinct active nodes and the resulting acceptance certificate has been durably retained. For example, with three known nodes and `metadata_min_write_replicas: 2`, any surviving pair remains writable.

Commit storage is intentionally independent of a receiving node's current head. A replica can retain several accepted maximal heads when partitions have produced independent histories. Acceptance records historical durability and does not vanish when one of the original store witnesses later goes offline.

The floor is intentionally independent of cluster size. This availability model permits disconnected cohorts to advance different valid histories. When cohorts reconnect, accepted heads and compact ancestry are exchanged. Ancestor heads collapse; divergent heads are reconciled through immutable multi-parent commits. Non-conflicting namespace changes merge automatically and incompatible alternatives are preserved as first-class conflicts rather than silently discarded.

Catalogue control objects use the same write floor before namespace metadata may reference them, then converge opportunistically to active nodes.

## DATA membership and placement

Active nodes advertise currently eligible aggregate DATA capacity. Deterministic capacity-aware placement derives preferred replica owners and fallback candidates for each `ObjectId`.

Cluster size does not implicitly redefine `min_write_replicas`. If policy allows a degraded floor below `replicas`, publication can proceed at the floor and repair converges later.

When a node or backend cannot admit a preferred object, writers can use the next deterministic candidate. This is required for heterogeneous capacities.

## Failure domains

`network.failure_domain` identifies nodes that share a physical/site failure boundary. Placement prefers distinct domains when enough are available. Capacity calculations and replica placement use that topology instead of pretending two stores in the same failure domain are equivalent to two independent sites.

## Recovery

Recovery is layered rather than global:

1. **state identity** — reject non-empty state that does not carry the fresh storage-layout marker;
2. **mount preflight** — before services start, verify the configured mountpoint; optionally remove only a stale Macha/FUSE mount;
3. **DATA backends** — validate backend identity, rebuild accounting after unclean shutdown, reconstruct pack indexes;
4. **namespace metadata** — recover the local checkpoint/history and accepted-head certificates, then discover/reconcile additional accepted heads from peers;
5. **FUSE journal** — reconstruct accepted local mutations and resume publication;
6. **catalogue control** — fetch missing manifest/shards from metadata peers and converge them;
7. **DATA repair** — restore desired placement/replica count from an accepted live metadata view;
8. **cache** — rebuild/discard opportunistically.

No external catalogue provider is required for correctness recovery.

## Joining and rejoining nodes

A joining node starts with its own empty DATA/control stores and learns current membership, accepted metadata heads and ancestry. It may store an accepted head regardless of which local head it previously exposed. DATA objects are pulled only according to placement/repair policy. Catalogue control objects are converged to active metadata replicas. Artwork remains ordinary DATA and is not pulled merely because a node stores metadata.

A returning node's immutable objects and accepted metadata branches are reusable after validation. Reconciliation determines metadata ancestry; DATA placement/repair decides which objects remain useful. Reachability GC eventually removes objects no longer referenced once branch-aware GC safety permits it.

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

A node that has not been heard of for the absence horizon (`maintenance.garbage_grace_ms`, and never sooner than twice `network.dead_after_ms`) is dropped from the known-node roster, on each node independently. Gossip that old does not teach it back; its own connection does. An identity reset (see [Management](management.md)) removes a node sooner.

## Metadata replicas and write copies

Every node is a metadata replica, and every replica has the same standing: any active node can store commits, accept writes and serve metadata.

A namespace commit is accepted once the committing node durably holds it and its acceptance certificate. `dht.metadata_write_copies` is the number of copies sought before the write returns: with that many nodes present and answering, the commit is on that many when it returns; with fewer, it returns on the copies it has and repair delivers the rest. Nothing is refused for lack of peers, so a single node, or the survivor of any number of losses, keeps reading and writing. Nodes configured with different values share one namespace.

Commit storage is intentionally independent of a receiving node's current head. A replica can retain several accepted maximal heads when partitions have produced independent histories. The certificate records which nodes held the commit when it was accepted and does not vanish when one of them later goes offline.

This availability model permits separated nodes to advance different valid histories. When they reconnect, accepted heads and compact ancestry are exchanged. Ancestor heads collapse; divergent heads are reconciled through immutable multi-parent commits. Non-conflicting namespace changes merge automatically and incompatible alternatives are preserved as first-class conflicts rather than silently discarded. A head that cannot be merged with the node's own (no common ancestor is known, or its content cannot be fetched from any node present) is set aside until the membership changes: reads serve the node's own head and writes extend it.

Catalogue control objects and namespace tree nodes are valid on the committing node in the same way, then converge to the other active nodes.

## DATA membership and placement

Active nodes advertise currently eligible aggregate DATA capacity. Deterministic capacity-aware placement derives preferred replica owners and fallback candidates for each `ObjectId`.

`dht.write_copies` is the number of DATA copies sought before a write returns. A write returns on the copies that landed once no further node can take one promptly, and repair converges the object to `replicas` later.

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

A returning node's immutable objects and accepted metadata branches are reusable after validation. Reconciliation determines metadata ancestry; DATA placement/repair decides which objects remain useful. The returning node removes an object no longer referenced only once it has itself seen it unreferenced for `maintenance.garbage_grace_ms`, so it deletes nothing on the strength of what happened while it was away.

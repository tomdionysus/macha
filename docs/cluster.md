# Cluster and recovery

## Membership

Each node has a persistent random node ID, advertised endpoint, configured failure domain and DATA capacity. Bootstrap endpoints are discovery seeds, not masters. Once connected, peers exchange membership and maintain separate CONTROL and DATA transport lanes.

Cluster protocol 18 is deliberately incompatible with earlier storage semantics. Mixed-version operation is rejected.

## Metadata voters

The configured `dht.metadata_replicas` determines the metadata-voter group. Namespace reads and CAS mutations require majority evidence from that group.

A metadata voter is not required to hold every DATA object. Metadata/control authority and bulk DATA ownership are separate responsibilities.

Catalogue control objects must reach a voter majority before the namespace can reference them and are subsequently converged to all current voters.

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
4. **namespace metadata** — recover committed checkpoint/journal state and form voter quorum;
5. **FUSE journal** — reconstruct accepted local mutations and resume publication;
6. **catalogue control** — fetch missing manifest/shards from metadata peers and converge them;
7. **DATA repair** — restore desired placement/replica count from the committed live set;
8. **cache** — rebuild/discard opportunistically.

No external catalogue provider is required for correctness recovery.

## Joining and rejoining nodes

A joining node starts with its own empty DATA/control stores and learns current membership/metadata. DATA objects are pulled only according to placement/repair policy. Catalogue control objects are converged if the node is a metadata voter. Artwork remains ordinary DATA and is not pulled merely because a node votes on metadata.

A returning node's immutable objects are reusable after validation. Placement/repair decides which remain useful; reachability GC eventually removes objects no longer referenced or placed there.

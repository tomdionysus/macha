# Storage

## Storage classes

Macha has three storage classes with different correctness rules.

### DATA

DATA contains immutable payload objects: media extents, catalogue artwork, subtitles and similar blobs. A DATA object is addressed by SHA-256 of its plaintext. DATA is distributed by capacity-aware deterministic placement and governed by `dht.replicas` / `dht.write_copies`.

### CONTROL / metadata

Namespace metadata and content-addressed catalogue control objects are control-plane authority. On a namespace re-rooted onto the Merkle tree, the tree's branch, leaf and extent-spine nodes are control objects too, and the metadata record carries only the root (see [Metadata](metadata.md)). Every node stores metadata/control authority. Namespace mutations and catalogue manifests/shards are valid once the committing node durably holds them; `dht.metadata_write_copies` is the number of copies sought before the write returns, and every active node is equally eligible to take one.

CONTROL storage does not consume DATA quota.

### Cache

Cache is opportunistic and non-authoritative. A cache copy can accelerate a read but never satisfies DATA replication or metadata durability.

## DATA backends

A node may configure several authoritative DATA backends:

```yaml
storage:
  data:
    backends:
      - path: /mnt/disk-a/macha-data
        limit: 8T
        reserve_free: 4G
      - path: /mnt/disk-b/macha-data
        limit: 16T
        reserve_free: 4G
    packing:
      threshold: 1M
      target_size: 64M
```

`limit` is the maximum physical authoritative DATA admitted to that backend. `reserve_free` is a second gate against the actual filesystem: a DATA write is refused if it would cross the configured physical free-space reserve even when `limit` has room.

Backend paths are not treated as interchangeable directories. Each carries a node/backend identity marker. A non-empty backend without that marker is refused rather than adopted.

## Placement across nodes

Every active node advertises eligible DATA capacity. Macha derives a stable capacity-aware order for each `ObjectId`:

```text
ObjectId
   |
   v
preferred owner 1
preferred owner 2
...
   |
   +--> deterministic fallback 1
   +--> deterministic fallback 2
```

Configured capacity, not momentary free space, determines the stable placement weight. This prevents object ownership from churning continuously as disks fill.

Only nodes that host extents (`storage.hosts_extents`) are candidates. The writing node tries its own store first when it is one, then this order; repair later converges copies onto the preferred owners.

If a preferred node is full, offline or cannot complete the write inside the placement stall policy (`dht.write_stall_ms`), the writer tries the next deterministic candidate. A small node therefore does not cap an R=1 cluster.

For example:

```text
node A DATA limit:   1 GiB
node B DATA limit:   2 TiB
replicas:            1
write_copies:        1
```

The logical DATA capacity is approximately the aggregate eligible capacity, subject to reserves/overhead. Once A cannot admit an object, placement can fall through to B. No rule requires every object to fit on A.

## Write copies and convergence target

Two settings intentionally mean different things:

```yaml
dht:
  replicas: 3
  write_copies: 1
```

These are the defaults. `write_copies` is the number of durable authoritative DATA copies sought before a write returns. `replicas` is the desired converged replica count.

Thus a write with `replicas: 3` and `write_copies: 1` returns after one durable copy. That object is under-replicated, not falsely considered converged. Prompt replication sends its second copy at once and maintenance repair creates the remaining preferred replicas when eligible nodes/capacity are present.

With `write_copies: 2`, a write is on two nodes when it returns, provided two nodes are present and answering. With fewer, or once no further node can take a copy promptly, it returns on the copies that landed and repair delivers the rest; a peer that stalls past `dht.write_stall_ms` is not waited for once one copy has landed. A write is never refused for lack of peers.

Metadata may reference a new DATA object once a node present holds it, and its retention claim is placed on the holders present. A commit that brings in new bytes no node present holds is refused; the writer still has them and puts them again.

## Local backend selection

Within a node, `StoragePool` ranks configured backends by the same stable capacity-aware principle. A full/offline preferred backend falls through to another local backend. The node advertises the aggregate eligible configured DATA capacity, not the smallest backend.

## Physical representation

Logical object identity is independent of local representation.

### Loose objects

Objects above `storage.data.packing.threshold` are stored as individual encrypted object files.

### Packed objects

Objects at or below the threshold are appended to encrypted pack containers. The default is 1 MiB threshold and approximately 64 MiB target pack size.

Packing is purely local:

```text
ObjectId A --+
ObjectId B --+--> pack file
ObjectId C --+

ObjectId D ------> loose object file
```

No distributed metadata records pack coordinates. Reads still ask `LocalStore` for an `ObjectId`; `LocalStore` chooses the physical representation.

Each packed PUT record is independently authenticated and contains enough identity/size information to rebuild the index. A logical removal appends a tombstone. Touch/age state used by GC is likewise represented without changing the logical object.

On restart, the pack index is reconstructed by scanning durable records. A torn final record is truncated to the last valid record boundary. A corrupt committed record is an integrity error rather than being guessed around.

Compaction is copy-on-write:

1. calculate live records and required temporary physical space;
2. preserve `reserve_free`;
3. write replacement packs;
4. make replacements durable;
5. atomically install replacements and switch the live index;
6. delete obsolete packs.

If a crash leaves old packs behind after the replacement is installed, restart sees the later live records and the old bytes are reclaimable duplicates.

## CONTROL object storage

Control objects are configured separately:

```yaml
storage:
  metadata:
    path: /var/lib/macha/metadata-objects
    limit: 4G
    packing:
      threshold: 1M
      target_size: 64M
```

If `path` is omitted, it resolves below `state_path`. The store is content-addressed and may use the same physical packing implementation, but its quota is independent from bulk DATA.

The control-store `limit` is a safety ceiling, not a DATA budget. Operators should size and monitor the underlying filesystem so state/control growth has real headroom. DATA `reserve_free` is especially important when DATA and state paths reside on the same physical filesystem.

## Catalogue storage

Catalogue structure consists of 64 content-addressed shards plus a small manifest root. Those objects are CONTROL. Artwork bytes are DATA.

A catalogue commit cannot reference a new manifest/shard until that control object is durable on the committing node; copies on `metadata_write_copies` nodes are sought from those present. After commit, maintenance converges current control objects onto every active node. Missing extra copies are convergence debt, not grounds for copying artwork everywhere.

## Reads

A DATA read checks authoritative local storage, then cache, then remote candidates. Fetching an object from a remote authoritative owner does not require the reading node to become an owner. If the local DATA store is full, the node can still serve/read remote artwork or media.

The cache may retain a useful fetched copy independently, but that cache copy does not count toward the authoritative replica target.

## Garbage collection

Committed metadata is reachability authority. MachaDFS file extents and catalogue artwork contribute to the DATA live set. Catalogue manifests/shards, and every namespace tree node reachable from a tree-backed root, contribute to a separate CONTROL live set, both in the maintenance inventory the collector sweeps against and in the release horizon claims are released against. A tree node that cannot be read leaves the inventory unbuilt and the release horizon incomplete, so nothing is collected or released against a partial set.

Collection reads this node's own state only: it waits for no other node. What protects another node's references is time. An object's bytes go only once this node has itself seen the object unreferenced and unclaimed for `maintenance.garbage_grace_ms` (the absence horizon, 30 days by default), measured on its own clock from its own first sighting. The sightings are kept in `<state_path>/retention/unreferenced-data.bin` and `unreferenced-control.bin`, so a restart does not restart the wait, and a node back from a long absence removes nothing before its own grace has run. This also protects failed publications, convergence lag and recently retired references. DATA and CONTROL are swept separately.

The cost is stated: if an object is deleted on one side of a separation and still used on the other, and the separation outlasts the horizon, the merged file may have extents nobody holds. The availability survey reports such a file as incomplete (see [Files and availability](files.md)).

Packed logical deletion does not rewrite neighboring live objects immediately; dead bytes are reclaimed later by pack compaction.

## Fresh-storage boundary

The storage implementation is intentionally not a live migration layer. A fresh state namespace is marked `macha-state-layout-v18`; DATA backends are marked `macha-backend-v18`. Non-empty unversioned state or DATA is refused.

This prevents an older physical representation from being silently accepted under new durability/placement semantics.

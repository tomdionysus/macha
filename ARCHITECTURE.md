# Macha architecture

## Design boundary

MachaDFS (Macha Distributed File System) is a distributed media filesystem, not a general-purpose distributed POSIX filesystem. The authoritative model is immutable content-addressed objects plus quorum-managed namespace/control metadata. Local FUSE state makes accepted filesystem mutations crash-recoverable while distributed publication proceeds asynchronously.

The storage contract deliberately separates three classes:

```text
                     namespace / catalogue authority
                                |
                      CONTROL / METADATA
                      voter-quorum durability
                                |
                dedicated priority object storage

file manifests ---------------------------------------------+
        |                                                    |
        v                                                    |
immutable DATA objects: extents, artwork, subtitles, blobs   |
        |                                                    |
        v                                                    |
capacity-aware deterministic DHT placement                   |
        |                                                    |
        +--> authoritative DATA backend(s) per node           |
        +--> deterministic fallback when preferred is full    |
        +--> repair toward desired replicas                   |
                                                             |
optional CACHE <---------------------------------------------+
(non-authoritative; never counts for durability)
```

## Object identity

A logical immutable object is identified by SHA-256 of its plaintext bytes. Encryption and physical representation are node-local details; they never change the logical `ObjectId`.

Files are represented by namespace metadata and ordered extent references. Holes remain metadata rather than allocated zero objects. Completed extents can be published incrementally; an entire media file need not be held in memory.

## DATA placement

Each node advertises the configured capacity of its currently eligible authoritative DATA backends. Placement uses stable capacity-aware hashing; live free space is not a placement weight, avoiding continual reshuffling as disks fill.

For an object, placement produces preferred owners followed by deterministic fallback candidates. A preferred owner that is offline, stalled or unable to admit the object does not cap the cluster: the writer tries the next candidate.

`dht.min_write_replicas` is the number of durable DATA placements required before foreground publication. `dht.replicas` is the desired converged replica count. When the floor is lower than the target, repair records/converges the missing copies later. Neither cache copies nor failed provisional writes count.

Each node may have several local DATA backends. The same stable capacity-aware principle chooses a preferred local backend followed by fallbacks. Backend `limit` controls DATA admission; `reserve_free` additionally preserves real filesystem headroom.

## CONTROL / metadata

Namespace metadata is an encrypted versioned CAS history managed by a configured metadata-voter set. Reads and mutations require majority evidence. Ordinary mutations use deterministic deltas while full snapshots remain recovery/checkpoint material.

Content-addressed control objects are stored separately from DATA. The principal current user is the media catalogue. DATA quota exhaustion must not prevent a metadata voter from storing control objects required to represent committed metadata.

Control object storage has its own safety ceiling. Metadata/control capacity is an operational resource and must be monitored, but it is not borrowed by bulk media DATA.

## Catalogue

The catalogue is not one ever-growing immutable object. It consists of 64 deterministic content-addressed shards plus a small manifest root referenced by namespace metadata.

A catalogue mutation:

1. constructs only the affected shard contents and a successor manifest;
2. verifies newly referenced artwork DATA exists through the ordinary distributed store;
3. durably stores changed control objects on a majority of the metadata voters;
4. CAS-updates namespace metadata to the new manifest root;
5. leaves missing non-majority voter copies as control convergence debt.

Maintenance subsequently converges the current manifest and referenced shards onto every current metadata voter. Obsolete control objects are reclaimed by a dedicated control-store reachability sweep after the configured grace period.

Artwork is ordinary DATA. The catalogue stores only its content `ObjectId`, MIME type and role. Artwork obeys DATA placement/replication/fallback/repair/GC; it is not universally copied to metadata voters.

## Physical small-object packing

Packing is entirely below `LocalStore`:

```text
logical ObjectId
   |
   +-- size > threshold  -> encrypted loose object
   |
   `-- size <= threshold -> encrypted append-only pack record
```

Pack records are independently authenticated and identify their logical object. The in-memory pack index is derived state rebuilt by scanning packs on startup. An incomplete final record is truncated to the last valid boundary. Logical deletion appends a tombstone. Compaction writes live records into replacement packs, makes replacements durable, switches the live index, then removes obsolete packs.

No DHT, catalogue or metadata structure contains pack filenames, offsets or lengths. Packs can therefore be compacted or reconstructed without cluster-visible identity changes.

## Durability domains

Authoritative DATA backends on the same physical filesystem share a durability domain. Deferred writes receive generation/backend-incarnation tickets. Publication waits for the exact required placements to cross a physical durability barrier before committing references into namespace metadata. Linux uses `syncfs()` for the physical domain cut; supported fallback platforms use conservative fsync behavior.

Strict object writes use the same durability machinery with immediate scheduling. Deletion is reachability-safe and may be lazily made physically durable because a lost unlink can only preserve garbage, not remove a referenced object.

See `docs/durability.md` for the crash matrix.

## FUSE admission and publication

FUSE is a bounded local frontend. Accepted write bytes are first durable in per-inode spool state and an ordered operation journal. Namespace operations are journalled before their optimistic local result is exposed. Publication workers turn that durable local intent into immutable extents and metadata changes.

A crash therefore does not require guessing whether acknowledged local writes existed: the spool/journal is replayed. Publication is idempotent because DATA is content-addressed and metadata mutation uses CAS semantics.

Before any service/cluster startup, the daemon inspects the configured mountpoint. With `fuse.unmount_if_mounted: true`, a stale mount identified specifically as Macha is unmounted and disappearance is verified. An unrelated filesystem at that path is never automatically unmounted.

## Recovery and fresh genesis

The storage layout is intentionally a fresh-genesis contract. State carries `macha-state-layout-v18`; DATA backends carry `macha-backend-v18` identity. A non-empty unversioned state/backend is refused rather than silently adopted.

Recovery boundaries are independent:

- namespace metadata replays/recovers its committed checkpoint/journal;
- FUSE replays durable local operations not yet observed committed;
- DATA accounting is reconstructed if its derived checkpoint is dirty/missing;
- pack indexes are reconstructed from durable records;
- catalogue control objects can be fetched from another metadata voter;
- repair detects/converges missing DATA replicas;
- cache may simply be discarded/rebuilt.

External metadata providers are enrichment inputs, not recovery dependencies.

## Maintenance

Maintenance is low priority and bounded. It performs replica repair, local backend rebalance, reachability GC, catalogue control convergence/GC and scheduled integrity scrub. Foreground playback and mounted MachaDFS traffic suppress speculative work.

GC authority is reachability from committed metadata. DATA and CONTROL have separate physical sweeps. Newly orphaned/unreferenced objects remain protected by the configured grace period so failed publication and metadata convergence cannot race reclamation.

## Network model

Peers use separate CONTROL and DATA transport lanes. Health/membership and metadata/control RPCs are isolated from bulk DATA scheduling. Cluster protocol 18 is intentionally incompatible with earlier storage semantics; mixed versions are rejected.

## Correctness gates

The storage implementation is expected to preserve these invariants under test:

- R=1 capacity aggregates eligible heterogeneous nodes rather than collapsing to the smallest node;
- a full preferred owner falls through deterministically;
- publication never occurs below `min_write_replicas`;
- repair converges toward `replicas` after nodes/capacity return;
- DATA exhaustion cannot block control metadata commits;
- artwork follows DATA placement and remains remotely readable from a full node;
- pack restart/torn-tail/compaction preserve logical objects;
- old/unversioned non-empty storage is refused;
- metadata mutation memory remains bounded rather than multiplying whole namespace buffers.

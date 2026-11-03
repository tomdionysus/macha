# Macha architecture

## Governing laws

Four laws govern every Macha project, and five self-healing disciplines go
with them. Every project numbers them the same way; the server's statement of
them is [Principles and laws](docs/principles-and-laws.md). Read that first. The source
and reviews cite them by number: "law 2", "discipline 5".

1. **Thou Shalt Not Make Control Wait.**
2. **Thou Shalt Not Make The Viewer Wait.** And no viewer may be allowed to
   make another viewer wait.
3. **Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
   Viewer Wait.**
4. **Thou Shalt Not Shoot Thyself In The Foot.**

This section is how they apply in the node.

**Resolution order.** Law 1's reservation is set aside first, law 2 takes
priority within what remains, and law 3 governs everything left. Law 4 takes
no part in that contention; it is a veto over all three.

**Where each law lives in the node.** The laws are why the node has class
memory reserves rather than one budget (`runtime.control_memory_reserve_bytes`,
`viewer_memory_reserve_bytes`, `loader_memory_reserve_bytes`, beside
`reassembly_memory_reserve_bytes`, the headroom inbound RPC frame reassembly
needs so a peer's confirmation can always be received), why DATA
execution priority is viewer foreground, viewer read-ahead, user loader, then
speculative maintenance, why the HTTP server runs separate control and data
lanes, and why the fast-control RPC executor has a closed allow-list. Each of
those is one law expressed in a different resource. A change to any of them is
a change to a law and should be argued as one.

**Law 3's share.** While a viewer and loader publication are both runnable,
FUSE service is weighted `fuse.viewer_weight: 95` to `fuse.loader_weight: 5`
by default: the loader keeps a non-zero share and is never stopped, and either
class takes everything while the other is idle.

**Law 2's second clause, in the node.** A resource that is rationed against
background work but unrationed *per account* is a law-2 violation waiting for
a rogue or merely enthusiastic client, and the node cannot tell those apart.
Every per-viewer resource has a per-account bound as well as a node-wide one,
and a session cheap enough to be exempt from one limit is not thereby exempt
from being counted against the others.

**Law 2's two sanctioned waits.** Law 2 is not absolute in the sense that a
viewer never blocks on anything; it is absolute in the sense that no *other
class of work* may be the reason a viewer blocks. Where a viewer genuinely
waits - a request beyond the produced frontier, a held segment - the wait is on
production that viewer itself demanded, it is bounded by configuration, and the
bound is published to the client. Those two cases are documented as such in
[Streaming](docs/streaming.md); they are the exceptions, and they are not
precedent for a third.

**Why law 4 exists.** Every kind of non-recoverable state it names can
occur on the hardware Macha runs on: a replica that cannot rejoin because a
cache is smaller than its own unit of work, a process that cannot be stopped
without `SIGKILL` because a loop never checks its stop token, and a board that
browns out under sustained load with no remote power control. Any one of those
is law 4; together they are why it is a law rather than a preference.

### The disciplines in the node

1. **Re-derive, don't assert.** A durability, placement or confirmation check
   that fails against recorded evidence probes the content-addressed truth.
   Content addressing makes ground truth one `has(id)` away.
2. **One work-item policy.** The parked state is visible in Status, and no RPC
   waits without a deadline.
3. **Recover by resolving.** The outcome is re-journalled and counted in
   Status. Recovery may refuse to start only on key mismatch or header
   corruption. A configuration the node cannot work with -- unknown keys,
   unversioned storage, an `inbound_capable` setting that contradicts the
   network -- is refused at startup as a separate matter: it is the
   operator's to correct, not state to recover. This is also an operational
   rule: a node is expected to settle bad input and stay
   online, and repairing state by hand on a node is not the remedy for a
   recovery path that refuses.
4. **Compact history out of the hot path.** A metadata snapshot's size is a
   function of the live namespace.
5. **A bound smaller than one unit of its own work is not a bound.** A cache
   or budget that cannot hold one unit of the work it serves does not slow that
   work down, it stops it: the work runs with no usable capacity and makes
   almost no progress while consuming a core. A bound is checked against its
   unit of work, and a bound that cannot hold one is detected and reported,
   not left for an operator to find.

## Design boundary

MachaDFS (Macha Distributed File System) is a distributed media filesystem, not a general-purpose distributed POSIX filesystem. The authoritative model is immutable content-addressed objects plus replicated, branch-reconciling namespace/control metadata. Local FUSE state makes accepted filesystem mutations crash-recoverable while distributed publication proceeds asynchronously.

The storage contract deliberately separates three classes:

```text
                     namespace / catalogue authority
                                |
                      CONTROL / METADATA
                    minimum-write durability
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

Each node advertises the configured capacity of its currently eligible authoritative DATA backends. A node with `storage.hosts_extents` false (an edge node) is never a placement candidate; it publishes its writes to the owners. Placement uses stable capacity-aware hashing; live free space is not a placement weight, avoiding continual reshuffling as disks fill.

For an object, placement produces preferred owners followed by deterministic fallback candidates. A preferred owner that is offline, stalled or unable to admit the object does not cap the cluster: the writer tries the next candidate.

`dht.write_copies` is the number of DATA copies sought before a write returns. A write returns on the copies that landed once no further node can take one promptly; at least one durable copy always exists before a commit names the object. `dht.replicas` is the desired converged replica count, and repair converges the missing copies later. Neither cache copies nor failed provisional writes count.

Each node may have several local DATA backends. The same stable capacity-aware principle chooses a preferred local backend followed by fallbacks. Backend `limit` controls DATA admission; `reserve_free` additionally preserves real filesystem headroom.

## CONTROL / metadata

Namespace metadata is an encrypted immutable DAG replicated by every node. A commit is accepted on the committing node alone, once that node durably holds the commit and its acceptance certificate, without a vote or a distributed prepare/commit round; it is then offered to every node present, and repair delivers it to nodes that were away. `dht.metadata_write_copies` is the number of copies a merge commit seeks before it is accepted. A receiving replica may store a commit regardless of its current head. Accepted heads are a set rather than a singleton, so separated nodes may create independently valid branches. Replicas exchange accepted-head certificates, reconcile divergent heads by a deterministic merge computed from the two heads alone (see [Metadata](docs/metadata.md)), and materialise incompatible namespace/catalogue alternatives as durable conflicts rather than overwriting a branch.

A commit's record either inlines the serialised namespace or, on a node re-rooted with `macha-namespace-migrate`, carries the root of a content-addressed Merkle tree over the namespace, keyed by path. An ordinary commit then rewrites the leaf holding the changed path and the branches above it; the tree's new nodes are durably held by the committing node before a commit may name its root, and are sent to the other nodes after it. A reconciliation merge works on what differs between the two trees, or materialises both heads when one is inline, and re-roots the result.

Content-addressed control objects are stored separately from DATA. Their principal users are the namespace tree and the media catalogue. DATA quota exhaustion must not prevent a metadata replica from storing control objects required to represent committed metadata.

Control object storage has its own safety ceiling. Metadata/control capacity is an operational resource and must be monitored, but it is not borrowed by bulk media DATA.

## Catalogue

The catalogue is not one ever-growing immutable object. It consists of 64 deterministic content-addressed shards plus a small manifest root referenced by namespace metadata.

A catalogue mutation:

1. constructs only the affected shard contents and a successor manifest;
2. verifies newly referenced artwork DATA exists through the ordinary distributed store;
3. durably stores changed control objects on the committing node;
4. commits an immutable namespace commit referencing the new manifest root on that node;
5. sends the control objects and the commit to the other nodes afterwards, leaving any node not reached as control convergence debt.

Maintenance subsequently converges the current manifest and referenced shards onto every active metadata replica. Obsolete control objects are reclaimed by a dedicated control-store reachability sweep after the configured grace period.

Artwork is ordinary DATA. The catalogue stores only its content `ObjectId`, MIME type and role. Artwork obeys DATA placement/replication/fallback/repair/GC; it is not universally copied merely because nodes replicate metadata.

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
Accepted object references also install durable causal retention claims. Before a commit is accepted, the committing node claims every object it holds that the commit references; an object the commit introduces that this node does not hold is claimed on a node present, or the commit is refused. The other holders receive their claims after acceptance. Claimed copies cannot be evicted or garbage-collected; if corruption removes one, retention itself drives bounded background repair. GC remains active during partitions: each node compares its local claims with its sole accepted head and may release only claim dots causally observed by that head. Claims created by an unseen/concurrent branch are incomparable or newer and therefore survive automatically. Unclaimed staging and extra copies remain collectible without any global branch survey.

Strict object writes use the same durability machinery with immediate scheduling. Deletion is reachability-safe and may be lazily made physically durable because a lost unlink can only preserve garbage, not remove a referenced object.

See `docs/durability.md` for the crash matrix.

## FUSE admission and publication

FUSE is a bounded local frontend. Accepted write bytes are first durable in per-inode spool state and an ordered operation journal. Namespace operations are journalled before their optimistic local result is exposed. Publication workers turn that durable local intent into immutable extents and metadata changes.

A crash therefore does not require guessing whether acknowledged local writes existed: the spool/journal is replayed. Publication is idempotent because DATA is content-addressed and metadata commits are immutable, content-identified DAG nodes with per-origin mutation sequencing.

Before any service/cluster startup, the daemon inspects the configured mountpoint. With `fuse.unmount_if_mounted: true`, a stale mount identified specifically as Macha is unmounted and disappearance is verified. An unrelated filesystem at that path is never automatically unmounted.

## Recovery and fresh genesis

The storage layout is intentionally a fresh-genesis contract. State carries `macha-state-layout-v18`; DATA backends carry `macha-backend-v18` identity. A non-empty unversioned state/backend is refused rather than silently adopted.

Recovery boundaries are independent:

- namespace metadata replays/recovers its committed checkpoint/journal;
- FUSE replays durable local operations not yet observed committed;
- DATA accounting is reconstructed if its derived checkpoint is dirty/missing;
- pack indexes are reconstructed from durable records;
- catalogue control objects can be fetched from another metadata replica;
- repair detects/converges missing DATA replicas;
- cache may simply be discarded/rebuilt.

External metadata providers are enrichment inputs, not recovery dependencies.

## Maintenance

Maintenance is low priority and bounded. It performs replica repair, local backend rebalance, reachability GC, catalogue control convergence/GC and scheduled integrity scrub. Foreground playback, mounted MachaDFS traffic and loader work (ingest and acquisition) pace repair to a small share (`maintenance.repair_weight`) and suspend GC, rebalance and scrub.

Logical GC authority is reachability from accepted metadata; physical deletion is additionally fenced by durable local retention claims. DATA and CONTROL have separate physical sweeps. Newly orphaned/unreferenced objects remain protected by the configured grace period, and a physical copy cannot be reclaimed while any local causal claim remains. This lets GC make progress during partitions without deleting data protected by a concurrent accepted branch.

## Network model

Peers use separate CONTROL and DATA transport lanes. Object payloads travel on DATA; health/membership and metadata/control RPCs travel on CONTROL, isolated from bulk DATA scheduling. A node that accepts no inbound connections (`network.inbound_capable`) keeps both lanes dialled to every capable peer itself and is never dialled; a peer that needs a lane asks for it over the CONTROL session that node opened. The cluster protocol version is 23; a peer offering any other version fails the handshake rather than negotiating down, so a cluster is homogeneous in protocol and a protocol change is a rolling upgrade.

## Correctness gates

These are the invariants the storage implementation exists to preserve. Each is enforced by a regression case; [`VALIDATION.md`](VALIDATION.md) names them and how to run them.

- R=1 capacity aggregates eligible heterogeneous nodes rather than collapsing to the smallest node;
- a full preferred owner falls through deterministically;
- publication never names an object before at least one durable copy exists, and `write_copies` copies are sought;
- repair converges toward `replicas` after nodes/capacity return;
- DATA exhaustion cannot block control metadata commits;
- artwork follows DATA placement and remains remotely readable from a full node;
- pack restart/torn-tail/compaction preserve logical objects;
- unversioned non-empty storage is refused;
- metadata mutation memory remains bounded rather than multiplying whole namespace buffers.

## Source layout

`src/` holds only what every subsystem uses: value types, codecs, crypto, JSON, logging, configuration, durable file replacement, diagnostics, the supervised-thread guard, the retry policy and the process allocator. Everything else lives in a folder for its subsystem, and includes are written relative to `src/` (`#include "cluster/net.hpp"`).

| Folder | Contents |
| --- | --- |
| `subsystem/` | Subsystem interface, plugin ABI, registry and supervisor |
| `storage/` | Local stores, storage pool, durability domains, I/O pressure, block cache, retention |
| `contract/` | Interfaces between subsystems: gates, the absence horizon, the metadata view and the object store |
| `ledger/` | Held and retention ledgers, the object trie and reference counts |
| `cluster/` | Node runtime, RPC transport, membership, placement, replica selection, distributed store, telemetry, UPnP |
| `metadata/` | Metadata snapshots and history, metadata manager, namespace tree and control store |
| `auth/` | Users, roles and sessions |
| `http/` | HTTP server and response compression |
| `api/` | HTTP API endpoints (status, manage, catalogue, files and availability, acquisition, session, users, web) |
| `filesystem/` | Filesystem model, hydration, macOS name normalisation |
| `fuse/` | FUSE frontend in core, mount adapter and plugin |
| `media/` | Media engine, containers, segments, timestamps, VOD, subtitles, FFmpeg logging and tag probing |
| `playback/` | Playback sessions and segment holds |
| `catalogue/` | Catalogue, hints, media catalogue, media information |
| `acquisition/` | Ingest and the cluster-wide job view |
| `torrent/` | Torrent requests and coordinator (core) and the libtorrent plugin |
| `service/` | Service wiring, server configuration and `main` |

Put new code in the folder of the subsystem that owns it. Something moves into the root only when it has no subsystem of its own and several subsystems depend on it.

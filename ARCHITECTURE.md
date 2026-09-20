# Macha architecture

## Governing laws

Three laws order every scheduling, admission and priority decision in the
system. They are cited by number in the source and in review.

1. **Thou Shalt Not Make The Viewer Wait.** And no viewer may be allowed to
   make another viewer wait.
2. **Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
   Viewer Wait.**
3. **Control traffic must remain promptly serviceable.** Viewer priority is a
   large configurable share (95:5 by default), not indefinite starvation of all
   other work.
4. **Thou Shalt Not Shoot Thyself In The Foot.** No operation, code path or
   subsystem may leave the node in a state it cannot recover from on its own.

**Law 4 is different in kind from the first three and is numbered last only so
that the existing numbering keeps working** — laws 1 to 3 are cited by number
throughout the source. It does not take part in their contention at all. Laws
1 to 3 decide *who goes first*; law 4 decides *what may not be done at any
priority*. It is a veto over all three, and where it conflicts with them it
wins: a node that has destroyed itself serves no viewer at all.

"Non-recoverable" is deliberately broad, and means any of these:

- it needs physical access to fix, which on a remote node means an outage
  lasting until somebody travels;
- it needs manual state surgery, or an operator who knows an undocumented
  incantation;
- it loses data the node already acknowledged;
- it cannot be stopped or restarted cleanly, so the ordinary remedy is
  unavailable;
- it degrades without bound and offers no path back — a loop that will not
  finish, a queue that will not drain, a budget that cannot admit one item.

The last of those is the easiest to ship by accident and the hardest to see,
because the node stays up and reports itself healthy the whole time. All five
have been observed in this system, several of them on one afternoon:
2026-09-20 produced a replica that could not rejoin (a cache smaller than its
own unit of work), that could not be stopped without `SIGKILL` (a loop that
never checked its stop token), on hardware that browns out unrecoverably under
sustained load and has no remote power control. Any one of those is law 4. The
combination is why it is a law rather than a preference.

The practical test, before shipping anything: **if this goes wrong on the node
furthest away, does it come back without me?** If the honest answer is no, it
does not ship in that form.

They do not simply rank. Law 2 is explicitly subordinate to law 1 — that is
what its second clause says, and it is why loader work yields to a viewer
rather than negotiating with one. Law 3 is not subordinate to law 1: it is a
floor that law 1 may not eat through. "A large configurable share, not
indefinite starvation" is the whole point of the sentence. A node that serves
viewers perfectly while failing to answer `ping` has broken law 3, and it will
be recorded as dead by peers who cannot see how well it was doing.

So the resolution order is: law 3's floor is reserved first, law 1 takes
priority within what remains, and law 2 governs everything left.

Law 1's second clause is not a footnote. Protecting viewers as a *class*
against loader and speculative work is only half of it: one viewer must not be
able to consume a shared resource to the point where another cannot start. A
resource that is rationed against background work but unrationed *per account*
is a law-1 violation waiting for a rogue or merely enthusiastic client, and
the node cannot tell those apart. So every per-viewer resource needs a
per-account bound as well as a node-wide one, and a session that is cheap
enough to be exempt from one limit is not thereby exempt from being counted
against the others.

A viewer is someone watching or listening right now. A loader is durable work
the user asked for — FUSE publication, ingest, acquisition — which must finish
but need not finish first. Control is health, membership, status and session
traffic, which must stay answerable whatever else the node is doing, because it
is how the cluster and the operator find out anything at all.

The laws are why the node has three memory reserves rather than one budget
(`runtime.control_memory_reserve_bytes`, `viewer_memory_reserve_bytes`,
`loader_memory_reserve_bytes`), why DATA execution priority is viewer
foreground, viewer read-ahead, user loader, then speculative maintenance, why
the HTTP server runs separate control and data lanes, and why the fast-control
RPC executor has a closed allow-list. Each of those is one law expressed in a
different resource. A change to any of them is a change to a law and should be
argued as one.

Law 1 is not absolute in the sense that a viewer never blocks on anything; it is
absolute in the sense that no *other class of work* may be the reason a viewer
blocks. Where a viewer genuinely waits — a request beyond the produced frontier,
a held segment — the wait is on production that viewer itself demanded, it is
bounded by configuration, and the bound is published to the client. Those two
cases are documented as such in [Streaming](docs/streaming.md); they are the
exceptions, and they are not precedent for a third.

## The self-healing disciplines

A separate set of rules governs how the node behaves when something is wrong.
They exist because a system whose failures do not fail loudly produces outages
that are only visible one at a time, each hiding the next.

1. **Re-derive, don't assert.** A durability, placement or confirmation check
   that fails against recorded evidence probes the content-addressed truth and
   re-stamps the evidence. It never retries the stale assertion. Content
   addressing makes ground truth one `has(id)` away, so bookkeeping that could
   be cheaply re-derived is never trusted over it.
2. **One work-item policy.** Every retried unit of work has backoff, a failure
   budget, a parked state visible in Status, and an operator action. No loop
   retries at a fixed interval, and no RPC waits without a deadline. "Not yet"
   must never silently become "forever".
3. **Recover by resolving.** Recovery paths do not throw on an inconsistency
   that has a deterministic resolution: they resolve it, log one line,
   re-journal the outcome so the next start does not see it again, and count it
   in Status. Only a genuinely fatal condition — key mismatch, header
   corruption — may refuse to start. A node that stays up with a counter to
   read beats a node that exits correctly.
4. **Compact history out of the hot path.** A snapshot's size is a function of
   the live namespace. Retirement history and resolved conflicts belong in
   separately compacted structures, so that read, merge, replay and transfer
   cost scales with the library rather than with its history.
5. **A bound smaller than one unit of its own work is not a bound.** A cache
   whose eviction policy can evict everything a running operation needs to
   make progress is not a cache; it is a mechanism for converting a linear
   operation into a quadratic one, silently. The same holds for any budget: if
   it cannot admit one item, it does not degrade gracefully, it fails
   superlinearly and without a log line. Prefer a bound derived from the
   observed unit size over a byte count chosen when the unit was smaller, make
   a budget that cannot admit one item a startup-visible error, and count
   pinned or exempt entries against the budget rather than reporting a
   capacity the caller cannot actually use.

Discipline 3 is also an operational rule: a node is expected to settle bad
input and stay online. Repairing state by hand on a node is not the remedy for
a recovery path that refuses.

Discipline 5 arrived on 2026-09-20 from a replica that could not rejoin the
cluster: one materialisation of its namespace was ~51 MB against a 128 MiB
cache whose two slots were already pinned, so catch-up ran with **zero usable
cache**, replayed the delta chain from a snapshot on every single import, and
made 28 bytes per second of progress while pegging a core. Raising the limit
made it 577 times faster, but the number was never the point: nothing in the
system detected, reported or refused a budget smaller than one unit of its own
work, and the counters that said so unambiguously were read by nobody.

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

Each node advertises the configured capacity of its currently eligible authoritative DATA backends. Placement uses stable capacity-aware hashing; live free space is not a placement weight, avoiding continual reshuffling as disks fill.

For an object, placement produces preferred owners followed by deterministic fallback candidates. A preferred owner that is offline, stalled or unable to admit the object does not cap the cluster: the writer tries the next candidate.

`dht.min_write_replicas` is the number of durable DATA placements required before foreground publication. `dht.replicas` is the desired converged replica count. When the floor is lower than the target, repair records/converges the missing copies later. Neither cache copies nor failed provisional writes count.

Each node may have several local DATA backends. The same stable capacity-aware principle chooses a preferred local backend followed by fallbacks. Backend `limit` controls DATA admission; `reserve_free` additionally preserves real filesystem headroom.

## CONTROL / metadata

Namespace metadata is an encrypted immutable DAG replicated by every node. A mutation is first stored as the exact same immutable commit on `dht.metadata_min_write_replicas` distinct active replicas; only then is an acceptance certificate for that commit persisted. There is no privileged voter subset and no live distributed CAS/PREPARE/COMMIT phase. A receiving replica may store a commit regardless of its current head. Accepted heads are a set rather than a singleton, so disconnected cohorts may create independently valid branches. Replicas exchange accepted-head certificates and compact ancestry, reconcile divergent maximal heads by common-ancestor semantic merge, and materialise incompatible namespace/catalogue alternatives as durable conflicts rather than overwriting a branch.

Content-addressed control objects are stored separately from DATA. The principal current user is the media catalogue. DATA quota exhaustion must not prevent a metadata replica from storing control objects required to represent committed metadata.

Control object storage has its own safety ceiling. Metadata/control capacity is an operational resource and must be monitored, but it is not borrowed by bulk media DATA.

## Catalogue

The catalogue is not one ever-growing immutable object. It consists of 64 deterministic content-addressed shards plus a small manifest root referenced by namespace metadata.

A catalogue mutation:

1. constructs only the affected shard contents and a successor manifest;
2. verifies newly referenced artwork DATA exists through the ordinary distributed store;
3. durably stores changed control objects on the metadata write floor;
4. publishes an immutable namespace commit referencing the new manifest root to the metadata write floor;
5. leaves missing active-replica copies as control convergence debt.

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
Accepted object references also install durable causal retention claims on the physical DATA/CONTROL copies before metadata acceptance. Claimed copies cannot be evicted or garbage-collected; if corruption removes one, retention itself drives bounded background repair. GC remains active during partitions: each node compares its local claims with its sole accepted head and may release only claim dots causally observed by that head. Claims created by an unseen/concurrent branch are incomparable or newer and therefore survive automatically. Unclaimed staging and extra copies remain collectible without any global branch survey.

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

Maintenance is low priority and bounded. It performs replica repair, local backend rebalance, reachability GC, catalogue control convergence/GC and scheduled integrity scrub. Foreground playback and mounted MachaDFS traffic suppress speculative work.

Logical GC authority is reachability from accepted metadata; physical deletion is additionally fenced by durable local retention claims. DATA and CONTROL have separate physical sweeps. Newly orphaned/unreferenced objects remain protected by the configured grace period, and a physical copy cannot be reclaimed while any local causal claim remains. This lets GC make progress during partitions without deleting data protected by a concurrent accepted branch.

## Network model

Peers use separate CONTROL and DATA transport lanes. Health/membership and metadata/control RPCs are isolated from bulk DATA scheduling. The cluster protocol version is 21; a peer offering any other version fails the handshake rather than negotiating down, so a cluster is homogeneous in protocol and a protocol change is a rolling upgrade.

## Correctness gates

These are the invariants the storage implementation exists to preserve. Each is enforced by a regression case; [`VALIDATION.md`](VALIDATION.md) names them and how to run them.

- R=1 capacity aggregates eligible heterogeneous nodes rather than collapsing to the smallest node;
- a full preferred owner falls through deterministically;
- publication never occurs below `min_write_replicas`;
- repair converges toward `replicas` after nodes/capacity return;
- DATA exhaustion cannot block control metadata commits;
- artwork follows DATA placement and remains remotely readable from a full node;
- pack restart/torn-tail/compaction preserve logical objects;
- unversioned non-empty storage is refused;
- metadata mutation memory remains bounded rather than multiplying whole namespace buffers.

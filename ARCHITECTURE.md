# Architecture

Macha keeps the filesystem, metadata, inter-node placement and local storage layers separate. The point is not abstraction for its own sake: disk loss, node loss, network failure and cache eviction have different semantics and should fail independently.

```text
FuseAdapter -> FuseFrontend
                 |
                 +---- local inode/namespace overlay + write spool
                 +---- bounded request broker + async publication
                 +---- FUSE hydration hints
                           |
FileSystem ----------------+---- MetadataManager ---- voter quorum + committed checkpoints
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
                             CONTROL + DATA connections per active peer

NodeRuntime
   +---- state_path: node.id, metadata, backend identities, temp writes
   +---- Membership
   +---- fetched-block persistence worker
   +---- membership/storage refresh worker

Service
   +---- CatalogueManager / catalogue JSON API
   +---- PlaybackManager / concurrent HTTP streaming
   |       +---- direct logical-file range responses
   |       +---- MediaEngine -> in-process libav backend
   |       +---- custom AVIO -> pinned Macha ReadHandle
   |       +---- bounded fMP4 SegmentStore / capability URLs
   +---- repair / local rebalance / scrub / GC scheduler
```

## Bounded FUSE frontend

The mounted filesystem is a local bounded frontend to Macha, not a synchronous projection of distributed state. `FuseAdapter` contains only libfuse/macFUSE translation and delegates kernel operations to `FuseFrontend`. `FuseFrontend` has no reason to wait for catalogue work, repair, checkpoint propagation or metadata quorum on a kernel callback. Every operation enters an operation-class broker with a configured deadline capped by an absolute ceiling; if local/bounded completion is impossible, the request fails to the kernel.

The frontend seeds a process-local namespace/inode graph from the node's local committed metadata replica before mounting. Open file descriptions retain stable inode identity rather than paths. Rename therefore changes namespace edges, including whole directory subtrees, without invalidating an open descriptor. Namespace mutations update the local graph in kernel order and then enter a FIFO asynchronous publication queue. Replacing or unlinking a name detaches the old inode's published pathname, so later close/flush on an old descriptor cannot resurrect or overwrite the replacement.

Writes use per-inode byte spools under `state_path/fuse-spool`, and 0.14.9 adds a separate durable operation journal at `state_path/fuse-spool/operations.log`. The journal records inode identity/path state, namespace mutations, and the exact ordered write/truncate descriptors which make spool bytes meaningful. Before a write is acknowledged, its payload bytes are fsynced to the spool and its operation descriptor is appended and fsynced to the journal. Namespace mutations likewise journal their inode descriptor(s) and operation before the optimistic local namespace change is exposed. This deliberately adds local durability latency to FUSE mutation admission; distributed publication remains asynchronous. Writable-handle `flush`, `fsync` and `release` request publication, while closing a read-only handle never publishes another handle's dirty data. `fsync` also re-syncs the local spool/journal state but does not promise distributed quorum convergence. Kernel-facing mutation deadlines apply at the safe admission boundary: a mutation may time out before its broker task starts, but after execution begins the callback waits for the actual local result. Publication workers seal an operation prefix, wait through that inode's latest accepted namespace sequence, replay it through the existing `WriteHandle`, and retire only effects which have subsequently been observed in MetadataManager's decoded committed view. This reuses Macha's existing extent hashing, sparse/rebuild behaviour, replication, garbage retirement and metadata-conflict handling instead of creating a second object writer.

On frontend/process restart, the optimistic namespace is reconstructed from the local committed metadata snapshot plus the durable operation journal, then unpublished work is requeued in recorded order. Pending rename/unlink operations shadow stale snapshot paths while they remain unconfirmed, so an older decoded snapshot cannot reintroduce a name which the local FUSE client already moved or removed. A journal-referenced spool which is missing or shorter than its recorded write range is a startup error. A non-empty `inode-*.spool` with no recoverable journal history is never guessed at or discarded: it is durably renamed to an `*.orphan.*` quarantine artifact, logged with its byte count, and omitted from replay so an unattributable artifact from an older/crashed frontend cannot take the whole node offline. An incomplete final journal frame is trimmed to the last checksum-valid frame; an unexplained tail beyond otherwise valid referenced spool data is preserved separately and reported. The journal is compacted when no durable operations remain; it is not size-bounded while a backlog remains outstanding.

Reads snapshot the committed immutable manifest plus pending local operations. They do not commit a dirty writer first. Shrink/extend/write order is replayed over the committed data so an old suffix cannot reappear after truncate-and-extend. Any required remote extent fetch carries the FUSE read deadline and cancellation token. The frontend also emits immediate demand as a high-priority dynamic `HydrationHintProvider`; requested extents and configurable read-ahead therefore enter the same `CacheHydrator` used by playback prediction. `DistributedStore` coalesces duplicate object fetches, allowing FUSE and speculative/predictive demand to share one transfer. With `fuse.write_through_cache`, extents produced by FUSE publication are inserted into the existing `PersistentBlockCache`.

Queue limits provide bounded backpressure. Namespace capacity is reserved before changing the local graph so a saturated publication queue returns `EAGAIN` without leaving a half-accepted mutation. File bytes already accepted into the spool are never dropped merely because the publication queue is full; their inode is marked deferred and admitted when capacity becomes available.

FUSE does not run a namespace refresh timer and kernel lookups never perform distributed metadata I/O. `MetadataManager` publishes a process-local immutable decoded snapshot plus a namespace revision which advances only when `MetadataSnapshot::entries` changes. Namespace-facing kernel requests perform a lock-free revision comparison; only a newer already-decoded namespace revision is adopted. Catalogue-root, garbage, voter and other metadata-only commits therefore do not rebuild the FUSE inode/path graph. Concurrent adoptions are serialised and local queued namespace mutations take precedence, so idle mounts have no namespace-refresh wakeups and a remote generation notice cannot turn Finder/stat traffic into a metadata-quorum retry loop.

Asynchronous publication treats network disconnect/reset/abort conditions as retryable and never exposes a transient transport errno as a permanent inode error. Publication admission is foreground-aware: an open writable FUSE handle keeps concurrency at `foreground_commit_workers`, and a post-I/O quiet period prevents a short inter-file pause from immediately starting the whole backlog. Viewer playback is a stronger priority boundary: once foreground playback/probe/seek demand is observed, new FUSE data publication does not start and any active replay yields between bounded spool chunks and before its metadata commit. The bytes are already durable in the local spool, so delaying distributed convergence does not delay the writer or lose accepted data. Workers may prepare immutable extents concurrently when the mount is quiet, but their final metadata mutation is serialised with other FUSE namespace/data commits to avoid competing CAS operations against the same metadata snapshot.

FUSE itself remains outside Macha's control. When fail-closed mountpoint protection is enabled, the directory covered by the live mount has its write bits removed through a pre-mount file descriptor after mounting succeeds. An independent OS mount-table watchdog detects unexpected disappearance of the mount, requests node shutdown and leaves the naked directory protected so tools such as `rsync` fail rather than writing underneath the namespace. An intentional clean unmount restores the directory's original mode.

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
- the FUSE operation journal and per-inode local write spools;
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

The hydrator keeps at most `hydration.max_inflight` speculative fetches active (default four) and rebuilds the hint set before each dispatch. In-flight objects count as an already-dispatched prefix, so an ordered run can advance without jumping over an undispatched or failed earlier extent. Hint producers may provide a lightweight wake callback: playback progress and FUSE read demand use it to wake an otherwise blocked hydrator directly. The configured hydration interval is used only while asynchronous fetches are actually outstanding; an idle hydrator has no sampling loop. Failed objects wake at their retry deadline. Notifications are scheduling hints only—the provider state remains authoritative—so duplicate/missed wakeups cannot change correctness. The window is deliberately small: when playback changes, stale runs stop producing new work quickly without requiring a cancellation protocol. Speculative fetches are non-foreground and go only into the persistent cache; they do not alter authoritative DHT placement. With no configured persistent cache, hydration is inactive and foreground reads are unchanged.

## Replica-aware retrieval

`DistributedStore` owns one replica selector shared by foreground filesystem reads and speculative hydration. Hint engines never select peers. For each remote object, deterministic DHT owners remain the preferred source group and later HRW-ranked nodes remain fallback sources.

The selector tracks per-peer in-flight foreground/speculative work, recent transfer latency and consecutive failures. Equivalent unknown replicas are deterministically striped by extent index. Foreground reads optimise likely completion latency; speculative work strongly prefers peers not carrying foreground traffic and spreads independent extents over otherwise suitable replicas.

Remote requests for the same `ObjectId` are coalesced inside `DistributedStore`. If foreground playback asks for an object already being hydrated, the existing transfer is promoted from speculative to foreground accounting and both callers share the result. The same object is not raced against several replicas by default; a failed source is retried against the next suitable peer. Hedged duplicate reads remain a possible later optimisation.

Catalogue matching uses the stable `macha:<sha256>` media identity derived from file size and the ordered immutable extent manifest. Namespace renames therefore do not change the identity. Path bindings remain supported for manual/legacy catalogue records. Movie sequence prediction can use a shared `collection` or `tmdb_collection` external ID, or an existing common parent relationship, without changing the catalogue binary format.

## Metadata state

The canonical namespace is still a versioned `MetadataSnapshot`, but ordinary mutation is no longer replicated or persisted as a complete snapshot. 0.9.0 uses a deterministic **delta CAS + local encrypted journal**.

Each node stores metadata under `state_path/metadata/` as:

- `checkpoint.meta`: a complete encrypted **committed** `MetadataRecord`;
- `journal.log`: encrypted append-only records after that checkpoint. A CAS vote appends `prepare(delta)` (or, for rare repair/policy paths, a full prepare/seed); quorum commitment appends `commit(generation, hash)`.

The in-memory current record may therefore be newer than the committed record. On restart the checkpoint is loaded and the journal is replayed deterministically. A prepared vote remains current but cannot become a recovery witness until its matching commit marker is present. An incomplete final frame, or a complete-length final frame which fails AES-GCM authentication, is treated as an interrupted append: the rejected EOF bytes are preserved as `journal.log.corrupt.*` and only the authenticated prefix is retained. Authentication/replay failure before a later frame remains fatal because skipping an interior record could break the metadata chain.

Ordinary filesystem and catalogue mutation begins from the node's durable current record, applies the requested change locally, and computes a canonical `MetadataDelta`. The delta can contain mutation-sequence updates, entry upserts/deletes, a catalogue-root set/clear and garbage-tombstone upserts/erases. Cluster policy (`metadata_voters`, data replication and extent size) is not mutable through this path. If a change cannot be represented compactly, or the encoded delta would be larger than the resulting snapshot, the existing full-snapshot CAS primitive is used instead.

A voter accepts a delta only when its current generation/hash matches the proposal base. It applies the delta itself, re-encodes the canonical snapshot and computes the expected successor hash before durably appending the prepare record. Successful replies carry only generation/previous/hash. Once the proposer observes voter quorum, `commit_metadata` carries only generation+hash and each voter appends the commit marker. Mutation-sequence clocks make retry recovery idempotent after a quorum succeeds but the proposer loses the acknowledgement.

Journal compaction is deliberately not part of the mutation critical path. When the journal reaches 128 records or 8 MiB, the existing idle/speculative metadata-maintenance pass writes the current committed record to `checkpoint.meta` and then truncates the journal. The checkpoint is published first; replay ignores exact generations already represented by it, so a crash between checkpoint publication and journal truncation is safe.

0.10.0 writes SM8 snapshots and DLT2 mutation records so garbage retirements can carry an age and be removed from metadata. Existing SM5/SM6/SM7 snapshots remain readable. Persisted DLT1 journal frames are replayed with the exact historical SM7 successor encoding before their recorded hashes are verified; this is on-disk migration compatibility, not a wire-protocol fallback. Once a new SM8/DLT2 mutation is committed, subsequent journal state is native 0.10.0 format. The immutable object-file and backend-accounting formats are unchanged.

0.8.x state is migrated on first 0.9.0 open. `committed.meta` becomes the checkpoint. If `current.meta` contains a newer accepted vote, that full record is journaled **before** the new checkpoint becomes authoritative so migration cannot forget an uncommitted vote. The old files are then renamed to `.v10`. Rollback to pre-0.9 metadata handling after migration is unsupported.

Only the configured voter set participates in consensus. After a committed generation is known, maintenance distributes a complete checkpoint to every active node as a **recovery witness**. Witnesses are not extra votes. Full records remain the repair/recovery primitive because they allow a stale or replacement node to converge without requiring it to possess every intermediate delta.

The node-local persistent block cache also keeps an independent encrypted full committed metadata snapshot for disaster recovery. Every locally learned committed record refreshes this copy (an unchanged hash is not rewritten). If the primary checkpoint/journal cannot authenticate or replay, a valid cache snapshot may seed startup, but it is explicitly non-authoritative: the damaged primary files are quarantined, a durable `metadata/recovery.required` marker survives restart, isolated stale-read fallback is disabled, and mutation must first obtain a successful metadata quorum read. The marker is cleared only after that quorum validation succeeds.

Replica counts are mutable cluster policy. A coordinated whole-cluster restart may change data replication, metadata voter count, or both. The existing voter majority serialises the policy change through the full-snapshot CAS path; a resized voter group is seeded before normal metadata mutation continues, and object repair subsequently converges the immutable extent set to the new data replica count. Transport v11 does not advertise a node's desired replica policy, so rolling changes with mixed configurations are deliberately unsupported. `extent_size` is intentionally immutable for an existing namespace.

Read-only metadata retains the short in-memory TTL cache with generation invalidation. A newer remote generation notice or CAS conflict forces a quorum refresh before retry.

## Catalogue metadata

The catalogue is a complete cluster-wide metadata dataset. Its current snapshot is an immutable content-addressed `DistributedStore` object; `MetadataSnapshot::catalogue_root` is the quorum-committed pointer to that object. Catalogue mutations therefore acquire the same serialization and durability boundary as filesystem metadata without storing catalogue bytes inside the metadata record itself.

Every referenced artwork object is also content-addressed. The current catalogue root and all live artwork are marked **universal** during maintenance: every active node must hold them, regardless of normal `dht.replicas`. A joining node pulls the root and all artwork before its catalogue status becomes ready. Catalogue synchronisation is attempted before ordinary media repair. Search/list operate from the complete local snapshot.

API catalogue reads are live but cached. Warm reads acquire a shared immutable decoded snapshot and never perform distributed metadata or catalogue-root I/O. Metadata generation notices and `metadata_cache_ms` validation are handled by the service control plane; when `catalogue_root` changes it fetches/decodes the new immutable object and atomically publishes a replacement snapshot. An unchanged root reuses the existing object. If background validation temporarily fails, the previous coherent snapshot remains available, with cached and known metadata generations exposed separately in catalogue status.

Superseded catalogue roots and artwork that loses its final live reference receive the same committed retirement tombstones as filesystem extents. Combined filesystem+catalogue reachability is authoritative: a live reference removes a stale retirement, while an unreachable object becomes eligible for the local physical sweep only after `maintenance.garbage_grace_ms`. Mature tombstones are then pruned from metadata; a node that was disconnected long enough to miss the tombstone still converges because physical GC is reachability-based rather than dependent on retaining the deletion record forever.

This first implementation intentionally shares the filesystem metadata generation for the small `catalogue_root` pointer. If catalogue mutation volume later makes that a contention point, the root can move to an independent quorum namespace without changing catalogue object or artwork representation.

### Catalogue work queue and discovery

Catalogue matching is scheduled through a persistent per-node hint queue rather than being an inseparable phase of namespace traversal. A hint identifies one canonical namespace path plus one or more origins. Current producers are ingest completion, explicit/manual rescan, namespace-mutation discovery and periodic namespace discovery. The initial priorities are 100, 80, 50 and 10 respectively. Hints for the same canonical path coalesce; each origin retains its own source reference and priority, so clearing one producer does not destroy another producer's scheduling context. Equal-priority work is selected fairly across top-level roots (`Movies`, `TV`, `Music`) instead of allowing one large root to starve the others.

The scanner is therefore a discovery/reconciliation source, not the catalogue worker itself. It still walks provider-owned roots and computes stable `macha:<sha256>` media identities, but it submits unbound or artwork-refresh candidates to the hint queue. Periodic and namespace-mutation hints use that stable media identity as their revision token: rediscovering unchanged bytes does not repeat a terminal provider miss, while replacing bytes at the same path reopens the coalesced work item. Explicit rescans deliberately reopen terminal work. Ingest completion submits the exact final namespace path immediately at high priority and retains its job ID as provenance.

Provider ownership and matching semantics are unchanged. Movie and TV roots use TMDB metadata. Music reads embedded tags first, generates scored local candidates, then offers them to configured metadata providers in priority order (MusicBrainz first and optional Discogs fallback, with Cover Art Archive or Discogs artwork). Semantic misses become terminal negative results for that media revision; transport/provider failures remain retryable and are reported distinctly from a no-match result.

Hint processing reuses the same movie, TV and music probe/candidate/provider machinery. Workers stage successful hint results for a bounded provider batch and reconcile that batch in one catalogue mutation, avoiding one metadata CAS per media file. A successful hint is additive and idempotent: it can bind media, create hierarchy and stage artwork, but it cannot infer that unrelated catalogue data disappeared. Full namespace discovery remains the only destructive reconciliation authority. Only a complete traversal may remove vanished scanner-owned leaf bindings and empty scanner-created parents; if any configured root is unavailable, pruning remains suppressed exactly as before.

The queue is persisted under `state_path/catalogue/hints.json`. A daemon restart requeues work that was persisted as processing. Terminal ingest-origin results remain available until the ingest job is cleared so the acquisition API can report provider, matched catalogue IDs, no-match results or failures. Scanner-origin successful work may disappear once the media is bound; terminal scanner negative results act as a revision cache and are reopened by content change or explicit rescan. The full periodic scan remains the convergence safety net if an advisory hint is ever lost.

Catalogue workers run on every node for that node's persisted hints; full namespace traversal and destructive reconciliation remain coordinator-owned by the lowest active `NodeId`. Catalogue CAS/retry semantics protect overlapping additive updates during membership changes. This avoids requiring ingest on a non-coordinator to wait for a separate hint-forwarding protocol while preserving a single authority for absence/pruning.

## Voter failure and replacement

Normal voter replacement requires a majority of the old voter set and therefore preserves quorum intersection.

If the old voter quorum has been permanently destroyed, replacement recovery uses committed checkpoints instead. Recovery is allowed only when:

- every node still considered active participates in the checkpoint survey;
- surviving highest-generation committed checkpoints agree;
- the old voter quorum is no longer available;
- enough fresh, genesis-state nodes exist to fill the missing voter seats;
- the checkpoint extent size matches local configuration.

The new voter set is installed as the next metadata generation and committed by a quorum of that new set before it becomes a recovery checkpoint.
Configured bootstrap joiners use the same checkpoint survey as a genesis gate. Genesis is allowed only after every active member has answered and the survey contains no durable post-genesis history; an incomplete survey is not evidence of a virgin cluster.

This deliberately does not let an isolated old cohort promote itself around a network partition. Stale active membership delays recovery until `dead_after` expires.

After namespace recovery, ordinary object maintenance walks the recovered live-object set and repopulates replacement nodes.

## Offline behaviour

Namespace mutation always requires quorum.

If quorum is unavailable, a node may use its last valid post-genesis snapshot for read-only namespace access. It can read files only where the required extents exist in authoritative storage or cache. It cannot invent missing blocks or mutate the namespace.

## Transport v11

Each active peer pair can have two persistent authenticated bidirectional TCP lanes. `CONTROL` carries health, membership and other small control-plane RPCs. `DATA` carries object traffic only. Metadata remains on the CONTROL transport even when classified as read-ahead or speculative work. The DATA lane is lazy and is opened when object traffic is first required. Separating the TCP sequence spaces prevents retransmission or kernel buffering of bulk payloads from head-of-line blocking liveness and membership traffic.

Both lanes are canonical independently by authenticated `(NodeId, lane)`, not hostname or socket direction. Simultaneous cross-dial arbitration therefore leaves at most one CONTROL and one DATA connection per peer. The TCP dialler owns odd request IDs and the acceptor owns even request IDs on each lane, so either end can originate work without request/reply ambiguity. Requests may complete out of order and cancellation of one request does not tear down unrelated work.

The authenticated v13 handshake includes the requested lane and negotiates `max_frame_size`; the lower configured ceiling wins. Logical messages are split into variable-length frames no larger than that ceiling. Storage extents remain storage objects and are not transport framing units. Each frame is independently AES-256-GCM protected. v12-and-earlier peers are rejected at the protocol boundary. v13 retains the authenticated two-lane handshake and permits prioritised metadata frames without placing metadata payloads on the DATA transport; DATA is reserved for object traffic.

For queued data-class work, frame type defines priority completely: foreground, read-ahead, then speculative. `foreground` is reserved for media playback/probe/seek and other viewer-blocking reads. Mounted-filesystem reads/writes and useful read-ahead use `read_ahead`; repair and low-value prediction use `speculative`. The outbound scheduler selects the most urgent runnable transfer for every frame and returns to scheduling immediately afterwards. A promotion notification can raise an existing transfer and cancellation stops queued remainder frames without disturbing other request IDs. Object-transfer promotion and cancellation notifications remain on DATA because their request IDs are scoped to that connection. Metadata and placement probes may carry read-ahead/speculative frame classes on CONTROL; the frame scheduler therefore lets control-priority health and membership messages pre-empt them between fragments. DATA remains free of metadata payloads, preserving the viewer-critical object path.

RPCs have no wall-clock completion deadline. Control/data stall intervals are DEBUG observability thresholds only. Health probes use only the CONTROL lane; sustained inability to establish control-lane liveness within `dead_after` marks the peer dead.

Server execution retains dedicated control workers plus data workers. Data workers choose foreground before read-ahead before speculative queued work, and lower-priority work is capped below the full worker count so it cannot occupy every handler before a later foreground request arrives. Foreground work can use the whole pool. This preserves priority both in the queue and at the execution-capacity boundary after a request has reached the peer.

Every lane uses an HMAC-authenticated ephemeral X25519 handshake, HKDF-SHA256 directional keys and AES-256-GCM variable-length frames.


## Streaming and media engines

Playback policy is owned by `PlaybackManager`, not libav. A client supplies capabilities and preferences; the resolver chooses a source representation and a `PlaybackPlan` containing direct/remux/transcode mode plus per-stream copy/transcode decisions. `MediaEngine` consumes that plan and exposes media concepts only. The current implementation is `LibavMediaEngine`; callers do not depend on libav types.

Direct play exposes the pinned logical Macha file as a streaming HTTP body with byte-range support. Transformed playback opens the same pinned file through a custom seekable libav `AVIOContext`, so probe/remux/transcode reads go directly through `FileSystem::open_read` and the DHT. No FUSE mount, loopback HTTP source or media subprocess is involved.

Transformed output is fragmented MP4. A custom output `AVIOContext` publishes the init fragment and completed `moof`/`mdat` media fragments into a bounded `MediaSegmentStore`; Macha generates the HLS playlist itself. Finite movies and episodes are exposed as complete immutable VOD playlists from the first request. Remux planning uses indexed video random-access points; transcode planning uses the encoder GOP cadence. Fragment bytes are still produced lazily and retained in memory up to the configured budget, with older fragments optionally spilling to `streaming.temp_path`. Session capability URLs are distinct from the permanent API Bearer token.

The public HTTP server has a bounded accepted-connection queue and worker pool. Response bodies are abstract sources rather than necessarily resident byte strings, so long direct-media responses do not serialize catalogue/API work and several HLS fragment requests can be served concurrently.

A transformed producer blocks on a condition variable when generated segment distance exceeds the configured look-ahead from actual client demand and resumes as requested fragment indexes advance. Valid requests for future VOD fragments wait for sequential production rather than receiving a transient missing-segment response. Seeking or a quality/track/media change creates a new internal generation under the same logical playback session. Old generation URLs become invalid after replacement. The former growing HLS EVENT mode is intentionally not used for finite media; its future live/event role and observed native-player edge behaviour are recorded in `ROADMAP.md`.

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

Viewer and mounted-filesystem activity both mark the store busy for background work, including activity arriving over DATA from another node. Viewer traffic is still the highest DATA priority; mount/read-ahead traffic is the medium class. During `foreground_quiet_ms`, local rebalance and scrub pause. With the default busy network fraction of zero, inter-node repair pauses as well. Credits are burst-capped. Replica repair is additionally capped by objects examined and remote operations so a zero-byte settled scan still consumes a finite CPU budget.

Local backend state is deliberately separated from disk execution. A backend mutex protects only configuration/online state and the current `shared_ptr<LocalStore>`; the pointer is copied and the mutex released before any filesystem operation. Health/control handling therefore never waits behind a backend directory walk or `LocalStore` shutdown. Scrub, local rebalance, reachability GC and distributed push repair use independent persistent physical-object cursors, bounded per scheduler slice, rather than repeatedly materialising the full local object set. Distributed pull repair advances the immutable ordered live-object index directly and does not copy the complete live set for each slice.

## Namespace read path

The committed metadata record is decoded once per observed metadata generation into an immutable shared snapshot. FUSE `getattr` performs a direct lookup in that snapshot; `readdir` uses a generation-matched directory-child path index rather than scanning every namespace entry. Media-id resolution similarly caches only paths, not duplicate `FsEntry` extent manifests. A new committed generation invalidates these views atomically. This keeps ordinary stat/list cost proportional to the requested entry/directory rather than to the complete media namespace.

Macha namespace keys remain byte-preserving across platforms. On macOS the runtime namespace index, not metadata storage, implements canonical-equivalent lookup: each stored path is indexed by its NFC form and lookup aliases resolve back to the exact persisted spelling. Names returned by FUSE `readdir` are converted to Unicode Normalization Form D as required by macFUSE/Finder. New names use NFC for the new leaf while retaining the actual stored spelling of an existing parent. This allows namespace state written by older versions in NFC, NFD or mixed per-component forms to remain addressable without a metadata migration.

## Garbage collection

0.10.0 separates logical retirement from physical reclamation. The canonical mark set is the sorted union of every non-hole filesystem extent, the committed catalogue root and all artwork referenced by that catalogue. A live reference always wins, including when a content hash that was retired earlier is reused.

Committed deletion/replacement records a `GarbageRef` containing the object ID, retirement wall-clock time and a random retirement ID. The retirement ID makes maintenance ABA-safe: pruning an old retirement cannot remove a newer retirement of the same content hash after reuse. Tombstones from pre-0.10 SM7/DLT1 state have neither field; first 0.10.0 maintenance stamps them and gives them a fresh complete grace period. After the grace expires the tombstone stops protecting the object and is removed from metadata, so the canonical metadata garbage list is bounded by recent retirements rather than cluster lifetime.

Physical reclamation is a node-local mark/sweep. `StoragePool` advances a dedicated persistent cursor over authoritative object files, examines at most a bounded number per maintenance slice, and immediately yields to viewer or mounted-filesystem activity. An object is removed only when it is absent from the combined live set, absent from the still-protected retirement set, and its local file has been untouched for at least `maintenance.garbage_grace_ms`. The age check and removal share the same `LocalStore` mutation lock as `put()`. Reaffirming an existing content hash refreshes its file age, preventing a newly reused object from racing an old orphan decision.

Because sweep starts from physical objects rather than tombstones, it also collects data objects written before a metadata CAS that never committed, including crash/interrupted-upload orphans. No cluster-wide object census or delete RPC is required: committed metadata defines reachability and each node cleans its own authoritative store. Metadata quorum maintenance must continue to succeed for destructive maintenance to advance; an isolated node therefore retains its data rather than sweeping from an indefinitely stale namespace. Persistent cache eviction remains independent, while explicit mature retirements may discard cache copies immediately.

## Scaling boundary

0.9.0 removes namespace-size network and durable-write amplification from ordinary metadata mutation, but the canonical in-memory state is still a complete `MetadataSnapshot`. A proposer and voter re-encode that snapshot to derive the successor hash, and changing one very large file still carries that file's complete extent manifest in its delta. Background stale-node repair also uses complete snapshots. Those are now the metadata scaling boundaries; none require redesigning the immutable extent layer, storage pool or media cache.

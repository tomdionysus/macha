# Durability

## Rule

Macha may publish a logical reference only after the storage class responsible for that reference has satisfied its durability contract.

For DATA, the contract is a durable authoritative copy on a node present. For namespace/control metadata, it is a durable copy on the committing node. `dht.write_copies` is the number of copies of a DATA object sought before its write returns: with that many nodes present and answering, the object is on that many when it returns; with fewer, it returns on the copies it has and repair delivers the rest. A namespace commit seeks no copy before it returns: it is durable on the committing node, and every node present is sent it afterwards. `dht.metadata_write_copies` is the copies sought, before the write returns, of the control objects a catalogue write stores. Nothing is refused for lack of peers.

Cache never counts.

## DATA durability domains

Authoritative DATA backends on the same physical filesystem share a `DurabilityDomain`. The domain tracks monotonically increasing mutation generations and a durable frontier. A DATA placement is represented by a ticket containing the current process epoch, physical durability domain, mutation generation and backend incarnation.

Those fences prevent a ticket from an old process or a removed/reopened backend from satisfying a current publication.

A mutation generation is issued only after the object write/rename operations for that mutation have completed. The physical barrier can cover several generations at once.

On Linux the domain barrier uses `syncfs()` on a representative backend path. Platforms without `syncfs()` use the supported conservative fsync fallback.

## Strict and deferred writes

`LocalStore::put()` is strict: it returns success only after the local durability requirement has been met.

Distributed publication uses the deferred path where appropriate:

```text
write immutable object
      |
      v
provisional durability ticket
      |
      v
coalesce required tickets by physical domain
      |
      v
physical durability barrier
      |
      v
publication may reference object
```

Immediate versus batchable urgency changes scheduling, not correctness. Both use the same generation/ticket mechanism.

## Distributed DATA publication

The writer ranks preferred owners and deterministic fallbacks, trying its own store first when it is an eligible candidate. It obtains placements until `write_copies` have accepted and become durably covered, or until no further node can take a copy promptly: a peer that stalls past `dht.write_stall_ms` is not waited for once one copy has landed. The write returns on the copies that landed, and only then may MachaDFS namespace metadata reference the new extents.

Before a metadata commit is published, a reconciliation's merge commit included, every DATA object named by the entries it changes is given a durable retention claim on the holders present, and every CONTROL object it introduces (catalogue shards, namespace tree nodes) a claim that must be recorded on the committing node. A merge also claims the extents and catalogue roots of the conflict alternatives it records. Its claims carry a fresh sequence of the reconciling node that the merge's clock does not include, so every reconciler still mints the same commit; they become releasable once a head carrying that node's next mutation no longer references the object.

One refusal remains. A commit that brings new bytes into the namespace is refused if no node present holds those bytes (`DATA object is held by no node present before metadata publication`); the writer still has them and puts them again. Bytes the parent head already named are not checked, so a rename, a chmod or an append to a file whose older extents live on an absent node commits.

`replicas` can be larger than the copies a write returned on. Missing desired copies remain repair debt and are converged by maintenance.

A full or offline preferred owner changes which eligible candidate supplies a copy.

## Namespace metadata

Every node is metadata-capable. A metadata commit is accepted, and its write returns, once the exact immutable commit and its acceptance certificate are durably held by the committing node. Every node present is then sent it without the caller waiting, and repair delivers it to the rest. Until a second node holds it, a commit exists on one node: the loss of that node's state disk in that interval loses it. A receiver stores the commit independently of its current head; separated nodes may therefore preserve different accepted successors of the same ancestor. Ordinary linear commits may use compact deterministic deltas in the history store, while full records remain valid recovery material.

This is not majority consensus. It deliberately allows any surviving node to continue alone, so separated nodes can produce divergent valid histories. Those histories are preserved and destructive convergence is refused: two divergent heads are reconciled through a multi-parent commit computed from the two heads alone, non-conflicting namespace changes are merged, and concurrent alternatives are both retained in a durable conflict record, with the later one in place. Status reports them as `metadata.conflicts`, `conflicts_resolved` and `conflicts_superseded`; see [Metadata replication and reconciliation](metadata.md).

A write accepted by one node alone exists on that node until a peer is present to take a copy. In `GET /api/v1/status`, `diagnostics.metadata.head_holders` is how many of the nodes present held the current head at the last repair pass, and `head_holders_present` how many nodes were present at it.

Metadata copies are independent of DATA `dht.replicas` and `dht.write_copies`.

## Catalogue control durability

Catalogue manifest/shard objects are CONTROL, as are the nodes of a tree-backed namespace: a new namespace root may be referenced only once the committing node durably holds every tree node it introduces, exactly as a manifest may not name a shard the node does not hold. Before namespace metadata can point at a new catalogue manifest:

1. newly referenced artwork DATA must be readable through the normal DATA store;
2. changed catalogue shards must be durable on the committing node, with copies sought on `metadata_write_copies` nodes;
3. the successor manifest must be durable on the committing node, with copies sought likewise;
4. namespace metadata acceptance publishes the new manifest root.

After publication, maintenance converges the current manifest/shards to all active metadata replicas. A joining or returning node with missing control objects fetches them from another replica. This convergence does not make artwork universal.

Transient metadata/control unavailability (a node with no usable namespace yet) causes catalogue scanner work to defer. It does not consume the hint's provider/content failure attempts.

## FUSE durability

FUSE success is separated into local admission durability and later distributed publication.

For an accepted file write:

```text
write bytes to inode spool
      |
fsync local spool state
      |
append + fsync ordered operation descriptor
      |
return local success to kernel
      |
asynchronous publication builds DATA extents
      |
wait for DATA copies to be durable
      |
commit namespace metadata
      |
record operation completion / retire spool
```

Namespace mutations similarly journal their local intent before exposing the optimistic local result.

This allows an unclean process restart to replay acknowledged local operations without inventing state from partially written files.

Recovery and sustained namespace backlogs are drained as bounded ordered
batches, currently limited by both operation count and encoded delta bytes. A
compatible batch is applied to one mutable snapshot and published as one
metadata commit. If an operation fails, only the largest valid prefix may be
published; later operations remain behind the failing journal entry. A batch
which is already fully reflected in committed metadata advances journal state
without inventing another metadata generation.

The durable operation journal remains per operation. Its `published` markers
are written as one durability group after the accepted metadata commit, and its
`done` markers as a second group before spool retirement. This preserves replay
proof while avoiding one filesystem barrier per marker. Rename and operation
families whose individual effects cannot yet be proved from the final snapshot
remain singleton batch boundaries.

## Accounting

Authoritative DATA byte accounting is derived state. Clean stores carry an exact checkpoint. If accounting is missing/dirty after an unclean stop, the backend reconciles physical objects before mutation admission becomes authoritative again.

Accounting does not replace physical durability tickets; it only controls capacity admission.

## Deletion

Deleting an unreachable object is less safety-critical than publishing a new reference. A lost deletion after a crash merely leaves garbage. Reachability remains authoritative and later GC retries removal.

Namespace deletion completion is not physical deletion completion. The accepted
metadata mutation and grouped FUSE journal markers establish the namespace
result; physical DATA reclamation occurs later, after grace and retention
fences, through a resumable cursor capped at 64 examined objects per maintenance
slice. Foreground playback, mounted-filesystem or loader activity makes the slice yield.
This bound affects reclamation latency, not namespace publication latency.

For packed objects, deletion is a logical tombstone and dead physical bytes are reclaimed by compaction.

## Crash matrix

| Crash point | Required recovery result |
| --- | --- |
| Before local FUSE admission is durable | operation was not acknowledged |
| Durable FUSE spool/journal, no DATA placement | replay from local durable intent |
| Some provisional DATA placements, no required barriers | replay/deduplicate; metadata must not reference them |
| DATA barriers complete, metadata commit not accepted | durable unreferenced DATA; replay can reuse it |
| Metadata committed, local FUSE completion not recorded | replay observes committed state and completes idempotently |
| Pack tail torn | truncate to last valid committed pack record |
| Pack compaction after replacement install but before old deletion | replacement is live; old packs are duplicate reclaimable bytes |
| DATA accounting dirty | reconcile physical store and establish a new trusted baseline |
| Catalogue control object missing from one replica | fetch/converge from another replica without invalidating committed root |
| Cache missing/corrupt | discard/rebuild cache |
| GC unlink lost | unreachable garbage remains for a later sweep |

For a recovered batch, a crash before the grouped `published` markers causes
idempotent comparison with the accepted namespace; a crash after only a valid
prefix of a marker group preserves the remaining operations for replay. No
operation is reported done merely because another member of its batch was
accepted.

## Memory is part of stability

Metadata mutation must not scale by retaining several complete namespace representations. `MetadataRecord` payloads share immutable backing, record hashes are streamed, compact deltas are applied in place, and ordinary mutation paths avoid building full duplicate garbage indexes.

This is a tested invariant, not an aspiration; see [Validation](../VALIDATION.md).

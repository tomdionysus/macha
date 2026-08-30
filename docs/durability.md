# Durability

## Rule

Macha may publish a logical reference only after the storage class responsible for that reference has satisfied its durability contract.

For DATA, the foreground contract is `dht.min_write_replicas` durable authoritative copies. For namespace/control metadata, the contract is `dht.metadata_min_write_replicas` distinct durable metadata copies on active nodes.

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

The writer ranks preferred owners and deterministic fallbacks. It obtains placements until at least `min_write_replicas` have accepted and become durably covered. Only then may MachaDFS namespace metadata reference the new extents.

`replicas` can be larger than the publication floor. Missing desired copies remain repair debt and are converged by maintenance.

A full or offline preferred owner does not weaken the floor; it changes which eligible candidate supplies the required durable copy.

## Namespace metadata

Every node is metadata-capable. A metadata commit is publishable after the exact immutable commit has been durably stored on at least `dht.metadata_min_write_replicas` distinct active nodes and its acceptance certificate has been durably retained. A receiver stores the commit independently of its current head; disconnected cohorts may therefore preserve different accepted successors of the same ancestor. Ordinary linear commits may use compact deterministic deltas in the history store, while full records remain valid recovery material.

This is a durability floor rather than majority consensus. It deliberately allows arbitrary surviving cohorts of the configured floor size to continue. Consequently disconnected cohorts can produce divergent valid histories. 0.19 preserves those histories and refuses destructive convergence; automatic DAG reconciliation and first-class conflict records are the follow-on layer.

Metadata durability is independent of DATA `dht.replicas` and `dht.min_write_replicas`.

## Catalogue control durability

Catalogue manifest/shard objects are CONTROL. Before namespace metadata can point at a new catalogue manifest:

1. newly referenced artwork DATA must be readable through the normal DATA store;
2. changed catalogue shards must be durable on at least `metadata_min_write_replicas` active nodes;
3. the successor manifest must be durable on at least `metadata_min_write_replicas` active nodes;
4. namespace metadata acceptance publishes the new manifest root.

After publication, maintenance converges the current manifest/shards to all active metadata replicas. A joining or returning node with missing control objects fetches them from another replica. This convergence does not make artwork universal.

Transient metadata/control unavailability causes catalogue scanner work to defer. It does not consume the hint's provider/content failure attempts.

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
wait for DATA durability floor
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

Regression coverage deliberately retains many large `MetadataRecord` copies and enforces a loose RSS ceiling on Linux so a return to payload-deep-copy behavior fails testing rather than reaching the OOM killer in production.

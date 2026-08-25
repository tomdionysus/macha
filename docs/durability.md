# Durability architecture

Macha 0.16.0 has one owner for authoritative object durability: a **DurabilityDomain** for each physical filesystem. Storage, FUSE publication, repair and RPC code express durability requirements; they do not issue authoritative object barriers themselves.

This design separates three kinds of state which have different failure semantics:

| State | Authority | Required crash behaviour |
| --- | --- | --- |
| Accepted but unpublished file data | FUSE spool + operation journal | replay or abandon that inode generation; never invent bytes |
| Published media | immutable authoritative objects + committed metadata | every object referenced by the new manifest is durable on the required replica set before metadata publication |
| Persistent cache | no authority | may be discarded completely after a crash |

Capacity accounting, LRU state and garbage collection are derived/cleanup state. They must recover correctly, but they do not define whether media bytes are published.

## Core invariants

1. Existing published media remain intact and indexable until metadata explicitly replaces or deletes them.
2. A metadata commit must never reference a new authoritative object generation until the configured durable replica requirement for every referenced new extent has been satisfied.
3. The FUSE spool and operation journal are the WAL for accepted writes. They remain available until publication is confirmed and `data_done` is durable.
4. Missing, truncated or checksum-invalid spool data invalidates only the affected inode's dirty generation. A previously published manifest remains authoritative.
5. Cache contents are disposable. Cache loss or corruption is a cache miss, not media loss.
6. A durability acknowledgement is valid only for the exact node process, physical durability domain and backend incarnation which accepted the object.

## Durability domains

`StoragePool` identifies mounted authoritative filesystems by `st_dev`. Configured backend paths on the same filesystem share one `DurabilityDomain`; paths on different filesystems do not.

A domain owns:

- a process-local domain ID;
- a monotonically increasing mutation generation;
- the highest generation known durable;
- a group-commit coordinator thread (`macha-durable`);
- one or more current backend paths which can be opened when a physical barrier is required.

The domain deliberately does **not** keep a permanent directory file descriptor. A configured media backend is allowed to disappear and return. The coordinator opens a representative backend directory only for the duration of a physical barrier.

A mutation receives generation `N` only **after** all of that mutation's write/rename/unlink syscalls have completed. The coordinator captures its cut under the domain mutex and releases that mutex before performing stable-storage I/O. Writers therefore continue admitting generations `N+1...` while the barrier is in progress. The completed barrier advances the durable frontier only through the captured cut, even if the operating system happened to flush later writes as well.

On Linux the physical primitive is `syncfs()` on a representative path for the domain. On platforms without `syncfs`, the same coordinator owns the conservative file/directory `fsync()` fallback. No `LocalStore`, FUSE worker or RPC worker owns authoritative object barriers.

### Tickets

A provisional placement is represented by a ticket:

```text
node process epoch
physical durability domain
mutation generation
backend incarnation
```

Generation zero is reserved for a validated object which was already part of the backend's durable baseline before the current provisional mutation. It therefore needs no new physical barrier, but the domain and backend-incarnation fields are still required.

The process epoch changes on every node process start. The backend incarnation changes every time a configured backend is reopened. A stale ticket from either an old process or an old backend incarnation cannot satisfy a current publication.

## Group commit

A caller does not execute `syncfs()`. It asks the domain to make generation `N` durable and waits for the ticket.

Batchable requests use a short group window (500 ms by default in 0.16.0):

```text
publication A needs generation 100 --+
publication B needs generation 117 --+--> DurabilityDomain --> one syncfs()
publication C needs generation 123 --+                         durable >= 123
```

If the cut is generation 140, all waiters through 140 complete together. A later request for generation 117 returns immediately even if generations 141+ are already dirty.

Strict writes use the same mutation/ticket path but request **immediate** scheduling. Immediate means "do not deliberately wait for the batch window"; it does not create a second durability implementation and can still share a cut with work already admitted.

The public storage API enforces this distinction: `put()` is strict and durable-before-return; `put_deferred()` is the only provisional path and returns the ticket which the caller must retain.

## Spool-backed publication

For FUSE publication, the spool/journal is already durable before distributed object publication starts. The publication sequence is therefore:

```text
durable spool + operation journal
        |
        v
build immutable extents
        |
        v
provisional local/remote placement
        |
        v
collect exact durability tickets
        |
        v
await required replica tickets (group committed)
        |
        v
commit metadata manifest
        |
        v
observe committed metadata / durable data_done
        |
        v
unlink spool
```

The object writes themselves contain no per-extent `fsync()` or directory `fsync()`. A crash before the durability tickets complete simply replays from the still-durable spool. A crash after the object barrier but before metadata commit leaves harmless unreferenced content-addressed objects. A crash after metadata commit but before journal completion is recovered idempotently from metadata plus the journal.

Spool retirement is a best-effort unlink after the durable completion marker. The unlink itself is not synchronously forced: if a crash loses it, startup sees an already-completed stale spool pathname and removes it. Successful operation therefore does not accumulate zero-byte spool files during long uptimes.

## Distributed durability

Deferred object PUT replies carry the accepting node's `(process epoch, domain, generation, backend incarnation)` ticket. The publisher coalesces requirements for the same exact placement to the highest generation it needs.

A durability-barrier RPC is an **await request**, not a request for an RPC worker to execute storage I/O. The receiver submits the generation to its `DurabilityDomain`; the RPC worker waits while `macha-durable` performs any required group commit. Concurrent RPC requests on the same filesystem naturally share one physical barrier.

Remote requests are launched before the publisher blocks on its local domain, aligning group-commit windows across replicas rather than serialising one physical cut per node.

Transport v15 carries these physical-domain tickets. Mixed v14/v15 operation is intentionally rejected; all cluster nodes must be upgraded together. On-disk object, metadata, FUSE journal and spool formats are unchanged from 0.15.x.

## Accounting is derived state

`.macha.accounting` is not a per-publication transaction log in 0.16.0.

On the first authoritative mutation of a clean process/store session, Macha writes and fsyncs one `DIRTY` accounting record. All subsequent puts, removals and publications update the in-memory byte count without toggling accounting CLEAN/DIRTY around each durability generation.

On clean store shutdown:

1. the store waits for its final mutation generation to become durable;
2. it writes the exact byte count as a CLEAN accounting checkpoint;
3. it fsyncs that small checkpoint.

On an unclean process restart, a DIRTY/missing/corrupt accounting record triggers the existing object-tree reconciliation. Reads remain available while mutations wait for exact capacity accounting. Before the reconstructed tree is declared trustworthy, the domain establishes one physical durability baseline. This matters when only the process crashed: Linux may still have writes from the dead process in page cache even though the machine never rebooted.

Thus clean restart remains O(1), while a crash pays one reconciliation scan instead of every normal publication paying synchronous accounting I/O.

Historical `put`/`remove` accounting records remain readable for upgrade compatibility.

## Deletion and garbage collection

Creating a newly published object is safety-critical; deleting an already-unreachable object is not.

Authoritative deletion therefore performs the unlink and registers a domain mutation, but does not force a barrier immediately. If a crash loses the unlink, unreachable garbage survives and a later GC pass removes it again. Metadata/reachability remains the authority, so lazy deletion durability cannot remove published media.

A clean accounting checkpoint still waits through the store's final mutation generation, preventing a clean byte-count checkpoint from getting ahead of its directory state.

## Cache

`PersistentBlockCache` uses the shared encrypted object codec but an explicitly ephemeral `LocalStore` mode. It has no durability domain, no object/directory fsync, and no authoritative accounting WAL. Startup rebuilds useful cache state from the directory tree; malformed or missing entries are discarded. Recovery publication bypasses cache admission so a large crash replay does not evict the established working set merely because it traverses many extents.

## Crash-state matrix

| Crash point | Recovery result |
| --- | --- |
| Before spool/journal admission is durable | write was not acknowledged |
| Spool durable, no object yet | replay from spool |
| Some provisional objects visible, no domain ticket durable | replay; valid objects may deduplicate |
| Domain barrier in progress | metadata not yet publishable; replay remains authoritative |
| Tickets durable, metadata not committed | durable unreferenced objects; replay/deduplicate |
| Metadata committed, `data_done` not durable | journal replay observes already-published generation and completes idempotently |
| `data_done` durable, spool unlink lost | stale completed spool removed later |
| Accounting DIRTY | reconcile object tree, establish durable baseline, checkpoint exact usage |
| Cache missing/corrupt | discard/rebuild cache |
| Deletion unlink lost | unreachable garbage survives until later GC |

## Observability and regression requirements

During sustained spool recovery on Linux, `strace` should show authoritative `syncfs()` calls from `macha-durable`, not from FUSE or RPC data workers. After the first mutation-session accounting marker, `.macha.accounting` must not be fsynced around every publication.

Regression tests cover:

- no per-object/directory fsync for authoritative object data;
- accounting DIRTY once per process/store mutation session;
- crash accounting reconciliation plus physical baseline establishment;
- exact generation cuts and already-covered tickets;
- independent publication group commit;
- physical-domain sharing across multiple backend paths;
- backend-incarnation and process-epoch fencing;
- pre-existing durable object reaffirmation without a new barrier;
- publication barrier before metadata commit;
- lazy deletion durability;
- zero authoritative durability operations for cache contents;
- immediate successful spool unlink after durable publication completion.

The scheduling window is performance policy. These invariants are correctness policy; changing batching policy must not weaken them.

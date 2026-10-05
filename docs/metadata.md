# Metadata replication and reconciliation

## Contract

Every Macha node is a metadata replica, and every replica has the same standing.

A namespace commit is made on the committing node alone: it is accepted, and the write returns, once that node durably holds the commit, its acceptance certificate and its claims on the objects it refers to. No peer is asked first. The commit is then offered to every node present, in order, by a replicator the caller does not wait for; a node that is away, slow or stalled receives it from repair. A read on the committing node sees the commit at once. A read on another node sees it when it arrives there. A catalogue write is made the same way: its shards and manifest are stored on the committing node and sent to the others afterwards. `dht.metadata_write_copies` is the number of copies of a merge commit sought before it is accepted; it is not a condition of acceptance and not a majority derived from cluster membership.

For example, with twelve known nodes and:

```yaml
dht:
  metadata_write_copies: 2
```

any node may continue publishing metadata while the other eleven are unavailable, and any connected pair holds each commit on two nodes. Nodes configured with different values share one namespace.

## Commit, acceptance and heads are separate concepts

A live metadata write is two steps over immutable objects on the committing node: store the commit, then record its acceptance. The replicas present receive both afterwards.

A `MetadataCommit` is an immutable DAG node: it contains a complete state or deterministic delta, its primary parent, and any additional merge parents. A replica may durably store a valid commit regardless of which accepted head it currently exposes. Storing a commit therefore never means "replace your current head".

A commit becomes accepted once the writer has durably stored that exact immutable commit itself and persisted an acceptance certificate naming itself. The replicator then sends each node present the history it lacks and the certificate, so a peer never holds an accepted commit its author lacks. A merge commit, which is made in the background, is offered to the nearest nodes present until `metadata_write_copies` hold it before it is accepted, and its certificate names those nodes. The certificate records where the commit was held when it was accepted; acceptance does **not** disappear merely because one of those nodes later goes offline.

Each replica persists a set of maximal accepted heads. Normally the set contains one commit. A partition may leave several accepted heads. Learning another accepted head adds it to the set rather than replacing an unrelated branch. Once a reconciliation commit descends from those heads, the ancestors cease to be maximal and the head set collapses naturally.

Macha currently assumes authenticated, non-Byzantine cluster peers. Acceptance certificates are durable cluster evidence, not public-key Byzantine quorum certificates.

## Consequence: history may branch

A node cannot distinguish a failed remote node from a network partition, and it accepts writes on its own either way. There is therefore no guarantee of one globally linear history. Macha treats this as an availability property rather than corruption.

When disconnected branches meet, replicas exchange accepted heads and merge them from what the two heads themselves hold (see [Merging two heads](#merging-two-heads)). Non-conflicting namespace changes merge automatically. Identical changes are idempotent. Concurrent changes to one entry become durable first-class conflicts; neither alternative is destroyed until resolution. Resolving a conflict creates another metadata commit. The rest of the namespace remains usable while conflicts exist.

Reconciliation itself is an ordinary immutable commit with multiple parents, accepted like any other. Values changed on both branches that are not namespace entries (policy scalars, conflict records) join deterministically, so they never fail a merge.

A second accepted head whose namespace cannot be fetched from any node present is set aside until the membership changes. Meanwhile reads serve the node's own head, writes extend it and claim release follows it. A head that has been set aside for `maintenance.garbage_grace_ms` is dropped, since whatever held its content is not coming back; the times are kept in `<state_path>/metadata/set-aside.meta`, so a restart does not restart the wait. `diagnostics.metadata.heads_set_aside` in `GET /api/v1/status` counts the heads set aside.

## Authors and provenance

A node writes its mutations under an author id, which is its node id to begin with, and a sequence that starts at 1. The pair (author id, sequence) is a mutation's *dot*. One author's mutations form a chain, so a clock entry for an author means every mutation of that author up to that sequence.

A node keeps its author id, the sequences it has reserved and the highest it has had accepted in `<state_path>/metadata/author.meta`. It takes a new author id and starts again at 1 whenever it cannot show that its chain continues:

- the head it is about to extend lacks a mutation it had accepted;
- its head set was replaced by a recovery from the cache seed or by a re-root;
- its author record is gone while a head shows that it authored before.

Each head carries a clock, the highest sequence it has incorporated per author, and a legacy clock, the clock as it stood when the head's lineage first recorded provenance.

Each namespace entry carries a provenance:

- an identity, set when the file or directory is created and carried by a rename;
- the dot of its last change of content or attributes;
- the dot of when it came to be at its path, by creation or by rename.

An entry written before provenance was recorded has none until it is next changed. The catalogue root has a dot of its own, that of the mutation that last set or cleared it.

## Merging two heads

A merge reads the two heads and nothing else. No common ancestor is located, and no history is needed. A head has *seen* a mutation when its clock covers that mutation's dot.

- **An entry on one head only** is removed if the other head has seen both the mutation that put it at its path and the one that last changed it; otherwise it is kept. So an entry changed on one side and removed on the other keeps the change.
- **An entry that differs between the heads**: the value the other head has already seen loses. If neither head has seen the other's value the change is concurrent: the one with the later modification time is installed and both are kept in a conflict record. The same content written twice is no conflict.
- **A file renamed on one side and changed on the other** is one file, at the new name, with the change. Two renames of one file keep the later name.
- **A directory removed on one side** returns for an entry the other side put under it. A directory with entries under it wins a clash with a file at its path; the file is kept as the other alternative of a conflict record.
- **An entry without provenance** is judged by the legacy clock: it is removed only when the other head has seen every mutation that could have written it. A node that was away with writes no other node saw therefore loses none of them, and files removed elsewhere in that time can reappear from it.
- **The catalogue root** follows its own dot in the same way. Two concurrent roots leave one in place and record the other in a conflict; the catalogue merges the other in while this node's history still holds the two heads' common ancestor, and otherwise the root left in place stands.

A merge mints no dots: the merged clock is the join of the two clocks, and the record is a pure function of the two heads. Every reconciler therefore computes the same record.

## Implementation

The commit-store/acceptance model is implemented directly:

- every active node is eligible to store metadata commits and acceptance evidence;
- live publication uses `put_metadata_commit` followed by `accept_metadata_commit`;
- a receiver validates and stores an immutable commit without comparing it with its current head;
- an accepted commit carries durable evidence of the replicas which stored it at publication time;
- each replica persists encrypted accepted-head certificates separately from its materialised checkpoint;
- each replica keeps an encrypted history of the commits behind its heads, from which a head is replayed and served to a peer;
- each node truncates its own history: once it holds one accepted head and the history has passed 256 records or 64 MiB, that head becomes the root of its history. It asks no other node;
- a head a peer still holds from before a truncation is recognised by its clock: a head whose mutation clock covers another's and goes beyond it has incorporated every mutation the other holds, so the older head collapses without a merge. A head with an empty clock is never recognised this way;
- replicas exchange accepted heads, and a head that is an ancestor of another collapses;
- divergent maximal heads are folded deterministically through two-parent reconciliation commits; this permits an arbitrary number of heads to converge without choosing one branch as authoritative;
- non-conflicting namespace changes merge automatically; concurrent namespace or catalogue-root changes become durable first-class conflicts while the later value remains visible;
- same-generation sibling discovery invalidates metadata caches through an observation epoch rather than relying solely on numeric generation advancement;
- virgin founders construct the same deterministic generation-2 root and publish it through the ordinary immutable-commit path, so any founder may publish it;
- non-destructive DATA repair can continue from an accepted local branch while reconciliation is pending;
- accepted metadata references install durable causal retention claims on the physical DATA/CONTROL copies before publication; retention claims themselves drive bounded repair if a claimed copy is missing or corrupt;
- GC remains active during partitions. Each node releases only locally-held claims for objects absent from its sole accepted head, and only claim dots dominated by that head's causal mutation clock. An unseen/concurrent branch which touched the object carries a newer/incomparable claim dot and therefore remains protected without any global branch survey. An object's bytes go only once this node has itself seen it unreferenced and unclaimed for `maintenance.garbage_grace_ms`.

Validated historical materializations are shared and bounded. An uncached
delta chain is reconstructed once outside the replica-state critical section,
then installed through a short validated cache step; concurrent readers share
that computation. Current, committed, and accepted heads are protected from
eviction, but cache presence is never acceptance authority.

History import, immutable commit storage, and acceptance execute on a dedicated
bounded RPC executor. History transfer is dependency-first and windowed rather
than stop-and-wait. This keeps ping, membership, ordinary control work, and
foreground object service independent of reconstruction, encoding, filesystem
I/O, and metadata durability latency.

Two concurrent catalogue roots are merged item by item against the root their common ancestor held, while this node's history still holds that ancestor; genuine collisions are retained as first-class conflicts.

## What a snapshot carries, and what leaves it

A snapshot's size is a function of the live namespace. That sentence is
[discipline 4](principles-and-laws.md#self-healing-disciplines), and it is a
requirement rather than an observation: read, merge, replay and transfer cost
must scale with the library, not with how long the cluster has been running.

The overwhelming majority of a snapshot is the namespace entries themselves,
and most of that is their extent tables; everything else is meant to be a
rounding error. Three rules keep it that way:

- **Tombstones** (`garbage`) are consumed by the maintenance sweep after
  `maintenance.garbage_grace_ms` and then erased from metadata; the vector is
  kept in canonical ObjectId order (DLT7) so a reconciliation's union is an
  ordinary delta rather than a full snapshot frame.
- **Conflicts** leave the snapshot when decided: a later write to (or
  removal of) the conflicted path, or a later catalogue root, supersedes the
  record at the next commit and at every merge; an operator can also resolve
  one explicitly through `GET/POST /api/v1/manage/metadata/conflicts` (see
  the management guide). A decision is itself a change of the subject, so a
  head that has not seen it does not bring the conflict back. The standing
  count is in `diagnostics.metadata.conflicts`.
- **Merge deltas carry only what changed.** DLT7 gives `merge_parents` and
  `conflicts` independent presence flags, so a conflict-free reconciliation
  is a few hundred bytes of history on each replica.

`macha-metadata-dump <key> history.log heads.meta --stats` prints the
composition of each accepted head (entries, extents, tombstones, conflicts,
and the encoded bytes each accounts for). Read it before concluding that a
snapshot is large for a reason other than the library being large. For a
tree-backed head it reports the record and, given `--objects <path>`, the
tree's shape.

## The namespace tree

A metadata record either inlines the namespace as an entry map or, once the
node has been re-rooted with `macha-namespace-migrate` (see the operations
guide), carries a 32-byte `namespace_root` addressing a content-addressed
Merkle tree keyed by path, and never both. A cluster founds in the inline
form; a tree-backed node stays tree-backed.

- Tree nodes live in the control object store and follow the CONTROL rules:
  a commit may reference a new root once this node holds every tree node it
  wrote, and only the nodes a commit wrote are replicated, sending each peer
  present just the objects it reports missing.
- Node boundaries depend on keys only, so the same entry set yields the same
  root however it was reached, and a value change rewrites one leaf and the
  branches above it. Large extent lists live in their own spine, so a stat
  reads at most the tree's depth and fetches no extent node.
- "Has the namespace changed" is a root comparison.
- History replay rebuilds a tree-backed head locally without asking a peer.
- Reconciliation of two tree-backed heads merges what differs between the
  two trees. When one head is still inline, both are materialised, merged
  path by path and the result re-rooted.
- An entry with provenance sets a bit in its type byte and is followed by
  the provenance; an entry without provenance is encoded as it always was,
  in a tree leaf and in an inline map alike.

## Torrent requests

A snapshot also carries `torrent_requests`: the torrents the cluster has been asked to download, one record per request, with who has claimed it, what the operator wants and how far it has got (see the acquisition guide). SM15 is the SM13 layout with every section present followed by the requests; SM16 is SM14 followed by the requests; DLT9 is DLT8 followed by the requests a mutation rewrote and the tombstones it erased. SM15, SM16 and DLT9 are written only by a head with torrent requests and no legacy clock; a head with a legacy clock writes SM17 or SM18, and DLT10 where a mutation carries a dot.

Reconciliation joins the collection per request instead of recording conflicts: every field has a deterministic merge (cancel is final; a later claim epoch wins; within one epoch a request never moves backwards; removal wins), so the merge is commutative, associative and idempotent and never needs an operator. Removed requests stay as tombstones for seven days so a branch that has not seen the removal cannot bring one back.

SM17, SM18 and DLT10 belong to cluster protocol 23, which every node in the cluster must run.

## Clocks and dots in the encodings

SM17 is SM16 (tree-backed) and SM18 is SM15 (inline map), each followed by the legacy clock and the catalogue root's dot. DLT10 is DLT9 with the dot of each append, the legacy clock when a mutation first sets it, and the catalogue root's dot when a mutation moves the root. A head that carries a legacy clock is written as SM17 or SM18, with or without torrent requests; a delta is written as DLT10 only when it carries one of those values. A conflict record notes which alternative is in place.

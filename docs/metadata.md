# Metadata replication and reconciliation

## Contract

Every Macha node is a metadata replica. There are no permanent metadata voters, witnesses, leaders or designated authorities.

`dht.metadata_min_write_replicas` is the minimum number of **distinct active nodes** which must durably store an immutable metadata commit before that commit may be accepted. It is a durability floor, not a target replica count and not a majority derived from cluster membership.

For example, with twelve known nodes and:

```yaml
dht:
  metadata_min_write_replicas: 2
```

any mutually connected pair may continue publishing metadata while the other ten are unavailable.

## Commit, acceptance and heads are separate concepts

0.19 deliberately does not use a distributed CAS/PREPARE/COMMIT protocol for live metadata writes.

A `MetadataCommit` is an immutable DAG node: it contains a complete state or deterministic delta, its primary parent, and any additional merge parents. A replica may durably store a valid commit regardless of which accepted head it currently exposes. Storing a commit therefore never means "replace your current head".

A commit becomes accepted only after the writer has durably stored that exact immutable commit on at least `metadata_min_write_replicas` distinct nodes. The writer then persists an acceptance certificate naming those durable store witnesses. The certificate records historical publication durability; acceptance does **not** disappear merely because some witness later goes offline.

Each replica persists a set of maximal accepted heads. Normally the set contains one commit. A partition may leave several accepted heads. Learning another accepted head adds it to the set rather than replacing an unrelated branch. Once a reconciliation commit descends from those heads, the ancestors cease to be maximal and the head set collapses naturally.

Macha currently assumes authenticated, non-Byzantine cluster peers. Acceptance certificates are durable cluster evidence, not public-key Byzantine quorum certificates.

## Consequence: history may branch

A node cannot distinguish a failed remote cohort from a network partition. Therefore a fixed write floor smaller than a membership majority cannot guarantee one globally linear history. Macha treats this as an availability property rather than corruption.

When disconnected branches meet, replicas exchange accepted heads and ancestry, locate a common ancestor and perform semantic reconciliation. Non-conflicting namespace changes merge automatically. Identical changes are idempotent. Incompatible changes become durable first-class conflicts; neither alternative is destroyed until resolution. Resolving a conflict creates another metadata commit. The rest of the namespace remains usable while conflicts exist.

Reconciliation itself is an ordinary immutable commit with multiple parents and must satisfy the same metadata write floor before it is accepted.

## 0.19 implementation

0.19 implements the commit-store/acceptance model directly:

- every active node is eligible to store metadata commits and acceptance evidence;
- live publication uses `put_metadata_commit` followed by `accept_metadata_commit`; the old network CAS/PREPARE/COMMIT RPCs are rejected by protocol 20;
- a receiver validates and stores an immutable commit without comparing it with its current head;
- an accepted commit carries durable evidence of the distinct replicas which stored it at publication time;
- each replica persists encrypted accepted-head certificates separately from its materialised checkpoint;
- the 0.18 serialized `metadata_voters` field remains readable only for state compatibility and is cleared by the first policy transition;
- each replica retains encrypted compact ancestry/history across checkpoint compaction;
- accepted heads exchange ancestry, collapse stale ancestor heads and locate common ancestors;
- divergent maximal heads are folded deterministically through two-parent reconciliation commits; this permits an arbitrary number of heads to converge without choosing one branch as authoritative;
- non-conflicting namespace changes merge automatically; incompatible namespace or catalogue-root changes become durable first-class conflicts while the common-ancestor value remains visible;
- same-generation sibling discovery invalidates metadata caches through an observation epoch rather than relying solely on numeric generation advancement;
- virgin founders construct the same deterministic generation-2 root and publish it through the ordinary immutable-commit path; no genesis leader or voter exists;
- non-destructive DATA repair can continue from an accepted local branch while reconciliation is pending;
- accepted metadata references install durable causal retention claims on the physical DATA/CONTROL copies before publication; retention claims themselves drive bounded repair if a claimed copy is missing or corrupt;
- GC remains active during partitions. Each node releases only locally-held claims for objects absent from its sole accepted head, and only claim dots dominated by that head's causal mutation clock. An unseen/concurrent branch which touched the object carries a newer/incomparable claim dot and therefore remains protected without any global branch survey.

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

The remaining metadata work is conflict inspection/resolution APIs and long-term history/retention compaction policy. Catalogue trees already use semantic three-way item merges, with genuine collisions retained as first-class conflicts.

## Migration from 0.18

The old key `dht.metadata_replicas` described a fixed voter count whose effective write requirement was its majority. 0.19 accepts it only as a migration alias and translates it to that former write floor. Thus legacy:

```yaml
dht:
  metadata_replicas: 3
```

is interpreted as:

```yaml
dht:
  metadata_min_write_replicas: 2
```

A pre-0.19 committed checkpoint is imported once as a legacy accepted head. Subsequent 0.19 commits use explicit acceptance certificates. New configurations should use only `metadata_min_write_replicas`.

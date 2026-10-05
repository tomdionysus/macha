# Plan: one local-first path for every read and write

*2026-10-05. Approved by the operator the same day ("implement the plan").*

**Status, 2026-10-05: built. What is left below is measured small or waits on
an API change. 0.90.13 on both nodes.**

Measured on fi-1 (both nodes on the same build, the peer across the WAN):

| | 0.89.1 | 0.90.3 and later |
|---|---|---|
| 12 deletes sent together | 39 to 45 s in all | |
| 30 deletes sent together | | 0.22 s each, 0.3 s the slowest |
| one commit | 2 to 3 s | about 100 ms (tree update 10 to 20 ms) |
| unmatched list, 711 items | 1.9 to 5.6 s | 0.28 s |
| one unmatched item | slow (not timed) | 14 to 25 ms |
| mount's refresh after a commit | whole tree, 0.37 s | what changed |
| unmatched list, 409 hints (0.90.13) | | 0.06 s |
| create or mkdir through the mount, seen by the API (0.90.13) | about 3 s with a viewer active | 0.1 to 0.2 s |

Built:

- **The commit is local** (design 3; 0.90.0, 0.90.2, 0.90.3). A mutation asks
  no peer: it reads this node's own head, claims on this node, writes its
  tree nodes and catalogue shards here, stores and accepts here. A replicator
  delivers the claims, the control objects and the head to every node
  present afterwards. This is finding 2.1, and it gives every caller the
  contract's "acknowledge" and "read": a commit is durable here when it
  returns and this node's view (`MetadataView::local()`) shows it at once.
- **Lookups read the tree** (1.1, 1.6; 0.90.0): no whole-namespace index;
  stat-only scans that seek and stop early.
- **The tree update splices** (2.4, 2.5; 0.90.0) and **the tree diff reads
  only what differs**.
- **The management API's global write lock is gone** (2.2; 0.90.0).
- **The catalogue write is local** (4.3 in part; 0.90.3), and a conflict
  decided for the root in place is removed (0.90.1: a defect of 0.89.0 that
  had both nodes re-merging three catalogues every few seconds).
- **The mount applies a commit's diff** and lists by key range (1.2, 1.3;
  0.90.4), and **the media-id index follows the diff** (1.5; 0.90.4).
- A commit's debug line gives its time by stage (0.90.2).
- **Stage 2, in part**: the maintenance inventory and the release horizon
  follow the tree diff through a census the filesystem keeps (3.1; 0.90.5);
  the availability roll-up is carried across a head change and a tree is
  surveyed once (3.2 in part; 0.90.6, effective from 0.90.7 where a commit's
  claims stop counting as a storage change); media information finds files
  through the media index (3.6 in part; 0.90.6).
- The management API logs why a write was refused or a request failed
  (0.90.7), which covers the provider 503s that left no trace.
- **Stage 3, in part**: a catalogue loaded from its root keeps its keyframe
  indexes, which it had been dropping; a catalogue commit's claims decode
  only changed shards (4.2); a write does not re-read artwork held here
  (0.90.8). The editor's provider calls run on four provider sets instead of
  queueing behind one lock (4.6; 0.90.9). The scanner's namespace check reads
  this node's own head (0.90.9).
- **0.90.11 to 0.90.13**: a catalogue write encodes and claims only the
  shards it touches (4.1); claim release is bounded per call (3.4); artwork
  types and media bindings are indexed once per catalogue snapshot (4.10 in
  part); the mount journals a descriptor and its operation under one barrier
  and reclaims an inode by lookup (1.4, 2.8 in part). Catalogue discovery and
  targeted rematch read the media index (3.6). A match downloads its artwork
  four at a time (4.4) and artwork options are kept ten minutes (4.5). The
  availability survey keeps a memo of what it settled and the path table can
  follow the tree diff (3.2). The mount's namespace commits no longer wait for
  the loader's share while a viewer is active, which held a create about 3 s
  behind playback. The unmatched list checks each file through the media
  index.
- Measured on 0.90.7: ten namespace-only commits, then one maintenance pass
  ending with the availability survey about a minute later (seven round
  trips to the peer, 2,839 tree nodes asked), then idle under 1% of a core.

How this differs from the design as written: the journal has not moved. With
the commit made local, a caller outside FUSE is answered from a durable
local commit rather than from a journal record, and concurrent callers share
commits through the filesystem's group commit. FUSE keeps its own journal in
front of that, for batching. What the journal would still add for other
callers is one fsync shared across more operations; it is not needed for the
contract.

Left, with why:

- **Paging on every list call** (operator, 2026-10-05: "paging on all list
  calls should be available"). `GET /api/v1/catalogue/items` answers 6,764
  items, 9.5 MB, and ignores `limit`. An API change: announced to Core and
  every client before it ships.
- A multi-file match from the web client has not been measured since 0.90.9;
  the deployed web bundle gives up after 8 s. Redeploying it is the Client
  session's job (operator); asked 2026-10-05.
- A testing gap found and closed for what is built: a new test cluster keeps
  its namespace as a map, so service-level tests exercise the tree paths only
  where they migrate first (`tests/test_namespace_migration.cpp`). New
  tree-path work needs its test there.

Further optimisation (measured small; operator 2026-10-05: not now, kept
written down):

- The survey memo and the followed path table apply only while no peer's
  holdings grow. On this cluster gbni-1 imports torrents all the time, so
  every survey still asks from the root (2,834 tree nodes, 7 round trips) and
  the table is walked. A peer reporting what it gained would let the memo
  stand. Left as it is (operator).
- Ingest and data publications commit one operation at a time (2.3): each is
  a local commit of about 100 ms, so a 4 GiB copy spends about 7 s of its copy
  time committing.
- Repair after a commit (3.3) keeps its positions and runs one more pass;
  tombstones mature one at a time (3.5); both are paced maintenance.
- The non-namespace snapshot per commit (2.6, 2.7): a few milliseconds.
- The hint store rewrites its file per change (4.9): 266 KB with 409 hints.

## Why

On 0.89.1 a delete through the management API took 2 to 3 s and ran one at a
time; twelve took 39 to 45 s on an otherwise idle cluster. The unmatched list,
a single unmatched item, a match and an artwork choice were each slow in their
own way. The cause is one fault of scope, repeated:

- The journalled, immediately readable, batch-published write path exists, and
  it is private to the FUSE frontend. The management API, ingest, the torrent
  coordinator and data publications commit one operation at a time, and each
  commit waits on the peer.
- The Merkle tree replaced how the namespace is stored. Most of what reads it,
  including FUSE's own view, still rebuilds whole-namespace state after every
  commit. The catalogue has the same shape.

## Requirements (operator, 2026-10-05)

1. File access and operations are fast, stable and reliable, and work properly
   with the cluster. No local delay on an idle node.
2. Locally the node is always fast and always consistent with itself. The
   cluster may take time to agree; that time is never the caller's.
3. A mutation may be journalled locally, provided a crash or reset cannot lose
   it, and the peer catches up afterwards.
4. All reads and writes use the same mechanism, behind the interface FUSE
   uses.
5. A failed journal write or the loss of the disk itself is acceptable.
   Never a hard failure, a node that cannot recover, or loss of data the
   cluster had.
6. Design it properly across the whole codebase, test it well once, get it
   working, iterate. No soak runs, sanitizer builds or mutation sweeps as
   gates.

## The contract

- **Acknowledge.** A mutation is acknowledged when its journal record is
  durable on this node. Concurrent callers share one durability barrier.
- **Read.** Every read on this node sees every mutation this node has
  acknowledged, whether or not it has been published.
- **Publish.** A background publisher drains the journal in ordered batches
  into tree commits. A commit is accepted locally first; claims on peers and
  replication follow it and are never on a caller's path or under a lock a
  caller needs.
- **Recover.** On start the journal is replayed before the node serves. A
  record that cannot be read ends the replay at that point and is logged; the
  node serves what it has.
- **Degrade.** A journal write that fails fails that operation with an error
  and leaves the node serving. A node whose state disk is lost rejoins and
  converges from its peers, losing only what it had not yet published.
- **Bound.** No request path and no per-commit path does work proportional to
  the namespace or the catalogue. Cost is in the size of the change, the
  depth of the tree, or the size of one directory.

What this changes from the absent-node design: decision 2 there had a commit
seek its second copy for as long as the peer made progress. For namespace
operations the caller no longer waits for that; the publisher does.

## Design

### 1. The namespace layer

One component inside `FileSystem`, owning three things that today are split
between `FuseFrontend` and `FileSystem`:

- **The journal.** The FUSE operation journal, moved under the filesystem
  with its format and replay kept. One append and one fsync per group of
  waiting operations, taken outside the lock that lookups use (2.8).
- **The view.** The committed tree root plus an ordered overlay of
  unpublished operations, keyed by path. A lookup consults the overlay, then
  the tree with a stat-only read. A directory listing is one key range of
  the tree merged with the overlay's entries for that directory. This
  replaces `FileSystem::namespace_index()` (1.1), the FUSE `paths` map of the
  whole namespace and its refresh walk (1.2, 1.3, 1.4), and the macOS
  composed-name alias map, which becomes a per-directory comparison.
- **The publisher.** One worker. It takes what is queued, namespace
  operations and data publications together (2.3), applies them to the tree
  as one commit, and retires the overlay entries the commit covers. Its batch
  bounds are the FUSE worker's (256 operations, 256 KiB).

FUSE keeps its inode table for open files and kernel handles only: entries
exist for what the kernel holds, not for every path.

Callers: the FUSE frontend, the management and files APIs, ingest, the
torrent coordinator and the catalogue scanner all call the same interface.
The management API's global mutex goes (2.2); what it protected, two stale
sessions acting on one unmatched file, is already covered by the media-id
check on each request.

Ingest stops committing checkpoints to the tree: its progress is journalled,
and the tree sees the file when it is complete (parents, create, extents and
rename in one batch).

### 2. The tree

- `update_namespace_tree` splices the changed key ranges into the existing
  spine instead of listing every leaf and rebuilding it (2.4). Import on a
  replica does the same and writes only nodes it does not hold.
- A leaf entry keeps its encoded extent root. Changing one entry does not
  decode or rebuild its neighbours' extent sequences (2.5).
- Lookups, prefix walks and emptiness checks are stat-only and can stop early
  (1.6). The batch working set uses ordered sets (2.9).

### 3. The commit

- `mutate_impl` holds its lock for the local work only: read the head, apply,
  encode, store and accept locally. Claims on peers and `publish_commit` run
  after the lock is released (2.1).
- The non-namespace part of the snapshot is not decoded three times, sorted
  and copied whole per commit (2.6); tombstones are inserted in order; only
  conflicts whose key is in the delta are re-examined (2.7).

### 4. Derived state follows the diff

Each of these is updated from `diff(old root, new root)` when the head moves,
and is not rebuilt:

- the maintenance reachability inventory and the release horizon (3.1);
- the availability roll-up, survey and path table, with the repeated-survey
  defect fixed and the pause callback passed (3.2);
- repair, which works over the extents the diff added and uses index presence
  for objects already settled (3.3);
- the media-id index (1.5);
- claim release, bounded per call and driven by the diff's removals (3.4).

Tombstone maturity is bucketed so one pass erases many in one commit (3.5).

### 5. The catalogue

The same contract: a catalogue write is journalled, readable at once, and
published in batches.

- A commit re-encodes only the shards whose items changed; the claim step
  diffs the two manifests (4.1, 4.2).
- Writers build outside the lock and share a commit (4.3); media profiles and
  keyframe indexes are published in batches (3.6).
- Match uses the path it was given, fetches artwork concurrently, and does
  not re-read what it has just stored (4.4, 4.5).
- Provider calls are made outside `editor_mutex_`; provider failures are
  logged with the upstream message (4.6).
- The unmatched list and item read each file once through the view, and the
  catalogue snapshot carries its media-id bindings and artwork index (4.7,
  4.8, 4.10).
- The hint store has an index by id and appends changes instead of rewriting
  the file under its mutex (4.9).

## Stages

Each stage is built whole, passes the suite once on the laptop and on fi-1,
is committed and pushed, deployed on the operator's order, and measured on
the cluster. What the measurement shows decides the next iteration.

### Stage 1: the local path

Design sections 1, 2 and 3. Before writing: read the three areas the audit
did not cover and this stage touches (the FUSE data path, `MetadataReplica`
beyond import, `metadata_merge.cpp` against an overlay).

Tests that carry the contract:

- a mutation by any caller is visible to a read by any other caller before
  it is published;
- a mutation acknowledged before a kill is present after restart;
- concurrent mutations from several callers publish as one commit;
- a caller is answered while the peer is unreachable or slow;
- a lookup after a commit reads a bounded number of tree nodes (counted, not
  timed);
- a failed journal write fails the operation and the node keeps serving.

Done when, on the cluster: the twelve-delete burst answers each request in
local time (journal fsync, tens of milliseconds) and publishes in one or two
commits; a FUSE `stat` after a commit does not walk the tree; an ingest of
one file makes one or two commits; the suite is green on both machines.

### Stage 2: per-commit background work

Design section 4. Done when a commit on an idle node is followed by work
proportional to the commit: the availability line reports only changed
subtrees, and the maintenance thread's time after a single-file commit does
not depend on library size.

### Stage 3: the catalogue and the management API

Design section 5. Done when a match, an artwork choice, the unmatched list
and an unmatched item answer in the time of their own remote calls and local
reads, with no whole-catalogue or whole-namespace work behind them.

## Formats and protocol

Stage 1 is intended to change no wire format and no stored tree format: the
splice produces the same nodes a full build would. If the leaf encoding has
to change to carry an entry's extent root unopened, that is a format change
and is announced before it is built. The journal moves but keeps its format.
No API route or payload changes in any stage; if one becomes necessary it is
announced to Core and every client first.

## Risks

- **Two views during the move.** While FUSE and the filesystem each keep a
  view, they can disagree. Stage 1 moves all callers in one step for that
  reason; there is no intermediate release with half of them moved.
- **Replay order across callers.** The journal now carries operations from
  several sources. Order within the journal is the order of acknowledgement;
  the publisher preserves it.
- **A merge while operations are unpublished.** A head from a peer arrives
  while the overlay holds local operations. The overlay is re-based on the
  merged root, as FUSE's unconfirmed operations are today.
- **Unbounded journal.** If the publisher cannot commit (the local store is
  failing), the journal grows. Admission is bounded as it is today
  (`max_operation_journal_bytes`), and a full journal refuses new mutations
  with an error rather than blocking.

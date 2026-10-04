# Absent-node tolerance: design overview

*2026-10-05. Agreed by the operator the same day; decisions 1 to 4 taken as recommended, with
stable file identity added to stage B.*

## The rule

Any node may disappear at any time and may never come back. Writes and deletes proceed with the
nodes that are available, without penalty. The system merges and converges on its own. Permanent
loss of a node changes nothing about that.

Stated as a test for every mechanism: **if the node it depends on never returns, nothing is
refused, nothing waits, and nothing grows forever.**

## Where the code stands against the rule

Two audits read the whole dependency (`TODO/absent-node-audit/`). The short form:

- **Authoring** has no voters and no quorum, but it has floors that refuse. With the metadata floor
  at 2 (the default, and the live value) one node down stops every namespace change, and the floor
  cannot be lowered while it is unmet.
- **Everything destructive** (tombstone removal, claim release, control GC, the DATA sweep, history
  truncation) requires every node ever known to be directly reachable. A node is never forgotten
  except by an operator. So one lost node stops all reclamation forever and five kinds of state grow
  without bound.
- **Merging** needs a common ancestor in history. If history was truncated below the fork point, or
  a head's content cannot be fetched, the node holding both heads stops serving converged reads and
  refuses all writes, and that state spreads.
- **A fixed roster** (`metadata_participants`) must be wholly present for the retention baseline.
  Nothing edits it.
- **Loaders** turn a temporary outage into a permanent one: publications park after an hour and
  only an operator unparks them; ingest fails outright on a data floor.

These are not five defects. They are one design habit: safety obtained by waiting for a node. The
replacement has to obtain the same safety from things a node always has: its own state, its own
clock, and whoever is present.

## The design: four principles

### P1. Accept on what is here; replication is debt, not a precondition

A write or delete is accepted once it is durable on the accepting node. Copies on other nodes are
made at once when peers are present and owed when they are not. The replication target stays; it
becomes something the system repairs towards and reports against, never something that refuses.

- `metadata_min_write_replicas` and `min_write_replicas` stop being conditions of acceptance. The
  floor leaves the snapshot, the acceptance certificate and the peer-compatibility check, so nodes
  configured differently still exchange heads.
- A commit, a merge commit, a tree node, a catalogue shard and a retention claim are all valid on
  one node. Claims are recorded locally and pushed to holders that are present; a claim for an
  object whose only holder is away is recorded here and delivered when (if) it returns.
- A slow peer costs a bounded wait, not the 30 s budget: a per-peer state marks a peer that missed
  its deadline as suspect, and suspect peers are skipped until a probe answers.

**Safety:** none is lost relative to today for anything already replicated. **Given up:** a write
accepted by one node alone exists on one node until a peer appears. If that node is destroyed first,
the write is gone. That is the direct price of the rule and cannot be designed away; it is made
visible instead (below).

### P2. A merge needs only the two heads

Today a merge is three-way and so depends on history reaching back to the fork. Replace it with a
merge that reads nothing but the two states:

- Every namespace entry carries the dot (author node, mutation sequence) of its last change.
- Every head already carries `mutation_sequences`: the highest sequence it has incorporated from
  each node. That is a version vector.
- An entry present on one side and missing on the other: if the other side's vector covers the
  entry's dot, that side saw it and deleted it, so it stays deleted. If not, that side never saw
  it, so it is kept.
- An entry present on both sides with different dots: if one side's vector covers the other's dot,
  the newer wins. If neither covers the other, it is a true concurrent edit and becomes a conflict
  record, as today.

This fits the tree-wise merge already built: the diff finds the differing leaves, the dot rule
decides each one.

Consequences:

- No common ancestor, so "no known common ancestor" and "common ancestor cannot be reconstructed"
  cease to exist as states.
- History is no longer needed for correctness. Each node truncates its own history by size, when it
  likes, asking nobody.
- A node returning after any length of absence merges exactly. Deletions made while it was away
  stay deleted; its own work is kept.
- A head whose content cannot be fetched from anyone present is not adopted. The node keeps serving
  and writing on the head it has, and tries again on the next membership change. Divergent heads
  never stop reads or writes.
- The remaining ways a merge throws (policy scalars changed on both sides, conflict records
  diverged) become deterministic joins.

**Safety:** the dot rule is the standard observed-remove construction; it is correct provided each
node's own commits form a chain (to be proven against the code before building). **Given up:** the
conflict record loses its "base" alternative, so the visible value of a concurrently edited path
has to be chosen by rule rather than left at the ancestor (decision 4). The vector keeps one entry
per node that ever authored, about 40 bytes each, for ever.

**Cost:** a namespace format change (a dot per entry) and a migration that stamps existing entries.

Two points on the merge:

- The ancestor may still be used where it is at hand, but only as a shortcut that yields the same
  result as the two-head rule (grafting a subtree one side never touched). It may not change the
  outcome, or a node with the ancestor and a node without would mint different merge commits.
- Rename against a concurrent edit cannot be detected from two heads by path alone. Each file
  therefore gets a stable identity, set at creation and carried by a rename, so a rename on one side
  and an edit on the other meet on the same identity.

### P3. Destruction waits for time, not for nodes

One setting, the **absence horizon H**. Every destructive action is fenced by H measured on the
acting node's own clock, and by nothing else:

- A node deletes an object's bytes once the object has been unreferenced and unclaimed in that
  node's own head for H.
- H runs from the moment the acting node itself first sees the object unreferenced, never from a
  timestamp another node wrote. A node back from a long absence therefore has H after its merge
  before it removes anything.
- A tombstone is dropped once it is older than H and its object is gone.
- Claim release, control GC and claim-row pruning run on the node's own sole head, as their
  arithmetic already allows. The "every known node reachable" fence is removed from all three gates.
- "Stable" means every node directly reachable now holds the head. A node known only by gossip does
  not count.
- The retention baseline becomes a fact each node establishes about its own store. The roster goes.
  (Why the head's live set alone did not already suffice needs to be confirmed in the code first.)

**Safety:** the fence exists to protect an object that an absent node's branch still refers to. H
gives that node H to come back before the bytes go. **Given up, precisely:** if an object is deleted
on one side of a separation and still used on the other (a file deleted here, appended to there),
and the separation lasts longer than H, the merged file may have extents nobody holds any more. The
availability survey reports such a file as incomplete; nothing blocks. Nothing else is lost by a
long absence.

### P4. Membership forgets

A node not directly seen for H is dropped from the known set automatically, on every node
independently. Its torrent claims and pins lapse with it. If it comes back later it is a joiner
with content: P2 merges its namespace exactly, and its objects are simply more copies.

Identity reset remains for the operator who wants it sooner, but nothing depends on it.

## What each audited dependency becomes

| Audit | Today | Under the design |
|---|---|---|
| M1, M4 metadata floor | commit and merge refused below floor | accepted locally; shortfall is debt (P1) |
| M2 floor transition | cannot lower while unmet; mixed floors cannot talk | floor is not a compatibility key (P1) |
| M3 claims barrier | commit fails if a claim cannot reach its floor | claim recorded locally, delivered later (P1) |
| M5, M10, H2 merge preconditions | cluster-wide write wedge | merge needs two heads only; unfetchable head set aside (P2) |
| M6 `repair_once` | gossip-live unreachable node keeps "unstable" | direct reachability only (P3) |
| M7 baseline roster | every roster node must return | per-node fact, no roster (P3) |
| M9 `recovery_required` | node dead until a peer supplies heads | unchanged: a node with no authentic state has nothing to serve. It retries on membership change. |
| H1 history truncation | unanimity of all known nodes | local, by size (P2) |
| N1-N4 known set | never forgotten | forgotten after H (P4) |
| G1-G3 gates | all known reachable | own head, own clock, H (P3) |
| R1 claim release | fenced by gates | runs locally (P3) |
| R2 per-origin state | never pruned | `node_status`, resets, claim origins pruned after H; the vector is kept |
| C1 catalogue commit | 503 below floor | accepted locally (P1) |
| C3 catalogue root conflict | needs all three roots | same rule as P2: unfetchable alternative set aside, retried on membership change |
| G4, M11 sticky remote generation | caches and gates off until restart | a generation advertised only by a node no longer present is dropped with the node's liveness |
| T1 torrent claims and pins | pin to absent node needs operator | pin lapses like a claim |
| Data path | stalled peer holds a write 30 s; append fails if old extents are away | suspect state bounds the wait; append claims locally (P1) |
| FUSE publications | parked after 1 h, operator unparks | never parked for a cause that P1 removes; anything still parked unparks on membership or storage change |
| FUSE namespace queue, `fsync`, `durability_poisoned` | operator skip; no deadline; never reset | each gets a bounded failure and an automatic re-arm on the event that could change the answer |
| Ingest | dies on DATA floor | no floor to die on (P1) |
| Network | 2.5 s dial per backoff for ever; 30-35 s to close a dead route | per-peer down state; dialling stops when the node is forgotten (P4) |

## Making the cost visible

P1 trades refusal for exposure, so the exposure must be reported, per the codes-are-primary rule:
counts of commits and objects held by fewer nodes than the target, and how long the oldest has
waited. These are new status fields, so Core and every client are told before it ships.

## Order of work

- **Stage A, nothing blocks.** P1, P3, P4 and the "unfetchable head is set aside" part of P2. No
  format change. History is kept for H so the existing three-way merge still has its base. A node
  returning after more than H merges without a base in this stage: entries are unioned, so files
  deleted during its absence reappear. Nothing is lost.
- **Stage B, exact merge.** Dots on entries, the two-head merge, local history truncation. Removes
  the reappearance case and the last dependence on history.
- **Stage C, loaders and network.** The automatic re-arm work and per-peer down state.

Stage A is the one that changes what the live cluster does today: with the metadata floor at 2, a
single node down currently stops every namespace change.

## The live cluster

The gates appear to be open now (non-zero `retention.released` on both nodes, `cluster_stable`
events), so es-1 is evidently not in the known set and the baseline evidently completed. That is
read from counters, not from the roster itself; the dump tool does not print either. Under this
design the question stops mattering: es-1 returning is a merge.

## Decisions

1. **H.** One value for everything above. The live `garbage_grace_ms` is 30 days; the default is
   24 hours. Recommendation: 30 days as the default.
2. **Acknowledging a write when peers are healthy.** Wait briefly (about a second) for a second
   copy before acknowledging, or acknowledge on local durability always. Recommendation: wait
   briefly; it costs nothing when the peer is healthy and the suspect state bounds it when not.
3. **Stage B.** Whether to take the format change. Recommendation: yes; it is the only version in
   which history and the fork point cannot wedge anything.
4. **Concurrent edit of one path with no base.** Which value is visible until someone resolves it.
   Recommendation: the later modification time, ties by content hash, with the other kept in the
   conflict record.

## Not yet verified

- That each node's commits form a chain, which the dot rule needs.
- Why the baseline was needed in addition to the head's live set.
- Whether a node that loses its state and returns under the same id can reuse sequence numbers.
  If it can, P2 needs a fresh author id per state lifetime.

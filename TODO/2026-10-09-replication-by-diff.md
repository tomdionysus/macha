# Replication by diff

Status: proposal, 2026-10-09; stage 0 built in 0.90.71. Supersedes the direction note of the same
day. This is the ledger design's stages 5 and 6
([archive/2026-10-08-on-disk-object-ledger.md](archive/2026-10-08-on-disk-object-ledger.md)),
re-scoped: since that design was written, enough of the node's state became
canonical on-disk tries that one mechanism can replace several. Nothing
here is built; each stage ships alone, the suite green on the laptop and
fi-1, deployed to both nodes before the next.

## The rule this proposal rests on

**A node's holdings have one identity, and it is the only signal that they
changed.** Every node keeps, per DATA backend, a `held` trie whose root
hash depends only on the ids it records (0.90.54). A put, a remove, a loss,
a scrub correction, a backend coming online or going away: each changes a
root hash or the set of roots. Nothing else can change what a node holds.
So anything that memoises over holdings keys on that identity and on
nothing else, and anything that compares two nodes' holdings compares those
tries. Today's code instead watches proxies for "my holdings changed" (the
namespace head, the `NodeEvent::storage` count, `losses()`, `indexed()`),
and every list of proxies is incomplete: a backend coming online is missed
now, and the next missed proxy is a matter of time. The rule removes the
class of defect rather than the instance.

## What exists now (survey, 2026-10-09)

Canonical tries on the state device, each an `ObjectTrie` (radix-256 over
the 32-byte id, leaves of at most 128 records, copy-on-write pages, a
journal, a root hash that depends only on the records):

- **held**, one per DATA backend under `ledger/data-<token>` (0.90.54),
  seeded from a walk on a disk's first start under the ledger and kept by
  every put and remove since, checked against the disk a directory at a time
  (0.90.57 to 0.90.60). `HeldLedger` wraps it; `LocalStore::has()` answers
  from it once seeded.
- **claimed**, one per class under `retention/ledger-{data,control}`
  (0.90.55): each object's observed-remove state.
- **referenced** (`ReferenceCounts`, 0.90.63): the extents and tree nodes
  the followed namespace head refers to, a count per object, at a saved
  root; moved root to root by the census's tree diff.
- Frozen `Snapshot`s of any trie, readable from any thread (0.90.62).
- Tombstones as immutable batches named by the head (0.90.67).

What the diff replaces, with its size:

| Piece | Where | Lines | What it does today |
|---|---|---|---|
| Availability survey | `service/availability_service.{hpp,cpp}` | 879 | Rolls this node's holdings up the namespace tree (a `has()` per extent), asks peers `tree_holdings` about subtrees, memoises the answers, publishes `missing_here`, `peer_lacks`, `unavailable`; keeps `availability/holdings.bin` so a restart can answer peers before its presence index fills |
| Holdings roll-up, survey, memo | `ledger/availability.{hpp,cpp}` | 588 | `HoldingsRollup`, `missing_extents`, `extents_peer_lacks`, `survey_availability`, `SurveyMemo`, the `tree_holdings` wire format |
| Repair push and pull | `cluster/distributed_store.cpp` (`repair_step` and its cursors) | about 600 of 2976 | Walks the live set with two cursors, probes peers with `have_object(s)`, `have_valid_objects`, `have_control_objects`, sends at most two extents a step and waits |
| Claim walk | `service/claim_walk.{hpp,cpp}` | 96 | Walks `claimed` for claims this node does not hold; stays (it is already a trie walk, no peer involved) |
| `indexed()` / `held_indexed()` | `storage_pool.cpp:698`, `retention_ledger.hpp:31` | | Gates the roll-up while a presence index fills; one caller; wrong when no backend is open |
| `outside_namespace` | `contract/horizon.hpp` | | The inventory's flat list of catalogue DATA (artwork, media indexes) that repair covers apart from the namespace and `known_present` does not trust |

Measured shape on the cluster: about 940,000 referenced extents, of which
about 203,000 are held by neither online node; 60 of 296 posters held by no
online node (2026-09-27); repair over the WAN moves about 4 MB in 5 s
because one step sends two extents and waits for each.

## The design

### Identity and the held view (built, 0.90.71)

Every `ObjectStore` answers `held_view()`: a frozen view of what it holds,
read from its held ledgers (never `has()`, never a DATA device), and the
identity that names it:

```
struct HeldIdentity {
    Hash256 hash;       // a disk: its held trie's root; the pool: over (token, root) by token
    uint64_t removals;  // ids that stopped being held since start; never returns to an earlier value
    bool complete;      // false while an online disk is still being seeded
};
struct HeldView { HeldIdentity identity; std::function<bool(const ObjectId&)> held; };
```

A ledger hands out its snapshot and its removal count under one lock, so a
view and its identity never disagree. An absent or closed disk contributes
nothing; with no disk online the node holds nothing and knows it
(`complete`, the hash of nothing). `removals` is what lets a roll-up keep
answering peers after gains (it under-claims, which is safe) and stop after
any removal (it would over-claim). Every pool backend keeps a held ledger,
whatever the cache. Building it found that **packed objects were never in
the held ledger** (`has()` answered them from the pack index); they are
now, with every open listing packed objects the ledger lacks.

### The diff

One RPC pair on the speculative frame:

```
trie_diff        { trie, token, prefix, depth, hash, want_records }
trie_diff_reply  { per child slot whose hash differs from the asker's: slot, count, hash
                   | for a leaf: its records }
```

`trie` names which trie (`held`, `claimed` per class, `referenced`
extents); `token` the backend for `held`. The asker sends the 256 child
hashes of its node at `prefix` in one message and gets back the slots that
differ, so one round resolves one interior node; a request may carry several
prefixes (as `tree_holdings` carries several tree nodes), so one round
resolves a batch of differing subtrees. Descent stops where hashes agree.
Two nodes at steady state differ only where placement means them to, so
the common case is a root comparison and a handful of rounds; the worst
case (disjoint tries) is bounded by the leaf count, about 7,400 per 940,000
ids, batched.

`ObjectTrie::Snapshot` grows `children(prefix)` (the 256 `(count, hash)`
slots of the interior node at a prefix) and `records(prefix)` (a leaf's
records). Both read saved nodes through the node cache; neither touches a
DATA device (law 2).

**Which pairs are diffed.** `held` against `held`, per peer backend
against each local backend: the symmetric difference of ids, independent
of the namespace. The asker then applies `referenced` locally. `referenced`
is not diffed against `held` (their records differ in value, so their
hashes never agree); "what this node lacks" is a merge join of the local
`referenced` and `held` snapshots in id order, about 7,400 sequential leaf
reads a side, paced, repeated only when either identity changes.
`referenced` against a peer's `referenced` is a root comparison that says
whether the two follow the same head; it is informational, since held
against held needs no shared head.

**Several backends.** A node's held view is its set of online, seeded
backend tries, each named by its disk's token, and the diff runs per pair.
This keeps each trie the record of one disk, verified against that disk,
with no migration; the ledger design's open question "does `held` record
the backend" is answered by the trie's name rather than a column. The
rounds multiply by the backend count, which is one on both nodes today.
If a node ever runs many backends the pool can maintain a union trie; not
now.

**What falls out.** For each peer that answered: what it holds that this
node lacks (the pull list, placement-ordered), what this node holds that it
lacks (the push list, filtered by `should_own`), and per object `lost`:
referenced, held here by nothing, and held by no peer that answered. A peer
that did not answer makes its share unknown, as `unknown` is today; nothing
is concluded about it.

### Repair on the diff

`repair_step` takes the pull and push lists from the diff instead of
walking the live set. No `have_object` probe is sent: presence is known
exactly, so `known_present` becomes a lookup. Transfer is pipelined: a
window of in-flight gets and puts bounded by the network credit in bytes,
rather than two extents then a wait. Yielding stays at operation boundaries
on the repair share, paced, never stopped.

### Catalogue DATA

The catalogue's DATA (artwork, media indexes) becomes a second source of
`referenced` counts beside the namespace, fed from the catalogue head's
shards by the same change mechanism the census uses. Then the merge join
and the diff cover it: `outside_namespace` goes, lost artwork is in the
pull list when a peer holds it and reported `lost` when none does, and the
API's 1 to 2 s remote probe for a missing poster is answered from the
survey. Re-fetching artwork that is lost everywhere is the catalogue's
business, driven by `lost`; not this proposal. The catalogue plan's stage
10 is this stage.

### Laws and standing rules

- Law 1: the identity is an atomic read; the trie journals are unchanged;
  no commit path waits on a diff.
- Law 2: diffs and merge joins read the state device through the node
  cache; nothing here reads a DATA device. They run on the speculative
  frame and the repair share, paced by viewer and loader activity, never
  gating them (pace, never gate).
- Absent nodes never block: a peer that does not answer is not in the
  diff; `lost` is relative to who answered; a node's own identity needs no
  peer. es-1 returning changes nothing until it is seeded, then its
  identity appears and the diff runs.
- Local always fast: a put or remove journals as today; the identity
  moves as a side effect.
- Codes are primary; no custom headers; the wire format is the trie's own
  `(count, hash)` slots.

## Stages

0. **The holdings identity. Built in 0.90.71.** `held_view()` on every
   store and on `ObjectLedger`, replacing `indexed()`, `losses()` in the
   contract, `held_indexed()` and `held_losses()`. The survey keys its
   roll-up on `(head key, identity hash)` and its answers on `removals`;
   storage events, the loss count and the 30-minute cold wait are gone;
   the roll-up, the survey and the answers read the roll-up's own view. The
   kept roll-up (`availability/holdings.bin`, 0.90.52's stopgap for the
   warm-up) is deleted. While a disk seeds, no roll-up
   is made and peers are answered from the last one (an under-claim), not
   refused. Packed objects are in the held ledger. Closed ACTIVE 1's
   `StoragePool::indexed()` item.
1. **The trie's diff surface.** `Snapshot::children(prefix)` and
   `records(prefix)`, tested exhaustively: canonical regardless of insert
   order, every depth, a leaf at the root, an empty trie, after a
   checkpoint and a rewrite. A local `diff(a, b)` over two snapshots as the
   reference the RPC must match. The merge join `missing(referenced, held)`
   with a pause between leaves.
2. **`trie_diff` over the wire**, gated like `tree_holdings` on the first
   release whose transport accepts it; a peer that refuses it is surveyed
   the old way. The survey publishes `missing_here`, `peer_lacks` and
   `unavailable` from the diff when every extent-hosting peer answers it,
   and from the roll-up otherwise. Two-node tests: identical holdings cost
   one round; one lost copy is found in two; a peer that refuses falls
   back; a peer that fails mid-descent leaves its share unknown.
3. **Repair on the diff.** The pull and push lists come from the survey;
   no presence probe when the lists are from a diff; the pipelined window.
   Measured on the cluster: bytes per second over the WAN against today's
   4 MB in 5 s; probes per step, which should read zero. The walks stay
   for a peer surveyed the old way.
4. **Catalogue DATA as a `referenced` source**, with the catalogue plan's
   stage 10 note folded in here; `outside_namespace` goes.
5. **Retire** once every node runs stage 3: `tree_holdings` and its
   formats, `HoldingsRollup`, `missing_extents`, `extents_peer_lacks`,
   `survey_availability`, `SurveyMemo`, `PeerHoldings`,
   the repair cursors and the `have_*` probes
   in repair, and `ledger/availability.{hpp,cpp}` with them.

Stage 0 is small and stands on its own; it is the structural answer to the
`indexed()` defect and the primitive every later stage builds on.

## Open questions

- Whether `claimed` is diffed at all. The claim walk restores a claimed
  object this node lacks by asking peers; a `claimed` diff would tell it
  which peer to ask. Decide after stage 3 from the walk's probe counts.
- The pipelined window's bound: bytes of credit, as proposed, or also a
  count, so a WAN of small extents cannot hold a thousand puts in flight.
- Whether the API should expose the counts (lacking here, lacking per
  peer, below target, lost) as resources now that they are cheap; they
  were asked for.

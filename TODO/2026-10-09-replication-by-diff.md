# Replication by diff: what the ledger makes possible next

Status: direction, 2026-10-09. A written proposal (with a survey of the
code it replaces) comes before any code, as the ledger's stage 4 had. This
is the ledger design's stage 5 and 6
([archive/2026-10-08-on-disk-object-ledger.md](archive/2026-10-08-on-disk-object-ledger.md)),
re-scoped: since that design was written, enough of the node's state became
canonical on-disk tries that one mechanism can replace several.

## What exists now (0.90.53 to 0.90.70)

Every node keeps, on the state device, in `ObjectTrie`s whose shape and hash
depend only on their records:

- **held**: per store, which objects it holds (0.90.54), checked against the
  disk daily (0.90.57 to 0.90.60);
- **claimed**: per class, each object's observed-remove claim state (0.90.55);
- **referenced**: per class, how many times the followed namespace head
  refers to each object, at a saved root (0.90.63);
- frozen views of any of them, readable from any thread (0.90.62);
- tombstones as immutable batches named by the head (0.90.67).

Two nodes holding the same records have tries with the same root hash, and
any subtree can be compared by its hash.

## What one diff could replace

A request pair, "here is my subtree hash at this prefix; send what differs",
descending only where hashes differ, between two nodes' tries. Driven by it:

- **The availability survey and its holdings roll-up** (`AvailabilityService`,
  `availability/holdings.bin`, the `tree_holdings` messages, `missing_here`,
  `peer_lacks`): what a peer holds that this node lacks, and the reverse, is
  the diff of two `held` tries against `referenced`. The survey's memo, its
  per-peer vectors and its kept roll-up (0.90.52's stopgap) go.
- **Repair's walks** (push and pull cursors over the live set, presence
  probes): repair works the diff's output instead of probing object by
  object, so it can be pipelined (today one step sends at most two extents
  and waits; about 4 MB in 5 s over the WAN).
- **Catalogue DATA** (the catalogue plan's stage 10): if the catalogue's
  objects are a source of the `referenced` counts beside the namespace, they
  are covered by the same diff and repair, and **lost artwork is repaired**
  (60 of 296 posters were held by no online node on 2026-09-27; nothing
  re-fetches them).
- **The counts operators asked for** ("extents this node lacks, each peer
  lacks, below their replication target") fall out of the diff.
- `lost` reported per object: held by nobody reachable.

## Questions the proposal must answer

- Which trie pairs are diffed (held against held, per class; referenced
  against referenced to confirm two nodes follow the same head), and how a
  node with several DATA backends presents one `held` view.
- Whether `held` records the backend (the old open question; proposed yes,
  so a read is one lookup).
- Pacing: the diff is speculative work, paced, never gating (laws 1 to 3).
- The fallback for a peer without the diff, and when it is retired (stage 6).
- The catalogue's objects as a `referenced` source: what changes in the
  catalogue's own retention and in both horizons.
- Absent nodes never block: a diff with a missing peer simply is not run.

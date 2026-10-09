# The on-disk object ledger

Status: stages 1 to 4 shipped (0.90.53 to 0.90.63), 2026-10-09. Stages 2 and 3 of the object ledger
spec ([archive/2026-09-29-object-ledger-spec.md](archive/2026-09-29-object-ledger-spec.md)),
refreshed against the code at 0.90.52 and the operator's answers to its open
questions. Stage 0 (the `ObjectLedger` interface) was built in the object
ledger experiment (0.74.0 to 0.87.1); stages 1 to 3 were not.

## Why now

A restarted node does not know what it holds. `held` is answered by each
store's in-memory presence index, rebuilt by walking the DATA disk at every
start: 1,180 s for 807,076 objects on gbni-1's hard disk (2026-10-08). Until
it is warm, every miss is a disk lookup, a holdings roll-up is not built,
and peers asking what it holds were refused for 17 to 20 minutes after each
restart, which made fi-1's reads of extents no node holds wait 6 to 16 s for
their refusal. 0.90.52 keeps the roll-up on disk so peers are answered at
once; that is a stopgap for one consumer. The ledger makes `held` itself
persistent, exact and fast from the first second, for every consumer.

## Operator decisions (2026-10-08)

1. **Per class**: one ledger for DATA and one for control, as the retention
   store is now. Every question is asked per class; the control set (4,155
   objects on gbni-1) stays wholly cached, so control lookups never touch
   disk; each class has its own journal, so control commits never queue
   behind a DATA import's writes.
2. **Every node carries `referenced` for the whole library**: GC correctness
   must not depend on placement.
3. **The page cache is configurable with a sensible default**, in MB:
   `storage.ledger_cache`, default 64 MiB. At about 64 bytes a record,
   gbni-1's 807,076 DATA objects are about 50 MB: today's library fits
   resident; ten times it keeps the top levels and hot pages.
4. **Built in-tree**, no LMDB or other dependency.
5. **The owner is recomputed lazily**, consistent with repair: a topology
   change bumps an epoch, and an object's owner is recomputed when repair
   next visits it. **Local columns are exact and fast**: `held` and
   `claimed` are journaled before they are acted on.

## What exists today (survey, 2026-10-08)

- **Presence**: `PresenceIndex` (`src/storage/presence_index.*`), three
  `std::set<ObjectId>` and an authoritative flag, about 40 B an object, for
  loose objects only, warmed by walking `objects/xx/yy/*.obj`
  (`local_store.cpp:1808`). Pack contents are a separate
  `std::map<ObjectId, PackEntry>` rebuilt by replaying every pack record in
  the constructor (`local_store.cpp:449`, `:481`). `has()` consults both;
  before warm-up a miss goes to disk. `losses()` counts removals and pruned
  zero-byte files. `StoragePool` adds the backends up.
- **Claims**: `RetentionStore` (`src/storage/retention.*`), two in-memory
  `std::map<ObjectId, ObjectState>`, an observed-remove set per object, no
  size bound. On disk under `state/retention/`: `claims.log` (journal:
  `be32 length | nonce | tag | AES-GCM ciphertext`, fsync per frame, at most
  65,536 ids a frame), `checkpoints/gen-*/` (256 shards by the first id
  byte), `claims.current` (manifest). Replay stops at the first torn frame
  and truncates there; replaying is idempotent. Compaction at a record
  threshold or 64 MiB of journal. `prune_unclaimed` is not journaled; it is
  durable at the next checkpoint.
- **Referenced**: `FileSystem::NamespaceCensus`, in memory: walked once
  after a start, then followed commit to commit by tree diff
  (`filesystem.cpp:2584`, `:2606`). Horizons copy it whole
  (`node_horizon_builder.cpp`).
- **Other persisted overlap**: `state/retention/unreferenced-{data,control}.bin`
  (first time seen unreferenced), `availability/holdings.bin` (0.90.52),
  `availability/last-survey.bin` (per path, not per object), the store's
  accounting slots (`.macha.accounting`), and the pack log.
- **Consumers of `ObjectLedger`**: maintenance (inventory, release horizon,
  release, prune, retained via GC, compaction), the claim walk (`claimed`,
  `held`), availability (`held`, `held_losses`, `held_indexed`), catalogue
  control GC (`retained`), `predicate_query` (tests only).

## Requirements: a restart in seconds, memory bounded by the cache

**Restart.** The ledger is a persisted database. A 20-minute start (gbni-1
today) is unacceptable, and so is any start whose cost grows with the
library. A restart opens the last checkpoint's root page and replays the
journal written since it, and is then ready. The bound is 30 s on a Pi
(operator, 2026-10-08); the expectation is far under it (milliseconds to
open, the replay set by how much journal a checkpoint allows), measured in
stage 1 as an exit criterion. No start ever walks the DATA disk. Pages
come from the state SSD into the cache as lookups touch them; a cold lookup
is one or two SSD page reads.

**Memory.** The ledger's resident memory is its page cache
(`storage.ledger_cache`, 64 MiB default) and fixed buffers, whatever the
number of extents. It removes, stage by stage, the structures that grow with
the library today: the presence index (about 40 B a loose object) and the
pack index (stage 2), the retention store's claim maps (stage 3), the
census's extent vectors and the unreferenced-since map (stage 4). Bounded
by this work, not by the ledger: the availability survey's unavailable and
unknown lists grow with what is missing; the holdings roll-up grows with
tree nodes (about 8,000), not extents; the metadata snapshot cache has its
own 128 MB budget; the catalogue grows with items (about 15 MB). Each
stage's exit includes resident memory measured against a synthetic library
ten times today's.

## The design

**One mechanism, generalised from the retention store.** The ledger is the
retention store grown columns and made disk-resident: its journal frame,
replay and torn-tail truncation, and its id-prefix sharding are reused, not
reimplemented. The retention store's in-memory maps go; its files migrate.

**Structure, per class.** A radix-256 trie keyed by the 32-byte object id.
Interior pages hold 256 child slots, each with the child page's location and
subtree hash; leaf pages hold records sorted by id. Pages are fixed size
(4 KiB proposed), live in one page file per class under `state_path` on the
state SSD (the config refuses a ledger on a DATA backend's device), and are
copy-on-write: a checkpoint writes changed pages to free space and then a
new root, so a crash leaves the last root intact. The trie's shape depends
only on the ids, so two nodes' tries over the same ids hash the same:
canonical, diffable by subtree hash (stage 3).

**Record.** Per object: `held` (this node has the bytes, and in which
backend), `claimed` (the observed-remove set the retention store keeps
now), `referenced` (by this node's head), `unreferenced_since` (folding in
`unreferenced-*.bin`), and `owner_epoch` with the cached owner flag
(lazy). About 64 bytes plus the claim set.

**Journal.** One per class, the retention store's frame. Every change to a
local column is journaled and fsynced before it is acted on: a put records
`held` after its bytes are durable (the store's barrier), a removal records
not-held before the bytes go. Journal writes on the commit path are control
class (law 1): appended in O(delta), never waiting on a page read; pages
are updated by the ledger's own thread from the journal. A checkpoint folds
the journal into pages and starts a new journal, at a record threshold or
journal size, as now.

**Cache.** One page cache per node, `storage.ledger_cache` (64 MiB
default), shared by both classes with the control trie pinned. Clean pages
are evicted least recently used; dirty pages are written by the checkpoint.

**Crash recovery.** Open the last root, replay the journal, done: seconds,
no disk walk. The store remains the truth for bytes. Two disagreements are
possible and both settle without an operator: when the ledger says held and the
file is gone (a disk fault), the read that finds it absent records the
loss; when the file exists and the ledger does not say so (a crash between
the bytes and the journal), a background verification pass, speculative class
and paced, walks the store slowly and records what it finds. That pass is
today's warm-up walk, demoted from a gate to a check.

**What it replaces.** `PresenceIndex` and its warm-up gate; the pack index
replay at start (pack records feed `held` through the journal); the
retention store's in-memory maps; `unreferenced-*.bin`;
`availability/holdings.bin` (the roll-up becomes a view over `held` and the
namespace tree); the census's resident vectors once `referenced` is a
column (stage 2's last step).

**Laws.** 1: commit-path writes are journal appends; control's trie is
pinned; nothing control does reads a DATA page from disk. 2: the ledger does
no I/O on a DATA device; verification and owner recomputes are speculative.
3: a claim batch costs no more than today's journal append. 4: every derived
column rebuilds without a peer; a damaged page file is rebuilt from the
store walk and the claims journal and checkpoint.

## Stages

Each ships alone, with the suite green on the laptop and fi-1.

1. **The trie and its journal, as a primitive**, with exhaustive tests:
   insert, remove, lookup, range, subtree hash canonical regardless of
   order, checkpoint, replay, torn tail, crash at every step of a
   checkpoint, cache eviction under a bound. Measured on fi-1's NVMe with a
   synthetic 10M-record class: cold and warm lookup, journal append, a
   checkpoint, replay time, resident memory at 64 MiB.
2. **`held` in the ledger.** The store records puts and removals through
   the ledger; `has()` answers from it; the warm-up walk becomes the
   background verification. Migration: first start under it runs the walk
   once to seed, in the background: until the seed completes the node
   answers `held` as it does today, and nothing waits on it. Exit: a
   restart of gbni-1 is ready within the 30 s bound with `held_indexed`
   true at once; verification finds nothing to correct. Shipped 0.90.54;
   the verification pass is 0.90.57/58 (a maintenance step paced by time, a
   directory at a time, a pass a day); 0.90.59's seed holds no write up.
3. **Claims in the ledger.** Done in 0.90.55: one trie a class under
   `retention/ledger-{data,control}`, the record an object's observed-remove
   state, `claims.log` the journal; prunes journaled; checkpoint at the old
   thresholds or 8,192 changed objects; the shard files migrate at the first
   start and are removed at the first checkpoint. Separate tries from
   `held`, not a column of one record: the two are written by different
   owners under different locks, and merging them waits on stage 5's need
   for one diffable record. Exit to confirm on the cluster: the migration
   counts match, the restart is inside the bound, resident memory drops.
4. **`referenced` and `unreferenced_since`** as columns, fed by the census's
   tree diff; horizons read the ledger instead of copying vectors.
5. **The diff** (spec stage 3): one RPC pair, subtree-hash descent between
   two nodes' tries; repair pull and push driven by it, the walks kept as
   the fallback for a peer without it. `lost` reported.
6. **Retire** the fallbacks and the replaced files once every node runs 5.

## Stage 4 (2026-10-09)

Shipped: 0.90.62 (frozen trie views) and 0.90.63 (reference counts, horizons
as views). Confirmed on the cluster 2026-10-09: the first start counted the
namespace once (gbni-1: a 4.2 s first inventory, 48 MB of extent counts,
468 KB of node counts); a restart used the saved root and wrote nothing to
the counts (no walk), its first inventory 1.36 s. Resident memory against
0.90.62 still to read after a few hours (0.90.62 grew to about 990 MB in
its first half hour on gbni-1).

**Deferred, by measurement:** `unreferenced_since` on disk. The sighting
files hold only objects waiting out their grace: gbni-1 about 1,600 DATA
and 10,800 control (63 KB and 433 KB), fi-1 about 2,400 and 51,500 (95 KB,
2.1 MB); a few MB in memory at most, growing with garbage, not the library.
On disk it would turn the per-pass sweep into a scan for no measurable gain.
Revisit if a node's sightings reach hundreds of thousands.

The proposal as agreed:

**What exists** (survey, 0.90.61). The referenced set is held five times,
each about 32 B an id: the census (`FileSystem::NamespaceCensus`, extents
once per reference, sorted vectors), `MaintenanceObjects` (a deduplicated
copy), `NamespaceReferences` (another, per release build),
`InventoryHorizon` and `ReleaseHorizon` (`ReferencedSets`, sorted vectors,
copied again with the catalogue's sets). Consumers read them as
`binary_search` over spans: repair (`repair_step`), DATA GC (`gc_step`),
claim release (`release_unreferenced`), control GC (`control_gc_step`),
`query_referenced`. The census is walked whole at every start, then followed
by tree diff. `UnreferencedSince` is two in-memory maps loaded and saved
whole. Cost on gbni-1 (0.90.58 to 0.90.60): an inventory build takes 0.7 to
1.7 s and runs one to four times a minute, a release build about 0.45 s one
to five times a minute: 4 to 5 s of a Pi core every minute copying sets.

**Proposal.**
1. One `referenced` trie a class under `state/ledger/referenced-{data,control}`:
   the record is the number of references from the followed head (extent
   references counted, so a shared extent leaves only when its last
   reference goes). The census follow writes count deltas from the tree
   diff it already computes, journaled, with the root it reflects kept
   beside the trie: a restart resumes from that root by diff, not a walk.
   The census, `MaintenanceObjects.live/namespace_nodes` and
   `NamespaceReferences` go.
2. Horizons become pinned snapshots of those tries: the trie is
   copy-on-write, so a snapshot is a root plus the node-file generation it
   reads; a rewrite waits while an older generation is pinned. A horizon
   keeps today's meaning exactly (immutable, of one head), built in O(1)
   instead of copied. Consumers take a `referenced(id)` lookup and an
   ordered `next()` instead of a span.
3. `unreferenced_since` becomes one trie a class (value: first sighting in
   ms), replacing the two maps and their whole-file saves.
4. The catalogue's own sets (`live`, `control_live`, outside-namespace) stay
   in memory: they grow with catalogue items (7,477), not extents, and are
   unioned in by the snapshot's lookup.

**Decisions needed.**
- Pinned snapshots (exact current semantics, more mechanism) or consumers
  reading the newest followed head (simpler; each consumer's safety under a
  head newer than its horizon must then be argued, and release with several
  accepted heads is the hard case). Recommended: pinned snapshots.
- Whether a release horizon and the inventory may be snapshots at different
  heads of one followed chain (today they are built from different heads:
  `release_head()` and the inventory's generation).

## Open questions

- **Whether `held` records the backend.** A pool with several backends
  answers from `ranked(id)` order today; recording the backend makes a read
  one lookup. Proposed: yes.
- **Page size**: 4 KiB proposed (the SSD's page); measured in stage 1.
- **Journal encryption**: the retention journal is AES-GCM sealed. The
  ledger holds ids and flags, no content; sealing costs CPU on the commit
  path. Proposed: keep it sealed, one definition of the frame.

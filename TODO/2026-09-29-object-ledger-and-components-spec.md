# The object ledger and the component model: stage 0

Status: EXPERIMENTAL, on `experiment/object-ledger`. An evolving design,
written 2026-09-29 from a design conversation with the operator. This
document is complete in itself: it carries forward the high-level design of
the earlier specs on this branch and replaces them as the working plan. No
code until the operator has read it.

**Canonical version.** The most recent version of this spec is canonical.
Earlier specs in the series are historical only:
[the object ledger spec](2026-09-29-object-ledger-spec.md),
[the first stage 0 spec](2026-09-29-object-ledger-stage-0-spec.md) and
[the components stage 0 spec](2026-09-29-object-ledger-stage-0-components-spec.md).
They are a record of how the design evolved and are not read as
requirements.

## Purpose

Macha answers "what is object X to this node?" from several structures that
each hold the whole library in memory and are rebuilt by walking the
namespace: the maintenance reachability sets, the retention release sets and
the retention claim map. None of them scales with the library (at 4 MiB
extents: 64 MB of ids at an 8 TB library, 1 GB at ~130 TB, 12.8 GB at ~400M
extents), each consumer joins them by scan, and nobody can ask a peer "what
do you hold that I lack" short of shipping every id.

The destination is one versioned store of objects and the edges between
them, with metadata and an object ledger as two views over it, kept current
by the writes that change it, on disk behind a bounded cache, and verifiable
across nodes in O(1).

Stage 0 builds none of that storage. It restructures the node so that the
destination can be reached without surgery: components with declared
dependencies, wired and run by a composition root, speaking only through
contracts, with three of those contracts (object store, metadata view,
object ledger) implemented by today's code. That restructure is service-
wide by intent, not only the maintenance path.

## Fidelity and the kill criteria

**Fidelity** means operational and functional fidelity to the user and the
operator, not fidelity to the current code. The node takes the same
decisions about the same objects (what is kept, repaired, released and
deleted), serves viewers, loaders and control as well or better, and obeys
the laws. Code may change freely where that holds. New diagnostics are
allowed; anything that changes what the API sends is announced to Core and
every client before it ships, as always.

**The experiment is killed**, or the offending part withdrawn, if any part
of the new structure, taken as an operational system holistically:

- loses fidelity to the user;
- becomes brittle;
- breaches the Laws and Guidelines (`docs/principles-and-laws.md`), which
  matters most;
- or produces a system that is less performant, less resilient, or more
  brittle than its predecessor.

Doing work twice is acceptable where it buys fidelity and a more efficient,
performant and testable system.

## This is an experiment

An experiment is built to find out whether it works. Its evidence is what
it produces, not a precondition for starting: no step waits on an up-front
proof, baseline or harness. The kill criteria above are the evaluation,
applied to the structure as an operational system at every step: in-process
until the component conversions, then on the cluster. gbni-1 and fi-1 are
the only Macha cluster and a disposable test cluster; dropping the library
costs a great deal of time and is avoided where possible, but not at the
expense of the experiment. `develop` is never modified by the experiment:
on success the whole experiment commit tree merges into it, on failure
development continues from it as it stands.

Judged as a staged design, the argued result is that stage 0 moves the
structure toward the goals while keeping operational and functional
fidelity:

- **Toward the goals.** The contracts are the seams where later stages swap
  implementations: the on-disk store becomes a new implementation of the
  ledger and metadata contracts, checked by their conformance suites, not
  surgery through `Service`; diff-driven repair becomes a new consumer of
  predicate queries that already exist and are tested. The contracts'
  shape (horizons as pinned heads, snapshot handles, `held` as the store's
  own index, `claimed` as the retention store) is the object DAG model's
  interface; only what sits behind it changes.
- **Worth having alone.** Declared blocking makes law 1 checkable and the
  ACTIVE item 2 audit mechanical; `has()` stops waiting on locks and the
  disk; the catalogue's hidden repair becomes an explicit step; injected
  dependencies make the maintenance pass deterministically testable.
- **Fidelity.** Most of stage 0 forwards to the same implementations. The
  places where behaviour can change (an authoritative presence index, a
  derived start order, one locking mechanism everywhere) are each covered
  below.

## The model this prepares for

Not built at stage 0; every contract below is drawn so it can later be
implemented by it.

### Objects refer to objects; heads pin roots

Every edge is between objects. File extents are referenced by the namespace
tree nodes whose entries hold them; tree nodes by their parent nodes;
catalogue shards and media indexes by catalogue manifests. A head (a
metadata generation and hash, with its mutation clock) pins a set of roots:
its namespace root, its catalogue roots, and the roots of its unresolved
conflict alternatives.

- **Content addressing makes it a DAG.** An object's id is the hash of its
  bytes, so no object can refer to itself or to an ancestor. Reference
  counting is exact on a DAG, the model content-addressed stores such as git
  use.
- **Edges from an immutable object never change.** They are recorded once,
  however many heads share the object. A commit creates O(depth) new tree
  nodes along the changed paths, each adding references to its children;
  unpinning a head removes references only for the nodes unique to it. Both
  cost O(delta), never O(library).
- **Two views over one relation.** Metadata is the edges by referrer: the
  tree itself, plus facts about entries (type, size, mode, times, media
  information, holes, conflicts). The object ledger is the same edges by
  child: who refers to X, plus facts about the object that metadata does not
  have (held, claimed, owner). Neither is a superset of the other; both are
  interfaces over one versioned store.
- **Versioning belongs to the store**: heads, branches, clocks, conflicts
  and reconciliation are properties of the edge set and its pins. Today's
  two reachability horizons (the maintenance inventory at the known
  generation, the retention release horizon at the sole accepted head)
  become two pinned heads of one store, and "referenced" becomes "reachable
  from a pinned root", which is exactly the question GC asks.
- **Consequence for scope:** metadata's semantics stay out of scope; its
  storage is in scope, because it becomes one index of the store.

### Replication domains

Each fact belongs to one domain, and every contract says which:

- **Replicated and convergent:** edges, entry facts and heads.
- **Local to the node:** pins (which heads this node holds: its known
  generation, its view of the sole accepted head; two nodes may pin
  different heads at the same moment, which is why GC fences on every known
  node being reachable), `held` (the store's own index) and `owner`
  (placement at the current topology epoch).
- **Local but causally bound to the replicated clock:** `claimed`, the
  retention claims with their observed-remove rule.

### Correct by construction, verifiable in O(1)

With both views as indexes of one store, updated in one transaction, forward
and reverse edges agree on each node without a check. Between nodes, each
head carries a homomorphic multiset hash (LtHash or MuHash) of its edges,
written by the committer; each node's ledger keeps the same sum per pinned
head. Nodes compare hashes of heads, never of their pin sets. A mismatch
refuses GC and rebuilds; a wrong count can delay a deletion, never cause
one. Contracts fix behaviour, not storage: counts, referrer sets or anything
else may sit behind them.

### Performance through caching, not residency

A block cache over the on-disk structure makes random access cheap enough
that the difference from today's resident vectors does not matter. Pain that
cannot be avoided is taken in operations that are not temporally bounded, or
where the slowdown is negligible in normal operation.

### Later stages (unchanged in intent)

- **Counts:** `missing_here`, `held_owned` and column totals in Status and
  telemetry; cluster cohesion from every node's figures.
- **On-disk store:** the object DAG and its indexes on the state device,
  migration from the retention checkpoints, the resident vectors and the
  in-memory claim map gone, namespace walks replaced by delta application.
- **Diff-driven repair:** subtree-hash descent between nodes over a
  canonical index; `lost` reported; walks kept as the fallback.
- **Retire the fallbacks** once every node runs the diff.

## What the code does today

The facts stage 0 must preserve, checked against the tree on 2026-09-29.

### Two reachability horizons

1. **The maintenance inventory**, at the known metadata generation.
   `FileSystem::maintenance_objects_cached` (`src/filesystem/filesystem.cpp`)
   walks the namespace and conflict roots; `Service` adds the catalogue's
   `maintenance_objects()` and keeps (`src/service/service.hpp:103-114`)
   `maintenance_live_`, `maintenance_universal_` (always empty:
   `CatalogueMaintenance::universal` is "intentionally empty in the 0.18
   storage model"), `maintenance_control_live_`, `maintenance_garbage_`,
   `maintenance_stale_garbage_`, `maintenance_inventory_generation_` and
   `maintenance_catalogue_complete_`. Rebuilt (`src/service/service.cpp:1425-1483`)
   when `network_due || garbage_due || gc_due`, and, when only repair is due
   and an inventory exists, only once `metadata_ready_for_dependants`.
   Consumed by repair, tombstone collection, control GC and the DATA sweep.
2. **The retention release horizon**, at the sole accepted head
   (`metadata_->retention_release_view()`), rebuilt when that head's hash
   changes (`src/service/service.cpp:1655-1720`): a second namespace walk,
   the conflict roots, the catalogue's `retention_objects` for every
   catalogue root, and the namespace tree's own nodes, giving
   `retention_release_data_live_`, `retention_release_control_live_`,
   `retention_release_clock_` and `retention_release_complete_`. A horizon
   with an unreadable catalogue root or tree node is discarded and the
   previous one kept. Consumed only by `RetentionStore::release_unreferenced`.

### Three destructive gates

| gate | consumer | condition (`src/service/service.cpp`) |
|---|---|---|
| tombstone collection | `collect_garbage`, `maintain_garbage_metadata` | `garbage_due && !rebuilt_inventory && cluster_gc_stable && maintenance_catalogue_complete_` (1623) |
| control | control retention release, `control_gc_step`, control `prune_unclaimed` | `gc_due && destructive_gc_enabled && maintenance_catalogue_complete_ && maintenance_control_live_ && inventory generation >= known` (1722); release additionally needs a complete horizon |
| DATA | DATA retention release, `gc_step`, DATA `prune_unclaimed` | `gc_due && !rebuilt_inventory && destructive_gc_enabled && maintenance_catalogue_complete_ && maintenance_live_ && inventory generation >= known` (1783); release additionally needs a complete horizon |

`cluster_gc_stable` is every known node reachable and metadata stable;
`destructive_gc_enabled` is that plus a release view with
`retention_baseline_complete`. The skip reason logged at debug (1751-1781)
describes the DATA gate only. The control gate does not test
`!rebuilt_inventory`, although the comment at 1477 says a new inventory is
never used destructively in the pass that built it (open question 1).

### Consumers

| consumer | today |
|---|---|
| inventory build | `Service` 1425-1483 |
| release horizon build | `Service` 1655-1720 |
| repair | `repair_step(budget, ops, maintenance_live_, maintenance_universal_, should_yield, maintenance_inventory_generation_)` (1575) |
| claim walk | `retention_store().next_retained` + store `has` (1516-1547) |
| tombstone collection | 1623-1637 |
| control release and GC | 1722-1741 |
| DATA release and sweep | 1783-1851, with `retained(data, id)` as the sweep's `is_retained` |
| rebalance keep check | `retained(data, id)` (`src/cluster/distributed_store.cpp` 2300, 2764) |
| publication claims | `retain_batch` (`distributed_store.cpp` 847, `src/cluster/cluster.cpp` 1528) |
| peer remove refusal | `retained(data, id)` (`cluster.cpp` 1535) |
| catalogue staging GC | `retained(control, id)` (`src/catalogue/catalogue.cpp` 1872) |
| retention compaction | `compact_if_needed` (`Service` 1907) |

## Part A: foundations

System-wide, and first, because every contract in part B is written in
their terms.

### A1. Operations declare their work class and what they may wait on

What exists: the work classes are system-wide (`FrameType` in
`src/cluster/net.hpp`; the order control > viewer >> loader > speculative in
`docs/principles-and-laws.md`). `DataWorkContext`
(`src/cluster/data_work.hpp`) carries class, quantum, deadline, cancellation
and a no-progress budget for DATA work, and `DataResourceArbiter` grants
leases against it; its constructor refuses `control`. `DurabilityUrgency` is
a separate vocabulary for durability.

What is missing is a declaration on the contract. Whether an operation may
block, and on what, is today found by reading its implementation or by an
incident: 0.73.2's `GET /api/v1/torrents/jobs` took 0.6-1.7 s because the
HTTP thread called `snapshot_view()`, which falls back to `read_record()`,
where `available_snapshot_view()` answers from the cache. Both return the
same type.

Stage 0 adds:

- **A declaration per operation** of what it may wait on: `none`,
  `state_device`, `data_device`, `network`, and **locks**. Locks are the
  usual hidden source: an operation that takes a lock another operation
  holds across I/O waits on that I/O. Rule: an operation declared `none`
  acquires no lock that anything holds across I/O.
- **Mechanical checking of locks.** Mutexes become annotated capabilities
  (Clang thread-safety analysis: `CAPABILITY`, `GUARDED_BY`, `REQUIRES`,
  `ACQUIRE`, `EXCLUDES`, ordering), with locks held across I/O in their own
  capability class. The laptop build (Apple Clang) runs the analysis with
  warnings as errors; GCC on fi-1 ignores the attributes harmlessly.
  `std::mutex` is not annotated by default, so every mutex in the codebase
  moves to annotated wrapper types, and I/O is modelled as a capability of
  its own (negative capabilities) so "held across I/O" is expressible. This
  touches all locking, deliberately: one mechanism everywhere means that if
  it works it works everywhere and if it fails it fails everywhere, and it
  is far easier to test than many local conventions. The analysis is
  intra-procedural and does not see through virtual calls or
  `std::function`, which is the contract boundary: within a component it is
  mechanical; across contracts the declaration is held by review and the
  runtime boundary check below.
- **One work context for every class**, of which `DataWorkContext` is the
  DATA specialisation; control gets the same carrier and DATA arbitration is
  unchanged.
- **How the context travels:** inside `Budget` (A3). Every bounded
  operation already takes a budget, so the context rides with it: no
  thread-local ambient state, which would be a hidden dependency, and no
  extra parameter on every call. A point operation declared `none` needs no
  context; a point operation that may block takes one explicitly.
- **A boundary check:** entering an operation declared to wait on
  `data_device` or `network` under a control-class context fails in tests
  and is recorded in production diagnostics. This makes the ACTIVE item 2
  audit (HTTP paths through `snapshot_view()`) mechanical.

Declarations must be true. Every row of every table in part B is checked
against its implementation before the contract is written, including every
lock it takes.

### A2. Thread safety is part of the contract

Every operation is marked `thread_safe` or `single_owner`, with a strong
preference for inherent thread safety: pure over (component state,
arguments), synchronised inside the component, never relying on the
caller's locking. A `single_owner` operation that remains names its owner.

Two patterns make that the default:

- **Caller-owned cursors (A3).** Today `StoragePool` keeps `gc_cursor_`,
  `RetentionStore` keeps per-class cursors (`cursor_for`,
  `prune_cursor_for`), and `LocalStore::next_object` advances a caller's
  cursor in place. A walk becomes (state, cursor, budget) → (page, next
  cursor).
- **Published immutable state (read-copy-update).** State that is rebuilt
  and read concurrently is published as an immutable snapshot behind an
  atomically swapped owning pointer; a reader takes a handle and keeps it
  for as long as it needs; the old snapshot lives until its last holder
  lets go. Metadata snapshots already work this way; the ledger's horizons
  adopt it (B3).

### A3. One cursor, one budget, one page

No shared abstraction exists. `std::span` is used for contiguous bytes;
cursors are per component (`LocalStore::Cursor`, `StoragePool::Cursor` and
`CursorItem`, the retention store's `std::optional<ObjectId>&` plus
`bool& complete`); budgets are bare `size_t operation_budget` arguments with
component-specific meaning, alongside byte budgets and `should_yield`
callbacks.

Stage 0 adds one set of value types every contract uses:

- `Cursor<Key>`: an opaque position the caller owns. Serialisable where a
  position is persisted (repair's push position survives restarts), not
  required elsewhere. `LocalStore`'s cursor holds a live
  `recursive_directory_iterator` over loose objects, which visits them in
  filesystem order, not id order; it is wrapped as an opaque, movable,
  non-serialisable cursor until the store's walk is redesigned.
- `Budget`: values (operations, bytes, a deadline, any unbounded) plus an
  injected yield source carrying the work context (A1). Yielding depends on
  service-level state today (`repair_share.should_yield`,
  `higher_class_active()`, which includes a peer's viewers), so the yield
  source holds references to the pacer; the budget is therefore not a pure
  value type, and says so. It replaces `size_t` budgets and `should_yield`
  callbacks at the contract boundary.
- `Page<T>`: the items, the next cursor, whether the pass is complete, and
  what stopped it (budget, yield, end, error).

Contiguous results that consumers binary-search are exposed as
`std::span<const T>` over storage owned by a snapshot handle (A2), never as
a bare span, so hot loops cost what they cost now and no refresh can free
what a reader holds.

### A4. No implicit side effects

An operation's contract states every piece of state it changes. Where an
operation changes something its name and contract do not say, it is split,
stated, or the design around it is replaced. Relying on a side effect is an
antipattern and an architectural smell.

Known case: `CatalogueManager::maintenance_objects()` runs the catalogue's
repair while building the maintenance inventory (`src/service/service.cpp:1430`).
It is split into an explicit catalogue repair step and a side-effect-free
inventory read, called in that order at the same point in the pass.

Every operation moved behind a contract is audited for others, and each
finding is recorded here before its contract is written.

### A5. The component model and the composition root

In inversion of control the composition root owns construction, start,
stop and destruction, ordered from the dependency graph. Components are
written to take part: they declare the contracts they require and provide,
receive their dependencies at construction, implement their lifecycle
hooks, report faults upward, and never reach sideways for a collaborator.

What exists:

- For plugins, most of the model: `Subsystem` (idempotent `start()` and
  `stop()`, `attach_fault_sink`), `SubsystemSupervisor` (restart, backoff,
  disable), `SubsystemRegistry` (a component publishes the capability it
  provides) and `SubsystemContext` (`src/subsystem/subsystem.hpp`), whose
  comment describes it as "replacing today's practice of handing out
  whatever concrete internal reference happens to be convenient"; it still
  hands out concrete classes (`NodeRuntime*`, `FileSystem*`).
- For core, none of it. `Service` owns about seventeen components as
  `std::unique_ptr` members (`src/service/service.hpp:59-77`), constructed
  in declaration order. Consumers reach collaborators through `node_`
  (`node_.retention_store()`, `node_.local_store()`, `node_.control_store()`)
  and `Service`'s members: a service locator, so no component declares what
  it uses.
- Cycles are broken by injection after construction:
  `MetadataManager::set_namespace_store(DistributedStore*)` and
  `set_publication_retention`, `MetadataReplica::set_namespace_delta_applier`,
  the cluster's `set_ingest_bridge`, `set_torrent_bridge` and
  `set_torrent_intent_handler`, and the transport's `set_inbound_handler`.

Stage 0:

- **One component model for core and plugins.** `Subsystem`'s lifecycle and
  fault sink are generalised into a component contract that core
  components implement too.
- **A composition root** builds the graph from the declarations and hands
  each component exactly its declared dependencies as contracts.
  Dependencies are constructor parameters typed by contract, so a missing
  dependency does not compile; the graph and its order are assembled at run
  time. `SubsystemContext` becomes a view of the graph.
- **Start and stop order is behaviour.** The order the root derives is
  tested equal to today's construction, start and stop order, or pinned
  where the graph leaves it free; startup waits (`wait_services_ready`) and
  the bounded shutdown are preserved.
- **The graph varies by node.** Components are enabled by configuration
  (torrent, scanner and ingest on fi-1; a different set on gbni-1), so each
  node's graph and order differ. Each real node configuration is tested
  in-process as a fixture, then deployed one node at a time.
- **Cycles are resolved, not hidden.** Each setter above is removed by
  layering (part B puts the object store below metadata, which removes the
  reason for `set_namespace_store`) or becomes a declared port: a contract
  one component provides and another consumes, wired by the root.
- **Supervision policy for core is unchanged at stage 0.** Core components
  use the lifecycle but are not restarted by the supervisor; a core fault
  behaves as today (open question 3).
- **Scope is the whole service.** Every component, not only maintenance,
  moves onto declared dependencies: HTTP APIs, FUSE, playback, torrent,
  ingest, auth and the catalogue included.

## Part B: three contracts over today's implementations

Layering: the object store at the bottom; metadata over it (its tree nodes
are objects in the control store, through `ControlNamespaceNodeStore`); the
ledger over both. Nothing below the ledger depends on it. Consumers take the
narrowest contract they need.

### B1. Object store: bytes by id

Implemented by `StoragePool` for DATA (`NodeRuntime::local_store()`, over
one `LocalStore` per backend) and by a `LocalStore` for control
(`NodeRuntime::control_store()`); the rows below name `LocalStore` where the
behaviour is per backend. Knows nothing of references or claims. Domain:
local.

| operation | today | thread | waits on |
|---|---|---|---|
| `has(id)` | `LocalStore::has` (`src/storage/local_store.cpp:1210`): per-object lock, index lookup, and on a miss `file_size` on the DATA device | safe | today: the per-object lock (held by a `put` of the same id across its write), and `data_device` on a miss. Target: `none` (below) |
| `valid(id)` | `LocalStore::valid`: a full `get` | safe | data_device |
| `get(id)` | `LocalStore::get` | safe | data_device |
| `put(id, bytes, durability)` | `put`, `put_deferred` | safe | data_device |
| `remove(id)`, `remove_if_older_than` | same | safe | data_device |
| `next(cursor, budget) -> Page` | `next_object`: the pack index under the store lock, then a directory walk of loose objects | safe with a caller-owned cursor | data_device |
| `stored_size`, `last_write`, `touch` | same | safe | as each implementation shows, checked per A1 |
| `durability_barrier` | same | safe | data_device |

**Making `has()` wait on nothing.** Publish presence only when a put
completes (insert into the index at the end of the write), and treat the
index as authoritative once `warm_presence_index` has finished. A miss is
then a definite no, the index read cannot observe a half-written object, and
neither the per-object lock nor the disk fallback is needed. Until warm-up
completes, `has()` answers "unknown" or declares that it waits on the device
during warm-up.

Why this is sound, checked 2026-09-29:

- **No production writer bypasses the store.** In `src/`, only `LocalStore`
  touches object paths or `.obj` files. Only tests write the objects
  directory directly, deliberately, to corrupt or remove files
  (`tests/test_invariants.cpp`, `tests/test_storage_metadata.cpp`,
  `tests/test_storage_v18.cpp`, `tests/test_support.hpp`); they become fault
  injection through the contract. After stage 0 a bypass is impossible, not
  merely absent: the store is the only way to reach the bytes.
- **The disk fallback is history, not a second writer.** `present_loose_`
  was added on 2026-09-07 as a cache in front of the original disk check
  (cold checks cost 2.6-16 s per quantum commit on gbni-1), and is filled
  asynchronously at start (`src/storage/local_store.hpp:122-140`); the
  fallback covered both the cache's non-authority and the warm-up window.
- **What remains are physical events, not writers**, and the store owns
  observing them: a backend going offline invalidates its presence entries,
  tied to the existing re-adoption (on 2026-09-29 fi-1's backend dropped off
  USB and its mountpoint emptied; hits already trust the index today, so
  this closes a gap rather than opening one); corruption is scrub's, as
  now; zero-size files from a crashed write are already pruned
  (`pruned_loose_`).

Placement and cross-node repair stay in `DistributedStore`, which becomes a
consumer of this contract and the ledger's.

### B2. Metadata view: edges by referrer

Implemented by `MetadataManager`, `MetadataReplica` and the namespace
primitives. Domain: replicated. Counts are approximate call sites outside
`src/metadata/` from a text search, to be replaced by an exact survey
before the contract is written.

| operation | today | approx. sites | waits on |
|---|---|---|---|
| `current()` | `available_snapshot_view()`: cached, may be empty | 28 | none |
| `converged()` | `snapshot_view()`: cached, else `read_record()` | 17 | network |
| `release_head()` | `retention_release_view()` | 2 | none |
| `entries(view, cursor, budget)` | `for_each_namespace_entry`: an unbounded walk with a callback | 15 | state_device (tree nodes via the object store) |
| `conflict_roots(view)`, `catalogue_roots(view)` | `metadata_conflict_extent_roots`, `metadata_catalogue_root_set` | service | none |
| `mutate(delta_fn)` | `mutate`, `mutate_delta` | ~20 | network |
| commit application | `MetadataReplica` accepting own and replicated commits; `set_namespace_delta_applier` | internal | state_device |
| `status()` | `cluster_status()` | 19 | none |
| `resolve_conflict` | same | 1 | network |
| `repair_step(budget)` | `repair_once()` | several | network |

- Every view is immutable, published per A2, and names its head
  (generation, hash, clock).
- `current()` and `converged()` are A1 applied to one type with two
  behaviours. Which callers may use `converged()` becomes checkable; for
  HTTP handlers that is ACTIVE item 2.
- `entries` with a resumable cursor and a budget is new code over the tree,
  not a pass-through; the callback walk remains for callers that need a
  whole pass, with its declaration.
- Commit application is in the contract because it is where the later
  store's edge writes happen; at stage 0 it is declared and wrapped, not
  changed.
- The catalogue is a second referrer family. At stage 0 it keeps its own
  component and contract (`maintenance_objects`, split per A4, and
  `retention_objects`); folding its edges into the store is a later stage.
- Map-backed snapshots are still supported by the code
  (`require_coherent_namespace`); the cluster runs tree-backed. The DAG
  model needs either their retirement or an adapter (open question 7).

### B3. Object ledger: edges by child

One object, owned by the composition root, replacing the reachability and
claim members above. Domains: `referenced` replicated (derived from pinned
heads), `held` local, `claimed` local and causal.

**Horizons as pinned heads.** At stage 0 there are exactly two, matching
today's structures: `inventory` (known generation; carries catalogue
completeness, the tombstone lists and the `everywhere` ids) and `release`
(sole accepted head; carries its mutation clock and completeness). Each is
an immutable snapshot published per A2:

- `horizon(kind) -> Handle`; the handle owns the snapshot and exposes
  `referenced(class, id)`, `referenced_ids(class) -> std::span<const ObjectId>`,
  `stamp()`, `complete()`, `size(class)`.
- A consumer holds the handle for its pass; a refresh publishes a new
  snapshot and never invalidates one in use.
- `everywhere(id)` is false for every id today, passed to repair as the
  `universal` vector is now (open question 2).

**Refresh is the caller's decision**, made where `Service` makes it today
and under the same conditions; the ledger never refreshes itself on a read.
`refresh_inventory` returns whether it rebuilt; `refresh_release` replaces
the horizon only when the new one is complete. The caller keeps
`rebuilt_inventory` for the pass and the `gc_quiescent_until` follow-up.

**Three gates**, one per row of the gate table, each exactly the condition
in the code, returning `permitted` or the reason; the DATA gate's reason is
today's logged string, checked in the same order. The gates read the
horizons' stamps and the facts the caller passes in (`cluster_gc_stable`,
the release view); they fetch no cluster state themselves.

**Held** forwards to the object store. **Claimed** forwards to
`RetentionStore` with meaning unchanged: `retain`, `retain_batch`,
`retained`, `claims`, `next_retained` (on A3 cursors),
`release_unreferenced`, `prune_unclaimed`, `compact_if_needed`. Journal,
checkpoints and on-disk layout are untouched.

**Predicate queries** for later stages (`to_pull`: owner, not held,
referenced; `to_push_from`; `surplus`; `releasable`; `garbage`;
`missing_here`; `held_owned`) are implemented and tested but called by
nothing at stage 0: routing repair through them would change what it
visits and in what order.

**Toward pinned roots.** Later, the two horizons become two pins on one
store, and "referenced" becomes "reachable from any pinned root". Retention
release today asks it of one head (the release head, with its clock).
Answering it over all pinned heads should be equivalent, because a claim
added for an object live at the newer head is not dominated by the older
head's clock and survives either way; that is an argument, not a proof, and
nothing relies on it until it is proved and tested (open question 6).

Declarations: horizon reads and gates `thread_safe`, waiting on `none`;
refreshes `single_owner` (the maintenance pass), waiting on `state_device`
plus whatever the metadata and catalogue reads they make declare; claim
writes on `state_device` as the retention journal does.

### Consumers after stage 0

| consumer | through |
|---|---|
| inventory build | `refresh_inventory` |
| release horizon build | `refresh_release` |
| repair | `horizon(inventory)`: `referenced_ids(data)`, the `everywhere` ids, `stamp()` |
| claim walk | `next_retained` + `held` |
| tombstone collection | tombstone gate; tombstone lists from the inventory horizon |
| control release and GC | control gate; `horizon(release)` and `horizon(inventory)` control sets |
| DATA release and sweep | DATA gate; the same for data; `retained` as `is_retained` |
| rebalance keep check, peer remove refusal, catalogue staging GC | `retained` |
| publication claims | `retain_batch` |
| retention compaction | `compact_if_needed` |

## Part C: order of work

Each step ships on its own and meets the fidelity bar. Work done twice for
fidelity is expected.

- **0a. Foundations A1-A3.** The work context for all classes, travelling in
  `Budget`; declarations; lock annotations and the Clang analysis in the
  laptop build; the boundary check; `Cursor`, `Budget`, `Page`; the
  snapshot-handle pattern.
- **0b. The component model and composition root (A5)**, with today's
  concrete classes wired as they are now: same order, tested; collaborators
  declared; setters become ports or are listed as remaining.
- **0c. The object store contract (B1)**, walks on A3 cursors, the
  authoritative presence index with offline-backend invalidation, and the
  tests that write object files directly moved to fault injection through
  the contract.
- **0d. The metadata contract (B2)**, the exact call-site survey, the A4
  split of the catalogue's inventory read, the `current()` / `converged()`
  split with every call site mapped one-to-one.
- **0e. The ledger contract (B3)** and every consumer in the table moved
  onto it; `Service`'s reachability members and direct `retention_store()`
  calls gone.
- **0f. Rewire the root on contracts** instead of concrete classes, every
  component in the service; the A4 audit and A2 markings complete.

## Laws

- **Law 1 (control):** no new work on any control path; A1 makes a control
  path that can wait on I/O visible at the contract, and the `has()` change
  removes one such wait from the claim walk and GC.
- **Law 2 (viewer):** no new I/O on any device; the contracts forward to the
  same implementations.
- **Law 3 (loader):** publication's claim path costs one forwarding call
  more.
- **Law 4 (recovers alone):** no new state on disk. The root keeps today's
  start, stop and bounded-shutdown behaviour; no core component is newly
  restarted.

Repair stays paced by `repair_share`, never gated.

## Tests

- The existing suites green at every step on the laptop and on fi-1
  (`macha-tests`, `macha-tests-runtime`, `macha-tests-torrent`).
- **Decision-trace equivalence.** The maintenance pass run against fixed
  inputs (a namespace, a catalogue, a store, claims, a membership) records
  its decisions: objects visited by repair and in what order, gate results,
  claims released, objects deleted. The trace before and after each step
  must be identical. This is the primary evidence of fidelity; the cluster
  only confirms performance.
- A1: a control-class context entering a `data_device`- or
  `network`-declared operation fails in a test build; the Clang analysis
  rejects an operation declared `none` that takes a lock held across I/O.
  Both mutation-proven.
- A2: a horizon handle held across a refresh still reads its own snapshot;
  the old snapshot is freed when the last handle goes.
- A3: each adapted walk visits the same ids in the same order across budget
  boundaries; a persisted cursor resumes after serialise and restore.
- A5: derived start and stop order equals today's for each real node
  configuration, as in-process fixtures; bounded shutdown holds.
- B1: `has()` never reports a partially written object and never touches
  the device after warm-up; a backend going offline and being re-adopted
  leaves presence correct.
- B3: for each gate, a table test over every condition in its row (each
  false in turn denies, all true permits); refresh keeps the previous
  release horizon when the new one is incomplete; the inventory is not
  rebuilt when only repair is due before metadata is ready; each predicate
  query against a hand-built ledger.
- Conformance: each contract has a suite any later implementation must
  pass; the ledger's includes the retention store's existing tests.

## Exit

- All steps merged; suites green on both platforms; decision traces
  identical.
- No component reaches a collaborator through `NodeRuntime` or `Service`;
  every dependency is declared and wired by the root. Anything remaining is
  listed here with its reason.
- On the cluster, one node at a time and spaced for viewers: repair's bytes
  and examined counts in the same range under the same load; resident
  memory and latency the same or better; no law breached.
- None of the kill criteria met.

## Decision log

Operator decisions, dated, with the reasoning given. Later entries
supersede earlier ones where they conflict.

- **2026-09-29. Fidelity is operational and functional**, to the user and
  the operator, not fidelity to the current code.
- **2026-09-29. Kill criteria**: any part that, taken holistically as an
  operational system, loses fidelity to the user, becomes brittle, breaches
  the Laws and Guidelines, or is less performant, less resilient or more
  brittle than its predecessor. Qualitative until T0 instruments the
  existing code, then quantitative.
- **2026-09-29. This is an experiment**: built to find out; evidence is
  what it produces, not a precondition.
- **2026-09-29. The service-wide restructure is the point**, not scope
  creep: every component moves onto declared dependencies.
- **2026-09-29. Doing work twice is acceptable** where it buys fidelity and
  a more efficient, performant and testable system.
- **2026-09-29. One locking mechanism everywhere**: if it works it works
  everywhere, if it fails it fails everywhere, and it is far easier to
  test.
- **2026-09-29. No writer may bypass the store**; after stage 0 it is
  impossible, not merely unused.
- **2026-09-29. `develop` is never modified** by the experiment and is the
  only development stream. Work lands on branches cut from
  `experiment/object-ledger` when accepted. On success the whole commit
  tree merges into `develop`; on failure development continues from
  `develop` as it stands and the experiment's plain-semver version line
  ceases to exist.
- **2026-09-29. The cluster** (gbni-1, fi-1) is the only Macha cluster and a
  disposable test cluster; dropping the library is avoided where possible
  but not at the expense of the experiment. It is first used for
  measurement (T0), then from the component conversions (T5).
- **2026-09-30. No CI.** Every accepted stage and substage is pushed; the
  history is the record.
- **2026-09-30. The standard of proof.** Macha is to be Dijkstra-provable
  and Knuth-legible before mass peer review, and easing independent testing
  is a reason for the experiment. 100% coverage demonstrates that the code
  operates deterministically in all its paths; whether the design is fit
  for its purpose is a separate, higher-order question, which the contracts,
  the laws and the kill criteria answer. Properly structured components
  with stated contracts and 100% coverage, mutation-proven, are the proof
  where complexity allows; the functional and behavioural suite covers the
  rest.
- **2026-09-30. Sanitizers are used but not canonical.**

## Open questions for the operator

1. **The control gate and `rebuilt_inventory`.** Control GC can run against
   an inventory built in the same pass, contrary to the comment at
   `src/service/service.cpp:1477`. Stage 0 preserves the code. Intended, or
   a defect to fix as a step of the experiment, with its own test?
2. **`universal`.** Keep `everywhere` for a future placement rule, or remove
   the plumbing from repair, since nothing sets it?
3. **Supervision of core components.** Once core components share the
   plugin lifecycle, which may the root restart without restarting the
   process?
4. **Where the component model lives.** Generalise `src/subsystem/` in
   place, or a new home with `subsystem/` as the plugin loader over it?
5. **The backlog while `develop` is frozen.** ACTIVE's queue, including
   open P0s (the FUSE recovery test segfault, gbni-1's unexplained heap
   corruption), has nowhere to land while the experiment runs. Do such
   fixes become steps on the experiment branch, or wait until it ends?
6. **Release over pinned roots.** Prove that retention release over "reachable
   from any pinned head" is equivalent to today's release against one head
   before the later store relies on it.
7. **Map-backed snapshots.** Retire them, or keep an adapter into the DAG
   model?
8. **Context in `Budget`.** Confirm that the work context travels in the
   budget, and that point operations which may block take it explicitly.

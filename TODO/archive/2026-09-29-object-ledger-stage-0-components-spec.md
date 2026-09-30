# Object ledger stage 0: components, contracts and three interfaces over today's code

**Historical only.** The canonical spec is the most recent version of
[the object ledger and the component model](../2026-09-29-object-ledger-and-components-spec.md).
This document records how the design evolved and is not read as requirements.

Status: EXPERIMENTAL, on `experiment/object-ledger`. Written 2026-09-29 from a
design conversation with the operator, after checking
[the object ledger spec](2026-09-29-object-ledger-spec.md) and
[the first stage 0 spec](2026-09-29-object-ledger-stage-0-spec.md) against the
code. This is now the plan for stage 0. The first stage 0 spec remains the
detailed account of the ledger facade (horizons, gates, consumer table) and
is incorporated here by reference; where the two differ, this one holds. No
code until the operator has read it.

## The requirement

At the end of stage 0 the node is built from components that declare what
they depend on, are wired and run by a composition root, and talk to each
other only through contracts. Three of those contracts carry the object
ledger design: an object store, a metadata view and an object ledger, each
implemented by today's code.

**Nothing the node does changes.** The same objects are examined in the same
order, the same decisions are taken at the same points, the same log lines
are written, and Status and the API send the same bytes. Nothing is announced
to Core or the clients, because nothing they receive changes. Every sub-stage
below ships on its own under the same rule.

## The model this prepares for

Worked out in conversation; not built at stage 0, but every contract here is
drawn so it can be implemented by it later.

- **One relation underneath.** The namespace and catalogue are a set of
  edges: at head H, referrer R (a path and a position in its extent list, or
  a catalogue root) refers to object X.
- **Two views over it.** Metadata indexes the edges by referrer ("what does
  this path hold, in order") and adds facts about the referrer: type, size,
  mode, times, media information, holes, conflicts. The ledger indexes the
  same edges by object ("who refers to X at H") and adds facts about the
  object: held, claimed, owner. Neither is a superset of the other; both are
  interfaces over one versioned store.
- **Versioning belongs to the store.** Heads, branches, mutation clocks,
  conflicts and reconciliation are properties of the edge set, so each view
  reads at whichever head it names. Today's two reachability horizons (the
  maintenance inventory at the known generation, the retention release
  horizon at the sole accepted head) become two reads of one store.
- **Three replication domains.** Edges and referrer facts are replicated and
  converge. `held` and `owner` are local to each node. `claimed` is local but
  causally bound to the replicated clock (the observed-remove rule). Each
  contract states which domain each of its facts belongs to.
- **Correct by construction, verifiable in O(1).** With both views as
  indexes of one store, updated in one transaction, forward and reverse
  edges agree on each node without a check. Between nodes, each head carries
  a homomorphic multiset hash of its edges (LtHash or MuHash), written by the
  committer; a node's ledger keeps the same sum, and a mismatch refuses GC
  and rebuilds. Contracts fix behaviour, not storage: counts, referrer sets
  or anything else may sit behind them.
- **Performance through caching, not residency.** A block cache over the
  on-disk structure makes random access cheap enough that the difference
  from today's resident vectors does not matter. Any unavoidable cost goes
  where no deadline waits on it, or where normal operation does not notice.

Consequence for scope: the parent spec's "out of scope: metadata replication,
the namespace tree's own format" is the wrong line. Metadata's semantics stay
out of scope; its storage is in scope, because it becomes one index of the
store. Stage 0 touches neither, but draws the metadata contract so that it
can.

## Part A: foundations

These are system-wide and come first, because every contract in part B is
written in their terms.

### A1. Operations declare their work class and what they may block on

What exists:

- The work classes are system-wide: `FrameType` (`src/cluster/net.hpp`:
  control, foreground, read_ahead, speculative, loader) and the order in
  `docs/principles-and-laws.md` (control > viewer >> loader > speculative).
- `DataWorkContext` (`src/cluster/data_work.hpp`) carries class, quantum,
  deadline, cancellation and a no-progress budget, and `DataResourceArbiter`
  grants leases against it. It is DATA-only: its constructor throws on
  `control`.
- `DurabilityUrgency` (`src/storage/durability_domain.hpp`) is a separate
  vocabulary for durability.

What is missing is a declaration on the contract. Today the class is supplied
at run time on the DATA path and is absent everywhere else, so whether an
operation may block, and on what, is found by reading its implementation or
by an incident. 0.73.2 is the example: `GET /api/v1/torrents/jobs` took
0.6-1.7 s because the HTTP thread called `snapshot_view()`, which can survey
peers' accepted heads, where `available_snapshot_view()` answers from memory.
Both return the same type; nothing at the call site says which may block.

Stage 0 adds:

- **A blocking declaration per operation**: `none` (memory, no lock that a
  lower class holds across I/O), `state_device`, `data_device`, `network`, or
  a combination, stated in the contract beside the signature.
- **One work context for every class**, injected, of which
  `DataWorkContext` is the DATA specialisation. Control gets the same
  carrier; the DATA-only arbitration stays exactly as it is.
- **A boundary check** in test and debug builds: an operation declared to
  block on `data_device` or `network`, entered under a control-class context,
  fails. Production records it in the existing diagnostics rather than
  failing. This makes the ACTIVE item 2 audit (HTTP paths through
  `snapshot_view()`) mechanical instead of a code read.

### A2. Thread safety is part of the contract

Every operation is marked `thread_safe` or `single_owner`, with a strong
preference for inherently thread-safe operations: pure over (component
state, arguments) and synchronised inside the component, never relying on
the caller's locking.

The usual obstacle is a cursor held inside a component. Today
`StoragePool` keeps `gc_cursor_`, `RetentionStore` keeps per-class cursors
(`cursor_for`, `prune_cursor_for`), and `LocalStore::next_object` advances a
caller's `Cursor` in place. Under A3 a cursor is a value the caller owns and
passes back, so a walk becomes (state, cursor, budget) → (page, next cursor)
and is thread-safe by construction. A `single_owner` operation that remains
says who the owner is.

### A3. One cursor, one budget, one page

There is no shared abstraction today. `std::span` is used for contiguous
bytes. Cursors are per component: `LocalStore::Cursor`, `StoragePool::Cursor`
and `CursorItem`, the retention store's `std::optional<ObjectId>&` plus
`bool& complete`. Budgets are bare `size_t operation_budget` arguments with
component-specific meaning, alongside byte budgets and `should_yield`
callbacks.

Stage 0 adds, in `src/`, one set of value types every contract uses:

- `Cursor<Key>`: an opaque position the caller owns. A cursor is
  serialisable where its position is persisted today (repair's push
  position survives restarts) and need not be elsewhere. `LocalStore`'s
  cursor holds a live `recursive_directory_iterator` for loose objects,
  which visits them in filesystem order, not id order; replacing it with a
  resumable id position would change the GC sweep's order, so at stage 0 it
  is wrapped as an opaque, movable, non-serialisable cursor.
- `Budget`: operations, bytes and a deadline, any of them unbounded, plus
  the work context from A1 for yielding. It replaces `size_t` budgets and
  `should_yield` callbacks at the contract boundary.
- `Page<T>`: the items, the next cursor, whether the pass is complete, and
  what stopped it (budget, yield, end, error).

Existing walks are adapted to these types without changing iteration order
or per-step bounds; the adapter's tests prove the order is unchanged.
Contiguous results the consumers binary-search today are handed out as
`std::span<const ObjectId>` over the same storage, so hot loops cost what
they cost now.

### A4. No implicit side effects

An operation's contract states every piece of state it changes. Where an
operation changes something its name and contract do not say, it is split,
stated, or the design around it is replaced. Relying on a side effect is not
acceptable in a contract.

Known case: `CatalogueManager::maintenance_objects()` runs the catalogue's
repair while building the maintenance inventory (the comment at
`src/service/service.cpp:1430`). Stage 0 splits it into an explicit catalogue
repair step and a side-effect-free inventory read, called by the owner of
the maintenance pass in that order at the same point, which leaves behaviour
unchanged.

Stage 0 includes an audit of the operations moved behind contracts for any
other case, recorded in this spec before the contract is written.

### A5. The component model and the composition root

In inversion of control the composition root owns construction, start, stop
and destruction, ordered from the dependency graph. Components are written
to take part in that: they declare their dependencies as contracts, receive
them at construction, implement their lifecycle hooks, report faults upward,
and never reach sideways for a collaborator.

What exists:

- For plugins, most of the model: `Subsystem` (idempotent `start()` and
  `stop()`, `attach_fault_sink`), `SubsystemSupervisor` (restart, backoff,
  disable), `SubsystemRegistry` (a component publishes the capability it
  provides) and `SubsystemContext` (`src/subsystem/subsystem.hpp`), whose own
  comment describes it as "replacing today's practice of handing out whatever
  concrete internal reference happens to be convenient". It still hands out
  concrete classes (`NodeRuntime*`, `FileSystem*`), not contracts.
- For core, none of it. `Service` owns about seventeen components as
  `std::unique_ptr` members (`src/service/service.hpp:59-77`) and
  construction order is declaration order. Consumers find collaborators
  through `node_` (`node_.retention_store()`, `node_.local_store()`,
  `node_.control_store()`) and through `Service`'s members: a service
  locator, so no component declares what it uses.
- Cycles are broken today by injection after construction:
  `MetadataManager::set_namespace_store(DistributedStore*)`,
  `MetadataManager::set_publication_retention`,
  `MetadataReplica::set_namespace_delta_applier`, the cluster's
  `set_ingest_bridge` / `set_torrent_bridge` / `set_torrent_intent_handler`,
  and the transport's `set_inbound_handler`.

Stage 0:

- **One component model for core and plugins.** `Subsystem`'s lifecycle and
  fault sink are generalised into a component contract that core components
  implement too. Each component declares the contracts it requires and the
  ones it provides.
- **A composition root** builds the graph from those declarations, orders
  start and stop from it, and hands each component exactly its declared
  dependencies as contracts. `SubsystemContext` becomes a view of that graph
  rather than a list of concrete pointers. Stop order respects the bounded
  shutdown that exists today.
- **Cycles are resolved, not hidden.** Each post-construction setter above
  is either removed by layering (part B puts the object store below
  metadata, which removes the reason for `set_namespace_store`) or becomes a
  declared port: a contract one component provides and another consumes,
  wired by the root like any other dependency.
- **Supervision policy for core is unchanged at stage 0.** Core components
  use the lifecycle but are not restarted by the supervisor; a core fault
  behaves as it does today. Whether and how core components are supervised
  is a later decision (open question 3).

## Part B: three contracts over today's implementations

The layering: the object store at the bottom, metadata over it (its
namespace tree is stored as objects in the control store, through
`ControlNamespaceNodeStore`), the ledger over both. Nothing below the ledger
depends on it. Consumers take the narrowest contract they need.

### B1. Object store: bytes by id

Implemented by `LocalStore` (DATA) and the control store. Knows nothing of
references or claims.

| operation | today | thread | blocks on |
|---|---|---|---|
| `has(id)` | `LocalStore::has`: presence index | safe | none |
| `valid(id)` | `LocalStore::valid` | safe | data_device |
| `get(id)` | `LocalStore::get` | safe | data_device |
| `put(id, bytes, durability)` | `put`, `put_deferred` | safe | data_device |
| `remove(id)`, `remove_if_older_than` | same | safe | data_device |
| `next(cursor, budget) -> Page` | `next_object(Cursor&, bool&)`: the pack index under the store lock, then a directory walk of loose objects | safe with A3's caller-owned cursor | data_device |
| `stored_size`, `last_write`, `touch` | same | safe | none / data_device as today |
| `durability_barrier` | same | safe | data_device |

Placement and repair across nodes stay in `DistributedStore`, which becomes a
consumer of this contract and of the ledger's.

### B2. Metadata view: edges by referrer

Implemented by `MetadataManager` and the namespace primitives. Drawn from
what consumers call today (counts are call sites outside `src/metadata/`):

| operation | today | sites | blocks on |
|---|---|---|---|
| `current()` | `available_snapshot_view()`: cached, may be empty | 28 | none |
| `converged()` | `snapshot_view()`: may read the record and survey peers | 17 | network |
| `release_head()` | `retention_release_view()` | 2 | none |
| `entries(view, cursor, budget)` | `for_each_namespace_entry` | 15 | state_device (tree nodes via the object store) |
| `conflict_roots(view)`, `catalogue_roots(view)` | `metadata_conflict_extent_roots`, `metadata_catalogue_root_set` | service | none |
| `mutate(delta_fn)` | `mutate`, `mutate_delta` | 20 | network |
| `status()` | `cluster_status()` | 19 | none |
| `resolve_conflict` | same | 1 | network |
| `repair_step(budget)` | `repair_once()` | several | network |

Every view is immutable and names its head (generation, hash, clock). The
split between `current()` and `converged()` is A1 applied: the same type
today, two different blocking declarations here. Which callers may use
`converged()` becomes checkable, and the answer for HTTP handlers is ACTIVE
item 2.

The catalogue is a second referrer family over the same edges. At stage 0
it keeps its own component and contract (`maintenance_objects`, split per
A4, and `retention_objects`); folding it into the edge store is a later
stage.

### B3. Object ledger: edges by object

As specified in [the first stage 0 spec](2026-09-29-object-ledger-stage-0-spec.md),
restated in the terms of part A:

- **Two horizons**, `inventory` (known generation) and `release` (sole
  accepted head with its clock and completeness), each stamped with the head
  it is current to; `referenced(horizon, class, id)` and
  `referenced_ids(horizon, class) -> std::span<const ObjectId>`.
- **Refresh is the caller's decision**, made where `Service` makes it today;
  the ledger never refreshes itself on a read.
- **Three gates**, tombstone, control and DATA, each exactly the condition
  in the code, returning `permitted` or the reason. The DATA gate's reason is
  today's logged string.
- **`held`** forwards to the object store; **`claimed`** forwards to
  `RetentionStore` unchanged, with A3 cursors on `next_retained`.
- **`everywhere`**, false for every id today, replaces the `universal`
  vector passed to repair.
- **The parent spec's predicate queries** (`to_pull`, `surplus`, `garbage`
  and the rest) are implemented and tested but called by nothing, because
  routing repair through them would change what it visits and in what order.

Declarations: horizon reads and gates are `thread_safe`, blocking `none`;
refreshes are `single_owner` (the maintenance pass), blocking
`state_device` and whatever the metadata and catalogue reads they make
declare; claim writes block on `state_device` as the retention journal does
today.

## Part C: order of work

Each step ships on its own, passes every suite, and changes no behaviour.

- **0a. Foundations A1 and A3.** The work context for all classes, the
  blocking declaration and its boundary check, `Cursor`, `Budget`, `Page`.
  No consumer moves yet.
- **0b. The component model and composition root (A5)**, with core
  components wired as they are today: same construction order, same
  collaborators, now declared. The post-construction setters become ports or
  are listed as remaining.
- **0c. The object store contract (B1)**, `LocalStore` and the control
  store behind it, walks on A3 cursors.
- **0d. The metadata contract (B2)** and the A4 split of the catalogue's
  inventory read. The `current()` / `converged()` split lands here, with
  every existing call site mapped one-to-one; none changes which it calls.
- **0e. The ledger contract (B3)** and every consumer in the first stage 0
  spec's table moved onto it; the service's reachability members and direct
  `retention_store()` calls gone.
- **0f. The side-effect audit (A4)** closed for everything behind the three
  contracts, and the thread-safety markings (A2) complete.

## Laws

- **Law 1 (control):** no new work on any control path. The A1 boundary
  check makes law 1 violations visible at the contract instead of in
  production latency.
- **Law 2 (viewer):** no new I/O on any device; the three contracts forward
  to the same implementations.
- **Law 3 (loader):** publication's claim path costs one forwarding call
  more.
- **Law 4 (recovers alone):** no new state on disk. The composition root
  keeps today's start and stop behaviour, including bounded shutdown; core
  components are not newly restarted.

Repair stays paced by `repair_share`, never gated.

## Tests

- The existing suites, green at every sub-stage on the laptop and on fi-1
  (`macha-tests`, `macha-tests-runtime`, `macha-tests-torrent`).
- A1: a control-class context entering a `network`- or `data_device`-
  declared operation fails in a test build; mutation-proven.
- A3: each adapted walk visits the same ids in the same order as before,
  across budget boundaries, and a persisted cursor resumes where it
  stopped after a serialise-and-restore.
- A5: the composition root starts and stops the graph in dependency order;
  a component with an undeclared dependency does not compile or does not
  wire; stop respects the shutdown bound.
- B3: the first stage 0 spec's gate tables and horizon tests.
- Conformance: each contract gets a suite any later implementation must
  pass. For the ledger it includes the retention store's existing tests.

## Exit

- All sub-stages merged, suites green on both platforms.
- No consumer reaches a collaborator through `NodeRuntime` or `Service`;
  every dependency is declared and wired by the root. What remains, if
  anything, is listed here with its reason.
- On the cluster, one node at a time and spaced for viewers: the same
  maintenance log lines at the same stages over a day, repair's bytes and
  examined counts in the same range under the same load, resident memory and
  latency unchanged beyond noise.

## Open questions for the operator

1. **The control gate and `rebuilt_inventory`.** Control GC can run against
   an inventory built in the same pass, which the comment at
   `src/service/service.cpp:1477` says never happens. Stage 0 preserves the
   code. Intended, or a defect to fix on `develop` with its own test?
2. **`universal`.** Keep `everywhere` for a future placement rule, or remove
   the plumbing from repair on `develop`, since nothing sets it?
3. **Supervision of core components.** Once core components share the
   plugin lifecycle, should the root restart a faulted core component as the
   supervisor restarts a plugin, and which components are allowed to be
   restarted without restarting the process?
4. **Where the component model lives.** Generalise `src/subsystem/` in place,
   or a new home with `subsystem/` becoming the plugin loader over it?
5. **Order against the queue.** The 2026-09-29 handover puts the ledger
   fifth. 0a and 0b are useful on their own (0a makes ACTIVE item 2
   mechanical); should they move up independently of the rest?

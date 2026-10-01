# T3 (part one): the activity clock on the injected clock

The store's and arbiter's idleness (`NodeRuntime::activity_idle_for`, the
viewer and loader clocks behind `foreground_idle_for` and friends) read the
steady clock directly; T1 left it there ("later work"). The maintenance
pass's deadlines were on the injected `MaintenanceClock`, its idleness on
real time, so no test could step a pass through a busy-then-quiet period
without sleeping.

- `NodeRuntime` takes an `ActivityClock` (a `Clock::time_point()` source) at
  construction; the steady clock when none is given. `Service` passes the
  maintenance clock, so production is unchanged and a test's manual clock
  moves idleness and the pass's deadlines together.
- The trace harness (`TracedNode::advance`) no longer sleeps in real time:
  it steps the manual clock past the quiet window. Traces unchanged: the
  maintenance trace group 1100/1100 over `--repeat 100` (2026-10-01).
- `rpc_cluster/test_a_joining_node_pulls_its_objects_through_maintenance`:
  two nodes hold a file; a third joins on a manual clock; the test settles
  its pass, steps the clock a second, and repeats until the joiner holds
  every extent (bounded in steps). Nobody calls repair. 20/20. Mutation:
  network repair never due in the pass -> killed. This is the claim the
  three-node rewrite (T2 README) had to leave uncovered.
- Suites: macha-tests 679/679, macha-tests-runtime 17/17 (laptop, Clang).

# T3b: the horizons and the gates

- `src/contract/horizon.{hpp,cpp}`: `ReferencedSets` (one sorted,
  duplicate-free set per retention class), `InventoryHorizon` (stamped with
  its generation; catalogue completeness; tombstones split into collectable
  and stale by the data set, each in the order found) and `ReleaseHorizon`
  (stamped with the head's hash and clock).
- `src/contract/gates.{hpp,cpp}`: `tombstone_gate`, `control_gate` and
  `data_gate`, each a function of `PassFacts` and the inventory (or none),
  returning the verdict, the reason (the DATA gate's is the string the pass
  logs, in the same order) and the `name=0|1` conditions the decision trace
  records. Not yet called by the pass (T3d).
- `tests/test_ledger_gates.cpp`: each gate over all 512 combinations of the
  facts times seven inventory states (none; generation behind, equal,
  ahead; catalogue complete or not), against the pass's conditions, reason
  chain and trace strings as written at `3c6c1fd`, copied into the test as
  the reference; the horizons' sets, partition and stamps.
- Mutation (`build/claude-t3-gates-mutate.py`): 21 mutants over the gates'
  conditions, reasons, flags and the horizons' sorting, class selection
  and tombstone partition; 21 killed.
- Suite: macha-tests 686/686 (laptop, Clang).

# T3c: the horizon builder, and the pass on handles and gates

- `src/contract/horizon_builder.hpp`: the builder contract (spec B4):
  `namespace_objects()` (the filesystem's cached walk, whose generation the
  pass reads to decide), `inventory(...)` and `release(head)`, each
  declared as waiting on the state device and the network.
- `src/ledger/node_horizon_builder.{hpp,cpp}`: `build_inventory` and
  `build_release` as functions of what they are given (namespace objects
  and the catalogue's inventory; a head, a tree-node store and a catalogue
  source), under `NodeHorizonBuilder`, which binds them to the node's
  filesystem, catalogue and control store. The inventory build still runs
  the catalogue's repair through `maintenance_objects()` until T4.
- The pass (`src/service/maintenance.*`) no longer takes `FileSystem`; it
  takes the builder. Its eleven reachability members are two handles,
  `inventory_` and `release_`; it builds, keeps a release horizon only
  when complete, and reads the three gates (T3b) for its verdicts, trace
  conditions and the DATA skip reason.
- Repair, the DATA sweep, retention release and control GC take spans
  (`repair_step` and `repair_once` an optional span: no live set is not an
  empty one). Repair tells live sets apart without a generation by their
  data, as it did by the vector's address.
- Equivalence: the seven trace fixtures unchanged, 1100/1100 over
  `--repeat 100`.
- `tests/test_horizon_builder.cpp`: each build against fakes (an in-memory
  namespace tree, a catalogue given as a function, a node store that
  refuses a second read of any node), compared with the pass's release
  build as it stood at `be930c9`, copied in as the reference.
- Mutation (`build/claude-t3c-mutate*.py`): 23 mutants. All 12 in the
  builder killed by its tests; in the pass, 6 killed by the trace fixtures
  and one more ("release rebuilt every pass") by the full suite. Five
  survived both full suites; each was found to be a gap that predates T3c
  (the trace fixtures and suites unchanged, the traces identical, so the
  same mutation of the parent's code survives them too):
  - stale tombstones not erased: no test revived a deleted object. New
    fixture `revived-tombstone` (the same content written again under
    another name; the tombstone is erased at once); 20/20; kills it.
  - repair's live-set identity never changing, and always changing: no
    test changed the set without a generation. New
    `rpc_cluster/test_repair_without_a_generation_tells_live_sets_apart_by_identity`
    runs each pass to its end and reads that step's `complete` (after two
    steps it is false anyway: the pull walk has not finished, which is
    also why the generation test's `!second.complete` proves less than it
    says); 10/10; kills both.
  - control GC on the data set, and the DATA sweep on the control set:
    each consumer checks the retention claims again before it deletes
    (`retained(control, id)` in `control_gc_step`, `retained(data, id)` as
    the sweep's `is_retained`), so a wrong live set deletes nothing a claim
    covers. Redundant guards, as designed; recorded, not tested further.
  - an incomplete release horizon kept: no fixture makes one. The rule
    becomes the ledger's publish refusal in T3d, tested there.
- Suites: macha-tests 693/693, macha-tests-runtime 17/17 (laptop, Clang).

# T3d: the ledger holds and publishes the horizons

- `ObjectLedger` (`src/contract/object_ledger.hpp`) gains `inventory()` and
  `release()` (handles, null before the first publish; waits on nothing)
  and `publish` for each; publishing a release build refuses an incomplete
  one and keeps the previous, so a published release horizon is complete
  by the ledger's invariant (spec B3). `ReleaseBuild` moved beside the
  horizons in `contract/horizon.hpp`.
- `RetentionLedger` moved from `src/storage/` to `src/ledger/` (it now
  sits above metadata) and holds the horizons as `Published` snapshots.
- The pass takes the ledger non-const, reads one handle of each horizon
  per pass, publishes what it builds and then reads the horizon back from
  the ledger: it holds no horizon of its own across passes, and never uses
  one the ledger refused. (A first version kept its own copy after
  publishing; the mutant "the pass keeps its release though refused"
  survived it, because no fixture builds an incomplete release horizon
  while a gate is open. Reading back removes the copy rather than testing
  it.)
- `claim_walk/test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release`:
  publish and read back, a handle held across a publish keeps its
  snapshot, an incomplete release refused before and after a complete one.
- Equivalence: the trace fixtures unchanged, 1200/1200 over `--repeat 100`
  (twelve cases with T3c's `revived-tombstone`) before the read-back
  change, 240/240 over `--repeat 20` after it.
- Mutation (`build/claude-t3d-mutate*.py`): the ledger's three publish
  mutants and the pass's two publish calls, all killed.
- Suites: macha-tests 694/694, macha-tests-runtime 17/17 (laptop, Clang).

# T3e: claims as the storage layer's contract

- `src/contract/claim_store.hpp`: `ClaimStore`, the claims contract
  (`retain`, `retain_batch`, `retained`, `next_retained`, `retained_ids`,
  `claim_objects`, `claims`, `release_unreferenced`, `prune_unclaimed`,
  `compact_if_needed`), with the claim vocabulary (`RetentionClass`,
  `RetentionDot`, `RetentionClock`) moved into it. `RetentionStore`
  implements it, unchanged otherwise.
- `NodeRuntime::retention_store()` is `claims()`, returning the contract.
  The node's RPC handlers (claim writes, delete refusal) and
  `DistributedStore` (publication claims, rebalance keep checks) use it:
  they are the storage layer, below the ledger (spec B3, decision log
  2026-10-02).
- `ObjectLedger` forwards `retained`, `release_unreferenced` (against a
  release horizon), `prune_unclaimed` (present means the ledger holds it)
  and `compact_if_needed`. The pass calls none of `NodeRuntime`'s claims:
  every claim operation it makes goes through the ledger.
- Remaining: the catalogue's staging GC reads `claims()` through
  `NodeRuntime` until T5 (spec, Exit).
- Tests: `claim_walk/test_retention_ledger_forwards_claims_releases_prunes_and_compacts`
  against a real `RetentionStore`. The first mutation run, against the
  trace fixtures alone, left three survivors (`retained` always false,
  prune treating everything as held, compaction never): the fixtures never
  see them -- the sweep's live set covers what `retained` guards, no
  fixture prunes an absent object's tombstone, nothing observes the
  journal. Against the new test all six forwarding mutants are killed
  (`build/claude-t3e-mutate*.py`).

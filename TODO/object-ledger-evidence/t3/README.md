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

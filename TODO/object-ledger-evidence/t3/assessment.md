# T3 assessment: the ledger, widened, against the kill criteria

2026-10-02, laptop evidence (Clang). Still owed before acceptance, with
T2's: the GCC build, the three suites, coverage and the in-suite
microbenchmarks on fi-1.

The plan asks what T3 built, whether it kept fidelity, and what proved
brittle or wrongly shaped before T4 and T5 build on it.

## What T3 built

- T3a: the activity clock injected; the trace harness without real-time
  sleeps; a joining node's pull tested through maintenance.
- T3b: `InventoryHorizon` and `ReleaseHorizon`; the three gates as
  functions of the pass's facts.
- T3c: the horizon builder (spec B4), its own component; the pass on two
  horizon handles and the gates; consumers on spans.
- T3d: the ledger holds and publishes the horizons and refuses an
  incomplete release horizon; the pass reads every horizon back from it.
- T3e: claims as the storage layer's contract (`ClaimStore`, owned by
  `NodeRuntime`); the pass reaches claims only through the ledger.
- T3f: the seven predicates and their queries, called by nothing.

## Fidelity

- Decision traces identical at every substage: 1100/1100 over 100 runs
  (T3c), 1200/1200 (T3d, with the new fixture), 240/240 over 20 runs after
  each later change. The seven fixtures' files did not change; one fixture
  was added (`revived-tombstone`).
- Each gate equals the pass's condition, reason and trace string over
  every input (3,584 combinations each). Each build equals the pass's
  previous build for the same head, against fakes.
- Nothing the API sends changed.

## Performance

- No work was added to a hot path. The pass takes one handle of each
  horizon per pass (a mutex and a pointer copy) and publishes once per
  build. Repair, the sweep, release and control GC take spans of the
  horizon's ids where they took vectors: no copy where there was none
  before. Building and sorting the sets is the same work, moved.
- Memory: one copy of each horizon, as before (the pass's eleven members
  held the same sets).
- Not measured on fi-1 yet; the in-suite benchmarks and the GCC run come
  with acceptance.

## Laws

No scheduling, pacing or gate condition changed: law 1 to law 3 hold as
they held. The pass's thread does the same builds at the same points.

## What proved brittle or wrongly shaped

1. **The spec put building inside the ledger** (`refresh_inventory`,
   `refresh_release`). A ledger that builds waits on the network and
   depends on the catalogue, which made its reads' declarations a union of
   lock-free and blocking, and made a cycle with every component the
   catalogue's claims reach. Corrected by the operator's decision: four
   roles, the builder its own component (B4).
2. **The spec's layering contradicted its own consumer table.** It moved
   `NodeRuntime`'s RPC handlers onto a ledger that sits above
   `NodeRuntime`. Corrected: claims are the storage layer's contract
   (`ClaimStore`), the ledger forwards to it (decision log 2026-10-02).
3. **The predicates were named, not defined** (one of seven). Defined in
   B3 for the review they need before any stage calls them. `garbage`
   cannot be queried until the stores walk what they hold in id order.
4. **A copy is a second source of truth.** The first T3d kept the pass's
   own handle after publishing, so it could use a release horizon the
   ledger had refused; the mutant that proved it survived. The pass now
   reads back from the ledger. Every T5 conversion that publishes should
   do the same.
5. **The trace fixtures are blind to whatever they never drive.** Of the
   pass's mutants, the fixtures missed: stale tombstones never erased,
   repair's live-set identity, the ledger's forwarding of `retained`,
   prune and compaction, and two consumers given the wrong set (each
   guarded again by a claims check). Each was a gap before T3; each now
   has a test except the doubly guarded two, recorded as such. A generation
   test (`!second.complete`) proved less than it said. The fixtures are the
   fidelity evidence for decisions they exercise, not for the rest.
6. **Header mutants are slow** (`predicates.hpp` is included through
   `distributed_store.hpp`: about five minutes a rebuild). A contract
   header should be included by what implements it, not carried through a
   widely included one; to fix when `DistributedStore` is converted (T5).

## Not brittle

The horizons, the gates as pure functions, `Published` (the read-back
pattern on it), the builder's separation from the pass, spans at the
consumer boundary, and `ClaimStore` came out as designed, each tested as a
primitive and mutation-proven: 21 + 23 (T3c) + 5 (T3d) + 6 (T3e) + 21
(T3f) mutants, all killed or explained.

## Remaining at stage 0

- The catalogue's staging GC reads claims through `NodeRuntime` until T5
  (spec, Exit).
- `NodeRuntime` is still a locator for the pass (T2 point 4); T3 removed
  the claims and the filesystem from what the pass reaches through it.
- The builder's inventory build still runs the catalogue's repair (A4's
  known case, T4) -- and that repair commits catalogue-root
  reconciliations, a likely part of the catalogue conflict loop (ACTIVE).

## Verdict

No kill criterion met. Before T4: the fi-1 GCC run with T2's; carry points
4 and 6 into the T5 conversions; T4 splits the catalogue's repair out of
the inventory build, which the conflict loop now gives a second reason to
watch.

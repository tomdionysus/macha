# T4 assessment: the metadata contract, against the kill criteria

2026-10-02, laptop evidence (Clang). Owed before acceptance: the GCC build,
the three suites and coverage on fi-1, and `entries` checked against a copy
of each live node's namespace.

## What T4 built

- T4a: `MetadataView` (reads, commits, status, diagnostics, and from T4d
  `entries`) and the component's upkeep as a separate contract
  (`MetadataMaintenance`, the pass only); every holder but the owner on the
  contract; the survey done by the compiler.
- T4b: the catalogue's hidden repair split out of the inventory read: head,
  repair, read, in order at the same point.
- T4c: `entries`, the resumable, budgeted namespace walk.
- T4d: the namespace store at construction (`set_namespace_store` gone);
  commit application installed there, declared; `entries` on the contract.

## Fidelity

- Decision traces identical: 1200/1200 over 100 runs after T4b, 240/240
  over 20 after each other substage; no fixture changed.
- Every call site moved onto the contract calls the method it called
  before: each contract operation is a one-line call of the manager's
  existing one.
- `entries` equals the callback walk on every tree shape and bound tested
  and on map-backed snapshots; on a live node through the contract.
- Two changes of timing, both earlier, never later: the catalogue's head is
  captured exactly where it was (the split keeps the order), and the
  replica's commit application is installed when the manager is built.

## Performance

- The contract adds one virtual call per metadata operation.
- `entries` is new and called by nothing yet; a resumed page reads one
  root-to-leaf path (under a tenth of a 1000-entry tree's nodes in the
  test).
- Not measured on fi-1 yet.

## Laws

No scheduling, pacing or gate changed. The catalogue's repair still runs
twice per pass, as it did (below).

## What proved brittle or wrongly shaped

1. **The spec's table was a text search** and both over- and under-counted:
   it counted `FileSystem`'s and the catalogue's own wrappers as metadata
   calls, missed `read_record`, the current generation and revision, the
   diagnostics and the upkeep, and estimated ~20 `mutate` sites where all
   are `mutate_delta`. The compiler is the survey; the contract is what it
   found.
2. **One contract was two.** Repair and validation are the metadata
   component's upkeep, not a view; giving them to every reader would have
   widened every holder's dependency. Split (decision log 2026-10-02).
3. **The catalogue's split needed three steps, not two.** The read compares
   against the head as it was before the repair; reading after would call
   an inventory complete that today's code calls incomplete under a race.
4. **The pass repairs the catalogue twice per pass**, once in its own stage
   and once inside the inventory build, now visible as two named steps.
   Each may commit a catalogue-root reconciliation; whether that feeds the
   fi-1 conflict loop is open (ACTIVE).
5. **The inventory read still fetches**: it fills missing catalogue objects
   into the control store. Recorded in A4; a store repair, for a later
   stage.
6. **The replica's commit applier outlives its manager**: it captures
   `this` and nothing clears it at destruction. Predates T4; recorded.
7. **Method**: a header adding a virtual method was edited under a running
   build and produced 250 segfaulting cases from mixed vtables. Nothing
   edits sources while a build or mutation run is using them.

## Not brittle

The contract as one-line delegation, `entries` and its skip-by-first-key
walk, the fake metadata view (which made the catalogue testable without a
manager), and `MetadataMaintenance`'s narrow surface. Mutants: 6 + 2 (T4b,
one surviving by the double repair above, explained), 10 (T4c), all others
killed.

## Handed on

- ACTIVE item 2: every `converged()` call outside `src/metadata/`, with
  which are request paths (the manage route; `FileSystem`'s
  `namespace_index` users and `find_media`, unguarded).
- `FileSystem::snap()` has no callers: dead.

## Verdict

No kill criterion met. Before T5: the fi-1 run and the live-namespace check
for `entries`; the double catalogue repair and the commit applier's
lifetime to the operator.

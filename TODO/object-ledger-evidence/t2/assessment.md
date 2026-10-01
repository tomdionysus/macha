# T2 assessment: the thin slice against the kill criteria

2026-10-01, laptop evidence (Clang). Still owed before acceptance: the GCC
build, the three suites and the coverage measurement on fi-1, after the T0
soak ends (see README, T2d).

The plan asks whether any contract, the budget, the handle or the
component contract proved brittle or wrongly shaped, and what changes
before widening. The kill criteria ask whether anything lost fidelity to
the user, became brittle, breached the Laws and Guidelines, or became less
performant or resilient.

## Fidelity

- The claim walk is equivalent to the 0.73 walk by construction of its test
  (every held pattern, credit, step bound and a changing ledger, side by
  side with a reference copy) and its traces are unchanged.
- The maintenance pass moved verbatim but for renames; the seven trace
  fixtures are unchanged over 100 runs each, and the lifecycle of both
  real node configurations is the recording made at T1, unchanged.
- Nothing the API sends changed: Status reads the same diagnostics, now
  from the port.

## Performance

- Claim walk: ~3.2 against ~3.05 us per claim, inside the laptop's spread;
  the cost is the presence check, not the paging.
- The pass now builds its ledger once instead of once per pass: no cost.
- `Published<T>` costs a mutex and a pointer copy per handle; it is not yet
  on any path.
- Measured on the laptop only. fi-1 measurements come with the GCC run.

## Laws

The pass's scheduling, pacing and gates are the same code: law 1 to law 3
(viewer above loader above background, repair paced by weighted share,
never gated) hold as they held. No scheduling change was made.

## What proved brittle or wrongly shaped

1. **The spec's component lifecycle was wrong in one place.** It had `stop`
   after a separate request; Service's recorded order shows a stop with no
   request beside it, so a component's `stop()` includes the request.
   Corrected in A5.
2. **A component's signals can precede it.** Events reach maintenance and
   its diagnostics are read from the node's start, before the component
   exists. Making the component own them would have lost early events and
   raced on the pointer; a port owned outside it is the shape. Recorded in
   A5. Every component the root takes from Service at T5 must be checked
   for the same.
3. **Two lists name the same dependencies.** `Maintenance::required()` and
   the externals Service declares are kept in step by hand, and the
   constructor parameters are a third statement of the same thing. The root
   refuses a mismatch at `start()`, so the first start in any test catches
   it, but it is the brittle point of the model. Before widening, derive
   the declaration from the dependency struct (one place), or accept it
   and say so in the contract.
4. **`NodeRuntime` is still a locator** for the pass (config, membership,
   the replica, three stores, the block cache, viewer signals). Accepted
   for stage 0 (decision log, 2026-10-01); T3 and T4 remove most of it. It
   is the largest remaining gap between the contract and the code.
5. **The root needed a lock** that the spec did not mention: lifecycle
   calls come from two threads (startup, stop). The old code had the same
   race unguarded. The root's `request_stop` therefore blocks for a start
   in progress; for maintenance that is a thread spawn. A component whose
   `start()` can block for long must make it cancellable, as FUSE's is.
6. **The wait guard sees only what the suites drive.** The ACTIVE item 2
   audit is complete for the paths the suites run and blind to the rest
   (a cold catalogue under a control context; one manage route). The guard
   is right; the suites are not yet a complete audit.
7. **The budget is not a value type** and says so (the yield source holds
   service-level state). That held up in the claim walk; it has not been
   tested where a budget crosses a thread.
8. **Tooling:** the laptop's Clang coverage cannot measure the suite (see
   README, T2d). Coverage is GCC on fi-1 until the framework gives each
   forked case its own profile.

## Not brittle

`Cursor`/`Page`/`Stop`, `WorkContext`, `Waits` and `may_enter`, `NoIo`
as a capability, `ObjectStore::has`, `ObjectLedger`, `Published<T>` and
the root's ordering all came out as specified, each tested exhaustively
as a primitive and mutation-proven: 13 + 16 + 9 + 24 mutants, all killed.

## Verdict

No kill criterion is met. Before widening (T3): settle point 3; carry
points 2 and 5 into the T5 conversions as a checklist; fill point 6's two
gaps with cases of their own.

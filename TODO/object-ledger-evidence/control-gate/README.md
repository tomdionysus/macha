# The control gate and `rebuilt_inventory` (spec open question 1)

Operator, 2026-09-30: a defect, fixed as its own step, pinned by a trace
fixture before and after.

The DATA and tombstone gates never use an inventory destructively in the
pass that built it (the comment at the inventory rebuild says so); the
control gate did not test `!rebuilt_inventory`. The pass now records
`control-gate-in-rebuilding-pass: open|shut` whenever the control gate is
evaluated in a pass that rebuilt the inventory.

- Before the fix (`before/`, the fixtures recorded with the trace point and
  the old gate): 7 of the fixtures' steps show the control gate open in a
  rebuilding pass.
- After: all 19 rebuilding passes are shut; no other line of any fixture
  changed except the control gate's own inputs (now including `rebuilt`).
- Reverting the fix fails 5 of the 11 trace cases.
- Deterministic: 3 x 1,100 runs of the trace group, all identical.

Laws: law 4 (recovery) is unaffected: control GC runs one quiet window
later, in the follow-up pass the rebuild already schedules.

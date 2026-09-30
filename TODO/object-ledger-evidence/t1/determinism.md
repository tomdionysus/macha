# T1 determinism

Laptop (Apple Clang, Release), `./build/macha-tests --filter maintenance_trace
--repeat 100`, eleven cases (seven trace fixtures, four clock primitives),
five consecutive runs on 2026-09-30, load average 3-12:

```
All 1100 tests passed; wall=415313ms case-sum=2475034ms
All 1100 tests passed; wall=434809ms case-sum=2598471ms
All 1100 tests passed; wall=414369ms case-sum=2475401ms
All 1100 tests passed; wall=380025ms case-sum=2268401ms
All 1100 tests passed; wall=450823ms case-sum=2690260ms
```

5,500 of 5,500 fixture runs identical to the committed traces.

The lifecycle recorder (`macha-tests-runtime --filter lifecycle --repeat
20`, twice): 40 of 40 identical for each configuration.

## How the harness got there

Each non-determinism was captured (a failing run's trace diffed against
the fixture), its mechanism named, and removed from the comparison without
removing a decision:

| variation | mechanism | resolution |
|---|---|---|
| first pass opens GC or waits a quiet window | a startup event lands before or after the first pass | fixtures start after one quiet window has passed |
| a pass sees the foreground busy | the store's `idle_for` is real time while the wake-up is armed on the manual clock | real idleness before every advance |
| inventory rebuilt at intermediate generations | a pass woke mid-write | views are settled state, last value per step |
| gate reads open or shut after GC | a stray wake-up ran one more pass | scheduling inputs excluded; verdict kept from the last decided pass |
| a push record for an object that needed nothing | a repair verification ran or did not before the step | only copies sent are recorded |
| push present or absent in the peer fixture | this node pushing races the peer pulling | repair not compared across nodes |
| claim on one node or the other | the retention barrier picks among present nodes | the peer fixture writes to both |
| release a step late after the peer returns | rejoin traffic after the step restarts the quiet window | step once the cluster reports stable |
| settle never quiet | a metadata retry deferred to a manual-clock deadline | parked means parked, not finished |
| no decided verdict after an unlink | the rebuild's one follow-up fell after the step | each advance also steps one quiet window |
| no decided verdict while the peer is away | when a GC-due pass falls depends on topology events | the peer fixture compares conditions and actions, not verdicts |
| a claim walk waits for credit | credit is scaled by real CPU load | credit waits are pacing, not compared |

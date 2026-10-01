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

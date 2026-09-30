# T1 mutation record

Each mutation applied alone, `macha-tests` rebuilt, the `maintenance_trace` group
run, source restored and object removed (`build/claude-t1-mutate.py`, laptop,
2026-09-30, against the fixtures that held over 5,500 runs):

- tombstone gate ignores stability -> KILLED by maintenance_trace/test_trace_peer_unreachable_and_back
- tombstone gate ignores the catalogue -> SURVIVED
- tombstone grace not applied -> KILLED by maintenance_trace/test_trace_tombstones_maturing
- control gate ignores the catalogue -> KILLED by maintenance_trace/test_trace_incomplete_catalogue_and_release_horizon
- DATA gate ignores destructive fencing -> KILLED by maintenance_trace/test_trace_peer_unreachable_and_back
- DATA release ignores an incomplete horizon -> SURVIVED
- horizon kept when incomplete -> SURVIVED
- claim walk inverts presence -> KILLED by maintenance_trace/test_trace_claimed_objects_lost, maintenance_trace/test_trace_incomplete_catalogue_and_release_horizon, maintenance_trace/test_trace_backend_offline_and_back, maintenance_trace/test_trace_tombstones_maturing, maintenance_trace/test_trace_peer_unreachable_and_back
- claim walk visits one claim a step -> KILLED by maintenance_trace/test_trace_claimed_objects_lost, maintenance_trace/test_trace_incomplete_catalogue_and_release_horizon
- pull pass skips the first object -> KILLED by maintenance_trace/test_trace_claimed_objects_lost
- pull pass stops after one object -> SURVIVED

## The four survivors

- **Tombstone gate ignores the catalogue: an equivalent mutant.** While the
  catalogue inventory is incomplete the inventory is rebuilt on every pass
  that could open the gate (the rebuild condition includes
  `!maintenance_catalogue_complete_`, and `garbage_due` excludes the
  repair-only case), so `!rebuilt_inventory` already shuts it. The term is
  redundant in every reachable state; recorded for T3's gate tables.
- **DATA release ignores an incomplete horizon: an equivalent mutant.** An
  incomplete horizon is discarded and the previous complete one kept, so
  `retention_release_complete_` is true whenever
  `retention_release_data_live_` is set; the first term is implied by the
  second.
- **Horizon kept when incomplete: not reached by a fixture.** The mutant
  matters only when the maintenance inventory is complete and the release
  horizon is not, i.e. a namespace tree node unreadable while the catalogue
  is readable. A fresh test node has no namespace tree root; a fixture needs
  a tree-backed namespace, left for T3, whose B3 table tests include
  "refresh keeps the previous release horizon when the new one is incomplete".
- **Pull pass stops after one object: pacing, not a decision.** Later passes
  continue from the cursor and visit the same objects in the same order;
  only how many a step covers changes. A change to the order itself
  ("pull pass skips the first object") is killed.


## The clock (`src/service/maintenance_clock.cpp`)

`build/claude-t1-mutate-clock.py`, filter `clock`:

- advance moves only steady time -> KILLED by maintenance_trace/test_manual_clock_moves_only_when_advanced
- advance moves only wall time -> KILLED by maintenance_trace/test_manual_clock_moves_only_when_advanced, maintenance_trace/test_manual_clock_wait_returns_on_ready_stop_or_deadline
- manual wait ignores its deadline -> KILLED by maintenance_trace/test_manual_clock_wait_returns_on_ready_stop_or_deadline
- manual wait ignores stop -> KILLED by maintenance_trace/test_manual_clock_wait_returns_on_ready_stop_or_deadline
- system wait never waits unbounded -> SURVIVED
- system wait ignores its deadline -> KILLED by maintenance_trace/test_system_clock_reads_and_waits_in_real_time
- manual clock starts at zero wall time -> KILLED by maintenance_trace/test_manual_clock_moves_only_when_advanced

The survivor is equivalent on both toolchains: libc++ (laptop) and
libstdc++ (fi-1, run there by hand) both honour `wait_until` to
`time_point::max()`, and the unbounded-wait test (still waiting after 50 ms,
ended only by stop) passes with the mutant. The branch is kept because it is
0.73.2's exact wait (the Service loop split it the same way); a library
that overflowed there would turn the pass's idle wait into a busy loop.

Coverage of `maintenance_clock.cpp` on fi-1 (gcov, `--filter
maintenance_trace`): 100% of 27 lines; every source branch taken both ways
(the untaken gcov branches are exception edges).

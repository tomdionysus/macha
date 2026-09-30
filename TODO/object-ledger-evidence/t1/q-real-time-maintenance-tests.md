# Q: maintenance tests that wait on real time

Cases in `macha-tests` that set maintenance timing
(`maintenance.garbage_grace`, `foreground_quiet`, `no_progress_backoff`,
`interval`) or watch `maintenance_wakeups()`, found with basemind and mapped
to their enclosing case, with fi-1's case times from the T0 final run
(`../t0/suites/fi-1-macha-tests.txt`, 615 cases, 206.7 s summed):

| ms | case |
|---|---|
| 5249 | rpc_cluster/test_partition_delete_defers_destructive_gc_until_cluster_healthy |
| 4196 | hydration_catalogue/test_catalogue_sync_search_and_artwork_gc |
| 3650 | filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event |
| 1210 | hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst |
| 1027 | rpc_cluster/test_held_retention_claims_cost_repair_no_credit |
| 902 | filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace |
| 608 | invariants/test_catalogue_artwork_batch_defers_durability_until_barrier |
| 575 | rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers |
| 501 | rpc_cluster/test_retained_missing_copy_repairs_without_namespace_reachability |
| 376 | rpc_cluster/test_repair_progresses_while_the_loader_never_goes_quiet |

10 cases, 18.3 s, 8.8% of summed time. A lower bound: any other case that
waits on a maintenance outcome (GC reclaiming, repair copying) under the
default 2 s quiet window waits on real time too, and is not found by this
search. These are the first candidates for the manual clock once their
assertions can be restated as trace fixtures (the Q track's rule: the
integration test goes only after the isolated one is mutation-proven against
the same fault).

# The port's suite on a Pi, 2026-11-17: what is left

Macha `port/dannan-dagda` at `71e69e1` on Dannan 0.8.5 and Dagda `8b544d6` (the state layout beside its host's files,
repair stage 4), build-3, `macha-tests --jobs 3`: **514 passed, 31 failed.** The run's log is
`build/port-suite3-2026-11-17.log` (build-3 `/root/pi/suite3.log`).

How it got here: the first run (Dagda `d119d14`) failed 177, 173 of them at start on Dagda's state-layout check, fixed by
Dagda in `8b544d6`; the second failed 175, nearly all at start because test nodes had no key custody, which Dagda's
store needs for its ledgers (`71e69e1` gives every test node custody as an operator would). Macha's own fallout fixed on
the way: the store's message types registered twice (`c5fbbb4`), an API request during startup crashing the HTTP
reactor on the store node's activity clock (`8b40c4a`), two tests following the registration and stop-order changes.

## The 31, grouped by what the failure looks like

The groups are hypotheses from the failing line alone; none is diagnosed. Each needs its mechanism proved before a
change, and a Dagda cause pauses the port for Dagda's fix (the operator's rule).

1. **The trace fixtures (6)**, step 3's REWRITE cases: `test_trace_quiet_node`, `_tombstones_maturing`,
   `_revived_tombstone`, `_claimed_objects_lost`, `_peer_unreachable_and_back`, `_incomplete_catalogue_and_release_horizon`:
   "trace differs". Dagda says its `MachaLoop` reproduces these fixtures unchanged by draining after each transfer, as
   Macha's old pass waited; Macha's pass does not drain. First check: the diff of each against its fixture.
2. **Copies not reaching a peer (8)**: `rpc_cluster/test_a_joining_node_pulls_its_objects_through_maintenance`,
   `_replacement_node_recovers_namespace_and_replication`, `_replication_policy_change_on_restart`,
   `_a_file_touch_commits_while_a_holder_is_away`, `_two_node_mutual_bootstrap_metadata_write_floor` (failing before the
   port too, ACTIVE's P0 list), `maintenance_trace/test_repair_resumes_after_yielding_to_a_viewer_that_arrived_mid_step`,
   `availability/test_what_only_a_host_out_of_reach_holds_is_unavailable`,
   `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`. Likely one cause in how
   Macha's pass drives stage 4's window (transfers no longer land within the step).
3. **Nothing reclaimed (6)**: `rpc_cluster/test_a_delete_is_reclaimed_while_a_node_is_away`,
   `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`, `namespace_service/test_a_stale_tombstone_in_a_batch_is_erased_once`,
   `_the_control_collector_keeps_an_unclaimed_namespace`, `_tombstones_live_in_batches_in_a_tree_backed_head`, and the
   claim case `rpc_cluster/test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim`. First check: the
   provider is registered only with `maintenance.collect_garbage` set (default false, the cutover ruling); the test
   configs may not set it.
4. **A backend taken away is not seen (3)**: `maintenance_trace/test_trace_backend_offline_and_back`,
   `hydration_catalogue/test_ingest_blocks_while_storage_is_unavailable` (both wait for `online_backends() == 0`),
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change` (P0 list before the port).
5. **A failed startup not shown as failed (2)**: `service_root/test_a_failed_startup_is_what_every_caller_is_told`,
   `_with_no_reason_still_says_it_failed`: the store node's recovery fails ("backend missing") but
   `readiness().failed` is not set in time.
6. **The pass's wait (2)**: `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace` (P0 list before
   the port), `_disconnected_maintenance_sleeps_until_peer_event`.
7. **One each**: `filesystem_fuse/test_fuse_frontend_local_semantics` (`payload_bytes() >= extent`),
   `rpc_cluster/test_no_progress_deadline_follows_the_lane_configured` (the error text no longer says "no progress":
   check against the three-answers rule, which forbids matching English anyway),
   `rpc_cluster/test_service_shutdown_cancels_pending_outbound_rpc_before_join` (its blocked call is never entered),
   `availability/test_two_nodes_survey_what_neither_holds` (P0 list before the port).

Dagda's word (2026-11-17, with `82e5201`, which changes no API): stage 4's unpaced large rejoin is still slower than
stage 3's on the Pis (18 to 25 s against 15 to 16 s for 26 x 4 MiB); not found yet; read no large-object repair
comparison as settled until Dagda sends the fix.

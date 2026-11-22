# The port: tests still on Macha's store parts (2026-11-16)

Before Macha's store code, its `StorageServer` and `LocalState`'s store parts can go (the plan's R6/R7 steps 3 and 4),
every case below moves onto the store node's own parts (`BareNode::store_node()`: `data()`, `control()`, `cache()`,
`claims()`, `ledger()`, `references()`, `store()`), or retires against a named Dagda case where its subject is
Dagda's. Fate is Dagda's mapping's; UNMAPPED cases are newer than it. The fixtures in `tests/test_support.hpp` and
`tests/fake_cluster_node.hpp` (`TestNode`'s `DistributedStore`, `distributed_store_for`, `StoreBench`, the
`local_store()`, `control_store()`, `block_cache()`, `claims()`, `ledger()` seams) move with them.

| file | case | fate | uses |
|---|---|---|---|
| test_availability.cpp | test_what_only_a_host_out_of_reach_holds_is_unavailable | REWRITE | `local_state().data()` |
| test_availability.cpp | test_a_failed_diff_is_asked_again_on_a_timer | UNMAPPED | `local_state().data()` |
| test_availability.cpp | test_two_nodes_survey_what_neither_holds | REWRITE | `local_state().data()` |
| test_availability.cpp | test_a_disk_coming_back_is_surveyed_without_a_namespace_change | REWRITE | `local_state().data()` |
| test_contract.cpp | test_the_wait_guard_on_a_cold_catalogue | REWRITE | `DistributedStore` |
| test_filesystem_fuse.cpp | test_coalesced_delete_burst_wakes_at_exact_garbage_grace | REWRITE | `local_state().data()` |
| test_hydration_catalogue.cpp | test_catalogue_scanner_matches_binds_and_reconciles_a_library | REWRITE | `local_store()` |
| test_hydration_catalogue.cpp | test_ingest_blocks_while_storage_is_unavailable | REWRITE | `local_state().data()` |
| test_hydration_catalogue.cpp | test_catalogue_warm_read_defers_remote_refresh | REWRITE | `DistributedStore` |
| test_hydration_catalogue.cpp | test_catalogue_sync_search_and_artwork_gc | REWRITE | `local_state().data()` |
| test_hydration_catalogue.cpp | test_catalogue_uses_final_state_after_coalesced_metadata_burst | REWRITE | `local_state().data()`, `local_state().retention()` |
| test_hydration_catalogue.cpp | test_books_are_catalogued_from_their_files_and_served_as_content | REWRITE | `local_store()` |
| test_invariants.cpp | test_catalogue_keeps_what_the_namespace_holds | REWRITE | `local_store()` |
| test_invariants.cpp | test_a_failed_read_keeps_the_object_and_a_bad_record_is_removed | UNMAPPED | `StoragePool` |
| test_maintenance_trace.cpp | test_trace_backend_offline_and_back | REWRITE | `local_state().data()` |
| test_maintenance_trace.cpp | test_trace_claimed_objects_lost | REWRITE | `local_state().data()`, `local_state().retention()` |
| test_maintenance_trace.cpp | test_repair_resumes_after_yielding_to_a_viewer_that_arrived_mid_step | UNMAPPED | `local_state().data()` |
| test_maintenance_trace.cpp | test_trace_incomplete_catalogue_and_release_horizon | REWRITE | `local_state().control()` |
| test_namespace_service.cpp | test_tombstones_live_in_batches_in_a_tree_backed_head | UNMAPPED | `local_state().control()` |
| test_namespace_service.cpp | test_a_catalogue_change_on_a_tree_leaves_the_tree_alone | UNMAPPED | `local_state().retention()` |
| test_namespace_service.cpp | test_the_control_collector_keeps_an_unclaimed_namespace | UNMAPPED | `local_state().control()`, `local_state().retention()` |
| test_namespace_service.cpp | test_a_merge_claims_what_it_introduces | UNMAPPED | `local_state().retention()` |
| test_node_services.cpp | test_observation_gauges_follow_activity_repair_and_the_mount | REWRITE | `.store()` |
| test_node_services.cpp | test_a_commit_is_published_only_once_its_objects_are_claimed | GAP-HEADS:H22 | `claims()` |
| test_rpc_cluster.cpp | test_the_repair_step_that_first_sees_a_new_pull_list_walks_it | UNMAPPED | `.store()`, `StoreBench` |
| test_rpc_cluster.cpp | test_a_pull_list_new_while_the_push_runs_is_not_passed_over | UNMAPPED | `.store()`, `StoreBench` |
| test_rpc_cluster.cpp | test_store_control_objects_take_no_data_credit | UNMAPPED | `.store()`, `StoreBench` |
| test_rpc_cluster.cpp | test_two_node_mutual_bootstrap_metadata_write_floor | GAP-HEADS:H8,H27 | `local_state().data()` |
| test_rpc_cluster.cpp | test_a_file_touch_commits_while_a_holder_is_away | GAP-HEADS:H22 | `local_state().data()`, `local_state().retention()` |
| test_rpc_cluster.cpp | test_a_delete_is_reclaimed_while_a_node_is_away | REWRITE | `local_state().data()`, `local_state().retention()` |
| test_rpc_cluster.cpp | test_replication_policy_change_on_restart | REWRITE | `DistributedStore`, `local_state().data()` |
| test_rpc_cluster.cpp | test_replacement_node_recovers_namespace_and_replication | REWRITE | `DistributedStore`, `local_state().data()` |
| test_rpc_cluster.cpp | test_a_joining_node_pulls_its_objects_through_maintenance | REWRITE | `local_state().data()` |
| test_rpc_cluster.cpp | test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim | GAP-HEADS:H4,H22 | `local_state().retention()` |
| test_runtime_dependencies.cpp | test_embedded_music_metadata_and_artwork | REWRITE | `local_state().data()` |
| test_storage_metadata.cpp | test_storage_pool_and_persistent_cache | GAP-HEADS:H25 | `StoragePool` |
| test_storage_v18.cpp | test_a_full_preferred_node_spills_data_but_not_control | REWRITE | `.store()`, `control_store()`, `local_state().data()`, `local_store()` |
| test_storage_v18.cpp | test_catalogue_control_objects_recover_on_metadata_replica | REWRITE | `control_store()` |

# Suite failures seen once, and what each was (2026-10-03 to 2026-10-04)

Each was reproduced by forcing its mechanism unless marked otherwise.

| test | what it was | proof | fixed in |
|---|---|---|---|
| `hydration_catalogue/test_ingest_pause_resume_and_cancel…` | product: a cancel or pause accepted during the final rename was overwritten by the worker | worker held in the rename, cancel acknowledged, job went on to `cataloguing` | product, `aadf6cb` |
| `session/test_session_revoke_propagates_cluster_wide` | product: the push to peers was skipped when the RPC client's route table was busy (`try_lock`), leaving the 10 s gossip tick | 2 of 60 loaded runs failed, each with a push that reached nobody; 0 of 120 after | product, 0.87.2 |
| `rpc_cluster/test_inbound_incapable_node…` | test: read the site's route table while its dial was still being installed | a 200 ms gap between start and install failed it 4 of 4 | test, `6f13e1a` |
| `media_playback/test_an_async_start…` (starts == 3) | test: counted a start of a session deleted before it reached the engine | 3 of 8 on fi-1; 12 of 12 after | test, `d453031` |
| `media_playback/test_an_async_start…` (output_media_ms) | test: read published progress 250 ms after the last advance, the monitor's own sample period | a 700 ms sample failed it 4 of 4 | test, 0.87.2 |
| `availability/test_two_nodes_survey_what_neither_holds` | test: waited for a file to be named, not for its data commit | probe: wait satisfied with the file at 0 extents, root 6 not 7 | test, `e32aebd` |
| `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` | test: called the pass parked after 300 ms quiet; a lone node's formation-settle wake comes 475 ms after the one before | probe on the real clock: wakes at 138, 166 and 641 ms | test, manual clock |
| `filesystem_fuse/test_removing_empty_directories_in_a_burst…` | test: waited for idle with its own readers still issuing requests | broker pending 2 to 4 with all else zero; readers stopped first, 2.5 to 2.8 s every run | test |
| `filesystem_fuse/test_fuse_admission_backpressure` | test: asserted a spool file a prompt publisher had already retired | 5 of 40 and 3 of 40 at a zero durability window; 40 of 40 after | test |
| `hydration_catalogue/test_metadata_decoded_cache_ttl…` | test: a 30 ms TTL on the real clock across two durable commits | reproduced under load at the same lines; stepped time now | test |
| `filesystem_fuse/test_write_data_work_context…` | test: read a counter the maintenance pass also takes | probe: 4096 bytes with no pause, 0 after 400 ms and one more pass. The original assertion was not captured, so this is a race the test had, not a match to that failure. The replacement runs with no maintenance pass. | test, consolidated |
| `rpc_cluster/test_rpc_slow_control_does_not_abort_data` | **not explained** | not reproduced in 280 + 120 loaded laptop runs, 150 on fi-1, or 60 verbatim with 96 busy loops; liveness pings held past the peer-death window do not abort the calls; the original assertion was not captured. Rewritten without real-time sleeps. | open |

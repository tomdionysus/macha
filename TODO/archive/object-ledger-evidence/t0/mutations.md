# T0 mutation record

Each mutation was applied alone, `macha-tests` rebuilt, the `observation` group run, the
source restored and its object file removed (scratch script, 2026-09-30, laptop).
Tests named are those that failed.

## Pass 1: src/observation.cpp

- bucket sub-bucket mask -> KILLED by observation/test_observation_bucket_contains_every_small_value, observation/test_observation_quantiles, observation/test_observation_buckets_tile_the_value_range
- bucket exact below 8 -> KILLED by observation/test_observation_bucket_contains_every_small_value, observation/test_observation_buckets_tile_the_value_range, observation/test_observation_quantiles, observation/test_observation_snapshot_windows, observation/test_observation_window_rendering
- upper bound width -> KILLED by observation/test_observation_buckets_tile_the_value_range
- quantile cap at max -> KILLED by observation/test_observation_quantiles
- quantile rank ceil -> KILLED by observation/test_observation_quantiles, observation/test_observation_window_rendering
- quantile empty -> KILLED by observation/test_observation_quantiles
- quantile fallthrough -> KILLED by observation/test_observation_quantiles
- minus saturation -> KILLED by observation/test_observation_snapshot_windows
- window max -> KILLED by observation/test_observation_snapshot_windows
- record max CAS -> KILLED by observation/test_observation_histogram_records_from_many_threads, observation/test_observation_process_registry_and_durations, observation/test_observation_quantiles, observation/test_observation_snapshot_windows, observation/test_observation_window_rendering
- histogram series bound -> KILLED by observation/test_observation_registry_is_bounded
- counter series bound -> KILLED by observation/test_observation_registry_is_bounded
- event bound drops oldest -> KILLED by observation/test_observation_events_are_bounded_oldest_first
- events dropped counted -> KILLED by observation/test_observation_events_are_bounded_oldest_first
- elapsed never negative -> KILLED by observation/test_observation_process_registry_and_durations
- route segments kept -> KILLED by observation/test_observation_route_labels, baseline/test_baseline_observation_probe_costs
- route word length -> KILLED by observation/test_observation_route_labels
- route non-api -> KILLED by observation/test_observation_route_labels
- route method -> KILLED by observation/test_observation_route_labels
- render skips idle histograms -> KILLED by observation/test_observation_window_rendering
- render skips still counters -> KILLED by observation/test_observation_window_rendering
- log rotation threshold -> SURVIVED
- log rotation target -> KILLED by observation/test_observation_log_rotates_at_its_bound
- recorder advances previous -> KILLED by observation/test_observation_recorder_writes_windows_and_events
- recorder window start -> KILLED by observation/test_observation_recorder_writes_windows_and_events
- recorder drains events -> KILLED by observation/test_observation_recorder_writes_windows_and_events, observation/test_observation_recorder_thread_and_final_window, observation/test_a_service_writes_its_lifecycle_to_the_observation_file
- recorder final window -> KILLED by observation/test_observation_recorder_thread_and_final_window, observation/test_a_service_writes_its_lifecycle_to_the_observation_file
- recorder periodic tick -> KILLED by observation/test_observation_recorder_thread_and_final_window

The survivor (the rotation bound's newline byte) was answered by the
`edge.jsonl` case in `test_observation_log_rotates_at_its_bound`.

## Pass 2: the survivor, the failure log, and the Service's events

- log rotation threshold -> KILLED by observation/test_observation_log_rotates_at_its_bound
- log failure resets -> KILLED by observation/test_observation_log_survives_an_unwritable_path
- services_ready event -> KILLED by observation/test_a_service_writes_its_lifecycle_to_the_observation_file
- shutdown event -> KILLED by observation/test_a_service_writes_its_lifecycle_to_the_observation_file
- recorder reset after stop -> SURVIVED
- gauges rss -> KILLED by observation/test_a_service_writes_its_lifecycle_to_the_observation_file

The survivor (a second stop queueing a stray shutdown event) was answered
by checking the event queue is empty after the second stop:

- recorder reset after stop -> KILLED by observation/test_a_service_writes_its_lifecycle_to_the_observation_file

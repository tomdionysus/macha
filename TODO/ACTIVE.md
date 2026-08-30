# Active tasks and concepts to explore

Last updated: 2026-08-30

This is the working backlog for the current session. Add new work here. When an item is implemented and its stated verification is complete, remove it from this file and add a dated entry with evidence to `COMPLETED.md`.

The existing documents in this directory remain the detailed plans, checkpoints, and UAT records. This file is only the current index.

## Phase 2 namespace batching follow-up

- [ ] Design a durable batch identity that permits dependency chains such as create/rename/unlink to share a publication without weakening restart proof.
- [ ] Test mixed create/rename/unlink dependencies within a batch and across batch boundaries after that identity exists.
- [ ] Decide whether each published prefix member needs an explicit in-memory association with its accepted commit hash/generation.
- [ ] Preserve rename as a safe singleton boundary until the durable mixed-operation design and crash matrix are complete.

## Diagnostics and operational proof still needed

- [ ] Record a reproducible local benchmark recipe without default-suite timing thresholds.
- [ ] Deploy the cluster Status telemetry aggregation correction and run its two-endpoint UAT: each single response must contain live numeric telemetry for every connected node and complete online aggregates, without client fan-out.
- [ ] Correct the misleading Status `runtime.rss_bytes` metric, which currently uses lifetime-peak `ru_maxrss`. Report current resident bytes, or expose separately and explicitly named current and peak values; add platform-aware contract tests and operational documentation.
- [ ] After correcting the RSS metric, repeat the namespace-burst UAT for at least three rounds after every materialization cache reaches its 64-entry bound, then establish whether current RSS reaches a stable ceiling or decays after drain.
- [ ] Diagnose the concurrent-suite failure in `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`. It originally timed out once during Phase 5 and then passed isolated and in the following full run. During the telemetry-aggregation checkpoint it timed out at the same 10-second assertion in two consecutive complete runs (12-way and 4-way concurrency), while an immediate isolated run passed in 0.890 seconds. Preserve the captured logs and diagnose the missing catalogue completion signal/state transition; do not increase the timeout or dismiss it as timing noise.

## Separate known issue

- [ ] Diagnose and correct faulty torrent/ingest behaviour. Pausing the torrent removed the local node's residual CPU during Phase 3 idle UAT. Treat this as a separate subsystem investigation so it does not obscure metadata/convergence measurements.

## Deferred architectural concepts

- [ ] Consider delta-native commit identity based on parent identity, canonical delta, and a state-tree root instead of a fully serialized namespace payload.
- [ ] Consider a persistent or copy-on-write namespace tree with incremental subtree hashing.
- [ ] If either protocol-level design proceeds, define rolling-upgrade negotiation, checkpoint/anchor migration, and independent corruption validation first.

## Additional investigation

- [ ] Investigate whether ingest from the spool has performance problems unrelated to the namespace batching and convergence work currently in progress.
- [ ] Replace the spool's hard-coded 16 GiB capacity limit with a documented, validated configuration setting and a safe default preserving current behaviour.
- [ ] Design occupancy-aware spool admission backpressure that also tracks sustained publication/drain throughput. Admission should be fast while capacity is readily available, progressively slow as occupancy approaches the configured limit, and never admit data beyond the durable capacity bound.
- [ ] Define the backpressure policy precisely, including measurement windows, high/low watermarks or hysteresis, minimum progress, fairness between writers, restart behaviour, and behaviour when publishing stalls or the cluster becomes unwritable.
- [ ] Ensure throttling blocks or paces the ingesting FUSE requests without polling, busy-waiting, unbounded buffering, or consuming critical RPC/control threads.
- [ ] Add deterministic tests for configurable capacity, occupancy accounting, progressive throttling, recovery as the spool drains, a completely stalled publisher, multiple concurrent writers, restart near capacity, and enforcement of the hard upper bound.
- [ ] Run an rsync-style mounted-FUSE UAT: initial writes should run quickly, throughput should progressively approach sustainable publication speed as the spool fills, occupancy should remain bounded, and write speed should recover cleanly after the backlog drains.

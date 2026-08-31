# Active tasks and concepts to explore

Last updated: 2026-08-31

This is the working backlog for the current session. Add new work here. When an item is implemented and its stated verification is complete, remove it from this file and add a dated entry with evidence to `COMPLETED.md`.

The existing documents in this directory remain the detailed plans, checkpoints, and UAT records. This file is only the current index.

## Phase 2 namespace batching follow-up

- [ ] Design a durable batch identity that permits dependency chains such as create/rename/unlink to share a publication without weakening restart proof.
- [ ] Test mixed create/rename/unlink dependencies within a batch and across batch boundaries after that identity exists.
- [ ] Decide whether each published prefix member needs an explicit in-memory association with its accepted commit hash/generation.
- [ ] Preserve rename as a safe singleton boundary until the durable mixed-operation design and crash matrix are complete.

## Diagnostics and operational proof still needed

- [ ] Record a reproducible local benchmark recipe without default-suite timing thresholds.
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

- [ ] Diagnose why the deployed identity-association reset still does not work.
  The 2026-08-30 UAT failed after changing the handler to apply/propagate before
  metadata persistence and adding durable `MACHMEM2` tombstones. The synthetic
  unavailable-metadata and restart tests pass, so they do not reproduce the
  real failure.
- [ ] Capture the actual reset HTTP request and response, server log path,
  membership/telemetry/RPC state before and after the action, and state after a
  membership refresh. Determine whether the failure is API routing/request
  shape, reset application, peer propagation, immediate reauthentication, or
  status aggregation retaining the retired durable node.
- [ ] Add a test reproducing the deployed failure before claiming the reset is
  fixed. Retain the current focused tests, but do not treat them as sufficient
  UAT evidence.
- [ ] Execute the phased FUSE publication throughput work in
  [2026-08-31-fuse-publication-throughput-plan.md](2026-08-31-fuse-publication-throughput-plan.md).
  Phase 0/1 diagnostics, loader priority, concurrency, fair byte quanta and real
  viewer pre-emption are complete. Phase 4A bounded within-file extent
  pipelining is implemented and locally verified; perform its three-node UAT
  before moving it to `COMPLETED.md`. The pre-change measured limit was roughly
  7.72 MiB/s aggregate synchronous extent/durability I/O with substantial host
  I/O wait.
- [ ] Fix stale FUSE mount recovery ordering. Startup currently calls
  `create_directories(mount_path)` before `prepare_fuse_mountpoint()`, so a
  disconnected Macha mount returns `ENOTCONN` before
  `fuse.unmount_if_mounted: true` can recover it. Add a regression around the
  preflight contract and retain refusal of unrelated filesystems.
- [ ] Phase 2: design and prove versioned durable incremental extent staging and
  safe spool-range retirement without exposing partial files.
- [ ] Phase 3: aggregate sequential local write descriptors and make durability
  group commit byte/urgency driven.
- [ ] Phase 4A UAT: verify the bounded within-file extent pipeline materially
  improves useful throughput while viewer and control latency remain protected.
- [ ] Phase 4B: replace transient per-extent tasks with a shared byte-bounded
  data executor, add per-peer/storage-domain bounds, isolate all storage waits
  from communications, and aggregate compatible physical durability barriers.
- [ ] Phase 5: integrate continuous progress rates, occupancy hysteresis,
  concurrent-writer fairness, and authoritative catalogue hints.
- [ ] Complete the final mixed-size rsync, concurrent-writer, communications,
  restart, peer-loss, and idle-soak verification matrix.

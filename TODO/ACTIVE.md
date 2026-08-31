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
  The 2026-08-31 rsync UAT found a direct scheduler defect: any open writer caps
  publication at the single foreground worker despite eight configured commit
  workers, causing multi-GB head-of-line blocking at about 1.9 MB/s while disks
  and CPUs are underused.
- [ ] Deploy and UAT the first Phase 0/1 implementation checkpoint documented in
  [2026-08-31-fuse-publication-phase-0-1-checkpoint.md](2026-08-31-fuse-publication-phase-0-1-checkpoint.md).
  Deterministic coverage now proves open loaders use multiple workers, closed
  files receive priority, demand coalesces, useful bytes reconcile, and viewer
  demand gates publication. Explicit byte bounds and fair resumable quanta are
  still pending, so Phase 1 is not yet complete.
- [ ] Phase 0: add useful-byte and per-stage publication telemetry, split demand
  coalescing from real publication counts, and record physical baselines.
- [ ] Phase 1B: remove the false single-publisher cap, prioritise closed files,
  add fair concurrent publication with worker and byte bounds, and run the
  four-file/three-node UAT checkpoint. Local implementation and deterministic
  tests are complete: generations now retain a resumable writer/cursor across
  extent-aligned byte quanta, requeue at the tail, respect a global admitted
  byte budget, and remain atomically invisible until final commit. See
  [2026-08-31-fuse-publication-phase-1b-fair-quanta.md](2026-08-31-fuse-publication-phase-1b-fair-quanta.md).
  The three-node UAT found that real FUSE reads updated the interactive clock
  while the publication gate watched only the foreground clock. The signal
  mismatch is now fixed and covered through the adapter's public viewer hook;
  redeploy and repeat the bounded viewer-pre-emption UAT before completion. See
  [2026-08-31-fuse-publication-phase-1ab-uat.md](2026-08-31-fuse-publication-phase-1ab-uat.md).
- [ ] Phase 1A: separate loader priority from crash-recovery provenance. Durable
  spool publication remains user-requested loader work after restart and must
  not be capped by `recovery_commit_workers`. Add an RPC loader class below
  viewer/read-ahead and above speculative maintenance, retain recovered origin
  only for checksum/cache/crash semantics, and test both scheduler ordering and
  restart behaviour. Deterministic implementation is complete and documented in
  [2026-08-31-fuse-publication-phase-1a-loader-priority.md](2026-08-31-fuse-publication-phase-1a-loader-priority.md);
  coordinated loader-class UAT passed, while live viewer pre-emption remains to
  be combined with the next Phase 1B UAT before this item moves to
  `COMPLETED.md`.
- [ ] Phase 2: design and prove versioned durable incremental extent staging and
  safe spool-range retirement without exposing partial files.
- [ ] Phase 3: aggregate sequential local write descriptors and make durability
  group commit byte/urgency driven.
- [ ] Phase 4: pipeline bounded data RPC, isolate storage waits from control
  communications, and aggregate compatible physical durability barriers.
- [ ] Phase 5: integrate continuous progress rates, occupancy hysteresis,
  concurrent-writer fairness, and authoritative catalogue hints.
- [ ] Complete the final mixed-size rsync, concurrent-writer, communications,
  restart, peer-loss, and idle-soak verification matrix.

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

## Separate known issue

- [ ] Diagnose and correct faulty torrent/ingest behaviour. Pausing the torrent removed the local node's residual CPU during Phase 3 idle UAT. Treat this as a separate subsystem investigation so it does not obscure metadata/convergence measurements.
- [ ] Diagnose node 51's non-graceful shutdown observed during the 2026-08-31 rolling deployment. The process reached `Service::stop`, stopped at `service maintenance joining`, exceeded systemd's one-minute stop timeout and was killed. Add a bounded shutdown regression that identifies which maintenance owner fails to observe stop; do not hide it by increasing `TimeoutStopSec`.

## Deferred architectural concepts

- [ ] Consider delta-native commit identity based on parent identity, canonical delta, and a state-tree root instead of a fully serialized namespace payload.
- [ ] Consider a persistent or copy-on-write namespace tree with incremental subtree hashing.
- [ ] If either protocol-level design proceeds, define rolling-upgrade negotiation, checkpoint/anchor migration, and independent corruption validation first.

## Additional investigation

- [ ] Support multiple advertised endpoints per node and multiple candidate IPs
  per bootstrap node. A node must be able to advertise at least its local/LAN
  and internet/WAN endpoints simultaneously, with address family, scope and
  provenance sufficient for peers to choose an endpoint reachable from their
  own network position.
- [ ] Integrate endpoint discovery with the completed UPnP/external-IP work:
  automatically combine configured listen/advertise addresses, interface/LAN
  addresses, discovered public address and mapped public port, while suppressing
  unusable, stale and duplicate candidates. Common home-network deployments
  should work with little or no manual advertise configuration.
- [ ] Deduplicate connections and membership by durable node identity rather
  than endpoint. Race viable candidates intelligently (with bounded fallback
  and remembered reachability), converge multiple successful paths onto one
  logical peer/session, prefer direct LAN paths where appropriate, and avoid
  reconnect storms when the same node is present through bootstrap, LAN and WAN
  addresses.
- [ ] Define configuration/wire compatibility, endpoint refresh/expiry and
  security rules before implementation. Add deterministic tests for same-LAN,
  remote-WAN, NAT hairpin unavailable, dual-stack/multi-homed, endpoint change,
  duplicate bootstrap entries and simultaneous-dial cases, plus UAT with the
  existing UPnP/public-connectivity status reporting.

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
  Phase 0/1 diagnostics, loader RPC priority, concurrency, fair byte quanta and
  Phase 4A bounded within-file extent pipelining are complete. The Phase 4A UAT
  reached 19.5 MiB/s across the full observation window and 40.2 MiB/s in a
  clean loaded interval, versus the pre-change 7.72 MiB/s. Its FUSE-read
  "viewer" leg is now explicitly invalidated: it proved bounded yielding, but
  FUSE is loader/convenience traffic rather than the real viewing path.
- [ ] Complete Phase 1C/1D invariant UAT. The Phase 1C implementation and focused
  deterministic tests are complete, and the verified binary is deployed on
  nodes 50 and 51. FUSE
  reads are loader class, the exclusive playback gate is gone, and configurable
  work-conserving weights default to `95:5`. Live `rsync --append-verify`
  demonstrated concurrent publication, including a 50,122,257-byte publication
  advance in 20.795 seconds after the cluster returned to 3/3. The overnight
  copy is intentionally still running. A real viewer/streaming test remains
  required after Phase 1D: an attempted playback while node 51 was offline
  failed because its media extents were unavailable, so that incident is not
  valid scheduler UAT.
  See
  [the Phase 1C checkpoint](2026-08-31-fuse-publication-phase-1c-weighted-scheduling.md)
  and
  [the integrated plan](2026-08-31-fuse-publication-throughput-plan.md#phase-1c-weighted-viewerloader-scheduling-and-fuse-classification).
- [ ] Correct the remaining failure found by the deployed Phase 1D loaded UAT:
  viewer priority currently ends at RPC admission and does not bound
  already-admitted loader disk/object-transfer work. Add deterministic
  viewer-versus-loader distributed-read tests, then repeat the real
  `rsync --append-verify` plus playback/seek UAT. See
  [the failed UAT record](2026-08-31-phase-1d-loaded-uat-failure.md).
  The zero-rate spool dead zone is now corrected, natively tested and passed
  deployment UAT: drained partial publication grants bounded one-for-one
  bootstrap credits without weakening the hard limit, and live rsync remained
  active above 50% occupancy during playback. Viewer/resource arbitration is
  still active. See
  [the bootstrap-admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md).
  The first local correction is now implemented: blocking DATA object/store
  work has configurable node-wide byte admission and non-borrowable viewer
  headroom below RPC admission, with deterministic loader-saturation proof and
  operational counters. A short loaded rsync plus real playback/seek UAT is the
  next gate; per-device/per-peer and durability resources remain active. See
  [the DATA resource checkpoint](2026-08-31-phase-1d-data-resource-headroom.md).
  The deployed loaded UAT passed its objective gate: 24 viewer admissions had
  zero waits while loader work completed a 696,597,597-byte publication, net
  spool occupancy fell by 1,348,025,437 bytes, all nodes converged to generation
  885, and timeouts/RPC failures remained zero. Operator confirmation of
  subjective playback/seek quality is still required. The live run's apparent
  6.8x read ratio compared five in-progress publications with one completed
  publication, so it was not a valid same-cohort amplification measurement.
  Completed-cohort counters now make the next UAT comparison exact. See
  [the UAT record](2026-08-31-phase-1d-data-resource-headroom-uat.md).
- [ ] Phase 1D invariant gate: propagate `control > viewer >> loader >
  speculative` through executors, locks, byte credits, physical I/O, RPC,
  durability and metadata; reserve control capacity and viewer headroom; make
  materialisation/rebuild resumable at bounded checkpoints; remove the global
  commit convoy; and prove maximum lower-class work ahead of control/viewer is
  bounded. Preserve 95:5 viewer/loader service, loader non-starvation,
  work-conserving borrowing, durability, overlapping-writer functionality and
  atomic visibility. Implement the testable subphases and UAT gate in
  [Phase 1D](2026-08-31-fuse-publication-throughput-plan.md#phase-1d-make-control-and-viewer-priority-non-bypassable)
  before resuming later throughput work. The first 1D.1 cut now preserves an
  immutable loader class/quantum through nested `WriteHandle` DATA operations
  and prevents loader writes from manufacturing viewer activity; 51/51
  filesystem/FUSE tests and both targeted control-isolation tests pass. The
  remainder of 1D.1 remains active. The first 1D.2 cut also makes old-generation
  materialisation resumable under the FUSE byte quantum; its end-to-end
  overwrite regression and all 53 filesystem/FUSE tests pass. Rebuild is now
  resumable too, and the redundant frontend-wide commit mutex has been removed
  without weakening the narrower metadata/path/content locks. Sparse
  changed-range reconstruction is now complete locally: untouched extents are
  reused without fetch/hash, touched extents alone are rebuilt, and completed
  cohort counters expose exact amplification. Publication notifications are
  also coalesced at durable watermarks, and only one pressure transition sweeps
  dirty inodes. Restart-boundary injection, synchronous durability/metadata
  completion, metadata-generation coalescing and resource-specific budgets
  remain active. See
  [the 1D.1 checkpoint](2026-08-31-fuse-publication-phase-1d-data-work-context.md)
  and
  [the materialisation checkpoint](2026-08-31-fuse-publication-phase-1d-resumable-materialization.md)
  and
  [the rebuild checkpoint](2026-08-31-fuse-publication-phase-1d-resumable-rebuild.md)
  and
  [the sparse changed-range checkpoint](2026-08-31-fuse-publication-phase-1d-sparse-changed-ranges.md)
  and
  [the notification-coalescing checkpoint](2026-08-31-fuse-publication-phase-1d-notification-coalescing.md).
  Pressure-aware retirement selection is now implemented and its focused
  full-spool proof, all 57 filesystem/FUSE tests and the controlled 228/228
  complete suite pass locally. Deployment and loaded UAT remain. The
  Transient-failure cursor preservation is now complete locally: a failed
  pipelined extent remains retryable in place and the publication retains its
  exact process-lifetime WAL/materialisation/rebuild cursor. The reserved
  retirement ticket and resource-specific priority bounds remain active. See
  [the retirement-selection checkpoint](2026-08-31-fuse-publication-phase-1d-retirement-selection.md).
  See also
  [the transient-failure cursor checkpoint](2026-08-31-fuse-publication-phase-1d-transient-failure-cursor.md).
- [ ] Correct overlapping-writer/full-spool publication collapse documented in
  [the live diagnosis](2026-08-31-overlapping-fuse-writer-rebuild-stall.md).
  Sparse changed-range reconstruction now prevents an interleaved overlay from
  rereading/rehashing the whole canonical file and preserves atomic visibility.
  Redundant per-write queue notifications and repeated pressure sweeps are now
  eliminated. Closed generations are now selected by greatest bounded
  retirement return under pressure, with nearest completion as the tie-break.
  The broader collapse remains active: coalesce compatible durable append
  ranges into fewer metadata generations, preserve bounded cursor progress
  across transient object failures, and reserve full-spool retirement progress
  so accepted bytes cannot remain pinned behind pathological or failing work.
- [ ] Fix stale FUSE mount recovery ordering. Startup currently calls
  `create_directories(mount_path)` before `prepare_fuse_mountpoint()`, so a
  disconnected Macha mount returns `ENOTCONN` before
  `fuse.unmount_if_mounted: true` can recover it. Add a regression around the
  preflight contract and retain refusal of unrelated filesystems.
- [ ] Phase 2: design and prove versioned durable incremental extent staging and
  safe spool-range retirement without exposing partial files.
- [ ] Phase 3: aggregate sequential local write descriptors and make durability
  group commit byte/urgency driven.
- [ ] Phase 4B: replace transient per-extent tasks with a shared byte-bounded
  data executor, add per-peer/storage-domain bounds, isolate all storage waits
  from communications, and aggregate compatible physical durability barriers.
- [ ] As part of Phase 4B, remove the avoidable 1 ms completion polling in
  `DistributedStore::put_impl`. The 2026-08-31 deployment sample found each
  outstanding deferred extent put in `sleep_for(1ms)` while publisher workers
  waited on their futures. Use event-driven completion/deadline notification,
  and ensure stalled/spilled RPCs cease to count as viable unfinished quorum
  work once every fallback has been exhausted.
- [ ] Replace the FUSE spool admission cliff with smooth, publication-rate-driven
  backpressure. The 2026-08-31 loaded UAT admitted about 1.4 GB at unrestricted
  speed, crossed the fixed 50% threshold (8.59 GB of 16 GiB), then accumulated
  169 seconds of throttle wait because no whole-file retirement had established
  a rate. Measure successful bounded partial-publication progress continuously
  and use a token-bucket-style controller that tapers admission as occupancy
  rises, retains a bounded fast initial burst, approaches sustainable publishing
  throughput without stop/start oscillation, and blocks only at the hard limit
  or when publication genuinely makes no progress. Add deterministic tests for
  smooth threshold crossing, zero-whole-file-retirement bootstrap, rate changes,
  hard-limit safety and event-driven wake-up, then repeat loaded rsync UAT and
  verify steady forward progress without weakening viewer/control priority.
- [ ] Phase 5: integrate continuous progress rates, occupancy hysteresis,
  concurrent-writer fairness, and authoritative catalogue hints.
- [ ] UAT the aggregate spool retirement-rate correction documented in
  [2026-08-31-spool-aggregate-retirement-rate.md](2026-08-31-spool-aggregate-retirement-rate.md).
  This UAT is paused until Phase 1C passes: the first attempt proved one correct
  aggregate sample but exposed FUSE `--append-verify` reads being misclassified
  as viewer traffic and the binary viewer gate starving publication. After the
  correction, observe at least two retirements above 50% occupancy and verify
  window bytes, elapsed time, admission pacing, hard-bound safety, weighted
  genuine-viewer priority and loader non-starvation.
- [ ] Verify the aggregated node-status API sometimes reporting a connected
  peer's `metadata_generation` as 0 while that peer's local API reports the
  current generation. Recheck the previously observed alternating disk-usage
  values at the same time and determine whether telemetry aggregation or the UI
  is substituting a missing sample with zero.
- [ ] Complete the final mixed-size rsync, concurrent-writer, communications,
  restart, peer-loss, and idle-soak verification matrix.

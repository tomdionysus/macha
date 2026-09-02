# Active tasks and concepts to explore

Last updated: 2026-09-02

This is the working backlog for the current session. Add new work here. When an item is implemented and its stated verification is complete, remove it from this file and add a dated entry with evidence to `COMPLETED.md`.

The existing documents in this directory remain the detailed plans, checkpoints, and UAT records. This file is only the current index.

## Deployment rule

- [ ] For every deployment, synchronize the complete source tree and all
  top-level build metadata needed by CMake. Never construct a remote build tree
  by copying only files believed to have changed. Configure after synchronization,
  build nodes in parallel, and verify installed binary versions and hashes on
  identical hardware before UAT.

## P0: bounded metadata history and crash recovery

- [ ] Execute the ranked source-wide
  [heap allocation and ownership audit](2026-09-01-heap-allocation-audit.md).
  The application-owned lifecycle fixes, bounds, RAII conversion and parallel
  regression suite are implemented and pass on macOS plus aarch64 ASan/LSan.
  Add the remaining large-index current/peak byte diagnostics and prove a stable
  loaded RSS plateau; allocator statistics alone are not an explanation.
  Implementation is tracked in the resumable
  [ownership/lifecycle remediation checkpoint](2026-09-02-ownership-lifecycle-remediation.md).

- [ ] Complete and UAT the
  [ingest heap-amplification remediation](2026-09-01-ingest-heap-amplification.md):
  fixed extent workers and pre-copy FUSE write byte admission are implemented;
  full-suite verification, four-node deployment and loaded RSS proof remain.

- [ ] Execute the phased
  [metadata-history memory remediation](2026-09-01-metadata-history-memory-remediation.md)
  before resuming loaded ingest/throughput UAT. Overnight rsync grew each
  replica's `history.log` from roughly 64 MB to about 2.1 GB; Macha retained the
  complete decrypted history plus a 64-entry full materialisation cache and the
  kernel OOM-killed node 50.
- [ ] Replace the disabled history compactor with an exact-head,
  cluster-acknowledged checkpoint/ancestry-floor protocol that is safe across
  partitions, returning nodes, same-generation siblings, concurrent notices,
  and restart.
- [ ] Extend the now-correct current-RSS Status reporting with current swap and
  separately named peak memory. Add pinned-cache byte diagnostics and
  pressure-triggered cache shedding; service-manager limits are a final safety
  net, not the primary fix.
- [ ] Complete live stale-FUSE recovery proof: preflight ordering is fixed, but
  UAT must hard-kill a mounted node and prove automatic detach, identity
  retention and clean generation rejoin.
- [ ] Remove serial remote `has_on` amplification from metadata publication
  retention. Live 0.22.2 recovery showed one recovered FUSE file commit holding
  `mutation_mutex_` inside `retain_metadata_publication()` while checking its
  extents remotely; GBNI-1's convergence/catalogue refresh waited behind it and
  RSS temporarily reached 2.5 GB. Batch or otherwise bound those checks without
  weakening retention-before-acceptance, and keep them off critical control
  traffic. See the
  [reconciliation recovery record](2026-09-01-metadata-reconciliation-recovery.md).
- [ ] Pass synthetic multi-gigabyte-equivalent history, branch/partition,
  unclean-restart and cache-pressure regressions, then run an overnight four-node
  rsync UAT. The gate requires bounded/stable RSS and swap, automatic node
  restart/rejoin, bounded history growth, writable metadata, no viewer/control
  regression, and no operator mount cleanup.

## Phase 2 namespace batching follow-up

- [ ] Design a durable batch identity that permits dependency chains such as create/rename/unlink to share a publication without weakening restart proof.
- [ ] Test mixed create/rename/unlink dependencies within a batch and across batch boundaries after that identity exists.
- [ ] Decide whether each published prefix member needs an explicit in-memory association with its accepted commit hash/generation.
- [ ] Preserve rename as a safe singleton boundary until the durable mixed-operation design and crash matrix are complete.

## Diagnostics and operational proof still needed

- [ ] Record a reproducible local benchmark recipe without default-suite timing thresholds.
- [ ] After the P0 byte-bounded cache and Status memory metrics land, repeat the
  namespace-burst UAT for at least three cache-fill/eviction rounds and prove
  current RSS and swap reach a stable ceiling and recover after drain.

## Separate known issue

- [ ] Diagnose and correct faulty torrent/ingest behaviour. Pausing the torrent removed the local node's residual CPU during Phase 3 idle UAT. Treat this as a separate subsystem investigation so it does not obscure metadata/convergence measurements.

## Deferred architectural concepts

- [ ] Consider delta-native commit identity based on parent identity, canonical delta, and a state-tree root instead of a fully serialized namespace payload.
- [ ] Consider a persistent or copy-on-write namespace tree with incremental subtree hashing.
- [ ] If either protocol-level design proceeds, define rolling-upgrade negotiation, checkpoint/anchor migration, and independent corruption validation first.

## Additional investigation

- [ ] Update the client to consume the immutable media-profile endpoint; see
  [the short client handoff](2026-08-31-clientside-media-profile.md).

- [ ] Complete real playback UAT for the dedicated media-information engine and
  idempotent admission. Implementation, 242/242 core tests, 3/3 runtime checks
  and four-node deployment are complete. From ES-1 verify an ordinary profiled
  session has no source reads; verify a genuine miss continues normal media
  negotiation rather than returning `profile_pending`; and record cold-process,
  repeated-session, same-key replay, and start/seek latency evidence in
  [the playback admission plan](2026-08-31-playback-immutable-media-profile-and-idempotent-admission.md).
  The 2026-09-01 loaded UAT proved a remaining violation: a profile miss did
  3,358 ms of synchronous remote inspection and session creation took 4,708 ms.
  Profile optimisation must remain advisory; a miss must not put speculative
  profiling on the admission critical path. The first remediation cut now
  preserves and retries completed profiles across catalogue publication
  conflicts without rescanning; local verification is 248/248. Deployment and
  loaded UAT remain; see
  [the playback reclamation checkpoint](2026-09-01-playback-profile-publication-and-pipeline-reclamation.md).

- [ ] Support multiple advertised endpoints per node and multiple candidate IPs
  per bootstrap node. A node must be able to advertise at least its local/LAN
  and internet/WAN endpoints simultaneously, with address family, scope and
  provenance sufficient for peers to choose an endpoint reachable from their
  own network position. Include ordinary poor-network behaviour in the design:
  transient connection loss, latency and packet loss must not destroy a logical
  playback attempt, create duplicate sessions or turn a retry into an ambiguous
  404. Reuse connections where practical, retry/reconcile creation with the
  same idempotency key, fail over among advertised endpoints, preserve precise
  server error codes, and test loss immediately after request commit and during
  playlist/fragment delivery.
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
- [ ] Replace provider-shaped public catalogue identities such as `tmdb:*` with
  stable, opaque Macha-owned entity IDs such as `macha:item:*`. Keep immutable
  `macha:<content-hash>` file identities separate; retain provider IDs only in
  server-internal provenance/matching indexes; omit them from normal client
  responses; atomically rewrite hierarchy/artwork associations; and preserve
  temporary server-side aliases so existing links, caches and client-owned
  playback state survive migration.
- [ ] Diagnose and correct leaked or over-retained playback transcode sessions.
  The live UI repeatedly reports `Macha playback request failed: video transcode
  limit reached` despite only two possible viewers (web on node 200 and one
  Samsung TV). Verify teardown on client disconnect, abandoned session-creation
  responses, playback end, source/player replacement, PATCH generation handover,
  DELETE, idle expiry, pipeline failure and node failover. Distinguish logical
  leases from running physical encoders, make teardown event-driven and bounded,
  expose enough session/pipeline age and ownership diagnostics to identify a
  leak, and add regressions proving dead or superseded transcodes promptly stop
  consuming admission capacity. The 2026-09-01 poor-Wi-Fi UAT reproduced this:
  after playback failed, node 50 retained two sessions, two audio transcodes and
  the sole allowed video transcode. The current 30-minute idle expiry makes a
  lost client DELETE capable of blocking subsequent playback for far too long.
  A configurable event-driven physical `pipeline_idle_ms` lease (60 seconds by
  default) is now implemented and locally tested without shortening the logical
  session lifetime. Keep this broader item active until loaded Wi-Fi UAT proves
  reclamation, replay/reconciliation and physical memory release.
- [ ] Investigate node 50 becoming unable to complete even a 30-second SSH banner
  during the 0.22.0 deployment build while the old Macha daemon remained active.
  Do not attribute this to a four-job compile without evidence: correlate Macha
  thread CPU, runnable/blocked tasks, memory/swap pressure, I/O wait, transport
  queues and publication/catalogue/playback activity. Verify that loader and
  background work cannot starve host control access, and repeat a controlled
  build both with Macha active and hard-stopped.
- [ ] Add an optional free-form human-readable `node_name` configuration value,
  advertise it through cluster telemetry, and expose it consistently at JSON
  path `.nodes[].node_name` in the aggregated Status response. Node identity,
  authentication and deduplication must continue to use the durable node ID;
  the name is display-only and may be empty or duplicated. Add parsing,
  propagation, compatibility and status-contract tests, then configure the four
  current nodes as `Corvus GBNI-1` (10.44.1.50), `Corvus GBNI-2`
  (10.44.1.51), `Corvus ES-1` (10.34.1.50), and `Corvus MacBook Pro`
  (10.44.1.200).
- [ ] Make playback negotiation representation-aware at the catalogue-item
  level. One human work identity (currently often provider-shaped, such as a
  TMDB item; ultimately an opaque Macha item ID) may reference many distinct
  immutable files: resolution/quality variants, codecs, containers, editions,
  languages, commentary tracks, separate audio/subtitle assets, or duplicate
  copies. Persist one media-information profile per immutable file ID, then
  plan across the complete representation set in this order: prefer the best
  appropriate direct-stream file; otherwise choose the cheapest correct remux,
  including a valid combination of separate assets where supported; otherwise
  transcode the cheapest suitable source while respecting requested quality and
  preferring the highest useful source quality. Preserve client preferences,
  stream language/default/forced semantics, availability, locality, seek cost,
  viewer priority and resource limits. Add deterministic multi-representation
  Direct/Remux/Transcode selection tests before changing the current planner.

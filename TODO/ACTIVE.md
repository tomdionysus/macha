# Active tasks and concepts to explore

Last updated: 2026-09-02

This is the working backlog for the current session. Add new work here. When an item is implemented and its stated verification is complete, remove it from this file and add a dated entry with evidence to `COMPLETED.md`.

The existing documents in this directory remain the detailed plans, checkpoints, and UAT records. This file is only the current index.

## Primary programme: structural ingest and runtime remediation

- [ ] Execute the phased
  [structural ingest and runtime remediation](2026-09-02-structural-ingest-runtime-remediation.md)
  before further throughput tuning or loaded ingest UAT. The 2026-09-02 live
  append-verify plus playback run showed node 50 growing from roughly 218 MiB
  RSS to over 1 GiB in about five minutes while completing only one file.
  Inspection found implementation-level amplification rather than a failed
  architecture: store-wide locks cover disk/crypto, existing object puts redo
  full payload work, FUSE reads copy and replay complete pending histories,
  retained memory is not governed process-wide, small metadata mutations do
  whole-snapshot work, profile pruning scans unstable whole-library views, and
  several expensive paths bypass work-class priority.
- [ ] Treat the plan's Phase 0–3 gates as P0 and primary. Do not resume loaded
  rsync, overnight ingest or throughput UAT until object-store priority,
  append-verify complexity and process-wide retained-memory bounds pass their
  deterministic tests.
- [ ] Treat Phases 4–7 as the continuation of the same programme, not optional
  optimisation. They remove metadata/publication amplification, causally unsafe
  profile GC, priority bypass and diagnostic amplification.
- [ ] Remediate the failed short guarded UAT defined by the
  [first structural runtime checkpoint](2026-09-02-structural-runtime-first-checkpoint.md).
  The indexed overlay passed with one range examined per read, but GBNI-1 RSS
  rose from 498,794,496 to 992,722,944 bytes in about 65 seconds and crossed the
  confirming growth-rate stop gate. Stopping also exposed an uncaught
  `hydration executor is stopping` exception on GBNI-1. Complete retained-owner
  diagnostics and shutdown lifecycle regressions before repeating loaded UAT;
  see the [UAT record](2026-09-02-structural-runtime-first-uat.md). The shutdown
  race and first retained-owner instrumentation are now locally corrected and
  260/260 core plus 3/3 runtime tests pass; deploy only for the guarded
  attribution UAT described in the
  [checkpoint](2026-09-02-shutdown-and-retained-owner-checkpoint.md).
- [ ] Correct the playback-local memory/copy amplification isolated in the
  [rsync/playback attribution UAT](2026-09-02-isolated-rsync-playback-uat.md).
  Rsync-only plateaued near 445 MiB, while a clean transcode-only run rose from
  roughly 386 MiB to 790 MiB and retained roughly 205 MiB above baseline after
  Status reported no session, pipeline or segment ownership. Prioritise active
  RPC/reassembly/object payload accounting, elimination of repeated 4 MiB
  stripe copies, bounded DATA connection ownership and physical teardown.
  The first correction now single-flights cold connection creation per
  authenticated peer/lane; a 24-caller regression creates exactly one DATA
  transport and the 261/261 core plus 3/3 runtime suites pass. Its clean UAT
  proved one canonical connection but still retained about 258 MiB after
  teardown; a diagnostic glibc trim returned that memory immediately. The
  event-driven playback cleanup worker now requests Linux heap reclamation only
  after the final transformed pipeline has stopped, with regression and Status
  coverage; 23/23 playback, 261/261 core and 3/3 runtime tests pass. Four-node
  UAT then returned GBNI-1 from approximately 783 MiB live to 406 MiB
  immediately after teardown (one request/run/success), passing the physical
  teardown gate. Continue reducing the separately observed roughly 385 MiB
  live transcode increase, full-stripe reply copies and 7.56-second startup.

### Backlog precedence and supersession

- Existing completed checkpoints remain valid evidence for exactly the layer
  they tested, but they do not override a newly demonstrated lower-layer
  failure. In particular, zero DATA-arbiter viewer waits is not end-to-end proof
  that the viewer did not wait on store locks, CPU, disk, metadata or logging.
- The active heap audit, ingest heap remediation, Phase 1D priority work, Phase
  2/3 descriptor and metadata batching, Phase 4B executor work, metadata
  representation concepts, and profile lifecycle work are merged into the new
  primary plan at the phase named in its `Superseded or paused work` section.
- Smooth spool-throttle tuning and aggregate throughput UAT are paused. Their
  current input rate is distorted by avoidable work amplification; tuning now
  would preserve the wrong behaviour.
- [ ] Make spool backpressure smooth and visibly progressive once occupancy is
  high. The 2026-09-02 rsync-only isolation run remained memory-stable but, at
  17,179,856,131 bytes against the 16 GiB limit, rsync appeared frozen: accepted
  bytes and progress stopped instead of converging toward the measured
  publication/drain rate. Replace the hard-wall behaviour with bounded,
  occupancy-aware pacing that continues to acknowledge incremental progress,
  while preserving the viewer/control priority laws. Resume this tuning only
  after the primary Phase 1-3 amplification and ownership gates pass.
- Any older task that proposes another independently bounded queue or accepts
  admission-layer priority as the final invariant is superseded. Its document
  is retained as historical evidence, not as current design authority.

## Deployment rule

- [ ] For every deployment, synchronize the complete source tree and all
  top-level build metadata needed by CMake. Never construct a remote build tree
  by copying only files believed to have changed. Configure after synchronization,
  build nodes in parallel, and verify installed binary versions and hashes on
  identical hardware before UAT.

## Preserved P0 safety work, subordinate to the primary Phase 0–3 gates

- The remaining work from the
  [heap allocation and ownership audit](2026-09-01-heap-allocation-audit.md),
  [ownership/lifecycle checkpoint](2026-09-02-ownership-lifecycle-remediation.md)
  and
  [ingest heap-amplification remediation](2026-09-01-ingest-heap-amplification.md)
  is absorbed into primary Phases 0–3. Their RAII, fixed-worker and pre-copy
  admission changes remain useful completed evidence, but their previous loaded
  RSS acceptance gate is replaced by the process-wide ownership and stable-RSS
  gate in the primary plan.

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

## Namespace batching requirements absorbed into primary Phase 4

- [ ] Design a durable batch identity that permits dependency chains such as create/rename/unlink to share a publication without weakening restart proof.
- [ ] Test mixed create/rename/unlink dependencies within a batch and across batch boundaries after that identity exists.
- [ ] Decide whether each published prefix member needs an explicit in-memory association with its accepted commit hash/generation.
- [ ] Preserve rename as a safe singleton boundary until the durable mixed-operation design and crash matrix are complete.

## Diagnostics and operational proof still needed

- [ ] Make storage-capacity telemetry explicitly unavailable while a backend's
  crash-recovery accounting scan is still reconciling. A live 0.22.2 restart on
  GBNI-1 had `/mnt/diskB` mounted read-write with its objects/packs present and
  312 GB physically used, but Status advertised authoritative `used_bytes: 0`
  and the full 8 TiB quota free until the recursive scan completed. Preserve
  capacity and backend-online state, publish null/unavailable used/free values,
  expose the reconciliation state, and add restart/status regressions.

- [ ] Record a reproducible local benchmark recipe without default-suite timing thresholds.
- [ ] After the P0 byte-bounded cache and Status memory metrics land, repeat the
  namespace-burst UAT for at least three cache-fill/eviction rounds and prove
  current RSS and swap reach a stable ceiling and recover after drain.

## Separate known issue

- [ ] Diagnose and correct faulty torrent/ingest behaviour. Pausing the torrent removed the local node's residual CPU during Phase 3 idle UAT. Treat this as a separate subsystem investigation so it does not obscure metadata/convergence measurements.

## Architectural concepts absorbed into primary Phase 5

- Delta-native commit identity and a persistent/copy-on-write namespace tree are
  no longer detached concepts. They are design candidates for primary Phase 5,
  whose acceptance requirement is mutation cost proportional to changed state.
- Rolling-upgrade negotiation, checkpoint/anchor migration and independent
  corruption validation remain mandatory prerequisites to any wire-identity
  change.

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

### Historical FUSE throughput plan — absorbed and paused

The detailed
[FUSE publication throughput plan](2026-08-31-fuse-publication-throughput-plan.md)
and its Phase 1A–1D/4A checkpoints remain evidence of useful bounded-quanta,
resumable-rebuild, sparse-range, notification-coalescing, retirement-selection,
DATA-headroom and transient-cursor work. They are no longer a separate active
programme.

Their remaining work is reassigned as follows:

- object/RPC isolation, event-driven completion and durability coalescing:
  primary Phase 1;
- incremental extent staging, sequential descriptor aggregation, overlapping
  writers and safe spool retirement: primary Phase 2 and Phase 4;
- shared byte-bounded execution, per-peer/domain bounds and resource-specific
  priority: primary Phase 1 and Phase 3, under the single process-wide ledger;
- end-to-end `control > viewer >> loader > speculative`: primary Phase 7;
- smooth spool control, aggregate retirement-rate UAT and final throughput
  matrix: paused until primary Phases 1–3 pass.

This explicitly drops the earlier claim that DATA-headroom counters or weighted
publication alone establish the viewer invariant. They establish only their
local admission behaviour. The older documents are not deleted because their
tests and measurements remain useful.

- [ ] Verify the aggregated node-status API sometimes reporting a connected
  peer's `metadata_generation` as 0 while that peer's local API reports the
  current generation. Recheck the previously observed alternating disk-usage
  values at the same time and determine whether telemetry aggregation or the UI
  is substituting a missing sample with zero.
- The former final mixed-size rsync, concurrent-writer, communications,
  restart, peer-loss and idle-soak matrix is absorbed into the primary Phase 7
  exit gate and remains paused until Phases 1–3 pass.
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

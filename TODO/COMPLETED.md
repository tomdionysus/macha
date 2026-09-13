# Completed and tested

Last updated: 2026-09-13

The 2026-09-08 entries below were ledgered by a pruning pass over
`ACTIVE.md`, and cover only the items that pass removed from that file.
0.24.1–0.35.0 is not otherwise ledgered here yet — see the documentation
hygiene item in `ACTIVE.md`.

## Cluster users, passwords and roles — 0.38.0, deployed 2026-09-12

Supersedes the "No authorization tiers yet" P0 in `ACTIVE.md`. Design and the
seven places the implementation diverged from it are in
`2026-09-12-cluster-users-and-roles-plan.md` (see its "What actually shipped"
section — the plan itself was not edited, so it stays usable as evidence).

- [x] **Cluster-replicated users, passwords and roles.** `UserStore`
  (`src/users.hpp`), `user_sync` RPC, `/api/v1/users`, `macha-users`.
  Passwords are scrypt with per-record parameters; the table is sealed at rest
  under an HKDF subkey, because it replicates to an offsite node. Verifying a
  password reads only the local replica — no RPC, no metadata, no catalogue —
  so a node that is alone, or whose metadata has gone read-only, still
  authenticates.
- [x] **Roles are capabilities, not a ladder.** `media_viewer`, `importer`,
  `manager`, `manage_users`; every role implies `media_viewer` and nothing else
  implies anything. Resolved at mint time, gated in one place before dispatch
  (`Service::required_role`).
- [x] **root and anonymous are ordinary accounts** created once by the founding
  node, neither renameable nor deletable. Anonymous access is the anonymous
  account's roles rather than a config key. At least one account always holds
  `manage_users`, enforced in `UserStore` rather than only the API.
- [x] **Three latent session defects found and fixed on the way.** Session
  gossip had never once run since 0.24.0 (wrong frame class on send,
  undispatched on receive); `propagate_session` blocked login for up to 30 s per
  unreachable-but-active peer; periodic gossip drew from the same memory budget
  as data work and could wedge a loader (fixed by moving session/user gossip to
  `FrameType::control`).
- [x] **Gossip re-announces every 30 s.** `broadcast_best_effort()` reports
  frames queued, not delivered, so a peer whose inbound route is not usable yet
  can be marked told having received nothing. Found during this release's own
  rollout: an upgraded node came up with an empty user table and refused every
  request until the sending node happened to restart.
- [x] **`DataResourceArbiter::acquire` no longer waits for ever.** It waits in
  no-progress windows — any release resets the window — and logs the class,
  size and used/active/waiting counts when it gives up. New
  `dht.data_credit_no_progress_deadline_ms`, 120 s.
- [x] **`test_storage_data_credit_reserves` no longer depends on the host.**
  `maintenance.background_concurrency` defaults to `hardware_concurrency() / 2`,
  so its three loader acquires deadlocked for 360 s on any machine with fewer
  than six cores — every node in this cluster — while passing on a twelve-core
  development machine. The test now states the ceiling it means to test. This is
  the "passes in isolation" class `ACTIVE.md` had already flagged; the cheap
  check (core count) was the one that found it.
- [x] **`catalogue.api.advertised_endpoint` is validated at load.** The shipped
  example had always claimed a path was "rejected at startup"; nothing checked.

Deliberately not built: **no recovery key and no recovery endpoint.** One must
be presentable without an account to be useful, which means a standing
unauthenticated path to the most privileged account in the cluster, bought with
a capability that already exists behind strictly more access — anyone who could
present one has root on a node, where `macha-users passwd root` does the same
job. The machinery (an X25519 envelope sealing the cluster key, so a key can be
verified without anything derived from it being stored) is implemented and
tested but uncalled; `test_no_recovery_route_is_exposed` stops the route being
reintroduced by accident, and `macha-recover` ships saying so.

## libtorrent port mapping stated, and an unbindable advertise refused — 0.38.2

- [x] **`torrent.upnp` and `torrent.natpmp` exposed.** libtorrent maps its own
  listen port and both default on inside libtorrent, so the session created
  router mappings regardless of configuration: a node with
  `network.upnp.enabled: false` still had 6881 mapped. Defaults unchanged
  (true), so no node's behaviour shifted — the point is that it is now
  refusable and visible.
- [x] **`torrent_listen_interfaces()` refuses an address it cannot bind.**
  0.37.2 made the session bind the node's advertised address, correct while
  that was a LAN IP. Once nodes advertised public DNS names it bound *nothing*,
  silently: `listen_interfaces` takes an IP literal or a device name, never a
  hostname. It now uses the advertised address only when it parses as an IP
  literal, and otherwise binds every interface and says so.
- [x] **Port-mapping outcomes leave DEBUG.** Success once at info, failure once
  at warn with what to do about it. gbni-1 ran for hours with "no router found"
  and nothing above debug mentioned it, while its peer counts stayed low and it
  could not seed.

## es-1 publication livelock — 0.36.8 + 0.36.9, deployed 2026-09-09

- [x] **The whole of the "es-1 publication livelock starves RPC and takes the
  node out of the cluster" item from `ACTIVE.md`.** Two defects, one in each
  release, and the item's own diagnosis of *why* the parking discipline could
  not catch it turned out to be exactly right: the work never failed, so
  nothing could park it.

  **0.36.8 — reassembly starvation and an unbounded wait.** `MessageAssembler`
  could not get a retained-memory lease to reassemble an inbound frame, threw
  `process retained-memory RPC reassembly saturated` and killed the channel
  about once a second, so peer channels died 1–2 s after connecting and
  telemetry — the only consumer that never re-dials — appeared to vanish. The
  loop is closed exactly as the item described: publication holds its bytes
  until a peer confirms, the confirmation is a frame that must be reassembled
  into the same ledger, and that reassembly is refused because publication is
  waiting. Fixed with `runtime.reassembly_memory_reserve_bytes` (32 MB),
  placed below the control/viewer waiter gate and above the loader gate and
  durable-lower budget — both halves load-bearing, since above every gate it
  inverts the deadlock and above the viewer gate it breaks governing law 1.
  Separately, the publication `DataWorkContext` carried no deadline at all, so
  the wait took the unbounded `cv_.wait` branch and all eight commit workers
  sat in `ensure_buffer_memory` holding 492 MB between them.
  `fuse.publication_no_progress_deadline_ms` (30 s) makes that a bounded
  no-progress budget that enters the ordinary retry/park path.

  **0.36.9 — the hold-and-wait itself.** Every byte of `owners.publication` is
  a `WriteHandle` extent lease. A writer is retained across clean yields and
  retryable failures so a resumed publication never replays spool bytes, and
  it keeps its leases while retained; publication scheduling is otherwise
  breadth-first, so the number of writers holding partial state was simply the
  width of the backlog — 123 leases at a 4 MiB extent, the entire durable-lower
  budget. No byte budget could fix it, which is why lowering
  `publication_inflight_bytes` from 256M to 96M had changed the inflight figure
  and left `owners.publication` at 515,899,392 unchanged.
  `fuse.publication_max_open_writers` bounds how many inodes may hold a writer,
  derived from `runtime.loader_memory_reserve_bytes`.

  **Evidence.** es-1 before: 317 publications started, **0** completed,
  `owners.publication` 515,899,392 byte-identical across restarts at 99.7% of
  the durable-lower budget, spool 8,589,783,970 draining at 0 B/s, 190 backend
  failures. After the deploy the spool drained to **0** with
  `data_publication_bytes_confirmed` reading exactly 8,589,783,970 — every
  byte — in about 45 minutes. Over the following 15 hours under live ingest the
  cluster published ~475 GB with `parked_publications` 0, `waits.loader` 0 on
  every node, peak ledger use 156 MB of 768 MB, and zero warnings or errors.
  A viewer request had been failing on this node with `viewer fragment memory
  admission unavailable` before the fix, so governing law 1 was being violated
  in practice, not merely at risk.

  Two defects **in the fix itself** were found the next day and remain open in
  `ACTIVE.md`: the no-progress counter misses `rebuild_step` and the
  non-pipelined `flush()` and counts the wrong thing anyway, and the
  open-writer bound is soft and can overshoot by up to `commit_workers - 1`.
  Neither has fired. Plan and post-deploy audit in
  [`2026-09-09-publication-hold-and-wait-plan.md`](2026-09-09-publication-hold-and-wait-plan.md);
  the handover that preceded it is
  [`2026-09-09-publication-livelock-continuation.md`](2026-09-09-publication-livelock-continuation.md).

## Self-healing disciplines — 0.29.0 → 0.32.0

- [x] Executed the whole of
  [the self-healing disciplines plan](2026-09-06-self-healing-disciplines-plan.md),
  written after one afternoon of ordinary load surfaced six P0 defects, each
  hidden behind the previous, all instances of four habits: trusting
  bookkeeping over re-derivable truth, retries that turn "not yet" into
  "forever", recovery that refuses instead of resolving, and snapshots that
  carry retirement history.
- [x] 0.29.0 — durability is re-derived from disk, not asserted from a dead
  token. This closed the `object durability quorum unavailable before
  publication` wedge: three recovered inodes on gbni-1 had retried
  `required=1 durable=0` on the same object ids for the life of each process
  while 14.5 GB sat in the spool and the WAN socket idled. The barrier now
  probes the restarted peer with the object ids and re-stamps the batch, so
  a requirement naming a dead `durability_epoch` no longer outlives that
  peer's restart.
- [x] 0.30.0 — "not yet" never becomes "forever": one `RetryPolicy` per work
  item with backoff and a terminal parked outcome an operator resolves
  (`parked_publications`, `manage/parked-publications`), replacing three hot
  loops including the ~40/s spin on `FUSE namespace advanced during data
  publication` during startup recovery. The 120 s `service_startup_timeout_ms`
  elapsed-time gate — which turned a slow-but-progressing 5-minute replay into
  an infinite crash loop eight times on gbni-1 — became a no-progress gate
  (`startup_progress.hpp`, `service_startup_no_progress_ms`, absolute ceiling
  now off by default). Regression:
  `test_service_startup_gate_waits_while_recovery_progresses`.
- [x] 0.31.0 — recovery resolves rather than refusing, with a journal fuzz
  fixture (`test_fuse_journal_fuzz_every_frame_mutation_still_starts`).
- [x] 0.32.0 — DLT7: a flags byte per topology set, so a merge delta no longer
  has to resend the whole standing conflict set (~305 KB on the cluster, ~100
  merges/day × 3 replicas of pure history growth) whenever `merge_parents`
  changes. Plus canonical tombstones and conflicts compacted out of the
  snapshot.
- [x] Evidence per discipline, and the closing two-writer / rolling-restart
  run, in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md).
  Follow-ups filed there remain open and are indexed in `ACTIVE.md`.

## Retention-check batching and cheap presence — 0.26.0, 0.32.7–0.32.10

- [x] Root-caused and fixed the live incident where a bulk movies rsync to
  `corvus-es-1` froze data publication entirely and pinned the maintenance
  thread near 100% CPU for minutes: `DistributedStore::retain_data`'s serial
  per-extent `has_on` loop, each call either a full local decrypt+hash or a
  synchronous control-plane RPC, producing a hard `retention claim … control
  RPC deadline exceeded` against an unrelated peer — a direct
  governing-law-3 violation. Plan, root-cause trace and the correction the
  test suite caught mid-implementation (`rebalance_step`/`repair_step`'s
  presence checks could *not* move to the cheap path, having no downstream
  re-verification) in
  [retention-check batching and a cheap local presence check](2026-09-06-retention-check-batching-and-cheap-presence-plan.md).
- [x] This subsumed the separately-filed scaling item that `LocalStore::valid()`
  was used as a cheap presence check while doing a full read + AES-GCM decrypt
  + SHA-256.
- [x] Measured on the real three-node cluster during a live bulk import
  (0.32.7 remote claims are `has()` not re-reads; 0.32.8 parallel barrier
  fan-out; 0.32.9 presence remembered rather than `stat`'ed; 0.32.10 CONTROL
  puts together to the nearest replica): es-1 retention avg 5,217 ms → 121 ms,
  publish avg 556 ms, no `deadline exceeded` or `peer closed` since 0.32.7.
  Numbers in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md).
  The N² retention-journal growth that run also recorded stays open in
  `ACTIVE.md`.

## Correctness and security fixes — 0.24.1 → 0.24.4

- [x] **`NodeRuntime::accept_history_checkpoint_proposal()` accepted
  unconditionally — 0.24.1.** Confirmed as the live root cause of a real
  outage, not a theoretical gap: a lagging proposer whose own survey was
  already stale got an unconditional ack from every participant for a floor
  they had moved past, then durably committed and compacted *itself* to that
  stale floor, discarding the only shared ancestry two-parent reconciliation
  had — which is exactly what produced "divergent metadata heads have no known
  common ancestor" across all three nodes on 2026-09-05 (confirmed by
  cross-node journalctl timing). `MetadataReplica::record_checkpoint_ack()`
  now validates the proposal's `floor_hash` against this replica's own
  accepted head before acking, on both the RPC and local-owner paths.
  Regression:
  `test_history_checkpoint_ack_refuses_a_floor_this_replica_has_already_superseded`.
  This prevents recurrence; it did not retroactively restore ancestry already
  discarded.
- [x] **Data race on `LocalStore::last_mutation_generation_` — 0.24.3.**
  `scan()`'s write moved under `m_`, matching `durability_barrier()`'s read.
- [x] **Cleared ingest jobs could resurrect — 0.24.3.** Seven sites (one more
  than originally scoped) in `src/ingest.cpp` moved from unguarded
  `jobs_[job.id]` to `find()`-guarded access, matching what
  `should_pause_or_cancel()` already did. Regression:
  `test_cleared_ingest_job_does_not_resurrect_while_worker_finishes`.
- [x] **`MetadataReplica::accept_commit()` held the global replica mutex
  across `persist()` — 0.24.3.** Writing the entire snapshot payload
  (potentially hundreds of MB) on the single metadata RPC worker. The
  checkpoint write now happens after `m_` is released, serialized only against
  other durable-mutation writers, mirroring the off-lock-write /
  on-lock-bookkeeping pattern `import_history()` already used.
- [x] **Wedged namespace queue on a non-retryable error — escape hatch in
  0.24.3, fixed forward in 0.24.4.** `GET/POST
  /api/v1/manage/filesystem/blocked-namespace-operation[/skip]` lets an
  operator abandon a wedged operation by exact sequence number; never
  automatic. The 0.24.3 version of this fix had a journal-bookkeeping bug that
  crash-looped a node on restart — which is what put the subsystem crash
  isolation item on the backlog.
- [x] **Non-constant-time bearer token comparison — closed as a side effect of
  0.24.0.** `http.cpp` no longer holds or compares a raw bearer token;
  `SessionManager::validate()` hashes the presented token (SHA-256) before any
  lookup, so the untrusted byte comparison no longer exists.

## Breaking config change: `mount_path` moved to `fuse.mount_path` — 0.25.0

- [x] Top-level `mount_path` became a hard startup error ("obsolete
  configuration key: mount_path (moved to fuse.mount_path)") rather than
  silently ignored. Every node's `macha.yaml` has since moved its line under
  `fuse:` — all three cluster nodes run 0.34.0.

## gbni-1 SSH unresponsiveness during builds — diagnosed, not a code defect

- [x] "Node 50 loses SSH responsiveness with Macha active during a build" was
  diagnosed as gbni-1 browning out under load (power/undervolt), not a Macha
  scheduling or memory defect. It is now a standing operational rule rather
  than an open investigation: never run `cmake --build` on gbni-1 — build on
  gbni-2 and ship the staged `make install DESTDIR=` tree — and rate-cap its
  rsync (`--bwlimit`, `nice -n 10 ionice -c3`).

## Cluster session/auth subsystem, `/api/v1/session` — 0.24.0

- [x] Shipped `POST/GET/DELETE /api/v1/session`: anonymous-only bearer-token
  session creation, introspection and revocation. Every non-exempt API route
  now requires `Authorization: Bearer <token>` — this closes the
  previously-logged "auth optional when `token_file` unset" P0 security gap
  as a side effect of replacing the mechanism entirely.
- [x] Sessions are cluster-replicated (push-on-mutation plus periodic gossip
  backstop, modelled on `TelemetryStore`) and persisted locally, with each
  node's own in-memory replica doubling as its O(1) lookup cache — no
  separate cache layer, so a pushed mutation is the invalidation.
- [x] Retired `Macha-Viewer-Session`/`X-Macha-Viewer-Session`/
  `viewer_session_id` in favour of the authenticated session id for playback
  logical-viewer correlation, and moved playback's `Idempotency-Key` from a
  request header to an `idempotency_key` query parameter.
- [x] Built extension seams for later work, not yet used: `CredentialValidator`
  (real, non-anonymous credentials) and `session_has_role()` (per-route/action
  role gating — still not wired into any route).
- [x] Fixed during joint live testing: `create()` didn't enforce
  `max_sessions` (only the gossip `apply()` path did) — a client-side timer
  bug caused a real re-mint storm against a live dev node. `create()` now
  returns `nullopt`/`429 too_many_sessions` at capacity; regression test
  `test_session_create_enforces_max_sessions` added.
- [x] Coordinated the client-side contract with the Macha UI Work session in
  parallel; verified end-to-end against a live running node (create → gated
  route with/without token → introspect → revoke → post-revoke rejection).
- [x] Closed the separately-logged "non-constant-time bearer token comparison"
  P0 security item as a side effect of this rework: `http.cpp` no longer holds
  or compares a raw bearer token at all (the old `bearer_token_`/`read_token()`
  plain `!=` compare was removed entirely). `SessionManager::validate()`
  (`src/session.cpp`) hashes the presented token with SHA-256 before any
  lookup and does a `std::map` lookup on the digest, matching the plan's
  original design reasoning — confirmed by code audit on 2026-09-05, no fix
  needed.

Evidence: `CHANGELOG.md` 0.24.0.

2026-09-05: backfilled the eight releases (0.23.4–0.23.11) that shipped after
this ledger's previous update, found stale during a full backlog
reprioritisation pass. Full detail for each remains in `CHANGELOG.md`; entries
below are intentionally brief pointers rather than re-narrated detail.

## Snap transcode seeks to source keyframes — 0.23.11

- [x] Fixed a seek deep into a transcoded title costing ~7s to first fragment
  by snapping the transcode seek to the nearest source keyframe
  (`media_vod::nearest_keyframe_at_or_after`), without reusing remux's
  whole-file density-checked `indexed_plan`, which was observed to silently
  reject the snap on long files with any sparser GOP elsewhere.
- [x] Fixed a rounding bug (`llround` → `ceil` on the keyframe timestamp) that
  was making the container seek land one keyframe early, discarding a full
  extra GOP for nothing. Verified live: ~7s → ~4s. A residual ~1.7s gap vs.
  position-zero remains unexplained and is intentionally left open — tracked
  under playback P0 item 1 above via the `media playback pipeline seek timing`
  diagnostic added to isolate it.

Evidence: `CHANGELOG.md` 0.23.11.

## Fix seek/generation-replacement stall on stale segment requests — 0.23.10

- [x] Fixed a segment request against a superseded playback generation
  blocking for the full 15s `streaming.startup_timeout_ms` before returning
  404, via a reversible `MediaSegmentStore::mark_superseded(bool)` signal
  distinct from the irreversible `cancel()`.
- [x] Not confirmed as the cause of a separately reported ~1-2s live A/V
  offset symptom — that investigation continued into the audio-drift work
  above.

Evidence: `CHANGELOG.md` 0.23.10.

## Fix audible pitch shift from drift correction, and the drift fix itself — 0.23.8/0.23.9

- [x] 0.23.8 shipped bounded audio drift compensation for transcoded playback
  (`audio_next_pts` re-anchoring), measured live at ~0.4ms drift/second before
  the fix.
- [x] 0.23.9 replaced 0.23.8's `swr_set_compensation()` mechanism (which
  worked but caused an audible pitch shift, since nudging the resample ratio
  changes pitch) with libswresample's own `async=1` + `swr_next_pts()`
  correction, which injects/drops samples rather than changing the ratio.
  Verified: healthy audio over 8 minutes, drift within ±15ms.
- [x] Known gap carried forward: no automated regression harness yet for the
  real (non-stub) transcode audio path — tracked under playback P0 item 1
  above.

Evidence: `CHANGELOG.md` 0.23.8, 0.23.9.

## Per-node advertised API address for any-node Direct Play failover — 0.23.7

- [x] `/api/v1/status` now reports `api_host`/`api_port` per node, distinct
  from the internal RPC bind address, fixing failover guessing the wrong port.
- [x] Added optional `catalogue.api.advertised_host`/`advertised_port` for
  NAT/port-forwarding. Static config-only for now — UPnP/external-IP probing
  of this endpoint remains open under P1 above.

Evidence: `CHANGELOG.md` 0.23.7.

## Safe distributed checkpoint and metadata-history compaction — 0.23.6

- [x] `MetadataReplica::compact_history_if_safe()` — an existing, already-tested
  primitive with zero production callers — is now actually invoked, via a
  leaderless propose → durable-ack → commit → prune round gated on every
  durably-known participant acknowledging the same accepted-head hash
  (mirroring the existing `all_known_reachable()` gate used for destructive
  object GC). Unbounded `history.log` growth (the direct cause of a prior
  37MB → 20.9GB blowup) is fixed; a returning node whose floor was pruned
  elsewhere now converges automatically.
- [x] No client-visible behaviour change. Confirmed directly against
  `src/metadata.cpp` during the 2026-09-05 code audit as real and complete,
  not just a changelog claim — see the still-open byte-bounded-cache/shedding
  follow-up under the structural-safety P0 tier above.

Evidence: `CHANGELOG.md` 0.23.6.

## Cluster-wide ingest/torrent job visibility and control — 0.23.5

- [x] `GET /api/v1/ingest/jobs` and `/api/v1/torrents/jobs` now answer with
  every job in the cluster (on-demand RPC survey, not replication), each
  tagged with its owning `node_id`; an unreachable peer is skipped rather than
  failing the whole request.
- [x] `POST .../jobs/{id}/{pause,resume,retry,cancel,clear}` forwards to the
  owning node when the receiving node doesn't have the job locally, preserving
  404-vs-409 semantics cluster-wide.

Evidence: `CHANGELOG.md` 0.23.5.

## Signed artwork capability URLs — 0.23.4

- [x] Catalogue responses embed a signed, short-lived capability URL
  (`?exp=&sig=`, HMAC'd with the cluster auth key) on each artwork reference,
  letting a client use a plain `<img src>` without a bearer header. Default
  TTL 24h. Note: the URL is not yet actually cache-friendly — see the P2
  catalogue item above about `exp`/`sig` needing to be quantized so repeated
  fetches produce an identical URL.

Evidence: `CHANGELOG.md` 0.23.4.

## Remove per-request connection setup from Status and streaming — 0.23.3

- [x] `http.cpp` negotiates HTTP/1.1 keep-alive by default, with bounded
  `keep_alive_max_requests`/`keep_alive_idle_timeout`, falling back to
  `Connection: close` only on explicit request, protocol/version mismatch, or
  backlog pressure. Verified directly against current `src/http.cpp`.
- [x] Also shipped in 0.23.3: truthful per-node `phase` (fixing the rolling-
  deployment incident where recovering nodes reported fabricated-looking
  healthy zeros) and serialized foreground metadata-head reconciliation
  (fixing a live 37MB → 20.9GB metadata-history blowup in under a week). The
  *aggregation* half of Status truthfulness was later found still open — see
  the P1 item above.

Evidence: `CHANGELOG.md` 0.23.3.

## Bounded terminal FUSE recovery failure

- [x] Reproduced GBNI-1's crash-recovery state in which durable spooled writes
  target a path absent from the accepted namespace.
- [x] Prevented a terminally poisoned inode from being re-admitted indefinitely,
  while preserving its journal/spool and leaving retryable failure semantics
  unchanged.
- [x] Passed 65/65 FUSE tests, 277/277 complete core tests and 4/4 runtime tests.
- [x] Deployed complete source as 0.23.1 to all four nodes. The three Linux
  binaries are byte-identical. The exact live inode-922 failure occurred once,
  stayed settled, and left the mount and local API responsive.

Evidence: [terminal FUSE recovery loop](2026-09-03-fuse-terminal-recovery-loop.md)

## Persistent logical-viewer transcode admission

- [x] Replaced physical-running-pipeline admission accounting with one retained
  video/audio entitlement per persistent logical player/UI session.
- [x] Added `Macha-Viewer-Session` reconciliation for replacement POSTs while
  preserving PATCH, generation, idempotency and ownerless ephemeral-session
  semantics.
- [x] Serialized child-pipeline handover, retained entitlement through idle
  reclaim and Direct/Remux transitions, and released it only at DELETE/expiry.
- [x] Added separate physical-pipeline diagnostics and deterministic coverage
  for replacement POSTs, repeated seeks, every playback mode, contention,
  reclamation and exact capacity release.
- [x] Passed 24/24 playback tests, 276/276 complete core tests and 4/4 runtime
  dependency tests; deployed complete source to all four nodes with identical
  Linux binaries and a healthy, writable generation-3904 clean baseline.

Evidence: [logical viewer transcode admission](2026-09-03-logical-viewer-transcode-admission.md)

## Phase 2 bounded FUSE operation metadata

- [x] Added a configurable aggregate heap admission bound for durable `DataOp`
  history, checksum-vector capacity and a concurrent publication snapshot.
- [x] Made saturation wake publication and wait on durability/retirement events;
  acknowledged work remains durable and recovery-safe.
- [x] Added current/peak/limit/wait diagnostics and hard-bound plus liveness
  regressions. The complete filesystem/FUSE suite passed 64/64.

Evidence: [Phase 2 operation-metadata bound](2026-09-02-phase-2-operation-metadata-bound.md)

This is the completed-work ledger for the current session. An item belongs here only after implementation and its stated verification are complete. Detailed design notes, exact test results, and UAT measurements remain in the linked records.

## Hydration shutdown ownership and first retained-owner diagnostics

- [x] Reproduced the live late-submit race which threw `hydration executor is
  stopping` through a scheduler `std::jthread` and aborted GBNI-1 during stop.
- [x] Made late submission a normal rejected result, completed executor-owned
  queued promises before join, and added explicit cancellation/rejection
  accounting plus a deterministic blocked-provider shutdown regression.
- [x] Added O(1) current retained-owner counters for FUSE operations, checksums,
  overlay ranges, publication snapshots, durability tickets and publication
  pipeline bytes; clean completed inodes release empty operation-vector
  capacity.
- [x] Added active playback fragment resident, spill, descriptor and VOD-plan
  ownership counters.
- [x] Passed 62/62 focused FUSE tests, a clean 260/260 parallel core run, and
  3/3 runtime tests. Loaded stable-RSS acceptance remains active.

Evidence: [shutdown and retained-owner checkpoint](2026-09-02-shutdown-and-retained-owner-checkpoint.md)

## Asynchronous identity reset and retired-node status — 0.22.2

- [x] Captured the deployed failure: resetting obsolete ES-1 identity `85ff…`
  applied its tombstone immediately but held the HTTP response for 34.7 seconds
  while synchronously publishing a 5.77 MB metadata snapshot.
- [x] Moved peer propagation and metadata auditing to a coalescing background
  worker. Reset now returns `202 Accepted` after only its small locally durable
  operational tombstone, with `audit_state: "queued"`.
- [x] Excluded retired identities from ordinary status rows, offline health,
  replica counts and capacity totals. Per-node detail preserves the durable
  record with `state: "retired"` and reset audit data.
- [x] Added a regression which holds the metadata mutation owner while reset
  admission completes in under 500 ms, plus reset propagation/unavailable-state
  and retired-status coverage. All 252 core tests and all 3 runtime tests pass.
- [x] Deployed to all four nodes. Live reset returned `202` in 2.568 ms and its
  queued metadata audit completed 312 ms later at generation 2234. Both GBNI
  observers omitted obsolete identity `85ff…` from normal cluster status. After
  recovery from a separate ES-1 disk `EIO`, all four current identities reported
  online, healthy and writable at generation 2234.

Evidence: [identity association retirement](2026-09-01-identity-association-retirement.md)

## Exact metadata reconciliation recovery — 0.22.2

- [x] Reproduced and corrected compact-delta selection for reconciliation which
  reorders an append-ordered garbage/tombstone set. Unrepresentable ordering now
  selects a full immutable record instead of emitting a byte-inexact delta.
- [x] Added one bounded local full-record fallback after compact-body rejection,
  matching remote-replica safety without retrying or loading full history.
- [x] Added regressions for both ordering rejection and local fallback; all
  250/250 core and 3/3 runtime tests passed.
- [x] Deployed complete source as 0.22.2 to all four nodes. The three identical
  Linux builds have SHA-256
  `1fc6c8473a64525da48d6fead630780615f525f22ae136dd2ff9bdf5731144d0`.
- [x] Live generation-2128 branches reconciled without reset or library loss.
  Metadata became writable/validated, ordinary queued publication advanced the
  cluster through generation 2155, and every catalogue API returned the same
  331 items and 374 artwork objects. Linux services remained active with zero
  restarts.

Evidence: [metadata reconciliation recovery](2026-09-01-metadata-reconciliation-recovery.md)

## Metadata-history bounded-memory and delta-reconciliation checkpoint

- [x] Replaced retained decrypted history payloads with a compact authenticated
  on-disk frame index and on-demand reconstruction.
- [x] Added a configurable `128M` default materialisation byte budget, retained
  the secondary 64-entry guard, and exposed history/cache byte diagnostics.
- [x] Made deterministic merges store DLT6 deltas when smaller, including
  merge-parent/conflict replacement, while preserving the immutable full record
  hash and safely supplying missing parents before full fallback.
- [x] Corrected `runtime.rss_bytes` to report current RSS on Linux/macOS and
  moved stale-Macha-mount recovery before mount-path filesystem access.
- [x] Added disk-backed large-history and four-node merge-body regressions.
  The authoritative serial suite passed 246/246 and runtime dependencies passed
  3/3. Live bounded-RSS/restart UAT remains active before loaded ingest resumes.

Evidence: [P0 metadata-history remediation](2026-09-01-metadata-history-memory-remediation.md)

## Bounded shutdown and clean cluster rejoin

- [x] Reproduced the deployed shutdown timeout and captured both maintenance
  owners that could remain blocked after stop: a synchronous outbound control
  RPC and pack compaction waiting for startup storage accounting.
- [x] Made shutdown close pending outbound routes before joining service
  maintenance, permanently reject new outbound calls once cancellation begins,
  and make the accounting wait stop-token-aware.
- [x] Correctly classified libfuse's positive signal return as an intentional
  shutdown rather than a frontend failure, while retaining negative errno and
  watchdog mount loss as fail-closed errors.
- [x] Added regressions for pending RPC cancellation, post-cancellation retry
  rejection, accounting-wait cancellation, and FUSE exit-result semantics.
- [x] Passed the final 245/245 core and 3/3 runtime suites. Live UAT on
  node 50 stopped Macha in 215 ms with systemd `Result=success`, then restarted
  it as the same node at generation 1643; node 51 independently reported it
  online at the same writable generation.

Evidence: [bounded shutdown and rejoin record](2026-09-01-bounded-shutdown-and-rejoin.md)

## Dedicated immutable media-information engine

- [x] Added an event-driven, durably hinted profiling service used by ingest,
  cataloguing, the profile API, and playback; it performs no idle polling.
- [x] Ordered and deduplicated optional scans while keeping every background
  media read in the speculative class below loader work.
- [x] Removed the client-visible playback profiling gate. A stored immutable
  profile feeds the unchanged negotiation path with zero probe reads; a genuine
  miss continues normal viewer-priority media-engine negotiation.
- [x] Coalesced concurrent scans by immutable media ID. Viewer-required
  negotiation cancels/takes over speculative ownership, and successful results
  are asynchronously persisted regardless of which path produced them.
- [x] Pruned profiles only when their immutable media ID has no live namespace
  copy, retaining one shared profile across identical aliases/copies.
- [x] Added separated profile-lookup, fallback-inspection, selection and total
  admission timing diagnostics and corrected the streaming/catalogue docs.
- [x] Passed all 242 core tests and all 3 runtime/libav tests, then built and
  deployed the source on all four nodes. Each node reported ready, writable and
  online at metadata generation 1633.

Evidence: [media-information checkpoint](2026-09-01-media-information-engine.md)

## Manual causal metadata repair and ancestry safety

- [x] Proved all three replicas held accepted generations 1552 and 1516 but no
  retained common ancestor because local history compaction had discarded the
  required bridge.
- [x] Proved generation 1552 strictly causally dominated every mutation in 1516,
  retained the same catalogue root, and contained all 622 older namespace
  entries plus 12 additions.
- [x] Added a dry-run/two-phase offline repair tool which refuses automatic,
  mergeable, concurrent, equal-clock, unstaged or under-witnessed repairs.
- [x] Backed up all three metadata replicas, staged one identical generation
  1553 two-parent record everywhere, then accepted it with nodes 50 and 51 as
  actual durable witnesses. No branch or object was discarded.
- [x] Disabled unsafe automatic history compaction until exact cluster-wide
  accepted-head identity can be proven durably.
- [x] Passed 29/29 storage/metadata tests, 234/234 core tests and 3/3 runtime
  tests. Live UAT converged all nodes, replayed queued operations, and remained
  healthy/writable overnight at generation 1569; later ordinary divergent
  branches reconciled automatically with zero conflicts.

Evidence: [manual causal repair record](2026-09-01-metadata-manual-causal-repair.md)

## Phase 1D.4 partial: transient publication cursor preservation

- [x] Retained the provisional writer, WAL cursor and resumable
  materialisation/rebuild state across retryable backend failures.
- [x] Kept failed pipelined extent payloads at their exact manifest offset and
  retried them in place, preventing both whole-generation replay and manifest
  holes.
- [x] Added a deterministic one-shot failure regression proving one publication
  start, exact one-pass spool reads, atomic visibility and exact final content.
- [x] Preserved the existing pipeline-failure invisibility contract.
- [x] Passed focused tests, filesystem/FUSE 58/58, the complete suite 229/229
  and runtime dependencies 3/3.

Evidence: [transient-failure cursor checkpoint](2026-08-31-fuse-publication-phase-1d-transient-failure-cursor.md)

## Phase 1D.4 partial: pressure-aware retirement selection

- [x] Ranked closed generations above the pressure threshold by releasable
  spool bytes per remaining replay work, with nearest completion as tie-break.
- [x] Preserved ordinary below-pressure FIFO, open-loader fallback, bounded
  quanta, viewer priority and event-driven scheduling.
- [x] Added `data_retirement_priority_selections`, counted only when a worker
  actually reorders closed work.
- [x] Proved a later small closed generation retires ahead of an earlier large
  generation and wakes a blocked writer in the pathological/full-spool test.
- [x] Passed the focused regression, filesystem/FUSE 57/57, runtime 3/3 and the
  final controlled complete suite 228/228.

Evidence: [retirement-selection checkpoint](2026-08-31-fuse-publication-phase-1d-retirement-selection.md)

## Catalogue coalesced-final-state regression correction

- [x] Retained failure-only diagnostics and proved the timed-out cache already
  held the exact final title and artwork at reconciled generation 18 while the
  test incorrectly required equality with writer-side generation 15.
- [x] Accepted newer generations only with exact final content and captured the
  integration run bound at catalogue-repair entry, before zero-grace artwork GC
  can add unrelated metadata events.
- [x] Retained the 10-second timeout, exact final search/artwork/GC assertions,
  and the separate exact one-follow-up ConvergenceDemand state-machine test.
- [x] Passed 10/10 isolated repetitions and the controlled complete suite
  228/228.

Evidence: [retirement-selection checkpoint verification](2026-08-31-fuse-publication-phase-1d-retirement-selection.md#verification)

## Phase 1D.4 partial: publication notification coalescing

- [x] Made one false-to-true spool-drain transition own the all-inode pressure
  sweep instead of rescanning for every blocked FUSE write.
- [x] Kept new durable work event-driven through durability-batch notification;
  no polling or idle owner was introduced.
- [x] Coalesced publication demand at monotonic data/namespace watermarks and
  suppressed unchanged flush/release/pressure notifications before queue work.
- [x] Added a single per-inode enqueue owner across the inode-to-queue lock
  handoff, preventing concurrent duplicate queue ownership.
- [x] Added Status counters for suppressed notifications and pressure sweeps.
- [x] Proved 1,002 notifications become two effective requests and 1,000
  suppressed duplicates; all three pressure regressions prove one sweep per
  episode.
- [x] Passed build, filesystem/FUSE 56/56, controlled complete suite 227/227 and
  runtime 3/3.

Evidence: [publication notification checkpoint](2026-08-31-fuse-publication-phase-1d-notification-coalescing.md)

## Phase 1D.2 partial: sparse changed-range reconstruction

- [x] Replaced canonical whole-file materialisation with a sparse overlay of
  sorted, merged changed byte ranges.
- [x] Reused untouched immutable extents without fetching or hashing them and
  reconstructed only touched/boundary extents under resumable loader quanta.
- [x] Preserved append-then-overwrite data, truncate/re-extension zero fill,
  exact final bytes and old-generation visibility until atomic commit.
- [x] Added completed-publication cohort counters for spool/source reads,
  immutable extent reuse and extent puts.
- [x] Added an eight-extent deterministic regression proving two touched extent
  reads, six untouched reuses, two puts and no whole-file materialisation.
- [x] Passed build, filesystem/FUSE 55/55, controlled complete suite 226/226 and
  runtime 3/3. The default 12-worker complete run is accurately retained as
  225/226 due to the resource-sensitive catalogue burst test; it passed alone
  and in the complete four-worker run.

Evidence: [sparse changed-range checkpoint](2026-08-31-fuse-publication-phase-1d-sparse-changed-ranges.md)

## Phase 1D.3 partial: node-wide DATA resource headroom

- [x] Added event-driven byte-bounded admission below RPC scheduling for
  blocking DATA object, local-store and cache work.
- [x] Reserved configurable non-borrowable foreground/read-ahead capacity while
  retaining work-conserving lower-class use of all non-reserved capacity.
- [x] Kept CONTROL completely outside DATA admission and ordered waiting loader
  work ahead of speculative work.
- [x] Covered incoming/outgoing transfer, direct repair placement, ordinary
  local reads and asynchronous fetched-object persistence; impossible
  oversized class work fails instead of waiting forever.
- [x] Added Status counters, configuration validation/documentation, isolated
  saturation/deadline/fairness tests and a real RPC/local-store saturation test.
- [x] Passed the clean complete default suite 225/225 and runtime dependencies
  3/3. The first eight-slot run was accurately retained as 223/225; both
  load-sensitive failures passed isolated and in the clean four-slot rerun.

Evidence: [DATA resource headroom checkpoint](2026-08-31-phase-1d-data-resource-headroom.md)

Deployment/UAT evidence: [three-node loaded UAT](2026-08-31-phase-1d-data-resource-headroom-uat.md)

## Aggregate spool retirement-rate estimator

- [x] Replaced the per-generation throughput EMA with cumulative physically
  retired spool bytes over one shared wall-clock epoch, so concurrent publisher
  completions contribute their aggregate capacity.
- [x] Kept provisional progress and failed reservation rollback out of the
  capacity signal and reset the internal epoch below the 50% burst threshold.
- [x] Preserved event-driven admission, hard capacity/free-space bounds,
  occupancy hysteresis and viewer priority without adding a loop or idle worker.
- [x] Exposed the rate-window numerator and elapsed milliseconds for operational
  audit and added deterministic aggregate-rate plus integrated retirement tests.
- [x] Passed filesystem/FUSE 50/50, foundations 15/15, runtime 3/3 and complete
  default 218/218 suites, including both catalogue regressions.

Evidence: [aggregate retirement-rate checkpoint](2026-08-31-spool-aggregate-retirement-rate.md)

## FUSE publication Phase 4A: bounded within-file extent pipeline

- [x] Added a configurable byte-bounded provisional extent pipeline for durable
  spool publication while leaving ordinary writes synchronous.
- [x] Preserved whole-file atomic visibility and the final aggregate durability
  barrier by merging private extent durability evidence in order.
- [x] Drained admitted work before every fairness/viewer yield so the pipeline
  is the hard bound on already-running loader I/O.
- [x] Added deterministic bound, atomicity and failure-invisibility tests plus
  operational pipeline diagnostics.
- [x] Passed build, new tests 2/2, filesystem/FUSE 50/50 and runtime 3/3. The
  complete run was accurately recorded as 216/217 because the separately
  tracked concurrent catalogue final-state test failed; it passed immediately
  in isolation.
- [x] Passed three-node UAT: 19.5 MiB/s over the complete observation window,
  40.2 MiB/s in a clean loaded interval, pipeline peak exactly two, viewer read
  gating exact, a 4 MiB direct read in 0.37 seconds, protected communications,
  and zero failures/timeouts.

Evidence: [Phase 4A implementation](2026-08-31-fuse-publication-phase-4a-extent-pipeline.md)
and [Phase 4A UAT](2026-08-31-fuse-publication-phase-4a-uat.md)

## FUSE publication Phase 0/1: diagnostics, loader priority and fair quanta

- [x] Added useful-byte, coalescing, publication, quantum/yield, admitted-byte,
  spool-pressure and RPC class/timing diagnostics.
- [x] Separated user-requested loader traffic from crash-recovery provenance and
  placed it below viewer/read-ahead work but above speculative maintenance.
- [x] Removed the false single-publisher cap for open loaders and stopped using
  the legacy recovery-worker value as a cap on restarted user ingest.
- [x] Added extent-aligned resumable publication quanta, a global admitted-byte
  budget, one retained cursor per inode, fair tail requeueing and atomic final
  metadata visibility.
- [x] Added bounded mid-generation yield/resumption machinery and its original
  FUSE-read signalling test. The later aggregate-rate UAT established that the
  FUSE classification and exclusive gate policy were wrong; Phase 1C now
  supersedes that policy while retaining the bounded-yield mechanism.
- [x] Added deterministic coverage for loader RPC ordering, recovery provenance,
  concurrent/open/closed loaders, coalescing, byte bounds, fair small-file
  progress, exact useful bytes, mid-generation viewer pre-emption, resumption,
  crash replay and confirmation.
- [x] Passed the final local build, `filesystem_fuse` 48/48, runtime 3/3 and
  complete 215/215 suites.
- [x] Passed the Phase 0/1 bounded-quantum three-node UAT: two live loader
  generations used fair bounded quanta with no amplification; loader/control
  queue waits remained sub-millisecond; a FUSE read demonstrated the old gate
  and clean resumption; and failures/timeouts remained zero. That FUSE leg is
  not accepted as viewer UAT evidence after the Phase 1C classification decision.

Evidence: [Phase 0/1 checkpoint](2026-08-31-fuse-publication-phase-0-1-checkpoint.md),
[Phase 1A loader priority](2026-08-31-fuse-publication-phase-1a-loader-priority.md),
[Phase 1B fair quanta](2026-08-31-fuse-publication-phase-1b-fair-quanta.md), and
[Phase 1A/1B UAT](2026-08-31-fuse-publication-phase-1ab-uat.md)

## Phase 0: diagnostic foundation

- [x] Added namespace admission, recovery, attempt, publication, confirmation, journal-group, record, and durability-barrier diagnostics.
- [x] Added metadata historical-request, reconstruction, and applied-delta diagnostics.
- [x] Added namespace batch/operation and convergence event/run diagnostics without introducing polling or high-cardinality logging.
- [x] Added characterization tests for one-publication-per-recovered-operation baseline behaviour and repeated linear-chain reconstruction.
- [x] Verified the Phase 0 implementation with its focused suites and recorded the continuation boundary.

Evidence: [Phase 0 diagnostic foundation](2026-08-30-phase-0-diagnostic-foundation.md)

## Phase 1: bounded shared metadata materialization

- [x] Added a bounded cache for immutable validated records and decoded snapshots, keyed by commit hash and seeded from materialized heads.
- [x] Reconstructed uncached chains from their nearest cached ancestor and retained validated intermediates.
- [x] Reused shared immutable materializations across policy validation, pruning/ancestry, committed and snapshot views, repair, catalogue, and retention consumers.
- [x] Bounded the initial cache to 64 entries while protecting current, committed, and accepted heads.
- [x] Added coverage for a 200-entry linear chain, zero replay on repeat lookup, eviction/reconstruction identity, branches, reconciliation, policy changes, and accepted-head pruning.
- [x] Ran the complete default/runtime suites for this slice, including the catalogue search/artwork/GC regression.
- [x] Performed a three-node UAT: the cluster converged, remained responsive, and returned to idle without sustained metadata work.

Evidence: [bounded record materialization](2026-08-30-phase-1-bounded-record-materialization.md), [shared decoded materialization](2026-08-30-phase-1-shared-decoded-materialization.md), and [Phase 1 three-node UAT](2026-08-30-phase-1-three-node-uat.md)

## Phase 1: materialization follow-up closeout

- [x] Audited every remaining intermediate encode and confirmed it is required
  to validate the current full-snapshot successor hash, including exact legacy
  delta-version encodings; removing it requires the deferred identity redesign.
- [x] Added a corrupt-delta regression proving a cached parent cannot validate
  a false successor, rejection cannot poison the cache, and a corrupt duplicate
  cannot displace a valid cached target.
- [x] Completed the merge, same-generation sibling, recovery authority, policy
  transition, corruption, and eviction audit against explicit deterministic
  tests.
- [x] Passed the new regression, `storage_metadata` 28/28, and the live
  same-generation sibling reconciliation test.
- [x] Passed the expanded complete default suite 205/205 and runtime
  dependencies 3/3, including both catalogue regressions.

Evidence: [Phase 1 materialization closeout](2026-08-30-phase-1-materialization-closeout.md)

## Phase 2: bounded namespace publication and durability

- [x] Replaced one-operation publication with bounded ordered, event-driven batch draining governed by configurable operation-count and encoded-byte limits.
- [x] Added ordered batch application to one mutable snapshot and metadata delta, including largest-valid-prefix failure handling and idempotent/no-op recovery.
- [x] Grouped `published` and `done` journal markers into one durability barrier per group while preserving the existing crash-compatible journal format.
- [x] Batched confirmation-safe operation families, retained unsafe mixed operations and rename as boundaries, and preserved sequence order and journal epochs.
- [x] Avoided per-operation full-snapshot work, replaced `rmdir` full-map scans with descendant lookup, and retained garbage-grace semantics.
- [x] Proved 1,000 recovered unlinks publish in four batches at a 256-operation limit rather than 1,000 generations.
- [x] Added deterministic coverage for count/byte boundaries, oversized progress, unlink-plus-parent-rmdir, idempotence, partial idempotence, semantic failure prefixes, crash prefixes, and concurrent live admission during recovery publication.
- [x] Verified partial grouped `published` and `done` crash images recover without loss, duplication, or unnecessary publication.
- [x] Ran the complete suite after durability closeout: default 194/194 and runtime-dependency 3/3, including the explicit catalogue regression.
- [x] Performed a three-node deletion UAT: 1,001 deletion operations used five publications, completed in 0.77 seconds, kept status responsive, and returned all nodes to idle.

Evidence: [Phase 2 namespace batching](2026-08-30-phase-2-namespace-batching.md), [durability closeout](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md), and [Phase 2 three-node deletion UAT](2026-08-30-phase-2-three-node-deletion-uat.md)

## Phase 3: event-driven convergence foundation

- [x] Added `ConvergenceDemand`, using a semantic epoch, latest-generation diagnostic high-water mark, and edge-triggered scheduled state.
- [x] Coalesced event bursts so events arriving during a run schedule at most one follow-up pass over the newest state.
- [x] Kept metadata/topology signals distinct from storage-only maintenance and preserved same-generation sibling/topology events.
- [x] Deferred catalogue convergence when newer metadata arrives during repair so it does not consume an obsolete intermediate view.
- [x] Added scheduler-level burst coverage proving hundreds of notices produce two runs and process the final high-water state.
- [x] Added an integrated service-level gated burst around real metadata repair. A peer publishes exactly 33 generations while the follower's repair owner is held after claiming its run; the held burst schedules no extra runs, release produces exactly one latest-state follow-up, and the follower installs the final generation and namespace.
- [x] Corrected real same-generation sibling signaling. Explicit metadata notices at the current numeric generation now advance the semantic epoch and wake convergence because they represent an accepted-head topology change; stale lower-generation notices and unchanged membership heartbeats remain non-events.
- [x] Added a two-service gated sibling test that creates distinct accepted children of one parent at the same generation, proves the remote notice advances demand without a generation increase, and verifies automatic reconciliation preserves both namespace changes in one accepted descendant.
- [x] Added an integrated catalogue burst test. While follower metadata repair is gated, a peer publishes intermediate title and poster replacements; catalogue repair performs no intermediate run, then runs exactly once against the final metadata state, exposes only the final search result and artwork bytes, and reclaims every superseded artwork object.
- [x] Corrected event-driven garbage-grace scheduling. After evaluating an immature final tombstone set, maintenance now arms one steady-clock wake at the earliest recorded wall-clock retirement expiry instead of becoming indefinitely quiescent or introducing a polling cadence.
- [x] Added a gated coalesced-delete test proving bytes survive immediately before their 750 ms grace deadline, are reclaimed after it without an unrelated event, tombstones are removed, cleanup convergence remains bounded, and the scheduler returns to a parked state.
- [x] Removed the redundant unconditional metadata announcement at the end of local publication. `NodeRuntime::accept_metadata_commit` is now the single owner of accepted-head change detection and notification.
- [x] Added a monotonic announcement diagnostic and proved 33 real publications emit exactly 33 announcements, while re-installing identical acceptance evidence emits neither a local event nor a remote rebroadcast.
- [x] Added a persisted three-replica catch-up test. A third voter stops at a shared base, two surviving voters publish 64 linear generations at W=2, and the restarted voter imports the required history, accepts the sole newest head, exposes the final namespace, and settles in at most four service convergence runs rather than one per ancestor.
- [x] Verified disconnected maintenance sleeps until a peer event and retained the catalogue search/artwork/GC regression.
- [x] Performed a three-node idle UAT at generation 1375. All three nodes remained healthy/writable with stable RPC connections, all convergence-related workers were parked, and post-torrent-pause instantaneous process CPU was 0.0% on every node.
- [x] Isolated the local pre-pause CPU use to libtorrent peer/UTP work rather than metadata, catalogue, hydration, FUSE, HTTP, or RPC work.
- [x] Performed a three-node active-burst UAT using 81 paced creates followed by 81 paced deletes. Each half used 20 metadata publications, all three nodes agreed on both the 80-entry intermediate namespace and the final empty state, status remained responsive during active work, RPC connections did not churn, and every node returned immediately to the previously proven idle state.

Evidence: [Phase 3 scheduler checkpoint](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md), [Phase 3 three-node idle UAT](2026-08-30-phase-3-three-node-idle-uat.md), and [Phase 3 three-node active-burst UAT](2026-08-30-phase-3-three-node-active-burst-uat.md)

Integrated burst verification: `rpc_cluster/test_service_metadata_repair_coalesces_real_generation_burst` passed six isolated executions; the complete `rpc_cluster` suite passed 28/28, `invariants` passed 36/36, `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` passed, and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed.

Same-generation sibling verification: `rpc_cluster/test_service_same_generation_sibling_notice_triggers_reconciliation` first reproduced the missing event, then passed eight consecutive isolated executions after the fix. The burst test passed another eight consecutive executions; the complete `rpc_cluster` suite passed 29/29, `invariants` passed 36/36, and the explicit inactivity and catalogue regressions passed.

Catalogue burst verification: `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` passed six isolated executions; the complete `hydration_catalogue` suite passed 23/23, and the real repair burst, same-generation sibling, and scheduler primitive regressions all passed explicitly.

Retention/grace verification: the new test first reproduced objects remaining indefinitely after grace, then `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace` passed five consecutive isolated executions after the fix. The complete `filesystem_fuse` suite passed 43/43, and both catalogue tests plus all three convergence regressions passed explicitly.

Acceptance rebroadcast verification: the strengthened real-repair burst first reproduced duplicate publication announcements, then passed eight consecutive isolated executions after the fix. The repeated-evidence sibling test passed five consecutive executions. The complete `rpc_cluster` suite passed 29/29 and `invariants` passed 36/36; the catalogue burst and exact garbage-grace tests also passed explicitly.

Lagging-replica verification: `rpc_cluster/test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` passed six isolated executions. The expanded complete `rpc_cluster` suite passed 30/30.

Phase 3 repository-wide checkpoint: `sh run-tests.sh build` passed the complete default suite 199/199 and runtime-dependency suite 3/3. The run explicitly included `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`, the real burst and sibling tests, the exact garbage-grace test, and the lagging-third catch-up test.

## Phase 4: bounded metadata RPC executor

- [x] Routed history import, commit storage, and acceptance RPCs to a dedicated executor rather than ordinary control or data workers.
- [x] Kept socket reading, frame assembly, queue admission, ping, membership, ordinary control handlers, and foreground DATA work independent of metadata execution.
- [x] Preserved asynchronous RPC completion: executor owners finish work and queue replies without occupying the session reader.
- [x] Bounded pending metadata work by both job count and payload bytes, returning an immediate in-band error on overload without closing the peer session.
- [x] Preserved per-peer FIFO across history/store/accept operations while permitting explicitly configured workers to execute different peers concurrently.
- [x] Defined and tested lifecycle boundaries: queued work is cancellable; running work owns its durability boundary and survives reply-route disconnect; shutdown completes a running owner and abandons queued work whose session has closed.
- [x] Kept metadata recovery from consuming fast-control, ordinary-control, or foreground DATA capacity.
- [x] Added deterministic gated tests for executor isolation, count/byte backpressure, session survival, per-peer order, cross-peer concurrency, cancellation, disconnect, and shutdown.
- [x] Set the production default to one metadata owner, 64 queued jobs, and 256 MiB of queued payload. This avoids multiplying idle workers while retaining bounded multi-worker support and per-peer FIFO for explicit configurations.
- [x] Replaced stop-and-wait history transfer with a dependency-first window of at most eight asynchronous uploads; sender diagnostics and the lagging-third test prove more than one and no more than eight requests are in flight.
- [x] Moved history-entry reconstruction, delta application, snapshot encoding/hash validation, encryption, and durable history append out of the global replica-state critical section. A separate durability owner preserves append order and a short state lock installs the validated immutable entry afterward.
- [x] Added single-flight off-lock materialization. Eight concurrent readers of a cold 200-delta chain now share exactly one reconstruction and install through a validated short lock step.
- [x] Prewarmed acceptance-policy materializations outside the replica lock and moved accepted-head public reconstruction onto the off-lock path.
- [x] Documented the closed fast-control allow-list (`ping` and `members` on CONTROL frames only), its handler constraints, and the proof required before extending it.
- [x] Fixed the event-driven follow-up edge exposed by the complete suite: a coalesced convergence run which leaves one follow-up pending now continues immediately without requiring an unrelated external wake, while disconnected settled maintenance remains parked.
- [x] Performed the Phase 4 three-node recovery UAT: a paused third replica missed a 65-operation workload, the surviving pair remained writable and responsive, the resumed replica converged to the identical 64-entry namespace, and all nodes returned to sleeping idle at the same generation without RPC churn.

Evidence: [Phase 4 bounded metadata RPC executor](2026-08-30-phase-4-bounded-metadata-rpc-executor.md) and [Phase 4 three-node recovery UAT](2026-08-30-phase-4-three-node-recovery-uat.md)

Verification: the 27-test `storage_metadata` suite passed; the strengthened cold-chain test proved one reconstruction for eight concurrent callers; and the lagging-third test proved a bounded transfer peak in `(1, 8]`. The first repository-wide run correctly exposed the lost follow-up wake in `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`; after correcting that scheduler edge, the catalogue burst and disconnected-idle regressions passed explicitly. The final repository-wide run passed the default suite 203/203 plus runtime dependencies 3/3, including both catalogue burst/search/artwork/GC regressions and the lagging-third recovery test.

## Phase 5: bounded operational diagnostics

- [x] Added accepted-head persistence write, byte, and failure counters with
  success recorded only after durable replacement completes.
- [x] Added fixed-size atomic RPC execution timing summaries by message and
  frame class, including count and total/max queue and handler microseconds.
- [x] Exposed local aggregates through Status without a sampler, polling loop,
  per-request history, metadata mutation, or gossip expansion.
- [x] Added deterministic metadata, RPC isolation/backpressure, and Status API
  assertions for the new diagnostics.
- [x] Documented bounded FUSE recovery batching, shared metadata
  materialization, metadata RPC isolation, and operational interpretation of
  the new counters.
- [x] Observed and retained one initial catalogue-burst suite timeout; the
  immediate isolated run passed, and a second complete run passed 203/203.
  Runtime dependencies passed 3/3.
- [x] After formatting and rebuilding, explicitly passed the 1,000-operation
  FUSE recovery test, lagging-third multi-node RPC recovery test, and catalogue
  sync/search/artwork/GC regression.
- [x] Verified the final handler-only timing implementation with the complete
  `rpc_cluster` group, 34/34.

Evidence: [Phase 5 operational diagnostics](2026-08-30-phase-5-operational-diagnostics.md)

## Phase 5: three-node operational diagnostics UAT

- [x] Paused one deployed replica, admitted 257 directory operations through
  MachaDFS, resumed the same process, verified identical 256-child views, and
  removed the exact fixture with all three nodes active.
- [x] Measured 12 publications for 514 create/delete operations, zero rejected
  metadata jobs, zero accepted-head persistence failures, and bounded RPC queue
  separation from CONTROL work.
- [x] Proved the lagging replica imported required history and persisted one
  final create head rather than every intermediate accepted head, with no new
  reconstruction or delta replay on any node.
- [x] Verified stable generation/counters/RSS, two canonical connections with
  zero churn, direct Status responsiveness, and fully sleeping processes after
  drain.
- [x] Preserved physical DATA GC and repeated-burst RSS as explicit remaining
  work rather than over-claiming them from a directory-only fixture.

Evidence: [Phase 5 operational diagnostics UAT](2026-08-30-phase-5-operational-diagnostics-uat.md)

## Phase 5: journal and convergence Status diagnostics

- [x] Exposed existing namespace publication and operation-journal append/barrier
  totals through an O(1), lock-free FUSE diagnostic snapshot.
- [x] Exposed convergence event/run/epoch totals and scheduled state from the
  existing edge-triggered demand object.
- [x] Kept startup and non-mounted nodes explicit through `available`, and used
  weak frontend ownership so Status cannot extend mount lifetime.
- [x] Proved exact live-path journal semantics: inode descriptor admission,
  operation admission, publication, and completion are four durable append
  groups; recovered operations retain the existing two-group expectation.
- [x] Passed the focused production-shaped API test, `filesystem_fuse` 44/44,
  `invariants` 36/36, the complete default suite 204/204, and runtime 3/3.

Evidence: [Phase 5 journal and convergence Status diagnostics](2026-08-30-phase-5-journal-convergence-status.md)

## Phase 5: journal and convergence Status UAT

- [x] Exercised 65 creates followed by 65 removals on the deployed three-node
  cluster; each half completed in three publications and under 0.3 seconds.
- [x] Accounted exactly for 520 journal records and 272 durability barriers:
  two admission barriers per operation plus grouped publication and completion
  barriers per metadata batch.
- [x] Measured 130 confirmed namespace operations in six publications, or 21.7
  operations per publication, with no retries, failures, timeouts, rejected
  metadata work, or residual queue.
- [x] Observed 23 convergence events on every node, with 8 local and 12 remote
  completed runs; all epochs drained, scheduling parked, and all nodes agreed
  at generation 1439.
- [x] Verified the fixture was absent, direct Status latency was 1-12 ms, and
  delayed process CPU returned to approximately 0.21%, 0.12%, and 0.07%.

Evidence: [Phase 5 journal and convergence Status UAT](2026-08-30-phase-5-journal-convergence-uat.md)

## Phase 5: physical-object GC separation proof

- [x] Confirmed namespace deletion completion does not wait for physical DATA
  removal; metadata publication and grouped journal confirmation remain the
  foreground durability boundary.
- [x] Confirmed physical GC uses a resumable cursor capped at 64 examined
  objects per Service slice and yields immediately to foreground playback or
  mounted-filesystem activity.
- [x] Confirmed destructive reclamation remains fenced by cluster reachability,
  stable/current metadata, catalogue liveness, retention claims, and grace.
- [x] Strengthened deterministic coverage to require a multi-slice GC pass, and
  retained the exact-deadline test proving bytes survive namespace deletion
  until grace before event-driven reclamation.
- [x] Documented the operational distinction and bounded cursor semantics in
  the durability and operations guides.

Evidence: [Phase 5 physical-object GC separation proof](2026-08-30-phase-5-physical-object-gc-proof.md)

## Plan-ledger reconciliation

- [x] Removed stale active entries for off-lock materialization, validated
  short-lock installation, concurrent single-flight deduplication, and its
  deterministic eight-reader proof; these were completed and verified in
  Phase 4.
- [x] Confirmed the final batching/recovery, convergence, RPC-isolation, and
  physical-GC semantics are recorded across `docs/durability.md`,
  `docs/metadata.md`, and `docs/operations.md`.

## Status disk-usage availability correction

- [x] Reproduced the alternating plausible/zero presentation from raw Status
  responses on all three live nodes and located the ambiguity in the API rather
  than the client.
- [x] Made unavailable per-node storage usage/free, cache measurements, and
  online-backend count structurally null instead of fabricated-looking zeros.
- [x] Made incomplete cluster storage/cache aggregates unavailable rather than
  summing missing observations as empty disks.
- [x] Preserved genuine measured zero usage as numeric with `available: true`.
- [x] Added deterministic regression coverage and passed the focused test,
  `invariants` 35/35, and the FUSE/convergence Status integration test.
- [x] Passed the complete default suite 204/204 and runtime dependencies 3/3,
  including the explicit catalogue search/artwork/GC regression.

Evidence: [Status disk-usage availability correction](2026-08-30-status-disk-usage-availability.md)

## Combined Status and repeated-burst UAT

- [x] Verified the unavailable-versus-zero disk contract before and after work
  on every deployed Status endpoint: live self measurements remained numeric,
  while unavailable remote usage, free space, cache measurements, and online
  backend count remained null.
- [x] Completed 2,064 create/remove operations in 48 publications, advancing
  all nodes from generation 1439 to 1487 with exact journal accounting, no
  failure or timeout, and no residual queue.
- [x] Filled every materialization cache to its 64-entry bound. All 3,960 new
  requests were hits; reconstructions, misses, and applied-delta totals did not
  increase, and bounded evictions occurred on every node.
- [x] Verified all convergence epochs drained, scheduling parked, exact UAT
  fixtures were absent, and delayed CPU returned to approximately 0.260%,
  0.142%, and 0.066%.
- [x] Identified that `runtime.rss_bytes` is lifetime-peak `ru_maxrss`, not
  current RSS. The memory-ceiling claim was therefore kept active pending a
  correctly defined metric and repeated post-cap observations.

Evidence: [Combined Status availability and repeated-burst UAT](2026-08-30-combined-status-rss-uat.md)

## Cluster Status telemetry aggregation correction

- [x] Found that authenticated telemetry notifications were transmitted with
  request ID zero but silently ignored by both RPC notification receive paths.
- [x] Routed telemetry notifications from both canonical connection directions
  into the existing bounded speculative executor, keeping decode/store work off
  critical socket-reader threads.
- [x] Added an edge-triggered telemetry wake on authenticated peer observation,
  so connection formation disseminates a coalesced current sample without
  waiting for the periodic local sampler.
- [x] Proved that one two-node Status request contains the connected peer's live
  numeric storage/cache/backend telemetry and a complete online aggregate; no
  client fan-out or HTTP-time network call is required.
- [x] Passed both focused regressions, `rpc_cluster` 35/35, and `invariants`
  36/36. The repository-wide run was accurately retained as 206/207 because
  the tracked catalogue-burst test timed out under suite load; it passed an
  immediate isolated rerun in 0.890 seconds, and runtime passed 3/3. A second
  complete run at four-way concurrency again finished 206/207 with the same
  test timing out at 10.415 seconds.

Evidence: [Cluster Status telemetry aggregation correction](2026-08-30-cluster-status-telemetry-aggregation.md)

## Cluster Status telemetry aggregation UAT

- [x] Queried each of the three deployed Status endpoints independently and
  confirmed that every single response contained all three connected nodes.
- [x] Verified every node was live with numeric storage/cache/backend telemetry
  and that all endpoints returned identical available online/known aggregates.
- [x] Verified the cluster was healthy and writable at generation 1487, all
  convergence demand was drained, and all metadata executor queues were empty.
- [x] Confirmed deployed telemetry notifications were executing under the
  speculative RPC class in both canonical route distributions.

Evidence: [Cluster Status telemetry aggregation UAT](2026-08-30-cluster-status-telemetry-aggregation-uat.md)

## Linux systemd install and uninstall targets

- [x] Added a Linux-default systemd install with binary, unit, configuration,
  example, and documentation paths derived consistently from CMake settings.
- [x] Added first-install-only creation of `/etc/macha/macha.yaml`; upgrades
  preserve operator changes and clearly print the operational config path.
- [x] Added a manifest-based `make uninstall` target which removes managed
  artifacts while deliberately preserving configuration, keys, state, cache,
  spool, mounts, and media data.
- [x] Replaced the legacy EnvironmentFile indirection with a direct generated
  `ExecStart` and documented service enable/disable/reload steps.
- [x] Verified a staged `/usr` install, both generated unit paths, complete
  uninstall, configuration preservation, and checksum-stable reinstall.
- [x] Corrected Debian multiarch systemd installation: cached
  `lib/<architecture>/systemd/system` defaults migrate to
  `lib/systemd/system`, while custom paths remain configurable; also removed
  the generated CMake CMP0012 warning.

Evidence: [Linux systemd install and uninstall targets](2026-08-30-linux-systemd-install.md)

## FUSE spool rate backpressure — first checkpoint

- [x] Retained the safe 16 GiB default while documenting and testing
  `fuse.max_spool_bytes` and `fuse.spool_reserve_free` as configurable policy.
- [x] Replaced logical-capacity `ENOSPC` with event-driven blocking admission;
  only an impossible single request or the independent physical reserve can
  fail for capacity.
- [x] Start publication from spool pressure even while a writer remains open,
  and pace admission from measured end-to-end publication throughput: local
  burst below 50%, progressive slowdown, and publish-rate admission by 90%.
- [x] Added O(1) spool occupancy/rate/wait diagnostics to Status.
- [x] Added focused drain-recovery and stalled-publisher regressions. The FUSE
  group passed 45/45, the complete backend suite passed 211/211, and runtime
  dependencies passed 3/3.

Evidence: [FUSE spool rate backpressure](2026-08-30-fuse-spool-rate-backpressure.md)

## Version 0.21.0

- [x] Bumped the project minor version from 0.20.0 to 0.21.0 across CMake,
  generated server metadata, the release heading, and shipped configuration and
  legacy systemd-wrapper examples.
- [x] Reconfigured and rebuilt every target successfully; generated
  `kServerVersion` is `0.21.0`, and runtime dependencies passed 3/3.

## Linux miniupnpc API 18 compatibility

- [x] Removed the compile-time dependency on `UPNP_CONNECTED_IGD` and
  `UPNP_PRIVATEIP_IGD`, which were added after miniupnpc first shipped API 18
  without changing its API-version number.
- [x] Added a version-aware compatibility classifier for the documented
  `UPNP_GetValidIGD()` ABI values, retaining private-WAN support for API 18+
  and rejecting disconnected/unknown devices.
- [x] Rebuilt successfully with UPnP enabled; the focused compatibility test
  passed and the complete `foundations` group passed 13/13.

## Weighted loader cold-setup accounting

- [x] Separated loader admission/activity tracking from the start of useful
  weighted service, so cold distributed-writer setup cannot consume the loader
  slice before producing work or manufacture a ratio-amplified cooldown.
- [x] Added a deterministic scheduler regression proving that a ten-second cold
  setup is excluded from the 25 ms service slice and 475 ms cooldown at the
  default 95:5 weighting.
- [x] Built and passed the scheduler regression on macOS and natively on both
  Linux nodes 50 and 51, together with all three spool-pressure regressions.

Evidence: [Spool progress-bootstrap admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md)

## FUSE write admission shutdown race

- [x] Closed the final byte-admission race in which shutdown could release one
  blocked writer's lease and allow another waiter to acquire newly available
  capacity after `stopping` had already become true.
- [x] Preserve the bounded-memory contract by checking shutdown after the wait
  condition clears and before accounting or copying the request payload.
- [x] The focused shutdown/admission regression passes, runtime dependencies
  pass 4/4, and the complete core suite passes 262/262 at 12-way process
  isolation.

## Playback allocator bound and decoder parallelism — versions 0.22.6–0.23.0

- [x] Removed the temporary hot-path transport ownership and allocator-sampling
  instrumentation before performance validation.
- [x] Bounded glibc arenas at process start with configurable
  `runtime.glibc_arena_max` (default four) and a hard Linux multi-wave test.
- [x] Added bounded `streaming.video_decoder_threads` (default two, range
  1–16), applied it before codec open, exposed the effective configuration in
  playback Status, and made changes restart-required.
- [x] Passed runtime dependencies 4/4 and the complete core suite 262/262 at
  12-way process isolation.
- [x] Passed four-node UAT: three smooth allocator-bound lifecycles followed by
  two smooth two-thread HEVC lifecycles. Every teardown reached zero playback
  ownership, all heap reclaim attempts succeeded, and drained RSS remained on
  a stable approximately 204–223 MiB plateau without lifecycle ratcheting.

Evidence: [Transport, allocator and decoder checkpoint](2026-09-02-transport-and-allocator-attribution-checkpoint.md)

## Spool progress-bootstrap admission

- [x] Removed the zero-rate dead zone above 50% spool occupancy by granting
  bounded one-for-one admission credit only after useful partial publication
  has successfully drained.
- [x] Preserved the configured hard limit, physical reserve, whole-file atomic
  visibility and event-driven wakeup contract; zero-byte or failed work grants
  no capacity.
- [x] Passed the focused regression and all spool-pressure tests on macOS and
  both Linux nodes, then deployed the identical binary to nodes 50 and 51.
- [x] Passed loaded UAT with rsync and real playback: rsync accepted roughly
  506 MB while publication replayed 441 MB in 30 seconds above the soft
  threshold, with bounded waits, no freeze, no ENOSPC, no backend failure and a
  healthy 3/3 cluster.

Evidence: [Spool progress-bootstrap admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md)

## Structural remediation Phase 1 — object-store concurrency and priority

- [x] Removed store-index locking from loose/packed payload I/O, AES-GCM,
  hashing, filesystem capacity inspection, loose removal, crash-accounting
  fsync and bounded pack compaction.
- [x] Preserved exact same-object single-flight and introduced logical pack
  reader leases so compaction cannot unlink a pack between index selection and
  `open(2)`.
- [x] Moved physical validation/retention/deletion off CONTROL workers and
  propagated loader/speculative/viewer classes through the shared DATA
  admission boundary, with non-blocking inner RPC admission to avoid bounded
  worker inversion.
- [x] Coalesced deferred durability ownership into exact non-dominated physical
  generation frontiers.
- [x] Passed deterministic blocked-operation, priority, recovery, corruption,
  durability and compaction regressions; final local verification is 267/267
  core plus 4/4 runtime at 12-way process isolation.

Evidence: [Object-store concurrency checkpoint](2026-09-02-object-store-concurrency-checkpoint.md)

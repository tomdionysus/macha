# Current release

## 0.23.11 — Snap transcode seeks to source keyframes, cutting seek startup cost (development)

- Live-measured a seek deep into a transcoded HEVC Main10 title costing
  ~7s to first fragment versus ~2s from position zero. Root cause: for
  transcode (unlike remux), a seek lands on the nearest source keyframe but
  then fully *decodes* (not just skips) every frame between that keyframe
  and the exact frame-accurate requested position, purely so encoding can
  start at that exact frame -- a viewer does not need that precision, and it
  can cost a full GOP's worth of software HEVC decode on slow hardware.
- Fixed by snapping the transcode seek itself to the nearest keyframe at or
  after the target (`media_vod::nearest_keyframe_at_or_after`), in both the
  cold-session-creation path and the interactive PATCH-seek fast path
  (`reseek_hls_vod`). Deliberately does not reuse remux's `indexed_plan`
  for this: that function also validates keyframe density across the
  *entire remaining file*, which is irrelevant to transcode (it lays down
  its own GOP structure regardless) and was observed to silently reject the
  snap -- with no error, just silently falling back to the old frame-accurate
  behaviour -- whenever any other part of a long file had a sparser GOP.
- Second bug caught only via live measurement showing no improvement despite
  correct-looking code: the keyframe's timestamp was rounded to milliseconds
  with `llround` (nearest), which can round a fractional-millisecond
  keyframe timestamp *down*; reconstructing microseconds from that
  truncated value then made the container seek land one keyframe *earlier*
  than intended -- a full extra GOP decoded for nothing. Fixed by rounding
  up (`ceil`) instead, everywhere a keyframe timestamp (not a raw user
  seek request) is converted to milliseconds.
- Verified live: ~7s reduced to ~4s for a fresh seek on quiet hardware
  (position zero remains ~2-2.5s). A residual ~1.7s gap versus position
  zero remains unexplained and is left for follow-up, rather than guessed
  at -- a stream-info/container-seek timing diagnostic was added
  (`media playback pipeline seek timing`) to help isolate it.
- Also fixes the interactive PATCH-seek case, though live measurement there
  was confounded by CPU contention between the old (still-encoding) and new
  pipeline during handover on constrained hardware -- a separate, likely
  larger effect for real-world scrubbing, not addressed here.

## 0.23.10 — Fix seek/generation-replacement stall on stale segment requests (development)

- Live-diagnosed via a real seek into Apollo 13 on a running node: a segment
  request against a superseded generation (e.g. client read-ahead, or a
  request already in flight when a seek arrives) could block for the full
  duration of the *replacement* pipeline's startup (`streaming.startup_timeout_ms`,
  15s default) before returning its 404. `update_session`'s seek path starts
  the replacement pipeline synchronously before stopping the old one (by
  design, to avoid a resource-accounting race -- see the ordering comment at
  the `stop_pipeline`/`start_pipeline` call site), and only `stop_pipeline(old)`
  wakes anything blocked in `MediaSegmentStore::wait_object()` on the old
  generation's store via `cancel()`. Nothing woke those waiters earlier.
- Fixed by adding `MediaSegmentStore::mark_superseded(bool)`, a lighter,
  reversible signal distinct from `cancel()`: it wakes `wait_object()` callers
  blocked on a not-yet-produced segment (so they get a prompt 404 `not_ready`)
  without stopping production or setting `finished`/`error`. `update_session`
  now marks the old generation's store superseded immediately, before
  attempting to start the replacement pipeline; if that attempt then fails,
  it is cleared again so the still-active old session keeps its normal
  long-poll behaviour. This does not change the existing resource-handover
  ordering/invariant at all -- `stop_pipeline(old)` still only runs once the
  replacement is confirmed.
- Added a regression test exercising `mark_superseded`'s wake and reversal
  behaviour directly against `MediaSegmentStore`.
- Not yet confirmed whether this was the cause of a separately reported live
  symptom (apparent ~1-2s A/V offset partway through a title) -- reproducing
  that report during investigation showed a seek triggering this exact stall
  cascading into the client's failover logic and losing playback position,
  but it was not established whether a real seek/PATCH occurred in that
  specific live report versus continuous playback simply outrunning
  real-time production. This fix addresses the confirmed stall either way;
  whether it resolves the original report remains to be observed live.

## 0.23.9 — Fix audible pitch shift introduced by 0.23.8's drift correction (development)

- 0.23.8's `swr_set_compensation()`-based correction genuinely worked (bounded
  drift as measured), but the correction mechanism itself was wrong: nudging
  the resample ratio changes pitch, and no bound on that rate is small enough
  to be inaudible to a sensitive listener — reported live as an unacceptable,
  clearly audible pitch shift. That mechanism has been removed entirely
  (`maintain_audio_drift_compensation()`, `swr_set_compensation()`, and its
  supporting counters are gone).
- Replacement: enable libswresample's own built-in correction directly
  (`av_opt_set_double(swr, "async", 1, 0)` plus `swr_next_pts()` fed the real
  source PTS each frame) — the same machinery behind ffmpeg's own `-async 1`
  and the `aresample` filter's default behaviour. This corrects by injecting
  silence or dropping samples (`swr_inject_silence()`/`swr_drop_output()`),
  never by changing the resample ratio: `max_soft_comp` (the opt-in
  pitch-bending stretch/squeeze path) is left at its disabled default and is
  never touched by this code, so a pitch shift is structurally not possible
  through this path.
- This replacement had its own near-miss during development: an initial
  attempt tried to avoid overflowing `swr_next_pts()`'s required
  `AVRational{1, in_rate*out_rate}` denominator by dividing it by
  `gcd(in_rate, out_rate)` — but that unit is a unit fraction (numerator 1)
  and a unit fraction cannot be reduced by any GCD (`gcd(1, N)` is always 1).
  The result was silently ~48000x too coarse for the common 48kHz -> 48kHz
  case, which made libswresample believe it was catastrophically far ahead
  and drop nearly all audio (measured: 5 packets survived an 8-minute
  capture). Caught in local verification before reaching any node clients
  actually watch on. Fixed by rescaling into the safe `1/in_rate` unit first,
  then multiplying by `out_rate` as plain `int64_t` arithmetic — never
  constructing the overflow-prone `AVRational` at all, matching the pattern
  ffmpeg's own `libavfilter/af_aresample.c` uses.
- Re-verified against the same real title after the fix: audio packet count
  and duration are healthy across a full 8-minute capture (no dropped audio),
  and the audio/video offset still oscillates within roughly ±15ms instead of
  growing unbounded — same drift-correction quality as 0.23.8's measurement,
  now via a mechanism that cannot shift pitch.
- Same known gap as 0.23.8: no automated regression test yet for the real
  (non-stub) transcode audio path; both this fix and the incident it fixes
  were only caught by live measurement and live listening, not CI. Building
  that harness (Phase 0 of the resilience plan) would have caught both the
  pitch shift and the near-total-silence regression before either shipped.

## 0.23.8 — Bounded audio drift compensation for transcoded playback (development)

- Transcoded audio's presentation clock (`StreamPipeline::audio_next_pts` in
  `src/media_engine.cpp`) was a free-running sample counter, seeded from the
  real source timestamp once at pipeline start and never re-anchored
  afterward — unlike video, which re-derives its PTS from the real source
  timestamp on every single frame. Any systematic mismatch between resampled
  output sample count and real elapsed source duration (resampler rounding,
  EAC3 frame timing, channel downmix) compounded without bound for the life
  of the stream. Measured live against a real title (EAC3 5.1 -> AAC stereo):
  ~0.4ms drift per second, extrapolating to hundreds of ms over a feature-length
  film — matches user reports of audible desync a few minutes into playback.
- Fix: `maintain_audio_drift_compensation()` periodically compares the
  running produced-sample count against the expected count derived from the
  current frame's real source PTS, and uses `swr_set_compensation()` to
  gradually nudge the resample ratio back toward the source timeline (bounded
  to roughly 1% speed adjustment per correction window) rather than either
  leaving the drift unbounded or snapping to a corrected PTS (which would
  produce an audible click).
- Verified against the same real title: offset now oscillates within roughly
  ±15ms across an 8-minute sample instead of growing monotonically, well
  under the threshold where A/V desync becomes perceptible.
- Known gap: no automated regression test exists yet for the real (non-stub)
  transcode audio path — `macha-tests` links a stub media engine
  (`src/media_metadata_stub.cpp`) for speed, and this fix is verified only by
  live measurement against production content, not a deterministic CI case.
  Building that harness is exactly the Phase 0 exit criterion already
  described in `TODO/2026-09-03-playback-resilience-and-av-sync-plan.md`;
  tracked there rather than duplicated here.

## 0.23.7 — Per-node advertised API address fixes any-node Direct Play failover (development)

- `GET /api/v1/status` now reports `api_host`/`api_port` on every entry in
  `nodes[]`: where clients should reach that node's HTTP/catalogue API,
  distinct from the existing `host`/`port`, which is the node's internal RPC
  bind address and was never a reliable (or even necessarily correct-protocol)
  address for REST calls. Any-node Direct Play failover was guessing peer
  ports from the RPC bind address and landing on the wrong port; it now uses
  this field instead.
- New optional `catalogue.api.advertised_host` / `advertised_port` config
  covers NAT/port-forwarding, mirroring the existing `network.advertise`
  pattern for the RPC port. `advertised_host` defaults to this node's
  resolved RPC advertise address (not the bound `listen`, which is
  conventionally a wildcard bind and not itself dialable); `advertised_port`
  defaults to the bound `port`. Distinct from the existing self-only
  `connectivity.advertised` field, which covers this node's own
  external/UPnP RPC-port connectivity, not peers' API addresses.
- Carried over the existing gossip wire's trailing-optional-field pattern
  (`NodeTelemetry`) so nodes mid-rollout on the previous version keep
  interoperating; older peers simply don't report `api_host`/`api_port` yet.

## 0.23.6 — Safe distributed checkpoint and metadata-history compaction (development)

- `MetadataReplica::compact_history_if_safe()` — an existing, tested local
  primitive that re-roots `history.log` at the sole committed accepted head —
  is now actually called in production, gated behind a new cluster-wide
  distributed checkpoint protocol. Previously it had zero callers: a
  generation-only safety check was known to be unsafe (a returning accepted
  branch could outlive the common ancestor on every replica), so history
  compaction stayed permanently disabled and `history.log` grew without
  bound.
- New leaderless propose → durable-ack → commit → prune round
  (`MetadataManager::attempt_history_checkpoint()`, new `propose_history_floor`
  / `commit_history_floor` RPCs, new durable `checkpoint-proof.meta` per
  replica). Compaction only fires once every durably-known participant has
  durably acknowledged the exact same accepted-head hash as the new ancestry
  floor — mirrors the existing `all_known_reachable()` gate already used for
  destructive object GC. A restart only ever trusts a proof that still
  validates against the replica's current committed head.
- A returning node whose own compaction floor has since been pruned
  everywhere else in the cluster (peers compacted further while it was
  unreachable) now converges automatically instead of getting stuck
  advertising an unmergeable rootless sibling forever.
- No client-visible behavior change. Existing nodes with multi-gigabyte
  `history.log` files (from the previously-unbounded retention) shrink back
  to a single root record once the cluster completes its first round.

## 0.23.5 — Cluster-wide ingest/torrent job visibility and control (development)

- `GET /api/v1/ingest/jobs` and `GET /api/v1/torrents/jobs` (list and
  single-job) now answer with every job in the cluster, not just the jobs
  owned by the node the client happened to talk to. Each job is tagged with
  its owning `node_id`. Implemented as an on-demand RPC survey of active
  peers (new `get_ingest_jobs`/`get_torrent_jobs` wire messages), not
  replication — a peer that can't be reached is skipped, not fatal to the
  request.
- `POST .../jobs/{id}/{pause,resume,retry,cancel,clear}` now works
  regardless of which node's API receives the request: if the job isn't
  owned locally, the action is forwarded to the owning node via a new
  `ingest_job_action`/`torrent_job_action` RPC and the result (including the
  updated job, still tagged with its `node_id`) is returned as if it had
  been handled locally. The existing 404-vs-409 semantics (job not found vs.
  job can't perform that action in its current state) are preserved
  cluster-wide.
- No client-visible API surface changed beyond the new `node_id` field on
  each job — existing UIs keep working unmodified, just with complete
  visibility instead of a partial, node-dependent view.

## 0.23.4 — Signed artwork capability URLs (development)

- Embed a signed, short-lived capability URL (`?exp=...&sig=...`, HMAC'd with
  the cluster auth key) directly on each artwork reference in catalogue
  responses (`GET /api/v1/catalogue/items`, `/items/{id}`, `/search`),
  alongside the existing bare `id`/`role`/`mime_type` fields. This lets a
  client load artwork via a plain `<img src>` without attaching a bearer
  header, the same way playback stream/subtitle URLs already carry their own
  embedded authorization rather than requiring a separate header. Unlike the
  session-scoped stream token, artwork has no session to anchor a validity
  window to, so the expiry is explicit and carried in the URL; default TTL is
  24h, configurable via `catalogue.api.artwork_capability_ttl_ms` — long
  enough that normal browsing/caching isn't disrupted, with an expired URL
  recovered by simply re-fetching the catalogue item. The existing
  header-authenticated `GET /api/v1/catalogue/artwork/{id}` endpoint is
  unchanged and still works with a bearer token; the signature is verified
  (not just the URL shape) before the request is ever exempted from that
  check, so an unsigned request to the same path still requires the ordinary
  bearer token when one is configured. Artwork responses also now carry
  `Cache-Control: public, max-age=<ttl>, immutable`, since the id is a
  content hash and the bytes it names never change.

## 0.23.3 — HTTP keep-alive, honest Status telemetry, and metadata/startup reliability (development)

- Implement bounded HTTP/1.1 keep-alive for the catalogue/media API server
  instead of closing every connection: reused connections are bounded by
  configurable `keep_alive_max_requests` and `keep_alive_idle_timeout_ms`,
  yield early under accept-queue backlog so a busy or idle keep-alive
  connection cannot starve a waiting new connection, and always close rather
  than reuse when a request body was not fully drained from the wire.
- Stop presenting stale peer telemetry as current: `/api/v1/status` now
  treats a live sample older than the freshness window exactly as if no live
  sample existed for `storage`/`cache`/`runtime` figures (falling back to
  durable last-known data, or explicit unavailability), rather than showing
  arbitrarily old RSS/CPU/load/capacity numbers as authoritative merely
  because a sample was once observed.
- Add a truthful per-node `phase` (`starting`, `recovering`, `ready`) to
  telemetry so a node's own in-progress local recovery — which legitimately
  reports zero capacity/usage before its storage is ready — is no longer
  indistinguishable on the wire from a genuinely empty node. A recovering
  peer now reports `state: "online"` (control-plane reachable, which is
  true) with `phase: "recovering"` and non-authoritative storage/cache
  figures instead of fabricated-looking zero, and `cluster.conditions`
  reports "one or more online nodes are still recovering" for that window.
  This directly fixes the 0.23.1 rolling-deployment incident where Status
  reported all nodes green with plausible load figures while nodes were
  stopped or 80-100 seconds into recovery.
- Serialize foreground metadata-head reconciliation (`MetadataManager::read_group()`)
  behind a dedicated `reconciliation_mutex_`, re-checked after acquisition, so
  concurrent reads observing the same accepted-head divergence produce at
  most one merge commit instead of each independently publishing its own.
  Unlike the write/repair paths, ordinary reads previously took no lock at
  all here; under real operational churn (node restarts/reconnects) this let
  redundant, mostly `Body::full` reconciliation commits accumulate on every
  concurrent read during a divergence window, which is the direct cause of a
  live node's metadata history growing from 37MB to 20.9GB in under a week.
  The underlying divergence-tolerant merge/history-fetch machinery is
  unchanged; only concurrent access to the merge-and-publish step is now
  serialized.
- Bound `Service::wait_services_ready()`, which previously waited on local
  startup with no timeout at all: a rare (reproduced at roughly 1-in-30
  startups under stress) internal stall below the readiness/subsystem
  construction path — one that neither completes nor throws — could hang a
  node forever with no diagnostic and no way for the process supervisor to
  intervene, since systemd's `Restart=on-failure` only ever triggers once a
  process actually exits. Add configurable `service_startup_timeout_ms`
  (default 120000); on timeout, log the last-known per-subsystem readiness
  state and terminate the process outright rather than attempt an ordinary
  exception unwind, which would try to join a startup thread that may be
  permanently blocked and hang identically in `stop()`. Restart-driven
  recovery replay on the next boot is what actually resolves the stalled
  state. The root cause of the underlying rare stall itself — a suspected
  lock or lost wakeup somewhere in subsystem construction/start — was not
  pinned down and remains open; this bounds its worst-case impact rather than
  eliminating it.

## 0.23.1 — bounded FUSE recovery failure (development)

- Stop terminal asynchronous publication failures from being immediately
  re-admitted as deferred work. A journal-restored inode whose accepted
  namespace path has disappeared now remains explicitly poisoned for recovery
  instead of consuming a worker and flooding logs indefinitely; unrelated FUSE
  paths and control traffic remain serviceable.
- Add a crash analogue covering durable spooled writes followed by accepted
  namespace removal, proving exactly one terminal attempt, scheduler quiescence,
  and continued access to unrelated files.

## 0.22.2 — metadata reconciliation recovery (development)

- Reject compact metadata deltas which cannot reproduce the exact immutable
  snapshot byte ordering, including reconciliation which canonicalises an
  append-ordered garbage/tombstone set.
- Give local metadata replicas the same single-shot full-record fallback as
  remote replicas when a compact body fails exact reconstruction, preventing a
  valid library from becoming permanently unavailable behind a failed merge.
- Make node identity-association reset admission asynchronous: the API returns
  `202` after its small local durable tombstone, while peer propagation and the
  cluster metadata audit execute off the request path.
- Retire reset identities from ordinary status listings, cluster health and
  capacity totals while preserving an explicit per-node `state: "retired"`
  audit response.

## 0.21.0 — asynchronous node startup (development)

- Bring the RPC control plane and status API online before local backend recovery.
- Report startup/readiness independently from cluster reachability and storage availability.
- Make metadata-history cold-start linear and add periodic full-history anchors.
- Build UPnP support against miniupnpc API 18 headers which predate the symbolic
  `UPNP_GetValidIGD()` return-value macros, while preserving private-WAN and
  disconnected-gateway classification.
- Install Linux systemd units in systemd's unit search path rather than a
  Debian multiarch library directory, including migration of the old cached
  default and warning-free generated install scripts.
- Turn configurable FUSE spool capacity into event-driven write backpressure:
  pressure starts publication, admission progressively follows measured drain
  throughput, and saturation blocks writers rather than returning logical
  `ENOSPC`.

## 0.19.0 — metadata replica availability

- Replace fixed metadata-voter placement with all-node metadata replication. Every active node is eligible to durably store namespace and catalogue-control metadata.
- Add `dht.metadata_min_write_replicas`: the literal number of distinct active metadata replicas which must durably store an immutable commit before it may be accepted. Legacy `dht.metadata_replicas` remains a migration-only alias and maps its old voter count to the equivalent former majority write floor.
- Replace the live metadata CAS/PREPARE/COMMIT state machine with immutable commit storage plus durable acceptance certificates. A replica stores a valid commit without comparing it to its current head; acceptance records the distinct durable store witnesses and survives their later absence.
- Persist a maximal accepted-head set on each replica. Independent partition branches coexist instead of competing for a single current slot; accepted ancestors fall out of the head set only when a descendant/merge commit supersedes them.
- Add protocol-20 `put_metadata_commit`, `accept_metadata_commit`, and accepted-head discovery RPCs; reject the legacy network CAS/PREPARE/COMMIT RPC family so live code cannot accidentally re-enter the old consensus model.
- Add encrypted compact metadata ancestry/history, exchange it between arbitrary replicas, collapse stale ancestor heads, and reconcile divergent accepted heads through deterministic two-parent merge commits. N simultaneous heads converge by repeated deterministic DAG merges rather than winner selection.
- Merge non-conflicting namespace changes automatically and preserve incompatible namespace/catalogue-root alternatives as durable first-class conflict records instead of silently choosing a winner.
- Invalidate metadata caches on an independent peer-observation epoch so same-generation sibling heads are discovered promptly; numeric generation alone is no longer a complete freshness signal.
- Let any active replica initiate virgin-cluster genesis. All virgin founders construct the same deterministic generation-2 root and publish it through the ordinary commit-store/acceptance path; there is no genesis coordinator, voter or PREPARE arbitration. Bootstrap-configured joiners remain fenced until their peer survey completes.
- Publish catalogue CONTROL objects against the same any-node metadata write floor and converge them opportunistically to active replicas.
- Fence catalogue CONTROL GC by catalogue-root epoch as well as object age: data-before-metadata staging survives the root epoch in which it is first observed/re-affirmed and cannot be reclaimed by a stale maintenance live-set during root publication.
- Replace metadata voter/quorum status with replica/write-floor fields while retaining deprecated 0.18 JSON aliases for client compatibility.
- Keep non-destructive DATA repair running from an accepted local branch while metadata reconciliation/validation is pending.
- Separate write availability from convergence validation: a reachable `metadata_min_write_replicas` cohort remains writable while reconciliation is pending, with validation reported independently as stability telemetry.
- Add durable causal DATA/CONTROL retention claims on physical copies. Metadata publication re-affirms referenced immutable objects before acceptance; local/remote deletion, placement eviction and GC refuse to remove a claimed copy. Missing claimed copies are repaired in bounded background slices even when the claiming branch is absent from the node's current namespace view.
- Keep GC useful during partitions: unclaimed staging/extra copies remain reclaimable, while inherited claims are released only after every currently known metadata replica is online and the accepted-head set is validated coherent. This prevents one partition from erasing an ancestor object still required by another accepted branch.
- Keep the 0.18 on-disk storage layout readable; bump the peer protocol to 20 because the metadata publication contract is intentionally incompatible with protocol 18/19.
- Fix protocol-20 compact metadata deltas so ordinary mutations retain SM12/SM13 write-floor and retention-governance state, persist completed retention baselines even with an empty migration roster, and preserve mutation-sequence counters across restart.
- Keep legacy accepted-head certificates readable during upgrade while preventing a protocol-20 branch from downgrading to legacy authority, and route background maintenance through the same virgin-cluster policy fencing as foreground discovery.
- Split dependency-free backend tests from concrete yaml-cpp/FFmpeg runtime-adapter tests so storage, cluster, filesystem, catalogue and playback policy coverage can build and run on constrained development hosts; add `run-tests.sh` to execute every test binary present in a build tree.

## 0.18.2 — cluster status telemetry

- Add optional UPnP IGD public port mapping, AWS external-IP fallback, boot/manual public endpoint self-probing, runtime advertised endpoint updates, and Status API diagnostics.

- Add live node telemetry on the authenticated peer protocol, gossiped across the cluster with boot-incarnation sequencing, freshness expiry and rolling-upgrade compatibility.
- Persist a bounded coalesced last-known telemetry cache independently on each node; routine status persistence never mutates the namespace, enters metadata quorum/CAS, or replays historical telemetry. Legacy SM9/DLT3 status records remain readable for compatibility.
- Add durable SM10/DLT4 cluster-wide endpoint→NodeId reset tombstones under `/api/v1/manage`, including endpoint-, NodeId-, and IP-only scopes, live membership/telemetry/RPC eviction, stale-gossip suppression, propagation, confirmation metadata, and audit logging.
- Improve wrong-node RPC diagnostics with endpoint, expected NodeId, and the actually authenticated NodeId.
- Add `/api/v1/status` cluster/node status surfaces with authoritative live membership, explicit metadata availability (`unavailable`, `read-only`, `writable`), known-versus-online storage/cache totals, optional runtime/load/peer/RPC observations, and explicit connectivity re-check actions. Metadata availability is logged only on state transitions.
- Preserve maintenance dependency ordering while publishing metadata availability: when metadata repair cannot establish its required state, that maintenance pass stops before catalogue/GC/repair work rather than continuing against a failed metadata validation.
- Make authenticated metadata-journal replay reproduce the same deterministic same-generation sibling-seed arbitration accepted by the live replica, preventing valid replacement-node convergence state from failing restart with `seed generation conflict`.
- Fence catalogue CONTROL-store GC to the metadata generation used to build its reachability inventory, so a concurrent catalogue commit cannot have a newly-published manifest/shard deleted by a stale maintenance live-set (including with zero garbage grace).
- Protect the data-before-metadata catalogue publication window on every metadata voter: CONTROL GC will not reclaim objects written after that voter observed its current catalogue root, so newly staged manifests/shards survive until the successor root is committed and observed even when `garbage_grace` is zero.

## 0.18.1 — acquisition recovery

- Add an explicit torrent retry action for completed downloads whose linked ingest job failed, reusing the existing persisted ingest job and staging payload without redownloading.
- Keep an operator-paused torrent paused until explicit resume instead of allowing a stale libtorrent status sample to overwrite the Macha job state.

## 0.18.0 — storage correctness reset

0.18.0 defines a fresh on-disk/storage contract and intentionally does not migrate an existing Macha namespace.

- Separate authoritative DATA from priority CONTROL/METADATA storage.
- Rework DATA placement so configured capacity participates in stable deterministic placement and full/offline preferred owners use deterministic fallbacks.
- Define `min_write_replicas` as the synchronous DATA publication floor and `replicas` as the convergence target repaired in the background.
- Add node-local encrypted small-object packing with restart index reconstruction, torn-tail recovery, tombstones and copy-on-write compaction.
- Store catalogue structure as content-addressed shards plus a manifest in CONTROL storage; publish references only after metadata-voter quorum durability.
- Store artwork as ordinary DATA, using the same placement, fallback, replication, repair and GC rules as media extents.
- Add catalogue control-object convergence across current metadata voters and defer scanner work during infrastructure/quorum outages without consuming semantic failure attempts.
- Preserve physical filesystem headroom with per-DATA-backend `reserve_free` admission.
- Retain the metadata memory-amplification corrections: shared immutable record payloads, streaming record hashes, compact/in-place deltas and reduced transient RPC copies.
- Add configurable stale Macha FUSE mount recovery before service startup with `fuse.unmount_if_mounted`.
- Bump the incompatible cluster transport/storage contract to protocol 18.
- Replace migration-era documentation with the current storage, durability and recovery invariants.

Older development history remains in Git history rather than this operational document.

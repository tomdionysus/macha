# Active tasks and concepts to explore

Last updated: 2026-09-13

This is the authoritative, ordered backlog. Detailed plans and UAT records in
this directory remain evidence; completed work belongs in `COMPLETED.md` and is
not repeated here. Work top-to-bottom unless new evidence changes the order.

**Start here if you are new to this work.** Read, in order:

1. The **P0 cluster section** immediately below. The 2026-09-13 storage outage
   is resolved and all three nodes carry data again; what is left is that
   gbni-2 is stranded four releases behind with no SSH route in.
2. **"What the four client sessions now depend on"** near the end of this file.
   Seven API contracts were settled with the client sessions on 2026-09-13 and
   exist nowhere else in this repository. Breaking one breaks four clients.
3. **"Cluster and repository state as of 2026-09-13"**, which records node
   addresses, what is deployed where, what access actually works, why the repo
   version is ahead of the cluster's on purpose, and that nothing since 0.38.2
   is pushed or tagged.

None of the three is a task list; all three will mislead you if you assume
otherwise.

**The two things most worth picking up next**, if nothing else has changed:
`rpc_cluster/test_concurrent_reads_during_divergence_produce_one_reconciliation`
fails 40% of the time on both Pis, in isolation, at a metadata-divergence
assertion — a reproducible lead, not a load flake, and either a racing test or
a real dropped head. And the 10-second `/api/v1/status` under P1, where the
obvious causes are now eliminated and one hypothesis is left standing with a
five-second experiment written out for it.
`2026-09-12-cluster-users-and-roles-plan.md` carries a "What actually shipped"
section recording where that implementation diverged from its plan.

2026-09-05: full reprioritisation pass. Two independent full-repo audits were
run: (1) every doc under `TODO/`, `COMPLETED.md` and `CHANGELOG.md` in full,
cross-referenced against each other; (2) an independent, docs-blind walk of all
of `src/`/`tests/` verifying claims directly against current code (file:line
citations). Findings were then cross-checked against each other. Consequences
of that pass:

- Items confirmed shipped (by both the changelog and direct code inspection)
  were moved to `COMPLETED.md`.
- Items this file called open that code inspection shows are now partially
  shipped were reworded rather than re-litigated from scratch.
- A new **P0 — Verified correctness defects** section and a new
  **P0 — Security hardening** section were added: these are concrete bugs and
  a concrete exposure found by reading the actual code, not carried over from
  older docs. Nothing in `src/` carries a `TODO`/`FIXME` marker — the entire
  informal backlog that would normally live in comments instead had to be
  found as verified behaviour.
- A new **P1 — Scaling cliffs** and **P2 — Code health / error-handling
  consistency** section record real, verified-but-not-yet-urgent debt (the
  system is small today; several of these are O(N) or O(N²) patterns that are
  invisible at current scale and will not stay invisible).
- This pass is still not exhaustive for the 73 dated plan docs in this
  directory — see the "Documentation hygiene" item under P2 for the specific
  staleness this audit found in those docs, root docs, and `CHANGELOG.md`.

2026-09-08: pruning pass. Every remaining item was re-checked against current
source (file:line) and against 0.24.1–0.35.0 in `CHANGELOG.md`. Items that
shipped, that a later release superseded, or that duplicated another item were
removed here and ledgered in `COMPLETED.md`; items whose evidence had moved on
were reworded down to what is actually still open rather than left carrying a
history that reads as work. What went, and why:

- The self-healing programme (0.29.0–0.32.0) and its `[x]` durability-wedge
  finding: shipped. Its three sub-items went with it — the publication hot
  loop is now under `RetryPolicy` backoff and parking (0.30.0), and the 120 s
  startup gate is now a no-progress gate (`startup_progress.hpp`,
  `service_startup_no_progress_ms`, 0.30.0). Only the duplicate-path cause
  survives, promoted to its own item.
- The DLT7 scaling item: shipped as DLT7 in 0.32.0 (`metadata.cpp:780`).
- Four `[x]` correctness/security items (0.24.0–0.24.3) and the retracted
  `LocalStore::valid()` scaling item, which was already restated in P0.
- The unlink-doesn't-abandon-publication sub-item, which was the same fix as
  the "short-circuit unlink" item in P0 structural; merged into it.
- The `mount_path` deployment rule: all three nodes run 0.34.0, so every
  config has already moved.
- The node-50 SSH item: diagnosed off-list as gbni-1 browning out under build
  load, which is now a standing operational rule, not an open investigation.
- Two claims in the "dead wiring" item that code inspection now contradicts
  (`StorageLock` is used at `cluster.hpp:77`; the scanner's
  `request_media_profiles` is invoked from `catalogue_api.cpp:441`).

The governing laws are:

1. Thou Shalt Not Make The Viewer Wait.
2. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.
3. Control traffic must remain promptly serviceable. Viewer priority is a large
   configurable share (95:5 by default), not indefinite starvation of all other work.

## P0 — Cluster: one node still behind, and one blind spot

The 2026-09-13 live incident that opened this file is resolved and ledgered in
`COMPLETED.md`. gbni-1 healed itself on 0.38.3 and es-1 returned on its own.
What the incident left behind:

- [ ] **gbni-2 (inverbeg) runs 0.38.1 and cannot be reached to upgrade it.**
  **2026-09-13: the operator is working on console access and said to work
  around it for today.** Do not plan any change that requires all three nodes.
  Its sshd now offers **password authentication only** — `Authentications that
  can continue: password`, so public-key auth is disabled server-side. The host
  key still matches, so it is the same machine; this is a config change on the
  node, not a different host. Until someone with console access restores
  `PubkeyAuthentication` / `authorized_keys`, that node cannot be deployed to,
  and it is the only node without the pack-recovery fix: a power loss there
  reproduces the whole 2026-09-12 outage.
- [ ] **es-1 has no persistent journal, so reboots cannot be diagnosed.** It
  rebooted at ~2026-09-13 12:41 (the outage this file opened with) and
  `journalctl -b -1` answers "no persistent journal was found". The cause is
  therefore unknowable after the fact, and will be again next time.
  `Storage=persistent` in `journald.conf` is the whole fix. `last` is also not
  installed there.
- [ ] **A node with zero storage capacity reports itself healthy.** gbni-1 ran
  for a day as `state: "online"`, `data storage ready`, `storage_cap=0.0G`,
  because `NodeRuntime::recover_storage` marks the plane ready whether or not
  any backend came online (`cluster.cpp:387`). Readiness should distinguish
  "no backend configured" from "every configured backend is offline". Belongs
  with the "powered-off node is reported online" item under P1 and the
  maintenance-section gap under P2 diagnostics.

## P0 — Playback correctness and poor-network resilience

- [x] **An abandoned playback session holds a node's only transcode slot for
  30 minutes — found 2026-09-10, FIXED in 0.40.0 on the operator's decision of
  2026-09-13: "shorter timeout if and only if the session has never been
  usefully accessed."** `streaming.session_unused_idle_ms` (default 120 s)
  expires a session that has never served a stream object; one playlist,
  fragment, subtitle or Direct Play body earns the full `session_idle`
  permanently, so a paused or seeking player is never evicted by it. A new
  `stream_served` flag carries that, because `stream_touched` is reset by every
  `start_pipeline()` and can only say "not recently". The reaper takes the
  lesser of the two budgets, so an unused session can never outlive a used one.
  `playback/status` reports `session_unused_idle_ms` and
  `unused_sessions_reclaimed`; gated by
  `test_a_session_never_streamed_from_does_not_hold_a_transcode_slot`.
  **Not deployed** — 0.40.0 is unreleased and the cluster runs 0.39.1.
  The second option considered and **not** taken, per the same decision:
  eviction at admission, which converts admission from a guarantee into a
  lease and is client-visible. Original analysis, kept because the mechanism
  is the record:
  The video/audio transcode entitlement lives on the *session*, not the
  pipeline: `video_transcodes_locked()` (`playback.cpp:1152`) counts sessions
  whose `logical_session->video_transcode_entitled` is set and never inspects
  whether an engine is running. Two independent clocks in the reaper
  (`playback.cpp:2601`): `pipeline_idle` (60 s) reclaims the **engine**, while
  `session_idle` (**30 minutes**) is what finally erases the session and
  releases the entitlement. So pipeline reclaim does not free the slot — it
  makes the session cheap to hold while it goes on holding it.
  With `max_video_transcodes: 1` on these nodes, one session created and not
  deleted closes that node to transcoding for half an hour while the node
  looks perfectly healthy, and `reserve_resources` refuses with
  `429 resource_limit` (`playback.cpp:2780`) — there is no eviction path.
  Because the count is per *logical* session, the viewer who caused it is the
  one person who cannot observe it; the cost falls entirely on others.
  **No client-side fix closes this.** The phone client's failover creates a
  fresh session and drops the old one; `@macha/core` closes all five of its
  standby paths but uses `keepalive`, which React Native ignores and Tizen 3
  does not have, so a suspended app's closing `DELETE` may never leave; and a
  client that crashes, is force-quit or loses power can never send one.
  Explicit `DELETE` releases immediately (`playback.cpp:2349-2357`, which also
  stops the pipeline and removes the session temp directory).
  Two server-side options, neither built: a shorter idle for a session never
  fetched from — `stream_touched` (`playback.cpp:617`) is set at construction
  and advanced only by a real fragment fetch, and the reaper already computes
  both that and "has no running engine" every pass, so this is a branch in an
  existing loop needing no new state; or eviction of an entitled, engineless,
  trafficless session at admission, which is more invasive because it converts
  admission from a guarantee into a lease and is client-visible.

- [x] **Complete VOD playlist with bounded segment holds — shipped and
  deployed to all three nodes 2026-09-08.** Verified live on a real transcode
  session: `PLAYLIST-TYPE:VOD` with `ENDLIST` on first fetch, an in-plan
  segment beyond the hold window refused in 0.35 ms with
  `500 segment_not_ready`, and an index past the plan still a genuine 404.
  Two numbers changed during implementation on findings from the client
  sessions, both recorded in the plan: the refusal is **500**, not 503, because
  the status is the only thing a player can read on a fragment error and every
  intermediary emits 503 for a dead service; and the hold is **6000 ms**, not
  the planned 12000, because hls.js's `fragLoadingTimeOut` is deprecated and
  inert and the tightest real deadline is media3's 8000 ms read timeout on the
  React Native music path.
  **Still open. Asked of the phone session on 2026-09-13 (operator: "there is
  [a device], talk to Macha Mobile App React Native"), and partly answered:**
  - **iOS will not be answered by that session at all** — it has only ever run
    on Android, and there is no iOS device there. iOS's time-to-first-byte
    deadline remains unread by anyone. Nobody should plan around it arriving.
  - **HTTP status never reaches that client's JavaScript**, verified against
    the expo-video v57 source: `PlayerError` is `{message: string}` with no
    status, no code, no cause, and segment/playlist requests go from the native
    player straight to the stream URL without passing through the app's HTTP
    layer. So `segment_not_ready` and a genuinely dead stream are
    indistinguishable *on that client*. The 500-over-503 reasoning still holds
    for players that read status; it buys nothing there. Any measurement has to
    come from logcat at the media3/OkHttp level or a packet capture.
  - **Statically, the hold looks right with margin.** Read from the Gradle
    cache rather than from documentation: the video path (expo-video) uses a
    bare `OkHttpClient` — connect 10 s, read 10 s; the music path
    (react-native-track-player) uses `DefaultHttpDataSource.Factory()` defaults
    — connect 8 s, read **8 s**. So the tightest deadline on that client is
    8000 ms against a 6000 ms hold: the player should receive the 500 rather
    than abort first, with 2 s of margin. **If the hold is ever raised above
    8000 ms the music path starts aborting first.** That is an argument, not a
    measurement.
  - **Blocked on the anonymous-roles experiment**: the phone cannot create a
    playback session while anonymous holds no roles, so questions 1 and 2 wait
    on either that ending or credentials for an account with `media_viewer`.

  Design, phases and the corrections made during implementation are in
  [the plan](2026-09-08-bounded-vod-playlist-and-segment-holds.md), which
  also records that the motivating bug report was retracted in full and that
  this work does not address the DTS/TrueHD cold-start latency below — which
  has itself since been retracted in full; see the item below.

- [x] **DTS/TrueHD "40-60x slower cold transcode" — RETRACTED IN FULL
  2026-09-08 by the session that raised it. There is no exotic-audio decode
  problem.** Every measurement behind this item, both the original 0.36.0
  cold-start figures and the steady-state throughput numbers added later the
  same day, was confounded by which node served the session. The client's
  endpoint registry had settled on gbni-2 — the wireless node, whose raw read
  throughput is roughly 0.55 MB/s against gbni-1's 3.31 MB/s — and its
  `EndpointBandwidth` never sampled media transfers, so the node carrying
  essentially all the bytes was the one it measured least and had nothing to
  deprioritise it with.
  Re-run on gbni-1 after the client fixed endpoint selection, same build, no
  server change: Django first frame 43.4 s -> 22.2 s -> **3.5 s**; Death Proof
  25.6 s and 0.46x -> **2.5 s and 1.00x**; Inglourious Basterds 0.14x ->
  **1.00x**; Full Metal Jacket 0.65x -> 0.95x. Headroom now builds on every
  transcode (Django 9 -> 67 s, Death Proof 9 -> 62 s) instead of pinning at
  zero. Nothing starves.
  **The TrueHD audio anomaly is retracted with it.** Death Proof decoding
  ~1.2 KB/s of audio against ~48 KB/s elsewhere was a symptom of a starved
  pipeline, not a codec fault: on the re-run it decodes 502 KB in 20 s
  (25 KB/s) with 479 video frames and zero drops. A pipeline delivering in
  7-second bursts starves audio and video alike, and the comparison was
  against titles that were not starving.
  **Kept as a lesson rather than deleted.** The measurements were real; the
  inferences on top of them kept landing on the server while the variable was
  on the client. Every "same node, same client, within 15 minutes" control
  cited here was assumed rather than recorded. The client now records which
  node served each measurement, which is what would have caught it hours
  earlier. Do not re-open this on the strength of the numbers above.

- [ ] **A seek past the produced window is refused instantly, and on media3
  that is fatal — measured on an Android device 2026-09-13, and it is a
  consequence of the complete VOD playlist rather than of the hold.** Confirmed
  against current source, not inferred from the report. `public_stream_response`
  has two distinct refusal paths (`playback.cpp:1976-1981`): a segment inside
  `segment_count + segment_hold_window` (8) is **held** for up to
  `segment_timeout` (6000 ms), while one beyond that window is refused
  **immediately** with `beyond_hold_window`, because nothing is working toward
  it. A seek to the one-hour mark of a 2:43 title lands hundreds of segments
  past production, so it takes the second path and is answered in well under a
  millisecond. **Raising `segment_timeout` therefore cannot help this case at
  all** — an important correction, because the device session proposed exactly
  that, and the 8000 ms media3 ceiling made it look affordable.
  What the device measured: seek at 15:57:56, `InvalidResponseCodeException:
  500` at 15:58:00.724, surfaced as a **fatal** `ExoPlaybackException: Source
  error` with **no retry** — media3 does not back off and re-request a 500 on
  the HLS path. So the server's hold is the only retry budget in the system;
  there is nothing behind it. The client then treated the 500 as node failure,
  stopped a perfectly healthy session on gbni-1, recorded the node as failed and
  restarted from scratch on es-1, discarding 15 s of completed transcode for a
  6.2 s gap. That failover behaviour is the client's defect and they have filed
  it, but the refusal that triggered it is ours.
  Cold start, for contrast, never touches any of this: direct -> transcode
  admitted in 1722 ms, first frame at ~2.2 s, then 2:30 of content played with
  zero load failures, because transcode on gbni-1 runs faster than realtime for
  that title and no segment was ever late.
  **The design gap:** a `PLAYLIST-TYPE:VOD` playlist with `ENDLIST` tells the
  player every segment exists, and a native player seeks by requesting the
  segment at that offset — it does not ask the server first. Production is
  strictly sequential from the session's seek origin, so everything outside a
  9-segment window is a promise the server will not keep. The seek-only PATCH
  that *does* reposition production exists and is cheap
  (`HlsVodPlan::reusable_seek`, `video_random_access_points`,
  `seek_segment_seconds` — no reprobe, no index rebuild), but nothing tells a
  client it is mandatory before seeking, and on this client the seek never
  reaches JavaScript at all.
  **Corrected 2026-09-13, same day, by the device session against its own
  source:** the seek that produced this measurement *did* originate in
  JavaScript — their own scrubber, which already tracks the pending seek — and
  the app simply never told the node about it. So the client-side fix is
  available to them and they have taken it: on a transformed generation, PATCH
  the session with the new position before seeking the player. That narrows,
  but does not remove, the case for the server-side fix: a seek from the
  lock-screen or notification media session on the music path never reaches
  their JavaScript, and nothing else covers it. Video has no such path. Weigh
  option 1 as covering that narrower case rather than "clients cannot tell us
  about seeks".
  **Both server-side candidates below are now closed, and the direction is
  settled (operator, 2026-09-13).**
  - **Implicit seek on an out-of-window request: REJECTED.** Inferring seek
    intent from a read position is unsound — a reader legitimately touches
    distant offsets for structural reasons (an AVI's index lives at the end of
    the file and must be read before anything can play), and repositioning the
    encoder on that would mean re-reading the tail of a multi-gigabyte file
    across the network for a seek nobody asked for. Note this is also what
    Jellyfin/Emby do, and their thrash under scrubbing is the prior art for
    why not.
  - **A growing `EXT-X-PLAYLIST-TYPE:EVENT` playlist: REJECTED, permanently,
    and this has been round more than once.** Static media is not an event.
    The file exists in full; a playlist that declines to say so is working
    around the server's own limitation at the client's expense.
  - **The direction is to pre-package.** Every rendition transcoded and
    segmented before playback, which is what commercial VOD does and the only
    shape with no seek problem at all: seeking is free because every segment
    already exists. The open question is not whether but **how to do it
    smartly** — what triggers packaging, which renditions are worth producing
    for a given library and client mix, where the segments live and against
    what storage budget, how it is paced against viewer and loader work under
    the governing laws, and what a viewer sees for a title that has not been
    packaged yet. That is a design piece, not a patch, and nothing above should
    be built in its place.
  Historical, for the reasoning only — **neither is to be built**:
  - **Treat an out-of-window in-plan segment request as an implicit seek**:
    reposition production to that segment's random-access point in the same
    generation and then hold. Segment indices are plan-absolute, so
    repositioning within a generation is coherent and the client's URL stays
    valid. This makes the VOD playlist honest, and is the only option that
    helps a player which seeks natively. Needs a debounce and a one-reposition-
    at-a-time rule, or a deeply prefetching player will restart the encoder
    repeatedly — `note_segment_requested` already exists to drag the authorised
    window and is the natural place for the policy.
  - **Document the PATCH-before-seek contract** and tell all four clients. Cheap
    and immediate, but it cannot work where the player seeks without telling the
    app, which is precisely the measured case.
  **The two refusals are distinguishable at the HTTP layer without parsing a
  body**, which decides how dumb a native transport module can be:
  `segment_not_ready` is **500** with `Retry-After: 1` and
  `Cache-Control: no-store`, while `stream_failed` is **503** with neither. So
  status alone separates "retry" from "dead", and the body's `reason` is needed
  only to tell the four not-ready sub-cases apart (`beyond_hold_window`,
  `hold_timed_out`, `session_hold_limit`, `hold_budget_exhausted`).
  Also recorded, because it disposes of an earlier argument: the 500-over-503
  choice is not merely inert on that client, it is harmful. It cannot read the
  code, so it cannot distinguish "hold, I am building it" from "this generation
  is broken", and its failover treats both as a dead node.

- [ ] **gbni-2 serves reads at roughly a sixth of gbni-1 — measured
  2026-09-08, not yet diagnosed.** Client-measured raw read rate with no
  encoder in the path (forced Direct Play, then a plain 8 MB range read):
  gbni-2 0.58 / 0.53 / 0.31 MB/s across three unrelated titles, against
  gbni-1 at 3.31 MB/s. Identical across titles, so it is the node rather than
  extent placement or any particular file.
  This is the actual cause of everything attributed to codecs above, and it
  bounds playback directly: a 21.1 GB / 153 min title needs ~2.3 MB/s of
  source reads for realtime, so ~0.55 MB/s caps a transcode at about 0.24x
  no matter how fast the encoder is.
  gbni-2 is the wireless node
  ([[project-cluster-topology]] records the link as flaky), so the first
  question is whether this is simply Wi-Fi throughput or something in the
  read path on that node. Worth separating with a plain network throughput
  measurement between nodes before looking at storage. Note `cpu_cores` does
  not help a client avoid it: all three nodes report 4 cores, and this is I/O,
  not CPU, so the capacity axis the clients just built sees three identical
  nodes.

- [ ] **1. Correct A/V desynchronisation — partially shipped.** Bounded audio
  drift compensation shipped in 0.23.8/0.23.9 (libswresample `async=1` +
  `swr_next_pts()`, verified ±15ms over 8 minutes with no pitch shift); the
  transcode-seek keyframe-snap and rounding fixes shipped in 0.23.10/0.23.11.
  Still open, per the plan doc's own 2026-09-05 progress note: session-relative
  timeline origin; codec delay/priming applied exactly once by one documented
  owner; monotonic DTS/PTS across encoder flush, fragment rollover and
  generation change; bounded correction of malformed inputs; Direct/Remux
  regressions. **The harness prerequisite is met (2026-09-08).**
  `tests/test_transcode_timeline.cpp` drives the real libav pipeline — no
  injected engine — over a synthesized deterministic source longer than 90
  seconds carrying a non-zero audio start, AAC priming and a seek, and
  measures the published fragments back through libav rather than trusting
  the pipeline's own bookkeeping. It gates: where each output stream starts,
  how much media each carries, per-fragment declared-vs-actual duration, the
  accumulated playlist timeline, and a clean finish. Two cases, ~10s total.
  Measured state on that source: start gap 7ms, A/V span gap 53ms over 100s
  — the drift compensation shipped in 0.23.9 holds. It found two real
  defects on its first run (both ledgered under verified defects below).
  What it deliberately does not cover: **pitch**. A resample-ratio change of
  the kind 0.23.8 shipped keeps the timeline honest while changing how the
  audio sounds, so it would pass. That remains a listening test, and the
  harness says so in its own header rather than implying coverage it lacks.
  Direct and Remux equivalents are not built yet.
- [x] **2. Split lightweight status from expensive diagnostics — shipped in
  0.39.1, deployed 2026-09-13.** `/api/v1/status` is now `cluster`, `nodes`,
  `startup`, `connectivity`, `subsystems` and nothing else; the tree moved to
  `GET /api/v1/status/diagnostics` behind the same `view_status` role, and the
  light response names that route in `diagnostics_endpoint`. Diagnostics was
  68% of the payload live (10,091 of 14,914 bytes), but the locks were the
  larger cost: reaching those numbers took one in nearly every subsystem,
  several held by the busy paths that make someone open Status.
  **The question that prompted it is still open** — see "Status took 10 s" in
  P1 below. The split was deliberately also an experiment: a poll now takes
  none of those locks, so if Status is still slow the cause is not in the
  handler.
  (The other half of this item — `PlaybackManager::status()` holding the
  global session mutex while taking each session's subtitle-cache mutex —
  was confirmed and fixed in 0.24.2: `status()` no longer takes any
  per-session subtitle-cache lock while holding the global mutex, and that
  per-session lock is now `try_lock`-only so one busy session's cache can't
  block a `status()` call at all, only make its own count best-effort.
  Regression test: `test_status_does_not_block_on_a_contended_subtitle_cache`
  in `tests/test_media_playback.cpp`. Separately, the audit's claim that
  `public_stream_response` holds the global mutex across a full libav
  open/probe/seek/demux was re-checked against current code on 2026-09-05
  and is **not accurate** — all such work in both `public_stream_response`
  and `probe_source`/`resolve_session` already runs with the global mutex
  released.)
- [ ] **4. Make transformed output bandwidth-aware.** Auto negotiation selected
  H.264/AAC but, without a client maximum bitrate, CRF output expanded a roughly
  5 Mbps source to bursts around 7–12 Mbps. Define a conservative poor-network
  default, honour client limits, and prevent a compatibility transcode from
  silently increasing delivery demand.
- [ ] **5. Build useful buffer margin.** Do not run a four-second-fragment
  producer and network delivery only one fragment ahead. Bound startup work,
  produce ahead under viewer priority, and recover from transient stalls
  without restarting or multiplying sessions. Note: `MediaSegmentStore` never
  evicts fragments and disk spill is unbounded (2026-09-05 code audit) — any
  buffer-margin work should fix that bound at the same time, not paper over it.
- [ ] **6. Remove avoidable cold transformed-planning work.** Persist complete
  immutable profiles and remux/seek-index suitability. Admission must use
  stored information, never wait on speculative profiling, coalesce background
  scans, and retain normal media-engine fallback. The "immutable profile hit"
  fast path in `probe_source()` already exists and takes no probe reads; what
  remains is moving a cold miss off the synchronous foreground request path
  (confirmed via 2026-09-05 code audit of `playback.cpp`).
- [ ] **7. Improve viewer source delivery — confirmed safe to start,
  narrower scope than it looks.** Investigated 2026-09-05: real prefetch/read-ahead
  (`ReadAheadHintProvider`, `HydrationScheduler`, `CacheHydrator`) and real
  locality preference (`DistributedStore::owners`/`ranked`, `ReplicaSelector::order`)
  already exist and do not overlap items 1/3/4/5/6 below (different files,
  confirmed by direct code inspection). The actual gap is narrower than the
  original wording: `CacheHydrator`'s worker pool fetches one extent per RPC
  with no coalescing of adjacent sequential stripes into fewer/larger transfers
  and no dedup of redundant full-stripe copies. Extend `hydration.cpp`/
  `distributed_store.cpp`'s existing fetch path rather than building new
  infrastructure.
- [ ] **8. Optimise codec throughput only after phases 1–7 are measured.** Keep
  proven bounded decoder parallelism and memory lifecycle. Evaluate hardware
  acceleration or more parallelism only against corrected transport/timelines.
- [ ] **9. Run the ordered UAT matrix.** Test Direct, Remux and Transcode; start,
  seek and quality changes; LAN and impaired Wi-Fi; cold/warm profiles; one/two
  logical viewers; and concurrent loader work. Gate on bounded A/V drift,
  prompt Status, stable playback, no ambiguous 404, exact session admission,
  bounded RSS/CPU and prompt teardown. This is one of nine exit gates across
  the whole backlog with no recorded result yet (see plan docs); none of the
  other eight block this one.

The completed logical-viewer entitlement work is the foundation: capacity is
tied to one persistent logical viewer/UI session, not each stream generation.
Keep teardown active until UAT proves that seek, quality changes, disconnects,
supersession, failure and failover cannot leak physical encoders or produce
`transcode limit reached` for one viewer.

## P0 — Verified correctness defects (found 2026-09-05, code-audit-confirmed)

None of these came from a TODO/FIXME comment — there are none anywhere in
`src/` or `tests/`. Each was independently verified against current source,
not inferred from docs. All are small and isolated; none require design work.

- [x] **Transcoded playlists declared the plan, not the media — found and
  fixed 2026-09-08 by the new timeline harness, on its first run.**
  `playlist()` emitted `#EXTINF` from `vod_segment_durations` (what was
  planned) while `Segment::duration` (what was published) was written and
  never read anywhere. The first flush of a fragmented MP4 writes the delayed
  `moov` and no `moof`, so that boundary produces no fragment and its media
  joins the next one: fragment 0 measured **6.0s of media while declaring
  2.0s**, and since a player builds its seek map by accumulating `EXTINF`,
  every later fragment sat **four seconds early on the timeline for the whole
  title**. The remux path had been given a `carry_boundary()` call for exactly
  this on 2026-09-07; the transcode branch never got one, and the carried
  value was discarded by the playlist regardless. Fixed on both sides: the
  transcode cut now carries an unproduced boundary (`media_engine.cpp`), and
  the playlist advertises published durations (`media_segments.cpp`). Gated
  by `test_transcoded_audio_and_video_carry_the_same_timeline` — reverting
  either fix fails it. Not yet confirmed against a live client on the
  cluster; the measurement is from the published fragments, not from a player.
- [x] **Every completed transcode generation finished in an error state —
  same run, same day.** `MediaSegmentStore::mark_finished()` compared
  fragment *count* against plan entries, so a run that legitimately carried a
  boundary (25 fragments for a 26-entry plan) was recorded as
  `media pipeline produced 25 fragments for a 26 fragment VOD plan`. Not
  cosmetic: `playlist()` withholds a playlist entirely once an error is set,
  so a transcode that had in fact produced all of its media ended by serving
  an **empty playlist**, and the session reported `exit_code=1`. Now compares
  published media against planned media, which is the invariant
  `publish_duration()` actually maintains.

- [ ] **FUSE journal can hold two inodes on one path** (gbni-1: 11240 vs
  7270 on `/TV/Big.Mistakes.S01E01…mkv`, a create-over of a file whose
  previous inode still had pending data). 0.28.3 resolves it at recovery
  instead of exiting, and re-journals the loser, so this no longer stops a
  node starting — what is still unexplained is why the displacing op did not
  clear the old path in the first place (`unlink`/`rename` journal the
  descriptor *before* clearing `current_path` and rely on the op record at
  replay). Find that, and add the hand-built-journal test: 0.31.0's
  `test_fuse_journal_fuzz_every_frame_mutation_still_starts` fuzzes frame
  encoding, not this shape.
- [ ] **A FUSE mount can stop adopting the cluster namespace (live, gbni-1,
  2026-09-06) — root cause fixed in 0.28.2, three related gaps still open.**
  The mount sat two hours behind its own replica: a namespace op published
  before a restart was recovered as "unconfirmed", its effect had since been
  overwritten, and `refresh_namespace_if_stale()` refused every newer view
  while any op was unconfirmed. 0.28.2 retires published ops at recovery
  and confirms live ones by generation (`NamespaceOp::published_generation`),
  and exposes `namespace_refreshed_revision`/`namespace_available_revision`
  plus `FUSE namespace refresh deferred reason=…` so staleness is visible.
  The `stat` 0-bytes-on-two-nodes finding in the next item is plausibly the
  same mechanism (a mount holding a pre-publication entry): rechecked
  2026-09-06 15:50 before the 0.28.2 rollout, all three mounts already
  agreed on 2421711002 bytes after the day's restarts, which is what a stale
  mount (not stale data) predicts. The op that wedged gbni-1 was
  `seq=149 kind=chmod` on an 'Allo 'Allo episode. Still open:
  - [ ] Adoption is also refused while any op is *queued or in flight*
    (`fuse_frontend.cpp` `namespace-queue`/`queue-race` deferrals). An op
    retrying on a retryable backend error (write floor unavailable, peer
    down) therefore blinds the mount to every remote change for as long as
    the retry lasts. Replace the global gate with per-path protection: skip
    only the paths touched by pending ops (`snapshot_path_shadowed()` in the
    recovery path already has the exact rule), adopt everything else, and
    re-run adoption when the pending set changes.
  - [ ] A crash between `apply_namespace_backend()` returning and the
    `namespace_published` journal marker re-publishes the op on recovery
    unless `namespace_effect_confirmed()` happens to see it; a re-published
    `mkdir` gets EEXIST, which is non-retryable, which wedges the queue
    behind an operator skip. Journal the marker before reporting success,
    or treat EEXIST-with-matching-entry as achieved.
- [ ] **`rm -rf` on a FUSE-mounted directory fails with "directory not empty"
  and has no effect, and `stat()` of the identical path returns a different
  size on different nodes (live findings, 2026-09-06) — likely one root
  cause, not two.**
  - Original report: `rm -rf /mnt/machamedia/*` on `corvus-gbni-1` neither
    deletes anything nor reports a sensible per-entry error — it fails
    outright with ENOTEMPTY, which should not be possible for a plain
    recursive delete of files/directories the caller can already
    `readdir`/`stat`.
  - Corroborating finding, found independently while verifying the 0.25.0
    deploy across all three nodes: `stat` on the exact same path
    (`/mnt/machamedia/TV/Chernobyl (2019)/Chernobyl (2019) - S01E01 -
    1.23.45 (1080p BluRay x265 Silence).mkv`) returns the correct size
    (2421711002 bytes) on `corvus-gbni-1` but **0 bytes on both
    `corvus-gbni-2` and `corvus-es-1`**, reproducibly and stably (rechecked
    twice, ~15s apart, unchanged) — not a transient post-restart race.
    Meanwhile every node's own `/api/v1/status` reports the cluster as fully
    converged: identical `metadata_generation` (7552) on all three, quorum
    validated, health "healthy". So the shared/replicated metadata layer
    believes it agrees, but each node's own FUSE-facing view of at least
    this file's attributes does not actually agree — pointing at namespace/
    attribute *projection* inside `FuseFrontend`/`FileSystem` (turning
    replicated metadata into a local `getattr`/`readdir` view), not at
    metadata replication/consensus itself, which the aggregate numbers say
    is fine.
  - This was found by chance while spot-checking a handful of files during
    deploy verification, not a systematic sweep — treat "affects at least
    one file across at least two nodes" as a floor, not a ceiling, on how
    widespread this is.
  - All three nodes were freshly restarted onto 0.25.0 within the same ~90
    minute window this was found in, but nothing in 0.25.0 touches metadata/
    namespace/getattr code (it only added `run_supervised`, the shared
    `macha_core` build, and the subsystem-plugin scaffolding) — a restart is
    much more likely to have *surfaced* a pre-existing local-view bug (by
    forcing every node to rebuild its namespace projection from scratch)
    than to have introduced one, but this has not been confirmed against a
    pre-0.25.0 baseline and shouldn't be assumed either way.
  - Not yet root-caused. First places to look: whatever `getattr`/`readdir`/
    `unlink`/`rmdir` return for entries with in-flight or already-completed
    namespace mutations, and specifically why a freshly-restarted node's
    rebuilt local view would disagree with another node's for the same
    replicated metadata generation (the P1 scaling item on `readdir` being
    O(entire namespace) touches the same frontend code, but this is a
    correctness bug, not a performance one). Needs a minimal repro (a small
    directory tree, known contents, compare `stat`/`readdir` output across
    all three nodes, then delete it) before guessing further.

- [ ] **A hard failure in one subsystem takes down the entire macha process
  (new, found 2026-09-05 live incident) — foundation shipped in 0.25.0,
  FUSE/Torrent migration still open.** `FuseFrontend`'s durable-journal
  replay throws `DecodeError` on any unexpected record, uncaught, which
  crashes the whole node — including its unrelated metadata/RPC/API roles —
  not just the local FUSE mount. Lived this directly: a 0.24.3 bug in
  `skip_blocked_namespace_operation()`'s journal bookkeeping (fixed in 0.24.4)
  crash-looped `corvus-es-1` 49 times because journal replay runs in the
  constructor with no isolation. 0.25.0 shipped the mechanism (Phase 0 of
  [subsystem crash isolation via a plugin architecture](2026-09-05-subsystem-plugin-isolation-plan.md)):
  a mandatory `run_supervised` thread-entry guard on every subsystem thread
  (~30 sites), `macha_core` as a shared library, the `Subsystem`/plugin ABI,
  and `SubsystemSupervisor` (`dlopen` + version-checked load + backed-off
  retry + disable-after-N-failures), verified against real fault-injecting
  `.so`/`.dylib` test plugins. 0.28.0 shipped Phase 1: BitTorrent acquisition
  is now a real `dlopen`'d module (`libmacha-torrent`), reached through
  `TorrentService`/`SubsystemRegistry`, absent-or-faulted per node at runtime.
  This item still does not close: `FuseFrontend`'s constructor — the one that
  actually crash-looped es-1 — is exactly as unprotected as it was until
  Phase 2 moves FUSE into its own plugin and off the main thread. Single
  binary, single process throughout, no separate OS processes/IPC (considered
  and rejected).
- [ ] **Terminal durability poisoning is never cleared.** `fuse_frontend.cpp`
  sets `durability_poisoned = true` on any exception during the durability
  batch and nothing ever resets it — one transient fsync failure disables all
  writes on that node for the rest of the process lifetime. Needs an explicit,
  deliberate recovery path (even if only "restart the process"), not silent
  permanent lockout.
- [ ] **Unbounded startup wait with no escape.** `wait_for_initial_namespace()`
  (`fuse_frontend.cpp`) busy-waits in 100ms sleeps forever, with no timeout or
  cancellation, and runs from the `FuseFrontend` constructor before
  `fuse_mount`. A node whose metadata replica never becomes available hangs
  indefinitely with only a debug log line to show for it.
- [x] **FUSE-mounted reads never register as viewer demand — RESOLVED in
  0.40.0 as intended behaviour, on the operator's answer of 2026-09-13: "FUSE
  is loader, not viewer."** The dead `note_viewer_activity()` declaration and
  definition are gone, the policy is written down on the `FuseFrontend` class
  itself, and the tests that used the hook to simulate viewer pressure now
  drive `FileSystem::note_foreground_activity()` directly, which is what the
  HTTP playback path does. Original finding:
  `FuseFrontend::note_viewer_activity()` is declared, documented as "called by
  the kernel adapter before viewer-critical open/read callbacks", and defined
  — but is never called anywhere. FUSE reads open with `FrameType::loader`
  unconditionally, so the viewer/loader duty-cycle gate that governing law 1
  depends on is driven exclusively by the HTTP playback path today. If any
  client reads media via the FUSE mount directly (rather than through HTTP
  streaming), it currently gets loader priority, not viewer priority. Confirm
  whether this is intentional (FUSE is documented elsewhere as
  "loader/convenience traffic") or a real gap, and wire it up or remove the
  dead declaration.
- [ ] **`rpc_cluster/test_concurrent_reads_during_divergence_produce_one_reconciliation`
  fails 40% of the time on aarch64, reproducibly, in isolation — found
  2026-09-13. This is the flake worth fixing first, because unlike the three
  below it does not need load to reproduce.** Measured on both Pis at
  `--serial` with nothing else running: **4 failures in 10 runs on gbni-1, and
  4 in 10 on es-1**. It has never failed on macOS/clang locally across many
  full-suite runs, so it is aarch64/GCC or simply timing on slower hardware.
  **Not caused by the 0.39.1 status split**, which was the suspicion when it
  surfaced: es-1 was rebuilt with `src/status_api.*` and the three touched test
  files reverted to their 0.39.0 contents and scored *the same* 4 in 10, so the
  behaviour predates that change.
  It fails at `tests/test_rpc_cluster.cpp:2831`,
  `REQUIRE(accepted_heads().size() == 2)` — the setup assertion, before the
  test's actual subject. The test hand-builds two sibling metadata heads at the
  same generation on one node and expects both to still be accepted when it
  looks. Sometimes only one is. The obvious candidate is that the node
  reconciles the divergence on its own between `make_sibling` returning and
  that line — which is precisely what the test then goes on to measure, so a
  race against it is plausible without any product defect. The other candidate
  is that a head is genuinely being dropped, which would be a real bug.
  Deciding which needs someone to instrument `accepted_heads()` over the gap;
  both answers are useful, and a 40% reproduction rate makes it cheap.
- [ ] **Three load-dependent test flakes needing a real fix, not another
  isolation-pass shrug — second found 2026-09-08, third 2026-09-13.**
  The third is
  `hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`,
  which timed out at its full 60 s under four-way parallel load on es-1 during
  the 0.38.3 and 0.38.5 rollouts and passed in `--serial` isolation at 364 ms
  and 470 ms. Same shape as the two below; noted because the item named two
  tests and there are now three, which starts to look like one shared cause
  rather than three separate races.
  - `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
    has now failed under parallel-suite load and passed in isolation on at
    least five separate occasions across this project's history (three plan
    docs, plus runs on 2026-09-05 and again on 2026-09-08 during the bounded
    VOD playlist work). "Passes in isolation" has been the accepted verdict
    every time; the actual race has never been root-caused. It fails at
    `test_hydration_catalogue.cpp:3827`, a `wait_until(..., 12s)` on superseded
    objects being absent from both nodes' local stores.
  - `invariants/test_status_collects_connected_peer_telemetry_without_client_fanout`
    — **new, 2026-09-08.** Timed out at its full 60 s deadline once during a
    full-suite run, then passed three times in `--serial` isolation at 277 ms,
    275 ms and 526 ms, and passed every subsequent full-suite run that day
    (five or more). Authorship was considered rather than assumed: it was first
    seen on a working tree carrying the phase-1 `wait_object`/playback-route
    changes, but the case builds two `Service` instances and never creates a
    playback session, so neither changed code path is reachable from it. The
    revert-and-reproduce cycle used for the aarch64 hang was deliberately *not*
    run here, because against a flake this rare two green runs on a reverted
    tree are indistinguishable from two green runs without the revert — the
    experiment has almost no power at that sample size, and claiming it settled
    anything would be false precision.

  **Hypothesis worth testing before hunting two separate races (2026-09-08, a
  hypothesis and not a finding).** These may be one problem. Both cases stand
  up multiple `Service`/node instances on real ports, both end in a
  deadline-bounded `wait_until` rather than an assertion, and both fail by
  timing out rather than by asserting anything false. The one failing
  full-suite run measured `wall=81741ms` against 33–37 s for the green runs on
  the same machine that hour — roughly 2.2x slower overall, which is what
  CPU starvation under `effective_parallelism` around 5 looks like. If the
  deadlines in these multi-service cases were sized against an unloaded
  machine, both would be timing bugs in the harness rather than races in the
  code, and root-causing either one separately would find nothing. Check that
  first: it is cheap, and it would explain why five investigations have ended
  in "passes in isolation".

  Do not record a sixth sighting in place of doing this.

## P0 — Security hardening for a network-exposed cluster

Found during the 2026-09-05 code audit. Macha's own `SECURITY.md` already
states the trust model plainly ("Anyone with the cluster key can authenticate
as a node and access cluster data") — these are gaps *within* that accepted
model, on the HTTP surface a client (and potentially the public internet, via
the offsite Spain node) actually talks to. Given the cluster already spans a
home network and an offsite node, this is not a hypothetical exposure.

- [ ] **Wildcard CORS header remains.** `Access-Control-Allow-Origin: *` is
  still sent unconditionally on every response, including mutating ones. Less
  severe now that every route requires a valid session bearer token (a
  malicious page can't drive the API without already possessing one), but
  still not best practice — a page that somehow obtained a token (e.g. one
  leaked to a compromised client) could use it cross-origin undetected. Stop
  sending a wildcard origin on any endpoint that doesn't strictly need it.
- [x] **Authorization tiers — shipped in 0.38.0, deployed 2026-09-12.** See
  `COMPLETED.md`. Every route now requires a session and is gated on roles.
  Two consequences left open below.
- [ ] **The whole HTTP API is reachable from the public internet, and
  anonymous can read the library.** All three nodes moved to public
  `https://<name>.macha.network` endpoints on 2026-09-12. Verified from outside
  the network: `POST /api/v1/session` with no credentials returns 201, and that
  token reads `/api/v1/catalogue/items`. This is `session.allow_anonymous: true`
  plus the `anonymous` account holding `media_viewer` — correct for a
  television on a LAN, permissive on a public endpoint. **The operator was told
  and chose to keep it** (2026-09-12, "single user alpha"). Revisit before this
  is anything but alpha. To close it: `PATCH` the anonymous account's roles to
  `[]` (no restart, effective next session), or set
  `session.allow_anonymous: false`, or stop exposing 7438 and reach it over
  WireGuard.
  **2026-09-13, asked again and declined for now:** the operator is
  deliberately running with anonymous holding no roles in order to exercise how
  the system behaves in that state, and does not want `allow_anonymous` turned
  off yet. That is a live experiment, not an oversight — do not "fix" it. It
  has a cost: the phone client cannot start a playback session at all while it
  stands, which is blocking the device measurements requested below.
- [ ] **Mixed-version sessions break during a rolling upgrade.** A session
  minted by a pre-0.38 node carries `roles: ["anonymous"]`, which an upgraded
  node refuses with 403 on every route. The session *wire format* is
  compatible; the role vocabulary is not. No compatibility shim exists.
  Operator's call on 2026-09-12, reaffirmed 2026-09-13: "we'll have to deal
  with that for the time being." Upgrade every node promptly, or write the shim
  before beta. Note this is currently live rather than hypothetical — gbni-2 is
  on 0.38.1 and cannot be upgraded.
- [ ] **Unbounded JSON recursion depth.** `json.cpp`'s recursive-descent parser
  has no depth limit. Combined with the 8 MiB body cap, a deeply nested body
  on any POST/PUT can exhaust the stack. Add a depth limit.
- [ ] **Untrusted length fields drive large allocations before validation.**
  `metadata.cpp` (at least 4 sites), `telemetry.cpp` and `cluster.cpp` each
  read a length/count field off the wire and `reserve()` a container to it
  before reading any of the actual data — e.g. a 4-byte field in `metadata.cpp`
  can trigger a multi-hundred-MB allocation from one small malicious or
  corrupt message. Cap reservations to the remaining message size.
- [ ] **Unsigned identity-reset tombstones accepted from any peer.**
  `membership.cpp` checks only `epoch >= existing` before evicting a roster
  entry and tearing down routes for it.
- [ ] **Unvalidated, echoed `Content-Type` on a bearer-exempt endpoint.**
  `POST /api/v1/catalogue/items/{id}/artwork?mime=` accepts any non-empty
  `mime` query value and `GET /api/v1/catalogue/artwork/{id}` echoes it
  verbatim with `Cache-Control: public, immutable` and no `nosniff`/CSP, on an
  endpoint that doesn't require the bearer token. Validate against an
  allow-list of image MIME types.

## P0 — Structural ingest, metadata and retained-memory safety

Resume the
[structural ingest/runtime remediation](2026-09-02-structural-ingest-runtime-remediation.md)
after the immediate playback correctness blocker. Existing checkpoints remain
valid evidence, but do not prove the end-to-end invariants.

- [x] **`repair_once()` holds the metadata mutation mutex across per-peer
  replication RPCs, wedging every metadata mutation on the node — found live
  on es-1, 2026-09-10; FIXED 0.37.0 (lock released across the fan-out,
  re-validated on re-acquire), gated by
  `test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes`
  via the new `stall_peer_for_tests` fixture.** Still under the lock, and
  deliberately so: discovery and `publish_commit`, each bounded by the 30 s
  control no-progress deadline rather than unbounded. `MetadataManager::repair_once()` takes
  `std::unique_lock mutation_lock(mutation_mutex_)` (`metadata_manager.cpp:1908`)
  and then, still holding it, calls `replicate_accepted_head()` once per
  active peer in three separate loops (`:1936`, `:1963`, `:2008`). Each of
  those reaches `push_history_to_peer()`, whose `remote_has` lambda issues a
  blocking `node_.call(owner, has_metadata_history_entry, …)`
  (`metadata_manager.cpp:496`) per history hash. `mutate_impl()` — the entry
  point behind every `mutate_delta()`, and therefore behind every
  `WriteHandle::commit()` — takes the *same* mutex at `:1625`. So one slow or
  unresponsive peer converts a background maintenance pass into a total stall
  of local metadata writes.
  **Measured on es-1 while wedged:** the `macha-maint` thread (LWP 650116) sat
  in `Service::loop → repair_once → replicate_accepted_head →
  push_history_to_peer → AsyncRpc::get()`, and **ten** threads were piled up
  behind it blocked in `MetadataManager::mutate_impl` on
  `pthread_mutex_lock` — the ingest worker among them, inside
  `copy_file → WriteHandle::commit → FileSystem::commit_write → commit_file`.
  Nothing had crashed: there were no `subsystem '…' thread stopped` lines in
  the journal, and `cluster.health` cheerfully reported **`healthy`** with all
  three nodes `online` throughout, which is the same
  observability gap as the "powered-off node reads as online" item in P1.
  The lock is documented as protecting "discovery, accepted-head selection, or
  reconfiguration" (`:1906`). Replication is none of those — the header of
  that very loop says "Convergence is replication, not head replacement" and a
  peer holding a different branch simply keeps it — so the peer RPCs look
  releasable, but the baseline-commit branch (`:1977`-`2019`) does mutate and
  must stay serialised. Fix by narrowing the lock to selection/commit rather
  than by putting a deadline on the RPC; a deadline only bounds how long the
  node is dead for.

- [x] **Ingest is strictly serial, so one wedged job stalls the whole queue —
  FIXED 0.37.0: `ingest.max_concurrent_jobs` (default 10), claimed-set
  ownership, dedicated catalogue poller, `concurrency` on `ingest/status`.**
  `IngestManager::loop()` (`ingest.cpp:1039`) selects a single job and calls
  `process_job()` synchronously to completion before looking at the next;
  `active_job_id_` is one `std::string` (`ingest.hpp:140`) and there is one
  worker thread (`:141`). This is what turned the metadata stall above into
  the visible symptom: **six torrent ingests on es-1, all reporting
  `queued`**, staged under `/mnt/diskB/ingest/torrents/` with staging at
  3.78 GB of a 500 GB limit — i.e. nothing resource-bound, just five jobs
  behind one that could not finish. Wanted: concurrent jobs under a
  configurable bound, `ingest.max_concurrent_jobs`, default 10.
  Note the interaction with the retained-memory items below — N concurrent
  imports means N concurrent `WriteHandle`s against the same durable-lower
  budget, so the bound is a memory knob as much as a throughput one.

- [x] **A publication whose basis went stale retried forever, silently — found
  live on gbni-1 2026-09-10, FIXED 0.37.1.** One inode failed 68 consecutive
  times with `parked_publications` at 0 and health `healthy`. Three defects in
  series: `commit_file`'s content-change guard reported a permanently stale
  publication basis as retryable `EAGAIN` (now `ESTALE`, which replays against
  a fresh writer); the park budget's density rule was unreachable at the
  backoff ceiling, 30 min / 30 s = 60 attempts against a threshold of 100 (now
  backstopped by `RetryPolicy::max_failing_duration`, default 1 h, deliberately
  separate from `failure_window` so a long WAN/wifi outage does not park every
  publication); and a long failure run was DEBUG-only (now WARN plus
  `publications_retrying_persistently` on `diagnostics.filesystem`).
  Trigger was rsync `--append-verify` appending to a file whose publication was
  in flight — a legitimate thing to do that the system mishandled.

- [ ] **A job being imported still reports `queued` — found 2026-09-10, not
  root-caused.** The head job on es-1 carried `files_total: 1`,
  `bytes_total: 739234786` and a populated `current_file` while its `state`
  read `queued`. That combination should be unreachable: `plan_job()` persists
  `scanning` on entry (`ingest.cpp:1338`) and returns the job to `queued` once
  planning completes (`:1342`), then `import_job()` persists `importing`
  *before* it ever sets `current_file` (`ingest.cpp:1590`-`1605`). So either
  the cluster-aggregated `/api/v1/ingest/jobs` view is merging a stale peer
  copy over the local record, or something resets the state after planning.
  Worth settling because it is why a wedged queue reads as an idle one — the
  operator sees six identical `queued` rows and no indication that any work
  was ever started.

- [x] **es-1 publication livelock starves RPC and takes the node out of the
  cluster — FIXED 0.36.8 + 0.36.9, deployed 2026-09-09, see `COMPLETED.md`.**

- [ ] **The no-progress counter added in 0.36.9 misses two extent-publishing
  paths, and counts the wrong thing anyway — found 2026-09-10, latent.**
  `FileSystem::write_progress_` ticks in `drain_one_extent()` and `commit()`,
  but `rebuild_step` (`filesystem.cpp:1229`) and the non-pipelined branch of
  `flush()` (`filesystem.cpp:489`) publish durable extents without ticking it.
  A node writing non-sequentially — `rsync --inplace` patching partially
  present files, which is what any resumed import does — can publish at full
  speed while the counter reads flat, and under memory pressure that fails
  healthy work with `EAGAIN` and eventually parks it: the exact failure 0.36.8
  exists to prevent, reintroduced at a different site.
  **Patching the two call sites is not the fix.** The waiter is blocked on the
  shared durable-lower budget, which anyone's release can free, so
  publication's own releases were never the right thing to count; and ticking
  in `rebuild_step` would re-arm every blocked waiter on work that frees no
  memory, masking a genuine deadlock for the length of a long rebuild.
  Move the budget into `RetainedMemoryLedger::acquire()`
  (`retained_memory.hpp:290`), which already owns the loop, the condition
  variable, the deadline and `available_locked()`; the predicate must be **per
  waiter**, not a global release counter, or the deadline becomes decorative on
  a busy node and the original wedge goes undetected again. Re-derive
  `publication_no_progress_deadline_ms` against the corrected counter rather
  than inheriting 30 s. Keep a publication-specific Status field alongside any
  ledger-wide one — its absence is what made es-1 unreadable.
  Not fired to date: `waits.loader` is 0 on all three nodes across 15 hours of
  heavy ingest, because 0.36.9's bound removed the memory pressure the bug
  needs. Full write-up in
  [`2026-09-09-publication-hold-and-wait-plan.md`](2026-09-09-publication-hold-and-wait-plan.md).

- [ ] **The open-writer bound is soft and can overshoot — found 2026-09-10.**
  `runnable_data_locked` tests `writer_cap_reached()` before selecting an
  inode and the worker opens the writer afterwards, so N workers can each pass
  the test at bound-1. Overshoot is up to `commit_workers - 1`; es-1 reported
  `peak_open_publications` 9 against a bound of 8 within a day of the deploy.
  Harmless as configured (9 writers is 108 MB against a 512 MB durable-lower
  budget) and the misleading comments are corrected, but making it exact needs
  the slot reserved at selection time under `data_queue_mutex`, the way
  `reserved_video_transcodes` already reserves a transcode entitlement across
  its admission window.

- [x] **`WriteHandle::drain_one_extent` waits on its extent future with no
  deadline — FIXED 0.37.0.** The real mechanism was one layer down:
  `put_impl` spilled a silent replica after `write_stall` and looked for a
  replacement, but a spilled put still counted as unfinished, so with none
  available the loop never concluded (an infinite 1 ms spin). The extent put
  now carries the pipeline's `DataWorkContext` and fails retryably once
  nothing has moved for the no-progress budget. The fault-injection hook this
  was waiting on now exists (`RpcClient::stall_peer_for_tests`); gated by
  `test_extent_put_to_a_silent_peer_fails_within_the_no_progress_budget`,
  confirmed to hang against the pre-fix code.

- [ ] Finish process-wide retained-memory ownership bounds for decoded metadata,
  catalogue/profile state, reconciliation retries, RPC/reassembly, object
  payloads and playback. Retest unexplained idle RSS growth and the fixed
  `hydration executor is stopping` shutdown race.
- [ ] **Confirm the decrypted history/materialisation cache is bounded by
  bytes, not just entry count, and sheds under pressure.**
  `metadata_materialization_cache_bytes` exists as a config default and is
  reported in Status, but no audit or test has watched it actually shed
  end-to-end under memory pressure. (The compaction protocol itself shipped
  in 0.23.6 and the `accept_commit()`-holds-`m_`-across-`persist()` half was
  fixed in 0.24.3; both are ledgered.)
- [ ] **Retention journal grows ~N² over a large import.** Each publication
  quantum re-claims every extent of the file, so a file published in K
  quanta writes K × extents retention entries. Recorded during the 0.32.10
  measurements in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md)
  and not acted on. The rest of that incident's programme — the serial
  `has_on` loop, the writer-side barrier, the CONTROL put fan-out — shipped
  in 0.26.0 and 0.32.7–0.32.10 and is measured on the real cluster there
  (es-1 retention avg 5,217 ms → 121 ms, no deadline-exceeded since 0.32.7).
  The other follow-ups that run recorded are also still open: present-content
  skip on writer restart
  ([plan](2026-09-06-skip-redundant-replica-writes-for-present-content-plan.md)),
  journal compaction while busy, compact extent encoding, and 9 standing
  conflicts that need a human.
- [ ] **Short-circuit unlink of an in-flight (not yet published) write —
  raised 2026-09-06, during the `has_on` incident.** Confirmed against
  the actual code: `FuseFrontend::unlink()` (`fuse_frontend.cpp:5311`) only
  detaches the pathname from `state_->paths` and journals the op; it does not
  touch the inode's `data_ops`/`durability_pending`/`unconfirmed_data_entry`
  state — nor do rename-over or truncate. `journal_data_abandoned` is reached
  only from `abandon_corrupt_data` and from *recovery*
  (`test_fuse_recovery_abandons_publication_for_file_removed_from_namespace`),
  never from the live namespace path. The
  `refresh_namespace_if_stale()` comment confirms this is deliberate today:
  "Open handles and dirty state retain the detached inode object." So an
  unlinked-but-still-publishing file's data keeps flowing through the full
  pipeline (spool → durability → distributed publish → retention) to
  completion, and only afterward does it become ordinary garbage eligible
  for the `garbage_grace` GC pass — wasted CPU/disk/network for content that
  is already known, at unlink time, to be moot. Wanted behaviour: on unlink
  of a dirty inode with no writable handles, (1) retire its unpublished
  `data_ops` and stop scheduling further publication work for it, (2) reclaim
  its spool bytes immediately rather than waiting through
  durability+publish+grace, (3) ensure the eventual durable history records
  "never existed" rather than "created then deleted," so retention/GC never
  has to process those extents at all, not even later.
- [ ] **Catalogue does not react to a committed unlink — raised 2026-09-06,
  same discussion.** When a namespace unlink is actually durably committed/
  synced (not the short-circuit-unlink case above, which is about aborting
  publication early — this is about the ordinary case where a real file is
  genuinely removed), nothing today notifies the catalogue layer to check
  whether that removal affects any catalogue item. `manage_api.cpp`'s
  `DELETE /api/v1/manage/filesystem` handler calls `hints_.erase_prefix(path)`
  (line ~805) on that one path, but that only clears scanner *hints*, not
  catalogue items, and the ordinary FUSE `unlink()` path
  (`fuse_frontend.cpp:4627`) has no catalogue awareness at all. Wanted
  behaviour: on commit, determine whether any catalogue item's full set of
  referenced files is now empty, and if so remove or update that item —
  **not** a naive "unlink one file -> delete the catalogue item," since (a)
  two namespace paths can reference the same underlying file, and (b) a
  single catalogue item may legitimately reference more than one file (e.g.
  multiple quality variants/parts). The invariant to hold: the catalogue
  (and therefore the Movies/TV UI) must never continue showing an item that
  no longer has any surviving backing file — no phantom library entries.
- [ ] **`DELETE /api/v1/catalogue/items/{id}/metadata` hangs** (separate
  issue, previously filed inside the item above). It hangs because its HTTP handler
  currently performs `repair_once()`, materialises and copies the complete
  catalogue, repeatedly scans all items to discover descendants, walks all
  artwork, and waits for the distributed metadata commit before replying. The
  endpoint must promptly validate the item/revision, durably enqueue one
  idempotent clear job, and return `202 Accepted` with a job/status `Location`;
  retries must recover the same operation. Background execution must use an
  indexed descendant closure and delta-sized mutation, remain below control and
  viewer priority, publish completion/failure explicitly, and preserve atomic
  hierarchy removal plus targeted media rematching. Add large-hierarchy,
  concurrent retry, restart/recovery, conflict, failure and latency regressions;
  neither request handling nor status polling may perform repair, full-snapshot
  copying, remote durability waits or artwork-wide scans.
- [ ] Complete durable namespace batching for mixed create/rename/unlink chains.
  Keep rename a safe singleton until crash/restart proof exists.
- [ ] Make spool backpressure smooth and visibly progressive near its configured
  limit, pacing toward measured publication/drain rate instead of alternating
  full-speed bursts and apparent freezes. **Still reproducible 2026-09-10**:
  under a sustained 6 MB/s import gbni-1's spool sat at exactly
  `max_spool_bytes` (16.00 GB) with rsync throttled to 11 kB/s and its own ETA
  reading `??:??:??`. The mechanism is working as designed — ingest is paced to
  publication drain — but from outside it is indistinguishable from a stall,
  which is precisely what this item is about.
- [ ] Prove large-history, partition/sibling-head, cache-pressure,
  unclean-restart and stale-FUSE recovery, then run a guarded overnight
  four-node rsync UAT. Require bounded RSS/swap/history, automatic rejoin,
  writable metadata, no viewer/control regression and no manual mount cleanup.

Do not tune aggregate ingest throughput around known amplification. Older FUSE
throughput, heap-audit and ownership documents remain detailed evidence but are
absorbed here rather than separate active programmes.

## P1 — Cluster connectivity, status and operations

- [ ] **`GET /api/v1/status` took 10 seconds once — observed by the operator
  2026-09-13, cause not found, and the obvious suspects are eliminated.**
  What was ruled out by reading the code and measuring the live nodes, so that
  nobody spends the time again:
  - **It is not the handler computing.** `status_response` does no I/O and
    makes no network call; every field is an atomic, an in-memory snapshot or
    a short-held mutex. Ten seconds is a *wait*.
  - **It is not connectivity or UPnP.** Status copies a cached
    `PublicConnectivityStatus`; only `POST /status/connectivity/check` probes.
    Confirmed against the journal — no connectivity or UPnP activity at all in
    the two hours around the observation.
  - **It is not the FUSE or metadata diagnostics providers.**
    `FuseFrontend::diagnostics()` is ~60 relaxed atomic loads and `noexcept`;
    `MetadataManager::cluster_status()` is atomics.
  - **It is no longer the diagnostics locks**, because 0.39.1 moved them off
    the polled route entirely. That is the experiment: if it recurs now, the
    cause is not inside the handler.
  **The remaining hypothesis, untested: HTTP worker starvation.** The API has
  16 workers and a 15 s `keep_alive_idle_timeout`. A worker that has answered a
  request and is waiting for the next one on a kept-alive connection blocks in
  `recv_before` for up to that long, pinned. `queue_has_backlog()` exists to
  close a connection rather than keep it alive when others are waiting, but it
  is only consulted *between* requests — never while a worker is already
  blocked waiting. Ten seconds sits inside that 15 s window, and three client
  families each holding connections would do it. There was exactly one
  established connection per node when measured, which is consistent with an
  intermittent fault under client load rather than a standing one.
  **The five-second test that tells them apart**, next time it is slow: hit
  `/api/v1/health` and `/api/v1/status` on the same node. Health touches two
  atomics and is ungated, so *both slow* means the request never reached a
  handler and it is the worker pool; *health fast, status slow* means it is
  inside the handler and deserves gdb stacks.
  Instrumentation was offered and not built: per-section `elapsed_ms` in the
  diagnostics assembly, plus slow-request logging in the HTTP layer. For an
  intermittent fault that is what converts "saw it once" into an answer.

- [ ] **`test_storage_data_credit_reserves_viewer_headroom_and_control` hangs
  on aarch64 — pre-existing on HEAD, confirmed not from the 0.36.0 work
  (2026-09-08).** The case times out at its full 60 s deadline on both
  gbni-2 and es-1, in the full suite and in `--serial` isolation, on a build
  of current HEAD. It passes in 372/372 on macOS (arm64, AppleClang) and
  passes in 52 ms on gbni-1's older build tree (2026-09-07 04:10), so it is
  both platform- and revision-sensitive: something between that build and
  HEAD broke it on aarch64/Linux. Authorship was established rather than
  assumed — reverting `media_segments.cpp`, `media_engine.cpp` and
  `test_framework.cpp` to HEAD on gbni-2 and rebuilding reproduced the hang
  identically, so the 0.36.0 changes are not the cause. The delta therefore
  falls in the 0.34.x/0.35.0 line.
  The hang is early: the captured output stops after `node metadata ready
  generation=1`, before any RPC result, and no `REQUIRE` failure is printed,
  so the body blocks rather than asserting. The case covers DATA credit and
  viewer headroom reservation — governing-law-1 territory — so a genuine hang
  there is worth root-causing rather than filing as flake. It is *not* a
  flake: it reproduces serially, every run, on two separate machines.
  Note the live cluster has been running affected code since 0.35.0; 0.36.0
  neither introduces nor worsens it.
- [ ] **A powered-off node is reported `state: "online"` (live, 2026-09-08).**
  While gbni-1 was physically dark — no ICMP response, incomplete ARP entry,
  SSH `Host is down` — both surviving nodes' `/api/v1/status` listed it as
  `"state": "online"`. es-1's own roster entry in the same document carried
  `live_age_ms: 14061248` (~3.9 hours) while also labelled `online`. This is
  direct live corroboration of the aggregation half of the Status
  truthfulness item below: the per-sample freshness fix (0.23.3) is working
  in that the age is reported honestly, but nothing folds that age into the
  `state` the aggregate advertises. A node that has been unreachable for
  hours should not read as `online` to an operator or a failover client.
- [ ] **`metadata_quorum_validated` and `metadata_replica_set_validated`
  report `false` with a fresh timestamp (2026-09-08, all three nodes,
  post-0.36.0).** Observed alongside `metadata_quorum_available: true`,
  `metadata_availability: "writable"`, `health: "healthy"`, `conditions: []`
  and three replicas online with generations converging — i.e. the cluster is
  demonstrably fine. The `*_validated_at_unix_ms` values were only ~20 s old,
  so validation is running and returning false rather than never running.
  Either the flag means something narrower than its name suggests, or it is
  wrong; either way an operator reading Status cannot currently tell.
  Possibly the same aggregation gap as the item above. Not a deploy blocker,
  not yet diagnosed.


- [ ] Support multiple advertised endpoints per durable node (LAN/WAN,
  IPv4/IPv6 and configured/discovered), multiple bootstrap candidates,
  reachability-aware racing/fallback, expiry and deduplication by node identity.
  Integrate UPnP/external-IP discovery. Test poor Wi-Fi, partitions, NAT without
  hairpin, endpoint changes, simultaneous dial and commit interruption.
- [ ] Revisit UPnP/external-address discovery sources generally (currently only
  wired to the RPC port's `network.advertise`/`connectivity.advertised`); when
  this happens, fold in the 0.23.7 per-node advertised API endpoint
  (`catalogue.api.advertised_host`/`advertised_port`, `nodes[].api_host`/
  `api_port`) added for any-node Direct Play failover, which today is a static
  config-only override with no UPnP/external-IP probing of its own.
- [ ] Make client/API failover preserve one logical operation: reuse session
  idempotency keys, reconcile ambiguous POST results, fail over endpoints, and
  never turn transient transport loss into a misleading 404 or duplicate lease.
- [ ] **Unconfirmed, flagged during 0.24.0 joint client/server live testing:**
  the client's cluster discovery advertised a node as `toms-macbook-pro.local`
  rather than `127.0.0.1`, and a request to that `.local` hostname sat pending
  (possibly slow mDNS resolution, possibly a CORS mismatch for that specific
  origin). Not investigated — surfacing here since it may be the same
  advertised-endpoint-discovery gap as the item above, or a distinct client-
  side resolution issue.
- [ ] **Correct aggregated Status truthfulness and freshness — partially
  shipped.** 0.23.3 fixed the *freshness* half (a stale sample now reports
  explicit `unavailable`/`recovering` state rather than fabricated zero or
  stale-as-live data) — confirmed directly against `status_api.cpp`. **Still
  open:** the *aggregation* half. This was independently reproduced live
  *after* 0.23.3 shipped (`2026-09-03-fuse-terminal-recovery-loop.md`): node
  200 reported `state: online` while genuinely recovering at generation 0, and
  separately reported ES-1 as green at generation 0 in its own aggregate view
  while ES-1's own endpoint reported healthy/writable at generation 4507.
  **2026-09-09: measured on the live cluster and split into three causes in
  [node telemetry visibility](2026-09-09-node-telemetry-visibility.md).**
  Causes 1 (a stale sample blanked its whole `runtime` block) and 2
  (`metadata_generation` preferring a membership record carrying 0 over a
  live sample) are fixed there. **Cause 3 also shipped, in 0.36.7**: gossip
  was sent only after 2 s free of foreground *and* read-ahead work and then
  only onto an idle writer, so a node went invisible exactly while busy or in
  trouble. Both gates are gone, gossip runs every tick on the SPECULATIVE
  class at `network.telemetry_interval_ms` (10 s), and a demand-driven wake
  publishes sooner but no more than once a second. **Still open:** audit
  how a node folds a peer's telemetry into its own aggregate response — this
  is a different code path from the per-sample freshness fix. This is likely
  the same underlying gap as playback P0 item 2's Status-latency investigation
  above; resolve together rather than tracking twice.
- [ ] Add optional display-only `node_name` at `.nodes[].node_name`; configure
  `Corvus GBNI-1`, `Corvus GBNI-2`, `Corvus ES-1`, and `Corvus MacBook Pro`.
- [ ] Complete hard-kill stale-FUSE recovery proof and automatic clean rejoin.
- [x] **Two torrents stuck in `metadata` forever with no error — found live on
  gbni-2 2026-09-10, FIXED 0.37.2.** libtorrent's default `listen_interfaces`
  enumeration binds `eth0` and loopback but never `wlan0`; gbni-2's `eth0` is
  `NO-CARRIER`, so its session held loopback sockets alone and could reach no
  peer. Deterministic, not a startup race — a restart rebound identically. The
  engine now binds the node's advertised address (`torrent.listen_interfaces` /
  `torrent.listen_port` override), and the plugin consumes libtorrent alerts at
  all for the first time, so a loopback-only session, a failed bind, a DHT
  bootstrap or a tracker refusal is now in the journal instead of silent.
  **Still open:** none of this reaches the HTTP API. `torrents/status` says
  nothing about listen endpoints or DHT, and `TorrentJob` carries `peers`/
  `seeds` as bare counts with a free-text `error` — so a client cannot tell a
  dead session from a slow swarm. The macha-client team asked for exactly that
  on 2026-09-10 (session health with structured warning codes; per-job
  trackers, stall durations, connected-vs-candidate peers, structured errors).
  It needs new `TorrentJob` fields, a persistence-shape change and the
  cluster RPC bridge to carry them, so it is real work, not serialisation.

- [ ] Diagnose faulty torrent/ingest independently so it does not obscure
  convergence and runtime measurements. **2026-09-10: largely answered** by
  the `repair_once()` mutation-mutex item under P0 structural ingest — the
  torrent/ingest subsystem was not itself faulty, it was the most visible
  victim of a node-wide metadata stall. What remains here is the narrower
  original ask: enough per-subsystem signal to tell those two apart without a
  gdb backtrace.
- [ ] Diagnose `ingest failed: metadata acceptance certificate durability
  floor unavailable` failures on torrent ingest once the torrent has
  downloaded, which are also unaccountably slow. **Probably the same root
  cause** as the `repair_once()` item under P0 structural ingest: that exact
  string is thrown by `ensure_accepted_head_durable()`
  (`metadata_manager.cpp:889` and `:899`), which `mutate_impl()` calls at
  `:1675` while holding `mutation_mutex_` — so an ingest commit reports it
  after waiting out whatever else held that mutex, and "unaccountably slow" is
  precisely what a caller queued behind a peer RPC under that lock looks like
  from outside. Re-check this once that item lands rather than diagnosing it
  separately.
- [x] **Sanitizer build — shipped 2026-09-08. CI — declined by the operator,
  not deferred.** `MACHA_SANITIZE` builds the whole tree (core, executables,
  plugins, both test binaries) under `address`, `undefined`,
  `address,undefined` or `thread`; whole-tree rather than per-target because
  `macha_core` is a shared library the executables link and the plugins
  `dlopen`, so partial instrumentation would leave the interposed allocator
  and the shadow memory disagreeing across that boundary. `thread` combined
  with `address` is refused at configure time. Case deadlines now scale
  automatically under instrumentation (3x ASan, 10x TSan, `--timeout-scale` /
  `MACHA_TEST_TIMEOUT_SCALE` to override), because the declared 30/60/120s
  deadlines were chosen against an ordinary build and a spurious timeout would
  hide the report the run existed to produce. Documented in
  `tests/TESTING.md`; the LSan hook the runner already had is unchanged.
  The CI half of this item was **declined by the operator on 2026-09-08** —
  it is not a backlog item awaiting time. The gap it named is therefore real
  and standing: every regression gate remains a human running
  `./run-tests.sh` before deploying, and nothing runs TSan periodically
  unless someone does. Do not re-file CI as an open item; the sanitizer build
  is the part of it that was wanted.
  - [ ] Still open, and now cheap: no TSan run has been made yet against the
    concurrency-heavy subsystems (`net.cpp`/`metadata.cpp`/`playback.cpp`).
    The ~47-thread hand-reasoned lock ordering that made this the audit's
    biggest process gap is still unexercised by a sanitizer. ASan+UBSan
    across the full suite is green as of 2026-09-08.

## P1 — Scaling cliffs (found 2026-09-05, not yet urgent at current 3-node/home scale)

These are real, verified, O(N) or worse patterns that cost nothing today and
will not stay that way. Listed here rather than P0 because nothing currently
observed ties a live symptom to them — but the worst one (`readdir`) is a
plausible contributor to "the mount feels slow with a big library" if that's
ever reported, and is worth fixing opportunistically rather than waiting for
that report.

- [ ] **`readdir` is O(entire namespace).** `fuse_frontend.cpp` iterates *all*
  paths under the global namespace mutex, taking each inode's mutex, for every
  directory listing — same pattern duplicated in `rmdir` and twice in
  `rename`. `FileSystem::NamespaceIndex` already has a parent→children index
  the frontend doesn't use for this. This is the single worst scaling property
  found in the codebase audit.
- [ ] **SHA-256 plus a heap allocation inside a `std::sort` comparator.**
  `placement.cpp`'s `fallback_score()` allocates and hashes twice per
  comparison, and `StoragePool::ranked()` — hit on every put/get/has/valid/
  remove — sorts using it.
- [ ] **`MetadataManager::node_info()` and `replica_nodes()` are O(n²) roster
  copies per metadata read** (a full roster copy per call to `node_info()`,
  called once per id inside `replica_nodes()`).
- [ ] **`CatalogueHintQueue` is a linear `find_if` over the whole map at 8
  call sites**, with no id→path index — O(N²) per scan.
- [ ] **Whole-catalogue deep copy on every single mutation.** 8 sites do
  `auto current = *current_snapshot();` (a full catalogue copy) then re-shard
  and re-encode all 64 shards for a single-item change.
- [ ] **`StagingArea::reserve()` runs a full recursive directory-size walk
  under its own mutex**, called every 500ms per active torrent and on every
  `GET /api/v1/ingest/status`.
- [ ] **`MediaInformationService::source_for()` linear-scans the entire
  filesystem snapshot** computing a media ID per entry, per lookup, on every
  publication and hint.
- [ ] **`admit_deferred()` is O(all inodes) with a per-inode lock, called
  after every publication quantum.**
- [ ] **`RetainedMemoryLedger::request_shedding_locked` linear-scans every
  live allocation under the global mutex** — exactly under memory pressure,
  which is the worst time to do it.

## P2 — Raised by client teams and the operator, not yet decided (2026-09-13)

- [x] **The anonymous account has no password, and a roles-less anonymous
  account is no longer reported as "disabled" — raised by the operator and,
  independently, by the web client session; both fixed in 0.38.4 and deployed
  to gbni-1 and es-1 on 2026-09-13, verified live against the cluster's own
  roles-less anonymous account (403 `anonymous_disabled` before, 201 with
  `roles: []` after).** The reported "cannot set a password" turned out to be the right
  behaviour arrived at for the wrong reason: the API *did* permit it, and that
  was a live privilege hole. `/api/v1/users/me` needs only `media_viewer`,
  which anonymous holds at genesis, so any unauthenticated visitor could
  `PATCH` a password onto the anonymous account and then log in as it — and the
  username/password mint path never consults `session.allow_anonymous`, so the
  resulting bound session survived anonymous access being switched off.
  Anonymous now has no credential at all (`kdf` 0) and `verify` refuses the
  username outright, which also makes the random password existing clusters
  carry inert without a migration. Separately, an anonymous account with no
  roles now mints a session carrying `roles: []` instead of `403
  anonymous_disabled` — that is how a registered-users-only deployment is
  expressed, and the client needs the empty list to know to show a login.
  Details in `CHANGELOG.md` under 0.38.4.

- [x] **Status has no role of its own — requested by the operator via the web
  client session, 2026-09-13; shipped in 0.38.5 as `view_status`, with
  `/api/v1/health` added for liveness.** The operator's decision on the crux
  below was that Status *should* be gated: an anonymous visitor sees cluster
  health only if the anonymous account holds `view_status`, which it might not.
  Anything using `/api/v1/status` as a health check was using the wrong route,
  so there is now a right one — unauthenticated, role-free, and reporting only
  whether this node is serving. The upgrade problem was solved by resolving
  role implications at mint rather than only at write, so existing accounts
  gain the role on their next login with no migration. Details in
  `CHANGELOG.md` under 0.38.5.
  **Original framing, kept because the reasoning is the record:** The client gated its Status
  section on `manager`, which takes the diagnostic screen away from an ordinary
  viewer at exactly the moment it earns its place; leaving it ungated shows it
  to a session the server granted nothing. The capability being asked about is
  neither "manage the library" nor "view media" but "see the health of this
  cluster", and no role says that. A `view_status` role would.
  **What has to be decided first, because it is a reversal.** `/api/v1/status`
  is deliberately ungated today (`service.cpp:191`) and the comment there
  argues the case: an importer-only account watching an ingest is the person
  who most needs to see whether the cluster is healthy, so requiring a role
  would tell them nothing. Introducing `view_status` means that route stops
  answering for any session that lacks it — including the roles-less anonymous
  session a registered-users-only deployment now mints, which is currently how
  a client reaches Status to render a login wall at all. So the question is not
  only the role's name but whether Status becomes gated, and what an
  ungated-but-sessioned caller sees instead. Also needs: the route set
  (`/api/v1/status`, `/api/v1/status/*`, connectivity checks), whether
  `manager` implies it (roles are capabilities, not a ladder, so implication
  has to be argued rather than assumed), and what existing accounts get at
  migration. The client is unblocked — it is on `manager` today and says
  switching is a one-line change once a name ships.
- [x] **`GET /api/v1/users` returns `{"users": [...]}` while every other
  collection in the API uses `items` — CHANGED to `items` in 0.40.0 on the
  operator's decision, 2026-09-13.** Core has accepted either key since its
  0.8.0, so no client needs a release, and gbni-2 goes on emitting `users`
  until it can be upgraded. The operator's other point: the web client should
  not be reading that endpoint directly at all, since it is core's surface.
  Core checked and reports the web client uses its `UsersApi` accessor
  throughout with no envelope handling anywhere — so the accept-either shim is
  in the phone client or one of the two TV clients, and is worth finding: a
  client holding its own copy of a wire format will not notice the next change
  either. Original note: Raised independently by the mobile
  session, which read `users_api.cpp` directly; the web client had already
  built an accept-either shim after its page silently rendered nothing (reading
  `.items` off a payload without it yields `undefined`, which throws nowhere).
  Single records from `POST`/`PATCH` are returned bare, which both clients
  assumed correctly. Cheaper to settle now than after a client ships around it.
  The inconsistency was inherited from the manage endpoints rather than chosen.
- [x] **Clients cannot tell which build a node is running — DECLINED
  2026-09-13. The answer is semver and nothing else.** No git describe, no
  commit hash in Status. The consequence is accepted rather than unnoticed:
  a behavioural claim pinned to a version is only as good as the discipline
  that every behaviour change moves the version, which is what the 0.40.0 bump
  for a removed response field already demonstrates.
- [ ] **Tracker list is stale.** 111 tracker errors in five minutes on gbni-2;
  `coppersurfer.tk` and others have been dead for years. DHT carries the
  torrents, so this is noise rather than breakage, but it buries real tracker
  failures.

## P2 — Catalogue and media model

- [ ] Make negotiation representation-aware. One Macha work identity may
  reference several immutable files/assets, each with a persisted profile.
  Select the best Direct representation, then cheapest correct Remux/asset
  combination, then cheapest suitable Transcode while respecting quality,
  stream flags, availability, locality, seek cost and resource limits.
- [ ] Replace provider-shaped public IDs (`tmdb:*`) with opaque Macha work IDs.
  Keep provider IDs internal, preserve aliases during migration, and keep work
  identity separate from immutable content hashes.
- [ ] Update clients to consume immutable profiles and send a useful bandwidth
  ceiling plus a persistent logical-viewer/session identity.
- [x] **Make signed artwork capability URLs actually cacheable — FIXED in
  0.40.0 on the operator's instruction, 2026-09-13.** `exp` is now quantized to
  a bucket of the TTL (rounded up to the bucket after next, so remaining
  validity is always between one and two TTLs), making the URL byte-identical
  for every request inside a bucket and letting the existing 24 h `immutable`
  header be consulted for the first time. Gated by
  `test_catalogue_artwork_url_is_stable_so_it_can_be_cached`. **Not deployed**
  — 0.40.0 is unreleased. Clients need no release; the two id-to-URL memos can
  be deleted once it ships. Still open and unowned: "cached posters go stale
  after a minute or two" was never the bucket expiring, because there was no
  bucket, so that symptom has a different and still unmeasured cause. The
  measurement behind the fix: ("images load slowly, and when 'cached' they're just less slow").
  What the measurement settled, against current source, so nobody re-derives it:
  the artwork response already sends `public, max-age=86400, immutable`
  (`catalogue_api.cpp:551`, the max-age being `artwork_capability_ttl` in
  seconds), so the header is not the problem; `exp` is **not bucketed at any
  granularity** — `signed_artwork_url()` uses `unix_ms() + ttl` per call
  (`catalogue_api.cpp:99`), and `artwork_json()` runs it for every item on every
  `/items` and `/items/{id}`, twice per item since `artwork` and
  `effective_artwork` are both emitted; and a stable content identity for the
  bytes **already exists on the wire** as the artwork `id` (a SHA-256 of the
  bytes), which core already carries as `ArtworkRef.id`. Two clients
  independently built an id→url memo beside a key they already had; that was a
  core documentation gap, not a missing field, and core is fixing the comment.
  A header-free URL form is also already guaranteed — the signed capability is
  bearer-exempt (`capability_request()`, `catalogue_api.cpp:564`) — so a client
  reporting that artwork needs a header is constructing its own URL instead of
  using the payload's.
  Because there is no bucket, "cached ones go stale after a minute or two" is
  **not** a bucket expiring and has a different, unmeasured cause. Do not
  attribute it; the web client is measuring it.
  The fix, agreed in shape with core: round `exp` **up to a bucket boundary of
  the TTL** rather than to a bare hour — a naive hourly bucket would give a URL
  minted at 10:59 an hour of life instead of a day, invisibly to clients. One
  consequence core flagged: it treats a capability whose `exp` has passed as
  non-re-hostable onto other nodes, so that path will fire more often near a
  boundary. Correct behaviour, and the authenticated URLs are the recovery.
  Original filing: `exp`/`sig` are
  recomputed fresh on every `/items`/`/items/{id}` catalogue call, so the same
  artwork object gets a different query string (and therefore a different full
  URL, which browsers key their cache on) every time — the existing 24h
  `Cache-Control: public, max-age=86400, immutable` on the artwork endpoint
  never gets consulted, and posters are re-fetched over the network on every
  page load. Fix by quantizing `exp` to a coarser bucket (e.g. top of the next
  hour/day) so `sig` becomes a pure function of `(artwork_id, quantized_exp)`;
  repeated fetches within that window then return an identical URL. No
  client-side change needed. Found 2026-09-04 via `macha-client-b8`.

## P2 — Diagnostics and repeatable proof

- [ ] Record reproducible local and four-node benchmark recipes without brittle
  default-suite wall-clock thresholds.
- [ ] Keep diagnostics bounded, snapshot-based and disabled by default when
  they perturb viewer behaviour; never instrument per packet or fragment on a
  critical thread merely to diagnose a P0.
- [ ] **Status has no maintenance section at all — found 2026-09-10.** The
  diagnostics object exposes `convergence`, `data_resources`, `data_store`,
  `filesystem`, `metadata`, `retained_memory`, `rpc_server` and
  `rpc_transport`, and nothing for the maintenance loop: no work remaining, no
  queue depth, no pass progress. So "has background maintenance finished?"
  cannot be answered from the API — only inferred from `DIAG high thread CPU
  name=macha-maint` lines in the journal, and from GC reclaim messages. That
  is discipline 1 of the self-healing plan (background work whose progress is
  observable) unaddressed for the one subsystem that runs continuously. It is
  the same shape of gap the publication counters closed in 0.36.9.

## P2 — Code health and error-handling consistency (found 2026-09-05)

Not urgent, but real debt worth chipping away at opportunistically. No design
work needed for any of these.

- [ ] **Duplicated logic that can silently drift.** `fixed_vod_durations` is
  defined byte-identically twice (`media_engine.cpp` and
  `media_engine_common.cpp`); `fuse_frontend.cpp`'s `load_journal` reimplements
  `scan_fuse_journal_frames` inline instead of calling the shared function that
  exists specifically to be the single implementation of torn-tail semantics;
  `net.cpp`'s `PeerConnection` and `Session` are ~450-line near-duplicates;
  three near-identical `MediaInput` implementations exist in `playback.cpp`,
  `media_information.cpp` and `media_catalogue.cpp`; `catalogue_api.cpp` has
  private `json_escape`/`url_decode` duplicating `Json::dump()`/
  `http_url_decode()`.
- [ ] **Empty `catch (...) { }` blocks that discard exceptions with no
  log at all** — concentrated in `net.cpp` (29 as of 2026-09-08, up from 21)
  and `storage_pool.cpp` (10).
  The `storage_pool.cpp` cluster in particular (backend I/O failures in
  `has`/`remove`/`list`/`older_than`/`rebalance_step`/`scrub_step`) will make
  diagnosing a flaky physical disk very hard in the field.
- [ ] **Inconsistent error-signalling contracts.** `DistributedStore::put(span)`
  throws on quorum-unavailable while the sibling overload `put(id, span) -> bool`
  returns false for the identical condition; `LocalStore` mixes bool-for-no-space/
  throw-for-I/O-error/optional-for-absent/noexcept-swallow-everything across its
  own methods. `catalogue_api.cpp` catches `std::exception` and returns 503 for
  what are actually 400-shaped client errors (malformed JSON, unknown kind, bad
  artwork id, invalid `If-Match`) — contrast the correct 400/404/409 ladder in
  `manage_api.cpp`.
- [ ] **Confirmed-dead wiring, worth deleting or finishing, not leaving
  half-connected.** Re-verified 2026-09-08; two of the original five claims
  no longer hold and were dropped (`StorageLock` is used at `cluster.hpp:77`;
  the scanner's `request_media_profiles` is invoked from
  `catalogue_api.cpp:441` via `service.cpp:494`). Still true:
  `PlaybackManager`'s own copy of `request_media_profiles`
  (`playback.cpp:668`) is stored and never called; `status_api.cpp:479`
  hardcodes `metadata_replica = true` for every node regardless of actual
  role; three status fields (`ffmpeg_available`/`ffprobe_available`/
  `ffmpeg_version`, `playback.cpp:2388`) are hardcoded false/false/empty and
  marked "kept for one release" with no tracked removal date; two vestigial
  config knobs (`recovery_commit_workers`, `foreground_commit_workers`) are
  parsed, range-validated and logged but gate nothing.
- [ ] **Test-only hooks are live branches in the production hot publication
  loop.** `suspend_loader_for_tests` and `fail_publication_once_after_spool_bytes_for_tests`
  (`config.hpp`) are real conditionals compiled into the shipped binary, not
  behind a test-only build flag. Low risk today, but worth gating out of
  release builds since they're reachable via ordinary config.
- [x] **`MANIFEST.sha256` — DELETED in 0.40.0.** 104 of its hashes failed
  `shasum -c`, nothing in the build referenced it, and the operator's decision
  on 2026-09-13 was that the file has no purpose. Do not reintroduce it
  without a build step that maintains it.

## P2 — Documentation hygiene (found 2026-09-05, backlog-adjacent but not code)

- [ ] **Swagger/OpenAPI description of the HTTP API — WANTED, and soon
  (operator, 2026-09-13, upgrading the 2026-09-07 "optional").** This is now
  the largest piece of agreed but unstarted work in this file, and it should be
  generated from the route table at build time rather than written by hand, so
  that it cannot drift from `service.cpp`. Four client sessions currently learn
  the API by reading `status_api.cpp`/`service.cpp`, which is how two of them
  ended up holding private copies of a wire format. Publish an OpenAPI 3 document for `/api/v1/*`
  (session, status, catalogue, playback, manage routes) and serve it from the
  daemon (e.g. `/api/v1/openapi.json` plus a Swagger UI page, or generate the
  document at build time from the route table so it cannot drift). The UI and
  site sessions consume the API by reading `status_api.cpp`/`service.cpp`
  today; a machine-readable contract would replace that. Nice-to-have, not
  on the import/stability critical path.

- [ ] `TODO/2026-09-03-playback-resilience-and-av-sync-plan.md` cites
  `TODO/2026-08-31-cluster-any-node-playback-failover.md` as tracking a
  client-side fix — that file does not exist anywhere in the repo. Either
  create it with the actual current tracking location or fix the reference.
- [ ] `TODO/COMPLETED.md` stops at 0.24.0 (last updated 2026-09-05). Thirty
  releases have shipped since — 0.24.1 through 0.35.0, including the whole
  self-healing programme (0.29.0–0.32.0), the subsystem-plugin foundation
  (0.25.0/0.28.0), the playback/streaming work (0.32.x–0.34.0) and SPA
  serving (0.35.0). The 2026-09-08 pruning pass ledgered the entries it
  removed from this file; the rest of that range is still unledgered.
- [ ] `docs/streaming.md`, `README.md`, `ROADMAP.md` and `VALIDATION.md` each
  contain claims contradicted by shipped code or by each other (keep-alive
  described as absent when it shipped in 0.23.3, a stale "0.19 metadata
  availability" README headline at 0.23.11, `ROADMAP.md` referring to "stale
  voters" from a voter model abolished in 0.19, and `VALIDATION.md`'s build
  command not matching `CONTRIBUTING.md`/`tests/TESTING.md`). None of these
  are load-bearing for engineering decisions right now, but they will mislead
  the next person who reads them at face value.
- [ ] `TODO/namespace-publication-and-metadata-efficiency.md`'s own checkboxes
  contradict its header ("Phases 0-5 ... complete" while Phase 4 shows all 16
  items unticked and Phase 3 shows 6 of 13 unticked, despite both phases having
  separate docs and a `COMPLETED.md` entry recording them as done). Reconcile
  the checkboxes with the ledger so this file stops contradicting itself.

## What the four client sessions now depend on (settled 2026-09-13)

Negotiated with the `@machafoundation/core` session and relayed by it to the
web, Android TV and mobile clients. None of it exists anywhere else in this
repository, and a server change that breaks one of these breaks four clients at
once. Recorded here because the conversation that settled them was
cross-session and will not be in the next session's context.

- **`GET /api/v1/health` is the liveness contract.** No token, no role, works
  during recovery. `200 {"status":"ok"}` when serving, `503` with `starting` or
  `failed` when not, and the HTTP status carries the same answer as the body.
  Core probes it every 10 s for latency ranking, failover and the endpoint
  pre-save gate. It must stay unauthenticated and must keep reporting nothing
  else: no version, no node id, no topology.
- **An old node answers `401`, not `404`**, to that route, because
  authentication happens before routing. Core falls back to
  `/api/v1/catalogue/status` on *any* answer that is not a liveness answer,
  which retires itself once no node needs it. This fact is why a 404-only
  fallback would have fired on every node except the one that needs it.
- **`view_status` gates the Status screen and nothing operational.** It is in
  core's `UserRole` and `USER_ROLES` with a test pinning the order. Health,
  ranking, failover and the connection gate are all indifferent to it.
- **A role-less session gets `403` from `/api/v1/status`, never a reduced
  payload.** There is no reduced-view code path; the gate is above the handler.
  Two client reports of "200 with an empty roster" were gbni-2 (ungated 0.38.1)
  misattributed to gbni-1 — see the endpoint-attribution note below.
- **`/api/v1/status` no longer carries `diagnostics`** (0.39.1). Core never
  typed that block, so it is unaffected; a client whose Status screen reads
  diagnostics needs `/api/v1/status/diagnostics`.
- **`GET /api/v1/session` needs a session and no role**, and re-checks
  `credential_generation` on every request, so it catches revocation and not
  only expiry. A `401` there can mean the account changed underneath the token;
  a client must not tell a viewer their session merely timed out.
- **A PATCH naming `mode` clears `video`, `audio`, `max_height` and
  `max_bitrate`** (`playback.cpp:248`). Unchanged since 0.34.0 and confirmed
  against 0.39.1. Android TV has been told to delete its local rule; if they
  can produce a refusal from a body containing `mode` and nothing else, that is
  a real regression and should be treated as one.
- **A role-less session learning no cluster membership is correct**, per the
  operator: such a session sees only the endpoint it was configured with.
- **Record which host served a measurement before reporting it.** Two separate
  client reports today were gbni-2 attributed to gbni-1, and the retracted
  DTS/TrueHD investigation in September was the same mistake at larger scale.
  The node is in the URL; there is no excuse for losing it.

## Cluster and repository state as of 2026-09-13

Facts a new session needs before touching anything. None of this is a task.

**Nodes.** gbni-2 moved off the home LAN on 2026-09-12 and is now
`root@inverbeg.macha.network` (public address; same machine, identical host
key). gbni-1 is `10.44.1.50` / `macnessa.macha.network`, es-1 is `10.34.1.50` /
`ramaroja.macha.network`. All three advertise public `https://` API endpoints.

**Access is not what it was.** During 2026-09-12 the SSH key stopped working on
gbni-2 (`Permission denied (password)`) having worked earlier the same session,
and es-1 is unreachable at every layer. gbni-1 was reachable at the end.
SSH to gbni-1 and es-1 is filtered from outside — port 22 is refused or times
out from both a laptop and from gbni-2 — so a session with no LAN route can
reach only whatever nodes happen to be exposed.

**Versions deployed (2026-09-13, after the 0.39.1 rollout).** gbni-1 and es-1
run **0.39.1** (`libmacha_core.so` sha256 72cb8162…, GCC 14.2.0, built on each
node from the synced tree; the `macha` binary is a thin main and is unchanged
across these releases). Verified live on both after the rollout:
`/api/v1/health` 200 `{"status":"ok"}` unauthenticated, `/api/v1/status` and
`/api/v1/status/diagnostics` both 403 to the cluster's role-less anonymous
session.

**The repository is at 0.40.0 and the cluster is at 0.39.1. That is correct,
not drift.** 0.40.0 is open and unreleased — no code sits between the two. It
was bumped because 0.39.1 removed a field from a response clients poll, which
is a breaking client-visible change that a patch number understated; the next
piece of work starts on 0.40.0. Redeploying purely so the nodes report the new
string is cosmetic and was deliberately not done, since a rolling restart costs
a viewer interruption.

**gbni-2 is now four releases behind** and cannot be reached — see the P0 item
above. It is the only node where `/api/v1/status` is ungated and still carries
`diagnostics`, where `anonymous` can be given a password, and where
`/api/v1/health` answers 401.

**Live account roles, for anyone reading the gating.** The stored records are
`tom` and `root` (manage_users, manager, importer, media_viewer), `bryan`
(manager, importer, media_viewer) and `anonymous` (**no roles**, generation 2 —
the operator removed `media_viewer` on 2026-09-13 to make this a
registered-users-only deployment). None of them carries `view_status` on disk
and none needs to: 0.38.5 expands implications at mint, so the three human
accounts receive it on their next login and anonymous correctly does not. 0.38.2's torrent-bind fix is therefore live on two nodes: es-1
logs `torrent listen: advertised address 'ramaroja.macha.network' is not an IP
literal, binding all interfaces instead` and then binds 6881 on every
interface, which is the derivation working as intended.

**Torrent config applied by hand, now redundant.** gbni-1 and gbni-2 have
`torrent.listen_interfaces: 0.0.0.0:6881,[::]:6881` added directly to
`/etc/macha/macha.yaml`, with a timestamped backup beside it. 0.38.2 fixed the
derivation, so on gbni-1 the line no longer does anything; it is harmless to
leave and harmless to remove. es-1 never had it and does not need it.

**Build once, ship the artefacts.** All three nodes are aarch64 Debian 13 with
the same glibc. A `-j2` build on gbni-1 takes ~25 minutes; staging with
`DESTDIR` and shipping a 3 MB tarball takes about a minute. gbni-2 has 16 GB
RAM and builds at `-j4`. See `project-cluster-deployment` in session memory.

**Run the test suite on a node, not only locally.** A clean macOS/clang build
is not evidence: 0.38.0 shipped two defects that only GCC caught (a missing
`<functional>` include, and `-Werror=missing-field-initializers` on designated
initialisers). And do not run the suite concurrently with itself on a four-core
node — it produces failures that vanish in isolation and wastes the signal.

**Repository.** `main` is pushed and carries 0.38.1; 105 tags are pushed, the
newest being 0.38.2. Work since is on a local branch `work-0.38.2`, unpushed:
0.38.2 and the torrent bind fix, then 0.38.3 (pack recovery), 0.38.4 (anonymous
account), 0.38.5 (`view_status` and `/api/v1/health`), 0.39.0 (a line under the
0.38 series), 0.39.1 (the status/diagnostics split) and 0.40.0. Nothing since
0.38.2 is tagged. Everything through 0.39.1 is deployed and running on gbni-1
and es-1. **Pushing and tagging are the operator's call and have not been
done.** `CLAUDE.md` in the repo root is the operator's and is deliberately
untracked.

**Commit messages carry no attribution trailers**, by standing instruction. All
256 commits on every ref were scanned on 2026-09-13 for `Co-Authored-By`,
`Claude-Session`, `Generated with`, `anthropic` and `claude.ai` — in messages,
in parsed trailers, in history diffs, in both branch tips and in all 105 tag
messages. Zero found; nothing was rewritten. The only matches were a legitimate
`.gitignore` commit for Claude Code's machine-local settings and a dated UAT
log describing that session's own rules.

**The branching convention is confirmed (operator, 2026-09-13).** Work on a
long-lived `develop`, releases tagged on `main`, bare semver, **annotated**
tags, never name a branch after a version, version bump inside the release
commit — and no branches other than those two. The commands to convert were
written out for the operator rather than run: **pushing, tagging and branch
deletion are his alone, without exception.** The 105 existing tags are
lightweight; converting them would mean force-pushing all 105, so new tags are
annotated and history is left alone unless he says otherwise.

## Deployment rule

- [ ] For every deployment, synchronise the complete source tree and all CMake
  inputs, configure after synchronisation, build nodes in parallel, and verify
  installed versions and byte-identical hashes on identical RPi hardware.

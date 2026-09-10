# Active tasks and concepts to explore

Last updated: 2026-09-10

This is the authoritative, ordered backlog. Detailed plans and UAT records in
this directory remain evidence; completed work belongs in `COMPLETED.md` and is
not repeated here. Work top-to-bottom unless new evidence changes the order.

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

## P0 — Playback correctness and poor-network resilience

- [ ] **An abandoned playback session holds a node's only transcode slot for
  30 minutes — found 2026-09-10, confirmed independently by two client teams.**
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
  **Still open, and unowned:** the native paths ship unmeasured. Nobody has
  confirmed on a device that a cold session receives a 500 rather than aborting
  first, or what a player does with a burst of refusals against a per-session
  cap of 2 while prefetching a 20 s forward buffer. iOS's time-to-first-byte
  deadline has never been read at all. Both the cap and the timeout are server
  config, so acting on real numbers stays cheap when a device is available.

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
- [ ] **2. Split lightweight status from expensive diagnostics.**
  `ClusterStatusService::status_response` (`src/status_api.cpp`) still
  unconditionally computes and includes the full `diagnostics` object on
  every ordinary `/api/v1/status` call — no opt-in/expensive split yet.
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
- [ ] **FUSE-mounted reads never register as viewer demand.**
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
- [ ] **Two load-dependent test flakes needing a real fix, not another
  isolation-pass shrug — second one found 2026-09-08.**
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
- [ ] **No authorization tiers yet, though the seam now exists.** 0.24.0 gave
  every session a `roles` list and a `session_has_role()` helper, but nothing
  is wired to check it: one bearer token still grants catalogue reads,
  media-file deletion, namespace deletion, and cluster identity-association
  reset (which can be wildcard: `port: 0` matches every port, an omitted
  `node_id` matches any stale identity) — `manage_api.cpp` itself advertises
  `"privileged": false` in its own discovery document while gating deletion
  behind the same token as read access. Add at least a read-only vs.
  destructive role split and gate the destructive `manage_api.cpp`/
  `catalogue_api.cpp` routes on it.
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

- [ ] **`WriteHandle::drain_one_extent` waits on its extent future with no
  deadline and no cancellation check** (`filesystem.cpp:515`), so a stalled
  extent put blocks publication silently — the same "never fails, never
  completes" shape one layer below the 0.36.8 fix. Deferred from that work
  because it needs a `DistributedStore` fault-injection hook, which does not
  exist; landing the wait change untested would add a new `EAGAIN` path to
  publication on the strength of inspection alone.

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
- [ ] Diagnose faulty torrent/ingest independently so it does not obscure
  convergence and runtime measurements.
- [ ] Diagnose `ingest failed: metadata acceptance certificate durability
  floor unavailable` failures on torrent ingest once the torrent has
  downloaded, which are also unaccountably slow.
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
- [ ] Make signed artwork capability URLs actually cacheable. `exp`/`sig` are
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
- [ ] **`MANIFEST.sha256` is stale** — 104 hashes fail `shasum -c` as of
  2026-09-08 (90 when this was filed on 2026-09-05).
  Nothing in the build references it, so it currently just misinforms anyone
  who checks it. Either regenerate it as part of the release process or
  remove it.

## P2 — Documentation hygiene (found 2026-09-05, backlog-adjacent but not code)

- [ ] **Swagger/OpenAPI description of the HTTP API (operator request
  2026-09-07, optional).** Publish an OpenAPI 3 document for `/api/v1/*`
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

## Deployment rule

- [ ] For every deployment, synchronise the complete source tree and all CMake
  inputs, configure after synchronisation, build nodes in parallel, and verify
  installed versions and byte-identical hashes on identical RPi hardware.

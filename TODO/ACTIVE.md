# Active tasks and concepts to explore

Last updated: 2026-09-13

This is the authoritative, ordered backlog. Detailed plans and UAT records in
this directory remain evidence; completed work belongs in `COMPLETED.md` and is
not repeated here. Work top-to-bottom unless new evidence changes the order.

**Start here if you are new to this work.** Read, in order:

1. The **P0 cluster section** immediately below. The cluster is two nodes now,
   both on 0.40.1, and the third was removed rather than repaired — which the
   system does not really support, and that is the first item.
2. **"What the client sessions now depend on"** near the end of this file.
   These are API contracts settled in conversation with the four client
   sessions and they exist nowhere else in this repository. Breaking one breaks
   clients that cannot be fixed from here.
3. **"Cluster and repository state as of 2026-09-13"**, which records node
   addresses, what is deployed, what access works, and where the branches and
   tags stand.

None of the three is a task list; all three will mislead you if you assume
otherwise.

**The two things most worth picking up next**, if nothing else has changed:

- **`hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
  fails 40-80% of the time in isolation on both Pis**, and every failure shows
  one concurrent import failing `exists` before the case times out. That looks
  like a real race in concurrent ingest rather than a harness deadline, it is
  reproducible on hardware that is sitting there, and "passes in isolation" has
  been the accepted verdict five times without anyone measuring it. It is the
  cheapest real bug on this list.
- **`rpc_cluster/test_concurrent_reads_during_divergence_produce_one_reconciliation`
  fails 40% of the time on both Pis, in isolation**, at a metadata-divergence
  assertion — either a racing test or a genuinely dropped head, and both
  answers are worth having.

**OpenAPI is the largest piece of agreed but unstarted work** (P2 documentation
hygiene): the operator asked for it on 2026-09-07 and upgraded it to "soon" on
2026-09-13. Generate it from the route table at build time so it cannot drift.

2026-09-13 (evening): rationalisation pass for a new session. Twenty-three
completed items were moved to `COMPLETED.md` in full rather than summarised —
in several cases the reasoning *is* the record: a retraction, an option the
operator declined, a measurement that disproved the thing it was taken to
support. Before moving them, open remainders buried inside completed items were
promoted to entries of their own rather than carried along or lost: the
unmeasured iOS half of the bounded-VOD work, torrent session health not
reaching the HTTP API, the TSan run nobody has made, and the web client holding
a bearer token in JS-reachable storage. One completed item was kept in place as
a numbered stub so an ordered list still reads. Nothing was deleted.

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

## P0 — Nodes that cannot accept inbound connections, and edge nodes (business, opened 2026-09-15)

A node behind CGNAT or a non-forwardable NAT must be a full participant
(mount, playback, ingest) while never being connected *to*; and a node that
serves the API and media to its own network, caching what it plays, must be
able to store no media at all. Plan:
[inbound-incapable nodes](2026-09-15-inbound-incapable-nodes-plan.md) --
`network.inbound_capable` and `storage.hosts_extents`, both self-declared,
gossiped and `auto` by default; reverse-dialled lanes over the existing
inbound-route machinery; no relay (extents live only where they can be
fetched from).

**Stages A, B and C shipped in 0.42.0 (2026-09-15), protocol 21.** What is
left: (1) the cluster UAT on fi-1 described in the plan (point Macha at the
peers' public endpoints so its RPC port is genuinely unreachable; expect
`auto` to resolve false within two probe rounds, mount/playback/ingest to
work, no refused dials to it on gbni-1/es-1, `all_known_reachable` to hold
everywhere, and a 20-minute idle DATA lane not to stall); (2) the open
retention question -- the drain test uses unretained objects, so whether a
draining node's retention claims release without special treatment is still
unproven; exercise a FUSE publication from a node that then stops hosting.

## P0 — The test suite must be deterministic (next, opened 2026-09-14)

**"Known flake" is not a category. It is the name we have been giving the
decision not to diagnose a failure.** Plan:
[the test suite must be deterministic](2026-09-14-test-suite-must-be-deterministic-plan.md).

Six full-suite runs on one laptop during the 0.41.0 work produced seven
failures across six different cases, every one passing in isolation. One of
them was not a flake at all: it was a real regression introduced that
afternoon (0.41.0 put a second plugin in the build's shared plugin directory,
and that test points two Services at it), and it had already been waved past
once in the same session as "the known flakes". That is the cost, concretely.

**First pass done 2026-09-15 (laptop only).** Measurement tooling landed
(`--repeat`, `MACHA_TEST_LOG_LEVEL`, per-run port salt, `TempDir` starts
empty), seven cases classified and fixed -- four test defects, two
infrastructure defects, one product defect (`RpcServer::stop` executed
queued requests during shutdown). Details, rates and what is still
unproven are in the plan. The Pi-only cases remain.

Done means: the full suite passes 20 consecutive times on gbni-1 and es-1 at
CI's real parallelism, no case is documented anywhere as expected to fail
sometimes, and a red run therefore blocks a deploy. The two cases with
measured rates on real hardware are the cheapest place to start; the plan says
which and why. The "three load-dependent test flakes" item below is folded
into this and should be deleted, not re-worded, when its cases are classified.

## P0 — Cluster: two nodes, and what removing the third left behind

The cluster is **two nodes** as of the evening of 2026-09-13: gbni-1 (macnessa)
and es-1 (ramaroja), both on 0.40.1. gbni-2 (inverbeg) was removed by the
operator after its sshd stopped accepting key authentication and it could not
be deployed to. The 2026-09-13 storage incident that opened the previous
version of this file is resolved and ledgered in `COMPLETED.md`.

- [ ] **Removing a node is not a concept this system has, and the tombstone
  that removed gbni-2 is a freshness boundary rather than an eviction.**
  `Membership::observe()` says so explicitly: stale gossip cannot recreate the
  association, but "a directly authenticated peer may establish it again". So
  if that machine is ever reachable again and completes a handshake it rejoins,
  and once re-observed with a fresh `seen_unix_ms` ordinary gossip carries it
  back to the other node. Nobody will have done anything; the cluster will
  simply be three again. A real decommission needs three things that do not
  exist: a durable retired state that survives the node returning, exclusion
  from placement, and re-replication of what it held before it goes.
- [ ] **Nothing knows whether removing gbni-2 cost any extents.** `replicas: 2`
  across three nodes means every object that reached its target still has a
  copy, so the expected state is under-replicated rather than unavailable — but
  `min_write_replicas: 1` permits a write to commit with a single copy, and if
  that copy was gbni-2 the extent is gone. Nothing records which objects only
  ever had one replica. 0.40.1 added `diagnostics.repair` so a node now reports
  what *it* cannot source, which is the first half of an answer; the other half
  is the cluster-wide join nobody has built (see P2 diagnostics). Stored bytes
  at removal: gbni-1 737 GB, es-1 1.82 TB.
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

- [ ] **The bounded-VOD hold is unmeasured on iOS, and its ceiling is now
  known on Android — device evidence 2026-09-13, iOS still unowned.** The
  complete VOD playlist with bounded segment holds shipped in 0.35 and is
  ledgered; what was never measured was what a real player does with a refusal.
  The phone session measured Android and reported:
  - **media3 does not retry a 500 on the HLS path.** It surfaces immediately as
    a fatal `Source error`. So the server's hold is the entire retry budget in
    the system — there is nothing behind it. That should govern how generous
    the hold is.
  - **The tightest deadline on that client is 8000 ms**, on the music path
    (`DefaultHttpDataSource` defaults); the video path allows 10000 ms
    (`OkHttpDataSource` with no timeouts set). Against a 6000 ms hold that is
    2 s of margin. **Raising `segment_timeout` above 8000 ms starts breaking
    the music path**, so treat 8000 ms as a hard ceiling on that knob.
  - **HTTP status never reaches that client's JavaScript** (`PlayerError` is
    `{message: string}`), so `segment_not_ready` and a dead stream are
    indistinguishable there. The 500-over-503 choice is sound for players that
    read status and buys that client nothing.
  - **iOS has never been measured by anyone** and will not be by that session,
    which has only ever run on Android. AVPlayer's time-to-first-byte deadline
    is unknown. Nobody should plan around that number arriving.
  Both the hold and the per-session cap are server config, so acting on real
  numbers stays cheap if an iOS device appears.

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

- [x] **gbni-2 serves reads at roughly a sixth of gbni-1 — MOOT since
  2026-09-13: that node is no longer in the cluster.** Kept here rather than
  ledgered because the lesson outlives the node and will apply to the next slow
  one: a node can be healthy by every status field the cluster reports and
  still be unable to serve playback, because nothing measures per-node read
  throughput and `cpu_cores` cannot express it. If gbni-2 ever rejoins, this
  becomes live again and undiagnosed. The measurement: Client-measured raw read rate with no
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
- [x] **2. Split lightweight status from expensive diagnostics — shipped
  in 0.39.1, deployed 2026-09-13. Ledgered in `COMPLETED.md`.** Kept as a
  numbered stub so the ordering of this list still reads. The question that
  prompted it is a separate open item: see "Status took 10 s" under P1.

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
  **Sized and re-planned 2026-09-14** as two stages in
  [FUSE behind the subsystem supervisor, then out into a plugin](2026-09-14-fuse-supervised-subsystem-plan.md):
  Stage A supervises FUSE in place (closes this P0), Stage B moves libfuse
  into `libmacha-fuse`. The plugin boundary is `fuse_adapter.cpp`, not
  `FuseFrontend`, so the FUSE tests keep linking `macha_core`.
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
- [ ] **`hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
  fails 40-80% of the time IN ISOLATION on both Pis — characterised 2026-09-13
  during the 0.40.0 rollout. "Passes in isolation" is false, and has been the
  accepted verdict five times.** This is the measurement the item below asked
  for instead of a sixth sighting, so act on it rather than re-observing it.
  Measured with `--serial --filter`, nothing running but the live node:
  **gbni-1 on 0.40.0, 8 failures in 10; es-1 on 0.39.1, 4 failures in 10.**
  It is therefore **not a 0.40.0 regression** — the unmodified 0.39.1 build on
  es-1 reproduces it, and nothing in 0.40.0 touches ingest. Do not read the
  80%-vs-40% difference as a version effect: the nodes differ in load, disk and
  network, and no controlled comparison was run.
  Every failure is the same shape: the case times out at its full 60 s having
  logged `ingest started workers=4`, one job failing `ingest failed id=…:
  exists`, and two of four copying successfully. An `exists` failure on a
  concurrent import looks like a race between workers over a destination path —
  `ingest.max_concurrent_jobs` and its claimed-set ownership shipped together in
  0.37.0 — and that, not the harness deadline, is the first thing to read.
  One hypothesis was tested and **refuted**: the failing runs also logged
  `subsystem plugin 'libmacha-torrent' build identity mismatch: plugin=0.39.1
  core=0.40.0; refusing to load`, because a freshly built test binary was
  loading the older installed plugin. Installing 0.40.0 so the two matched
  changed nothing — 8 in 10 before, 8 in 10 after. The mismatch is a real
  artefact of building on a node mid-deploy, but it is not this.
  A 40-80% reproduction rate on hardware that is sitting there makes this cheap
  to root-cause, and it is the only one of these flakes that does not need load
  to reproduce.

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
- [ ] **The web client holds a bearer token in JS-reachable storage, and only
  the server can fix it.** Anything in `localStorage`/`sessionStorage` is
  XSS-readable. The node already serves the web client, so a `Secure`,
  `httpOnly`, `SameSite` cookie set on a successful `POST /api/v1/session` and
  accepted alongside the `Authorization` header is same-origin and natural.
  Raised 2026-09-13 alongside the session-TTL question; the operator answered
  the TTL one (30 days stands) and this was deliberately **not** closed with it
  — it is a different question and needs its own decision. Native clients are
  the opposite case: they should hold the token, but in Keychain/Keystore
  rather than plaintext `AsyncStorage`, which is theirs to fix.

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

- [ ] **Spool usage is not in polled Status (requested 2026-09-14).** The
  numbers exist — `FuseFrontendDiagnostics` carries `spool_bytes`,
  `spool_limit_bytes`, `spool_publish_rate_bytes_per_second`,
  `spool_throttle_waits` and `spool_throttle_wait_ms` — but 0.39.1 moved the
  whole `diagnostics` block off `/api/v1/status` onto its own endpoint, so
  nothing a client polls reports how full the spool is. That is the wrong
  side of the split for this particular number: local write-back admission is
  paced against it, a spool at its ceiling is what a wedged publication
  pipeline looks like from outside (es-1, 2026-09-09), and it is one atomic
  read with no locks and no namespace walk.
  Put the small always-true summary — used, limit, and the publish rate —
  in the cheap always-present part of the status response, next to the
  `subsystems` block, which is there for exactly this reason (0.25.0: "it
  costs nothing to compute and is exactly what an operator needs promptly").
  Leave the per-counter detail in `diagnostics`. Read it through
  `SubsystemRegistry::fuse()` like the other FUSE-facing status does since
  0.41.0, so a node with no mount simply omits it rather than reporting
  zeroes that look like an idle spool.

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
- [ ] **Torrent session health does not reach the HTTP API.** 0.37.2 fixed the
  bind defect and made libtorrent alerts visible in the journal, but
  `torrents/status` still says nothing about listen endpoints or DHT, and
  `TorrentJob` carries `peers`/`seeds` as bare counts with a free-text `error` —
  so a client cannot tell a dead session from a slow swarm. The macha-client
  team asked for exactly that on 2026-09-10: session health with structured
  warning codes; per-job trackers, stall durations, connected-vs-candidate
  peers, structured errors. It needs new `TorrentJob` fields, a persistence-shape
  change and the cluster RPC bridge to carry them, so it is real work rather
  than serialisation.

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
- [ ] **No TSan run has ever been made against the concurrency-heavy
  subsystems** (`net.cpp`, `metadata.cpp`, `playback.cpp`). The sanitizer build
  shipped 2026-09-08 (`MACHA_SANITIZE`, whole-tree, with deadlines auto-scaled
  10x under TSan), so this is now cheap — it is simply that nobody has run it.
  The ~47-thread hand-reasoned lock ordering that the 2026-09-05 audit called
  its biggest process gap remains unexercised by a sanitizer. ASan+UBSan across
  the full suite was green as of 2026-09-08. Note CI was **declined** by the
  operator, not deferred: every regression gate is a human running
  `./run-tests.sh`, and nothing runs TSan periodically unless someone does.

## P1 — Scaling cliffs (found 2026-09-05, not yet urgent at current 2-node/home scale)

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
- [ ] **Tracker list is stale.** 111 tracker errors in five minutes were
  measured on gbni-2 before it left the cluster; `coppersurfer.tk` and others
  have been dead for years, and the list is shared, so the surviving nodes carry
  it too. DHT carries the torrents, so this is noise rather than breakage — but
  it buries real tracker failures, which is what makes it worth a few minutes.

- [ ] **Test-only hooks are live branches in the production hot publication
  loop.** `suspend_loader_for_tests` and `fail_publication_once_after_spool_bytes_for_tests`
  (`config.hpp`) are real conditionals compiled into the shipped binary, not
  behind a test-only build flag. Low risk today, but worth gating out of
  release builds since they're reachable via ordinary config.
## P2 — Documentation hygiene (found 2026-09-05, backlog-adjacent but not code)

- [ ] **Swagger/OpenAPI description of the HTTP API — WANTED, and soon
  (operator, 2026-09-13, upgrading the 2026-09-07 "optional").** This is now
  the largest piece of agreed but unstarted work in this file, and it should be
  generated from the route table at build time rather than written by hand, so
  that it cannot drift from `service.cpp`. Four client sessions currently learn
  the API by reading `status_api.cpp`/`service.cpp`, which is how two of them
  ended up holding private copies of a wire format. Publish an OpenAPI 3
  document for `/api/v1/*` — session, health, status, catalogue, playback,
  manage, users, ingest — and serve it from the daemon at
  `/api/v1/openapi.json`, with a Swagger UI page. **Generate it from the route
  table at build time**: a hand-written document drifts, and drift here is
  exactly the failure it exists to prevent. Note the route table is also where
  the role gating lives (`service.cpp:199-216`), so the document can state
  which role each route requires rather than leaving clients to discover it
  with a 403.

- [ ] `TODO/2026-09-03-playback-resilience-and-av-sync-plan.md` cites
  `TODO/2026-08-31-cluster-any-node-playback-failover.md` as tracking a
  client-side fix — that file does not exist anywhere in the repo. Either
  create it with the actual current tracking location or fix the reference.
- [ ] `TODO/COMPLETED.md` is ledgered in patches rather than continuously.
  The 2026-09-08 pruning pass and the 2026-09-13 rationalisation pass each
  moved their own entries across in full, but the 0.24.1–0.35.0 range is still
  only partly represented — the self-healing programme (0.29.0–0.32.0), the
  subsystem-plugin foundation (0.25.0/0.28.0), the playback/streaming work
  (0.32.x–0.34.0) and SPA serving (0.35.0) are recorded in `CHANGELOG.md` but
  not in the ledger. Lower value than it looks: `CHANGELOG.md` covers that
  range properly, so this is tidiness, not lost information.
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

## What the client sessions now depend on (settled 2026-09-13)

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
- **`GET /api/v1/users` answers under `items`**, like every other collection,
  from 0.40.0. Single records from `POST`/`PATCH` stay bare. Core has accepted
  either key since its 0.8.0, so no client needed a release.
- **A signed artwork URL is stable for up to 24 hours and valid for 24-48.**
  From 0.40.0 `exp` is quantized to a bucket of the TTL, rounded up to the
  bucket *after* next: `(now / ttl + 2) * ttl`. The invariant is "always between
  one and two TTLs", not "between 24 and 48 hours" — the bucket *is* the TTL, so
  reconfiguring `artwork_capability_ttl` moves the bound with it. With the
  default 24 h TTL this lands on a UTC midnight, because the bucket is a
  multiple of 86,400,000 ms from the epoch and the epoch is itself a UTC
  midnight. The minimum remaining validity is 86,400,001 ms, one millisecond
  *over* `max-age=86400`, so a cached copy can never outlive the signature that
  names it. Measured live: 418 refs, 418 identical URLs.
  The artwork `id` has always been the SHA-256 of the bytes and is the stable
  identity clients should key an image cache on; the signed URL is transport.
  The route is bearer-exempt with a valid signature, so a payload's `url` works
  in a bare `<img src>` with no headers.
- **Session lifetime is 30 days from mint, and that is decided** (operator,
  2026-09-13): counted from creation rather than last use, so a session expires
  even under daily use. No sliding expiry, no refresh tokens. Do not re-raise it
  as a defect.
- **`/api/v1/status/diagnostics` gained a `repair` block** in 0.40.1
  (`unsourceable_objects`, `unsourceable_sample`, `local_unreadable_objects`).
  No client types it today.
- **Record which host served a measurement before reporting it.** Two separate
  client reports today were gbni-2 attributed to gbni-1, and the retracted
  DTS/TrueHD investigation in September was the same mistake at larger scale.
  The node is in the URL; there is no excuse for losing it.

## Cluster and repository state as of 2026-09-13 (end of session)

Facts a new session needs before touching anything. None of this is a task.

**Nodes — there are two.** gbni-1 is `10.44.1.50` / `macnessa.macha.network`
(failure domain `test-lab`), es-1 is `10.34.1.50` / `ramaroja.macha.network`
(`spain`). Both advertise public `https://` API endpoints. SSH as `root@`, not
`tom@`. SSH to both is filtered from outside the LAN/VPN.

**gbni-2 (inverbeg) was removed from the cluster**, not merely unreachable.
Verified in both nodes' own `known-nodes.bin`: each lists one known node — the
other — plus a tombstone for `[inverbeg.macha.network]:7437`,
`stale=68836f3e0a8b…`, epoch 1, at 18:47:42Z, propagated to both. The 7-second
dial loop is gone. It was removed because its sshd began offering password
authentication only (`Authentications that can continue: password`) with the
host key unchanged — the same machine, reconfigured — so it could not be
deployed to and was four releases behind. **See the first P0 item: this is a
freshness boundary, not a decommission, and the node rejoins if it ever
completes a handshake again.**

**Versions deployed (2026-09-13, evening).** Both nodes run **0.40.1**
(`libmacha_core.so` sha256 493f5076…, GCC 14.2.0, built on gbni-1 at `-j2` and
shipped to es-1 as a 3 MB tarball; both verified byte-identical). Both answer
`/api/v1/health` 200 `{"status":"ok"}` unauthenticated, both loaded
`libmacha-torrent`, and neither logged an ERROR after restart. Neither had a
live viewer at restart time; both journals were checked first.

**The repository and the cluster agree at 0.40.1.** `main` and `develop` are
both at `fbe928e`, both pushed, and `0.40.0` and `0.40.1` are pushed as
**annotated** tags. The 105 older tags are lightweight and were left alone;
converting them would mean force-pushing all of them.

**Branching convention (confirmed by the operator 2026-09-13).** Work on the
long-lived `develop`; releases are tagged on `main`; bare semver; annotated
tags; never name a branch after a version; the version bump goes inside the
release commit. `work-0.38.2` was deleted when this was adopted. Only `main`
and `develop` exist.

**Pushing is allowed when asked**, and only when asked — the operator said
"Yes, pushes are ok" on 2026-09-13 after an earlier "you must never push".
Never push unprompted; never open a PR unless told.

**Live account roles.** `tom` and `root` (manage_users, manager, importer,
media_viewer), `bryan` (manager, importer, media_viewer), `anonymous` (**no
roles**, generation 2). None carries `view_status` on disk and none needs to:
0.38.5 expands role implications at mint, so the three human accounts receive
it on their next login and anonymous correctly does not. **The roles-less
anonymous account is a deliberate experiment**, not an oversight — the operator
is exercising how the system behaves as a registered-users-only deployment. It
means the manage and status APIs refuse an anonymous session, which has already
cost one diagnosis; if something "does not work" from a client, check the role
before reading code.

**The HTTP layer logs no requests at all.** There is no access log and no
slow-request log, so you cannot tell from a node whether a request even
arrived, or what status it received. That has now cost two diagnoses (the
10-second Status investigation and the identity-reset one). It is cheap to add
and it is in P1.

**Build once, ship the artefacts.** Both nodes are aarch64 Debian 13 on glibc
2.41. A `-j2` build on gbni-1 takes ~25 minutes; staging with `DESTDIR` and
shipping a 3 MB tarball takes about a minute. Sync with plain `rsync` and
**never `--delete`**. See `project-cluster-deployment` in session memory.

**Run the test suite on a node, not only locally.** A clean macOS/clang build
is not evidence: 0.38.0 shipped two defects only GCC caught. Expect
`hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
to fail there — it is a known pre-existing flake, characterised on 2026-09-13
at 40-80% in isolation on both Pis and reproducible on 0.39.1, and it is near
the top of this backlog.

**Commit messages carry no attribution trailers**, by standing instruction. All
refs were scanned on 2026-09-13 and are clean. `CLAUDE.md` in the repo root is
the operator's and is deliberately untracked.

## Deployment rule

- [ ] For every deployment, synchronise the complete source tree and all CMake
  inputs, configure after synchronisation, build nodes in parallel, and verify
  installed versions and byte-identical hashes on identical RPi hardware.

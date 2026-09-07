# Self-healing programme — progress and resume file

This file is the resume point for any session (human or scheduled) working
the programme in `2026-09-06-self-healing-disciplines-plan.md`. Keep it
current at every checkpoint; a fresh session reads only this and the plan.

## Standing rules (from the operator, 2026-09-06)

- Load generation is **rsync only**, from `/mnt/diskA` on gbni-1
  (10.44.1.50) and es-1 (10.34.1.50) into the node's own FUSE mount
  `/mnt/machamedia`. `/mnt/diskA` **MUST NOT be modified** — read side only,
  never a destination, never a cleanup target. **Never use the ingest
  subsystem against `/mnt/diskA`** (untested; unknown behaviour).
- es-1 is shared with other users: never kill a process we did not start;
  run our rsync/build work under `nice -n 10 ionice -c3` there; keep builds
  at `-j2`.
- Commits allowed on `claude-work`, never push. Attribution trailer per the
  session's current instruction. Commit at every phase boundary and at any
  point where >1 h of work would otherwise be uncommitted.
- Cluster may be restarted, chaos-restarted, and **wiped from scratch** when
  state gets in the way. Wipe scope on all three nodes: `/etc/macha/state`,
  the FUSE journal (`/etc/macha/fuse-operations.log`), `/mnt/diskB/spool`,
  and the object stores under `/mnt/diskB`. Keep `/etc/macha/macha.yaml`,
  `cluster.key`, and any `*.bak-*` files.
- Frontend work goes to the separate agent **"Macha UI Work"** (SendMessage);
  brief it with the same rules and the API change in question.
- After each phase: checkpoint here + memory + commit, then tell the
  operator it is safe to `/compact`.
- Deployment shape: rsync tree (no `--delete`), `cmake --build build -j2`,
  `make -C build install`, `systemctl daemon-reload`, `systemctl restart
  macha.service`, one node at a time, health-check between. See memory
  `project-cluster-deployment` and `project-onbox-diagnostics`.

## Node facts

| node | ip | role | notes |
|---|---|---|---|
| corvus-gbni-1 | 10.44.1.50 | writer, 4 GB, **undervolt-prone** | `/mnt/diskA` source; FUSE journal backup `fuse-operations.log.bak-20260906-dup`. Operator 2026-09-07: it shuts down from undervoltage under heavy load (went dark 04:13 a minute after restart + uncapped rsync). Policy: never build on it (build on gbni-2, ship `make install DESTDIR=` stage), rsync with `--bwlimit=8000` under nice/ionice, don't stack replay + build + import. |
| corvus-gbni-2 | 10.44.1.51 | replica, 16 GB | no diskA |
| corvus-es-1 | 10.34.1.50 | writer, 8 GB, offsite (CEST), shared with other users | `/mnt/diskA` source; WAN ≈ 50–60 Mbps via WireGuard/EC2 |

## Phase status

- [x] Baseline: 0.28.3 on all nodes; plan committed (`273a5c4`).
- [x] **Discipline 1 — re-derive, don't assert (durability probe).** DONE:
  0.29.0 on all nodes (core `0888fe4ffff8`), UAT runs 1+2 recorded in
  `2026-09-06-self-healing-uat.md` (pass). Loose end noted there: one
  `transient=yes outcome="remote-reasserted"` line per run — a re-stamped
  requirement counted as not-yet-durable once; harmless, look at the
  `durable[key]` bookkeeping when touching the barrier next.
- [x] **Discipline 2 — one work-item retry policy + no-progress startup gate.** DONE: 0.30.0 on all nodes (`bb3697c`), UAT 2a/2b/2c recorded in `2026-09-06-self-healing-uat.md` (pass: 254 retries at 10/s → 16 retries then park at 22.8 s; operator retry drains; RPC deadline fires at 30 s; startup ceiling removed). gbni-1 config back on shipped defaults.
- [x] **Discipline 3 — resolve-on-recovery + journal fuzz fixture.** DONE: 0.31.0 on all nodes (`21ebe79`), UAT recorded (pass: gbni-1 23→5→0 boot WARNs, journal 132 MB→8 B, spool 183 MB→0; es-1 22,997 pending ops / 6 GB / 218 MB journal resolved in one boot, second boot silent).
- [x] **Discipline 4 — compact history out of the hot path (+ DLT7).** DONE: 0.32.0 on all nodes (`27c1903`, `96c3413`); measured-first scope (no retirement log); conflicts 116 → 9, merge delta 335,961 B → 277 B, no full-frame reconciliations.
- [x] **UAT record complete**: `2026-09-06-self-healing-uat.md`, all four disciplines + the closing demonstrative run (2026-09-07 00:08–00:22: two concurrent writers, five rolling restarts incl. a writer, 0 ERROR, 1 WARN, 0 wedges, 277-byte merge delta). **PROGRAMME COMPLETE.**

## Import iteration log (newest first)

- **Schema-2 profile build DEPLOYED: gbni-1 17:44, gbni-2 17:54, es-1
  18:49 CEST (commit `cce1c9c`).** With the TV's capability list
  (bit_depth 10, hdr none) the gate now fires: `video=transcode/h264
  audio=transcode/aac`, CODECS `avc1.640029,mp4a.40.2`. Operator report
  via UI session on the earlier hotfix build: direct play of Ratatouille
  now has sound (CODECS fix proven on hardware), transcode still silent
  through Tizen's *native* HLS player (`forceNativeHls`), master playlist
  correct → leading hypothesis: Tizen 3 native player handles fMP4 HLS
  partially. No TS fallback exists (`hls_fmp4=false` is refused for
  non-direct). Next viewer items: (a) external fMP4-vs-TS stream test on
  the TV, (b) MPEG-TS HLS output behind an `hls_ts` capability if (a)
  confirms, (c) Dolby Vision profile gate (`dolby_vision` = list of
  profiles; profile 8.1 = HDR10).

- **Hotfixed 0.32.12 on all three (gbni-2 17:24, es-1 18:25 CEST, gbni-1
  17:28 — operator ordered the immediate restart).** Then the UI session's
  one-call check with the TV's exact list showed the gate cannot fire:
  `source.video.bit_depth/color_transfer` were null because the session's
  stream info comes from the **stored immutable media profile** (schema 1,
  no level/transfer, zero depth). Fix (in test → deploy next): profile
  schema 2 carries `level` + `color_transfer`; a schema-1 profile with a
  video stream is invalid → regenerated on next playback and republished;
  API reports schema 2. Until deployed the TV gets sound (CODECS) but the
  copied DV stream (broken picture).

- **0.32.12 rollout, twice wrong before right:** (1) GCC
  `-Werror=format-truncation` on the hex level `snprintf` failed the node
  builds; the chain's `grep error` swallowed the exit status and
  reinstalled 0.32.11 on es-1 and gbni-2 (two pointless restarts). Chain
  now checks `rc`. (2) The real 0.32.12 (es-1 18:11 CEST, gbni-2 17:17)
  **broke session create for any client listing `eac3`** (503 `write
  fragmented MP4 header: Invalid argument` — copied E-AC-3 needs
  `delay_moov`); the TV lists eac3. gbni-1 (TV pinned to it, real viewer)
  stayed on 0.32.11 by the playback gate — which protected it. Hotfix:
  audio copy AAC-only, everything else kept. UI session confirmed on
  gbni-2 the master playlist `#EXT-X-STREAM-INF:BANDWIDTH=10887601,
  CODECS="hvc1.2.4.L153.B0,mp4a.40.2",RESOLUTION=1920x802` → media.m3u8,
  and isolated the 503 to the eac3 entry alone (same client minus eac3 →
  201). Deploy order: es-1, gbni-2 (broken now), then gbni-1 when quiet.

- **Samsung Tizen 3 TV capability set (operator read it off the new
  "Playback support" card):** video h264/hevc/vp9; audio
  aac/opus/vorbis/ac3/eac3/mp3; containers mp4/webm/mp3/ogg; hls_fmp4
  true; **video_bit_depth 10**; hdr empty (Chromium 47 has no
  dynamic-range media query, so it can never claim HDR). Consequence: on
  this set only the HDR half of the 0.32.12 gate catches Ratatouille (DV
  profile 8, PQ); the depth check contributes nothing. A future client
  change that trusts the panel for HDR would re-break it and look like a
  server regression. **Next viewer item:** Dolby Vision is not HDR10 —
  probe the DV configuration record (side data), expose
  `dolby_vision_profile`, gate profiles 5/7 (FEL) behind a
  `capabilities.dolby_vision` flag; profile 8.1 = HDR10-compatible.

- **0.32.11 DEPLOYED (es-1 17:37 CEST, gbni-1 ~16:40, gbni-2 16:43;
  gbni-1's first gate skip was my own transcode probe's log lines).**
  Probe (HEVC-capable client, Ratatouille): gbni-2 create 0.66 s / seek
  0.76 s / mode=transcode 1.9 s / 720p 3.2 s / seek-in-transcode 6.1 s /
  auto 4.0 s; es-1 1.2 / 1.6 / 2.2 / 3.1 / 3.9 / 4.4 s (UI baseline was
  2.6-4.4 create, 5.6-9.2 seek, 7.7-13 other). x264 line
  `encoder_threads=4 frame_threads=1`. The create is video-copy +
  audio-transcode (E-AC-3→AAC) so the session still says
  `mode=transcode`; 0.32.12 copies (E-)AC-3/Opus when listed → remux.
- **UI session's 0.32.11 table (client side, same title/caps as its
  baseline):** create 0.58-0.88 s (was 2.8-4.4), PATCH seek 0.79-1.84 s
  (was 5.6-13.0), forced transcode 2.67-3.68 s (was 7.6-13.0); output
  shows video hevc copy + audio aac transcode → label `transcode`.
- **0.32.12 (commit `63e13ce`, suite 360/360; deploying) — TV
  silent-audio + broken direct decode:** master
  playlist with `EXT-X-STREAM-INF` CODECS/RESOLUTION/BANDWIDTH →
  `media.m3u8`; caps `video_bit_depth` + `hdr` (transfer list or bool)
  gate 10-bit/PQ/HLG sources out of copy/direct in `auto`; probe records
  `level`/`color_transfer`; create log names video/audio transform+codec
  and client caps at debug; audio copy for aac/ac3/eac3/opus. UI session
  confirmed hls.js 1.6.18 fMP4 passthrough infers codecs from the init
  segment — the master playlist is the fix.

- **0.32.11 (commit `cefaa72`, suite 358/358) — viewer path.** (1)
  `media_vod::indexed_plan`: fragments up to 90 s allowed (was 3× target
  = 12 s, whole-file reject → every HEVC title transcoded); rejection log
  now `keyframes= longest_gap_s=`. (2) x264 frame-threaded on all cores
  (`streaming.video_encoder_threads`, 0 = all), `x264-params
  rc-lookahead=8:sync-lookahead=0:sliced-threads=0` instead of
  `tune=zerolatency`. (3) first transcode fragment 2 s
  (`kStartupFragmentSeconds`, both copies of `fixed_vod_durations`). (4)
  VOD plan cache key without seek; hit → `reseek_hls_vod`. Probe script
  `/root/uat/patchprobe.py` (HEVC-capable caps, create → seek → mode →
  quality → seek → auto) on gbni-2; baseline run hit 429 (someone else's
  transcode). Deploy: spaced + playback-gated; gbni-2 had a viewer at
  16:26 CEST. Verify: create returns `mode=remux` for the Ratatouille
  title, PATCH seek < 1 s, forced transcode PATCH ≈ half of 5-13 s,
  `libav interactive video codec … encoder_threads=4 frame_threads=1`.

- **WAN control-lane starvation: CLOSED (25-min soak 16:13-16:38 CEST,
  0.32.10 on all three, imports running).** Mutations ≥100 ms: gbni-1
  p50 337 / p90 391 / max 391 ms; es-1 n=474 p50 615 / p90 698 / max
  1,079 ms; gbni-2 p50 289 / max 647. Retention avg 28-51 ms. **Zero**
  `deadline exceeded` / `peer closed` lines. Was 195-284 s. Remaining
  floor is the commit fan-out over a 60 ms link (~3 sequential round
  trips ≈ 0.5-0.7 s) — pipelining store/history/accept is the next
  step if it ever matters. Operator moved on to item 5 (viewer path).
- **Viewer path (item 5) — measured first (17:00 CEST):** every VOD plan
  on all three nodes in 24 h was `mode=transcode`; remux was tried and
  rejected with `unusable-keyframe-index` on the two titles that carried
  HEVC to an HEVC-capable client (Ratatouille: 1,533 cue entries
  materialised, still rejected). The rule (`media_vod.cpp`
  `kMaximumSegmentFactor=3`): any keyframe gap > 12 s anywhere in the
  file rejects remux for the whole file. x265/x264 scene-cut encodes
  have such gaps routinely. No DV/HDR/HEVC exclusion exists. Also found:
  PATCH restarts the pipeline for every non-subtitle change (container
  opened up to 3×, plan cache key includes the seek), the response waits
  for a full 4 s segment to be *encoded*, and x264 runs `tune=zerolatency`
  (sliced threads) with `thread_count` unset.

- **0.32.10 DEPLOYED gbni-2 15:30, es-1 16:40 CEST; gbni-1 deferred by
  the playback gate (viewer active), retrying every 5 min.** es-1 on
  0.32.10, 7 min: 35 mutations, retention avg **121 ms** (was 5,217),
  max 2,674; no CONTROL claim line ≥ 250 ms any more. What remains: the
  DATA presence scan is still seconds *with the presence cache warm*
  (`ids=3522 scan_ms=2643` on es-1; `ids=3201 scan_ms=8024` on gbni-1
  at 0.32.9) → the per-id cost is not the stat; suspect
  `LocalStore::object_mutex()` sweeping expired weak_ptrs on every call
  (O(N) per call → O(N²) per batch). Next.

- **0.32.9 on es-1 too (16:20 CEST, after its viewer stopped).** es-1's
  first phase line: `total_ms=4571 decode_ms=8 collect_ms=1
  catalogue_ms=15 data_ms=0 control_ms=4546 control_objects=65` — the
  DATA claim is now 0 ms on both writers; all that is left of the barrier
  is the CONTROL claim of the catalogue graph.
- **0.32.10 (commit pending → see git log; suite 358/358):**
  `retain_control` stored 65 catalogue objects on each candidate one RPC
  at a time and took candidates in NodeId order → 65 × 65 ms across the
  WAN per catalogue mutation. Now local → measured-nearest order
  (`order_commit_replicas` moved to placement.hpp) and all puts to a
  candidate in flight together; `CONTROL retention claim objects= …
  total_ms=` logged when ≥ 250 ms. Deploy spaced + playback-gated; verify
  `control_ms` on both writers ≪ 1 s and mutate p90 well under 1 s.

- **0.32.8 verified (15:15 CEST):** gbni-1's barrier line named the
  phase: `DATA retention barrier ids=3201 nodes=1 total_ms=16317
  scan_ms=16048 short=0` then `total_ms=357 scan_ms=261` for the next
  quantum — a cold-dentry `stat` per extent (~5 ms on the import-saturated
  disk); the 4,096-entry verified-loose cache cannot hold a 16 GB file's
  extents. Both writers have `min_write_replicas: 1` (floor 1 → nodes=1;
  also why a writer's death strands data — finding #6 — and why a put is
  "replicated" only after repair). es-1's 13 s `retention_ms` came with
  93-byte deltas and *no* `DATA retention barrier` line → outside
  `retain_data` (decode of the 5 MB parent snapshot? catalogue root diff?
  CONTROL claim?). peer_latency_ms with ping-only sampling: gbni-1→gbni-2
  113 ms, es-1→gbni-2 159 ms, gbni-2→gbni-1 4 ms — the wifi asymmetry is
  real, not a metric artefact. gbni-1's 16 GB spool (one file recovered
  after restart) drained at ~8 MB/s; es-1's second pass over Movies is
  mostly dedupe (216k reused vs 658 put extents).
- **0.32.9 (commit `4e9f60b`, suite 358/358 after a rerun — the first
  run hit a Mac load average of 650 and failed 16 timing assertions):**
  `LocalStore::has()` remembers loose objects this process installed or
  stat'ed (unbounded set, forgotten on remove); the presence test now
  models the crash case via a reopened store. `retain_metadata_publication`
  logs `metadata retention barrier total_ms= decode_ms= collect_ms=
  catalogue_ms= data_ms= control_ms= data_objects= control_objects=
  outcome=` when ≥ 250 ms. **Deployed gbni-2 15:01, gbni-1 15:07; es-1
  deferred — the playback gate found a live viewer on it (25 playback
  lines in 3 min), so es-1 stays on 0.32.8 until playback stops.**
  Verification (gbni-1, 0.32.9): the phase line named the next culprit —
  `metadata retention barrier total_ms=4286 decode_ms=13 collect_ms=1
  catalogue_ms=25 data_ms=0 control_ms=4247 control_objects=65`: the
  CONTROL claim for catalogue objects. es-1 (still 0.32.8) shows the
  expected cold-stat scan (`ids=2659 scan_ms=3392`) and a 33 s max.

- **0.32.7 verified (14:40 CEST, 4 min of traffic):** gbni-2
  `retain_objects` handler 12,113 ms → **0 ms**; commit fan-out
  `publish_ms` ≤ 554 ms on all nodes. Remaining cost is the writer-side
  barrier: gbni-1 `retention_ms` ≈ 1,040-1,150 per quantum commit steady,
  6.5 s / 14.7 s in the first minute after restart; es-1 4.4 s. No
  control-lane deadline or peer-closed lines since the deploy.
  **Rolling-restart incident:** the three 0.32.7 restarts landed inside
  two minutes and killed the operator's live TV playback (UI session
  report). Policy now: one node at a time, ≥5 min apart, check
  `journalctl … | grep -c "playback\["` for the last 3 min first.
  **Topology (operator via UI session):** gbni-2 is on flaky wifi; gbni-1
  wired; es-1 remote. Latency is directional (toward gbni-2 slow, from it
  fast). Nearest = measured, never same-site.
- **0.32.8 DEPLOYED (commit `4c84932`), spaced rolling restart with a
  playback check before each: gbni-2 13:53, gbni-1 13:59 (staged
  binaries), es-1 15:05 CEST; no viewer active at any of them.** gbni-1's
  import took the mount blip as `rc=11` and re-ran Movies (attempt 2) —
  the retry rule works. Verification sweep: next entry.
- **0.32.8 (in test):** `retain_on` claims fan out concurrently; local
  presence checks drop the per-id 4 MB loader lease (thousands per
  quantum commit, queued behind the node's own publications); barrier
  logs `DATA retention barrier ids= nodes= total_ms= scan_ms= short=
  fallback_claims=` when ≥ 250 ms; `peer_latency_ms` samples heartbeat
  pings only (0.32.7 sampled every control call — payload/handler time
  made gbni-1 see gbni-2 at 114 ms vs 4 ms the other way). Recorded, not
  changed: capacity placement gives the writer no guaranteed local copy;
  whole-file re-claim per quantum grows the retention journal ~N² per
  file. Verify after deploy: gbni-1 retention_ms per quantum ≪ 1 s and
  the barrier line's phase split; peer_latency_ms toward gbni-2 from
  gbni-1 in the tens of ms.

- **0.32.7 DEPLOYED (commit `2b427ce`; gbni-2 13:27, gbni-1 13:31 via
  staged binaries, es-1 14:34 CEST).** Both imports relaunched with the
  retrying import script (a non-zero rsync exit now re-waits for the mount
  and reruns the directory, up to 20 times; both nodes had skipped from
  Movies to TV when the 0.32.7 restart took the mount away). gbni-1 pid
  3393 (bwlimit 4000), es-1 pid 166386 (bwlimit 8000). First
  `peer_latency_ms` from gbni-2 before the relaunch: es-1 81 ms, gbni-1
  4 ms. Verification of the retention/mutation timings: next entry.
- **0.32.7 (operator: "WAN control-lane starvation is paramount")** —
  measured first (14:05-14:30): WAN RTT under load 58→60-87 ms (ss/ping
  from both ends), so the link's queue was not the seconds. What was:
  gbni-2 `retain_objects` handler avg 916 ms, max **12,113 ms** per batch
  (status `rpc_server.message_timings`), matching gbni-1's 12,467 ms
  mutation (delta 34,791 B; small deltas 250-2,000 ms); the handler still
  `valid()`-re-read every extent of every claimed file (a quantum re-claims
  the whole file) — the remote twin of finding #3. And node-id order put
  es-1 (333c…) before gbni-2 (6883…) for every gbni-1 commit. Fix: has()
  on the handler, no DATA admission; per-peer control-lane latency EWMA in
  the transport; `order_commit_replicas` local→nearest→unmeasured; mutate
  log line + status timing breakdown (`mutation_retention_ms_*`,
  `mutation_publish_ms_*`, `rpc_transport.peer_latency_ms`). Verify after
  deploy: gbni-2 retain_objects handler_max ≪ 1 s; gbni-1 mutate p90 well
  under 1 s; `rpc_transport.peer_latency_ms` on gbni-1 shows gbni-2 at a
  few ms and es-1 at 60+ ms; a `metadata commit store replica=` line from
  gbni-1 names es-1 only when gbni-2 failed; es-1's nearest is whichever
  gbni answers faster (expect gbni-2, the idle 16 GB box).
  Remaining in this area: DATA-lane pacing against control RTT (not the
  bottleneck today), retention-journal growth from whole-file re-claims.
- **UI session report (14:20):** transcode generation setup on a 9 GB
  HEVC/DV title costs 5-13 s per PATCH (server-side x264 full-res CRF 20,
  4 threads, no reuse across preference-only changes; `auto` picks
  transcode for an HEVC-capable client, DV/HDR suspected). Queued under
  viewer-facing items; not this iteration.

- **13:00 gbni-1 load mitigation.** After the relaunch macha itself ran at
  360 % CPU (3.6 of 4 cores) hashing/encrypting the 3.2 GB spool backlog;
  `vcgencmd get_throttled` = 0x50000 (under-voltage and throttling have
  occurred since the 12:15 boot), 64.8 °C, one 12.5 s metadata mutation.
  The rsync bwlimit bounds ingest, not the publication burst behind it.
  Applied live, no restart: `systemctl set-property macha.service
  CPUQuota=250%` (persistent drop-in) and import relaunched with
  `BWLIMIT=4000`. **Engineering follow-up:** a per-node background-effort
  budget (publication/hashing concurrency derived from a configured CPU
  ceiling) so a power-limited node never has to be throttled from
  outside; the priority law already keeps viewers ahead of loaders.
- **13:54 CEST: operator removed the stray copies on es-1 (root disk 69.9 →
  17 GB) and left macha stopped; restarted by me.** Guard now reads
  `mountpoint_stray_entries=0`, immutable, on es-1 too. The waiting import
  script had marked Movies "done" with rc=11 when the mount went away and
  moved on to TV, so it was relaunched (pid 165238, Movies→TV) — the
  script treats a non-zero rsync exit as the end of that directory; a
  retry-the-directory rule would be a small improvement.
- **0.32.6 DEPLOYED (commit `a2a8969`; gbni-2 12:41, gbni-1 12:42 via
  binaries staged on gbni-2 — no build on gbni-1, es-1 13:44 CEST).**
  Guard verified from the status API: `mountpoint_immutable=true` on all
  three, `mountpoint_stray_entries` 0/0/**1** (es-1's hidden `Movies`
  dir, see finding #7 — the daemon now reports it). Imports relaunched
  with the mount-guarded script: gbni-1 pid 2158 (12:42, Movies→TV→Music,
  bwlimit 8000), es-1 pid 164663 (13:44 CEST, Movies→TV). First check:
  es-1 spool 160→204 MB and 804 journal records in 2 min — writes reach
  Macha this time; root disk flat at 69.9 GB. gbni-1 spool 1.2 GB, load
  0.3. **Loop STOPPED at operator request ("stop after this iteration")**;
  resume with `/loop` + the same monitor prompt, or run
  `/Users/tom/.claude/jobs/6aee47b8/tmp/import-check.sh` by hand.
- **0.32.6 (DLT8 append-extents + finding #7 mountpoint guard; suite
  357/357 after one flaky rerun):** deploy order gbni-2 (build) → es-1
  (nice build) → gbni-1 (binaries staged on gbni-2, shipped, no build).
  Import scripts on both writers replaced by the mount-guarded version
  (`wait_mount` before every directory; gbni-1 now `--bwlimit=8000` too).
  Verify next ticks: (a) `mountpoint_immutable=true`, `mountpoint_stray_entries=0`
  on all three in status; (b) es-1 written-bytes ≈ 8 MB/s and, this time,
  `pub` counters and spool actually moving with it; (c) history growth per
  32 MB quantum ≈ 500 B not 105 KB (`hist=` MB in import-check).

- **Finding #7 (13:25 CEST, es-1) — writes before the mount go to the
  host disk.** es-1's 11:38 import restart began 25 s after the daemon;
  the mount comes up ~30 s after start, so rsync's generator walked the
  bare `/mnt/machamedia` directory and every write since went to the root
  NVMe: **52.6 GB / 46 files under the covered dir**, hidden by the mount,
  while Macha showed spool=0, pub=1/1 (the "it's not publishing" puzzle).
  `fail_closed_mountpoint` only chmod'ed the dir after mounting, which
  root ignores. Fix in 0.32.6: `prepare_fuse_mountpoint` counts stray
  entries (ERROR log + `filesystem.mountpoint_stray_entries`) and sets the
  immutable flag on the covered dir before services start (`chattr +i`
  semantics, stops root, persists; `filesystem.mountpoint_immutable`);
  proven on gbni-2 that tmpfs/FUSE mount over an immutable dir works and
  `touch` under it gets EPERM. **OPERATOR ACTION (deletion was refused
  by my permission classifier):** on es-1 remove the stray copies —
  `mkdir -p /mnt/rootview && mount --bind / /mnt/rootview && rm -rf
  /mnt/rootview/mnt/machamedia/Movies && umount /mnt/rootview` — they are
  exact copies of `/mnt/diskA/Movies/*` written 11:38–13:30 CEST today.
  gbni-1 and gbni-2 covered dirs verified empty. es-1's Movies import
  therefore stands where pass one left it; pass two restarts from there.

- **DLT8 first cut broke the retention barrier:** `retain_metadata_publication`
  only read `upsert_entries`; two rpc_cluster tests (touch requires
  retention; partition-delete defers GC) caught append records bypassing
  it. Fixed: an append retains the whole resulting entry, like the upsert
  it replaces.

- **0.32.5 (commit `97dbac9`, suite 355/355; deployed gbni-2 ~10:57, es-1
  11:38 CEST; gbni-1 still down):** spool pacing admits against drained
  quantum credit regardless of the stale whole-file rate sample (es-1's
  rsync had collapsed to 0.3 MB/s while a 13.9 GB file published). es-1
  import restarted pid 163043. Verify next tick: es-1 written-bytes delta
  ≈ 8 MB/s × 1800 s ≈ 14 GB per tick. Next engineering item: DLT8
  append-extents delta (each quantum commit re-sends the whole extent
  table: 105 KB/quantum, 124 MB history per node in 35 min).

- **Finding #6 (04:40, es-1, gbni-1 still down):** `media input read failed
  … error=extent unavailable` ×129 and `catalogue hint gave up … failures=3`
  ×43 on Music files gbni-1 wrote before it died: their only copy was on
  gbni-1 (data floor 1; second replica is repair debt that had not
  converged). Design items: push the second replica promptly after
  publication (bounded delay, not "eventually"), and catalogue hint jobs
  must re-queue when extents reappear rather than give up.

- **INCIDENT 04:13 (gbni time):** gbni-1 went dark at the network level
  (no ARP/ping from its own LAN, cluster marks it offline) ~1 min after its
  0.32.4 restart and import relaunch (65k-op spool replay + two rsyncs for
  a moment — a stale one survived the pkill). No console access from here;
  if it does not reboot on its own it needs a power cycle by the operator.
  es-1 + gbni-2 carry on (metadata floor 2 of 2 online). gbni-1's last
  known state: 0.32.4 installed, import pid 91906 running Movies.

- **0.32.4 (commit `f0760e9`, suite 355/355; deployed gbni-2 ~04:05, es-1
  05:10 CEST, gbni-1 04:12):** `open_writes_mutex_` no longer spans
  metadata mutations (commit_write / open_write truncate / namespace batch)
  — publications were serialized one WAN-bound commit at a time. es-1's
  rsync now runs with `--bwlimit=8000` (its disk was at 77 % iowait reading
  17 MB/s into a capped spool; good-neighbour). Imports restarted: es-1 pid
  157271, gbni-1 pid 91906. Verify next tick: gbni-1 extent puts per 10 min
  (was 9), data_publications_completed rising, es-1 iowait.

- **0.32.3 (commit `0820ecb`, suite 355/355; deployed gbni-2 03:46, es-1
  04:49 CEST, gbni-1 pending its build):** clean accounting checkpoints
  trusted with packs / dirty checkpoint = estimate while the walk runs;
  local retention claim = `has()`. es-1 status 40 s+ timeout → 0.7 ms;
  store online in 2.6 s vs a 32-min walk. gbni-1 deployed 03:50 (store
  online in 309 ms vs 4 min 21 s; status 1.3 ms). Imports restarted: es-1
  pid 156042, gbni-1 pid 90309 (Movies→TV→Music). Next: watch the batching
  effect and publication throughput; then the WAN/metadata latency items
  (congestion-aware data-lane pacing; publish_commit replica order).

- **0.32.2 (deployed 03:15–03:17 gbni / 04:17 CEST es-1, commit `eb9f568`, suite 354/354):** identity namespace batches (one commit per
  batch instead of per op) + utimens survives async publication. Imports
  restarted on both writers (gbni-1 pid 87771 Movies→TV→Music; es-1 pid
  154710 Movies→TV). Verify at the next check: namespace_publication_batches
  growing far slower than namespace_operations_published on gbni-1, data
  publications completing, `rsync -ani` dry run over Music showing no
  `>f..t` lines for files imported after this deploy. gbni-1 import order is now Movies → TV
  → Music (`/root/uat/import-all.sh` edited). Next iteration candidates,
  evidence in the UAT file "Iteration 2": WAN control-lane starvation
  (195–284 s metadata mutations; congestion-aware data-lane pacing driven
  by health RTT is the principled fix), `publish_commit` replica order by
  proximity, viewer seek (mid-file segment >90 s via playback API), es-1
  boot WARN storm. UI session is measuring real playback; reply to its
  questions sent (msg d2e4bf11).
- **0.32.1 (deployed 02:25–03:27):** write admission blocks instead of
  EAGAIN. Imports restarted on both nodes after deploy.
- **Iteration 1 (02:08–02:11):** gbni-1 import died at 30 % of Music on
  EAGAIN; es-1 unaffected.

## Stress import IN PROGRESS (started 2026-09-07 00:40, operator-requested)

Full libraries → real namespace roots, as the "new user imports two sites"
test. `/root/uat/import-all.sh` on gbni-1 (pid 82252: Music → Movies → TV,
6.1 TB / 15.5k files) and es-1 (pid 151383: Movies → TV, 2.4 TB / 6.3k
files); one rsync at a time per node, `nice -n 10 ionice -c3`, `-a --inplace`,
sources read-only, logs `/root/uat/import-{all,Music,Movies,TV}.log`,
`import-all.rc` appears when a node finishes. Estimate ~9 days at 8 MB/s
local / 6 MB/s WAN. **To stop: `kill <pid>` then `pkill -f "rsync -a --inplace /mnt/diskA"`
on that node** (never touch other users' processes on es-1).
Monitor: `/Users/tom/.claude/jobs/6aee47b8/tmp/import-check.sh` (one line
per node); this session wakes every ~30 min to run it. Watch for: gbni-1
RSS (4 GB box; snapshot heading to ~100 MB encoded), `mutate_max_ms`
growth, `snapshot_bytes`, WARN/ERROR, parked/abandoned, conflicts count,
full-frame reconciliations. Expected benign: conflicts for paths both
libraries hold with different rips.

## Programme status: COMPLETE (2026-09-07 00:25)

Nothing is in flight. A wake-up that reads this file should: verify all
three nodes are `active` with 0 boot WARNs and journals at 8 bytes, and
otherwise stop — the scheduled job (`61c413f7`) can be deleted. Follow-ups
are filed in the UAT file's last section (present-content skip on writer
restart; journal compaction while busy; compact extent encoding; the 9
standing conflicts need a human via the manage API).

## Discipline 1 — working notes

Goal: a barrier that fails on a stale placement token probes the peer for
the object ids, the peer verifies presence + flushes, returns a fresh token,
and the batch is re-stamped. Fallbacks: object absent on the peer → re-put
from a local copy if any → else discard the writer and replay from the spool.

Known evidence to reproduce: restart es-1 while gbni-1 publishes a big file;
gbni-1 then logs `quorum unavailable … replica=<es-1> outcome="remote-refused:
storage durability epoch changed"` forever (0.28.3 instrumentation).

Steps:
1. [x] Wire: `object_durability_barrier` request gains optional trailing
   `u32 count + count×32-byte ids`; server on epoch mismatch with ids →
   `reassert_durable()` each (has + flush), reply ok + epoch + fresh tokens.
2. [x] Client: `DistributedStore::durability_barrier(DurabilityBatch&, FrameType,
   std::vector<ObjectId>* unsatisfiable)` probes on `epoch changed` (and
   re-derives locally on a reopened backend), re-stamps replicas, reports ids.
3. [x] Writer: unsatisfiable → re-put from local copy else `ESTALE`; FUSE loop
   treats `ESTALE` as discard-writer-and-replay-from-spool.
4. [x] Tests: `storage_v18/test_durability_barrier_rederives_placement_after_peer_restart`,
   `…_reports_objects_a_restarted_peer_lost`; harness `restart()`. (Writer-level
   re-put test not written; covered live in step 5.)
5. [ ] Deploy 0.29.0 to all nodes; UAT: rsync a multi-GB file from
   `/mnt/diskA` on gbni-1 into its mount, restart es-1 mid-publication, expect
   `object durability re-derived … reasserted=N absent=0` on gbni-1 and
   `present=N/N` on es-1, publication completes, no `quorum unavailable`.
   gbni-1's stranded Pulp Fiction (inode 7666) clears on gbni-1's own restart.

Also in 0.29.0 (found by the suite while landing step 3): publication ENOENT
is re-derived from the decoded namespace view instead of poisoning the inode
(`publication_path_may_still_appear()`), race test 0/40, terminal test 0/10,
suite 341/341. Committed locally as 0.29.0.

## Discipline 2 — design notes (written while waiting on discipline 1's UAT)

Code facts gathered:
- `SubsystemRetryPolicy` (`subsystem_supervisor.hpp:26`): max_failures_in_window,
  failure_window, initial_backoff, max_backoff. Generalise to `RetryPolicy` in
  a new `retry_policy.hpp` with a small `RetryState` (consecutive failures,
  window deque, next_due) and `next_delay()` / `exhausted()`; the supervisor
  keeps its behaviour by adopting it.
- FUSE data loop retry: `fuse_frontend.cpp` ~3643 `if (retry) sleep 100ms` —
  fixed. Add `Inode::publication_retry` (RetryState) reset on success; delay
  = policy.next_delay(); on exhaustion → `inode->parked = {error, attempts,
  first/last failure}` and `backend_error` stays unset (parked ≠ poisoned);
  Status gets `filesystem.parked[]` (inode, path, error, attempts, since);
  manage API: `POST /api/v1/manage/filesystem/parked/{inode}/retry|abandon`.
- FUSE namespace loop: has 50 ms→5 s backoff (`:2851`) but no park except the
  operator skip on non-retryable errors; keep, and add the same park for a
  *retryable* error that exhausts the budget (blocked-op API already exists —
  extend it to list parked namespace ops).
- Startup: `Service::wait_services_ready` waits `service_startup_timeout`
  (`config.hpp:426`, default 120 s) then `_Exit(1)`. Replace with a
  no-progress gate: `NodeRuntime` exposes a monotonic `recovery_progress()`
  (bytes/frames/heads counters incremented by `recover_state` stages and by
  `MetadataReplica` load/materialise and the FUSE journal loader); the wait
  loop wakes every second and only kills when the counter has not moved for
  `service_startup_no_progress_ms` (default 120000). Keep
  `service_startup_timeout_ms` as an absolute ceiling (default 0 = none).
- RPC deadline: `net.cpp` "remains active while peer health is monitored" at
  ~2170/2190; add `rpc_no_progress_deadline` (default 30 s) for control-lane
  calls → fail with a distinct transient error.
- Log-rate rule: one line per backoff step.

Tests to write: FUSE publication against a peer refusing one object forever
→ parks within budget, other inodes unaffected, Status shows it, retry-now
works; namespace op exhausting its budget parks; startup gate does not fire
while the progress counter moves (simulate slow replay) and does fire when
it stops; RPC no-progress deadline returns within bound.

## Discipline 2 — status (0.30.0, code complete 2026-09-06 late evening)

Landed (see CHANGELOG 0.30.0 for the full list):
1. `src/retry_policy.hpp` — `RetryPolicy` + `RetryState`; supervisor adopts it.
2. FUSE data loop: per-inode `publication_retry` backoff, `Inode::parked`,
   `admit_deferred` skips backed-off inodes, `parked_publications()` /
   `retry_parked_publication()` / `abandon_parked_publication()`; Status
   `diagnostics.filesystem.parked_publications` +
   `publication_retries_backed_off`; manage API
   `GET /api/v1/manage/filesystem/parked-publications`,
   `POST .../{inode}/retry|abandon`. Config `fuse.publication_retry_*`.
3. Namespace loop: `fuse.namespace_retry_*` budget; exhaustion → EAGAIN
   `namespace_blocked_op`, keeps retrying at ceiling (ordered queue, no park).
4. Startup gate: `startup_progress.hpp` counter ticked by journal parse,
   delta apply, snapshot decode, storage scan, `mark_ready`;
   `service_startup_no_progress_ms` (120 s), `service_startup_timeout_ms` now 0.
5. RPC: `RpcClient::call(..., stall_notice, no_progress_deadline)`;
   `network.control_no_progress_deadline_ms` 30 s / `data_..._ms` 0.
Tests passing in isolation: `publication_backs_off_then_parks`,
`service_startup_gate_waits_while_recovery_progresses`,
`rpc_call_fails_after_no_progress_deadline`, plus all discipline-1 tests.

Discipline 2 is complete (all items below done 21:50; kept as the record of
what was run and where the on-box artefacts are: `/root/uat/d2-before.sh`,
`d2-after.sh`, `*.out`, `*-isolated.log` on gbni-1; UAT files under
`/mnt/machamedia/UAT/d2-before/`, `d2-after/` — leave them, they are part
of the namespace now). gbni-1's `/etc/macha/macha.yaml` is back to shipped
defaults (no `publication_retry_*`, no `service_startup_timeout_ms`).

- [x] Full suite green 345/345; committed `bb3697c` (0.30.0).
- [x] Deployed 0.30.0 to gbni-2 (21:01), es-1 (22:03 CEST), gbni-1 (21:03;
  booted in 3 s). gbni-1 config: temp `service_startup_timeout_ms: 1800000`
  removed; **UAT-only small budget added** under `fuse:` —
  `publication_retry_max_failures: 8`, `..._initial_backoff_ms: 250`,
  `..._max_backoff_ms: 5000` (backup `macha.yaml.bak-20260906-d2`). **Remove
  those three keys after the UAT** (restore the 100-in-30-min default).
- [x] UAT 2a before (0.29.0): `/root/uat/d2-before.sh` on gbni-1 — 90 s
  `nft reject` isolation; 254 retries at a flat 10/s. Recorded in UAT file.
- [~] UAT 2a after (0.30.0): `/root/uat/d2-after.sh` on gbni-1, log in
  `/root/uat/d2-after.out`. Isolation phase done: 16 retries, exponential
  backoff, both in-flight inodes (16565, 18307) parked at attempt 9 after
  22.8 s, listed by the API. Script continues: rsync 2 (Ghostbusters) must
  publish while parked, then it POSTs retry for the FIRST parked inode only
  → **after it finishes, POST retry for the other inode too** (both should
  drain; check `parked_publications=0`, spool retires). Record in UAT file.
- [ ] UAT 2b (RPC deadline): before evidence = es-1 0.29.0 `RPC stalled
  (control) … members … no_progress_ms=30000 … remains active` during the
  before-run (bounded only by dead_after), plus the 150–230 s
  `accept_metadata_commit` stalls from the plan doc. After: on gbni-1 add an
  `nft` **drop** (not reject) rule on *input* from 10.44.1.51 tcp dport 7437
  for ~60 s (do this only after d2-after.sh has finished — don't overlap
  faults) → gbni-2 should log `RPC made no progress for ~30000 ms (control)
  … cancelled for retry` and its next call succeed after the rule is removed.
- [ ] UAT 2c (startup gate): gbni-1 boots in 3 s now (journal replay linear
  since 0.28.3), so the gate is not stressed live; record that the elapsed
  ceiling is gone from config, `service_startup_no_progress_ms` governs, and
  cite `test_service_startup_gate_waits_while_recovery_progresses`.
- [x] Briefed "Macha UI Work" (msg 1e0c2bbd) on the parked-publications
  routes and status fields.
- [ ] Pre-existing WARNs seen on every boot (not 0.30.0): `FUSE async data
  publication failed inode=5806|922 error=missing` on gbni-1, `inode=2333`
  on es-1 — spool data missing for old journal entries; candidate for
  discipline 3 (resolve on recovery).
- [ ] Update UAT file + memory, commit, tell the operator `/compact` is safe.

## Discipline 4 — CODE COMPLETE as 0.32.0 (2026-09-07 ~00:30); suite, deploy, UAT pending

Measurement first (`macha-metadata-dump --stats`, new `--stats` mode, run on
gbni-2's live head gen 9903 at 23:30): encoded 2,319,777 B = entries
1,958,969 (extents 1,768,067 for 36,083 extents; paths+attrs 190,902),
**116 conflicts 335,749 B** (49 namespace_entry on media paths both
writers republished, 67 catalogue_root; 112 distinct head pairs; 0 identical
alternatives), 439 tombstones 24,584 B. Merge deltas = 335,961 B each
(the whole conflict set); reconciliations 23:22–23:27: 3 full frames
(7.7/6.4/5.4 MB) + 2 deltas; es-1 last 5: 4 full. Cause of full frames:
merge canonicalises tombstones by id, primary parent in append order,
DLT5/6 cannot reorder → `metadata_delta` nullopt → full snapshot.
Tombstones are 1% and GC'd → the plan's per-node retirement log is NOT
built (say so in the UAT).

Landed: DLT7 (flags: merge_parents / conflicts presence, canonical
garbage), `garbage_is_canonical`/`canonicalise_garbage`/
`prune_superseded_conflicts` (metadata.hpp), mutate_impl canonicalises +
prunes, merge prunes (`conflicts_superseded`), reconciliation log
`superseded= standing=`, `MetadataManager::resolve_conflict`, status
`diagnostics.metadata.{conflicts,namespace_conflicts,catalogue_conflicts,
tombstones,conflicts_superseded,conflicts_resolved}`, manage API
`GET /api/v1/manage/metadata/conflicts`, `POST …/{id}/resolve?choice=`,
docs (management.md, metadata.md), CHANGELOG 0.32.0, version 0.32.0.
Tests: dlt7_presence_flags_round_trip, merge_over_append_ordered_tombstones_is_a_delta,
superseded_conflicts_leave_the_snapshot (+2 updated) pass.

Remaining:
- [x] Suite 351/351; committed `27c1903` + `96c3413` (same-content rule);
  deployed to all three (23:55–23:57, and again 01:1x with the same-content
  rule). First mutation: conflicts 116 → 10 (`conflicts_superseded=106`);
  API resolve of one → 9 (`conflicts_resolved=1`). Recorded in UAT file.
- [~] Closing run IN FLIGHT (started ~01:20 from
  `/Users/tom/.claude/jobs/6aee47b8/tmp/final-run.sh`, output
  `final-run.out`, per-node logs `final-{gbni-1,gbni-2,es-1}.log` in the
  same dir): gbni-1 rsyncs Jurassic Park + Prometheus (2.1 GB each) to
  `/mnt/machamedia/UAT/final/gbni-1/`, es-1 rsyncs Idiocracy + Dog Soldiers
  (1.05 + 1.37 GB) to `…/final/es-1/`; restarts at +90 s gbni-2, +3 m es-1,
  +4.5 m gbni-1, +6.5 m gbni-2, +8.5 m es-1; waits for both spools to drain;
  prints counts. If this session is gone, read `final-run.out` and write the
  "demonstrative run" section of the UAT file from it.
- [ ] UAT before/after: (1) `--stats` on gbni-2 after the first merge under
  0.32.0 → conflicts should drop toward 0 as superseded (49 namespace ones
  whose paths were republished + 67 catalogue ones) — expect `superseded=N`
  in the `histories reconciled` line; (2) merge delta bytes: grep
  `history_body=delta history_bytes=` → hundreds of bytes, and no
  `history_body=full` reconciliations; (3) snapshot encoded bytes −336 KB;
  (4) any conflict that stays: list via the API, resolve one with
  `choice=right` (newer write) and show it gone.
- [ ] Demonstrative closing run (UAT file's last section): two concurrent
  rsync writers (gbni-1 + es-1, distinct dirs under /mnt/machamedia/UAT/final/)
  with rolling restarts of all nodes mid-publication; report: no wedges,
  reasserted/absent counts, parked=0, boot WARNs 0, merge delta sizes,
  snapshot bytes.
- [ ] Brief "Macha UI Work" on the conflicts API + status fields.
- [ ] Update UAT file, memory, commit; tell the operator `/compact` is safe.

## (original) Next — Discipline 4: compact history out of the hot path (+ DLT7)

Read the plan doc's discipline-4 section (`### 4.`) first. Evidence to
reproduce: snapshot size vs namespace size (270k tombstones → 15 MB
snapshots for 1,600 files per the plan). Start by measuring on-box:
`macha-metadata-dump` on each node (memory `project-metadata-forensics`)
for tombstone/conflict counts and encoded snapshot bytes; capture as the
"before". Then design per the plan: tombstones/conflicts leave the snapshot
once every replica has acknowledged the generation (or after a bounded
horizon), DLT7 delta format folded in, tests, CHANGELOG 0.32.0, deploy,
UAT = snapshot bytes proportional to live entries, sub-second decode, both
rsync writers running with rolling restarts (the "demonstrative run" in the
UAT file closes the programme).

Also carried forward (not blocking): the `cmake` plugin identity mismatch
ERROR the test suite prints (`libmacha-torrent` built at 0.27.0) — rebuild
the plugin locally when convenient; and journal compaction while busy
(journal only resets when idle; bounded by `max_operation_journal_bytes`).

## Discipline 3 — DONE as 0.31.0 (deployed 22:51–23:54; record kept)

Landed: all seven plan items below (fuse_journal scanner `corrupt_frame_offset`;
`load_journal` skip+count / quarantine tail; `initialise_namespace` drops
undescribed-inode ops with journaled markers; path-collision loser really
re-journaled; ENOENT-terminal → `abandon_data` + `publications_abandoned`;
metadata journal truncate-at-bad-frame, history skip-frame; status fields;
docs/operations.md "What recovery resolves on its own"; CHANGELOG 0.31.0).
New tests pass in isolation: `journal_fuzz_every_frame_mutation_still_starts`
(9 frames × 4 mutations), `recovery_abandons_publication_for_file_removed_from_namespace`,
`durable_journal_skips_unbacked_data_done`, `frame_scanner_exhaustive_tail_model`,
`metadata_journal_mid_frame_corruption_truncates_not_reseeds`.

UAT plan (before evidence already captured in the finding list below):
- Before: gbni-1 boot 21:50 = 2× `publication failed … error=missing` +
  20× `accepted data completion without published prefix` WARNs; journal
  132 MB (`/etc/macha/fuse-operations.log`), spool 183 MB (inode-922) +
  689 B (inode-5806). es-1 boot 22:03 CEST = 1× missing (inode 2333) + 1×
  without-published; journal 178 MB, spool 6.0 GB.
- After (first 0.31.0 boot): expect `FUSE data publication abandoned inode=922
  …` / `5806` / `2333`, `dropped FUSE spool generation …`, spool → 0,
  journal reset to 8 bytes once idle (`publications_abandoned` in status).
  Second boot: zero recovery WARNs, journal 8 bytes. Record boot WARN
  counts + sizes before/after for both nodes in the UAT file.
- Note for the UAT: the abandoned bytes are files the operator removed
  from the namespace after writing them (unpublished writes to since-deleted
  files); nothing visible is lost. Say so explicitly.

## Discipline 3 — findings (kept)

Findings that fix the scope (all verified on-box):
- gbni-1 spool = inode-922 (183 MB, since Sep 5) + inode-5806; es-1 spool =
  inode-2333 (6.0 GB, since Sep 4). Their files left the namespace; every
  boot `resume_recovered_data` → `open_write` → ENOENT "missing" → terminal
  → `backend_error` poison. They pin `durable_pending_operations` > 0 so the
  journal never resets (132 MB / 178 MB, re-parsed every boot, and the ~20
  benign `accepted data completion without published prefix` WARNs re-raise
  each time).
- `load_journal` throws on any semantic inconsistency (duplicate/non-monotonic
  marker, marker without op, unknown type) → process exits → systemd loop.
  Mid-journal checksum mismatch: same. `initialise_namespace` throws on an op
  whose inode has no descriptor.
- 0.28.3's path-collision "re-journal the loser" is a no-op: the loser's
  `journal_epoch` already equals the current epoch so `journal_inode_locked`
  returns early → the collision is re-resolved every boot.
- `MetadataReplica::load_journal`: a mid-journal auth/semantic failure →
  `recover_from_seed` quarantines *every* metadata file. `load_history`
  mid-file failure → same.

Plan (each with a test):
1. FUSE journal parse: a frame that does not fit the state so far is skipped
   and counted (`journal_recovery_skipped_frames`), never fatal; duplicate
   inode descriptor → last wins; "done without published" → DEBUG + count.
2. Mid-journal checksum corruption → quarantine tail to `<journal>.corrupt.<ts>`,
   truncate, count bytes; scanner reports `corrupt_frame_offset` instead of
   throwing.
3. `initialise_namespace`: op with no descriptor → drop with journaled
   done/abandoned markers (`recovery_dropped_operations`), spool preserved
   as orphan. Path-collision loser really re-journals.
4. Terminal ENOENT publication (file gone from namespace) → `abandon_data`
   (journaled, spool retired, `publications_abandoned`), not poison — fixes
   922/5806/2333 on the first 0.31.0 boot; journal resets after.
5. Metadata journal: mid-journal failure → truncate at that offset with
   quarantined tail (chain semantics), keep checkpoint/history; history:
   skip the bad frame, count, continue.
6. Fuzz fixture test: every frame × {truncate, drop, duplicate, corrupt} →
   frontend starts; plus the removed-file abandon test (two boots, second
   clean); metadata mid-journal corruption test.
7. Status fields + docs + CHANGELOG 0.31.0; deploy; UAT = before/after boot
   logs on gbni-1 and es-1 (WARN count, journal size, spool bytes), second
   boot clean.

## Next — Discipline 3: recover by resolving (+ journal fuzz fixture)

Read the plan doc's discipline-3 section first. Known live cases to drive
the design (all reproducible on the cluster today):
- Every boot on gbni-1 logs `FUSE async data publication failed inode=5806
  error=missing` and `inode=922 …` (WARN, 3× each since 19:24); es-1 logs
  the same for `inode=2333`, plus `FUSE journal recovery accepted data
  completion without published prefix inode=6435 sequence=65909` and
  `recovered durable FUSE operations pending=22997 namespace=0 inodes=1`.
  These are journal entries whose spool bytes are gone: recovery re-raises
  them on every start instead of resolving them once (tombstone the
  generation, or re-derive from the namespace whether the file is already
  published). Start by reading the recovery path in `fuse_frontend.cpp`
  (`replay_data_quantum`, the journal loader, `abandon_data`) and
  `metadata.cpp` `load_heads` / quarantine handling (memory
  `project-metadata-forensics` has the quarantine locations).
- Metadata quarantines from 2026-09-06 (see memory) — recovery that refuses
  a head instead of merging/resolving it.
Steps: (1) enumerate every `throw`/refuse in the FUSE journal recovery and
`MetadataReplica` load paths and classify resolve-vs-refuse; (2) resolve
each (drop-with-tombstone, re-derive, or quarantine-and-continue with a
status field), never re-raise on the next boot; (3) journal fuzz fixture:
truncate/corrupt/duplicate frames of a real journal copy
(`/etc/macha/fuse-operations.log.bak-20260906-dup` on gbni-1 is 129 MB —
copy a slice, don't move it) and assert recovery converges with no
repeated WARN across two boots; (4) CHANGELOG 0.31.0, docs, tests; deploy;
UAT = two boots of each node show zero repeated recovery WARNs and no
quarantine growth, with both rsync writers running.

## (original discipline-2 plan, kept for reference)

Start Discipline 2 from the design notes above: (1) `retry_policy.hpp`
(`RetryPolicy` + `RetryState`), adopt in `SubsystemSupervisor`; (2) FUSE data
loop backoff + park (`Inode::publication_retry`, `parked` state, Status
`filesystem.parked[]`, manage retry/abandon); (3) namespace loop park on
budget exhaustion; (4) startup no-progress gate (`recovery_progress()`
counter, `service_startup_no_progress_ms`); (5) RPC control-lane no-progress
deadline; tests for each; deploy; UAT = a peer refusing one object forever
(simulate by removing it on the peer after a restart) parks one inode within
budget while the rest of the cluster keeps publishing at full rate, and a
copy of gbni-1's 2026-09-06 state starts under the gate. Then brief the
"Macha UI Work" agent on the new Status/manage fields.

First live run (19:26): es-1's 6 s restart landed exactly on a barrier →
`remote-transport: send: Broken pipe` → the writer treated it as lost and
replayed the file from the spool. Fixed: transient outcomes are never
reported as unsatisfiable (`transient=yes|no` on the failure line); EAGAIN
keeps its own message. 342/342 after. gbni-1 is still publishing Pulp
Fiction (13.9 GB spool, ~8 MB/s) which holds the spool at the 16 GiB
ceiling and throttles any other writer there — rerun the UAT once that
spool has retired (spool_bytes on gbni-1 drops by ~13.9 GB).

**Evidence already captured (19:43, real workload):** gbni-1
`object durability re-derived after incarnation change reasserted=260
absent=0 peers=1`; es-1 `re-derived after epoch change present=260/260
expected=5b5b16ca current=714c3b85` — Pulp Fiction's 260 extents on es-1
re-stamped after es-1's restart, none re-sent. Recorded in
`2026-09-06-self-healing-uat.md` (run 1). Run 2 (clean, final cut, 2001: A
Space Odyssey, three spaced es-1 restarts) still to do after the deploy job
finishes (it waits for Pulp Fiction's spool to retire, then installs on
gbni-1 → gbni-2 → es-1).

Live UAT script for step 5:
1. On gbni-1: `nice -n 10 ionice -c3 rsync -a --inplace <one multi-GB file
   from /mnt/diskA> /mnt/machamedia/<dir>/` (source read-only).
2. While its publication runs (`write stage` lines on gbni-1), `systemctl
   restart macha.service` on es-1.
3. Expect on gbni-1: `object durability re-derived after incarnation change
   reasserted=N absent=0 peers=1`; on es-1: `object durability re-derived
   after epoch change present=N/N`; no `quorum unavailable`; publication
   completes (`data_publications_completed` advances; file readable on
   gbni-2 with the right size).

## Ready commit message

(committed) 0.29.0 — Re-derive durability from disk after a peer restart instead of retrying a dead token

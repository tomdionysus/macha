# Plan: what stands between the playback-session routes and the cluster

Date: 2026-09-21

Status: **Phase A executed 2026-09-21**, except its last two steps. The three
decisions were taken on the recommendations below and the work is committed on
`develop` as `60d794e`. The es-1 suite run and the `0.48.0` tag are still
owed: syncing to a node was refused as a production action, so the only
evidence so far is 484/484 on the laptop, which this project has never
accepted as sufficient. Phases B-E are gated on
core, the web bundle and the operator's word. Companion to
[the resource plan](2026-09-21-playback-sessions-as-a-resource-plan.md), which
is the specification. This file is the ordered list of what has to happen
before that work can be deployed to all three nodes and tested with the
clients. When every box here is ticked, the deploy checklist in `ACTIVE.md`
is the last thing left.

The readiness check found the code finished and the es-1 build current
(source byte-identical to `develop`, objects newer than sources, nothing left
to rebuild, `MACHTEL3` in the built library). What it found unfinished is
everything around the code: the release is unversioned, the docs describe the
old behaviour, two items the plan calls for are undecided, and all three
external gates are open.

## Decisions the operator owns — all three taken 2026-09-21

Taken on the recommendations, on the operator's instruction to execute this
plan. Each is now recorded in the release rather than living here: (1) `410`
is shipped, and is **mandatory on both the server and core before deploy** —
restated by the operator on 2026-09-21 and not subject to reopening; (2) the
example config and reference pair 64 node-wide with 32 per account, and the
live nodes still need raising; (3) the `playback/status` disclosure is
accepted and stated in the 0.48.0 changelog.

1. ~~**`410 generation_superseded`: bundle it, or hold it for a second flag
   day?**~~ **SETTLED, AND NOT REOPENABLE (operator, 2026-09-21): `410` is
   required on both the server and core before this deploys.** It is not
   conditional on any client's readiness to classify it, and no finding about
   a client is grounds for reverting it to `404`. Both halves are in place:
   the server implements it, and core's tolerance is in the tree the clients
   link. The original question, kept for the record, was whether to bundle it
   here or hold it for a second flag day.
2. **Node-wide `max_sessions` on each node.** All three run `8`; the
   per-account default is `32`. As configured the account cap is unreachable:
   the node-scoped refusal fires first every time, and core walks the cluster
   on a node-scoped refusal. *Recommendation: raise node-wide above the
   account cap, so that one account hitting its own limit is refused with
   `account_session_limit` and everyone else is untouched.* A session record
   is cheap; the expensive resource is already bounded by
   `max_video_transcodes: 1`. `64` node-wide with `32` per account is the
   shape; the numbers are the operator's.
3. **`GET /api/v1/playback/status` disclosure to any `media_viewer`.** Node
   aggregates (session count, transcode load, probe cache bytes, and now the
   cap limit). The security review flagged it as deliberate and asked for the
   operator to say so rather than discover it. *Recommendation: accept for a
   household system, and record the acceptance in the resource plan.*

## Phase A: make the repository releasable (this session)

Everything here is in-tree, needs no one else, and can be done today.

- [x] **Bump to 0.48.0** in `CMakeLists.txt`. `src/telemetry.cpp:24` already
  says the format was "introduced in 0.48.0"; the build says 0.47.0, which is
  what the nodes report now. Without the bump the post-deploy
  `/api/v1/health` version read is blind and the tags-match-binaries
  discipline kept since 0.46.0 is broken.
- [x] **Write the 0.48.0 `CHANGELOG.md` entry.** It has to say, in this order:
  the break (old stream route removed, `POST` no longer supersedes), the new
  shape (collection `GET` under `items`, `201` + `Location`, stream as a
  subresource), the cap (`streaming.max_sessions_per_account`, `429`
  `account_session_limit`, limit and count on the refusal and on creation),
  what rides `/api/v1/status` per node (`max_sessions_per_account`,
  `pipeline_idle_ms`, `session_idle_ms`), TEL3 with no compatibility and the
  one expected WARN on first start, the four security fixes, and `410` if
  decision 1 is yes.
- [x] **Re-apply `410 generation_superseded`** if decision 1 is yes, with a
  test pinning the status and the code, and retire the note at
  `src/playback.cpp:2073`.
- [x] **Constant-time compare on the stream token**
  (`src/playback.cpp:2037`, `it->second->token != token`). Two lines; removes
  the open security item rather than carrying it into a release that makes
  ids enumerable.
- [x] **`docs/streaming.md` rewritten for the resource model.** Lines
  259-291 describe one session per bearer and supersession by `POST`; lines
  306 and 404 give the retired stream URL. It needs the collection `GET`,
  `201` + `Location`, the cap and its refusal, the `account` block on
  creation, and the new stream path. This is the document the clients are
  briefed from, so it is on the critical path, not hygiene.
- [x] **`docs/configuration.md` and `macha.yaml.example`** gain
  `max_sessions_per_account` with the arithmetic from the `src/config.hpp`
  comment, beside `max_sessions`.
- [x] **Backlog hygiene that would otherwise mislead the next session**:
  mark the telemetry P1 (`ACTIVE.md`, "A telemetry set cannot carry an
  optional field safely") as closed by TEL3 and move it to `COMPLETED.md`;
  note in the resource plan that the cap key is settled as per-account
  (its line 288 still says "open decision"); record decision 3.
- [x] **Full suite on es-1 — 484/484, zero failures.** Run 2026-09-21 13:30
  on the operator's word that nothing is currently production. Tree rsynced
  and verified identical by md5 (`src/playback.cpp`,
  `tests/test_media_playback.cpp`), built clean in 3m04s at `-j3`, log kept at
  `/tmp/suite-0.48.0.log` on es-1 (39,614 bytes, 484 `[PASS]`, 0 `[FAIL]`).
  `aarch64`, glibc 2.41. The new case,
  `media_playback/test_a_superseded_generation_is_gone_and_a_future_one_never_existed`,
  passed at 136 ms. **Nothing was installed and the service was not
  restarted** — this was a build and a test run, not a deploy. The laptop had
  also given 484/484, but that has never been evidence for this project.
- [x] **Commit on `develop`** — `60d794e`. `README.md` was left unstaged; its
  change is unrelated to this release.
- [x] **Tag `0.48.0`** — the tag names the tree that went green on es-1.

## Phase B: the client side (core drives; this repository waits)

None of this is the server session's to do. Core is briefing Android TV and
mobile (confirmed 2026-09-21) and drives the sequence.

- [ ] **Brief the four client sessions** from the rewritten
  `docs/streaming.md`, before anything is published. The brief must name:
  the route break and that there is no dual-serve window; that `POST` creates
  every time and a client wanting the old behaviour must `DELETE` first;
  that a lost id is recovered from the collection `GET`; the cap, its
  `account` scope, and that adoption through the listing spends budget; that
  every node's cap limit is on `/api/v1/status`; `410` if decision 1 is yes;
  and the expected A85 "plays, no sound" on Direct Play, which predates all
  of this.
- [x] **Core tolerance — NOT A GATE, and never was.** The clients link the
  local core working tree directly (`file:../macha-ts`), so they compile
  against the tolerance the moment it is in that tree, which core confirms it
  is. There is no publish, no version pin and no client release to wait for;
  that direct link is what makes joint testing possible at all. `410` goes
  out and the clients test against it. This entry is kept, ticked, only
  because two earlier drafts of this plan wrote it as a blocker in two
  different wordings and both were wrong.
- [ ] **The web client builds a bundle against that core** with its own
  `410` branch in the segment classifier (the plan records that hls.js
  raises the status one layer below core).
- [x] **Android TV: nothing owed.** Core, 2026-09-21: the route change costs
  it nothing (`playback/stream` appears nowhere in its `src`; it takes
  `stream.url` off the session), and `ExoPlayerAdapter.ts:68` feeds the media3
  `httpStatus` straight into core's `playbackFailureKindForStatus`, so `410`
  arrives classified on its next build with no client change.
- [x] **Mobile: checked, NOT a regression. P1 against the client, not a
  blocker.** Core tested the three ways this could have been one
  (2026-09-21) and could not break it on any:
  - **No different fatality or retry posture.** expo-video is media3
    underneath. Core disassembled `DefaultLoadErrorHandlingPolicy` from the
    Gradle-cached AARs (byte-identical across media3 1.8.0 and 1.9.0):
    `isEligibleForFallback` returns true for `InvalidResponseCodeException`
    with status in {403, 404, 410, 416, 500, 503} — **404 and 410 sit in the
    same set with no branch between them** — and `getRetryDelayMsFor` gives
    do-not-retry only for five non-HTTP causes, so both fall through to the
    same `min(errorCount * 1000, 5000)` backoff.
  - **Nothing classifies on the message text**, which is the only thing that
    does differ (ExoPlayer puts the code in the exception message). Every read
    of it is display or one prefix strip.
  - **Nothing treats an unrecognised status differently from a 404.** Below
    session creation, `404` is named in three non-playback places only.
  - **The failover path is status-blind and already harmful today.** On
    `status === 'error'` the provider calls `failoverSource` unconditionally,
    which picks a replacement node and **records the failure so ranking learns
    from it** — a healthy node charged for answering honestly, which is
    precisely what the `410` axes exist to prevent. But it does that *today*
    on the `404`, by the client's own comment at `PlaybackProvider.tsx:641`.
    Same event, same blind failover, same charge. The status swap changes
    nothing on that path. One guard exists and is also status-blind:
    `errorBlamesEndpoint` declines failover while a seek is outstanding, so
    the commonest way mobile makes a superseded generation is already handled,
    identically before and after.

  **Severity correction, 2026-09-21, after the verdict above.** Core
  understated what this costs a mobile viewer and corrected itself against its
  own interest. It first described the outcome as a reload — picture stops,
  failover runs, playback resumes at the same position. The mobile session
  then corrected its own account: **failover on that client does not recover
  at all**, so a `410` on the mode-switch path takes a viewer who was watching
  something to a stopped player and an error. Attribution matters here and
  core flagged it: this is the operator's statement today, relayed by the
  mobile session, superseding a 2026-09-08 device note in that repo that had
  failover working but not seamless. Core has not verified it and says it
  cannot from here, because it is a device behaviour rather than something
  readable in a tree. **The regression verdict is unchanged** — mobile's
  failover is equally broken under today's `404`, on a path that never
  consults the status — so this enlarges the pre-existing hole rather than
  reopening the cutover decision. What it does change is how the mode-switch
  remedy reads on that client: the marker on `applyUpdate` is not protection
  against waste, it is the difference between a mode switch that works and one
  that ends playback.

  **Two caveats, recorded rather than smoothed over:**
  1. **Nobody has put a `410` in front of expo-video on a device.** All of the
     above is verified at the policy layer (disassembled bytecode) and by
     reading client code. It is not an observation. See Phase E.
  2. **The layer is not fully understood.** The mobile repo flags against
     itself an unresolved contradiction: the A85 took a segment `500` as
     fatal on first occurrence on 2026-09-13, which the disassembly says
     should have been retried with backoff. Its reconciliation is explicitly
     recorded as a guess. It cuts symmetrically — a `404` and a `410` reach it
     as the same `InvalidResponseCodeException`, so it cannot single out
     `410` — but it bounds the confidence above.

## Phase C: cluster preparation (can overlap Phase B)

- [ ] **Web bundle to `/etc/macha/web` on all three nodes.** This one is
  real, and it is the one place where a *built artefact* lags the linked
  source tree — do not confuse it with the core package above, which is not
  a gate. Core's account, 2026-09-21:
  - The deployed `index-BGrNH6KR.js` is the 0.17.2 bundle and has no `410`
    handling.
  - The hls.js-level branch exists on macha-client `develop` only
    (`WebHlsPolicy.ts`, commit `2a0b95f`). It is **not** on `main`, so the
    released 0.17.3 bundle does not have it either.
  - Exactly one tolerant artefact exists: `dist/assets/index-NDVfpduh.js`,
    624,128 bytes, built 2026-09-21 12:12 from develop against the linked
    core. `dist/` is gitignored, so it is in no commit and no tag — a local
    file on one machine.
  - **It is current, and a rebuild is NOT required** (core, correcting itself
    2026-09-21 after first reporting it five commits stale). It was built
    against core `5aa3f6f` and carries the 10 s floor: the web client session
    re-ran the build against core's current tree and got a **byte-identical**
    output — same name, same 624,128 bytes, `cmp` clean — and core verified
    the other half independently, `macha-ts/dist/playback/PlaybackCoordinator.js`
    built 12:10 from src at 12:09 carrying `ALTERNATE_RECOVERY_WINDOW_MS =
    10_000` with the 8 s transcode window beside it. So the 12:12 client build
    consumed core's 12:10 dist, not the earlier tree.
  - The client's practice is to record the **core SHA and the `dist` hash**
    rather than a version number, because a link resolves a working tree and
    not a commit. Worth keeping when this is deployed.

  **DONE 2026-09-21 13:34** (each node's local time; 11:34 UTC). The web
  client session deployed `index-NDVfpduh.js` to all three nodes, two minutes
  before the server cutover at :36 — so the ordering came out as intended,
  bundle first and routes second. All three `index.html` now reference it.
  Verified served: `index.html` is `Cache-Control: no-cache` and the bundle
  name is content-hashed, so a client picks it up on the next load; the
  deployed service worker holds no Cache API entries (zero `caches`
  references), so it cannot serve a stale shell.

  **Owner, confirmed by core 2026-09-21: the web client session builds it and
  the operator authorises the deploy. Not the server session, and not core.**
  The procedure is in macha-client's own notes: `npm run build`, then rsync
  `dist/` into **`/etc/macha/web`** on each node — *not* `/var/lib/macha/web`
  — files owned `1000:50`, `index` served `no-cache` so a deploy shows on the
  next load without a restart. That is the pattern 0.17.2 followed on
  2026-09-20. Core has passed all of this to the web client session directly,
  including that its `develop`-only `410` branch is absent from the released
  0.17.3.
- [x] **Config on all three nodes — DONE 2026-09-21 13:34, by this session,
  immediately before the cutover.** All three now carry `max_sessions: 64` and
  `max_sessions_per_account: 32` explicitly, backed up as
  `macha.yaml.bak-0.47.0` (the backups show the old `max_sessions: 8` and no
  per-account key, which is the proof these are set values and not defaults or
  the example). Independently re-verified by the web client session against
  three distinct file sizes, hashes and line ranges, none matching
  `macha.yaml.example`. **`account_session_limit` is reachable today.**
  Original item: `max_sessions` raised,
  `max_sessions_per_account` written explicitly rather than left to the
  default, so the two numbers sit together in the file. Back up each as
  `macha.yaml.bak-0.47.0` first. These reload live and can go in before the
  cutover; a 0.47.0 binary ignores the unknown key. Confirm that with a
  `reload_config` on one node before touching the other two.
- [x] **Tarball targets confirmed**: `aarch64` and glibc 2.41 on all three.

## Phase D: cutover — DONE 2026-09-21, all three nodes together

Executed on the operator's instruction. Built on es-1 from the tagged tree,
staged with `DESTDIR`, tarball `/tmp/macha-0.48.0.tgz` (3,293,529 bytes, md5
`89d88eb5a1a1cb6341d944810e5dabb0`) shipped to fi-1 and gbni-1 and md5-verified
on each. Zero playback traffic on any node beforehand. All three stopped,
installed and started within the same minute — not rolling, because TEL3
excludes a straggler from gossip rather than misreading it.

Verified after: all three report `0.48.0`; **exactly one** `persisted telemetry
ignored: bad telemetry set` per node, never repeated, which is the documented
self-healing discard; es-1 peered with both on control and data lanes;
metadata writable at generation **33038**, `replicas=3/3 required=2`. A
15-minute watch found zero errors and no drift.

One item from the list below was **not** done: verifying the new per-node
`playback` fields through `GET /api/v1/status`, because that route needs a
bearer token and the anonymous account no longer carries `view_status`. TEL3
is flowing — the nodes converged and gossip is healthy — but the field-level
proof was not taken. Worth doing during the joint test.

Original checklist:

Build once, ship the artefacts, every node together. This is the standing
rule and TEL3 makes it mandatory rather than advisable: a node left behind is
excluded from gossip, not merely degraded.

- [ ] Rebuild on es-1 from the tagged tree (`cmake --build build -j3`),
  re-run the suite if any source changed since the Phase A run.
- [ ] Stage and tar: `DESTDIR=/tmp/stage cmake --install build`, tar `usr`
  to `/tmp/macha-0.48.0.tgz`, record the md5 and size here.
- [ ] Ship to fi-1 and gbni-1, verify the md5 on each after transfer.
- [ ] Check the journal on all three for playback traffic; wait for zero.
  The window is short but not nothing: with `metadata_min_write_replicas: 2`
  and three replicas restarting together, metadata is read-only until two
  are back.
- [ ] Stop and start all three within the same minute: es-1, fi-1, gbni-1.
  No spacing. Each config already carries the new keys from Phase C.
- [ ] Verify on each: `/api/v1/health` reports `0.48.0`; exactly one
  `persisted telemetry ignored` WARN on first start and no second; no other
  WARN or ERROR; peers converge at one metadata generation, writable.
- [ ] Verify the feature, not the version string: `GET /api/v1/status`
  from one node shows `max_sessions_per_account`, `pipeline_idle_ms` and
  `session_idle_ms` in the `playback` block of *every* node, which proves
  TEL3 is flowing over gossip between all three.
- [ ] Record the deploy in `ACTIVE.md` and the cluster-deployment memory as
  every previous release was.

## Phase E: joint test (core co-ordinates; the server session observes)

Each case names the observation that proves it, from a client, against the
live cluster. Reading the code does not count.

- [ ] **Two sessions on one bearer.** Second `POST` from one client returns
  a distinct id, both stream.
- [ ] **Handover through the listing.** A client discards its id, lists the
  collection, adopts the session, continues playback without a new
  generation.
- [ ] **Ownership.** One account's id, presented by another account, answers
  `404` on `GET`, `PATCH` and `DELETE`, and its stream URL answers nothing
  useful with or without the token.
- [ ] **The account cap, observed. The blocker that was here is LIFTED** —
  the raise happened at 13:34 on 2026-09-21, before the cutover, and all three
  nodes run 64/32. This test can run whenever the joint test runs.
  *Kept because the trap recurs whenever a node is rebuilt from bare
  defaults:* at `max_sessions: 8` under a per-account cap of 32 the only `429`
  provokable is the **node-scoped** one, and the mobile client's
  `classifyCreateRefusal` treats that as fatal by design — so the test would
  look like mobile's cap handling failing when it is correct. Check both
  numbers on the node before running it.
  Method: drop `max_sessions_per_account` to `2` on fi-1 alone, live
  reload, and have one client create three. The third is `429
  account_session_limit` with limit and count, a second account on the same
  node is unaffected, and core does *not* mark fi-1 failed. Restore the number
  afterwards.
- [ ] **`410` classified on the web client**: a mode switch on a node the
  client has moved to yields a real `generation_superseded`, and the client
  resumes from the new `stream.url` without charging the node. Expected to
  pass: core reports the web client does not walk and does not mark the node,
  because `isSourceGoneStatus` asks
  `playbackFailureKindForStatus(status) === 'not-found'` rather than testing a
  status list, so the HLS and Direct Play read-ahead paths both read `410` as
  "ask for a new generation", and `isHlsNetworkDegradation` keeps it out of
  node-health evidence. 78 tests green. This observes it rather than
  re-deriving it.
- [ ] **`410` in front of expo-video on a real device.** Nobody has done this;
  core's mobile verdict is reasoned from disassembled media3 bytecode and
  client code, not measured. Provoke a superseded generation on mobile and
  watch what the player does with it. This is also the cheapest chance to
  learn something about the unexplained A85 `500`-fatal-on-first-occurrence
  contradiction, since it exercises the same error path.
- [ ] **Watch whether mobile charges a healthy node.** Its failover is
  status-blind and records the failure against ranking on any playback error.
  Expected to be unchanged from today, not improved — confirm it is not worse,
  and size the client fix from what is seen.
- **Two routing notes for whoever drives this** (core, 2026-09-21):
  - **Do not try to reach the account cap through mobile's failover path.**
    The cap-on-failover accounting that client built is bookkeeping for a
    recovery that does not arrive, so that route shows nothing. The
    **creation** path is where the cap is observable from mobile.
  - **If an item needs a working failover observed on a real device, use the
    television, not the phone.**
- [ ] **`DELETE` scope.** Deleting one of two sessions leaves the other
  streaming.
- [ ] **A85 Direct Play**: expect "plays, no sound"; it is not this release.
- [ ] When the above hold, move the resource P0 to `COMPLETED.md` and close
  the seamless-handover items it absorbed.

## What this plan does not cover

The metadata-stall, rejoin-cache, loader-I/O and other P0s are untouched by
this release and stay where they are in `ACTIVE.md`. The cutover's brief
read-only window is a consequence of the cluster's standing lack of
redundancy margin, not of this change.

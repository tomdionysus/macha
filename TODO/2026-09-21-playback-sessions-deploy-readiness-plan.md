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
- [ ] **Full suite on es-1 with the log kept. BLOCKED.** The rsync to
  `root@10.34.1.50` was refused by the harness as a production action, so the
  tree has not reached a node. Needs the operator to allow it or to run the
  sync. Then `cmake --build build -j3` and
  `./build/macha-tests 2>&1 | tee /tmp/suite-0.48.0.log`, and note the count
  here. Laptop result so far: **484/484**, which includes the new
  generation-status case and, unusually, the `storage_v18` durability-barrier
  case that normally fails on macOS only.
- [x] **Commit on `develop`** — `60d794e`. `README.md` was left unstaged; its
  change is unrelated to this release.
- [ ] **Tag `0.48.0`** once the es-1 suite is green, so the tag names a tree
  verified on the hardware it ships to.

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
- [ ] **Mobile cannot classify a fragment status at all, and this release
  does not cause that.** Core reports macha-client-rn has no
  `playbackFailureKindForStatus` and no httpStatus plumbing at the player
  layer; playback errors arrive through expo-video's `statusChange` as
  `{status, error}` with a message string and no code. Its only status logic
  is session creation (`policy.ts:125`). So a superseded generation reaches
  mobile opaque — but it reaches it opaque **today**, as a `404`, for the same
  reason. **This does not hold `410` back** (see the decision above): it is
  client work to be scheduled, not a reason to ship a lesser server. Asked of
  core 2026-09-21 and unanswered at the time of writing: whether `410` travels
  any differently from `404` down that path. If it does, mobile gets worse at
  cutover and that goes in front of the operator beforehand as an accepted
  cost rather than a surprise a viewer finds.

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
    file on one machine. It is also already behind core by five commits,
    including the 30 s to 10 s floor change, so its hash will move again.
  - Therefore a **fresh build from develop against current core** is what
    should reach the nodes, not that artefact. Owner to be confirmed with
    core; **not the server session's to build or deploy.**
- [ ] **Config on all three nodes** per decision 2: `max_sessions` raised,
  `max_sessions_per_account` written explicitly rather than left to the
  default, so the two numbers sit together in the file. Back up each as
  `macha.yaml.bak-0.47.0` first. These reload live and can go in before the
  cutover; a 0.47.0 binary ignores the unknown key. Confirm that with a
  `reload_config` on one node before touching the other two.
- [ ] **Confirm the tarball targets**: `uname -m` and `ldd --version` on
  fi-1 and gbni-1 match es-1 (`aarch64`, glibc 2.41). They did for 0.47.0;
  check anyway.

## Phase D: cutover (after A, B and C are all ticked)

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
- [ ] **The account cap, observed.** Drop `max_sessions_per_account` to `2`
  on fi-1 alone, live reload, and have one client create three: the third is
  `429 account_session_limit` with limit and count, a second account on the
  same node is unaffected, and core does *not* mark fi-1 failed. Restore the
  number afterwards. This is the only way to see the refusal that decision 2
  otherwise makes unreachable.
- [ ] **`410` classified** (if decision 1 is yes): a mode switch on a node
  the client has moved to, watched from the web client, yields a real
  `generation_superseded` and the client resumes from the new `stream.url`
  without charging the node.
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

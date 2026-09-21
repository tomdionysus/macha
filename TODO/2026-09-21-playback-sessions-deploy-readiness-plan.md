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
is bundled and shipped; (2) the example config and reference pair 64 node-wide
with 32 per account, and the live nodes still need raising; (3) the
`playback/status` disclosure is accepted and stated in the 0.48.0 changelog.

1. **`410 generation_superseded`: bundle it, or hold it for a second flag
   day?** The code still answers `404` (`src/playback.cpp:2073`) with the
   held-pending-tolerance note. The resource plan says bundle; the deploy
   checklist expects to observe a real `410` in the joint test, which the
   current code cannot produce. *Recommendation: bundle.* Gate 2 below has to
   be met for this release anyway, and it is the release that already breaks
   every client on purpose.
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

None of this is the server session's to do. It is listed so the gate is
visible and so the brief goes out with the right content.

- [ ] **Brief the four client sessions** from the rewritten
  `docs/streaming.md`, before anything is published. The brief must name:
  the route break and that there is no dual-serve window; that `POST` creates
  every time and a client wanting the old behaviour must `DELETE` first;
  that a lost id is recovered from the collection `GET`; the cap, its
  `account` scope, and that adoption through the listing spends budget; that
  every node's cap limit is on `/api/v1/status`; `410` if decision 1 is yes;
  and the expected A85 "plays, no sound" on Direct Play, which predates all
  of this.
- [ ] **The clients are consuming a core that carries the `410` tolerance.**
  **Not a registry release — the clients test against a direct link to the
  package** (operator, 2026-09-21). Do not treat "not on npm" as a gate, do
  not wait on a publish, and do not quote registry versions as evidence of
  anything: the only question is whether each client is on a core with the
  tolerance in it.
- [ ] **The web client builds a bundle against that core** with its own
  `410` branch in the segment classifier (the plan records that hls.js
  raises the status one layer below core).
- [ ] **Android TV and mobile take the release.** Both need at least the
  route change; the cap semantics matter most to Android TV (the household
  objection came from there).

## Phase C: cluster preparation (can overlap Phase B)

- [ ] **Web bundle to `/etc/macha/web` on all three nodes.** All three
  serve `index-BGrNH6KR.js` today. Not the server session's to deploy, but
  it is the first line of the deploy checklist and the cutover does not
  start without it. Verify with a fetch of `index.html` on each node and a
  grep of the referenced bundle for the `410` handling.
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

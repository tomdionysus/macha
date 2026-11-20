# Active tasks

Last updated: 2026-11-19 about 22:40Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-19b-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md`,
`COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The goal:** Macha's store moved onto Dagda (on Dannan) on both live nodes, live-a and live-b, working as well
as it did; the only work until it is done. Functional fidelity first: the API stays 0.94.2's, Dannan's and Dagda's
native forms later. The plan is [`2026-11-18-cutover-runbook.md`](2026-11-18-cutover-runbook.md) (rulings, gates,
rehearsals, downtime, the steps, rollback); the port's history
[`2026-11-14-port-rewrites.md`](2026-11-14-port-rewrites.md); done work in `COMPLETED.md`.

**Do not start the cutover. Nothing touches live-a or live-b without the maintainer's word.**

**The short file is fixed** (`b0b037c`, pushed; [`2026-11-19-short-file.md`](2026-11-19-short-file.md)). The
mechanism was proven by a deterministic failing test, and the fix follows the maintainer's rulings (2026-11-19, interactive): both
layers, "never go backwards, and guard FUSE". Suite 547/547 on Dagda 0.3.1.

**0.95.1 is cut** (`2fb804e`, on Dagda 0.3.2 and Dannan 0.9.2, 547/547). The tarball is
`build/release-0.95.1/macha-0.95.1-arm64.tgz` (sha256 `dd25f12c...6416`). The kit is updated: `build/install-0.95.1.sh`,
`build/cutover/migrate-0.95.1.sh`, and `build/rehearsal/*` now say 0.95.1.

**Rehearsals paused (2026-11-20 01:10Z).** R1's fresh single 0.94.2 node on build-3 committed `r1/big/f0.bin` short,
with no peer (see the short-file note). **The maintainer: "this is a Dagda problem. Assist Dagda to diagnose and fix it."**
- Dagda leads; Macha assists with builds and runs. R1's evidence is in `build/evidence/r1-0942.tgz` (sha256
  `5285baeb...5233`: state, journal, sums, listing, FUSE journal, yaml, rehearsal key). The node itself was cleaned
  under the maintainer's rule "You may NOT hold cluster cores idle for any reason. Copy evidence off if needed."
- f0's history (2a): created at gen 4, published once at gen 7 with 23 of its 24 extents, never again. This matches
  the stale-snapshot detach.
- **0.95.1 also commits short** (round 8 of the measurement, `f18.bin`, 2026-11-20 01:55Z; the retire line
  fired). A second way in, probably the reverse race: the view holds the inode's next commit before its base does.
  The evidence is in `build/evidence/r8-0951.tgz`, and Dagda has it. 0.95.1 is not cutover-ready.
- The rehearsals R1 to R4 and the two-writer round restart when Dagda's fix lands; the cutover is on the maintainer's word.

**GitHub CI (the maintainer 2026-11-20: "If you have public github repos, fix and maintain github CI"):**
- `main` is green at 0.94.2.
- `develop` has been red since `7f88fe6` (2026-11-13): its ASan stage crashes in
  `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node` (an ASan thread-registry CHECK,
  signal 6 or 11, every run), and `test_failed_convergence_retries_once_per_burst_of_events` failed once. That is the
  heap corruption parked "port first".
- CI runs only on `main`, `develop` and pull requests, so the port branch has had none. Its stale Dagda pin is fixed
  (0.1.5 to 0.3.2, `e7d4503`).
- Plan, settled by the maintainer's rules: before the port's squash onto `develop`, run `tools/ci.sh --local` (with ASan) once on
  a Pi as the pre-merge gate. If the ingest/torrent crash is still there on the port's code, the heap corruption is
  fixed before the merge. Then `develop` goes green on GitHub and stays green.

**Coverage (the maintainer 2026-11-20: "You will maintain ~100% code coverage on all code"):** every change brings tests
reaching its lines. The coverage build runs at each branch's pre-merge gate. Macha's whole-tree figure is not yet
measured: take it at the port's pre-merge gate, then close the gaps.

**GoogleTest (the maintainer 2026-11-20: "GoogleTest is our standard test framework, and you must move all tests to it"):**
Macha's 576 `MACHA_TEST` cases in 39 files move on their own branch after the cutover.
- Keep one process per case, parallel slots, the filter, failure logs, the timeouts and the fixtures.
- GoogleTest comes from Dannan's `dannan::testing` export.
- The case count matches before and after.

**The stack audits (2026-11-20):** the reports are in `2026-11-20-duplication-audit.md` and
`2026-11-20-no-special-node-audit.md`. The converged plan is the coordinating session's `TODO/2026-11-20-cluster-stack-placement.md`
-> Macha (agreed), and all of it starts after the cutover. Negotiation goes through the coordinating session.

**Done 2026-11-19** (details in the runbook): Dagda 0.3.1 (the maintainer's tag, `2793458`); Macha rebuilt on it (`b4f864d`,
545/545; `idle_bandwidth_fraction` docs; tarball sha256 `5673bfc4...0002`); rehearsals R1 to R4 all pass (end to end,
kills and resume, rollback, two nodes in the maintainer's new order); the runbook's steps rewritten to that order.

**Then, on the maintainer's word:** the cutover (live-b serves 0.94.2 while live-a migrates; live-b stops just before
live-a starts on 0.95.0, then migrates and joins; then the fidelity checks: about 29 h on live-b alone, minutes
fully down, about 16 h on live-a alone); after both, each on his word, the cutover check and garbage collection, the
Macha tag, merges, pushes.

**Where it stands:** `port/dannan-dagda` pushed, untagged; 0.95.0 on Dannan 0.9.2 and Dagda 0.3.1, 545/545. Both live
nodes serve 0.94.2 (since 2026-11-19 15:08Z; what the aborted first attempt left on them is in the runbook). The
rehearsal pair (build-3 A, build-4 B) runs 0.95.0 holding the short files as evidence, claims at 0 cores to
2026-11-20 22:34Z; kit `build/rehearsal/` (gitignored), on each Pi `/root/rehearsal/` (`clean.sh` removes everything
the rehearsals installed; build-4 also got FFmpeg's runtime libraries, which stay).

**Rulings in force** (the maintainer, interactive; the cutover's own are at the head of the runbook; the rest in "Standing
rules"):
- The API stays 0.94.2's; native forms come after Macha's functional fidelity is shown, each intent left as a code
  comment ([`2026-11-18-port-api-announcement.md`](2026-11-18-port-api-announcement.md)). Tombstone timing matches
  `main` (repair on the pass's clock). Dagda's read asks the other active peers (option A).
- Fix forward, each mechanism proved before a change; a Dannan or Dagda cause goes to its session with the
  mechanism and waits for its gated fix. A library's release is that library's and the maintainer's: Dagda acts on the maintainer's word in
  its own session only, never on a relay.
- Macha's reachability provider is always registered; `maintenance.collect_garbage` (default false) gates only the
  sweeps; the live nodes keep it false until the cutover check.
- Writes name `ack_level` `persisted`; agreement waits for Dannan; no shims: Macha calls `dagda::Context`, the root
  keeps `dagda::Node`'s owner surface. Dannan 0.9.2 for the cutover.

**The libraries (read their repositories before relying on this):**
- **Dannan** 0.9.2 tagged. Custody by `dannan-custody(1)`; `dannan-import-macha --dry-run` is a check at the cutover.
- **Dagda** 0.3.1 tagged and pushed (`2793458`); Macha builds on it. Dagda holds off the live nodes until the
  cutover is reported done. Dagda's heads view can lack a node's own commit while several heads exist (its finding,
  2026-11-19); Macha does not use heads, so it does not touch 0.95.0; its `covers()` helper is on a Dagda branch,
  uncommitted, and Dagda withdrew its repin recommendation to the maintainer.

**Builds:** the Pis were wiped 2026-11-18 and rebuilt; Macha's packages are on build-1, build-2 and build-3; installing
tooling needs only a claim. build-4 is light work only. build-3: `/home/maintainer/deps/prefix-0.9.2`,
`prefix-dagda-053ee6e`, `prefix-dagda-053ee6e-read`, `prefix-dagda-0.3.0`, `prefix-dagda-0.3.1`, the tree
`/home/maintainer/macha-store-2` (built on 0.3.1). `fuse_frontend.cpp` builds into `libmacha_core.so`, not the FUSE plugin.
build-1: `prefix-0.5.3`, `prefix-0.9.2`, `/home/maintainer/macha-main` (0.94.2, clean, all targets). Shared machines:
check what is there before relying on it. Scripts in `build/pi/`: `macha_port_compile.sh` (`S`, `R`, `J`, `DANNAN`,
`DAGDA`; writes only `$R.result`, so capture its output to see an error), `deps_dagda_tag.sh`, `main_0942_build.sh`
(`TARGET=all` for the daemon), `macha_package.sh`. Smoke-test kit: `build/smoke/`. Cutover scripts and disk
measurement: `build/cutover/`. Sync with the rsync line below, then format changed files with `clang-format-19` on
the Pi and copy them back; **rsync never deletes**. Tests: `build-port/macha-tests --jobs 3`, or `--serial
[--verbose] --filter NAME` (`--verbose` shows a passing case's output; `--repeat N` measures a flake);
`MACHA_TEST_LOG_LEVEL=DEBUG` or `ALL`. **Tee every result into a log on the Pi**: a background command's output lands
outside the boundary. Claim first, wait for "taken", then build; release when done. Temporary diagnostics go on the
Pi's copy only, never into Macha; the next rsync with `--checksum` removes them. A Macha mount point is immutable
while unmounted (`chattr -i` before removing a test node's directory).

## Known live defects until the cutover

All stay on 0.94.2 and are fixed by the port, except the first.
- **A file written through the mount can commit short, silently** (two nodes; `2026-11-19-short-file.md`). Not fixed
  by the port: the FUSE frontend is the same code on 0.95.0.
- **Repair** (develop `distributed_store.cpp`): push and pull share one 16-call step budget; a large rejoin took about
  80 s where the bytes once take about 38 s (2026-11-16). Nothing is lost. Dagda's redesign replaces it.
- **A loader refused for device pressure waits for the next release** (develop `data_work.hpp:239-247`). Fixed in
  Dagda `053ee6e`.
- **Durable writes into new directories** (`src/durable_file.cpp`): a crash just after the write can lose a
  directory it made. Fixed on the port branch (`46c61ba`).

## Waiting on the operator

- The short file: the mechanism report once proved, its fix, and whether the cutover waits on it; then the word to
  start the cutover.
- Dagda's and Dannan's branches pushed (both local on the maintainer's word).
- After the port: whether the heap chase resumes with media load.
- Carried, unchecked: the media-kind import hint; truncated pack records; the `*.pre-migration.*` archives; the
  replica journal reader with no writer left.

## Open, in order (Macha, after the cutover)

1. **The heap corruption** (parked by the operator, 2026-11-14: port first; findings in
   `archive/2026-11-15-ACTIVE-before-rationalisation.md`, item 1) and the FUSE `rm` crash (found-issues 1, 2).
2. **The deferred API intents** (`2026-11-18-port-api-announcement.md`), once fidelity is shown and the maintainer says.
3. **A full control store is silent; work that cannot complete is retried without backoff** (found-issues 3).
4. **Cancelling an ingest job leaves its worker running** (found-issues 4).
5. **The stop**: the release horizon's census (`namespace_references`) and the tree diff under `census_follow` do
   not check it.
6. **Import filing**: about 430 episodes filed as films in `/Movies`; names such as `Blade Runner ( (1982)`
   (found-issues 9).
7. **Books** as the web client reported at 0.92.2 (series counts, `published` types and nulls, split authors).
8. The genesis root-password file (`auth/users.cpp`, `initial-root-password`) is written with no fsync.
9. The rest of found-issues, items 5 to 17.
The cases once on the failing list (`availability` two, `two_node_mutual_bootstrap`, `coalesced_delete_burst`,
`accounts_converge`) all pass on the port; a recurrence is P0.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption.

## Cluster state (read 2026-11-19 00:30Z)

- **Both on 0.94.2**, health ok, running since 2026-11-13 21:47Z (live-a) and 21:10Z (live-b); rollback
  `/root/pre-0.94.2/`. DATA: live-a 5.9T of 9.1T, live-b 3.3T of 9.1T. `/var/lib/macha` 5.5G and 7.7G; 858G free
  on each NVMe root.
- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a, hostname `node-c`): `/usr/local`, `/var/lib/macha`, DATA
  `/mnt/diskB/data`, control store limit 8G, API 192.0.2.50:7438, advertises `live-a.example.net`,
  `bootstrap: []`. Never build there.
- **live-b** (root@live-b.example.net, Pi 5, 16 GB, site-b, inbound capable at :7437): DATA `/mnt/diskB/data`;
  API 203.0.113.50:7438 (inside only); advertises `live-b.example.net`, bootstrap live-a; power failures recur,
  no remote power control. Never build there.
- **Both:** systemd 257, no TPM, no `credential.secret` yet; `maintenance.idle_bandwidth_fraction` 0.5, every other
  maintenance key at its default; `dht` replicas 2, write_copies 1, metadata_write_copies 1, extent 4M; packing 1M
  into 64M; heartbeat 5 s, `telemetry_interval_ms` 10000, `dead_after_ms` 30000; config backups beside
  `/etc/macha/macha.yaml`; cores to `/mnt/diskB/crash`; torrent on.
- **Access:** the API needs a session; live-a's genesis root password is in `/var/lib/macha/initial-root-password`
  (works on live-b too); scripts that read it are in `build/baselines/`.
- **Syncing a tree to a Pi:** `rsync -rlp --checksum --exclude '/build*' --exclude /.git --exclude CLAUDE.md
  --exclude CLAUDE.local.md`; no `-t`, no `--delete`.

## The shared build Pis

build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202`, build-4 `.228` (light work
only): Pi 5, 16 GB, Debian 13 arm64, shared by every session. No builds on the Mac. Claim cores on the board (claim,
gate on "Taken"; `claim-board wait ... --for 90m` as a background command when none are free; release when the job
ends). Installing tooling needs only a claim.

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou Shalt Not Make Control Wait. 2. Thou
  Shalt Not Make The Viewer Wait. 3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer
  Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Everything the API and the web serve is viewer work**, including the bytes still draining to a client.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.**
- **No vacuous truths.** An empty set never satisfies "all" or "enough".
- **Three answers, HTTP numbers below as above**; refusals carry an HTTP status and a snake_case code; nothing
  matches English text.
- **Proportion.** Account for and control what matters to the user's experience.
- **Sanitizers**: ASan, UBSan and TSan only for a release or to hunt a named bug, never as a routine gate.
- **Testing**: test the change once on a Pi; no repeat or soak runs except to measure a suspected flake.
- **Report findings before fixing; prove a mechanism before editing.** Questions and design decisions go to the
  operator through the interactive question dialog, with a recommendation first.
- **Every response has a snake_case code; errors add a message; clients own sorting and presentation; every API
  change is announced to Core and every client before it ships** (none in 0.95.0).
- **A Macha defect the port finds is fixed in Macha first**; the libraries match the fix. A Macha test retires only
  against a named library case, recorded case by case.
- **One definition per concept**: a type Dannan or Dagda defines is aliased, never twinned.
- **Agreement is Dannan's**: Macha builds no consensus, no lease standing in for one, and no workaround meanwhile.
- **Peer sessions** ask and report over cross-session messages: answer from code and nodes, say what was not checked.
- **Builds** only on the Pis, -j2 or -j3, nice 19, claimed. Never edit sources under a running build of the same
  tree. Format with clang-format 19; clang-tidy reports nothing; a suppression is a `NOLINTNEXTLINE(check)` with its
  reason beside it.
- **Git**: commit on the branch as work lands; push `port/dannan-dagda` when needed, anything else only on his word;
  no tag, merge or PR unasked; stage named paths, never `-A`; no attribution trailers; no em dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files.
  Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

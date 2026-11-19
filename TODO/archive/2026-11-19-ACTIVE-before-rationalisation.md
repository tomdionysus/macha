# Active tasks

Last updated: 2026-11-18 about 19:00Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-18-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md`,
`COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The port: Macha onto Dannan and Dagda, the only work until the cutover.** the maintainer, 2026-11-18: "Macha, Dannan, Dagda -
work together and focus on the Macha move to Dagda. Work on nothing else until this is done. Work autonomously until
you're ready to do the cutover on live-a and live-b", and "This is your port, so we drive it from here." The
plan's history is [`2026-11-14-port-rewrites.md`](2026-11-14-port-rewrites.md); the cutover's steps are
[`2026-11-18-cutover-runbook.md`](2026-11-18-cutover-runbook.md); what was done is in `COMPLETED.md`.

**Where it stands (2026-11-18 19:00Z):** `port/dannan-dagda`, pushed. Steps 1 to 4 are done; step 5 (the suite) is
at four failures. Builds on **Dannan 0.9.2** (tag) and **Dagda `053ee6e`** (`feature/macha-store-gaps`, local in
Dagda's repository, not pushed, no tag). The last valid whole-suite run (build-3, Dagda `7b2a7be`, Macha `7b259bd`):
**540 passed, 4 failed.**

**A run was in progress at the clear** (build-3, started 18:50Z, claim held until 20:37Z): the compile on Dagda
`053ee6e` with the parked-pass change, then the whole suite. Read it first: `/root/pi/port2.result` (must be later
than 18:50Z and `rc=0`; at 19:00Z it still read `rc=0 17:40:31`, the build at 26%), then `/root/pi/suite8.log`,
which counts only if it is newer than that result (the log of 18:51Z, 539 and 5, is a stale binary's). If the
session's background command died with the clear, run the suite by hand. **Release the build-3 claim** when done.

**2026-11-18 23:40Z: the suite is green.** On Dannan 0.9.2 and Dagda `053ee6e`: 545/545 (suite13). On Dagda `053ee6e`
plus Dagda's gated, uncommitted read fix (`../dagda/build/read-asks-active-peers.patch`, sha256 `b3187110...c71`,
The maintainer chose option A): 545/545 (suite14) and the catalogue case 20/20 serially. Release commit 0.95.0 made, untagged.
Since 19:50Z: the API is 0.94.2's (the maintainer: fidelity first, native forms later, intents as code comments; record in
[`2026-11-18-port-api-announcement.md`](2026-11-18-port-api-announcement.md), nothing to announce); repair runs on the
pass's clock as `main`'s did (proved against a `main` build: the delete-burst case now counts 2 runs, as `main`).
The Pis were wiped 20:30Z and rebuilt; build-3 has `prefix-0.9.2`, `prefix-dagda-053ee6e`, `prefix-dagda-053ee6e-read`
and Macha's packages (all three build Pis, the maintainer's approval). The maintainer, 2026-11-19 00:00Z: Dagda commits and tags the fix (relayed to
Dagda); the cutover is "not yet": stop at a ready tarball and install script, schedule waits for him. Next: Dagda's
tag; `docs/install-linux.md` names its versions; the tarball by `build/pi/macha_package.sh` from that tag; then the maintainer's
decisions on the migration downtime and the cutover, live-b first.

**2026-11-18 19:50Z, after suite10 (545 cases: 538 pass, 7 fail) and per-case runs:**
- Fixed in `src/service/maintenance.cpp`, uncommitted: (a) the tombstone and garbage-batch exact wake `main` had
  (earliest retirement plus grace), lost in the port: the trace cases `tombstones_maturing`, `peer_unreachable_and_back`
  and `tombstones_live_in_batches` pass; (b) repair's quiescence now ends when the survey's lists name other work
  (`new_repair_work`, by content: inventory generation, the pull list, each peer's lacks): Macha's flag cleared only on a
  store event, so a turn that settled quiet before the survey saw a new peer's lack parked the pass for the garbage
  grace (30 days). Proved with per-node wait traces. Pointer identity (what Dagda's own `Repair::pass` uses) was tried
  first and re-ran a futile CONTROL claim walk on a survey that changed nothing
  (`trace_incomplete_catalogue_and_release_horizon`); `repair_resumes_after_yielding` 8/8 (was about 1 in 2), and `two_node_mutual_bootstrap`
  (failing since before the port) passes.
- `message_timings` keyed by registered name (`registered_type_name`, `dannan_bridge.cpp`; test in
  `test_dannan_bridge.cpp`): Dagda's new types had all merged under "unknown".
- **Sent to Dagda (19:50Z):** `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`, about
  1 in 5: s1's read of an object only s2 holds ends `ReadEnd::unavailable` at once; s1's last survey was generation 4
  with `peers=0`. Mechanism (Dagda's, proved here 19:59Z): a DATA read asks only active peers carrying the
  `dagda/extents` advert; s2 was active in s1's roster with no extents advert yet, so the read asked nobody and
  answered unavailable at once. Fix is the maintainer's choice, which Dagda is putting to him: (A) a read also asks the other
  active peers when hosting candidates are exhausted (Dagda and Macha both recommend), (B) test-only wait for adverts.
- **For the maintainer:** `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace` counts 3 convergence runs, not
  2. Proved: three unlinks are three commits, so three tombstone batches stamped about 1 ms apart; the case steps the
  clock to 1 ms before the latest batch's grace, which matures the first two, and erasing them is a commit, so an event
  and a run. The grace boundary and the collection checks pass. Not proved: whether `main` group-committed the two
  unlinks made while the convergence run was held into one batch. Question: is per-batch erasure on each batch's own
  grace right (the case's count changes), or must a burst share one batch?

**The four, as they stood at 19:00Z:**
1. `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` and
   `test_coalesced_delete_burst_wakes_at_exact_garbage_grace`: the port's pass woke every interval (an R3 interim).
   Fixed in `src/service/maintenance.cpp` (**uncommitted at the clear if the run had not finished; commit it once it
   compiles**): the deadline starts at never and is lowered only by real ones, as `main`'s pass did (metadata and
   catalogue retries, the GC quiet window, a busy class going quiet, repair's turn and credit,
   `dagda::Node::local_due()`, `availability().due()`). Unverified until the run.
2. `rpc_cluster/test_a_file_touch_commits_while_a_holder_is_away`: Dagda's defect (a short object re-placed at a
   majority, not `write_copies`, so a late copy was never claimed), fixed in Dagda `8398ec4`, in `053ee6e`.
3. `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor` (on the failing list before the port): the
   peer's copy did not arrive within 20 s of a `persisted` write. Not diagnosed; Dagda's claims fix may or may not
   bear on it. If it still fails: prove the mechanism (does prompt replication send the second copy at all?), and a
   Dagda cause goes to Dagda.
Any other failure in the run is new: diagnose, and send a library's cause to its session with the mechanism.

**Then, in order, to be ready for the cutover:**
1. The suite green; commit; push.
2. The release commit: version 0.95.0 in CMake, the README's `*v0.95.0*` line, the CHANGELOG entry (the store is
   Dagda's on Dannan 0.9.2; `46c61ba`; the API changes; `maintenance.collect_garbage`; custody required).
3. The API changes announced to Core and every client before they ship (list in `2026-11-14-port-rewrites.md`,
   "Before the flag day", plus repair's `offer_bytes` and `in_flight`, `b26fb55`). Ask the maintainer which sessions.
4. The deploy tarball built RelWithDebInfo on a claimed Pi: Macha, `libdannan` with `dannan-node`, `dannan-custody`,
   `dannan-import-macha`; `libdagda` with `dagda-migrate`. An install script from `build/install-0.94.2.sh` with the
   runbook's steps.
5. The 0.94.2 baselines ready for the comparison (`build/baselines/`, `2026-11-06-pi-baselines.md`).
6. The maintainer's decisions, asked interactively when the above is done: the Dagda tag to deploy from (Dagda proposes one
   after its release sanitizers); the migration's downtime (Dagda's estimate about 22 h for live-a, unmeasured; no
   rehearsal on a copy is possible with 5.9T, so live-b's migration is the measurement); the cutover itself,
   live-b first, never both down.

**Rulings in force for this work** (the maintainer, interactive, 2026-11-17 and 11-18; the rest are in "Standing rules"):
- Fix forward group by group, each mechanism proved before a change. **A Dagda or Dannan cause is reported to its
  session with the mechanism and waits for its gated fix**; the maintainer paused the whole port on one such (11-17) and later
  told the three sessions to work together without stopping (11-18), so carry on with Macha's own items meanwhile.
- Macha's reachability provider is always registered (repair's survey needs the inventory);
  `maintenance.collect_garbage` (default false) gates only the pass's sweeps. Only the tests about reclaiming set it.
  The live nodes keep it false until the provider's answers are checked against the namespace on both.
- Writes name `ack_level` `persisted`; agreement waits for Dannan; no shims: Macha calls `dagda::Context`, the root
  keeps `dagda::Node`'s owner surface.
- Dannan 0.9.2 for the cutover (Macha's decision on Dannan's report: 0.8.5 has a ledger double free and the
  directory-sync loss).
- `offer_bytes` and `in_flight` are in Status's repair section (announce).

**The libraries (2026-11-18 19:00Z; read their repositories before relying on this):**
- **Dannan** 0.9.2 tagged; `CallOutcome::peer` is a pointer. Its custody procedure and state file names are in the
  runbook. `dannan-import-macha --dry-run` is a check at the cutover, not a conversion.
- **Dagda** `feature/macha-store-gaps` at `053ee6e` on Dannan 0.9.2, gated 1152 ran, 1149 passed, 3 skipped. Since
  `d119d14`: the state layout beside its host's files, repair stage 4 (a window of transfers, `Repair::drain`,
  `get_objects` 773/774), a failed plane marked as it fails, a stop waiting for an open, claims placed on
  `write_copies` holders, `Node::local_due()`, the arbiter's pressure wait woken when its refusal can end. Open on
  Dagda's side: stage 4's unpaced large rejoin is slower than stage 3's (18 to 25 s against 15 to 16 s for
  26 x 4 MiB), so no large-object repair comparison is settled; its release sanitizers and the tag.
- Both sessions answer cross-session messages at once and have the maintainer's word to work on nothing else.

**Builds:** build-3 `/home/maintainer/macha-store-2`; `S=/home/maintainer/macha-store-2 R=/root/pi/port2 J=3 sh
/root/pi/macha_port_compile.sh` (source `build/pi/macha_port_compile.sh`; defaults `prefix-0.9.2` and
`prefix-dagda-053ee6e` under `/home/maintainer/deps`; a new Dagda commit needs a new prefix, as
`build/pi/deps_dagda_053ee6e.sh`, from `git -C ../dagda archive`). build-2 has an older tree and prefixes
(`/home/maintainer/macha-port`, Dannan 0.8.5). Sync with the rsync line below, then format changed files with
`clang-format-19` on the Pi and copy them back; **rsync never deletes, so remove deleted files on the Pi by hand**.
Tests: `build-port/macha-tests --jobs 3`, or `--serial --filter NAME`; `MACHA_TEST_LOG_LEVEL=DEBUG` for the log.
A crash: `gdb -batch -ex 'set follow-fork-mode child' -ex run -ex 'thread apply all bt 12' --args ...`. Claim
first; release when done. Edit scripts in `build/pi/` with the Write tool, never by reading and writing one path in
a single Python expression (that emptied the compile script once and a suite ran on a stale binary).

**Parked by the operator (2026-11-14: port first, heap later):** the heap corruption P0. Findings: 125 ASan-clean
rounds of the crash workload; the FUSE read late-write found and fixed by R2's reply buffer (`01eecf0`); libfuse's
low-level API after the port (BACKLOG). Detail in `archive/2026-11-15-ACTIVE-before-rationalisation.md`, item 1.

## Known live defects until the cutover

All are left on 0.94.2 and fixed through the port (the maintainer's rulings for the first and last).
- **Repair** (develop `distributed_store.cpp`): push and pull share one 16-call step budget, so a sending push holds
  a new pull list back, and a credit-limited push skips the pull for its step; on a rejoin the holder pushes while
  the joiner pulls the same objects. Measured 2026-11-16 (Dagda's harness, workload 7, two Pis on WiFi at about
  2.9 MB/s, a tenth of the plan's sizes): a large rejoin took about 80 s where the bytes sent once take about 38 s;
  small objects are bound by the call budget (410 objects in 38.9 s); a viewer on the holder was not slowed. Nothing
  is lost. The maintainer, 2026-11-16: no patch; Dagda's redesign replaces it (stages 1 to 4 built).
- **A loader refused for device pressure waits for the next release** (develop `data_work.hpp:239-247`): the
  refusal ends with time (the viewer goes quiet, the pressure eases), but nothing wakes the waiter, so a loader
  yielding to a viewer on a slow disk resumes only at the next DATA release, unbounded when the holders are long-lived.
  Found by Dagda 2026-11-18 (its copy of the arbiter); Macha's arbiter is gone on the port branch; fixed in Dagda
  `053ee6e`.
- **Durable writes into new directories** (`src/durable_file.cpp`): a crash just after the write returns can lose a
  directory it made, and the file in it. Fixed on the port branch (`46c61ba`).

## Waiting on the operator

- The Dagda tag for the port's commits (`0.2.0` is held locally and lacks them); CI and the deploy wait on it.
- Dagda's and Dannan's branches pushed (both local on the maintainer's word).
- After the port: whether the heap chase resumes with media load.
- The Codecov token for the repository.
- Carried, unchecked: the media-kind import hint; truncated pack records; the `*.pre-migration.*` archives; the
  replica journal reader with no writer left.

## Open, in order (Macha, after the port's items)

1. **The heap corruption** (above) and the FUSE `rm` crash (found-issues 1, 2).
2. **A full control store is silent; work that cannot complete is retried without backoff** (found-issues 3).
3. **Cancelling an ingest job leaves its worker running** (found-issues 4).
4. **The stop**: the release horizon's census (`namespace_references`) and the tree diff under `census_follow` do
   not check it.
5. **Import filing**: about 430 episodes filed as films in `/Movies`; names such as `Blade Runner ( (1982)`
   (found-issues 9).
6. **Books** as the web client reported at 0.92.2 (series counts, `published` types and nulls, split authors).
7. **Failing tests, each P0 when it recurs** (not rechecked on the current nodes):
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`,
   `availability/test_two_nodes_survey_what_neither_holds`,
   `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor`,
   `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
   `users/test_accounts_converge_across_nodes`.
8. The genesis root-password file (`auth/users.cpp`, `initial-root-password`) is written with no fsync.
9. The rest of found-issues, items 5 to 17.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption.

## Cluster state (read 2026-11-16 16:05Z; configs read 2026-11-16)

- **Both on 0.94.2** since 2026-11-13 (`replicas=2/2`), RelWithDebInfo, tarball sha256 `b6d1b65a...0d95`, installed by
  `build/install-0.94.2.sh`; rollback `/root/pre-0.94.2/`. Health `ok`, `libdannan.so.0.5.3`; running since
  2026-11-13 21:47Z (live-a) and 21:10Z (live-b); no exit, core or reopen-on-sight line since. live-b's one SEGV
  (2026-11-13 21:01:23Z, core in `/mnt/diskB/crash`) is logged one second after systemd began stopping the service.
  DATA: live-a 5.9T of 9.1T, live-b 2.6T of 9.1T.
- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a): `/usr/local`, `/var/lib/macha`, DATA `/mnt/diskB/data`, control
  store limit 8G, API 192.0.2.50:7438, listens 192.0.2.50, advertises `live-a.example.net`, `bootstrap: []`. Never
  build there.
- **live-b** (root@live-b.example.net, Pi 5, 16 GB, site-b, inbound capable at :7437): DATA `/mnt/diskB/data`;
  API 203.0.113.50:7438 (inside only); listens 0.0.0.0, advertises `live-b.example.net`, bootstrap live-a;
  power failures recur, no remote power control. Never build there.
- **Both:** `maintenance.idle_bandwidth_fraction` 0.5, every other maintenance key at its default; `dht` replicas 2,
  write_copies 1, metadata_write_copies 1, extent 4M; packing 1M into 64M; heartbeat 5 s, `telemetry_interval_ms`
  10000, `dead_after_ms` 30000; config backups beside `/etc/macha/macha.yaml`; cores to `/mnt/diskB/crash`; torrent
  on; one tree-backed head, SM19.
- **Access:** the API needs a session; live-a's genesis root password is in `/var/lib/macha/initial-root-password`
  (works on live-b too); scripts that read it are in `build/baselines/`.
- **Deploy (operator's word each time):** build on a claimed Pi with the log and `$?` in a file; `DESTDIR=/root/stage-V
  cmake --install build --prefix /usr/local`; on each node in turn `/root/stop-with-stacks.sh`, then an install
  script from `build/install-0.94.2.sh` (saves `/root/pre-V/`, rolls back if not healthy in 180 s). Never both nodes
  down at once.
- **Syncing a tree to a Pi:** `rsync -rlp --checksum --exclude '/build*' --exclude /.git --exclude CLAUDE.md
  --exclude CLAUDE.local.md`; no `-t`, no `--delete`.

## The shared build Pis

build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202`, build-4 `.228`: Pi 5, 16 GB,
Debian 13 arm64, shared by every session. No builds on the Mac. Claim cores on the board (claim, gate on "Taken";
`claim-board wait ... --for 90m` as a background command when none are free; release when the job ends). Macha's
state there: build-3 `/home/maintainer/deps`, `/home/maintainer/macha-store`, `/home/maintainer/macha-store-2`, `/home/maintainer/macha-asan`,
`/root/pi`, `/root/heap*`; build-2 `/home/maintainer/macha-asan`, `/home/maintainer/heap`, `/root/heap*`.

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
- **Sanitizers** (the maintainer, relayed 2026-11-17): ASan, UBSan and TSan only for a release or to hunt a named bug, never
  as a routine gate.
- **Testing**: test the change once on a Pi (the suites there are the gate; `tools/ci.sh` is the merge gate, ASan
  and UBSan included); no repeat or soak runs; ASan and TSan for releases only, otherwise only to hunt a named bug.
- **Report findings before fixing; prove a mechanism before editing.** Questions and design decisions go to the
  operator through the interactive question dialog, with a recommendation first.
- **Every response has a snake_case code; errors add a message; clients own sorting and presentation; every API
  change is announced to Core and every client before it ships.**
- **A Macha defect the port finds is fixed in Macha first** (on the port branch where the operator rules so); the
  libraries match the fix. A Macha test retires only against a named library case, recorded case by case.
- **One definition per concept**: a type Dannan or Dagda defines is aliased, never twinned.
- **Agreement is Dannan's**: Macha builds no consensus, no lease standing in for one, and no workaround meanwhile.
- **Peer sessions** (Dagda, Dannan, Lir and others) ask and report over cross-session messages: answer from code and
  nodes, say what was not checked, and change Macha's code only on the operator's word (for the port he has given
  it: the three sessions work together).
- **Builds** only on the Pis, -j2 or -j3, nice 19, claimed. Never edit sources under a running build of the same
  tree. Format with clang-format 19; clang-tidy reports nothing; a suppression is a `NOLINTNEXTLINE(check)` with its
  reason beside it.
- **Git**: commit on the branch as work lands; push `port/dannan-dagda` when needed (operator 2026-11-16), anything
  else only on his word; no tag, merge or PR unasked; stage named paths, never `-A`; no attribution trailers; no em
  dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files, so
  Python for text work. Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

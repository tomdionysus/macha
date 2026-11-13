# Active tasks

Last updated: 2026-11-12, on `port/dannan-dagda`. **State a branch's content from its diff and a node's state from
its files, never from a prior note** (operator: "We can't afford drift like this."). The version before this
rationalisation is `archive/2026-11-12-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md` (everything
not in progress, by area), `COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The work is the port onto Dannan and Dagda.** The first cutover, the transport, is done and live: both nodes run
0.94.0 (Dannan 0.5.3 beneath Macha, the old transport removed). The build log, the rulings in the operator's words
and what the second cutover must do are in [`2026-11-08-port.md`](2026-11-08-port.md). Issues found on the way and
their owners: [`2026-11-08-found-issues.md`](2026-11-08-found-issues.md).

**Where the branch is.** `port/dannan-dagda` at `0fefebe`, 26 commits past `develop` (`7efe943`, 0.93.6). Pushed to
`604e276`; **four commits are local and unpushed** because GitHub CI would go red at their ASan step (below):
`ba173f3` the CI kit, `08d7aa4` the clang-format pass, `ffc908c` and `0fefebe` the clang-tidy clean-up. 0.94.0 is
`f28414e`, untagged; the last tag is 0.93.4.

**This session's job, in order:**
1. **The playback start leak** (operator's go pending): the new ASan gate found that every async transcode start
   leaks its candidate session, since `63642c8` (2026-09-28). Defect, checked plan and verification in
   [`2026-11-12-playback-start-leak.md`](2026-11-12-playback-start-leak.md). One Macha commit on the branch.
2. **Finish the CI kit** (operator: "badges, github actions CI, the whole kit and caboodle"; reformat all then gate;
   clang-tidy gated and everything fixed; he adds the Codecov token). Done: `.github/workflows/ci.yml` and
   `codeql.yml` (Debian trixie, Dannan built from tag 0.5.3), `tools/ci.sh` (format, SPDX, the default preset with
   every test binary, clang-tidy 19 with warnings as errors, ASan and UBSan, coverage), `tools/check-format.sh`,
   `tools/check-spdx.sh`, `.clang-tidy`, `.editorconfig`, `CMakePresets.json` (default, asan, coverage), the
   `coverage` target, README badges, CONTRIBUTING's notes. On a fresh clone of `0fefebe` in the local image
   `macha-ci:trixie` (`build/ci-image/Dockerfile`; a run is `git clone /src /work` with `/src` read-only and output
   under `build/ci-out/`; about 75 min, clang-tidy 50 of them, so background it): format, SPDX, build, 713 + 20 + 21
   and clang-tidy pass; ASan fails on the leak above; coverage did not run. After the leak fix: the full
   `tools/ci.sh` green on a fresh clone, then push.
3. **Release 0.94.1**: CHANGELOG, CMake and README version; annotated tag `0.94.0` on `f28414e` and `0.94.1` on the
   release commit; fast-forward `develop` to the branch and `main` to `develop`; push with tags; build on macnessa,
   ship the tarball, inverbeg first.
4. **Macha as a Debian package on Dannan 0.5.6** (operator, 2026-11-13: "Yes, cut 0.5.6"; Macha gets its own deb,
   and libdannan comes in as a package dependency, not in the tarball). Dannan 0.5.6 brings `libdannan0.5.6` (SONAME
   `libdannan.so.0.5.6`), `libdannan-dev` (exact) and `dannan-tools` (`dannan-ca`, `dannan-import-macha`,
   `dannan-version`), in the tomcully.com apt repo (`../tomcully-apt`; signing and upload are the operator's).
   Macha's deb follows Manannan's pattern (debian/ with debhelper over CMake, a build-deb script versioned from git
   describe, CI with lintian, arm64 and x86-64), `Depends: libdannan0.5.6 (= exact)` and `dannan-tools`; CMake pins
   the exact Dannan version. Settle rollback under apt (the previous version reinstalled) against today's
   `/root/pre-V`. After 0.94.2 ships. **Dannan 0.5.6 is tagged and published** (2026-11-13, main `081439c`, no
   library change from 0.5.5; trixie main, amd64 and arm64): `libdannan0.5.6` (`/usr/lib/<multiarch>`, SONAME the
   whole version), `libdannan-dev` (CMake package in `/usr/lib/<multiarch>/cmake/dannan`, ExactVersion; Depends
   `libdannan0.5.6 (= 0.5.6)`, `libboost-dev (>= 1.83)`, `libssl-dev`), `dannan-tools` (with man pages), dbgsym for
   both. Macha: `find_package(dannan 0.5.6 CONFIG REQUIRED)`; Build-Depends `libdannan-dev (= 0.5.6)`; Depends from
   shlibs plus `dannan-tools`. Repo: `curl -fsSLo /etc/apt/sources.list.d/tomcully.sources
   https://tomcully.com/apt/tomcully.sources`. Nothing installed on the Pis yet: that is the operator's word.
5. **The store cutover (task 8)**: review Dagda by reading its code, then build it on the tag Dagda cuts. Dagda is on
   `feature/manifest-lock` at `c264fa2` (a 0.1.2 version commit; tags `0.1.0` and `0.1.1` exist, 0.1.2 not yet);
   `dagda-migrate` refusing a second manifest pass is the 0.1.2 change. Unproved for the cutover: macnessa's
   migration bytes and time at 6.3 TB (read rates measured: macnessa 103 MB/s, inverbeg 87 MB/s, so about 2 to
   2.5 days there and about 20 h on inverbeg). Dagda names its own deadlines; the rehearsal on corvus-develop passed.
   Decided 2026-11-13 (Dagda asked; operator: Macha's code, this session's call): `MachaLogSink::write`
   (`dannan_bridge.cpp`) writes the message alone for any `dagda.` scope, so store lines read exactly as Macha's
   (Dagda restores Macha's text and level for every line before 0.1.2); Dannan's lines keep their scope. Lands with
   the cutover, not before (operator, 2026-11-13: do not start the Dagda work).
   Dagda 0.1.2 tagged 2026-11-13 (local, on Dannan 0.5.3): manifest pass under flock, every store journal line in
   Macha's text and level (`../dagda/TODO/2026-11-12-journal-mapping.md`, section 8 the differences left), Dagda's
   open lines at DEBUG. For the facade: write " table=" on "availability surveyed" (Dagda cannot know it); fill
   Reachability's optional `entries` and `extents` from the namespace counts (DIAG maintenance-inventory); the
   provider keeps its own lines at 8a. Asked of Dagda before the review: its 15 forked-child TSan cases that pass
   only alone, and the 2 simulated crashes exiting with a thread unjoined.
6. **Status thread names** (operator, 2026-11-11): the port maps Dannan's threads to Macha's names in Status
   `threads[]`. Change every client (Core and each client session) to Dannan's real names, announced first, then
   drop the mapping.

**Peers:** the Dannan and Dagda sessions, by `SendMessage` (`ListAgents` shows them). They work on their own
repositories, `../dannan` and `../dagda`; `../macha` is read-only to them. Dannan's `develop` is at `f7bd5eb`, the
g++ warnings clean-up merged on this session's no-objection (reviewed from its description, not read); its push
waits on the operator. Macha's CI pins Dannan to tag 0.5.3 (`DANNAN_VERSION` in `ci.yml`); bump it when a tag
carries what Macha needs.

## Waiting on the operator

- **The async playback start leaks its candidate session** (2026-11-12, found by the new ASan gate; one session
  per async transcode start since `63642c8`). Defect, checked plan and verification in
  [`2026-11-12-playback-start-leak.md`](2026-11-12-playback-start-leak.md). Waiting on the go to fix it in Macha
  before the push and 0.94.1.
- **The Codecov token** for the repository (he said he will add it).
- Carried, unchecked, not now: the media-kind import hint; truncated pack records (counted read failures, report
  held, repair unchecked); the `*.pre-migration.*` archives; the replica journal reader, with no writer left.

Answered 2026-11-12: build on macnessa only, never on inverbeg (the Pi browns out); reformat the whole tree then
gate; clang-tidy gated now and everything fixed; Dannan is public.

## Open, in order (Macha, after the port's items above)

1. **The heap corruption and the FUSE `rm` crash** (found-issues 1, 2). Left until after the port. The first P0 of
   this series, a SIGSEGV on 0.92.4 at 2026-11-05 02:05:25Z, had no core; capture has been on since.
2. **A full control store is silent; work that cannot complete is retried without backoff** (found-issues 3).
3. **Cancelling an ingest job leaves its worker running** (found-issues 4).
4. **The stop.** The survey's and inventory walk's stop checks are in 0.93.4; the release horizon's census
   (`namespace_references`) and the tree diff under `census_follow` do not check it.
5. **Import filing:** about 430 real episodes probed as films and went to `/Movies`; folder names such as
   `Blade Runner ( (1982)` (found-issues 9).
6. **Books**, as the web client reported at 0.92.2: the Discworld series holds 3 of 49; three books send a
   timestamp `published`; 11 send `published` and `language` null, a possible regression, unexamined; "Le Guin,
   Ursula K." splits into two authors. Stage 3, per-user media state, waits.
7. **Failing tests, each P0 when it recurs** (carried, not rechecked on the current nodes; the three retired against
   Dannan cases in 0.94.0 are dropped):
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`,
   `availability/test_two_nodes_survey_what_neither_holds`,
   `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor`,
   `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
   `users/test_accounts_converge_across_nodes`. macOS torrent segfaults are the laptop's libraries: ignore there
   only.
8. The rest of found-issues, items 5 to 17 (8 is closed by the transport's removal; 14's `net.cpp` entries with it).

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption (now seen on macnessa, 2026-11-06).

## Cluster state (2026-11-12, read from the nodes at 22:15Z)

- **Both on 0.94.0** (`/api/v1/health` on each), service active. inverbeg 10:59Z, macnessa 11:00Z, by
  `build/install-0.94.0.sh` (untracked). Rollback per node: stop; move each of `membership/known-nodes.bin.macha`,
  `connectivity/inbound.bin.macha`, `telemetry/last-known.bin.macha` back over its name (all six files present);
  restore `/root/pre-0.94.0/` (`bin.tgz`, `macha.yaml`, `state.tgz`: 0.93.6); start. inverbeg lost power 2026-11-11
  and came back 18:34Z (up 1 day 3 h at the read); macnessa up 6 days.
- **macnessa** (root@10.44.1.50, reached locally; hostname corvus-gbni-1; Pi 5, 8 GB; domain site-a): `/usr/local`;
  `/var/lib/macha`; DATA on `/mnt/diskB/data`, 6.3 TB used; control store `/var/lib/macha/metadata-objects`, limit 8G;
  API on 10.44.1.50:7438. **The only build node** (operator, 2026-11-12: "DON'T build on inverbeg"), sources in
  `/home/tom/macha`, Dannan 0.5.3 in `/home/tom/deps/prefix-0.5.3`. NVMe SMART `FAILED` on its temperature flag only.
- **inverbeg** (root@inverbeg.macha.network, Pi 5, 16 GB, domain site-b, inbound capable, dialled at
  inverbeg.macha.network:7437): DATA on `/mnt/diskB/data`, filling from macnessa (6.0 MB/s with no viewer, paced to
  the 5% share by one); control store limit 8G; API on 192.168.1.50:7438, reachable from inside only. The journal is
  persistent. Power failures recur; no remote power control.
- **Both:** `maintenance.idle_bandwidth_fraction` 0.5; config backups beside `/etc/macha/macha.yaml`. Debian 13,
  aarch64, Boost 1.83. Core capture to `/mnt/diskB/crash`. Torrent on. One tree-backed head, SM19.
- **Access:** the API needs a session; anonymous sessions are off. macnessa's genesis root password is in
  `/var/lib/macha/initial-root-password` (root only); the same account works on inverbeg. Scripts that read it are in
  `build/baselines/`.
- **Deploy:** build on macnessa with the log and `$?` written to a file; `DESTDIR=/root/stage-V cmake --install build
  --prefix /usr/local`; the tarball must carry `libdannan.so.0.5*` and `dannan-import-macha` beside Macha's files;
  `tar czf /root/macha-V.tgz -C /root/stage-V usr`; on each node, one at a time, `/root/stop-with-stacks.sh`, then an
  install script from `build/install-0.94.0.sh` with `TARBALL=` and `HEALTH=http://<api address>:7438/api/v1/health`,
  which saves `/root/pre-V/` and rolls back if not healthy in 180 s. A config change: edit `/etc/macha/macha.yaml`
  with a backup beside it, then `build/baselines/restart_check.sh` per node, one at a time.
- **Laptop:** tree `build-port` (Dannan from `build/deps/prefix-0.5.3` first in `CMAKE_PREFIX_PATH`), `-j8`, tests
  `--jobs 8` with `TMPDIR` under `build/tmp`. An Intel Mac; no torrent plugin; no LeakSanitizer (Linux only, so the
  container). Scratch files and logs in `build/`. Code lookups through basemind; a hook blocks grep, sed, awk,
  head, tail and cat on local files, so Python for text work.

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou Shalt Not Make Control Wait. 2. Thou
  Shalt Not Make The Viewer Wait. 3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer
  Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Everything the API and the web serve is viewer work**, including the bytes still draining to a client.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.** A local operation never waits on a peer or walks the
  namespace.
- **No vacuous truths.** An empty set never satisfies "all" or "enough".
- **Three answers, HTTP numbers below as above**: "here it is", "cannot, and that will not change", or "cannot say
  now" (try later, bounded exponential backoff). Refusals carry an HTTP status and a snake_case code; nothing
  matches English text.
- **Proportion.** Account for and control what matters to the user's experience.
- **Testing**: test the change once (the touched tests on the laptop, then all three suites on macnessa, which are
  the gate), deploy, measure. No repeat runs or soak runs; sanitizers only as a targeted debugger, except that
  `tools/ci.sh` is the merge gate and runs ASan and UBSan on every push. A laptop under heavy load gives no verdict
  on timing.
- **Report findings before fixing; prove a mechanism before editing.** Design questions go to the operator with a
  recommendation.
- **Every response has a snake_case code; errors add a message; clients own sorting and presentation; every API
  change is announced to Core and every client before it ships.**
- **A Macha defect the port finds is fixed in Macha first**; the libraries match the fix. A Macha test retires only
  against a named library case, recorded case by case.
- **Deploying**: build once on macnessa, run all three suites there, ship the `usr` tree; never both nodes down at
  once. After any rejected or interrupted command, check what actually ran.
- **Builds**: laptop `-j8` and tests `--jobs 8`. When a shared struct changes, rebuild every target. Never edit
  sources under a running build. Format with clang-format 19; clang-tidy reports nothing; a suppression is a
  `NOLINTNEXTLINE(check)` with its reason beside it.
- **Git**: commit on the branch as work lands and push when the gate is green; stage named paths, never `-A`;
  no attribution trailers; no em dashes in Macha.
- **Reading the cluster**: correlate on the `Z` timestamp in the message. Record which host served a measurement.
- **The composition root is code**: concrete classes, constructor references, dependency order.
- **The DATA store is `StoragePool`** over one `LocalStore` per backend; the control store is a `LocalStore`.

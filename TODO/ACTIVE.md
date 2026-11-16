# Active tasks

Last updated: 2026-11-16 about 02:00Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-15-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md`,
`COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The port: Macha onto Dannan and Dagda, one branch, gate at the end** (operator, 2026-11-14; cutover "in the next
few days"). The plan and its progress, step by step: [`2026-11-14-port-rewrites.md`](2026-11-14-port-rewrites.md)
(R1 root, R2 filesystem and FUSE, R3 metadata and control, R4 catalogue, R5 ingest and torrent, R6 status and
adverts, R7 removal, then the flag day). The call-by-call map it follows: `store-map-callers/` (six sections).
Rulings: Macha calls `dagda::Context` only, no shims; the root keeps `dagda::Node`'s owner surface (open, stop,
reconfigure, the reference counts, reachability registration, the maintenance steps until 8c); agreement waits for
Dannan 0.9.0; ack levels are the stack standard (`persisted` = the disk that took it, this node's when it can).

**Where it stands (2026-11-15, 23:30Z):**
- Pushed to `25a37d3`, then `d35239d` (TODO). **Committed, not pushed:** `c176854`, R3's last piece (the provider
  `MachaReachability`, the census on `Node::references()`, `Maintenance` on the node's steps, key
  `maintenance.collect_garbage` default false), R4 (the catalogue on Context; a store failure is `CatalogueUnavailable`),
  R5 (torrent admission and I/O through `Context::devices()`, ingest blocking on `FsStorageUnavailable` only), all on
  Dagda `6bfb5b2` with `ack_level`. Compiled green on build-3 (`/home/maintainer/macha-store-2`, `build/pi/macha_port_compile.sh`).
- **Committed, not pushed (origin at `25a37d3`):** `c176854` (R3 to R5), `ce205f1` (NodeEvents is Dannan's, on Dannan
  0.8.3 with `fbcbb90` patched in; prefix `prefix-dannan-083-stop`, `build/pi/deps_dannan_083_stop.sh`; Dannan is
  gating 0.8.4, move onto the tag when it lands), `62f63f2` (Status reads the store node), `e8c8794` (161 store cases
  retired), `14d34b6` (Macha's DistributedStore out of production), `b0e2c61` (the tests read the store node's
  parts), `bfd13ec` (artwork read bounded, 503; two more retired). 165 cases retired, case by case, in
  `2026-11-16-test-retirement.md`; what was on Macha's store parts is listed in `2026-11-16-store-test-rewrites.md`.
  All compile green on build-3. No suite has run.
- **Waiting on Dagda's gate** (it named cases at `3ca592a`, not yet run, every Pi busy): retire
  `test_the_repair_step_that_first_sees_a_new_pull_list_walks_it` (StoreRepair.TheStepThatFirstSeesANewListWalksIt),
  `test_a_pull_list_new_while_the_push_runs_is_not_passed_over` (StoreRepair.APullListNewWhileThePushRunsIsNotPassedOver,
  new; a failure is a real Dagda gap) and `test_a_failed_read_keeps_the_object_and_a_bad_record_is_removed`
  (StoreStoragePool.AReadErrorKeepsTheObject). Those three are the last tests on Macha's store classes (`StoreBench`,
  `StoragePool`); once they go, `StoreBench` and Macha's store code can go.
- **Builds:** build-3 `/home/maintainer/macha-store-2` (the current tree; `/home/maintainer/macha-store` holds an older copy),
  `build/pi/macha_port_compile.sh` with `S` and `R` set, against `prefix-dannan-083-stop` and `prefix-dagda-6bfb5b2`.
  Dagda has since gated `6adc5ed` (2026-11-15: synchronous ticket waits on the caller's thread, placement ranked once,
  the barrier's log lines; API unchanged): move the prefix to it at the next rebuild. Deploy deps from Dannan's newest 0.8 patch (0.8.3, or 0.8.4 if the maintainer cuts one with the stop-aware wait) and Dagda's tag.
- **Next:** the plan's "R6 and R7, the order": steps 1 and 2 are in, and step 6's artwork deadline. Then steps 3 and
  4 together, after the three retirements above: Macha's store code and `StoreBench` go, `StorageServer` and
  `LocalState`'s store parts go, adverts and telemetry from Dagda's figures. Then step 5 (availability on Dagda's
  survey), the trace clock (Macha's `MaintenanceClock` as Dannan's `TimeSource`/`WallClock` into
  `MaintenanceOptions`, with the trace tests' rewrite). Then R6 (adverts from `dagda/capacity` and `store::figures_of`, telemetry and Status from Dagda's figures,
  reload through Dagda, the files path table from Dagda's survey) together with R7 and the plan's "Before any suite
  can run" list (`LocalState`'s store parts, Macha's `DistributedStore` and `StorageServer`, Macha's survey, the
  trace clock into `MaintenanceOptions`), each Macha test retired against the Dagda case
  `../dagda/TODO/2026-11-05-test-mapping.md` names; then the suites, fix forward, the harness, the migration rehearsal.

**Parked by the operator (2026-11-14: port first, heap later):** the heap corruption P0. Findings so far: 125
ASan-clean rounds of the crash workload; the FUSE read late-write (a timed-out read's worker writing into libfuse's
freed buffer) found and fixed by R2's reply buffer (`01eecf0`); libfuse's low-level API after the port (BACKLOG).
Detail in `archive/2026-11-15-ACTIVE-before-rationalisation.md`, item 1. Macha defects the map review found are
recorded there too; pack compaction's was Dagda's to fix (done, `7f14515`, reviewed).

**Custody at the next deploy, ahead of the cutover** (operator): `dannan-custody` from Dannan 0.8.x, roots sealed
with the host key (no TPM on the Pis), the unit's `LoadCredentialEncrypted=`, an off-node backup of the plain root or
of the sealed root with `credential.secret`; run between stop and start.

## The shared build Pis

build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202`, build-4 `.228`: Pi 5, 16 GB,
Debian 13 arm64, shared by every session. No builds on the Mac. Claim cores on Lir's board (`claim-board claim`,
gate on "Taken"; `claim-board wait ... --for 90m` as a background command when none are free; release when the job
ends); details in memory `project_build_host`. Macha's state there: build-3 `/home/maintainer/deps`,
`/home/maintainer/macha-store`, `/home/maintainer/macha-asan`, `/root/pi`, `/root/heap*`; build-2 `/home/maintainer/macha-asan`,
`/home/maintainer/heap`, `/root/heap*`.

## Waiting on the operator

- After the port: whether the heap chase resumes with media load (the 11-14 test-volume rule applies).
- The Codecov token for the repository.
- Carried, unchecked: the media-kind import hint; truncated pack records; the `*.pre-migration.*` archives; the
  replica journal reader with no writer left.

## Open, in order (Macha, after the port's items)

1. **The heap corruption** (above) and the FUSE `rm` crash (found-issues 1, 2).
2. **A full control store is silent; work that cannot complete is retried without backoff** (found-issues 3).
3. **Cancelling an ingest job leaves its worker running** (found-issues 4).
4. **The stop**: the release horizon's census (`namespace_references`) and the tree diff under `census_follow` do
   not check it.
5. **Import filing**: about 430 episodes filed as films in `/Movies`; names such as `Glass Harbour ( (1982)`
   (found-issues 9).
6. **Books** as the web client reported at 0.92.2 (series counts, `published` types and nulls, split authors).
7. **Failing tests, each P0 when it recurs** (not rechecked on the current nodes):
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`,
   `availability/test_two_nodes_survey_what_neither_holds`,
   `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor`,
   `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
   `users/test_accounts_converge_across_nodes`.
8. The rest of found-issues, items 5 to 17.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption.

## Cluster state (read 2026-11-13; not reread since)

- **Both on 0.94.2** since 2026-11-13 (live-b 15:30Z, live-a 15:31Z, `replicas=2/2`): the first optimised build
  (RelWithDebInfo), tarball sha256 `b6d1b65a...0d95`, installed by `build/install-0.94.2.sh`; rollback
  `/root/pre-0.94.2/`. live-b has since crashed once in malloc (the heap P0); read both nodes' state before
  stating it.
- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a): `/usr/local`, `/var/lib/macha`, DATA `/mnt/diskB/data` 6.3 TB,
  control store limit 8G, API 192.0.2.50:7438. Never build there.
- **live-b** (root@live-b.example.net, Pi 5, 16 GB, site-b, inbound capable at :7437): DATA
  `/mnt/diskB/data`, filling from live-a; API 203.0.113.50:7438 (inside only); persistent journal; power failures
  recur, no remote power control. Never build there.
- **Both:** `maintenance.idle_bandwidth_fraction` 0.5; config backups beside `/etc/macha/macha.yaml`; cores to
  `/mnt/diskB/crash`; torrent on; one tree-backed head, SM19.
- **Access:** the API needs a session; live-a's genesis root password is in `/var/lib/macha/initial-root-password`
  (works on live-b too); scripts that read it are in `build/baselines/`.
- **Deploy (operator's word each time):** build on a claimed Pi with the log and `$?` in a file; `DESTDIR=/root/stage-V
  cmake --install build --prefix /usr/local`; the tarball carries libdannan (and, after the cutover, libdagda) beside
  Macha; on each node in turn `/root/stop-with-stacks.sh`, then an install script from `build/install-0.94.2.sh`
  (saves `/root/pre-V/`, rolls back if not healthy in 180 s). Never both nodes down at once.
- **Syncing a tree to a Pi:** `rsync -rlp --checksum --exclude '/build*' --exclude /.git --exclude CLAUDE.md
  --exclude CLAUDE.local.md`; no `-t`, no `--delete`.

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
- **Testing**: test the change once on a Pi (the suites there are the gate; `tools/ci.sh` is the merge gate, ASan
  and UBSan included); no repeat or soak runs; sanitizers otherwise only as a targeted debugger. Operator to every
  session, 2026-11-14 (relayed): ASan and TSan runs are for releases; running the same tests more than once needs a
  flaky test or odd behaviour to justify it ("We're spending 50% of our time heating silicon for no good reason").
- **Report findings before fixing; prove a mechanism before editing.** Design questions go to the operator with a
  recommendation.
- **Every response has a snake_case code; errors add a message; clients own sorting and presentation; every API
  change is announced to Core and every client before it ships.**
- **A Macha defect the port finds is fixed in Macha first**; the libraries match the fix. A Macha test retires only
  against a named library case, recorded case by case.
- **One definition per concept**: a type Dannan or Dagda defines is aliased, never twinned.
- **Agreement is Dannan's** (operator 2026-11-15, relayed by Dannan): anything that could use consensus or agreement
  waits for Dannan's agreement primitive (0.9.0, leaderless, CASPaxos-shaped, per register; design in
  `../dannan/TODO/2026-11-15-agreement-design.md`). Macha builds no new consensus, no lease standing in for one,
  and no workaround meanwhile.
- **Builds** only on the Pis, -j2 or -j3, nice 19, claimed. Never edit sources under a running build of the same
  tree (the Pi has its own copy, so editing on the Mac is fine). Format with clang-format 19; clang-tidy reports
  nothing; a suppression is a `NOLINTNEXTLINE(check)` with its reason beside it.
- **Git**: commit on the branch as work lands; push only on the operator's word; stage named paths, never `-A`;
  no attribution trailers; no em dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files, so
  Python for text work. Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

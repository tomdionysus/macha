# Active tasks

Last updated: 2026-11-16 about 04:30Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
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

**Where it stands (2026-11-16, 04:30Z): `port/dannan-dagda` at `deceac1`, pushed** (the maintainer: "You can push when you
need to"; still no tag, merge or PR without his word). Every commit compiles on build-3 (GCC, warnings as errors);
**no suite has run**, the gate is at the end. Summary of the branch's content (details in the plan, commit by commit):
- R1 to R5 done: the root over `dagda::Node`, one lock and identity, readiness and `NodeEvents` from Dannan; reads,
  the FUSE reply buffer, writes and publication on tickets, hydration and activity through Context; the namespace
  tree, the claims barrier, the seed, the reachability provider `MachaReachability` and `Maintenance` driving the
  node's steps (`maintenance.collect_garbage` default false, so the live nodes reclaim nothing until the provider's
  answers are checked); the catalogue, torrent admission and I/O, and the ingest's classification through Context.
- R6 done except the leftovers below: Status, the observation gauges, telemetry, `data_storage_degraded` and peers'
  capacity and use from Dagda's figures and adverts; availability's path table over `Node::availability()`; reload
  and the hosting advert through the store node; artwork reads bounded (30 s, then 503); `MaintenanceClock` is
  Dannan's `TimeSource` and `WallClock`, handed to the node.
- R7 mostly done: 171 cases retired against named Dagda (and one Dannan) cases (`2026-11-16-test-retirement.md`,
  case by case); the rest read the store node's parts (`2026-11-16-store-test-rewrites.md`). Deleted:
  `DistributedStore`, `StorageServer`, `ControlObjectFetch`, the claim walk, `RetentionLedger`, `UnreferencedSince`,
  `StoragePool`, `PersistentBlockCache`, Macha's survey; `LocalState` keeps only the metadata seed and replica.
- Deps: Dannan 0.8.4 pinned (`find_package(dannan 0.8.4)`; it carries the stop-aware `NodeEvents` wait; 0.8.5 is out
  with a stop hang fixed, for the deploy), Dagda `6bfb5b2` (`6adc5ed` and `ab76b43` gated since; API unchanged).

**Next, in order:**
1. Macha's `LocalStore` and what stands on it (`presence_index`, `durability_domain`, `held_ledger`,
   `sealed_journal`, the tries, `retention`, `reference_counts`) and its contract twins (`object_store`,
   `object_ledger`, `claim_store`, `horizon`, `horizon_builder`) go; `tools/metadata_history_dump.cpp` reads its
   control store through Dagda's `LocalStore` instead.
2. R1's leftovers: Macha's `DataResourceArbiter` and `ActivityClocks` out of `NodeResources` (Dagda's admission and
   activity are the node's), one `StartupProgress`, stage names.
3. The maintenance trace tests rewritten over Dagda's maintenance (the mapping's REWRITE cases in
   `test_maintenance_trace.cpp`); the GAP-HEADS cases stay on Macha's replica until 8c.
4. Move the prefixes to Dannan 0.8.5 and Dagda's newest gated commit (or its tag).
5. The first suite run on a Pi; fix forward. Then the side-by-side harness and the migration rehearsal (a gate, the maintainer
   2026-11-14), then the port against the live baselines.

**Builds:** build-3 `/home/maintainer/macha-store-2` (the current tree), `build/pi/macha_port_compile.sh` with
`S=/home/maintainer/macha-store-2 R=/root/pi/port2`, prefixes `/home/maintainer/deps/prefix-0.8.4` (`deps_dannan_084.sh`) and
`prefix-dagda-6bfb5b2` (`deps_dagda_6bfb5b2.sh`). Claim first, wait for "Taken", then build: never in one batch.

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

- **Repair's pull starves under a sending push** (found by Dagda 2026-11-16 porting
  `test_a_pull_list_new_while_the_push_runs_is_not_passed_over`, confirmed in Macha's code): one operation budget
  serves the push and the pull (`distributed_store.cpp:2530-2538`, used at `:2991`, `:3069`), and the pull's only
  own share is the scan budget (`:2941-2943`), so a push that sends every step holds a new pull list until its pass
  ends, against the comment there. The live nodes (0.94.2) run it. The rule says fix in Macha first, which means a
  develop change and release for code the port deletes; recommended instead: Dagda fixes it test-first (a share of
  the operation budget kept for the pull while its list is new, at least one operation) and Macha records it as a
  known live defect until the cutover. Dagda is holding its fix for the answer.
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

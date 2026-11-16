# Active tasks

Last updated: 2026-11-16 about 16:10Z, on `port/dannan-dagda`, after a check of every claim here against the tree,
Dannan's and Dagda's repositories, build-3 and both live nodes (what it corrected is marked "checked 16:05Z"). **State a branch's content from its diff and a node's
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

**Where it stands (2026-11-16): the code of `port/dannan-dagda` is at `deceac1`; the branch is at `428a40b` (two
TODO commits since), origin the same** (the maintainer: "You can push when you need to"; still no tag, merge or PR without his
word). build-3's `/home/maintainer/macha-store-2` holds the same sources (checked 16:05Z by hash; its `build-port` is from
`deceac1`). Every commit compiles on build-3 (GCC, warnings as errors);
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
  with a stop hang fixed, for the deploy), Dagda `6bfb5b2` (`6adc5ed` and `ab76b43` gated since; API unchanged;
  `ab76b43` is Dagda's last full gate, every commit after it notes or a disabled test). **No Dagda tag holds what
  the port needs** (checked 16:05Z): CMake asks `find_package(dagda 0.2.0)` and the branch reports 0.2.0, but the tag
  `0.2.0` (`9020281`, local only, held by the maintainer) does not contain `6bfb5b2`, and `feature/macha-store-gaps` is not
  pushed. The deploy's Dagda is therefore a named commit of that branch until Dagda tags one. Dagda's word, ~17:00Z
  (not read here): its newest gated commit is `0d048e0` (its own loop reads the inventory before each repair turn;
  1120 ran, 1117 passed, 3 skipped), only `Node::maintenance_loop` changed, none of the steps Macha's pass calls;
  the candidate for step 4. Its benchmark driver built at `274053c` and was **reviewed by Macha the same evening**
  (read, not run): accepted as Macha's repair as deployed; asked of Dagda: a longer bound or a partial figure for
  the viewer scenario (repair on its 5 of 100 share there), elapsed compared as reference plus measure, prompt
  replication's figures beside repair's, the raw WiFi link recorded each round. Left unverified: what production
  passes as `Service`'s durability batch window (the driver uses the tests' 500 ms). Dagda sends the measured
  results; Macha reads its side of them.

**Next, in order: the port's code** (the operator, 2026-11-16: prepare for the Dagda replacement; repair is not
Macha's to touch, we wait for Dagda's redesign):
1. Macha's `LocalStore` and what stands on it (`presence_index`, `durability_domain`, `held_ledger`,
   `sealed_journal`, the tries, `retention`, `reference_counts`) and its contract twins (`object_store`,
   `object_ledger`, `claim_store`, `horizon`, `horizon_builder`) go; `tools/metadata_history_dump.cpp` reads its
   control store through Dagda's `LocalStore` instead. The largest item and waits on no other step: start here.
   What still includes those headers (checked 16:05Z), so the removal reaches each: `cluster/cluster.hpp`
   (`local_store`, `retention`), `cluster/node_identity.cpp` and `metadata/namespace_control_store.hpp`
   (`local_store`), `catalogue/catalogue.hpp` (`object_ledger`), `contract/gates.hpp` (`horizon`),
   `service/maintenance.hpp` (`horizon`, `horizon_builder`, `object_ledger`), `filesystem/filesystem.cpp`
   (`reference_counts`) and `filesystem.hpp` (`object_trie`), `storage/io_pressure.hpp` and `config_base.cpp` (a
   `StoragePool` mention each); in the tests `test_backend_support.hpp`, `interposed_local_store_files.hpp`,
   `test_namespace_service.cpp` (`local_store`), `test_storage_metadata.cpp` (`sealed_journal`) and a
   `DistributedStore` mention at `test_storage_v18.cpp:204`.
2. R1's leftovers: Macha's `DataResourceArbiter` and `ActivityClocks` out of `NodeResources` (Dagda's admission and
   activity are the node's), one `StartupProgress`, stage names.
3. The maintenance trace tests rewritten over Dagda's maintenance (the mapping's REWRITE cases in
   `test_maintenance_trace.cpp`); the GAP-HEADS cases stay on Macha's replica until 8c. **Hold the cases that assert
   push then pull within a step**: Dagda's redesign changes them at its stage 2 and names each one.
4. Move the prefixes to Dannan 0.8.5 and Dagda's newest gated commit (or its tag).
5. The first suite run on a Pi; fix forward. It runs on Dagda's current repair, which has the live defect below, so
   the comparison with 0.94.2 stays like for like.

**Leave alone until Dagda's stage 4:** the Status code reading Repair's gates and step figures (`transfer`,
`settle`, the gates): its shape changes, and Dagda brings the new one before building it. Keep it working, no more.

**Prep for the flag day, alongside the code** (the gates, the maintainer 2026-11-14: Macha's suites on Dagda; the side-by-side
harness on the Pis; the migration rehearsal on copies of real state; reliability under crash, restart and partition;
then the port against the live baselines):
- **Config:** check the port's mapping of every store key (`2026-11-13-store-cutover.md` step 2, plan S.3) against
  both live `macha.yaml` files; a key that no longer applies is refused or dropped with a clear line, never silently.
  Both files read 2026-11-16 (values under "Cluster state").
- **The migration rehearsal:** copies of real state from live-a and live-b (control store, ledgers, a pack sample)
  onto a claimed Pi, `dagda-migrate`, a verifier listing every id before and after, and a written rollback.
- **Deploy and custody:** the package on the new deps (the tarball carries libdannan, libdagda and `dannan-node`);
  custody at the next deploy, ahead of the cutover: `dannan-custody` from Dannan 0.8.x, roots sealed with the host key
  (no TPM on the Pis), the unit's `LoadCredentialEncrypted=`, an off-node backup of the plain root or of the sealed
  root with `credential.secret`, run between stop and start; each node's control store copied off-node first.
- **API changes announced to Core and every client** before they ship (list in `2026-11-14-port-rewrites.md`,
  "Before the flag day"); the repair figures marked provisional, since Dagda's redesign changes them again.
- **Baselines:** the 0.94.2 results the port is compared against (`build/baselines/`, `2026-11-06-pi-baselines.md`)
  lined up so "the port against the live baselines" is ready to run.

**Builds:** build-3 `/home/maintainer/macha-store-2` (the current tree), `build/pi/macha_port_compile.sh` with
`S=/home/maintainer/macha-store-2 R=/root/pi/port2`, prefixes `/home/maintainer/deps/prefix-0.8.4` (`deps_dannan_084.sh`) and
`prefix-dagda-6bfb5b2` (`deps_dagda_6bfb5b2.sh`). Claim first, wait for "Taken", then build: never in one batch.

**Parked by the operator (2026-11-14: port first, heap later):** the heap corruption P0. Findings so far: 125
ASan-clean rounds of the crash workload; the FUSE read late-write (a timed-out read's worker writing into libfuse's
freed buffer) found and fixed by R2's reply buffer (`01eecf0`); libfuse's low-level API after the port (BACKLOG).
Detail in `archive/2026-11-15-ACTIVE-before-rationalisation.md`, item 1. Macha defects the map review found are
recorded there too; pack compaction's was Dagda's to fix (done, `7f14515`, reviewed).

## The shared build Pis

build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202`, build-4 `.228`: Pi 5, 16 GB,
Debian 13 arm64, shared by every session. No builds on the Mac. Claim cores on Lir's board (`claim-board claim`,
gate on "Taken"; `claim-board wait ... --for 90m` as a background command when none are free; release when the job
ends); details in memory `project_build_host`. Macha's state there: build-3 `/home/maintainer/deps`,
`/home/maintainer/macha-store`, `/home/maintainer/macha-asan`, `/root/pi`, `/root/heap*`; build-2 `/home/maintainer/macha-asan`,
`/home/maintainer/heap`, `/root/heap*`.

## Known live defect until the cutover

**Repair's pull starves under a sending push** (develop `distributed_store.cpp`, live on 0.94.2). Push and pull share
one 16-call step budget (`:2530-2538`), so a push that sends every step holds a new pull list until its pass ends.
A second path, read and not tested: a push that ends a step credit-limited skips the pull for that step (the
`!result.credit_limited` guard at `:2939`). Delayed convergence, nothing lost. The maintainer's ruling, 2026-11-16: no patch in
Macha or Dagda; repair is redesigned in Dagda (`TODO/2026-11-16-repair-redesign.md`, commit `877585c` on
`feature/macha-store-gaps`): one queue of transfers ordered by copies left, either direction, a window against the
byte credit, the call budget gone; stage 0 measures first. For the port: Dagda names each ported trace case that
changes at its stage 2, and brings the new shape of Repair's public steps (which Status reads) before stage 4.
Since `877585c` on Dagda's branch (read 16:05Z, its session at work): the credit path is proven in Dagda's code
(`StoreRepair.DISABLED_APushOutOfCreditDoesNotHoldBackThePull`, `39fe114`, seen failing once on build-3, disabled
until stage 2); the maintainer ruled a holder may offer ids to an owner at the moment of transfer and the benchmark runs across
two Pis (`4f1aca6`); the benchmark is designed as workload 7 on **build-1 and build-2, both claimed whole**
(`e6f6474`, not built), which takes two of the four Pis from the port's first suite run while it runs. Dagda's
`ACTIVE.md` still lists Macha's port as "R3 in progress" and stage 0's benchmark as "reviewed by the Macha session".

## Waiting on the operator

- **Answered, no longer waiting: Dagda's review request for its repair benchmark** (harness workload 7, two Pis;
  Dagda's `TODO/2026-11-15-side-by-side-harness.md` last section, commit `e6f6474`). The operator, 2026-11-16 ~16:15Z:
  finish the review, then send it; sent to the Dagda session ~16:25Z (the design only; the driver is reviewed when
  built). What it said beyond the facts below: no fixture drives repair without metadata (the trace fixture holds a
  whole `Service`), and Macha takes no referenced set from outside, so the fair driver is a `Service` in each process
  writing files through `service.filesystem()`; the live cluster is the proof two machines join (shared key file,
  live-a `bootstrap: []`, live-b naming live-a); live `metadata_write_copies` 1 and `heartbeat_ms` 5000 against
  the harness's 2 and 1 s; scenario 1's clock charges Macha for metadata replication unless both start when b's
  referenced set is complete; the viewer scenario's reader must read through the filesystem. Told Dagda as unchecked:
  whether `Service` links from its extract's `macha_core` alone, whether repair's own puts wake the pass, packing of
  64 KiB objects at deployed values, a's writes while b is away. Step 1 is **held on the operator's word** (same
  exchange). Dagda asked: (1) how to drive Macha's repair at f0597a4 on its
  harness node (referenced ids, survey, credit, share, step, at the deployed interval) and whether a fixture does it;
  (2) the deployed values that pace repair; (3) whether two processes join with a shared key, listen/advertise set
  and b's bootstrap naming a; (4) anything unfair to Macha. Found so far, not sent:
  - Repair runs in `Maintenance::run` (f0597a4 `src/service/maintenance.cpp:560-857`): the gate on the share and
    quiescence (:562-563), the survey `availability_.refresh` (:589-606), the claim walks, then
    `store_.repair_step(byte_budget, operation_budget, ...)` with the pull list from `missing_here()` (or every
    referenced id until the survey knows) and the push list from `peer_lacks()` (:729-806). The 16 is a `constexpr`
    at :734, not config. Credit: `bandwidth * idle_bandwidth_fraction * cpu_scale` per second, banked to
    `max(extent, 5 s of link)` (:417-437).
  - Cadence: another step one `interval` after a step that did work (:1363-1364); the loop also wakes on storage and
    claims events, the credit deadline, the share's next turn and the survey's due time (:1344-1369, :1396-1415).
    Whether repair's own puts wake it at once through storage events was not checked.
  - Live values (live-a and live-b, read 2026-11-16), where they differ from f0597a4's defaults:
    `idle_bandwidth_fraction` 0.5 (default 0.10). The rest at default: `interval_ms` 1000, `foreground_quiet_ms`
    2000, weights 95/5, `cpu_target` 0.10, `initial_bandwidth` 32M, `max_bandwidth` 0, `busy_bandwidth_fraction`
    0.0, `background_concurrency` 0. Not in `maintenance` but pacing repair: `telemetry_interval_ms` 10000, so the
    peer-viewer window is `max(3 x 10 s, 3 s)` = 30 s (:414-415), against the 3 s Dagda's `peer_window` comment
    assumes.
  - Fixture: `tests/test_maintenance_trace.cpp` exists at f0597a4; whether it drives repair without metadata was not
    read. Question 3 not checked.
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

## Cluster state (read 2026-11-13; configs reread 2026-11-16)

- **Both on 0.94.2** since 2026-11-13 (live-b 15:30Z, live-a 15:31Z, `replicas=2/2`): the first optimised build
  (RelWithDebInfo), tarball sha256 `b6d1b65a...0d95`, installed by `build/install-0.94.2.sh`; rollback
  `/root/pre-0.94.2/`. live-b has since crashed once in malloc (the heap P0); read both nodes' state before
  stating it. **Read 2026-11-16 16:05Z:** both answer health `ok`, 0.94.2, `libdannan.so.0.5.3`; running since
  2026-11-13 21:47Z (live-a) and 21:10Z (live-b), after that evening's deliberate stop-and-start rounds; no exit,
  core or reopen-on-sight line in either journal since. live-b's one SEGV (2026-11-13 21:01:23Z, core
  `core.macha-media-inf.12251` in `/mnt/diskB/crash`) is logged one second after systemd began stopping the service.
  live-a's crash directory is empty. DATA: live-a 5.9T used of 9.1T, live-b 2.6T of 9.1T.
- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a): `/usr/local`, `/var/lib/macha`, DATA `/mnt/diskB/data` 6.3 TB,
  control store limit 8G, API 192.0.2.50:7438. Never build there.
- **live-b** (root@live-b.example.net, Pi 5, 16 GB, site-b, inbound capable at :7437): DATA
  `/mnt/diskB/data`, filling from live-a; API 203.0.113.50:7438 (inside only); persistent journal; power failures
  recur, no remote power control. Never build there.
- **Both** (configs read 2026-11-16): `maintenance.idle_bandwidth_fraction` 0.5, every other maintenance key at its
  default; `dht` replicas 2, write_copies 1, extent 4M; `telemetry_interval_ms` 10000, `dead_after_ms` 30000;
  live-a listens on 192.0.2.50 and advertises `live-a.example.net`, live-b listens on 0.0.0.0 and advertises
  `live-b.example.net`; live-a runs `libdannan.so.0.5.3`; config backups beside `/etc/macha/macha.yaml`; cores to
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
- **Git**: commit on the branch as work lands; push `port/dannan-dagda` when needed (operator 2026-11-16), anything
  else only on his word; no tag, merge or PR unasked; stage named paths, never `-A`;
  no attribution trailers; no em dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files, so
  Python for text work. Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

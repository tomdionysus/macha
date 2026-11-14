# Active tasks

Last updated: 2026-11-14 about 02:15Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-14-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md`,
`COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**Two things run at once** (operator, 2026-11-14: the store work in parallel with the heap chase; the heap result
comes first when it lands).

1. **The heap corruption P0** (found-issues 1; operator chose "Chase it now", report the mechanism before fixing,
   fix in Macha first). inverbeg crashed in malloc under 0.94.2; the core's victim was `FileSystem::media_ids`
   (`filesystem.cpp:2094`) from `MediaInformationService::prune`. Single node under ASan on neptune: 53 clean
   rounds. **The ASan pair, 01:30Z to 04:30Z 2026-11-14: clean.** 72 rounds (about 14,400 files of 64 KiB written,
   fsynced and deleted through FUSE on node A, replicated to B), no ASan or UBSan report during the run or at exit,
   zero ERROR lines, WARN only the expected startup ones; both units stopped `inactive`, FUSE unmounted, both Pis
   released. Plain FUSE churn with replication does not reproduce it; the victim's caller (`prune`) never ran,
   since there was no media. **Next waits on the operator:** the media-load pair (below). The setup as run: node A on corvus-neptune (unit `macha-heap-a`, FUSE mount
   `/mnt/heaptest`, driver `/root/heap_driver.py` writing 200 x 64 KiB files fsynced then deleted per round, about
   130 s a round, `ROUNDS=300`), node B on corvus-saturn (unit `macha-heap-b`, replica, `replicas=2/2`). Results in
   `/root/heap-pair/` on each (`asan.*`, `ubsan.*`, `macha.log`, `driver.log`); scripts in `build/heap/`.
   **Check:** `ssh root@10.44.1.202 'ls /root/heap-pair; tail -n 3 /root/heap-pair/driver.log; systemctl is-active
   macha-heap-a'` and the same on 10.44.1.170 for `macha-heap-b`. **Stop at the claim's end, 04:30Z** (about 80
   rounds) if clean: `systemctl stop macha-heap-a macha-heap-b`, `fusermount3 -u /mnt/heaptest`, pkill pattern
   `"[h]eap_driver"`. **If clean, next (operator's choice, but confirm with him first after the 11-14 test-volume rule): the pair
   again with media load**: scanner and
   media-information service on over a media library (make test media with ffmpeg `testsrc` on the Pi, never copy
   from the live nodes unasked) so `prune` runs during the FUSE churn. If ASan names a writer: report the
   mechanism to the operator before any edit.
2. **The store cutover, step 1a** (plan and log: [`2026-11-13-store-cutover.md`](2026-11-13-store-cutover.md);
   the class-by-class map: `build/store-api-map.md`). **Committed 2026-11-14 after neptune went green (714/714,
   20/20, 21/21):** `1a27043` (the bump) and `f84337c` (the first slice), as described here:
   - the move to Dannan 0.6.0 and Dagda 0.1.5: `CMakeLists.txt` (`find_package(dannan 0.6.0)`, `dagda 0.1.5`), both
     workflows (Dagda built from its tag after Dannan; both tags are on their remotes), CONTRIBUTING, the install
     guides and quickstart. Dannan's public headers 0.5.3 to 0.6.0 change nothing Macha uses;
   - 1a's first slice: `FrameType`, `WorkClass`, `work_class`, `frame_for`, `frame_type_name` are Dannan's
     (`src/cluster/frame_type.hpp`), `WorkClass::viewer` renamed `foreground` everywhere, `Waits`, `ThreadSafety`,
     `local_waits`, `default_waits`, `includes`, `may_enter` aliased (`src/contract/work.hpp`; Macha's own
     `WorkContext` and static `WaitGuard` remain), the cast helpers `dannan_frame`, `macha_frame`, `dannan_work`
     gone (the friction list's "enum casts"). 35 files.
   **Build:** `build/pi/deps_060.sh` (Dannan 0.6.0 and Dagda 0.1.5 into `/home/tom/deps/prefix-0.6.0` and
   `prefix-dagda-0.1.5` on neptune, log `/root/pi/deps.log`), then the tree rsynced to `/home/tom/macha-store` and
   `build/pi/macha_store_build.sh` (default preset, -j2, nice 19, every suite; log `/root/pi/macha.log`, verdict
   `/root/pi/macha.result`). **Check:** `ssh root@10.44.1.202 'cat /root/pi/macha.result; tail -n 20
   /root/pi/macha.log'`. Green: commit the bump and 1a as two commits (named paths). Red: fix, re-rsync (the rule
   below), rerun the script.
   **Then, green on neptune (714/714, 20/20, 21/21) and committed:** `00a741f` (`RetentionClass`, `RetentionDot`,
   `RetentionClock`), `f2de690` (`IdLookup`), `ed1d3fd` (`StorageUnavailable`, so Macha's catches catch Dagda's).
   **Finding, for the operator (report before acting):** the rest of 1a's list are not twins. The map's "inferred
   identical" is wrong, checked by a normalised diff against Dagda 0.1.5 (`build/twin_diff.py MACHA DAGDA`):
   `ObjectTrie` takes a `shared_ptr<const domain::StorageKey>` and a logger (Macha: a raw 32-byte key) and adds
   `rekey`, `probe`, `read_only` and a different journal replay; `UnreferencedSince` **seals its file** under the
   storage key (aad `dagda/sightings/1`; Macha's is plain), an on-disk format change; `DiskServiceMonitor` and
   `DiskServiceTimer` need a logger and a `TimeSource` at every construction and timer site. `TrieUnion` is identical
   apart from holding Dagda's `ObjectTrie`, and `ObjectStore`/`HeldView` hold a `TrieUnion`, so all five go together.
   Recommendation: end 1a here and move these five into 1b, where Dagda's `Node` builds the stores that construct
   them; `UnreferencedSince`'s format goes with the migration (dagda-migrate), not an alias. Waiting on the operator.
   **Custody moved ahead of 1b** (Dagda's `Node::open_store` refuses a node without custody, `node.cpp:199` at
   0.1.5, so plan 7b's Macha half is a prerequisite of step 1): `9770427` adds `custody.credential` (and
   `credentials_directory`) as `NodeConfig::custody`, unset = none as before, documented in
   `docs/configuration.md`; gated 715/21/21. Left of 7b: the unit's `LoadCredentialEncrypted=`, making and sealing
   custody on each node (`dannan-node --state <state_path> --init-custody --custody-credential <name>` prints the
   root), `dannan-node` in the tarball, off-node backups; all at deploy, on the operator's word.
   Then 1b: `dagda::Node(OpenLater)` in `LocalState`,
   the classes aliased, adapters for `DistributedStore` (tickets for `DurabilityBatch`, ReadOptions, RepairLists)
   and `AvailabilityService` (Macha keeps the path table); `catalogue.cpp:2721`'s `claim_deletion` becomes
   `node.data().claim_locks().deletion(id, retained(control))`, as Dagda's collector does (`collector.cpp:338`).

**Afterwards, in order:** the store cutover's steps 2 to 8 (plan file); custody (7b); Macha's deb on
`libdannan0.6 (>= 0.6.0)` and `libdagda0.1.5`; the partition rerun and the write-up of the 0.94.2 baselines
(`build/baselines/results-0.94.2/`, `partition.py`'s path fixed in build/); the Macha-side friction items in the plan
file (the duplicate `connections_canonical` write is done, `5d8b6dd`; left: async call watches, deferred connectivity check, memory fit,
durability epoch, payload copies); announce the `dagda.<name>` registration names to Core and the clients **at the
cutover** (operator, 2026-11-14), not before; Status thread names (clients to Dannan's names, announced, then drop
the mapping).

## The shared build Pis

corvus-jupiter `root@10.44.1.169`, corvus-saturn `root@10.44.1.170`, corvus-neptune `root@10.44.1.202`: Pi 5,
16 GB, Debian 13 arm64, shared by every session. **Operator, 2026-11-14: no builds on the Mac** (no cmake, suites,
Docker); **nobody schedules the Pis**: claim a free one on Lir's board, first come first served, release the moment
the job ends. Operator, 2026-11-14 afternoon: the board is the only coordination channel (claims_list, claim_take,
claim_release, note_post for anything disruptive); never message a holder for permission. One heavy job per Pi, -j2 or -j3, nice 19. After a session restart
the claim-board plugin gives `claim_take`, `claim_release`, `status_set` and blocks heavy ssh commands without a
heavy claim. By hand: version-1 JSON on `stack/pis/<host>/claims/Macha%20Server` and
`stack/sessions/Macha%20Server/status` (details: memory `project_build_host`). **Held now:** nothing (neptune and saturn
released 04:31Z 2026-11-14). Corvus LoRa builds on saturn
alongside node B (agreed); Dagda holds jupiter (bench, then its 0.1.5 packages with Macha's fixtures, copied to
`/root/dagda-arm64/macha-fixtures`). Macha's state on the Pis: neptune `/home/tom/deps`, `/home/tom/macha-store`,
`/home/tom/macha-asan` (ASan build of 0.94.2's line), `/root/pi`, `/root/heap*`; saturn `/home/tom/macha-asan`,
`/home/tom/heap`, `/root/heap*`. A shared TSan Pi (clang 19, 4K pages) is Lugh's and Dagda's to settle.

## Where the code is

- `port/dannan-dagda` at `ed1d3fd` (plus this TODO commit), past `develop` (`f0597a4`), pushed to `0a0801b`; the
  commits since are local: TODO commits, the bump, 1a's four slices, two clang-format fixes (the 1a slice and
  `a95e9cc` had left eight files unformatted; the whole tree now passes clang-format 19), the
  `connections_canonical` deletion. Before committing, check formatting on the Pi:
  `clang-format-19 --dry-run --Werror` on every changed file. `main` is `37fa763`, tagged 0.94.2; `develop` carries TODO commits past it. CMake says 0.94.2.
- The branch so far: Dagda 0.1.4 linked (`de57596`), `NodeId` and `Hash256` Dannan's (`6d8c6e7`), Macha's config as
  `dagda::NodeOptions` (`a95e9cc`, `src/cluster/store_node.{hpp,cpp}`).
- Peers: Dannan (0.6.0 tagged and pushed; 0.7.0 tagged 2026-11-14, SONAME `libdannan.so.0.7`, with Macha's fixes;
  0.8.0's P6 seams on `feature/owner-test-seam` reviewed at `c860fb5`, all four fit: `call_fault` replaces
  `NodeRuntime::CallFault` and `StalledCalls` (Macha counts held calls in its own callback, since Dannan's
  `stalled_calls()` is the count held now), `test::BarePeer` replaces `tests/stand_in_peer.hpp` via `tune` and
  `before_start`, `watch_advert` for the storage figures only (the generation stays on the roster watch),
  `AsyncSink` around Macha's sink; the review found a leave could be coalesced away at `c860fb5`, now fixed;
  `LogSink::write_at(time, ...)` added for 0.8.0, so Macha's stamping sink must override `write_at` when it moves
  behind `AsyncSink`; the fixes `2b1ce5f`, `8a0bce3` and `76537c6` read and passed, one note sent: the withdrawal test's 1 s
  wall-clock bound must have headroom; Macha's fixtures must destroy a node before the io threads it posts to; merged to Dannan's develop as
  `3d1d4c0`, rewritten ids checked by patch-id; `7921d23` raised: `supervised_loop(nullptr)`'s function-local
  static can be destroyed before a static `AsyncSink`'s writer stops, UB at exit; fixed in `bcd8b55` (the fallback and the process clocks never destroyed), read and passed; P4's design (`../dannan/TODO/2026-11-14-p4-worker-pool.md`) answered: Macha's maintenance
  takes the pool's pacing as a gate (started / should_yield / finished, plus the CPU scale), not per-task
  submission, and the pool and any loop must share one per-class `DutyShare` per node; code `f531ce5` reviewed: two
  wiring changes asked, `set_contended` safe after start (Macha's reading needs the store, built after the node
  starts) and the gates' time and process CPU injectable apart from the node's (the trace tests step
  `MaintenanceClock`); both done in `f752aa9` (`NodeConfig::gate_time`, `gate_process_cpu`), and the reading swap
  made safe at clear in `537c619` (once `set_contended` returns the old reading is never called; Macha clears it
  before destroying its store), both read and passed. For Macha's own move later: today `started()` gets `repair_busy` (this pass's byte counters
  too) while `should_yield`/`finished` get the `idle_for` reading (`maintenance.cpp:704-720`); one `contended()`
  unifies them, so check the pacing cases on that), Dagda (0.1.5 tagged and pushed; its arm64 debs, built from the tag with Macha's fixtures, all
  fixture-gated cases run and passed, are in `build/debs/dagda-0.1.5/`, sha256 checked),
  Lugh, Lir, Corvus LoRa, Corvus Radioberry; `SendMessage`, `ListAgents`. Their repositories are read-only to this
  session.

## Waiting on the operator

- Whether 1a ends at the five aliases, with `ObjectTrie`, `TrieUnion`, `ObjectStore`/`HeldView`,
  `UnreferencedSince` and `DiskServiceMonitor` moving into 1b (the finding in "Resume here").
- Whether to run the heap pair again with media load after a clean ASan pair, given the 11-14 test-volume rule.
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
8. The rest of found-issues, items 5 to 17.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption.

## Cluster state (read 2026-11-13; not reread since)

- **Both on 0.94.2** since 2026-11-13 (inverbeg 15:30Z, macnessa 15:31Z, `replicas=2/2`): the first optimised build
  (RelWithDebInfo), tarball sha256 `b6d1b65a...0d95`, installed by `build/install-0.94.2.sh`; rollback
  `/root/pre-0.94.2/`. inverbeg has since crashed once in malloc (the heap P0); read both nodes' state before
  stating it.
- **macnessa** (root@10.44.1.50, Pi 5, 8 GB, site-a): `/usr/local`, `/var/lib/macha`, DATA `/mnt/diskB/data` 6.3 TB,
  control store limit 8G, API 10.44.1.50:7438. Never build there.
- **inverbeg** (root@inverbeg.macha.network, Pi 5, 16 GB, site-b, inbound capable at :7437): DATA
  `/mnt/diskB/data`, filling from macnessa; API 192.168.1.50:7438 (inside only); persistent journal; power failures
  recur, no remote power control. Never build there.
- **Both:** `maintenance.idle_bandwidth_fraction` 0.5; config backups beside `/etc/macha/macha.yaml`; cores to
  `/mnt/diskB/crash`; torrent on; one tree-backed head, SM19.
- **Access:** the API needs a session; macnessa's genesis root password is in `/var/lib/macha/initial-root-password`
  (works on inverbeg too); scripts that read it are in `build/baselines/`.
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
- **Builds** only on the Pis, -j2 or -j3, nice 19, claimed. Never edit sources under a running build of the same
  tree (the Pi has its own copy, so editing on the Mac is fine). Format with clang-format 19; clang-tidy reports
  nothing; a suppression is a `NOLINTNEXTLINE(check)` with its reason beside it.
- **Git**: commit on the branch as work lands; push only on the operator's word; stage named paths, never `-A`;
  no attribution trailers; no em dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files, so
  Python for text work. Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

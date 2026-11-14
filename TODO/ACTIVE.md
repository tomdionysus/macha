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
   rounds. **Running now: the ASan pair** since 01:30Z: node A on corvus-neptune (unit `macha-heap-a`, FUSE mount
   `/mnt/heaptest`, driver `/root/heap_driver.py` writing 200 x 64 KiB files fsynced then deleted per round, about
   130 s a round, `ROUNDS=300`), node B on corvus-saturn (unit `macha-heap-b`, replica, `replicas=2/2`). Results in
   `/root/heap-pair/` on each (`asan.*`, `ubsan.*`, `macha.log`, `driver.log`); scripts in `build/heap/`.
   **Check:** `ssh root@10.44.1.202 'ls /root/heap-pair; tail -n 3 /root/heap-pair/driver.log; systemctl is-active
   macha-heap-a'` and the same on 10.44.1.170 for `macha-heap-b`. **Stop at the claim's end, 04:30Z** (about 80
   rounds) if clean: `systemctl stop macha-heap-a macha-heap-b`, `fusermount3 -u /mnt/heaptest`, pkill pattern
   `"[h]eap_driver"`. **If clean, next (operator's choice): the pair again with media load**: scanner and
   media-information service on over a media library (make test media with ffmpeg `testsrc` on the Pi, never copy
   from the live nodes unasked) so `prune` runs during the FUSE churn. If ASan names a writer: report the
   mechanism to the operator before any edit.
2. **The store cutover, step 1a** (plan and log: [`2026-11-13-store-cutover.md`](2026-11-13-store-cutover.md);
   the class-by-class map: `build/store-api-map.md`). **Uncommitted in the working tree, building on neptune:**
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
   **Next in 1a:** the remaining twins, each its own buildable commit: `RetentionClass`, `RetentionDot`,
   `RetentionClock` (`src/contract/claim_store.hpp`, values identical to Dagda's), `StorageUnavailable` (Macha's
   catches in `ingest.cpp` and `filesystem.hpp` must catch Dagda's), `IdLookup`, `UnreferencedSince`, `ObjectTrie`,
   `TrieUnion`, `ObjectStore`, `HeldView`, `DiskServiceMonitor`. Then 1b: `dagda::Node(OpenLater)` in `LocalState`,
   the classes aliased, adapters for `DistributedStore` (tickets for `DurabilityBatch`, ReadOptions, RepairLists)
   and `AvailabilityService` (Macha keeps the path table); `catalogue.cpp:2721`'s `claim_deletion` becomes
   `node.data().claim_locks().deletion(id, retained(control))`, as Dagda's collector does (`collector.cpp:338`).

**Afterwards, in order:** the store cutover's steps 2 to 8 (plan file); custody (7b); Macha's deb on
`libdannan0.6 (>= 0.6.0)` and `libdagda0.1.5`; the partition rerun and the write-up of the 0.94.2 baselines
(`build/baselines/results-0.94.2/`, `partition.py`'s path fixed in build/); the Macha-side friction items in the plan
file (duplicate `connections_canonical` write, async call watches, deferred connectivity check, memory fit,
durability epoch, payload copies); announce the `dagda.<name>` registration names to Core and the clients **at the
cutover** (operator, 2026-11-14), not before; Status thread names (clients to Dannan's names, announced, then drop
the mapping).

## The shared build Pis

corvus-jupiter `root@10.44.1.169`, corvus-saturn `root@10.44.1.170`, corvus-neptune `root@10.44.1.202`: Pi 5,
16 GB, Debian 13 arm64, shared by every session. **Operator, 2026-11-14: no builds on the Mac** (no cmake, suites,
Docker); **nobody schedules the Pis**: claim a free one on Lir's board, first come first served, release the moment
the job ends, ask a holder to use a claimed one. One heavy job per Pi, -j2 or -j3, nice 19. After a session restart
the claim-board plugin gives `claim_take`, `claim_release`, `status_set` and blocks heavy ssh commands without a
heavy claim. By hand: version-1 JSON on `stack/pis/<host>/claims/Macha%20Server` and
`stack/sessions/Macha%20Server/status` (details: memory `project_build_host`). **Held now:** neptune heavy and
saturn light, until 04:30Z; release both when the heap run and the build end. Corvus LoRa builds on saturn
alongside node B (agreed); Dagda holds jupiter (bench, then its 0.1.5 packages with Macha's fixtures, copied to
`/root/dagda-arm64/macha-fixtures`). Macha's state on the Pis: neptune `/home/tom/deps`, `/home/tom/macha-store`,
`/home/tom/macha-asan` (ASan build of 0.94.2's line), `/root/pi`, `/root/heap*`; saturn `/home/tom/macha-asan`,
`/home/tom/heap`, `/root/heap*`. A shared TSan Pi (clang 19, 4K pages) is Lugh's and Dagda's to settle.

## Where the code is

- `port/dannan-dagda` at `0d2a8e4`, six commits past `develop` (`f0597a4`), pushed to `0a0801b` (one TODO commit
  local). `main` is `37fa763`, tagged 0.94.2; `develop` carries TODO commits past it. CMake says 0.94.2.
- The branch so far: Dagda 0.1.4 linked (`de57596`), `NodeId` and `Hash256` Dannan's (`6d8c6e7`), Macha's config as
  `dagda::NodeOptions` (`a95e9cc`, `src/cluster/store_node.{hpp,cpp}`).
- Peers: Dannan (0.6.0 tagged and pushed; 0.7 `feature/macha-fixes` approved), Dagda (0.1.5 tagged and pushed),
  Lugh, Lir, Corvus LoRa, Corvus Radioberry; `SendMessage`, `ListAgents`. Their repositories are read-only to this
  session.

## Waiting on the operator

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
  and UBSan included); no repeat or soak runs; sanitizers otherwise only as a targeted debugger.
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

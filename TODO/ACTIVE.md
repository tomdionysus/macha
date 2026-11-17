# Active tasks

Last updated: 2026-11-17 about 19:30Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-17-ACTIVE-before-rationalisation.md` (the Dagda review exchanges of 11-16
and 11-17 in full). Alongside: `BACKLOG.md`, `COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not
requirements).

## Resume here

**The port: Macha onto Dannan and Dagda, one branch, gate at the end** (operator, 2026-11-14). The plan and its
progress, step by step: [`2026-11-14-port-rewrites.md`](2026-11-14-port-rewrites.md) (R1 root, R2 filesystem and
FUSE, R3 metadata and control, R4 catalogue, R5 ingest and torrent, R6 status and adverts, R7 removal, then the flag
day); the call-by-call map: `store-map-callers/`. Rulings: Macha calls `dagda::Context` only, no shims; the root
keeps `dagda::Node`'s owner surface; agreement waits for Dannan; ack levels are the stack standard (`persisted` = the
disk that took it, this node's when it can).

**Where it stands (2026-11-17 19:30Z):** `port/dannan-dagda` at `46c61ba` plus TODO commits, pushed. The port's code
is `deceac1` and one fix since, `46c61ba` (each directory a durable write makes is synced into its parent; the maintainer
ruled it onto the port branch, 2026-11-17; found by Dannan in its own copy, live on 0.94.2 until the cutover).
Every commit compiles on build-3 (GCC, warnings as errors). **No full suite has run**; a filtered run on 11-17 passed
both durable-file cases and showed one port case failing at start (step 5 below).
- R1 to R5 done; R6 done except R1's leftovers; R7 mostly done: 171 cases retired against named library cases
  (`2026-11-16-test-retirement.md`), Macha's distributed store, storage server, pool, cache, survey, claim walk and
  retention ledger deleted; `LocalState` keeps only the metadata seed and replica.
- Deps: Dannan 0.8.4 pinned (0.8.5 tagged, for the deploy); Dagda `6bfb5b2` on `feature/macha-store-gaps`. **No
  Dagda tag holds what the port needs**: CMake asks `dagda 0.2.0`, but tag `0.2.0` (local, held by the maintainer) does not
  contain the branch's commits and the branch is not pushed.

**Step 1 is held on the operator's word (2026-11-16).** Nothing below starts without it.

**Next, in order, once released:**
1. Macha's `LocalStore` and what stands on it (`presence_index`, `durability_domain`, `held_ledger`,
   `sealed_journal`, the tries, `retention`, `reference_counts`) and its contract twins (`object_store`,
   `object_ledger`, `claim_store`, `horizon`, `horizon_builder`) go; `tools/metadata_history_dump.cpp` reads its
   control store through Dagda's `LocalStore`. Includers to reach (checked 11-16): `cluster/cluster.hpp`,
   `cluster/node_identity.cpp`, `metadata/namespace_control_store.hpp`, `catalogue/catalogue.hpp`,
   `contract/gates.hpp`, `service/maintenance.hpp`, `filesystem/filesystem.cpp` and `.hpp`, `storage/io_pressure.hpp`,
   `config_base.cpp`; tests `test_backend_support.hpp`, `interposed_local_store_files.hpp`,
   `test_namespace_service.cpp`, `test_storage_metadata.cpp`, `test_storage_v18.cpp:204`.
2. R1's leftovers: `DataResourceArbiter` and `ActivityClocks` out of `NodeResources`, one `StartupProgress`, stage
   names.
3. The maintenance trace tests rewritten over Dagda's maintenance (the REWRITE cases in `test_maintenance_trace.cpp`;
   GAP-HEADS stay on Macha's replica until 8c). Dagda reports none of the ported cases changed through its repair
   stages 1 to 3; check that from the fixtures at the commit step 4 builds on, and take the names Dagda sends for
   stage 4.
4. Prefixes to Dannan 0.8.5 and Dagda's newest gated commit: **`d119d14`** (repair stages 1 to 3 and the viewer
   fix; gate 1133 ran, 1130 passed, 3 skipped, by Dagda's word). Not `d2872de` alone (it regresses a rejoin under a
   viewer). Nothing new binds in Macha: its pass passes `repair.lists()` (stage 2's `holds`), `Repair::transfer`
   debits the offer bytes, and `dagda::Node` gives its server the offer book (Dagda confirmed).
5. The first suite run on a Pi; fix forward. **Seen already:**
   `rpc_cluster/test_a_durable_trio_founds_commits_and_reads_through_every_node` fails at start with "Dagda's message
   types are registered already ... message type 515 is already registered as have_object" (two store nodes
   registering on one Dannan node in that fixture). Not diagnosed.

**Leave alone until Dagda's repair stage 4 is built:** the Status code reading Repair's gates and step figures. Its
shape is agreed (2026-11-16, Dagda's `TODO/2026-11-16-repair-redesign.md`, "Stage 4, the shape proposed"): a window
of transfers replaces the 16-call budget; `transfer()` returns before its I/O lands; `in_flight > 0` settles as
`more_work`; the store raises a storage event when the window drains to half; public step signatures unchanged; no
config key to retire. Nothing in Macha relies on the old behaviour (the stop order leaves in-flight transfers to
`dagda::Node::stop`). **The maintainer, 2026-11-16: `offer_bytes` and `in_flight` join Status's repair section at the cutover**,
announced with the other API changes; Dagda sends their names when built, and the case names whose order changes.

**Prep for the flag day, alongside the code** (gates, the maintainer 2026-11-14: Macha's suites on Dagda; the side-by-side
harness; the migration rehearsal on copies of real state; reliability under crash, restart and partition; the port
against the live baselines):
- **Config:** the port's mapping of every store key (`2026-11-13-store-cutover.md` step 2) checked against both live
  `macha.yaml` files; a key that no longer applies is refused or dropped with a clear line.
- **Migration rehearsal:** copies of real state from both nodes onto a claimed Pi, `dagda-migrate`, a verifier
  listing every id before and after, a written rollback.
- **Deploy and custody:** the tarball carries libdannan, libdagda and `dannan-node`; custody at the next deploy
  (`dannan-custody`, roots sealed with the host key, `LoadCredentialEncrypted=`, an off-node backup); each node's
  control store copied off-node first.
- **API changes announced to Core and every client** before they ship (list in `2026-11-14-port-rewrites.md`,
  "Before the flag day").
- **Release notes:** the port's CHANGELOG entry names the durable-directory fix (`46c61ba`) beside the cutover.
- **Baselines:** the 0.94.2 results (`build/baselines/`, `2026-11-06-pi-baselines.md`) ready for the comparison.

**Builds:** build-3 `/home/maintainer/macha-store-2` (in step with the Mac at `46c61ba`), `build/pi/macha_port_compile.sh`
with `S=/home/maintainer/macha-store-2 R=/root/pi/port2` (log `/root/pi/port2.log`, result `/root/pi/port2.result`),
prefixes `/home/maintainer/deps/prefix-0.8.4` and `prefix-dagda-6bfb5b2`; a full rebuild takes 15 to 25 minutes at -j2.
Tests: `build-port/macha-tests --filter TEXT --jobs 2`. Claim first, wait for "Taken"; release when done.

**Parked by the operator (2026-11-14: port first, heap later):** the heap corruption P0. Findings: 125 ASan-clean
rounds of the crash workload; the FUSE read late-write found and fixed by R2's reply buffer (`01eecf0`); libfuse's
low-level API after the port (BACKLOG). Detail in `archive/2026-11-15-ACTIVE-before-rationalisation.md`, item 1.

## Known live defects until the cutover

Both are the maintainer's rulings to leave on 0.94.2 and fix through the port.
- **Repair** (develop `distributed_store.cpp`): push and pull share one 16-call step budget, so a sending push holds
  a new pull list back, and a credit-limited push skips the pull for its step; on a rejoin the holder pushes while
  the joiner pulls the same objects. Measured 2026-11-16 (Dagda's harness, workload 7, two Pis on WiFi at about
  2.9 MB/s, a tenth of the plan's sizes): a large rejoin took about 80 s where the bytes sent once take about 38 s;
  small objects are bound by the call budget (410 objects in 38.9 s); a viewer on the holder was not slowed. Nothing
  is lost. The maintainer, 2026-11-16: no patch; Dagda's redesign replaces it (stages 1 to 3 built; stage 4 next).
- **Durable writes into new directories** (`src/durable_file.cpp`): a crash just after the write returns can lose a
  directory it made, and the file in it. Fixed on the port branch (`46c61ba`).

## Waiting on the operator

- Step 1's release (held 2026-11-16).
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
5. **Import filing**: about 430 episodes filed as films in `/Movies`; names such as `Glass Harbour ( (1982)`
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

## The libraries, as of 2026-11-17 19:30Z (read from their repositories)

- **Dannan:** tags to 0.8.5; develop `df4b2d8` is cutting 0.9.0 (agreement, the simulation harness, the file-system
  interface, the hybrid clock, shared peer records). Macha needs nothing in 0.9.0 (told Dannan 11-17); when Macha
  moves to 0.9, `CallOutcome::peer` becoming `shared_ptr<const NodeRecord>` touches `cluster/dannan_host.cpp:197-226`.
  Loader promotion at the peer moves to 0.10.0, its design sent to Macha before it ships.
- **Dagda:** `feature/macha-store-gaps` at `35244a4` (notes on `d119d14`), not pushed; tags `0.2.0` (local, held),
  `0.1.5`. Its repair redesign (`TODO/2026-11-16-repair-redesign.md`) is at stage 4, the shape Macha agreed.

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
  nodes, say what was not checked, and change Macha's code only on the operator's word.
- **Builds** only on the Pis, -j2 or -j3, nice 19, claimed. Never edit sources under a running build of the same
  tree. Format with clang-format 19; clang-tidy reports nothing; a suppression is a `NOLINTNEXTLINE(check)` with its
  reason beside it.
- **Git**: commit on the branch as work lands; push `port/dannan-dagda` when needed (operator 2026-11-16), anything
  else only on his word; no tag, merge or PR unasked; stage named paths, never `-A`; no attribution trailers; no em
  dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files, so
  Python for text work. Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

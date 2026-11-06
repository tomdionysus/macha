# Active tasks

Last updated: 2026-11-06, on `develop`, checked against the
code and the running nodes, not against earlier notes. **State a branch's content from
its diff and a node's state from its files, never from a prior note** (operator, on this
file's drift: "We can't afford drift like this."). The version before the morning's
rationalisation is `archive/2026-11-05-ACTIVE-before-rationalisation.md`.
Alongside this file: `BACKLOG.md` (everything else, by area), `COMPLETED.md`,
`CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**Live.** macnessa (root@10.44.1.50, builds in `/home/tom/macha`) and inverbeg
(root@inverbeg.macha.network) run **0.93.4**, one cluster, under `/usr/local`, installed
2026-11-06 02:34Z and 02:36Z. Develop, main and tag 0.93.4 (merge 9a08846) are pushed.
Gate on macnessa 751, 20, 21, trace repeat 84 of 84. Both nodes writable and connected;
the TV import (b2bf6286) resumed by itself. Of 39 metadata deltas read after the restart, none
was empty: 18 catalogue, 17 appends, 4 namespace changes.

**The old-code removal has landed.** 0.93.0 a namespace is a tree from its first commit;
0.93.1 the legacy write API; 0.93.2 legacy authority; 0.93.3 genesis and the inline formats.
What stayed: SM14, SM16, SM17, SM19 and DLT5 to DLT11 with envelope and hash; the legacy clock
(a live feature); `MetadataSnapshot::entries`, `merge_metadata_heads` and `metadata_delta` as
the working form; the map branches of `apply_metadata_delta_in_place` and `namespace_entry`;
`garbage` and `metadata_voters` in SM14, which its hash covers; `required == 0` for genesis on
a founder only. Replay of a copy of macnessa's real state reproduced every frame, root
2fdd09bd. **Test changes the review should read:**
`test_publication_generation_barrier_precedes_metadata_commit` now counts only the data
store's barrier, on an unproven explanation of the control one;
`test_concurrent_reads_during_divergence_produce_one_reconciliation` retries its first write,
on one log line.

**0.93.4, the fix group**, in the CHANGELOG. The repair stall reproduced in tests and is fixed,
with a wake when a disk comes back; whether it stalled fi-1 a month ago is not shown.

**The port question is answered: not today.** All three sessions agree, and nobody found an actual
loss: Macha has never run on either library. **Read
[`2026-11-06-port-readiness.md`](2026-11-06-port-readiness.md) first**: the answer by requirement, the
seams nobody owns (custody is a blocker), the proposed plan, and five decisions for the operator.
**Afternoon ruling:** both libraries answered "yes with amendments" and a bounded claim; the operator
rejected it: "development will continue on them with macha code frozen until they can answer Yes
unconditionally, the entire reason for their existence is this." Their amendments are their own work
lists now, recorded in that file; the cross points between them are settled there too.

**Next:**
1. **The operator's five decisions** in that document. Nothing past the baselines is agreed until then.
2. **Pi baselines** on 0.93.4, with the version recorded (operator cleared 2026-11-06; what to
   measure: evaluation section 6, item 4, plus the two libraries' lists in the readiness file). Not
   started, and no longer waiting: the TV import was killed at the operator's word at 15:47Z. Record
   `throttled=0x50000` on both Pis with the figures.
   **The kill, for the record:** all 7565 files had copied, 0 failed. `cancel` set the job `cancelled`
   on disk but did not stop its worker: `enqueue_catalogue_hints` (`ingest.cpp:1270`) checks neither
   cancel nor stop, and kept rewriting the 5 MB `hints.json`. macnessa was restarted at 15:51Z, writable
   in about 70 s, job still cancelled, no hint writes since. Stop's stacks:
   `/root/macha-stop-20261106T155059Z.stacks` on the node. Files are in the namespace; hints not yet
   submitted were not, so some may not be catalogued. A defect under the freeze, not fixed.
3. Artefacts for the libraries are cut at 0.93.4: `build/macha-fixtures/goldens-0.93.4/` and
   `state-scratch-0.93.4/`. Dannan's arm64 results: `build/dannan-arm64-415c6c4/`.
   **Cut for Dagda's heads, 2026-11-06**, under `build/macha-fixtures/`, each with a README:
   `tree-vectors-0.93.4/`; `state-scratch-0.93.4-branch/`, `-delete/`, `-markers/`, `-truncated/`,
   `-seed/`, `-recovery/`, `-twoheads/`, from `gen/state_scenarios.py`, on the MacBook. `set-aside.meta`
   is not reached. No `journal-frames`: 0.93.4 only ever
   writes `journal.log` empty. `-torrent/` is owed when S7 starts: torrent stays through the port, and
   the laptop has no torrent plugin, so it is cut on a node. `macha-metadata-dump`'s `tombstones=`
   counts `garbage`, not `tombstone_batches`, so it reads 0 on heads with batches.
   **Found by Dagda's S3 reading `metadata.cpp` at 9a08846, checked by the Macha session by reading,
   nothing run, for the operator under the freeze:** (1) History tail cut: **fixed on the operator's
   word as a freeze exception, 0.93.5, on develop, tests green on the laptop only** (70 of 70
   `storage_metadata`). Not gated on macnessa, not tagged, not deployed: the nodes stay on 0.93.4.
   Dagda has the new behaviour; Dannan's layer does not move. (2) `reanchor_history` (`:3523`) erases without subtracting
   bytes, but `:3505` returns first when the entry is materialised, so no way to reach it was found.
   (3) Journal PREPARE then COMMIT: unreachable, `journal.log` is 0 bytes on both nodes.
   `macha-metadata-dump` aborts on a state with no `heads.meta`.
   **Dagda's findings from reading 9a08846, for the operator under the freeze, not fixes:** nothing
   writes `journal.log` now, so `compact()`'s thresholds and `pending_history_` act only on old
   frames; `set-aside.meta` is plain, no magic or seal; a corrupt `author.meta` throws out of
   `reserve_mutation_dot` and stops every write with no recovery path (against the self-heal rule);
   replay compares exception text (`metadata.cpp:2302`, `:2627`; against "never match English
   text"); a policy commit under the mutation lock may hold it through a peer's stall window,
   `write_stall`'s bound untraced; comments differing from code at `metadata_manager.cpp:1151-1153`,
   `:1449-1450`, `maintenance.cpp:158-159`, `metadata.hpp:78-79`, `docs/metadata.md:71` against
   `metadata_manager.cpp:1255`; `test_metadata_seed_sibling_replays_after_restart` retired at
   18f0c4b was the proof of the preferred-head rule, which now has no named Macha case (Dagda
   writes one).

**macnessa SIGSEGV, 2026-11-06 17:18:05Z, on 0.93.4, during the mount write baseline, cause not
found:** thread `macha-rpc-data`, inside `malloc`, called from `file_json` in
`IngestManager::handle_jobs_query` serving a peer's request. A fault inside `malloc` points to the heap
being damaged earlier, so this stack is likely where it surfaced, not what did it; not shown. Heap
corruption is on the reopen-on-sight list. Core: `/mnt/diskB/crash/core.macha-rpc-data.6139.1793985485`
(holds keys and plaintext), read against `/root/macha-0.93.4.tgz`'s version. systemd restarted it.
**Again at 17:34:28Z**, same libc instruction (`libc+0x939b4`, level 2 translation fault), thread
`macha-maint`, inside `malloc`, from `ObjectTrie::decode` under `StoragePool::held_view`, 5 minutes
into a process, while the baseline deleted its scratch files from the mount. Core
`core.macha-maint.9769.1793986467`. Two unrelated callers at one instruction fits a damaged heap; the
writer that damaged it is not found. inverbeg ran the same workload without a crash.
**Again at 17:45:49Z**, SIGABRT, journal `corrupted double-linked list (not small)`, in `malloc` under
`file_json` again, core `core.macha-rpc-data.10116.1793987140`. Both write passes died in the phase of
200 fsynced 64 KiB files on the mount, after 1 and 2 of 5 runs.
**Again at 18:11:09Z**, SIGABRT, same message, thread `macha-catalogue`, in `malloc` under `hint_json` in
`CatalogueHintQueue::save_state_locked`, core `core.macha-catalogue.12003.1793988660`, in a process that
never ran the small-file phase. So that phase is not the only way to it. Four crashes in 53 minutes
after none from 12:18Z to 17:18Z. Three of the four stacks are building the large job or hint JSON,
macnessa's alone. **Left until after the port (operator, 2026-11-06), who attributes it to undervoltage.**
Kernel log from 12:18Z, read that evening: macnessa 918 undervoltage events, inverbeg 104 from 12:00Z, about
nine times the rate. Fits; does not prove it: macnessa's events came about every 26 s in the crash hour, so
10 to 12 s before three crashes is chance, and its worst hours (13Z 286, 14Z 179) had no crash, against 137
in 17Z. No software writer has been ruled out.
**Lined up with the baseline's timestamps, recursive delete from the mount is the better lead:** 17:18:05Z, 3 s
after a run of 200 small files was recorded and its `rmtree` began; 17:34:28Z, during the cleanup `rmtree`, in a
process 3 minutes old that had done nothing else on the mount; 17:45:47Z, 9 s after the same record and `rmtree`.
18:11:09Z is the odd one: its process deleted through the mount from 18:02Z to about 18:05Z, then crashed 6
minutes later. This replaces "the small-file phase" above. It matches the operator's `rm` TODO below. inverbeg ran
the same deletes with no crash, 16:34Z to 17:05Z, which remains unexplained. Timing only: no reproduction aimed at
it, no writer found. `ls /mnt/machamedia` also timed out
twice, about 20 and 3 minutes after a restart, then answered in ms: not looked into.
**TODO (operator, 2026-11-06): `rm` in the FUSE mount with a filter can crash macha.** Not yet
reproduced by this session; the 17:34 crash came during a recursive delete from the mount, which may
or may not be the same thing.

**Waiting on the operator:** the media-kind import hint and what to do with the running TV job,
which still has files bound for `/Movies`; truncated pack records, which stay counted read
failures and report held, with repair unchecked; source files gone for good, which still block
the job; macnessa's reboot of 2026-11-06 12:16Z to 12:18Z, see memory `project-core-capture`: SIGBUS in a page read of `libmacha_core.so` after a failed state write, PID 1 faulting 22 s later, about 100 undervoltage events an hour, the NVMe reporting critical warning 0x02 at 0 C; root disk loss is inferred, not logged; the two cores are on `/mnt/diskB/crash`;
delete the two cores (2.8 GB, hold keys and plaintext; stacks already read), recommended; the replica journal reader, with no production writer left; movie folder names such as `Blade Runner ( (1982)`, 62 of 1891 in `/Movies` on 2026-11-06 (`title_for_movie_year`, `src/catalogue/media_catalogue.cpp:587`, cuts at the year and keeps the ` (`), not fixed, under the freeze; the `*.pre-migration.*` archives,
not found directly under `/var/lib/macha` or `/mnt/diskB/data` on 2026-11-06, location unchecked. Torrent
moves out, not a P0: `2026-11-06-ingest-plugins-torrent-removal-plan.md`.

## The port to Dannan and Dagda

Evaluation: [`2026-11-04-dannan-dagda-port-evaluation.md`](2026-11-04-dannan-dagda-port-evaluation.md),
kept current. Rulings so far (memory `project-port-rulings`): two cutovers,
transport first then the disk, no soak, inverbeg first; a Macha case may be retired
against a named library case, recorded case by case; reverse migration built
but not a gate; torrent bytes stay pinned in Macha's tests until torrent moves out; Dannan's respec
confirmed, Pi and real IGD work held.

- **Dannan**, baseline `9a08846`, local develop `415c6c4`, unpushed: `../dannan/TODO/ACTIVE.md` "The first
  cutover", `2026-11-05-first-cutover.md`, `2026-11-05-macha-test-mapping.md`. Steps 1 to 10 done but for
  the gaps in the readiness document; step 11, the tag, at the operator's word. `sched::ClassWaits` built.
- **Dagda**, baseline `9a08846`, local develop `c3c09d2`, unpushed: `../dagda/TODO/ACTIVE.md`,
  `2026-11-05-second-cutover.md`, `2026-11-05-test-mapping.md` (786 cases), `2026-11-05-heads-plan.md`.
  Has 0.93.4's shapes, 453 on the Mac. **Heads do not exist yet.** Class-waits branch unmerged, awaiting
  Dannan's tag.
- **Macha's code is frozen until the review passes (operator, 2026-11-05):**
  "Macha has a working, tested codebase, this shouldn't be modified until they're
  ready." Exceptions: 0.92.5 (deployed), the old-code removal and 0.93.4; each moves
  the libraries' baseline, and they are told at each commit. Artefacts for their
  plans are fine, changing no tracked file, under `build/macha-fixtures/`, each with a
  README: `placement/`; `goldens-0.93.4/` (Status 381 of 494 keys by name, journal 31 of 83
  lines, 46 config fields, routes read from source), with `goldens/` at cac5998 beside it;
  `state-scratch-0.93.4/` (tree-backed), with `state-scratch/` at cac5998 (inline, **not the
  live form**); `membership-tombstone/`; `state-macnessa/` (three roster files, read-only
  off the running node, approved through Dannan). `gen/` holds the generators.

## Rulings, 2026-11-05 (the operator, asked one by one; "via Dagda" = reported by it)

- **The ready gate** (replaced 2026-11-06, see "The port's order" in
  `2026-11-06-port-readiness.md`): the Macha session is the review. Both libraries finish all tasks
  and report their capabilities; when this session is satisfied Macha can be ported with no loss of
  functionality, it builds the port with both libraries frozen.
- **Design defects** (torrent merge associativity, no fence under a torrent claim, the local barrier's
  deadline, the sweep's mixed ages, the policy loop, O(state) per delta, wire-sized allocations, reload
  and `write_copies`, I/O and free-space reserves): left. "leave them, we need to get on with this."
  He had understood they were in the fix group; they are not, and are not fixed.
- **Staging**: stages, as many stops as needed, no flag-day worry ("its availability isn't critical",
  via Dagda). No soak between cutovers ("None"); a stage moves on when its tests pass. inverbeg comes
  back first. Disk does not wait for heads. Stage one: store, reachability provider, lease template,
  Macha's loop running. Later: heads, metadata migrated on disk at 8c, Dagda's loop.
- **Heads** build against the frozen code as it runs, and keep "an unchanged mutation commits" and the
  no-dot policy commit as Macha's for now (via Dagda).
- **Maintenance loop**: Macha's until 8c, then Dagda's.
- **Keys**: Dagda may hold a copy of the cluster key for tests, out of any repo. Custody from a systemd
  credential; one state directory, Macha's `state_path` (via Dagda).
- **Dannan**: A every call names its deadline; B the Status byte change accepted, its loops take Macha's
  names; C a per-pool thread CPU reporter; D per-account limits stay Macha's, and every Macha budget in
  its layer must be representable in Dannan (its step 8). Generic class admission in `dannan::sched`
  is **required first**, before stage one, gated by every existing case on both sides.
- **Real migration on a copy, there and back, and the inverbeg rehearsal: dropped for now**: "No drop
  that, I'll decide later." (This overrides what Dagda reported.)
- **Baselines**: Macha's, after old-code removal and 0.93.4. **macnessa's drive**: no extended test.
- **Older items**: parked in `BACKLOG.md`. Unused branches deleted.

## Open work, in order

00. **macnessa's diskB left the USB bus 2026-11-05 10:16:26Z**, under undervoltage;
   macha stopped cleanly. Back and serving from about 11:56Z after a physical knock;
   ext4 journal replayed on mount. Operator: cease the investigation. SMART at that
   point: no reallocated, pending or uncorrectable sectors, no logged errors. Short
   self-test unfinished when dropped, extended never run; the drive has no shock
   attribute, so this says little about the knock. Reports about 41,000 spindle hours
   against 13,426 power-on hours. `smartmontools` and `smartd` now on macnessa.
0. **P0: macnessa SIGSEGV, 2026-11-05 02:05:25Z, on 0.92.4, cause unknown.** 25 min
   after the deploy; the first SIGSEGV of this boot (up since 11-03 23:27, every
   earlier exit a stop or SIGKILL). No core (soft limit 0, pattern `core`), no kernel
   line, no warning in the journal; ingest and a catalogue scan were running, and the
   rail was in an undervoltage interval, as it often is. Restarted by systemd, writable
   in 35 s, 6 FUSE operations recovered. Core capture is on, both nodes, since 02:22Z:
   cores in `/mnt/diskB/crash` (mode 700; they hold keys and plaintext), set by
   `/etc/sysctl.d/90-macha-core.conf` and `macha.service.d/core-dumps.conf`; proven
   with a throwaway process. Read one against the tarball of the version that crashed, in `/root`.
1. **P0: the stop.** The survey's walks and the inventory walk check it, with tests
   (0.93.4). Not yet seen on a node: one stop on 0.92.4 took 6 s, none shown to have
   landed in a walk. The release horizon's census (`namespace_references`) and the tree
   diff under `census_follow` do not check it; each is bounded by what changed, except
   a release build's first walk. The FUSE mount's minute after start is recovery, not a
   defect (data ready +17 s, metadata +35 to +40 s at generation 100k, FUSE +42 s).
2. **From the TV import's plan (job b2bf6286, read 2026-11-05).** 1,516 of 7,571 files were
   planned under `/Movies`. 0.93.4's extras rule would have filed about 750, read by
   simulating it over the plan. The rest it leaves: about 340 in extras folders of titles
   with no probed episodes (House MD, Man to Man with Dean Learner, The Sopranos), and
   about 430 not extras at all, mostly **real episodes that probe as films**: `House MD
   Season 6 Episode 06`, `S01 - E15`, `Jail E30`, `Hornblower 04`, Star Trek TNG. That
   probe is the larger defect, unfixed, for the operator. 1,286 of those files were already
   imported under `/Movies` and stay there.
3. **Books**, as the web client reported at 0.92.2 (49 books): the Discworld series
   holds 3 of 49; three books send a timestamp `published` (A Hat Full of Sky, Soul
   Music, I Shall Wear Midnight); **possible regression, unexamined:** 11 books
   send `published` and `language` null (Eric, Hogfather, Jingo, Maskerade, Mort,
   Pyramids, Snuff, Sourcery, Thud!, Where's My Cow?, Wintersmith), and Thud! lost
   its publisher too. "The Light Fantastic (Discworld)" keeps its clutter. "Le
   Guin, Ursula K." splits into two authors. Conversion is MOBI and AZW3 to EPUB
   only; per-user media state (stage 3) waits on the freeze. Catalogue checks go
   through the client session.
4. **Failing tests, each P0 when it recurs** (*carried, not re-checked on the two
   current nodes; every name checked to exist at cac5998 on 2026-11-05*):
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`
   and `availability/test_two_nodes_survey_what_neither_holds` (laptop, load);
   `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor` (section 8);
   `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
   `rpc_cluster/test_rpc_slow_control_does_not_abort_data`,
   `rpc_cluster/test_inbound_incapable_node_is_reached_only_over_its_own_sessions`,
   `users/test_accounts_converge_across_nodes`,
   `rpc_cluster/test_rpc_metadata_executor_cancellation_disconnect_and_shutdown`.
   macOS torrent segfaults are the laptop's libraries: ignore there only.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`
(seven standing from before 0.89.0); `RPC stalled (control)`; heap corruption
(three times on the old gbni-1; never on the current nodes).

## Replication by diff

[`2026-10-09-replication-by-diff.md`](2026-10-09-replication-by-diff.md). Stages 0 to
4 built (0.90.71 to 0.90.77), the survey half of 5 built (0.90.96). Left: repair's
store walk, push cursor, `repair_position` and `have_*` probes, about 75 test call
sites. Dagda dropped these by decision, so this work may fall to the second cutover
rather than Macha. Measure again on the current pair: repair was last measured over
a 1.39 MB/s path that no longer exists.

Also here: a torrent can land on a node away from where it is watched; per-peer down
state in the transport; `fsync` without a deadline; say why a node counts itself busy.

## 3. Memory

Every large part is reported once a minute (0.90.69: `memory_limit_bytes`,
`memory_budget_bytes`, `memory_in_flight_*`, `memory_metadata_cache_bytes`,
`memory_ledger_caches_bytes`, `memory_catalogue_bytes`, beside `rss_bytes`),
and `runtime.memory_bytes` is a limit that shrinks the configured parts pro
rata with a `MEMORY OVER LIMIT` WARN, 0 none (0.90.70; operator,
2026-10-09: account for all memory, proportionately, no nit-picking).
- **Set a limit per node** from a day of the gauges (both today: 768M in
  flight, 512M metadata cache, 64M ledger caches; first readings RSS about
  500 MiB, of which about 100 MiB is in the counted parts).
- **A shrunk cache must not fall below its working set** (from the
  backlog): the limit can now shrink the metadata cache, so a floor at its
  unit (one materialised head) and a WARN when the limit forces it lower.
- Resident memory against 0.90.54 and 0.90.62 (the ledger's stage 3 and 4
  exits): read from the gauges after a few undisturbed hours on one version.
- The uncounted rest of RSS (code, libraries, allocator, snapshots in use):
  measure only if the gauges show it growing.

## 4. The catalogue plan, stages 6 to 9 and 11

Plan: [`2026-10-06-catalogue-materialised-view.md`](2026-10-06-catalogue-materialised-view.md).
Stages 1 to 5 are done (0.90.27 to 0.90.33). Next: stage 6, the conflict
merge per shard; then derived indexes for list, search and the descendant
scan; the manifest change (per-family shards, growable count); batched
profile publication; measure again. Stage 10 (holdings for catalogue DATA)
is section 2's stage 4; if that needs per-family shards, stage 8 comes
first. Its residency (about 16 MiB) is now a gauge.

## 5. Matching

- **The first track of a match pays the provider again**: the editor's
  search and artwork options were fetched, then the match looked the release
  up and downloaded the cover again (4.1 s for the first track, 0 for the
  second; gbni-1, 2026-10-06). 0.90.86: the cover listing is fetched once
  for the options and the match. Left, each paid once and needed: the
  release record when the editor did not list its tracks first (same seat,
  so cached when it did), and the chosen cover image itself.
- Provider caches are per editor seat and lost on restart.

## 6. Control: what is left, all small

The control audits are done (lock audit 0.90.45 to 0.90.49 and 0.90.68;
CPU and memory audit 2026-10-08; control lane fixes 0.90.61, 0.90.64 to
0.90.66). The head record is 87,546 bytes (0.90.67), so whole-record copies
on control threads no longer grow with deletes. Left:
- DNS (`getaddrinfo`, net.cpp) has no timeout of its own. fi-1 dials
  gbni-1 by name (`macnessa.macha.network`): 4 to 6 ms through its router,
  and no `resolve failed` in its journal (2026-10-12). Build only on
  evidence.
- A dial's peer observer can wait on another thread's roster fsync
  (`persist_m_`): `Membership::observe` writes the roster on the caller's
  thread when it changes. The fix is a combining writer (a caller finding a
  write in progress leaves its roster to the writer), but an identity reset
  must stay durable before it answers. No stall traced to it yet.
- gbni-1's DATA barrier (`syncfs` on `diskB`) is mean 484 ms, max 5.1 s
  under 20 torrents: it flushes torrent staging and the FUSE spool on the
  same disk, and libtorrent's writes keep no write-behind; viewer and loader
  DATA writes wait on it. libtorrent's own sockets use cubic and can fill an
  uplink. (Recorded; not control.)
- Whether a commit's control objects should reach every node present with
  the head (claims stop at `metadata_write_copies` holders).

## 7. Features, API and optimisation

- **The API is RESTful, all of it**: audit every route; identity resets
  become a resource (decided); `providers/artwork?ref=` and
  `providers/artwork/choose` are among the routes to change.
- **OpenAPI**: generated from a declarative route table that dispatch runs
  from, served at `/api/v1/openapi.json`. Agreed, unstarted.
- **Core's request**: `providers/artwork` taking `item_id`.
- **Per-file readability**: designed; the operator chooses where it goes.
  Two of its three uses are wire changes; its answer (held by nobody
  reachable) comes from section 2's stage 2, so it follows that.
- **A media-type context on torrent add** (operator, 2026-09-25).
- **People on catalogue items**: approved, with a backfill.
- **Placement API asks**: the target `node_id` and a viewer-facing detail on
  `placement_failed`; the peer's own refusal text on a forwarded add; an
  optional node `name` (operator's call).
- **Movie sets**: later; needs a proposal first.
- **Ebooks**: stages 1 and 2 of `TODO/2026-11-04-ebooks-design.md` are
  merged. Stage 3, the per-user media state, waits on the freeze; OPDS is not
  started.
  **Audiobooks** (operator, 2026-11-04): a separate media class of their
  own, designed after ebooks; they share the per-user media state.
- Measured small, not now: ingest and data publications commit one
  operation at a time (about 100 ms each); the hint store rewrites its file
  per change (266 KB with 409 hints); the head record's largest part is now
  its 72 torrent requests (only if they grow).

## Cluster state

- **macnessa** (root@10.44.1.50, hostname corvus-gbni-1, Pi 5, 8 GB, domain
  site-a): `/var/lib/macha`, DATA on `/mnt/diskB/data` (4.0 TB, about 1.03M loose
  objects, 17 packs on 2026-11-04), API on 10.44.1.50:7438. The build node.
- **inverbeg** (root@inverbeg.macha.network, Pi 5, 16 GB, domain site-b, inbound
  capable, dialled at inverbeg.macha.network:7437, checked 2026-11-05): DATA on `/mnt/diskB/data` (about 670 GB and filling), API on
  192.168.1.50:7438.
- **Metadata, both nodes** (2026-11-05, `macha-metadata-dump --tree` and
  `build/macha-fixtures/gen/frame_forms.cpp` over the live histories): one
  tree-backed head, root `2fdd09bd...`, SM19 (`DHTMETB9`, 41,044 bytes) with the
  legacy clock set, 23 torrent requests, 347 tombstone batches, 11,112 tombstones,
  10 authors seen; tree 266 nodes, 8,713 entries, 1,251,866 extents, 63.6 MB;
  generation about 129,500, rising about 20 a minute on 0.92.4. Re-rooted by
  `macha-namespace-migrate` on 2026-11-03 16:04Z from an inline SM18 head of 2,567
  entries (21 MB); the `*.pre-migration.*` archives are still in
  `/var/lib/macha/metadata/` on both. Certificates `required=1`: nothing is on
  legacy authority. A cluster founded at cac5998 founds inline and stays inline
  until migrated (`build/macha-fixtures/state-scratch/` is such a state).
- Both on Debian 13, aarch64, Boost 1.83. Neither API answers on loopback;
  anonymous sessions are off. macnessa's genesis root password is in
  `/var/lib/macha/initial-root-password` (root only), used read-only for Status
  on 2026-11-05; inverbeg's not checked. `books.enabled` and
  `books.conversion.enabled` are on; `libmobi-tools` installed.
- **Deploy:** build on macnessa with the log and `$?` written to a file (a pipe
  hides a failed build); `DESTDIR=/root/stage-V cmake --install build --prefix
  /usr/local` (not `/usr`); `tar czf /root/macha-V.tgz -C /root/stage-V usr`; on
  each node, one at a time, `/root/stop-with-stacks.sh`, then `build/install-V.sh`
  (from `build/install-0.92.3.sh`, untracked) over ssh with `TARBALL=` and
  `HEALTH=http://<api address>:7438/api/v1/health`. A wrong prefix or health URL
  makes it roll a healthy node back. `build/deploy.sh` is stale; do not use it.
- Rollback: `/root/pre-<version>/` on each node holds the previous binaries and
  config, restored by the install script when a node does not come up.
- A hung stop's stacks and journal land in `/root/macha-stop-<time>.*`; the
  2026-11-05 one is copied to `build/macnessa-stop/`.
- Laptop: `build-nobooks` and `build-notorrent`, `-j8`; plain `build` is stale. It
  is an Intel Mac. Scratch files go in `build/`. Code lookups through basemind.

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou
  Shalt Not Make Control Wait. 2. Thou Shalt Not Make The Viewer Wait.
  3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
  Viewer Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Everything the API and the web serve is viewer work**, because it is
  viewed, including the bytes still draining to a client.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a
  new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.** A local operation
  never waits on a peer or walks the namespace.
- **No vacuous truths.** An empty set never satisfies "all" or "enough".
- **Three answers, HTTP numbers below as above** (operator, 2026-10-12):
  every request comes to "here it is", "cannot, and that will not change",
  or "cannot say now" (try later, bounded exponential backoff). Internal
  refusals carry an HTTP status and a snake_case code; nothing matches
  English text. "Not here" is final only when every place it could be was
  seen.
- **Proportion.** Account for and control what matters to the user's
  experience; no nit-picking that does not improve it (operator,
  2026-10-09).
- **Testing**: design it properly, test it well once (the touched tests on
  the laptop, then all three suites on macnessa, which are the gate), deploy,
  measure, iterate. No repeat runs, soak runs, sanitizer builds or mutation
  sweeps; never rerun a whole suite on an overloaded laptop. A laptop under
  heavy load (load average over about 50) gives no verdict on timing.
- **Report findings before fixing; prove a mechanism before editing.**
  Design questions (formats, budgets, semantics) go to the operator with a
  recommendation.
- **Every response has a snake_case code; errors add a message; clients own
  sorting and presentation; every API change is announced to Core and every
  client before it ships.** Client sessions may ask; they do not decide.
- **Deploying**: build once on macnessa, run all three suites there, ship the
  `usr` tree (below); never both nodes down at once. When a
  release adds a wire message or a record format, roll the peers first and
  expect the older node to refuse the newer one's messages until upgraded.
  After any rejected or interrupted command, check what actually ran.
- **Builds**: laptop `-j8` and tests `--jobs 8`. When a shared struct
  changes, rebuild every target. Never edit sources under a running build.
  `macha-tests-runtime` holds the config tests and is Linux-only in parts.
- **Reading the cluster**: journald's prefix and `--since` are node-local
  time; correlate on the `Z` timestamp in the message. Record which host
  served a measurement. Absence of failures on an idle node proves nothing.
- **The composition root is code**: concrete classes, constructor
  references, dependency order. No framework injection.
- **The DATA store is `StoragePool`** over one `LocalStore` per backend; the
  control store is a `LocalStore`.

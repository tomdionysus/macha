# Active tasks

Last updated: 2026-11-05 evening, on `develop`, checked against the code and the
running nodes, not against earlier notes. **State a branch's content from its diff
and a node's state from its files, never from a prior note** (operator, on this
file's drift: "We can't afford drift like this."). The version before the morning's
rationalisation is `archive/2026-11-05-ACTIVE-before-rationalisation.md`.
Alongside this file: `BACKLOG.md` (everything else, by area), `COMPLETED.md`,
`CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**Live.** macnessa (root@10.44.1.50, builds in `/home/tom/macha`) and inverbeg
(root@inverbeg.macha.network) run **0.92.5**, one cluster, under `/usr/local`, installed
2026-11-05 14:52Z and 14:54Z. Develop, main and tag 0.92.5 (808fa0f) are pushed.

**0.92.5 is proven on the nodes.** Before: every recent delta a 61-byte empty DLT5,
about 20 commits a minute. After: generation 130,059 held still for 180 s on both, same
head, whose last commit is one DLT11 with `batches+=1 batches-=2`, the fix doing its
work once. Gate 747, 20 and 21. Not measured: whether the collectors now run. The 0.92.4
stop took 3 s on macnessa, 4 s on inverbeg.

**Next:**
1. Dagda's `dagda-migrate --dry-run` on macnessa against the copy at
   `/root/macha-state-20261105T145340Z` (`ORIGIN.txt` says how it was taken; the loose sample
   was copied after restart, from objects older than the stop; `macha-metadata-dump.txt`
   beside it replays cleanly). Build as Dagda specified under `/home/tom/dagda-linux`,
   throwaway custody in a scratch node-state, whole output, elapsed time and disk back to it.
   The key stays on the node until the operator rules otherwise. The copy names the library.
2. Answer whatever the libraries raise; otherwise the questions below wait on him.

## The port to Dannan and Dagda

Evaluation: [`2026-11-04-dannan-dagda-port-evaluation.md`](2026-11-04-dannan-dagda-port-evaluation.md),
kept current. Rulings so far (memory `project-port-rulings`): two cutovers,
transport first then the disk, with a soak between; a Macha case may be retired
against a named library case, recorded case by case; reverse migration built
but not a gate; torrent bytes stay pinned in Macha's tests; Dannan's respec
confirmed, Pi and real IGD work held.

- **Dannan** planned on cac5998: `../dannan/TODO/ACTIVE.md` "The first cutover",
  `2026-11-05-first-cutover.md`, `2026-11-05-macha-test-mapping.md`. Branch
  `feature/macha-parity`, unpushed, steps 1 and 2 of 11 done, at step 3 when the
  operator says continue. 0.92.5 touches nothing in its layer. Proposing a generic class
  admission in `dannan::sched`, ledger first, then Dagda; Macha's view sent (the
  `io_pressure` group is the gate; Macha's own arbiter stays Macha's).
- **Dagda** planned on cac5998: `../dagda/TODO/ACTIVE.md`,
  `2026-11-05-second-cutover.md`, `2026-11-05-test-mapping.md` (786 cases),
  `2026-11-05-heads-plan.md`. Local on develop, unpushed. It took 0.92.4's shapes and
  three differences of its own (question 1). Told to build heads against cac5998, not
  to wait on `feature/tree-native-namespace`: that is Macha's finding under the freeze,
  not a ruling (Open work 2).
- **Macha's code is frozen until the libraries are ready (operator, 2026-11-05):**
  "Macha has a working, tested codebase, this shouldn't be modified until they're
  ready." One exception so far, 0.92.5, by his "Yes fix now"; deployed, so the
  libraries' baseline is 808fa0f, which touches only `maintenance.cpp` and a test. Artefacts for their
  plans are fine, changing no tracked file, under `build/macha-fixtures/`, each with a
  README: `placement/`; `goldens/` (Status 381 of 494 keys by name, journal 31 of 83
  lines, 46 config fields, routes read from source); `state-scratch/` (inline, **not the
  live form**); `membership-tombstone/`; `state-macnessa/` (three roster files, read-only
  off the running node, approved through Dannan). `gen/` holds the generators.
- **Pi baselines**: Macha's, later, at his word, with the deployed version.

## Questions for the operator

1. Dagda's three findings, fix in Macha? Read here, not reproduced. `survey_by_diff`
   (`availability.cpp:94`) takes `MaintenanceStopping` for a peer failure: the stop stays
   prompt, but on the `kept` and `followed` table paths a stopping survey runs on, and
   `followed` writes that table durably; recommended a rethrow. `ClaimLocks::deleting_` is a
   set, so of two deletes of one id the first to finish clears the mark; recommended a
   multiset; whether two can coincide is not traced. `ReferencedSets::not_held`'s `others`
   loop (`horizon.cpp:107`) has no pause. Also the inventory walk's stop check (`namespace_objects`,
   `maintenance_objects_cached`), and `storage_pool.cpp:603` (Open work 00), which Dagda has too.
2. **Maintenance loop.** Macha's session proposed that Macha's pass stays the one
   loop on a Macha node, which reopens his 2026-11-04 decision that maintenance
   moves into the node. Dagda's recommendation: Macha's loop until heads move at
   step 8c, Dagda's for a Dagda-only node, `MaintenanceOptions::automatic = false`
   between. Dagda has this and is holding the dependent mapping rows.
3. Dannan's rulings A to D: **ruled by the operator to Dannan, 2026-11-05.** A:
   every call names its deadline, no default; Macha's call sites adapt in the
   port (not the 24 h constant both recommended). B: the byte change accepted,
   rows with no thread go, Dannan's three loops take Macha's names. C: Dannan
   adds a per-pool thread CPU reporter. D, as replaced later the same day: the
   session and transcode limits per account stay Macha's (`reserve_session_slot`,
   `playback.cpp:1766`); Dannan builds no per-principal bound. His rule: bounds
   exist for fair usage and preventing problems, and every Macha budget in
   Dannan's layer (transfer, network, memory reserves, queues, reassembly) must
   be representable in Dannan; Dannan's step 8 tables them, asking Macha where a
   row needs it.
4. Zero storage: `data_storage` reads ready with no backend online. Dagda leaves
   it unready. A node whose only disk is offline at start must not crash-loop on
   the startup timeout, so perhaps "ready, degraded".
5. `mutate_delta`'s no-op (`metadata_manager.cpp:1919`) is unreachable, so a mutation
   that changes nothing still commits its clock. Macha's behaviour stands until he rules;
   0.92.5 removed the source of such commits on the nodes, not the rule. A policy commit
   carries no dot (`:1376`).
6. `read-ahead` in `frame_timings` and `read_ahead` in `traffic.classes` are the
   same class; unifying them is an API change for Core and the clients.
7. `query()` and its predicates have no production caller: retire as dead code?
8. Design defects left from Dagda's list: the three-concurrent-adds torrent merge
   is not associative; no epoch fence on actions under a torrent claim; the local
   `durability_barrier` has no deadline; the catalogue sweep mixes wall and steady
   ages; the policy disagreement loop; O(state) work per delta; wire-sized
   allocations; reload silently ignoring `write_copies`; no disk I/O control
   reserve and no control store free-space reserve.
9. Delete `feature/ebooks`, `experiment/dagda-backend` (never used), local and
   origin? `feature/loop-2` and `feature/dagda-found-defects` are merged, local only.
10. **Removing the pre-tree code**, once the freeze lifts. The condition the
   operator set is met: both nodes' heads are tree-backed (Cluster state), checked
   with Macha's own decoder, and the only inline records anywhere are genesis
   (generation 1, in every cluster) and the `*.pre-migration.*` archives. In
   order, each a release with its tests rewritten: (a) found a namespace as a tree
   at its first commit and require the namespace store (b080c38's 623c202), after
   which no new cluster is ever inline; (b) remove `mutate()`, the migration tool,
   `install_migrated_head` and `plan_namespace_migration` (5f6101c, 8fc5a68), and
   delete the archives on the nodes; (c) make genesis a fixed tree-backed or
   constant record so no founder decodes an inline snapshot (not in the branch),
   then remove `encode_snapshot`, the SM5 to SM13, SM15 and SM18 decoders, the
   mixed branch of `read_group` (`metadata_manager.cpp:1214`), `metadata_delta`'s
   map diff and the inline-tombstone path of `maintain_tombstone_batches`; (d)
   remove legacy authority: `required == 0` certificates, `migrate_legacy_head_locked`'s
   legacy branch, `acceptance_matches_record_policy_locked`'s legacy clause,
   `accept_pristine_genesis_authority_` and the `current.meta`/`committed.meta`
   layout reader (`metadata.cpp:2412`), none of which has a live instance (both
   certificates are `required=1`; no protocol-20 node exists). The legacy clock
   (SM17, DLT10) is a live feature, not old code. Dagda counts 45 of the 69
   `test_storage_metadata` cases as using a legacy form: that is the cost of (c)
   and (d).

## Open work, in order

00. **macnessa's diskB left the USB bus 2026-11-05 10:16:26Z**, under undervoltage;
   macha stopped cleanly. Back and serving from about 11:56Z after a physical knock;
   ext4 journal replayed on mount. Operator: cease the investigation. SMART at that
   point: no reallocated, pending or uncorrectable sectors, no logged errors. Short
   self-test unfinished when dropped, extended never run; the drive has no shock
   attribute, so this says little about the knock. Reports about 41,000 spindle hours
   against 13,426 power-on hours. `smartmontools` and `smartd` now on macnessa.
   **Open defect, read not reproduced:** `storage_pool.cpp:603` deletes an object on any
   read exception, so a read failing on I/O deletes a good object. It fired once at the
   failure (`0ec5f3a8...`). Held by the freeze, with the operator.
0. **P0: macnessa SIGSEGV, 2026-11-05 02:05:25Z, on 0.92.4, cause unknown.** 25 min
   after the deploy; the first SIGSEGV of this boot (up since 11-03 23:27, every
   earlier exit a stop or SIGKILL). No core (soft limit 0, pattern `core`), no kernel
   line, no warning in the journal; ingest and a catalogue scan were running, and the
   rail was in an undervoltage interval, as it often is. Restarted by systemd, writable
   in 35 s, 6 FUSE operations recovered. Core capture is on, both nodes, since 02:22Z:
   cores in `/mnt/diskB/crash` (mode 700; they hold keys and plaintext), set by
   `/etc/sysctl.d/90-macha-core.conf` and `macha.service.d/core-dumps.conf`; proven
   with a throwaway process. Read one against the tarball of the version that crashed, in `/root`.
1. **P0: the stop.** Fixed in 0.92.4 for the survey: macnessa on 0.92.3 hung at
   01:38:29Z, killed at 60 s, in `fill_path_table`; on 0.92.4 a stop 20 s after writable
   took 6 s. One stop, not shown to have landed in a walk; no test of its own. The FUSE mount's
   minute after start is recovery, not a defect (data ready +17 s, metadata +35
   to +40 s at generation 100k, FUSE +42 s).
2. **`feature/tree-native-namespace`** (b080c38, 4 WIP commits on 5a06d3c, unmerged,
   held by the freeze, not a port prerequisite). From its diff: founds a new namespace as a
   tree at its first commit (develop founds inline, and only `macha-namespace-migrate`
   makes a tree); removes `mutate()`, the migration tool, `install_migrated_head` and
   `plan_namespace_migration`; requires the namespace store; skips re-accepting a head
   whose certificate adds nothing; fixes the stale tombstone (taken as 0.92.5) and the
   `followed` path table keeping a lost file complete (`availability_service.cpp:531`, on
   develop by reading, not taken). It does **not** make genesis a constant
   (`metadata.cpp:2294`), remove legacy authority or remove the inline decoders; earlier
   notes said it did. Question 10 has the removal order.
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

## 8. Waiting on the operator

- The replication-by-diff proposal's three open questions.
- **Lost posters on the viewer's path**: the artwork route could answer 404
  at once for an object the last survey found no node holding (442 catalogue
  objects today), saving 1 to 2 s per showing; the cost is that a poster a
  peer gained since that survey is refused until the next one. Or the
  catalogue re-fetches lost artwork from its provider and nothing changes on
  the read path.
- The bootstrap test (`test_two_node_mutual_bootstrap_metadata_write_floor`): the first write waits for the bootstrap
  checkpoint survey (not the availability survey), or the test retries the
  refusal.
- The memory limit to set on each node (section 3, Memory).
- CPU priority for background work: `nice` or `SCHED_IDLE` for repair,
  scrub, transcode and libtorrent threads, a cap on encoder threads, or a
  systemd `CPUWeight`.
- The size of a viewer rate reserve on a node's uplink: a fixed figure or a
  share of the measured link (repair yields time, not rate).
- [Cost-budgeted scheduling](2026-10-03-cost-budget-scheduling-spec.md): a
  proposal with six questions. Its memory section is partly overtaken by
  0.90.69/70 (one limit, every part reported).
- Metadata editor B: the choice of fields.
- Torrent staging option A or B; stages 3 and 4 of the disk backend plan.
- A `CONTRIBUTING.md` line: "A new gate may pace lower-class work; it may
  never stop it."
- The replica's applier lifetime.
- Operator question: a configured DATA disk not yet online reads every
  extent it holds `unavailable` (seen on fi-1 after a reboot, about 1M for two
  to nine minutes until the USB disk mounted); Core's web queue drops
  unavailable titles. Should it read `unknown` until the disk comes up?
- From the backlog: anonymous access; whether a client should know a
  playback generation existed anywhere in the cluster.

## Cluster state

- **macnessa** (root@10.44.1.50, hostname corvus-gbni-1, Pi 5, 8 GB, domain
  site-a): `/var/lib/macha`, DATA on `/mnt/diskB/data` (4.0 TB, about 1.03M loose
  objects, 17 packs on 2026-11-04), API on 10.44.1.50:7438. The build node.
- **inverbeg** (root@inverbeg.macha.network, Pi 5, 16 GB, domain site-b, outbound
  only): DATA on `/mnt/diskB/data` (about 670 GB and filling), API on
  192.168.1.50:7438.
- **Metadata, both nodes** (2026-11-05, `macha-metadata-dump --tree` and
  `build/macha-fixtures/gen/frame_forms.cpp` over the live histories): one
  tree-backed head, root `2fdd09bd...`, SM19 (`DHTMETB9`, 41,044 bytes) with the
  legacy clock set, 23 torrent requests, 347 tombstone batches, 11,112 tombstones,
  10 authors seen; tree 266 nodes, 8,713 entries, 1,251,866 extents, 63.6 MB;
  generation about 129,500, rising about 20 a minute on 0.92.4 (Resume here, step 3). Re-rooted by
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

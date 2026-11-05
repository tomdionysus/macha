# Active tasks

Last updated: 2026-11-05, on `develop`. Rationalised for a clear; the version
before is `archive/2026-11-05-ACTIVE-before-rationalisation.md`.
Alongside this file: `BACKLOG.md` (everything else, by area), `COMPLETED.md`,
`CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**Live.** macnessa (root@10.44.1.50, builds in `/home/tom/macha`) and inverbeg
(root@inverbeg.macha.network) run 0.92.3, one cluster, under `/usr/local`.

**Released, not deployed: 0.92.4**, commit b2e02ec, develop merge 70cf4a4, tagged
and pushed with main. The operator approved the deploy; the harness's permission
classifier refused the install on inverbeg, so nothing was installed. The tarball
is ready and checksummed (md5 `73353a0b...`) as `/root/macha-0.92.4.tgz` on both
nodes and `build/macha-0.92.4.tgz` here; the script is `build/install-0.92.4.sh`.
Contents, in the CHANGELOG: the hung stop (the
survey's walks take a pause that throws `MaintenanceStopping`); the claim guard
no longer spans a DATA unlink (`ClaimStore::deletion` checks and marks under
the stripe, unlinks under the mark; a claim finding the mark treats the object
as not present; `test_a_delete_marks_its_object_and_holds_no_stripe`); real RPC
counts in telemetry; the remote generation raised by CAS; a connection counted
after its identity check; torrent disk and playback threads supervised, with
the source guard widened; a REQUIRE off a plain thread; two tests off the seed
marker; three comments; the README's Tizen client; the trace fixture's step.

Gates for 0.92.4: macnessa 746, 20 and 21 of each suite, `SUITE_RC=0`, build
`-j3`. Laptop `build-nobooks` 674 of 675 before the fixture fix (the failure
`availability/test_two_nodes_survey_what_neither_holds`, the load-sensitive case
below; the group passes alone). macnessa's `/home/tom/macha` is that tree rsynced
over develop, so `git status` there is dirty until the next checkout.

**Not proven: the stop fix on a node.** It has no test of its own. The proof is
a stop within about a minute of writable on macnessa, under
`/root/stop-with-stacks.sh`, which hung on 0.92.3 at 00:00:33Z. It needs a
deploy. The other whole-namespace walk on that thread, the inventory
(`namespace_objects`, `maintenance_objects_cached`), has no stop check yet.

**The loop in progress** (operator, 2026-11-05): "Then we loop again.
Investigate and fix the identified issues if necessary, and ask both projects
to replan when done and tested." So once 0.92.4 is gated: send Dannan and Dagda
the replan request against the new commit, with what changed. Rule (operator):
genuine Macha defects are fixed in Macha first and the libraries match the fix;
where they differ otherwise, those projects move, not Macha.

## The port to Dannan and Dagda

Evaluation: [`2026-11-04-dannan-dagda-port-evaluation.md`](2026-11-04-dannan-dagda-port-evaluation.md),
kept current. Rulings so far (memory `project-port-rulings`): two cutovers,
transport first then the disk, with a soak between; a Macha case may be retired
against a named library case, recorded case by case; reverse migration built
but not a gate; torrent bytes stay pinned in Macha's tests; Dannan's respec
confirmed, Pi and real IGD work held.

- **Dannan** replanned on 00861dd: `../dannan/TODO/ACTIVE.md` "The first
  cutover", `2026-11-05-first-cutover.md` (Status keys, config, state files,
  journal lines, hosted fields, message numbers, findings F1 to F22),
  `2026-11-05-macha-test-mapping.md`. Macha confirmed the mapping and adopted
  the hosted ids 1024 to 1045, numbers 512 plus current, the advert names and
  the memory owners. Branch `feature/macha-parity`, working step 1 of 11.
- **Dagda** replanned on 00861dd: `../dagda/TODO/ACTIVE.md`,
  `2026-11-05-second-cutover.md`, `2026-11-05-test-mapping.md` (786 cases),
  `2026-11-05-heads-plan.md`. Macha answered its uncertain rows. It holds no
  heads until the tree-native merge. It takes the 0.92.4 shapes when
  `feature/loop-2` merges.
- **Macha owes, in order:** the config, Status and journal goldens; the
  tree-native merge; then the real state directory, `macha-metadata-dump`
  output and the there-and-back check under `build/macha-fixtures/`; the Pi
  baselines when the operator says; the rehearsal on the nodes if he agrees.
  Placement vectors are done (`build/macha-fixtures/placement/`, untracked;
  Dannan and Dagda match them).

## Questions for the operator

1. 0.92.4's deploy: approved, refused by the harness's permission classifier. It
   needs the operator to allow it or run it. (The deploy is also the stop fix's
   proof.)
2. **Maintenance loop.** Macha's session proposed that Macha's pass stays the one
   loop on a Macha node, which reopens his 2026-11-04 decision that maintenance
   moves into the node. Dagda's recommendation: Macha's loop until heads move at
   step 8c, Dagda's for a Dagda-only node, `MaintenanceOptions::automatic = false`
   between. Dagda has this and is holding the dependent mapping rows.
3. Dannan's rulings, with Macha's recommendation: A, a call with no deadline: a
   24 h facade constant. B, thread names and the ten net-* rows in
   `/status/diagnostics`: accept the byte change, rename the three loop threads
   to Macha's. C, hot-thread DIAG lines: Dannan adds a CPU reporter. D, the
   per-account bound stays Macha's.
4. Zero storage: `data_storage` reads ready with no backend online. Dagda leaves
   it unready. A node whose only disk is offline at start must not crash-loop on
   the startup timeout, so perhaps "ready, degraded".
5. `mutate_delta`'s no-op (`metadata_manager.cpp:1919`) looks unreachable: the
   reserved dot changes the clock first, so a mutation that changes nothing still
   commits. Read, not reproduced. A policy commit carries no dot (`:1376`).
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
   origin?

## Open work, in order

1. **P0: the stop.** Fixed in 0.92.4 for the survey; see above. The FUSE mount's
   minute after start is recovery, not a defect (data ready +17 s, metadata +35
   to +40 s at generation 100k, FUSE +42 s).
2. **The goldens**, then the **tree-native branch**
   (`feature/tree-native-namespace`, 4 WIP commits on an older develop, needs a
   merge): genesis as a constant hash, legacy authority and inline formats out.
   On merge, go through Dagda's re-check list (`2026-11-05-heads-plan.md` section 4).
3. **Books**, as the web client reported at 0.92.2 (49 books): the Discworld series
   holds 3 of 49; three books send a timestamp `published` (A Hat Full of Sky, Soul
   Music, I Shall Wear Midnight); **possible regression, unexamined:** 11 books
   send `published` and `language` null (Eric, Hogfather, Jingo, Maskerade, Mort,
   Pyramids, Snuff, Sourcery, Thud!, Where's My Cow?, Wintersmith), and Thud! lost
   its publisher too. "The Light Fantastic (Discworld)" keeps its clutter. "Le
   Guin, Ursula K." splits into two authors. Conversion is MOBI and AZW3 to EPUB
   only; per-user media state (stage 3) follows the tree-native merge. These nodes
   have no API account, so catalogue checks go through the client session.
4. **Failing tests, each P0 when it recurs** (*carried, not re-checked on the two
   current nodes*): `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`
   and `availability/test_two_nodes_survey_what_neither_holds` (laptop, load);
   `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor` (section 8);
   `filesystem_fuse/test_fuse_recovery_thousand_operations_have_bounded_publications`,
   `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt`,
   `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
   `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`,
   `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection`,
   `rpc_cluster/test_rpc_slow_control_does_not_abort_data`,
   `test_write_data_work_context_preserves_loader_provenance`,
   `rpc_cluster/test_inbound_incapable_node_is_reached_only_over_its_own_sessions`,
   the pacing check in `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`,
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
  merged. Stage 3, the per-user media state, follows the tree-native merge;
  OPDS is not started.
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
- Both on Debian 13, aarch64, Boost 1.83. Neither API answers on loopback, and
  neither has a Macha account for this session or allows anonymous sessions.
  `books.enabled` and `books.conversion.enabled` are on; `libmobi-tools` installed.
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

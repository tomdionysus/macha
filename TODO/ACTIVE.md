# Active tasks

Last updated: 2026-10-12, on `develop`. Both nodes run 0.90.86.

The ordered list of open work; work top to bottom unless new evidence
changes the order. Alongside it: `BACKLOG.md` (everything else still to do,
by area), `COMPLETED.md` (finished work), `CLIENT-CONTRACTS.md` (what the
client sessions depend on) and `archive/` (past plans, evidence and
handovers; a record, not requirements). Rationalised on 2026-10-09 after
the object ledger, tombstone batches and the memory limit changed the plan;
the version before is `archive/2026-10-09-ACTIVE-before-rationalisation.md`. Section 2 was
re-cut the same day around the replication-by-diff proposal: the items it
closes sit under the stage that closes them.
Items marked *carried* come from earlier rationalisations and have not been
re-checked since.

## 1. Failing tests and defects

A failing test is P0: reproduce it before deciding whose it is.

- **`rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor`**:
  failed in full runs on 2026-10-05 and 2026-10-07; 24 of 24 under
  `--repeat`. A first write while two nodes form a set is refused
  (`waiting for bootstrap checkpoint survey`) when the peer's answer is late,
  and the test treats the refusal as fatal. Needs the operator's call
  (section 8).
- **`availability/test_two_nodes_survey_what_neither_holds`** exceeds its
  20 s wait under heavy laptop load (3.4 s alone). The survey it tests is
  retired at section 2's stage 5; fix only if it fails on fi-1.
- **gbni-1's memory through a multi-file import** on 0.90.34's bounded owed
  queue: now measurable by part (the `memory_*` gauges, 0.90.69).
- **gbni-1 heap corruption**, three times. An ASan 0.84.0 build and
  `/root/claude-missing-extent-driver.py` are staged on fi-1, not run.
  *Carried.*
- **Test failures**, each P0 when it recurs: *carried*
  `filesystem_fuse/test_fuse_recovery_thousand_operations_have_bounded_publications`,
  `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt`,
  `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
  `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`,
  `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection`,
  `rpc_cluster/test_rpc_slow_control_does_not_abort_data`,
  `test_write_data_work_context_preserves_loader_provenance`,
  `rpc_cluster/test_inbound_incapable_node_is_reached_only_over_its_own_sessions`,
  the pacing check in
  `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`
  under debug logging, and
  `users/test_accounts_converge_across_nodes` (a sign-in over 2 s under a
  loaded laptop; with 0.90.64 a busy slot answers `try_later` instead),
  `rpc_cluster/test_rpc_metadata_executor_cancellation_disconnect_and_shutdown`
  (laptop, 2026-10-12, `exit=1` with no assertion printed, in a three-test
  group; passed alone and in the group after).
- **The macOS torrent segfaults are the laptop's libraries, not Macha**
  (2026-10-12). The five cases that start a session crash every run, in
  libtorrent's own thread opening its listen socket (kqueue descriptor pool,
  null allocator). The test executable compiles boost.asio from MacPorts
  (`/opt/local/include`, 1.91) and exports a weak
  `service_registry::create<kqueue_reactor>`; dyld binds libtorrent's
  `use_service` to it, so Homebrew's libtorrent (2.1.1) reads a reactor
  another asio laid out. A bare session, a forked one, one on Macha's disk
  backend and one linked with every library of the test all run; a build
  against Homebrew's Boost 1.92 crashes the same, so the bottle's asio
  differs from that too. Linux is unaffected (distro libtorrent and Boost
  together). Fix: one Boost and libtorrent from one package manager on the
  laptop (the operator's machine; not changed).
- **Ingest and merges**: the ingest re-bases on a stale basis since
  0.90.79; a merge logs each conflict's key and an ingest stopped mid-put
  resumes since 0.90.83 (the stop cancelled filesystem I/O before it told
  the ingest to stop). Publications batch (operator, 2026-10-12: done). The
  merge rule (take the newer when one side is the other's ancestor) and the
  reconciler (one pair choice everywhere) from the 2026-09-23 diagnosis are
  watched, not built: checked 2026-10-12, the only standing conflicts are
  the seven from before 0.89.0, though both nodes merged up to 69 times a
  day from 10-04 to 10-10 (the observations' `metadata.reconcile_us`); the
  braid needs three heads, and two nodes fold the same pair. A
  `metadata merge recorded a conflict` WARN (0.90.83) reopens it with the key.
- **The unmatched list differs by node**: diagnosed 2026-10-12, fix waits on
  the operator (section 8). 674 on fi-1, 614 on gbni-1, 540 on both (same
  ids), 748 in all. Every file on one list only has no hint at all on the
  other: the scan coordinator submits the hints and keeps them in its own
  `catalogue/hints.json`, so each list holds what arrived while that node
  coordinated (fi-1's 134: Tiesto, Angry Boys; gbni-1's 74: the Enya
  imports of 2026-10-10).
- **`RPC stalled (control)`** each way at 09:47Z and 09:50Z on 2026-10-05,
  5 to 15 s without progress, then nothing. 0.90.65/66/68 removed three
  ways control could wait; watch for a recurrence. Recurred 2026-10-11:
  six control stalls of 5 s between the two nodes (`get_metadata_heads`,
  `members`; 10:27Z, 11:55Z, 14:27Z, 15:34Z), the 15:34:39Z one both ways
  in the same second, which points at the link (fi-1's mobile connection)
  rather than either node; loader polls stalled alongside. None after
  22:24Z that day (checked 2026-10-12 09:40Z).
- **A failed convergence run's retry: built, 0.90.82.** Events during its
  backoff bring it forward, `maintenance.convergence_settle_ms` (1 s) after
  the last and at most `convergence_max_delay_ms` (10 s) after the first.
  Diagnosed 2026-10-12: the events did not end the wait, and the unrelated
  wakes that did (every 4 s at the median on the cluster) each retried at
  once, so the backoff bounded nothing while events streamed.
- **fi-1, 2026-09-29**: a playback `PATCH` 503 after 19.9 s under software
  transcode, and a `FUSE mount disappeared` ERROR during a restart. *Carried.*
- **Transport backoff after a peer restart**: measured 2026-10-12 and left
  as is. Dial backoff is 250 ms doubling to 4 s, cleared by a successful
  dial. gbni-1's 0.90.81 restart (12:56:34Z listening) had fi-1's control
  and data lanes up at 12:56:35Z: a deliberate restart is down about 3 s,
  one refused dial, never near the cap. Only an outage longer than about
  8 s reaches 4 s, which then adds at most 4 s to it.
- **Known defects of 2026-09-24/25**: the acquisition API audit's four
  findings: a pause or cancel overwritten by a failing ingest, fixed
  0.90.84; the other three change the API and are built and tested but
  held out of the tree (full files in `build/api-held/`, the laptop's
  scratch directory) until Core and the clients are told (no client
  session was running, 2026-10-12; operator: send
  `build/api-held/ANNOUNCEMENT.txt` to Core when it is next up, then ship):
  one `catalogue.state` rule for an ingest and its torrent (a completed
  import with nothing counted read `completed` on the ingest, `waiting` on
  the torrent); a remote job's single `GET` asks its owner, so
  `catalogue.items` is the owner's, or null when it cannot be asked (was an
  empty array); torrent search results in provider order, not sorted by
  seeders. (The install docs' Fedora and MacPorts package names were
  checked 2026-10-12 against Fedora 43/44 and the MacPorts index: all
  exist, `zlib-devel` through `zlib-ng-compat-devel`, `macfuse +fs_link`.) (The
  bare metadata errors: one is gone, the other throws `MetadataNotReady`
  since 0.90.78.)

## 2. Replication by diff (the next large piece)

Proposal: [`2026-10-09-replication-by-diff.md`](2026-10-09-replication-by-diff.md),
written and approved 2026-10-09. Its rule: a node's holdings have one
identity, the `held` tries' root hashes, and it is the only signal that
they changed; nothing keys a holdings memo on proxies. One subtree-hash
diff between two nodes' `held` tries, joined locally with `referenced`,
replaces the availability survey and its roll-up, repair's walks and
probes, and the inventory's `outside_namespace`. Each stage ships alone;
the walks stay as the fallback until every node runs stage 3.

0. **The holdings identity: built, 0.90.71.** `held_view()` on every
   store; the survey keyed on (head, identity) only; `indexed()`,
   `held_indexed()`, the loss counts, the cold wait and the kept roll-up
   deleted. Closed `StoragePool::indexed()` and the missing re-roll when a
   disk comes back. Found on the way: packed objects were never in the held
   ledger; they are now, and each node's first start lists its packed
   objects (watch gbni-1's start time).
1. **The trie's diff surface: built, 0.90.72.**
2. **`trie_diff` over the wire: built, 0.90.72.** The survey diffs held
   tries when every peer runs 0.90.72 with one DATA disk; else the tree.
   Closes, once deployed and read on the cluster: **extents held by no node
   online** (about 203,000 of 940,000; five of 25 sampled movies EIO on
   both nodes, which is correct) as `unavailable`; the memo going stale
   when a peer's holdings grow (the diff has no memo); **per-file
   readability**'s answer. Read after deploy: `method=diff` in the survey
   debug line, its rounds, and `availability.survey_us` against before.
   **0.90.71 and 0.90.72 are committed but not deployed**: the deploy
   script was refused by the auto-mode classifier; fi-1's suites passed
   for 0.90.71 on a clean build.
3. **Repair on the diff: the push list built, 0.90.76.** Measured first (0.90.75, 2026-10-11, gbni-1
   pushing to fi-1): a local read is 40 ms an extent; a two-extent batch
   waits 3.95 s for its answers; repair's bandwidth estimate is 1.25 to
   1.34 MB/s and it moves about 0.74 MB/s idle, one extent a minute with a
   viewer. **The path is the limit, not repair:** a raw TCP stream gbni-1 to
   fi-1 (fi-1 dialling, as Macha does) runs at 1.39 MB/s, and four parallel
   streams total 1.3 MB/s: fi-1 is on a Finnish mobile connection, and its
   downlink is the cap (gbni-1 is on 1 Gbps fibre). So the pipelined window would not speed repair between these
   nodes; what stage 3 still buys is the push list from the diff instead of
   walking every local object, and no probes (already true when the lacks
   come from a diff, since they carry the inventory's generation). fi-1
   lacks about 570,000 extents gbni-1 holds (about 2 TB): about 18 days at
   the path's full rate, about 30 at repair's idle rate.
4. **Catalogue DATA: first part built, 0.90.77.** The inventory's referenced
   set already held the catalogue's objects, so diff-based lacks (0.90.72)
   and the push list (0.90.76) cover them; the survey now counts a catalogue
   object no node holds as `unavailable` (442 on fi-1's first 0.90.77
   survey, 2026-10-11), so repair stops asking for it. Left: the artwork
   route still asks peers for a poster nobody holds (1 to 2 s each showing;
   operator's call, section 8); re-fetching lost artwork from its provider
   is the catalogue's.
   Previously planned as: **Catalogue DATA as a `referenced` source** (the catalogue plan's stage
   10). Closes: **artwork held by no online node** (60 of 296 posters,
   2026-09-27) found and pulled, or reported `lost`; the 1 to 2 s remote
   probe per missing poster; `GET catalogue/status` asking the store for
   every artwork (`catalogue.cpp:896`, from the backlog). Catalogue
   mutations already compute released DATA per touched shard (0.90.33);
   whether stage 4 needs the plan's per-family shards (section 4) is for
   its design.
5. **Retire** `tree_holdings`, the roll-up, survey and memo,
   `availability/holdings.bin`, the repair cursors and repair's `have_*`
   probes, once every node runs stage 3.

The proposal's open questions (for the review): whether `claimed` is diffed
(decide from the claim walk's probe counts after stage 3); a count bound on
the pipelined window beside bytes; whether the counts (lacking here, per
peer, below target, lost) become API resources now that they are cheap.

Still separate, in this area:
- **Bringing es-1 back** (operator's question, 2026-10-07): install the
  current release before its first start; its 0.57-era config will be
  refused (`dht.min_write_replicas` and others renamed in 0.88.0, keys
  removed in 0.90.17); back up its state directory first; files deleted
  elsewhere since 2026-09-24 may reappear from it (0.89.0). Its first start
  on the current release seeds its held ledger, migrates its claims and
  counts its namespace; under the proposal it joins the diff once seeded,
  and answers "cannot say" until then. A rehearsal on a test cluster from
  0.57.0 is offered, not done.
- A torrent can land on a node away from where it is watched.
- Per-peer down state in the transport; `fsync` without a deadline.
- Seven namespace conflicts standing from before 0.89.0.
- Say why a node counts itself busy (the pacer's active classes in status).

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
- Done 0.90.85: the reverse dial's DEBUG lines under `RpcClient::mutex_`;
  the `mutex_` then `Membership::m_` order, documented at `mutex_`.
- gbni-1's DATA barrier (`syncfs` on `diskB`) is mean 484 ms, max 5.1 s
  under 20 torrents: it flushes torrent staging and the FUSE spool on the
  same disk, and libtorrent's writes keep no write-behind; viewer and loader
  DATA writes wait on it. libtorrent's own sockets use cubic and can fill an
  uplink. (Recorded; not control.)
- Whether a commit's control objects should reach every node present with
  the head (claims stop at `metadata_write_copies` holders).

## 7. Features, API and optimisation

- **Announce 0.90.64's `try_later` on the account routes** to Core and every
  client at the next session (recorded in CLIENT-CONTRACTS).
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
- **Movie sets** and **ebooks**: later; each needs a proposal first.
- Measured small, not now: ingest and data publications commit one
  operation at a time (about 100 ms each); the hint store rewrites its file
  per change (266 KB with 409 hints); the head record's largest part is now
  its 72 torrent requests (only if they grow).

## 8. Waiting on the operator

- The replication-by-diff proposal's three open questions.
- **The unmatched list across nodes** (section 1). Recommended: the list
  answers the union, as the job lists do (local hints plus each peer's from
  its last poll, never waiting on a peer), and an action on an id goes to
  the node holding that hint, as job actions do. The shape is unchanged;
  what it lists changes, so Core is told first. The alternative, hint
  outcomes replicated in metadata, is heavier and not needed for the list.
- **Lost posters on the viewer's path**: the artwork route could answer 404
  at once for an object the last survey found no node holding (442 catalogue
  objects today), saving 1 to 2 s per showing; the cost is that a poster a
  peer gained since that survey is refused until the next one. Or the
  catalogue re-fetches lost artwork from its provider and nothing changes on
  the read path.
- The bootstrap test (section 1): the first write waits for the bootstrap
  checkpoint survey (not the availability survey), or the test retries the
  refusal.
- The memory limit to set on each node (section 3).
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
- The replica's applier lifetime; fi-1's USB power.
- From the backlog: anonymous access; whether a client should know a
  playback generation existed anywhere in the cluster.

## Cluster state

- **gbni-1** (10.44.1.50, `macnessa.macha.network`) and **fi-1**
  (10.35.1.10) run **0.90.86**, cluster protocol 23. es-1 is offline
  indefinitely.
- Metadata writable 2/2. `dht.write_copies` and `dht.metadata_write_copies`
  are copies sought, not floors: a node alone still accepts writes.
- Both nodes' config: `maintenance.foreground_weight: 80`,
  `repair_weight: 20` (the code default is 95:5); `garbage_grace_ms` 30 days;
  `catalogue.api.max_connections: 128`;
  `dht.metadata_materialization_cache_bytes: 512M`;
  `streaming.segment_memory_bytes: 64M`; no `runtime.memory_bytes` (no
  limit). Neither sets `control_workers` or `max_concurrent_password_checks`
  (defaults 4 and 2). fi-1 sets `hosts_extents: true` and
  `inbound_capable: false`; gbni-1 resolves both automatically (true).
- On the state device (`/etc/macha/state`): `ledger/` holds each store's
  held ledger (with `verify-next`, the daily check's position) and
  `ledger/referenced/` the namespace's reference counts; `retention/` holds
  the claims tries (`ledger-data`, `ledger-control`) and `claims.log`.
  The head names 64 tombstone batches (control objects) since the 0.90.67
  migration; the head record is 87,546 bytes.
- Rollback: `/root/pre-<version>/` on each node holds the binaries and config
  in place before that version was installed (`pre-0.90.70` back to
  `pre-0.89.0`). A node cannot go back before 0.90.55 (claims) or 0.90.67
  (tombstone batches) without losing state; see the CHANGELOG.
- gbni-1 also runs Plex Media Server, which reads `/mnt/diskA` (the exFAT
  ingest source, bound at `/media/Movies` and `/media/TV`); Macha never
  touches that disk except to ingest from it.
- fi-1's `/root/macha/build-asan` and `build-coverage` hold some macOS
  objects; the trees need a clean rebuild before reuse. fi-1's
  `/root/macha-bisect` is 0.90.46's tree; it can go.
- Both nodes log at DEBUG. gbni-1 keeps its heap-check drop-in
  (`/root/heap-check.conf.keep`).
- Observation windows: `/etc/macha/state/observation/observations.jsonl` on
  both: control's commit times (`metadata.*_sync_us`, `fuse.journal_sync_us`,
  `durability.barrier_us.<dir>`), the ledger check (`ledger.verify.*`), the
  `memory_*` gauges, `rss_bytes`, viewer presence by bytes on the wire.
- Helpers: `build/deploy.sh VERSION` (stages from fi-1's build, installs
  fi-1 then gbni-1, rolls back a node that does not come up). Run it in the
  foreground and check both nodes' versions after: on 2026-10-09 a slow
  copy outran its 90 s fi-1 check and it stopped before installing gbni-1
  (`build/install-<version>.sh` then installs one node by hand).
  On fi-1 `/root/mapi.sh`, `/root/apitime.sh`, `/root/burst-delete.sh`,
  `/root/ryw-test.sh`, `/root/.macha-cold-read.py`,
  `/root/.macha-writebehind-probe.py`; on gbni-1 `/root/warm-io.sh`,
  `/root/match-trace.sh`, `/root/remove-empty-dirs.py`;
  `macha-metadata-dump <key> <history.log> <heads.meta> --stats --objects
  /etc/macha/metadata-objects` reads a node's head.
- Code lookups go through basemind (load its tools first; rescan after edits
  and moves).

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
- **Proportion.** Account for and control what matters to the user's
  experience; no nit-picking that does not improve it (operator,
  2026-10-09).
- **Testing**: design it properly, test it well once (the touched tests on
  the laptop, then all three suites on fi-1, which are the gate), deploy,
  measure, iterate. No repeat runs, soak runs, sanitizer builds or mutation
  sweeps; never rerun a whole suite on an overloaded laptop. A laptop under
  heavy load (load average over about 50) gives no verdict on timing.
- **Report findings before fixing; prove a mechanism before editing.**
  Design questions (formats, budgets, semantics) go to the operator with a
  recommendation.
- **Every response has a snake_case code; errors add a message; clients own
  sorting and presentation; every API change is announced to Core and every
  client before it ships.** Client sessions may ask; they do not decide.
- **Deploying**: build once on fi-1, run all three suites there, ship the
  `usr` tree; never compile on gbni-1; never both nodes down at once. When a
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

# Active tasks

Last updated: 2026-10-08, on `develop`. Both nodes run 0.90.53.

The ordered list of open work; work top to bottom unless new evidence
changes the order. Alongside it: `BACKLOG.md` (everything else still to do,
by area), `COMPLETED.md` (finished work), `CLIENT-CONTRACTS.md` (what the
client sessions depend on) and `archive/` (past plans, evidence and
handovers; a record, not requirements). This file was rationalised on
2026-10-08; the version before is
`archive/2026-10-08-ACTIVE-before-rationalisation.md`. Items marked
*carried* come from earlier rationalisations and have not been re-checked
since.

## 0. 0.90.52 across a gbni-1 restart: better, not whole

Restarted gbni-1 at 12:41:18Z on 2026-10-08 with `holdings.bin` kept. fi-1's
surveys: refused for about 30 s while gbni-1 started ("tree_holdings is not
available on this node"); then answered from the kept roll-up, but each
question about a subtree gbni-1 does not hold whole asks `held()` per extent,
a hard-disk lookup while the presence index warms, so questions hit the 30 s
RPC limit until 12:50 (unknown 781,051, then 518,508, 243,655, 158,551); whole
from 12:51 once the index warmed (525 s). Twenty blind minutes became nine
partial ones. The rest is `held()` from the ledger (section 1, stage 2).

## 1. The on-disk object ledger

Design: [`2026-10-08-on-disk-object-ledger.md`](2026-10-08-on-disk-object-ledger.md).
`held` lives in a per-class, disk-resident radix-256 trie with a journal
and copy-on-write checkpoints on the state SSD and a configurable page
cache (64 MiB default), so a restarted node knows what it holds in seconds
instead of walking its DATA disk for 20 minutes; then claims, `referenced`
and `unreferenced_since` move in; then the diff RPC drives repair. Built
in-tree, generalised from the retention store's journal and checkpoint (one
mechanism, not two). The operator answered the spec's five questions on
2026-10-08; they are in the design. Stage 1 is 0.90.53: the shared
`SealedJournal` (the retention store moved onto it unchanged) and
`ObjectTrie`, tested and measured (laptop, 224,000 records: open with a
near-full journal 437 ms, cold lookup 10 us); fi-1's figure at a few million
records is still to take. Stage 2 is 0.90.54: every store keeps a held ledger under
`<state_path>/ledger/`, seeded once from the first start's walk, then read at
every start (indexed at once, no walk) and answering `has()`. To confirm on
the cluster: the seed on the first 0.90.54 start (a log line "storage held
ledger seeded"), then a gbni-1 restart that is indexed at once and whose
peers' surveys never fail. Then stage 3, claims in the ledger. Still to do
in stage 2: the slow background verification pass, and a bulk seed that
does not hold flushes off while it builds (today a put waits for the seed
on the first start).

## 2. Control must never wait: what is left

From the lock audit (done, 0.90.45 to 0.90.49) and the CPU and memory audit
of 2026-10-08, ranked:

1. One metadata-mutation worker (`metadata_workers` 1) serves every class
   first come: a control accept waits behind history imports and checkpoint
   rewrites (`net.cpp:3727-3736`, `:4437`); its queue limits are shared.
2. Background work travels as control: maintenance's head repair
   (`maintenance.cpp:1045`, a full history record on the peer) and cluster
   job polling and torrent actions run on the 2-worker RPC control lane;
   `class_allowed` forces it (`net.cpp:438-445`). It can also take the 64 MB
   control memory reserve.
3. Whole-record copies inline on control threads (`get_metadata`,
   `get_committed_metadata`, `accept_commit`'s decode).
4. Account create, update and root reset run scrypt outside the password
   check limit (4 concurrent fill the 4 control workers; admin only).
5. The memory ledger counts few large structures (metadata, catalogue and
   cache owners declared, never charged), so the control reserve protects
   counted bytes only; `restore()` overcommits without the reserve.
6. `MetadataReplica::compact_if_needed` still writes the checkpoint under
   `m_` (reached only from `compact()`).
7. Not fully traced in the lock audit: `RpcClient::mutex_` holders and
   `Membership::m_` bodies.

Waiting on the operator (section 9): CPU priority for background threads
(audit item: no nice, SCHED or cgroup weight anywhere; an x264 transcode
uses every core) and the size of a rate reserve for viewers (repair yields
time, not rate, and kept about 3 Mbit/s of an uplink a viewer was using).

Recorded, not control: gbni-1's DATA barrier (`syncfs` on `diskB`) is mean
484 ms, max 5.1 s under 20 torrents, because it flushes torrent staging and
the FUSE spool on the same disk and libtorrent's writes keep no
write-behind; viewer and loader DATA writes wait on it. libtorrent's own
sockets use the system's cubic and can fill an uplink. Whether a commit's
control objects should reach every node present with the head (claims stop
at `metadata_write_copies` holders).

## 3. Failing tests and defects

- **`rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor`**:
  failed in full runs on 2026-10-05 and 2026-10-07; 24 of 24 under
  `--repeat`. A first write while two nodes form a set is refused
  (`waiting for bootstrap checkpoint survey`) when the peer's answer is
  late, and the test treats the refusal as fatal. P0; needs the operator's
  call: should the write wait for the survey, or the test retry.
- **`availability/test_two_nodes_survey_what_neither_holds`** exceeds its
  20 s wait under heavy load (laptop load 58 to 190; 3.4 s alone).
- **gbni-1's memory through a multi-file import** on 0.90.34's bounded
  owed queue is still to be seen; memory per component in the observation
  windows (they hold total RSS only).
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
  under debug logging, the macOS-only torrent segfaults, and
  `users/test_accounts_converge_across_nodes` (a sign-in over 2 s under a
  loaded laptop; scrypt contention suspected).
- **An ingest dies on EAGAIN from a checkpoint commit** instead of re-basing
  on the current entry; log the conflict key when a merge records one; an
  ingest that dies when its node restarts mid-put. *Carried.*
- **Artwork held by no online node** (60 of 296 movie posters, 2026-09-27),
  and nothing re-fetches lost artwork; a missing poster costs a 1 to 2 s
  remote probe each time it is shown (seen 2026-10-07)
  ([write-up](archive/2026-09-27-missing-artwork-and-single-copy-writes.md)).
- **The unmatched list differs by node** (771 on fi-1, 822 on gbni-1): hints
  are node-local.
- **`RPC stalled (control)`** each way at 09:47Z and 09:50Z on 2026-10-05,
  5 to 15 s without progress, then nothing.
- **A rejoin or follower convergence retry waits out its backoff**: wake it,
  debounced, with a cap from the first trigger. *Carried.*
- **Torrent and ingest staging has no mount check**: with fi-1's disk absent,
  staging wrote to the SD card under the mount point. *Carried.*
- **fi-1, 2026-09-29**: a playback `PATCH` 503 after 19.9 s under software
  transcode, and a `FUSE mount disappeared` ERROR during a restart. *Carried.*
- **Transport backoff after a peer restart**: up to 4 s before a restarted
  peer is dialled. *Carried.*
- **Known defects of 2026-09-24/25**: `metadata replica is still recovering`
  and `local metadata replica unavailable` raise a bare `runtime_error` an
  ingest would fail on; the acquisition API audit's four findings; unverified
  package names in the install docs. *Carried.*

## 4. The catalogue plan, stages 6 to 11

Plan: [`2026-10-06-catalogue-materialised-view.md`](2026-10-06-catalogue-materialised-view.md).
Stages 1 to 5 are done (0.90.27 to 0.90.33; COMPLETED). Next: stage 6, the
conflict merge per shard; then derived indexes for list, search and the
descendant scan; the manifest change (per-family shards, growable count);
batched profile publication; holdings for catalogue DATA; measure again.

## 5. Matching

- **The first track of a match pays the provider again**: the editor's
  search and artwork options were fetched, then the match looked the release
  up and downloaded the cover again (4.1 s for the first track, 0 for the
  second; gbni-1, 2026-10-06).
- Provider caches are per editor seat and lost on restart.

## 6. Replication and repair

- **Extents held by no node online**: five of 25 sampled movies (The
  Transformers: The Movie, Once Were Warriors, Evil Dead II, 28 Days Later,
  most of 12 Monkeys) return EIO on both nodes; the survey counts about
  203,000 of 940,000 extents unavailable. EIO is the correct answer.
  Whether es-1 holds them is not known either way: only its return settles
  it. fi-1 may also have dropped a copy it alone held at a restart between
  2026-09-15 and 09-24, when it did not host (the repair defect fixed in
  0.90.50); nothing recorded says whether it did.
- **Bringing es-1 back** (operator's question, 2026-10-07): install the
  current release before its first start; its 0.57-era config will be
  refused (`dht.min_write_replicas` and others renamed in 0.88.0, keys
  removed in 0.90.17); back up its state directory first; files deleted
  elsewhere since 2026-09-24 may reappear from it (0.89.0). A rehearsal on
  a test cluster from 0.57.0 is offered, not done.
- Repair is bound by the WAN: one step sends at most two extents and waits
  (about 4 MB in 5 s between fi-1 and gbni-1); pipelining would lift it.
- A torrent can land on a node away from where it is watched (the movie
  watched on 2026-10-08 was downloaded on fi-1 and read over the WAN).
- Expose the counts holdings give: extents this node lacks, extents each
  peer lacks, extents below their replication target.
- Per-peer down state in the transport; `fsync` without a deadline.
- Seven namespace conflicts standing from before 0.89.0.
- Say why a node counts itself busy (the pacer's active classes in status).
- Log the resumed push position at INFO.

## 7. Further optimisation (measured small; not now)

- The availability survey's memo applies only while no peer's holdings
  grow; with a peer importing, every survey asks from the root.
- Ingest and data publications commit one operation at a time: about 100 ms
  each, so a 4 GiB copy spends about 7 s committing.
- Repair keeps its positions after a commit and runs one more pass;
  tombstones mature one at a time.
- The non-namespace snapshot per commit: a few milliseconds.
- The hint store rewrites its file per change: 266 KB with 409 hints.

## 8. Features and API

- **The API is RESTful, all of it**: audit every route; identity resets
  become a resource (decided); `providers/artwork?ref=` and
  `providers/artwork/choose` are among the routes to change.
- **OpenAPI**: generated from a declarative route table that dispatch runs
  from, served at `/api/v1/openapi.json`. Agreed, unstarted.
- **Core's request**: `providers/artwork` taking `item_id`.
- **Per-file readability**: designed; the operator chooses where it goes.
  Two of its three uses are wire changes. (It would let clients show the
  unreadable movies of section 6 as such.)
- **A media-type context on torrent add** (operator, 2026-09-25).
- **People on catalogue items**: approved, with a backfill.
- **Placement API asks**: the target `node_id` and a viewer-facing detail on
  `placement_failed`; the peer's own refusal text on a forwarded add; an
  optional node `name` (operator's call).
- **Movie sets** and **ebooks**: later; each needs a proposal first.

## 9. Waiting on the operator

- CPU priority for background work: `nice` or `SCHED_IDLE` for repair,
  scrub, transcode and libtorrent threads, a cap on encoder threads, or a
  systemd `CPUWeight` (section 2).
- The size of a viewer rate reserve on a node's uplink: a fixed figure or a
  share of the measured link (section 2).
- The bootstrap test (section 3): the first write waits for the survey, or
  the test retries the refusal.
- Metadata editor B: the choice of fields.
- Torrent staging option A or B; stages 3 and 4 of the disk backend plan.
- A `CONTRIBUTING.md` line: "A new gate may pace lower-class work; it may
  never stop it."
- The replica's applier lifetime; fi-1's USB power.
- [Cost-budgeted scheduling](2026-10-03-cost-budget-scheduling-spec.md): a
  proposal with six questions.
- From the backlog: anonymous access; whether a client should know a playback
  generation existed anywhere in the cluster.

## Cluster state

- **gbni-1** (10.44.1.50, `macnessa.macha.network`) and **fi-1**
  (10.35.1.10) run **0.90.53**, cluster protocol 23. es-1 is offline
  indefinitely.
- Metadata writable 2/2. `dht.write_copies` and `dht.metadata_write_copies`
  are copies sought, not floors: a node alone still accepts writes.
- Both nodes' config: `maintenance.foreground_weight: 80`,
  `repair_weight: 20` (the code default is 95:5); `garbage_grace_ms` 30 days;
  `catalogue.api.max_connections: 128`. Neither sets `control_workers` or
  `max_concurrent_password_checks` (defaults 4 and 2 since 0.90.48). fi-1
  sets `hosts_extents: true` and `inbound_capable: false`; gbni-1 resolves
  both automatically (true).
- Rollback: `/root/pre-<version>/` on each node holds the binaries and config
  in place before that version was installed (`pre-0.90.53` back to
  `pre-0.89.0`).
- gbni-1's DATA presence index takes about 20 minutes to warm after a
  restart (1,180 s, 807,076 objects).
- gbni-1 also runs Plex Media Server, which reads `/mnt/diskA` (the exFAT
  ingest source, bound at `/media/Movies` and `/media/TV`); Macha never
  touches that disk except to ingest from it.
- fi-1's `/root/macha/build-asan` and `build-coverage` hold some macOS
  objects; the trees need a clean rebuild before reuse. fi-1's
  `/root/macha-bisect` is 0.90.46's tree, built for one comparison; it can
  go.
- Both nodes log at DEBUG. gbni-1 keeps its heap-check drop-in
  (`/root/heap-check.conf.keep`).
- Observation windows: `/etc/macha/state/observation/observations.jsonl` on
  both; since 0.90.44 they carry control's commit times
  (`metadata.*_sync_us`, `fuse.journal_sync_us`,
  `durability.barrier_us.<dir>`); since 0.90.40 viewer presence follows
  bytes on the wire.
- Helpers: `build/deploy.sh VERSION` (stages from fi-1's build, installs
  fi-1 then gbni-1, rolls back a node that does not come up; run it in the
  foreground, or check both nodes after: an interrupted run skips the
  rollback check); `build/install-<version>.sh` per release; on fi-1
  `/root/mapi.sh`, `/root/apitime.sh`, `/root/burst-delete.sh`,
  `/root/ryw-test.sh`, `/root/.macha-cold-read.py`,
  `/root/.macha-writebehind-probe.py`; on gbni-1 `/root/warm-io.sh`,
  `/root/match-trace.sh`, `/root/remove-empty-dirs.py`.
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
- **No vacuous truths.** An empty set never satisfies "all" or "enough":
  zero owners is not every owner holding it, zero peers asked is not every
  peer describing it, zero open backends is not every backend indexed.
  Two such defects were found on 2026-10-07/08 (fixed in 0.90.50 and
  0.90.51); `StoragePool::indexed()`, which skips a backend whose store is
  not open, has the same shape and is still to check.
- **Testing**: design it properly, test it well once (the suite on the
  laptop and on fi-1), deploy, measure, iterate. No soak runs, sanitizer
  builds or mutation sweeps as gates; one targeted mutation to show a new
  test guards its fix is fine. A failing test is P0: reproduce it before
  deciding whose it is. A laptop under heavy load (load average over about
  50) gives no verdict on timing; fi-1's suites decide.
- **Report findings before fixing; prove a mechanism before editing.**
- **Every response has a snake_case code; errors add a message; clients own
  sorting and presentation; every API change is announced to Core and every
  client before it ships.** Client sessions may ask; they do not decide. A
  client's account of itself is evidence about the client.
- **Deploying**: build once on fi-1, run all three suites there, ship the
  `usr` tree (`bin/macha`, `lib/macha/libmacha_core.*`, `lib/macha/plugins/`
  together); never compile on gbni-1; never both nodes down at once. Check the
  tarball's size and md5 on the build node. When a release adds a wire
  message, roll the peers first. After any rejected or interrupted command,
  check what actually ran before saying what did.
- **Builds**: laptop `-j8` and tests `--jobs 8`. When a shared struct
  changes, rebuild every target. After a deliberate mutation, delete the
  object file when restoring. Never edit sources under a running build.
  `macha-tests-runtime` is Linux-only.
- **Reading the cluster**: journald's prefix and `--since` are node-local
  time; correlate on the `Z` timestamp in the message. Record which host
  served a measurement. A node under high load may be Plex. Absence of
  failures on an idle node proves nothing.
- **The composition root is code**: concrete classes, constructor
  references, dependency order. No framework injection.
- **The DATA store is `StoragePool`** over one `LocalStore` per backend; the
  control store is a `LocalStore`.

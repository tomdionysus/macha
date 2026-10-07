# Active tasks

Last updated: 2026-10-06, on `develop`. Both nodes run 0.90.38.

The ordered list of open work; work top to bottom unless new evidence
changes the order. Alongside it: `BACKLOG.md` (everything else still to do,
verified against the code on 2026-10-06, by area), `COMPLETED.md` (finished
work), `CLIENT-CONTRACTS.md` (what the client sessions depend on) and
`archive/` (past plans, evidence and handovers; a record, not requirements).
Items marked *carried* come from
`archive/2026-10-05-ACTIVE-before-rationalisation.md` and have not been
re-checked since.

## 0. One definition of work class; the API is viewer-class work

Design: [`2026-10-06-one-work-class.md`](2026-10-06-one-work-class.md).
DATA admission, the activity clocks and the HTTP server each class work by
their own rule and disagree; an API request marks no activity, so a node
with someone in the editor reads as idle. One `WorkClass` (control, viewer,
loader, background) with one mapping from the wire frame, read by all
three; every HTTP route but the four control routes is viewer-class and
marks a viewer present; a work context carries its allowed waits apart
from its class; API-started DATA work carries the request's class down.
Six steps, one to two days; two questions for the operator at the end of
the document. Step 1 (one `WorkClass`, read by the arbiter, the clocks and
the pacing; three misclassified reads fixed) is 0.90.35; step 2 (the route
table classes each HTTP request; a served one marks its class present) is
0.90.36; step 3 (allowed waits apart from class) is 0.90.37; step 4 (the
request's class passed down its DATA work) and step 5 (the definition in
the docs) are 0.90.38; step 6 measured on polling load (COMPLETED);
continuous browsing not yet measured. Next: the non-interference reserves,
network first, then disk.

## 1. The catalogue plan: a materialised view of the local head

Plan: [`2026-10-06-catalogue-materialised-view.md`](2026-10-06-catalogue-materialised-view.md),
which supersedes the September shard plan. The catalogue view becomes a
pure function of this node's head: resident per-shard view, one install
point driven by head changes, mutations copying only the shards they
touch, a per-shard conflict merge, derived list and search indexes, one
manifest change (per-family shards, growable count), batched profile
publication, and holdings for catalogue DATA. It removes the two failures
of the burst test below, the polling refresh and its TTL, the second
repair per pass (section 7's question, answered), and every whole-snapshot
copy. Eleven stages, each shipping alone: correctness first (the install
point), then measurement, then cost. Stage 1 (one install point, the
installer) is 0.90.27; stage 2 (maintenance reduced, no second decode of
a commit) is 0.90.30, after the convergence fix (0.90.29); stage 3
(measured: 14.9 MB resident, a full load 156 ms on fi-1 and 328 ms on
gbni-1) is 0.90.31; stage 4 (the per-shard view) is 0.90.32; stage 5
(writes copy only the shards they change) is 0.90.33. Next: the work-class
item in section 0, then stage 6.

## 2. Failing tests and defects

- **gbni-1 killed by the kernel for memory during a large import** (2026-10-06
  21:16Z; 1.15 GB to 2.1 GB in a minute on a 4 GB node). Cause: each commit
  owed to the peer held two encoded snapshots and the decoded one until its
  claims on the peer were made, and the import made commits faster than a
  loaded WAN took the claims. Fixed in 0.90.34: owed commits hold claim ids,
  bounded at 32 MB, and only the newest head; a test pins the bound. Still
  to see: gbni-1's memory through the next multi-file import (the Futurama
  import had finished before the deploy). Also to do: memory per component
  in the observation windows, which hold only total RSS, so the next spike
  can be attributed from the record rather than from `/proc`.
- **gbni-1 heap corruption**, three times. An ASan 0.84.0 build and
  `/root/claude-missing-extent-driver.py` are staged on fi-1, not run.
  *Carried.*
- **Test failures**, each P0 when it recurs: *carried*
  `filesystem_fuse/test_fuse_recovery_thousand_operations_have_bounded_publications`
  (one segfault on fi-1, 2026-09-28),
  `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt`,
  `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
  `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`,
  `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection`,
  `rpc_cluster/test_rpc_slow_control_does_not_abort_data`,
  `test_write_data_work_context_preserves_loader_provenance`,
  `rpc_cluster/test_inbound_incapable_node_is_reached_only_over_its_own_sessions`,
  the pacing check in
  `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`
  under debug logging, the macOS-only torrent segfaults (5 of 21); and
  `users/test_accounts_converge_across_nodes` (laptop, full suite at
  `--jobs 8`, 2026-10-05: a sign-in it requires under 2 s took about 8 s;
  sign-in runs scrypt, so CPU contention is suspected).
- **An ingest dies on EAGAIN from a checkpoint commit** instead of re-basing
  on the current entry; log the conflict key when a merge records one; an
  ingest that dies when its node restarts mid-put. *Carried.*
- **Artwork held by no online node** (60 of 296 movie posters, 2026-09-27);
  nothing re-fetches lost artwork
  ([write-up](archive/2026-09-27-missing-artwork-and-single-copy-writes.md)).
- **The unmatched list differs by node** (771 on fi-1, 822 on gbni-1): hints
  are node-local, so Clear Metadata and Unmatch put a file in the list of the
  node that answered.
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

## 3. Matching

- **The first track of a match pays the provider again** (gbni-1, 0.90.29,
  2026-10-06 17:09Z): the editor had just run a provider search (1.3 s) and
  fetched artwork options four times (1.5 to 2.1 s each), then the match
  looked the release up again (1.4 s) and downloaded the cover again
  (1.9 s): 4.1 s for the first track, 0 ms lookup and artwork for the
  second. Commits took 339 and 290 ms with no lock wait: the 9 to 13 s
  commit waits did not recur after 0.90.27 and 0.90.29.
- Provider caches are per editor seat and lost on restart: the first track of
  each album after a restart pays the lookup and the cover again.

## 4. Replication and repair

- **Control objects crossed a saturated WAN at 0.9 to 4.6 s each: a law 1
  defect.** Under load (2026-10-06 20:38Z), fi-1 fetched gbni-1's new
  catalogue shards, control objects on the control lane, at 0.9 to 4.6 s
  while the link carried 20 torrents and repair. Two parts: the commit's
  claims had not delivered the shards (claim scope stops at
  `metadata_write_copies` holders), and control has no bandwidth reserve on
  the link. The second is the work-class design's network item (section
  0); the first, whether a commit's control objects go to every node
  present with the head, goes with it.
- Repair is bound by the WAN link: one step sends a batch of at most two
  extents and waits for it (about 4 MB in 5 s between fi-1 and gbni-1).
  Pipelining batches, or several steps per pass, would lift it further.
- Expose the counts holdings now give: extents this node lacks, extents each
  peer lacks, extents below their replication target.
- **Unsourceable objects on fi-1**: extents no node can supply, probably lost
  with es-1; the survey's `unavailable` count is the measure now. *Carried.*
- Per-peer down state in the transport; `fsync` without a deadline.
- Seven namespace conflicts standing from before 0.89.0.
- Say why a node counts itself busy (the pacer's active classes in status).
- Log the resumed push position at INFO.

## 5. Further optimisation (measured small; not now)

From the local-first work
([plan](archive/2026-10-05-local-first-filesystem-plan.md)):

- The availability survey's memo and the followed path table apply only
  while no peer's holdings grow; with gbni-1 importing, every survey asks from
  the root (2,834 tree nodes, 7 round trips). A peer reporting what it gained
  would let the memo stand.
- Ingest and data publications commit one operation at a time: about 100 ms
  each, so a 4 GiB copy spends about 7 s committing.
- Repair keeps its positions after a commit and runs one more pass;
  tombstones mature one at a time.
- The non-namespace snapshot per commit: a few milliseconds.
- The hint store rewrites its file per change: 266 KB with 409 hints.

## 6. Features and API

- **The API is RESTful, all of it**: audit every route; identity resets
  become a resource (decided); `providers/artwork?ref=` and
  `providers/artwork/choose` are among the routes to change.
- **OpenAPI**: generated from a declarative route table that dispatch runs
  from, served at `/api/v1/openapi.json`. Agreed, unstarted.
- **Core's request**: `providers/artwork` taking `item_id`; Core was to raise
  it again once the experiment ended.
- **Per-file readability**: designed; the operator chooses where it goes.
  Two of its three uses are wire changes.
- **A media-type context on torrent add** (operator, 2026-09-25).
- **People on catalogue items**: approved, with a backfill.
- **Placement API asks**: the target `node_id` and a viewer-facing detail on
  `placement_failed`; the peer's own refusal text on a forwarded add; an
  optional node `name` (operator's call).
- **Movie sets** and **ebooks**: later; each needs a proposal first.

## 7. Waiting on the operator

- Metadata editor B: the choice of fields.
- Torrent staging option A or B; stages 3 and 4 of the disk backend plan.
- A `CONTRIBUTING.md` line: "A new gate may pace lower-class work; it may
  never stop it."
- The replica's applier lifetime; fi-1's USB power. (The catalogue repair
  that runs twice per pass is answered by section 1: both go.)
- [Cost-budgeted scheduling](2026-10-03-cost-budget-scheduling-spec.md): a
  proposal with six questions.
- From the backlog: anonymous access; whether a client should know a playback
  generation existed anywhere in the cluster.

## Cluster state

- **gbni-1** (10.44.1.50, `macnessa.macha.network`) and **fi-1**
  (10.35.1.10) run **0.90.38**, cluster protocol 23. es-1 is offline
  indefinitely.
- Metadata writable 2/2. `dht.write_copies` and `dht.metadata_write_copies`
  are copies sought, not floors: a node alone still accepts writes.
- Both nodes' config: `maintenance.foreground_weight: 80`,
  `repair_weight: 20` (the code default is 95:5); `garbage_grace_ms` 30 days.
  The config before the weights is `macha.yaml.before-repair-weight`;
  before 0.90.17 removed three keys, `macha.yaml.before-dead-keys`. Both set
  `catalogue.api.max_connections: 128`.
- Rollback: `/root/pre-<version>/` on each node holds the binaries and config
  in place before that version was installed (`pre-0.90.38` back to
  `pre-0.89.0`, which also has the roster and sequence counter).
- fi-1's `/root/macha/build-asan` and `build-coverage` hold some macOS
  objects; their linked binaries are intact, the trees need a clean rebuild
  before reuse.
- Both nodes log at DEBUG. gbni-1's ALL trace for matching ended on
  2026-10-06 (restored from `macha.yaml.before-trace` by SIGHUP).
- gbni-1 keeps its heap-check drop-in (`/root/heap-check.conf.keep`).
- Observation windows: `/etc/macha/state/observation/observations.jsonl` on
  both.
- Helpers: `build/deploy.sh VERSION` (stages from fi-1's build, installs
  fi-1 then gbni-1, rolls back a node that does not come up); on fi-1
  `/root/mapi.sh`, `/root/apitime.sh`, `/root/burst-delete.sh`,
  `/root/ryw-test.sh`; on gbni-1 `/root/warm-io.sh`, `/root/match-trace.sh`,
  `/root/remove-empty-dirs.py`.
- Code lookups go through basemind (load its tools first; rescan after edits
  and moves).

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou
  Shalt Not Make Control Wait. 2. Thou Shalt Not Make The Viewer Wait.
  3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
  Viewer Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a
  new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.** A local operation
  never waits on a peer or walks the namespace.
- **Testing**: design it properly, test it well once (the suite on the
  laptop and on fi-1), deploy, measure, iterate. No soak runs, sanitizer
  builds or mutation sweeps as gates; a sanitizer is a debugger for one bug.
  A failing test is P0: reproduce it before deciding whose it is.
- **Report findings before fixing; prove a mechanism before editing.**
- **Every response has a snake_case code; errors add a message; clients own
  sorting and presentation; every API change is announced to Core and every
  client before it ships.** Client sessions may ask; they do not decide. A
  client's account of itself is evidence about the client.
- **Deploying**: build once on fi-1, run all three suites there, ship the
  `usr` tree (`bin/macha`, `lib/macha/libmacha_core.*`, `lib/macha/plugins/`
  together); never compile on gbni-1; never both nodes down at once. Check the
  tarball's size and md5 on the build node (a 0-byte tarball is
  `d41d8cd9...`). When a release adds a wire message, roll the peers first. A
  change to the telemetry format itself needs an all-at-once cutover; an added
  TEL3 field does not.
- **Builds**: laptop `-j8` and tests `--jobs 8`. When a shared struct
  changes, rebuild every target. After a deliberate mutation, delete the
  object file when restoring. Never edit sources under a running build.
  `macha-tests-runtime` is Linux-only.
- **Reading the cluster**: journald's prefix and `--since` are node-local
  time; correlate on the `Z` timestamp in the message. A journal watch over
  ssh needs `ssh -n` and a flushing last stage; test it against a known event.
  Record which host served a measurement. A node under high load may be Plex.
  Absence of failures on an idle node proves nothing.
- **The composition root is code**: concrete classes, constructor
  references, dependency order. No framework injection.
- **The DATA store is `StoragePool`** over one `LocalStore` per backend; the
  control store is a `LocalStore`.

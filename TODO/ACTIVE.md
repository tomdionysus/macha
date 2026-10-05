# Active tasks

Last updated: 2026-10-06, on `develop`. Both nodes run 0.90.16.

The ordered list of open work; work top to bottom unless new evidence
changes the order. Alongside it: `COMPLETED.md` (finished work),
`CLIENT-CONTRACTS.md` (what the client sessions depend on), `BACKLOG.md` (the
P-1 to P2 sections of 2026-09-05..22, not re-verified) and `archive/` (past
plans, evidence and handovers; a record, not requirements). Items marked
*carried* come from `archive/2026-10-05-ACTIVE-before-rationalisation.md`,
which has their full text, and have not been re-checked since.

## 1. Paging on every list call

Operator, 2026-10-05. Designed in
[title files and paging](2026-10-05-title-files-and-paging.md) and proposed
to Core: `limit` and `cursor`, `next_cursor` in the answer, the whole list
without `limit`. `GET /api/v1/catalogue/items` answers 6,764 items, 9.5 MB,
in one response today. Announce to Core and every client before it ships.

## 2. Matching

- A commit during a multi-file match sometimes waited 9 to 13 s (gbni-1,
  2026-10-05 19:10Z, every other track). Not reproduced; nothing else logged
  in the window. The catalogue lock's waits are logged at level ALL
  (`DIAG lock-held lock=catalogue.mutation`): run gbni-1 at ALL while an
  album is matched to see what it waits on.
- MusicBrainz answering 503 shuts the node's MusicBrainz gate for 60 s, so
  one rate-limit answer refuses every search for a minute. Proposed: wait for
  `Retry-After` (or a few seconds) and retry an interactive request once; back
  off only background matching; `provider_unavailable` with `retry_after_ms`.
  Discogs has the same gate.
- Provider caches are per editor seat and lost on restart: the first track of
  each album after a restart pays the lookup and the cover again.

## 3. Further optimisation (measured small; not now)

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

## 4. Open from the object ledger and absent-node work

- Per-peer down state in the transport; `fsync` without a deadline; the
  journal's `durability_poisoned` flag; a count of DATA objects below their
  replication target; seven namespace conflicts standing from before 0.89.0.
- The final sweep: annotated lock wrappers; coverage of the composition
  root's components.
- [Cost-budgeted scheduling](2026-10-03-cost-budget-scheduling-spec.md): a
  proposal with six questions waiting on the operator.

## 5. Defects and unexplained failures

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
- **Unsourceable objects on fi-1**: `repair cannot source an object this
  node should own`, rising with repair's pull walk; three of five random
  films failed to read on both nodes. Probably extents lost with es-1. fi-1
  also counts objects it cannot store while its backend is offline.
  *Carried.*
- **Artwork held by no online node** (60 of 296 movie posters, 2026-09-27);
  nothing re-fetches lost artwork
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
- **Known defects of 2026-09-24/25**: `catalogue_api` answers 503 for some
  client errors and does not check a parent cycle; `metadata replica is still
  recovering` and `local metadata replica unavailable` raise a bare
  `runtime_error` an ingest would fail on; the acquisition API audit's four
  findings; unverified package names in the install docs. *Carried.*
- **The disk resource audit's open points**: one monitor per pool, not per
  device; local maintenance budgeted from a network measurement; the 150-300%
  hysteresis band is a latch; law 1 holds by configuration only. *Carried.*

## 6. Replication and repair

- Say why a node counts itself busy (the pacer's active classes in status).
- Log the resumed push position at INFO.
- Measure pipelined pushes on an idle node, and step length on a busy HDD.

## 7. Features and API

- **The API is RESTful, all of it**: audit every route; identity resets
  become a resource (decided); `providers/artwork?ref=` and
  `providers/artwork/choose` are among the routes to change.
- **OpenAPI**: generated from a declarative route table that dispatch runs
  from. Agreed, unstarted.
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

## 8. Waiting on the operator

- Metadata editor B: the choice of fields.
- Torrent staging option A or B; stages 3 and 4 of the disk backend plan.
- A `CONTRIBUTING.md` line: "A new gate may pace lower-class work; it may
  never stop it."
- The catalogue repair that runs twice per maintenance pass; the replica's
  applier lifetime; fi-1's USB power.
- `BACKLOG.md` and the older ordered items (*carried*): the namespace tree's
  demand-loaded extent nodes and a persisted `file_media_id`; the
  cache-sizing invariant; the metadata stall on the RPC path; the rejoin and
  materialisation-cache P0; the loader-I/O P0.

## Cluster state

- **gbni-1** (10.44.1.50, `macnessa.macha.network`) and **fi-1**
  (10.35.1.10) run **0.90.16**, cluster protocol 23. es-1 is offline
  indefinitely.
- Metadata writable 2/2. `dht.write_copies` and `dht.metadata_write_copies`
  are copies sought, not floors: a node alone still accepts writes.
- Both nodes' config: `maintenance.foreground_weight: 80`,
  `repair_weight: 20` (the code default is 95:5); `garbage_grace_ms` 30 days.
  The config before the weights is `macha.yaml.before-repair-weight`.
- Rollback: `/root/pre-<version>/` on each node holds the binaries and config
  in place before that version was installed (`pre-0.90.16` back to
  `pre-0.89.0`, which also has the roster and sequence counter).
- fi-1's `/root/macha/build-asan` and `build-coverage` hold some macOS
  objects; their linked binaries are intact, the trees need a clean rebuild
  before reuse.
- gbni-1 keeps its heap-check drop-in (`/root/heap-check.conf.keep`).
- Observation windows: `/etc/macha/state/observation/observations.jsonl` on
  both.
- Helpers: `build/deploy.sh VERSION` (stages from fi-1's build, installs
  fi-1 then gbni-1, rolls back a node that does not come up); on fi-1
  `/root/mapi.sh`, `/root/apitime.sh`, `/root/burst-delete.sh`,
  `/root/ryw-test.sh`; on gbni-1 `/root/warm-io.sh`, `/root/match-trace.sh`,
  `/root/remove-empty-dirs.py`.

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

# Active tasks

Last updated: 2026-10-05, on `experiment/object-ledger-t5`. Both nodes run
0.90.14; see entry 1's plan for what each 0.90.x carries.

This is the ordered list of open work. `COMPLETED.md` is the ledger of
finished work; `BACKLOG.md` holds the older P-1 to P2 sections, written
2026-09-05..22 and not re-verified since; `archive/` holds every past plan,
record, handover and superseded file (evidence, not requirements). Work top
to bottom unless new evidence changes the order.

This file was cut down on 2026-10-05. The file as it stood, with the full
text and evidence behind every carried item, is
[`archive/2026-10-05-ACTIVE-before-rationalisation.md`](archive/2026-10-05-ACTIVE-before-rationalisation.md).
Items below marked *carried* come from it and have not been re-checked
against 0.89.1.

## 1. One local-first path for every read and write

**The top entry (operator, 2026-10-05).** File access and operations must be
fast and consistent locally on every path; the cluster converges behind.

- **Plan**: [local-first filesystem](2026-10-05-local-first-filesystem-plan.md).
  Built through 0.90.13; its status block says what was measured and what is
  left, each with why. The operator's standing instruction: commit and install each slice
  as it passes. Three stages: the local path
  (journal, view, publisher, tree splice, commit off the network); per-commit
  background work from the tree diff; the catalogue and the management API.
- **Evidence**: [the audit](2026-10-05-whole-library-work-audit.md).
- **How it is worked** (operator, 2026-10-05): designed across the whole
  codebase, tested well once (the suite on the laptop and on fi-1), deployed,
  measured, iterated. No soak runs, sanitizer builds or mutation sweeps as
  gates.
- **What it absorbs** from the older lists, so none is worked separately:
  - Bulk delete of unmatched files reporting failures (old 0c, 2026-09-28,
    seen again 2026-10-05). The false count was Core's 8 s request timeout,
    fixed in Core at `340b8ae` by Core's account; the server finished every
    delete, one commit at a time. Stage 1. Still its own small item: a failed
    `DELETE /api/v1/manage/unmatched/{id}` logs its cause.
  - HTTP and FUSE request paths that may wait on a peer through
    `namespace_index()` and `find_media()` (old 2). Stage 1.
  - Publications that neither batch nor throttle since the tree (old -3).
    Stage 1.
  - Validated probes reading every copy on the peer each pass (old 1).
    Stage 2.
  - Catalogue writes losing to "catalogue changed concurrently" (seen again
    2026-10-05 as `media information prune deferred`), and the slow cold
    artwork read (old 10). Stage 3.
  - `BACKLOG.md`: "The namespace does not meet its own scale target", "The
    catalogue materialises everything it has", "Scaling cliffs".

## 2. The object ledger experiment: what is left

The experiment is the development line; `develop` is frozen at `75e6f98`.
Spec: [object ledger and component model](2026-09-29-object-ledger-and-components-spec.md).
Plan: [stage 0](2026-09-29-object-ledger-implementation-plan.md). Evidence:
[`object-ledger-evidence/`](object-ledger-evidence/).

- T0 to T5 are built and merged into `-t5`. Absent-node tolerance
  ([design](2026-10-05-absent-node-tolerance-design.md)) is built and
  deployed: 0.88.0 (writes on the nodes present, own-clock deletion,
  membership forgets) and 0.89.0 (entry provenance, the two-head merge,
  protocol 23). 0.89.1 added a group commit of namespace batches and two
  claim fixes.
- **Entry 1 lands on this line** before the experiment is assessed. *An
  assumption, for the operator to confirm.*
- Left, as recorded 2026-10-04 (*carried*): a day on the cluster under
  normal load on the final build; coverage of the composition root's
  components; the T5 assessment and the merge of `-t5`; the operator's
  decision on `develop`. The final sweep S (annotated lock wrappers, the
  `*_for_tests` hooks) as far as it stands.
- Open from the absent-node work: per-peer down state in the transport;
  `fsync` without a deadline; the journal's `durability_poisoned` flag; a
  count of DATA objects below their replication target; seven namespace
  conflicts standing from before 0.89.0.
- After the experiment: [cost-budgeted scheduling](2026-10-03-cost-budget-scheduling-spec.md),
  a proposal with six questions waiting on the operator.

## Cluster state (2026-10-05)

- **gbni-1** (10.44.1.50, `macnessa.macha.network`) and **fi-1**
  (10.35.1.10) run **0.90.14** (tarball md5
  `1479ebc1a11fb512b0041b4c6f5b6613`), **cluster protocol 23**, installed
  19:58Z (fi-1) and 19:59Z (gbni-1). es-1 is offline indefinitely.
- Metadata writable 2/2. `dht.write_copies` and `dht.metadata_write_copies`
  are copies sought, not floors: a node alone still accepts writes.
- Rollback material on each node: `/root/pre-<version>/` holds the binaries
  and config in place before that version was installed (`pre-0.90.14` back
  to `pre-0.89.0`, which also has the roster and sequence counter).
- fi-1's `/root/macha/build-asan` and `build-coverage` were partly
  overwritten with macOS objects by a mis-excluded sync on 2026-10-05; their
  linked binaries are intact, the trees need a clean rebuild before reuse.
- `garbage_grace_ms` is 30 days on both (the absence horizon).
- gbni-1 keeps its heap-check drop-in (`/root/heap-check.conf.keep`).
- Observation: `/etc/macha/state/observation/observations.jsonl` on both.
- Left by testing on 2026-10-05: an empty `/scratch-delete-test` directory
  in the namespace; `/root/burst-delete.sh` and `/root/mapi.sh` on fi-1.

## The queue

After entry 1. Numbers in brackets are the item's number in the archived
file.

**Seen 2026-10-05, not investigated**

- `providers/artwork` and `providers/search` answered 503 after 17 to 19 s
  on both nodes. The code path is `provider_unavailable` (an upstream fetch
  that threw; 5 s connect, 20 s total). The reason is returned and not
  logged. Not reproduced. Logging is in entry 1, stage 3.
- The unmatched list differs by node (771 on fi-1, 822 on gbni-1): hints are
  node-local.
- Each node logged `RPC stalled (control)` against the other at 09:47Z and
  09:50Z, 5 to 15 s without progress, then nothing.
- fi-1 logs `repair cannot source an object this node should own`
  continuously (see "unsourceable" below).

**Defects and unexplained failures**

- **gbni-1 heap corruption**, three times, pre-dating the experiment. An
  ASan 0.84.0 build and `/root/claude-missing-extent-driver.py` are staged
  on fi-1 and have not been run. *Carried.*
- **Test failures** (each is P0 when it recurs) [12], *carried*:
  `filesystem_fuse/test_fuse_recovery_thousand_operations_have_bounded_publications`
  (one segfault on fi-1, 2026-09-28);
  `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt`;
  `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`;
  `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`;
  `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection`;
  `rpc_cluster/test_rpc_slow_control_does_not_abort_data`;
  `test_write_data_work_context_preserves_loader_provenance`;
  `rpc_cluster/test_inbound_incapable_node_is_reached_only_over_its_own_sessions`;
  the pacing check in
  `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`
  under debug logging; the macOS-only torrent segfaults (5 of 21);
  `users/test_accounts_converge_across_nodes` (laptop, full suite at
  `--jobs 8`, 2026-10-05: the sign-in it requires under 2 s took about 8 s;
  sign-in runs scrypt, so CPU contention is suspected, not a peer wait;
  passed alone and on the next full run). Fixed
  2026-10-05: `namespace_migration/test_a_merge_claims_what_it_introduces`
  (`45cba94`).
- **An ingest dies on EAGAIN from a checkpoint commit** instead of re-basing
  on the current entry [-3]. Entry 1 removes checkpoint commits; confirm
  there. Also from [-3]: log the conflict key when a merge records one; an
  ingest that dies when its node restarts mid-put. *Carried.* The merge rule
  and the sibling-merge storm that item describes belong to the three-way
  merge, which 0.89.0 removed.
- **Unsourceable objects on fi-1** [1, handover]: the count rises with
  repair's pull walk; three of five random films failed to read on both
  nodes. Which node should hold them is not established. Probably extents
  lost with es-1. fi-1 also pulls objects it cannot store when its backend
  is offline, and counts each as unsourceable. *Carried.*
- **60 of 296 movie posters are held by no online node** [0b]; nothing
  re-fetches lost artwork
  ([write-up](archive/2026-09-27-missing-artwork-and-single-copy-writes.md)).
- **A rejoin or follower convergence retry waits out its backoff**
  [handover]: wake it, debounced, with a cap from the first trigger. Tried
  and reverted 2026-10-02.
- **Torrent and ingest staging has no mount check** [handover]: with fi-1's
  disk absent, staging wrote to the SD card under the mount point.
- **fi-1, 2026-09-29 ~12:00Z** [3]: playback `PATCH` 503 after 19.9 s under
  software transcode, and a `FUSE mount disappeared` ERROR during a restart.
  Not investigated.
- **Transport backoff after a peer restart** [13]: up to 4 s before a
  restarted peer is dialled.
- **Known defects from 2026-09-24/25** [7]: `catalogue_api` answers 503 for
  some client errors and does not check a parent cycle; `metadata replica is
  still recovering` and `local metadata replica unavailable` raise a bare
  `runtime_error` an ingest would fail on; the acquisition API audit's four
  findings; unverified package names in the install docs. (`object
  replication quorum unavailable` went with 0.88.0.)
- **What the disk resource audit left open** [-2]: one monitor per pool, not
  per device; local maintenance budgeted from a network measurement; the
  150-300% hysteresis band is a latch; law 1 holds by configuration only.

**Replication and repair** [1], *carried*

- `repair_weight` 20 against `foreground_weight` 80, set in both nodes'
  config (operator, 2026-10-05); the code default is still 95:5.
- Say why a node counts itself busy (the pacer's active classes in status).
- Log the resumed push position at INFO.
- Measure 0.73.1's pipelined pushes on an idle node; step length on a busy
  HDD.

**Features and API, agreed or waiting**

- **Title files in the metadata editor** (operator, 2026-10-05): server
  routes, one call per file. Unmatch unbinds a file and puts it straight in
  the unmatched list, no automatic rematch; delete by path removes only that
  path; delete by content hash removes every path holding it. Both delete a
  title left with no files, and any season, show or album left empty,
  manual or scanner-made. Announce to Core and every client before shipping.
- **Paging on every list call** (operator, 2026-10-05); see entry 1's plan.

- **Per-file readability** [4]: designed; the operator chooses where it
  goes. Two of its three uses are wire changes.
- **A media-type context on torrent add** [5] (operator, 2026-09-25).
- **OpenAPI** [8]: generated from a declarative route table that dispatch
  runs from. Agreed, unstarted.
- **People on catalogue items** [9]: approved, with a backfill.
- **The API is RESTful, all of it** [16]: audit every route; identity resets
  become a resource (decided). `providers/artwork?ref=` and
  `providers/artwork/choose` are among the routes to change.
- **Core's request** [17]: `providers/artwork` taking `item_id`. Not now;
  Core raises it again when the experiment ends.
- **Placement API asks** [-1]: the target `node_id` and a viewer-facing
  detail on `placement_failed`; the peer's own refusal text on a forwarded
  add; an optional node `name` (operator's call).
- **Movie sets** [14] and **ebooks** [15]: later; each needs a proposal
  first.

**Waiting on the operator** [6]

- Metadata editor B: the choice of fields.
- Torrent staging option A or B; stages 3 and 4 of the disk backend plan.
- A `CONTRIBUTING.md` line: "A new gate may pace lower-class work; it may
  never stop it."
- The catalogue repair that runs twice per maintenance pass; the replica's
  applier lifetime; fi-1's USB power.

**Older ordered items**, in the archived file and `BACKLOG.md`, *carried*:
the namespace Merkle work's Stage D (demand-loaded extent nodes, a persisted
`file_media_id`); the cache-sizing invariant; the metadata stall on the RPC
path; the rejoin and materialisation-cache P0; the loader-I/O P0.

## Standing rules (learned the hard way; do not relearn)

- **basemind for every code query**, counts and surveys included (`code mode
  grep` reports exact `total_matches`). On 2026-09-29 shell counts were
  used and one was misread; the operator's rule is absolute. Rescan after
  edits.
- **A primitive is defined by its phase space** (small enough to test
  deterministically and exhaustively), not by a list; 100% coverage proves
  the implementation, and design fitness is a separate question
  (operator, 2026-09-30).

- **Pace, never gate.** No process or subsystem waits for a higher class to
  go idle; a new signal feeds the existing pacer (`repair_share`). A node is
  usually serving someone, and work that waits for viewers to stop makes a
  later viewer wait (operator, 2026-09-29, after 0.73.0 briefly held repair
  for a peer's viewers).
- Monitoring scripts that print `time.strftime` print each node's local time
  (gbni-1 BST, fi-1 EEST). Compare in UTC; a misread on 2026-09-29 made a
  correct resume look like lost progress.
- journald's timestamp prefix is node-local; correlate on the `Z` timestamp
  inside the message, and `--since` is local time on each node.
- `macha-tests-runtime` is Linux-only; `macha-tests-torrent` needs a working
  libtorrent session, so es-1, not the laptop.
- When a release adds a wire message, roll the peers first and the node that
  needs the fix last.
- Check the tarball's size and md5 on the build node before shipping; a
  0-byte tarball has md5 `d41d8cd9...`.
- A node under "high load" may be Plex: `ps --sort=-pcpu` first.
- Absence of failures on an idle node proves nothing.
- **When a shared struct changes (config, jobs), rebuild every target**:
  building only the plugin left core on the old `TorrentConfig` layout and
  the plugin tried to start 2,097,152 threads.
- **After a deliberate mutation, delete the object file when restoring**: a
  same-second restore skipped the rebuild and an hour went on a phantom bug.
- **Every response has a snake_case status code; errors add a message beside
  it; clients own sorting and presentation; every API change is announced to
  Core and every client** (operator, 2026-09-24).
- **Testing** (operator, 2026-10-05): design it properly across the codebase,
  test it well once, get it working, iterate. No soak runs, sanitizer builds
  or mutation sweeps as gates; a sanitizer is a debugger for a specific bug.
  This replaces the experiment's per-step rule of full coverage,
  mutation-proven.
- **The wait for viewers before load or a gdb attach was lifted**
  (operator, 2026-10-03); never both nodes down at once.
- **Report findings before fixing; prove a mechanism before editing.** A
  failing test on a node is P0: reproduce it before deciding whose it is.
- **A deploy needs the operator's order for that version.**
- **Client sessions may ask; they do not decide.** An API change waits on
  the operator.
- **A client's account of itself is evidence about the client, not a fact.**
- **The composition root is code**: concrete classes, constructor
  references, dependency order written down. No framework injection.
- **Check the branch before cutting one**; the operator sometimes commits in
  this checkout.
- **Never edit sources under a running build.**
- **The DATA store is `StoragePool`** over one `LocalStore` per backend; the
  control store is a `LocalStore`. Service accessors wait for the
  asynchronous start (`wait_services_ready()`).
- **The cmake install stage writes `etc/macha/macha.yaml`**: ship `usr` only.
- **A journal watch over ssh needs `ssh -n`** and a flushing last stage;
  test it against a known event before trusting its silence.

The governing laws are stated, with their numbering, in
[Principles and laws](../docs/principles-and-laws.md):

1. Thou Shalt Not Make Control Wait.
2. Thou Shalt Not Make The Viewer Wait.
3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.
4. Thou Shalt Not Shoot Thyself In The Foot.

## The older backlog

The P-1 to P2 sections written 2026-09-05..22 are in
[`BACKLOG.md`](BACKLOG.md), not re-verified since. The entries above outrank
them, and entry 1 absorbs three of them.

## What the client sessions now depend on (settled 2026-09-13)

**0.63.0 (announced 2026-09-27 to Core, web, Android TV and mobile, before
deploy).** `POST /api/v1/torrents/jobs` can answer `409
torrent_already_added` with the holding job's `id` and `node_id` at top level
(same fields as the 202) and no `error.reason`; a job holds its torrent in any
state until cleared. New torrent job `error_code`s `duplicate_torrent` and
`torrent_fault` (both `failed`, not retryable). New always-present
`threads` array in `GET /api/v1/status` (`name`, `running`, `restarting`,
`faults`, `last_fault_code`, `last_fault`, `last_fault_unix_ms`).

**Mobile walks and charges healthy nodes on any mid-stream player error, and
has done all along** (core, 2026-09-21). This is not a contract and not a
request; it is a standing client defect the server session needs to know
about, because it shapes what node-health evidence from a mobile viewer is
worth. macha-client-rn has no status-to-kind mapping at the player layer at
all — playback errors arrive through expo-video's `statusChange` as a message
string with no code — so on `status === 'error'` the provider calls
`failoverSource` unconditionally, picks another node, and **records a failure
against the node it left**. A routine superseded generation therefore costs a
healthy node a mark in that client's ranking.

`410`'s axes (`node_healthy: true`, `alternative_may_succeed: true`) exist to
prevent exactly this and mobile cannot read them. **0.48.0 does not cause it
and does not worsen it** — mobile is equally blind to the `404` it gets today,
and core verified there is no status-dependent branch anywhere on that path —
but the release makes it legible. Two consequences worth holding:

- **Do not read a mobile client's endpoint-failure record as evidence about a
  node.** It may be a seek that regenerated, not a fault.
- The fix is client work, scheduled by core. The seek case is usually already
  invisible (`repositionTo` repoints before refetching, and
  `errorBlamesEndpoint` declines failover while a seek is outstanding), so the
  exposure is mid-stream errors that are not seeks.

Negotiated with the `@machafoundation/core` session and relayed by it to the
web, Android TV and mobile clients. None of it exists anywhere else in this
repository, and a server change that breaks one of these breaks four clients at
once. Recorded here because the conversation that settled them was
cross-session and will not be in the next session's context.

- **`GET /api/v1/health` is the liveness contract.** No token, no role, works
  during recovery. `200 {"status":"ok"}` when serving, `503` with `starting` or
  `failed` when not, and the HTTP status carries the same answer as the body.
  Core probes it every 10 s for latency ranking, failover and the endpoint
  pre-save gate. It must stay unauthenticated. **Corrected 2026-09-18: it does
  carry `version`, and that is intended** -- it has since 0.42.1, and reading a
  node's running version without a token is how every on-box check and every
  deploy verification is done. The rule it still keeps is the one that matters:
  no node id, no topology, nothing about the cluster. Anything beyond "is this
  node serving, and what is it running" needs `/api/v1/status` and
  `view_status`. The code comment at `src/service/service.cpp:242` now says it carries
  the running version, deliberately (checked 2026-09-24).
- **An old node answers `401`, not `404`**, to that route, because
  authentication happens before routing. Core falls back to
  `/api/v1/catalogue/status` on *any* answer that is not a liveness answer,
  which retires itself once no node needs it. This fact is why a 404-only
  fallback would have fired on every node except the one that needs it.
- **`view_status` gates the Status screen and nothing operational.** It is in
  core's `UserRole` and `USER_ROLES` with a test pinning the order. Health,
  ranking, failover and the connection gate are all indifferent to it.
- **A role-less session gets `403` from `/api/v1/status`, never a reduced
  payload.** There is no reduced-view code path; the gate is above the handler.
  Two client reports of "200 with an empty roster" were gbni-2 (ungated 0.38.1)
  misattributed to gbni-1 — see the endpoint-attribution note below.
- **`/api/v1/status` no longer carries `diagnostics`** (0.39.1). Core never
  typed that block, so it is unaffected; a client whose Status screen reads
  diagnostics needs `/api/v1/status/diagnostics`.
- **`GET /api/v1/session` needs a session and no role**, and re-checks
  `credential_generation` on every request, so it catches revocation and not
  only expiry. A `401` there can mean the account changed underneath the token;
  a client must not tell a viewer their session merely timed out.
- **A PATCH naming `mode` clears `video`, `audio`, `max_height` and
  `max_bitrate`** (`playback.cpp:248`). Unchanged since 0.34.0 and confirmed
  against 0.39.1. Android TV has been told to delete its local rule; if they
  can produce a refusal from a body containing `mode` and nothing else, that is
  a real regression and should be treated as one.
- **A role-less session learning no cluster membership is correct**, per the
  operator: such a session sees only the endpoint it was configured with.
- **`GET /api/v1/users` answers under `items`**, like every other collection,
  from 0.40.0. Single records from `POST`/`PATCH` stay bare. Core has accepted
  either key since its 0.8.0, so no client needed a release.
- **`stream.look_ahead_ms` on the playback session payload** (0.45.0) is how far
  past the fragment it last requested a client may arrive and still find media
  already produced: `max_ahead_segments` x `segment_duration_ms`, `null` for
  direct play. Added because neither knob was on the wire or in the
  configuration reference, so a client could only hardcode 8 and 4000 and
  under-run against a node configured differently. Clients bound themselves
  against this rather than against the defaults. It follows `reconfigure()`, so
  it is read per session rather than cached across a node's lifetime.
- **`seek_ms`, `seek_offset_ms` and `seek_requested_ms` on the playback session
  payload** (0.46.0), on create and on every `PATCH`, all milliseconds on the
  title's timeline. `seek_ms` is where the generation's media begins, which is
  exactly what it has always meant; `seek_offset_ms` is how far into that
  generation the requested position sits; `seek_requested_ms` is the request the
  server honoured after clamping to `[0, duration - 1 ms]`. The invariant is
  `seek_ms + seek_offset_ms == seek_requested_ms`, exactly, in integer
  milliseconds, with no tolerance and no rounding slack, and the offset is never
  negative. The server does not move a position a client asked for and does not
  substitute a mode a client asked for: transcode and direct are frame-accurate
  with a zero offset, remux takes the last keyframe at or before the request and
  publishes the remainder. The offset is zero exactly when the mode can be
  frame-accurate, so a client wanting a cheap aligned seek asks for a position
  that already is a keyframe. Core types all three as `number | undefined`
  because an older node omits them, and deletes its `activationPosition`
  undefined branch — the invariant makes that state unreachable.
- **A node reports the playback budgets it enforces** (0.46.2) on the per-node
  entries of `GET /api/v1/status`, in a `playback` object beside `runtime`:
  `startup_timeout_ms` and `segment_timeout_ms`. They are each node's statement
  about itself, relayed like `load1` and `cpu_cores`; no node computes a
  cluster-wide figure, because telemetry carries no peer's streaming
  configuration and it would be inventing one. A client that needs a worst case
  across candidates composes it itself, since only the client knows which nodes
  those are. **Absent means the node cannot say** -- an older node, or one with
  streaming disabled -- and must never shorten a client's own budget, nor be
  filled in from another node's figure. Deliberately not on the session
  payload, unlike `look_ahead_ms`: these bound the request that creates the
  session, so a client cannot learn them from the response it is timing out on,
  and a node it has never used would never report them. Agreed with the core
  session on 2026-09-18 after it showed that a `playback/status` placement
  could not ride its existing health probe without regressing latency ranking
  for role-less sessions.
- **`pipeline_idle_ms` bounds how long a client may hold a generation before
  first requesting media.** A transformed session whose stream has been idle for
  `streaming.pipeline_idle_ms` has its physical pipeline reclaimed; the logical
  session and its entitlement survive, but the producing pipeline does not. The
  default is 60,000 ms, the configured minimum is 10,000, and the live value is
  already reported by `GET /api/v1/playback/status` as `pipeline_idle_ms` -- so
  a client with a standby or handover window must read it rather than assume 60 s,
  exactly as it must for `look_ahead_ms`. Core's standby windows (8 s transcode,
  30 s otherwise) sit inside the default, but that relation was implicit until
  2026-09-18 and nobody had written it down. **What is NOT yet settled is what
  happens to a fragment request arriving after reclamation** -- see the P1 above;
  until that is answered no client should treat a reclaimed generation as
  recoverable.
- **`session_unused_idle_ms` means "never served a stream object", not "idle".**
  Asked by the Android TV session on 2026-09-20 after a paused **direct-play**
  session survived 4.5 minutes against a 120,000 ms `session_unused_idle_ms`.
  That is correct and deliberate. A session carries a `stream_served` flag set
  the first time it serves **any** stream object -- playlist, fragment,
  subtitle or a Direct Play ranged body -- and never cleared, across seeks and
  quality changes. Once set, the session gets the full `session_idle_ms`
  (30 min default); only a session that has *never* fetched media is held to
  the short clock. Direct play sets it explicitly, with the reason in the code:
  "a single ranged body can outlive several idle windows without another
  request, so this must count as having been used." Both clocks measure from
  the session's last interaction of any kind, so a client still polling or
  PATCHing is never evicted by either. The short clock exists to stop a session
  created and abandoned from holding a transcode entitlement, not to reap
  paused viewers.
- **A fragment past the look-ahead is refused, not missing.** `500
  segment_not_ready` with `Retry-After: 1` and `Cache-Control: no-store`, logged
  as `reason=hold_timed_out`; never a `404`, because the playlist has already
  promised the object exists and a `404` invites an intermediary to cache the
  absence. Retrying is correct and succeeds as production advances. Production
  is sequential, so asking for a distant index does not skip the fragments
  before it -- where the gap exceeds the look-ahead, a new generation seeked to
  the arrival point is cheaper than making the current one catch up (measured
  2026-09-17 on es-1: 1.92 s cold start against ~9 s of catch-up for 28 s).
- **A signed artwork URL is stable for up to 24 hours and valid for 24-48.**
  From 0.40.0 `exp` is quantized to a bucket of the TTL, rounded up to the
  bucket *after* next: `(now / ttl + 2) * ttl`. The invariant is "always between
  one and two TTLs", not "between 24 and 48 hours" — the bucket *is* the TTL, so
  reconfiguring `artwork_capability_ttl` moves the bound with it. With the
  default 24 h TTL this lands on a UTC midnight, because the bucket is a
  multiple of 86,400,000 ms from the epoch and the epoch is itself a UTC
  midnight. The minimum remaining validity is 86,400,001 ms, one millisecond
  *over* `max-age=86400`, so a cached copy can never outlive the signature that
  names it. Measured live: 418 refs, 418 identical URLs.
  The artwork `id` has always been the SHA-256 of the bytes and is the stable
  identity clients should key an image cache on; the signed URL is transport.
  The route is bearer-exempt with a valid signature, so a payload's `url` works
  in a bare `<img src>` with no headers.
- **Session lifetime is 30 days from mint, and that is decided** (operator,
  2026-09-13): counted from creation rather than last use, so a session expires
  even under daily use. No sliding expiry, no refresh tokens. Do not re-raise it
  as a defect.
- **`/api/v1/status/diagnostics` gained a `repair` block** in 0.40.1
  (`unsourceable_objects`, `unsourceable_sample`, `local_unreadable_objects`).
  No client types it today.
- **Record which host served a measurement before reporting it.** Two separate
  client reports today were gbni-2 attributed to gbni-1, and the retracted
  DTS/TrueHD investigation in September was the same mistake at larger scale.
  The node is in the URL; there is no excuse for losing it.


## Deployment rule

Build once, ship the artefacts. Build on fi-1 (es-1 is offline), run the full suite
there, stage with `DESTDIR`, and ship the tarball — `bin/macha`,
`lib/macha/libmacha_core.*` and `lib/macha/plugins/` together, never the
executable alone. Verify `uname -m`, `ldd --version` and the tarball hash on
each target. Never compile on gbni-1.

**Adding a `NodeTelemetry` field no longer forces an all-at-once cutover.**
That rule was written for the positional format, where an added field made a
mixed-version cluster misparse every multi-node telemetry set it exchanged.
TEL3 is tagged and length-delimited, so an older node skips a field it does not
know by that field's own length. **Proven in the field on 2026-09-21**: 0.48.1
added two fields and was rolled one node at a time, and fi-1 on 0.48.1 peered
with two nodes on 0.48.0 across control and data lanes, writable at generation
33191, with no telemetry warning on any of them.

What still forces an all-at-once cutover is a change to the *format itself*, as
0.48.0's move to TEL3 did: a node speaking the old one refuses the set outright
rather than misreading it, which is intended and is why that release was not
rolled. (Until 2026-09-20 this
said "build nodes in parallel", contradicting the practice recorded in every
deploy note above it; it was also a checkbox that could never be ticked.)

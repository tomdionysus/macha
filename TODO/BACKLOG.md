# Backlog

Last touched 2026-10-09 (items moot after the object ledger, tombstone
batches and the memory limit removed or moved to ACTIVE). Verified against
the code at 0.90.24 on 2026-10-06: what is still to do from
the sections written 2026-09-05..22. Each section of the old file was checked
against the code, the CHANGELOG and COMPLETED; what was done or superseded is
gone from here, and the old file with its full reasoning is
[`archive/2026-10-06-BACKLOG-before-verification.md`](archive/2026-10-06-BACKLOG-before-verification.md).
`ACTIVE.md` outranks everything here. Grouped by area, not ordered.

## Playback

- **A stale session for the same media may produce a 503 on the next create**
  (n=1, 2026-09-21, unexplained). The throw is unchanged
  ("timed out waiting for first fragmented-MP4 segment",
  `src/playback/playback.cpp:2084`). Reproduce: play, abandon without DELETE,
  create again for the same media on the same node.
- **A generation can be reclaimed between its playlist and its first
  fragment.** A reclaimed pipeline is not revived: the stream route answers
  `404 transformed stream is not active` (`playback.cpp:2720`). Decide whether
  a request revives it; the client's measurement from a visible tab.
- **`look_ahead_ms` after a config reload** is computed from the current
  config (`playback.cpp:2453`), while a running session's segment store keeps
  the `max_ahead` it was built with (`media_segments.cpp:190`). Pick one of
  the three options and announce it.
- **The seek fast path's remux decline** is unobserved: capture a pure seek
  PATCH, read its reason, then decide the bound.
- **Poor-network resilience**: measure on iOS; design pre-packaging; the A/V
  timeline items; a buffer-margin bound and an eviction bound on
  `MediaSegmentStore` spill; coalesced extent fetch in hydration; the UAT
  matrix.
- **The joint test of the session resource** (0.48.0): watch a real 410 be
  classified after a mode switch, with a client.
- **Handover question for the operator**: should a client know a generation
  existed anywhere in the cluster?

## Storage, caches and memory

- **A cache's floor** moved to ACTIVE section 3 (the memory limit can now
  shrink caches). Still here: catch-up logs no progress and ignores a stop
  token, and `startup.phase` reads `ready` whatever the lag
  (`status_api.cpp:818`).
- **The namespace tree's Stage D**: `file_media_id` is derived on every call
  (`filesystem.hpp:607`), not persisted; the namespace tree node cache is not
  in the `memory_*` gauges (add it only if RSS grows unexplained).
- **The block cache cannot be judged from status**: counters exist
  (`status_api.cpp:302`) but a sustained zero-hit, high-eviction cache is not a
  condition. Then revisit fi-1 read latency and whether hydration runs.
- **An unclean exit leaks playback temp files**: no startup sweep of
  `streaming.temp_path`. (A node's own `state/tmp/write.<node-id>.*` files
  are swept at start since 0.90.42.)
- **Zero storage reported healthy**: `data_storage` is ready whether or not a
  backend is online (`local_state.cpp:85`); `storage_backends_online` feeds no
  condition (`status_api.cpp:309`).
- **Retained-memory waits**: the no-progress counter is one global count
  (`filesystem.hpp:408`), not a per-waiter predicate in
  `RetainedMemoryLedger::acquire`; smooth spool pacing; an end-to-end
  shedding test.

## Catalogue

- **The catalogue's cost and residency**: ACTIVE section 4's
  [plan](2026-10-06-catalogue-materialised-view.md); holdings for catalogue
  DATA, and `catalogue/status` asking the store for every artwork, are
  ACTIVE section 2's stage 4 (replication by diff).
- **Clear Metadata is synchronous.** (Its descendant scan goes with the
  plan's derived parent index.)
- **Opaque work ids**: items are still `tmdb:movie:`/`tmdb:tv:`.

## Mount and ingest

- **The adoption gate**: the global `namespace-queue` and `queue-race`
  deferrals remain (`fuse_frontend.cpp:5044`, `:5081`).
- **Two inodes on one path**: resolved at recovery (`fuse_frontend.cpp:4790`);
  the cause is not found and there is no hand-built journal test.
- **`durability_poisoned` is never cleared** (`fuse_frontend.cpp:2416`).
- **Unlink does not short-circuit a pending publication**: data is abandoned
  only after a publication fails with ENOENT (`fuse_frontend.cpp:4055`).
- Unclear, to check: a job shown `queued` while importing.

## Cluster and operations

- **Retiring a node**: a forgotten node comes back on a direct connection
  (`membership.cpp:217`); there is no durable retired state, no exclusion from
  placement and no drain.
- **The control plane under loader I/O**: accept Stage 1 under a real ingest
  on fi-1 or gbni-1 (es-1, where it was set up, is gone); Stage 2, viewer
  latency rather than viewer bytes; pacing must not spend publication retry
  budgets; log auth refusals (401/403); the disk control reserve.
- **Edge nodes**: the 20-minute idle DATA-lane stall check, and a FUSE
  publication from a node that then stops hosting.
- **Status**: the spool summary (only in diagnostics,
  `status_api.cpp:1253`); torrent session health (listen, DHT, trackers,
  stall durations); whether `*_validated` reads true on a converged cluster;
  a powered-off node shown online; the aggregation audit.
- **Multiple endpoints per node**, client failover across them, and `.local`.
- **Maintenance progress** for GC, rebalance, scrub and the inventory, as
  repair has (`diagnostics.repair`); benchmark recipes.
- **An optional MQTT plugin** publishing cluster state.

## Security

- CORS `*` on every response (`src/http/http.cpp:1214`).
- No cookie session for the web client.
- No JSON nesting limit (`src/json.cpp`).
- Allocations sized only by a constant from the wire:
  `telemetry.cpp:472`, `cluster.cpp:143`, `metadata.cpp:1951`.
- Identity-reset tombstones checked only by epoch (`membership.cpp:275`).
- Artwork MIME taken from the request, echoed and served without
  `nosniff` (`catalogue.cpp:1742`).
- Anonymous access: the operator's decision.

## Scaling

The rest of the cliffs found 2026-09-05: `fallback_score` hashed inside a
sort comparator (`placement.cpp:339`, `:395`); `node_info()` copies the roster
per id (`metadata_manager.cpp:270`); the hint store's linear `find_if` (about
eight sites in `catalogue_hints.cpp`); `StagingArea::reserve` walks its
directory (`ingest.cpp:405`); `admit_deferred` scans every inode
(`fuse_frontend.cpp:2938`); `request_shedding_locked` scans linearly
(`retained_memory.hpp:233`).

## Code health and documentation

- Dead wiring: `metadata_replica = true` (`status_api.cpp:627`);
  `PlaybackManager::request_media_profiles` stored and never called
  (`playback.cpp:902`); the `ffmpeg_available` constants (`playback.cpp:3547`).
- Duplicates: `fixed_vod_durations` in `media_engine.cpp:378` and
  `media_engine_common.cpp:17`; `catalogue_api.cpp`'s own `json_escape` and
  `url_decode`.
- About 32 empty `catch (...) {}` (net.cpp, storage_pool.cpp).
- `catalogue_api` answers 503 for some client errors.
- A dead citation in `archive/2026-09-03-playback-resilience-and-av-sync-plan.md:83`;
  unticked boxes in the namespace-publication archive doc; COMPLETED's note
  that 0.24.1 to 0.35.0 is not ledgered.

## Parked for after the refactor (operator, 2026-11-05)

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

## Import (operator 2026-11-06: not part of the port)

- **Fix: a large import submits every file's catalogue hint twice, the second time as a slow
  no-op pass.** Each file's hint goes in when that file lands (`src/acquisition/ingest.cpp:1872`,
  `:1904`, `:2073`), and the catalogue matches as the job runs. After the last copy, `process_job`
  (`:1421`) calls `enqueue_catalogue_hints` (`:1270`), which submits them all again. Every
  `submit_many` forces a durable rewrite of all hints (`src/catalogue/catalogue_hints.cpp:359`),
  though its comment expects one write per discovery pass. Seen on macnessa at 0.93.4, job
  b2bf6286: 7565 files, 5 MB `hints.json`, one thread at 100% from 13:13Z; hints change only in
  `updated_unix_ms`. Meanwhile the job shows `importing`, 100%, a stale `current_file` and rate,
  and logs nothing. **Operator 2026-11-06: remove the second pass.** Read for this: all three
  places that set `file.completed` submit the hint straight after, so a normal run and a resume
  lose nothing. One gap: they set `completed` before the submit, so a submit that throws can leave
  a file recorded complete with no hint, which only the second pass now covers. Submit first, then
  set `completed`, in the same change. Keep the `refresh_catalogue_jobs` call (`:1230`): it refills
  a cataloguing job with no hints. The per-file rewrite during the copy stays a cost of its own.

- Whether an owned source should survive `clear` and `remove_after_ms` while a job has failed
  files: clearing it removes a failed file's bytes, per the docs and a review of the code, so
  the file cannot be retried. Not traced by hand. Core has been told to warn.

## Moved from ACTIVE, 2026-11-08

These were ACTIVE's numbered sections; nothing in them is in progress while Macha is frozen for the port.
The dated plans they name are still in `TODO/`.

### Replication by diff

[`2026-10-09-replication-by-diff.md`](2026-10-09-replication-by-diff.md). Stages 0 to
4 built (0.90.71 to 0.90.77), the survey half of 5 built (0.90.96). Left: repair's
store walk, push cursor, `repair_position` and `have_*` probes, about 75 test call
sites. Dagda dropped these by decision, so this work may fall to the second cutover
rather than Macha. Measure again on the current pair: repair was last measured over
a 1.39 MB/s path that no longer exists.

Also here: a torrent can land on a node away from where it is watched; per-peer down
state in the transport; `fsync` without a deadline; say why a node counts itself busy.

### Memory

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

### The catalogue plan, stages 6 to 9 and 11

Plan: [`2026-10-06-catalogue-materialised-view.md`](2026-10-06-catalogue-materialised-view.md).
Stages 1 to 5 are done (0.90.27 to 0.90.33). Next: stage 6, the conflict
merge per shard; then derived indexes for list, search and the descendant
scan; the manifest change (per-family shards, growable count); batched
profile publication; measure again. Stage 10 (holdings for catalogue DATA)
is the replication-by-diff plan's stage 4; if that needs per-family shards, stage 8 comes
first. Its residency (about 16 MiB) is now a gauge.

### Matching

- **The first track of a match pays the provider again**: the editor's
  search and artwork options were fetched, then the match looked the release
  up and downloaded the cover again (4.1 s for the first track, 0 for the
  second; gbni-1, 2026-10-06). 0.90.86: the cover listing is fetched once
  for the options and the match. Left, each paid once and needed: the
  release record when the editor did not list its tracks first (same seat,
  so cached when it did), and the chosen cover image itself.
- Provider caches are per editor seat and lost on restart.

### Control: what is left, all small

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

### Features, API and optimisation

- **The API is RESTful, all of it**: audit every route; identity resets
  become a resource (decided); `providers/artwork?ref=` and
  `providers/artwork/choose` are among the routes to change.
- **OpenAPI**: generated from a declarative route table that dispatch runs
  from, served at `/api/v1/openapi.json`. Agreed, unstarted.
- **Core's request**: `providers/artwork` taking `item_id`.
- **Per-file readability**: designed; the operator chooses where it goes.
  Two of its three uses are wire changes; its answer (held by nobody
  reachable) comes from the replication-by-diff plan's stage 2, so it follows that.
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

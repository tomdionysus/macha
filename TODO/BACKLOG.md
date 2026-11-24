# Backlog

Last rationalised 2026-11-22, after the store cutover. `ACTIVE.md` outranks everything here; Macha's defects are in
`2026-11-08-found-issues.md`. Grouped by area, not ordered. Items whose subject the port moved into Dannan or Dagda
are gone (the version before is `archive/2026-11-22-BACKLOG-before-rationalisation.md`). **Line references were
written before the port; re-read the code before acting on one.**

## Status and clients

- **Report the underlying Dannan and Dagda versions** (the maintainer, 2026-11-21; after the cutover): the status
  API names each node's Dannan and Dagda versions beside Macha's, in full x.y.z, and the clients show them. It changes
  what the API sends, so it is announced to Core and every client session first.

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

- Catch-up logs no progress and ignores a stop
  token, and `startup.phase` reads `ready` whatever the lag
  (`status_api.cpp:818`).
- **The namespace tree's Stage D**: `file_media_id` is derived on every call
  (`filesystem.hpp:607`), not persisted; the namespace tree node cache is not
  in the `memory_*` gauges (add it only if RSS grows unexplained).
- **The block cache cannot be judged from status**: counters exist
  (`status_api.cpp:302`) but a sustained zero-hit, high-eviction cache is not a
  condition. Then revisit node-f read latency and whether hydration runs.
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

- **The catalogue's cost and residency**: the
  [plan](2026-10-06-catalogue-materialised-view.md) (stages below); `catalogue/status`
  asks the store for every artwork.
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
  on node-f or node-c (node-e, where it was set up, is gone); Stage 2, viewer
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
- Empty `catch (...) {}` blocks: count again on the ported tree (the files once counted are gone).
- `catalogue_api` answers 503 for some client errors.

## Parked for the maintainer (2026-11-05, until after the port; the port is now live)

- **Lost posters on the viewer's path**: the artwork route could answer 404
  at once for an object the last survey found no node holding (442 catalogue
  objects today), saving 1 to 2 s per showing; the cost is that a poster a
  peer gained since that survey is refused until the next one. Or the
  catalogue re-fetches lost artwork from its provider and nothing changes on
  the read path.
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
  extent it holds `unavailable` (seen on node-f after a reboot, about 1M for two
  to nine minutes until the USB disk mounted); Core's web queue drops
  unavailable titles. Should it read `unknown` until the disk comes up?
- From the backlog: anonymous access; whether a client should know a
  playback generation existed anywhere in the cluster.

## Import

- **The classifier and matcher, from the diskA clean-up** (the maintainer, 2026-11-24: greatly improve them; 0.96.1
  did the first part). On live-a the import's gaps were filled by scripts (`build/diska-audit/`: `tv_parse.py`,
  `tv_match.py`, `tv_plan.py`, `music_match.py`), which show what the code must do itself. Left:
  - **Extras** (the maintainer: kept, always under their show or film, never a film of their own): an extras folder
    whose title's episodes are not in the same import is filed as a film today.
  - **Category:** films filed under a show (`Star Voyage (2009)` in the TNG folder, Clubhouse under `/TV`); an
    episode name or a season folder means TV.
  - **Filing:** every episode to `/TV/<Show>/Season NN/`; seen instead: the file name used as the show folder,
    loose episodes at `/TV`'s top, `Season 1` and `Season 01` side by side, the same file stored twice. Same-name
    shows of different years stay apart by year (All Beasts Large and Little 1978 and 2020).
  - **Parsing:** a bare year after the series (`Aftermath 2024 S02E01`); release tags; multi-episode files
    (`S00E04-E05`).
  - **Matching:** well-parsed episodes still `no_provider_match` (SST S04E07, cause unknown); specials numbered
    differently from TMDB's season 0 (match by title, which needs a season's episode list from the provider; the
    API cannot give one); music albums with format suffixes (`CDM`, `CDR`, `WEB`, `Vinyl`), artists joined with `;`
    or `featuring`, `(Disc N)` and catalogue numbers in album names; match a whole album folder to one release by
    its track list; tracks with no album tag need a recording match the API lacks.
  - **Samples and trailers:** skipped at import (`ignore_terms` keeps them out of the catalogue only).

- Whether an owned source should survive `clear` and `remove_after_ms` while a job has failed
  files: clearing it removes a failed file's bytes, per the docs and a review of the code, so
  the file cannot be retried. Not traced by hand. Core has been told to warn.

## Larger work

The dated plans named here are in `TODO/`.

### Left from replication by diff

The proposal (`archive/2026-10-09-replication-by-diff.md`) is overtaken: repair, holdings and the survey are Dagda's.
Still Macha's: a torrent can land on a node away from where it is watched; say why a node counts itself busy.

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
was the replication-by-diff plan's stage 4, now a question for Dagda's holdings. Its residency (about 16 MiB) is now a gauge.

### Matching

- **The first track of a match pays the provider again**: the editor's
  search and artwork options were fetched, then the match looked the release
  up and downloaded the cover again (4.1 s for the first track, 0 for the
  second; node-c, 2026-10-06). 0.90.86: the cover listing is fetched once
  for the options and the match. Left, each paid once and needed: the
  release record when the editor did not list its tracks first (same seat,
  so cached when it did), and the chosen cover image itself.
- Provider caches are per editor seat and lost on restart.

### Control: what is left, all small

The control audits are done (lock audit 0.90.45 to 0.90.49 and 0.90.68;
CPU and memory audit 2026-10-08; control lane fixes 0.90.61, 0.90.64 to
0.90.66). The head record is 87,546 bytes (0.90.67), so whole-record copies
on control threads no longer grow with deletes. Left:
- A dial's peer observer can wait on another thread's roster fsync
  (`persist_m_`): `Membership::observe` writes the roster on the caller's
  thread when it changes. The fix is a combining writer (a caller finding a
  write in progress leaves its roster to the writer), but an identity reset
  must stay durable before it answers. No stall traced to it yet.
- node-c's DATA barrier (`syncfs` on `diskB`) is mean 484 ms, max 5.1 s
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
  reachable) comes from Dagda's availability survey.
- **A media-type context on torrent add** (operator, 2026-09-25).
- **People on catalogue items**: approved, with a backfill.
- **Placement API asks**: the target `node_id` and a viewer-facing detail on
  `placement_failed`; the peer's own refusal text on a forwarded add; an
  optional node `name` (operator's call).
- **Movie sets**: later; needs a proposal first.
- **Ebooks**: stages 1 and 2 of `TODO/2026-11-04-ebooks-design.md` are
  merged. Stage 3, the per-user media state, is next; OPDS is not started.
  **Audiobooks** (operator, 2026-11-04): a separate media class of their
  own, designed after ebooks; they share the per-user media state.
- Measured small, not now: ingest and data publications commit one
  operation at a time (about 100 ms each); the hint store rewrites its file
  per change (266 KB with 409 hints); the head record's largest part is now
  its 72 torrent requests (only if they grow).

## FUSE: libfuse's low-level API (operator, 2026-11-14: the way to go, done once, now the port is live)

Macha's adapter (`src/fuse/fuse_adapter.cpp`) uses libfuse's high-level, path-based API while `FuseFrontend`
keeps its own inode table, so every request is resolved twice and libfuse's node table, path cache, forget counts
and per-request read buffers run in-process, uninstrumented, beside Macha's. The move: `fuse_lowlevel_ops`, Macha's
inodes the only ones, replies from Macha's broker workers (async), Macha-owned reply buffers, interrupts
(`fuse_req_interrupt_func`), lookup and forget counts, readdirplus with a plain readdir fallback, and precise kernel
invalidation (`fuse_lowlevel_notify_inval_inode` / `_inval_entry`), which would let the 1 s entry and attribute
timeouts and `kernel_cache=0` be raised. Expected faster and more controllable; prove it with the baseline's list,
read, write and viewer phases before and after.
- **macOS (must keep working), checked 2026-11-14 against macFUSE's own header** (`macfuse/library`,
  `support/libfuse-3.18`, `include/fuse_lowlevel.h`, copy in `build/macfuse/`): every call above is declared with no
  platform guard. On Darwin `fuse_reply_entry`, `fuse_add_direntry(_plus)`, setattr and the xattr ops take Darwin
  types unless `FUSE_DARWIN_ENABLE_EXTENSIONS` is 0 (`fuse_common.h:61-71`): a thin attribute layer. macFUSE wiki and
  notes: INVAL_INODE, INVAL_ENTRY, DELETE supported; STORE, RETRIEVE, POLL not (ENOSYS since 5.4.0); interrupts
  supported; readdirplus delivery not documented; fd-based zero-copy replies not documented on macOS. Linux nodes run
  libfuse 3.17.2, macFUSE 5.5.0 ships 3.18.3: target the common API. Require macFUSE 5.x. macFUSE 5.5.0 notes its FSKit
  backend on macOS 26/27 rejects an empty FUSE_READ reply with EIO; Macha answers EOF with an empty read.
- **Fixes by design** the read late-write defect found in the heap chase (found-issues 1): `dispatch` returns
  ETIMEDOUT for a running read while the worker still writes into libfuse's buffer. With Macha owning the reply
  buffer it cannot happen; no interim patch unless the heap chase implicates it.
- The libfuse-facing adapter is separate from `FuseFrontend`'s store calls; the move is on the ported frontend.
- **Told to Dagda 2026-11-14 for the port's map:** the read API Macha's file reads use must never write caller
  memory after returning (owned results, or a cancel that waits for the copy), cancel promptly at fetch boundaries
  with the outcome told, offer an async form on a caller's executor, and carry the read class per call.

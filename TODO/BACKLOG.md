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

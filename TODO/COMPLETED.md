# Completed and tested

Last updated: 2026-09-24

## 2026-09-24 -- backlog reconciliation: items found already done

Every item in `ACTIVE.md` below the day's queue was checked against
`CHANGELOG.md` and the code at 0.57.0. These were found done; partly done
items were rewritten in place there to state only what remains.

**Namespace Merkle work (item 0 and the namespace P-1)**
- **Stage B: Merkle namespace as SM14, readable alongside SM13.** SM14 record
  shape and `decode_snapshot` dispatch (0.49.0); stat-only read proven to fetch
  no extent nodes and a corrupt-node fuzz case
  (`tests/test_namespace_tree.cpp`); `macha-metadata-dump --objects` over a
  tree-backed head (0.50.0, replay added 0.51.0).
- **Stage C: commit path carries the change set instead of rediscovering it.**
  0.49.1 (delta applied to the tree, incremental root byte-identical to a
  rebuild, history replay on the tree); 0.50.0 (all nine mutations through a
  working set, every reader/writer converted, prefix scan descends); 0.52.0
  moved the last non-exact `mutate()` caller.
- **Stage E: the migration and its interlock.** `macha-namespace-migrate`
  with `--witness`/`--expect-hash` (0.50.0); live cluster re-rooted 12:41Z
  2026-09-22, 22.79 MiB record to 3.97 KiB (0.50.1).

**Playback session as a resource (P0, 0.48.0)**
- **`POST` to the collection creates a member every time; the logical viewer
  stops being keyed on the bearer.** 0.48.0.
- **`GET` on the collection, under `items`** (the piece that unblocks
  handover). 0.48.0.
- **The stream moves under the session; the token stays in the path.** 0.48.0.
- **The per-account cap ships in the same change**, sharing the transcode
  entitlement key. 0.48.0 (`streaming.max_sessions_per_account`, 429
  `account_session_limit`).
- **Supersession becomes an explicit refusal against that cap.** 0.48.0: a
  second `POST` is a second session; over the cap is `account_session_limit`.
- **Deploy: current web bundle first, build once and cut all three over
  together, `max_sessions` raised to 64/32.** Done 2026-09-21, recorded in
  that section's own deploy note and the 0.48.0 cutover entries below; the
  compiled `max_sessions` default moved to 64 in 0.48.1.

**Seamless handover (P0)**
- **Piece 1, speed factor** -- shipped in 0.47.0 as `stream.production`; Core
  briefed and confirmed.
- **Piece 2, session key in the route** -- superseded by and shipped as the
  server-minted session id in the path (0.48.0).
- **Piece 3, direct-session exemption plus per-account caps** -- the cap
  shipped in 0.48.0; supersession no longer exists to be exempted from.
- **"HELD, not forgotten: `410 generation_superseded`"** -- released in 0.48.0
  with `scope: request`, `node_healthy: true`, `alternative_may_succeed: true`.

**Block cache observability (P1)**
- **Falsified 2026-09-21** -- the cache serves reads at 88x the cold path; why
  the item is P1 and not P0.
- **Counters on `PersistentBlockCache` -- hits, misses, evictions, entries.**
  `stats()`, 0.48.2.
- **Surface them on `GET /api/v1/status`, per node, not rolled up
  cluster-wide.** 0.48.2 (per-node `cache` block).
- **A test that pins a block-cache read-back.** `tests/test_invariants.cpp`
  (a `get` hit on a resident block, counted in `stats().hits`).

**Metadata-stall P0**
- **Root-cause the read-only blink** -- a hung health probe was given the whole
  liveness budget (2026-09-21, per-attempt budget `dead_after / 3`).
- **Regression test for the probe budget** --
  `rpc_cluster/test_a_hung_health_probe_is_retried_inside_the_liveness_budget`.
- **Stop the forever-dial loop at the offline node** -- nothing to do: ordinary
  bootstrap of a node that was down.
- **Make an ingest survive a transient read-only window.** 0.57.0: the
  durability-floor errors are `MetadataNotReady`
  (`src/metadata_manager.cpp:786`), and the ingest now blocks with
  `metadata_unavailable` and retries (`src/ingest.cpp:1246`).

**Loader-I/O P0**
- **Harness built (`run-io-pressure.py`)**: three vantages, `CD--` counting,
  node vitals on one clock; idle and loaded baselines recorded.
- **Stage 4: service-time and paced-admission counters, transition-only
  logging, docs.** `device_service_us`/`device_worst_us`/
  `device_pressure_onsets` (0.51.0), `device_slowdown_percent` (0.52.0),
  `pressure_refusals` (0.53.0) on the per-node data-resource block of
  `/api/v1/status` rather than in diagnostics; onset/release logged once per
  transition (0.57.0); `io_pressure_*` in `docs/configuration.md`.

**Cluster P0**
- **es-1 has no persistent journal -- no longer true**, verified 2026-09-20
  (`/var/log/journal` exists, two boots listed).

**Verified correctness defects (P0)**
- **Unbounded startup wait with no escape** (`wait_for_initial_namespace()`).
  0.41.0: takes the supervisor's stop token and
  `fuse.initial_namespace_timeout_ms` (default 10 minutes).
- **A hard failure in one subsystem takes down the entire macha process** --
  closed by 0.41.0 ("FUSE cannot take the node down any more"): FUSE is a
  supervised `libmacha-fuse` subsystem; a lost mount remounts.
- **`rpc_cluster/test_concurrent_reads_during_divergence_produce_one_reconciliation`**
  -- fixed 2026-09-15 (0.43.0), a test defect.
- **`hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`**
  -- fixed 2026-09-15 (0.43.0), a product defect (concurrent imports racing to
  create the shared scanner root).

**Cluster connectivity, status and operations (P1)**
- **`GET /api/v1/status` took 10 seconds once** -- retired 2026-09-20: the
  0.43.0 reactor removed worker starvation and it had not recurred.
- **`test_storage_data_credit_reserves_viewer_headroom_and_control` hangs on
  aarch64** -- no longer reproduces: full suite green on es-1 at 0.45.0,
  0.46.1 and 0.46.2. Which change fixed it was never recorded.

**Client asks from the placement API round (item -1)**
- **`node_id` on `/api/v1/torrents/jobs/{id}/{pause,resume,retry,cancel}`
  responses (Core).** The torrent route already emits it
  (`src/acquisition_api.cpp:267`, present at 0.48.0), so the item's premise
  that it did not was wrong.

**Code health (P2)**
- **Node-wide `max_sessions` is not on the wire, while
  `max_sessions_per_account` is.** 0.48.1 added `max_sessions` (and
  `transcode_entitlement_idle_ms`) to the per-node `playback` block.
- **The node-scoped session refusal carries no failure axes.** 0.48.1:
  `resource_limit` carries axes -- `scope: node` on create, `scope: request` on
  update, both `node_healthy: true`, `alternative_may_succeed: true`.

**Documentation hygiene (P2)**
- **`docs/streaming.md`, `README.md`, `VALIDATION.md` and `ROADMAP.md`
  contradicted shipped code** -- done in 0.46.3 (2026-09-19/20); `ROADMAP.md`'s
  "stale voters" corrected 2026-09-20.

## 2026-09-24 -- 0.54.0 to 0.57.0

All deployed and verified except 0.56.0 (committed and pushed, `60ce47a`;
deploy blocked by es-1/fi-1 being unreachable). Full detail in `CHANGELOG.md`.

- **0.54.0 -- the torrent's disk I/O is macha's** (stage 1 of the torrent
  disk backend). libtorrent's I/O goes through macha's `disk_interface`,
  admitted at loader class and timed into the disk monitor; the rate clamp
  (`pressure_download_rate`) is gone; `torrent.disk_threads` added. Verified
  overnight on gbni-1 and es-1: zero `CONTROL retention floor` failures and
  zero DATA credit abandons, against seven ingest deaths the evening before.
- **0.54.1 -- artwork is downloaded once, not once a day.** 30-day capability
  and max-age (URLs roll monthly), ETag = artwork id with a 304 before any
  read, `Timing-Allow-Origin`. Verified by the web client on every node over
  http and https.
- **0.55.0 -- extents publish as they verify** (stage 2): file-relative
  extents published on verification and journalled; the ingest commits a
  complete manifest instead of copying; sequential download. Adoption and
  the missing-extent fallback both proven in tests.
- **0.55.1 -- publication does not depend on piece alerts**: verified pieces
  come from the torrent's own bitfield on check, on finish and every 10 s;
  dropped alerts are a WARN. Pretty Woman confirmed publication no longer
  stalls (it still was copied: the ingest starts before publication
  finishes -- open in `ACTIVE.md`).
- **0.56.0 -- every response has a status code; codes are primary.** A
  top-level snake_case `status` stamped centrally on every JSON object
  response; `error_code` beside every text error (ingest, torrent,
  placement reason, hints, status diagnostics); hint `result` is a code.
  Core, web, Android TV and mobile clients notified before deploy.
- **0.57.0 -- a torrent is imported once it is published; a metadata outage
  no longer kills an ingest** (committed, not deployed). The torrent manager
  holds a finished torrent in `downloaded` until every extent is published
  (10-minute no-progress fallback); the ingest logs why it copies despite a
  journal; `ensure_control_local` takes no DATA credit and logs failure;
  `MetadataNotReady` blocks and retries an ingest; DATA pressure onset and
  release are logged. Laptop suite 533/533; torrent suite 13/14 (the swarm
  test cannot run on the laptop). Each fix has a test that fails without it.
- **README version line** under the header, enforced at configure time.
- **Also fixed, test-only:** the durability-barrier restart tests ask again
  until the answer is definitive; the data-credit test's bare client gets an
  `RpcServer` to answer the node's ping.

### Items moved from ACTIVE.md (as they stood)

-5. **`rpc_cluster/test_service_metadata_repair_coalesces_real_generation_burst`
   was a product race, not load noise -- FIXED in the tree 2026-09-23, not yet
   released.** `tests/test_rpc_cluster.cpp:2616`:
   `CHECK failed: s2.node().metadata_announcements() == announcements_before + burst + 1`.

   It passes 20/20 serial and 40/40 parallel on a quiet es-1, and fails
   **12/120** on the laptop at `--jobs 12` -- contention exposes it, it does not
   cause it. Instrumented: the failing run announces generation 16 **twice**
   (34 against 33). `NodeRuntime::accept_metadata_commit` decided whether the
   accepted-head set changed by copying it before and after
   `MetadataReplica::accept_commit`, outside the replica lock. A repeated,
   no-op gen-16 certificate whose two copies straddled the install of gen 17
   saw the heads differ and announced a change it did not make. Only
   duplicates, never a missed change; each duplicate is a spurious
   `metadata_notice` broadcast and epoch bump, i.e. peer cache invalidation for
   nothing -- against the "repeated evidence is intentionally a no-op" contract
   at `src/metadata_manager.cpp` `accept_commit_on`.

   Fix: `accept_commit` reports the `changed` it already computes under the
   lock (optional `bool* heads_changed`), and `NodeRuntime` announces on that.
   After: **0/120** at `--jobs 12`. The existing case is the regression test.
   - [ ] Confirm on es-1 with the next build (`--repeat 20`, parallel).

-4a. **`media_playback/test_abandoned_transcode_pipeline_is_reclaimed_before_session`
   fails 9/20 on the laptop at `--jobs 12` at HEAD `eea4795` (2026-09-23), NOT
   DIAGNOSED.** `tests/test_media_playback.cpp:2054`:
   `CHECK failed: playback.handle(current).status == 200`. Found in a full
   laptop suite run; unrelated to the -5 fix (fails with it stashed). Not
   previously recorded anywhere. Measure on es-1 before diagnosing.

   **Two more, from the laptop full suite of 2026-09-23 with the -5/-4 fixes
   in the tree, neither caused by them, NOT DIAGNOSED:**
   - `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node`,
     `tests/test_rpc_cluster.cpp:4753`: `CHECK failed: state->asString() == "queued"`.
     1/20 at HEAD `eea4795` in isolation at `--jobs 12`, 0/20 with the fixes.
   - `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt`,
     `tests/test_storage_v18.cpp:605`: `CHECK failed: !reopened.has(truncated_id)`.
     20/20 in isolation both with and without the fixes; failed only inside
     the full suite. A presence check that says a truncated object is present
     is worth a look on its own, whatever the timing.

   **`rpc_cluster/test_storage_data_credit_reserves_viewer_headroom_and_control`
   -- FIXED in the tree 2026-09-23 (test only).** Not the barrier's backoff
   after all, though it throws the same message. The node records the test's
   bare `RpcClient` as a peer from the handshake and pings it back over the
   same connection; the client has no inbound handler, `dispatch_inbound`
   throws, and the reader loop closes the connection and fails every pending
   call on it -- hence three different "uncaught" messages and the early
   `blocked_loader` completion. Proven with a temporary log at the throw
   (`type=ping` in every failing run). 10-15/20 before, **40/40** after giving
   the client an `RpcServer` that answers, the pattern the telemetry test at
   the top of the file already uses. The product is untouched on purpose
   (operator, 2026-09-23: don't change the product just to test it);
   `NodeRuntime` installs its handler in its constructor, before any dial.

-4. **P0: both durability-barrier-after-peer-restart tests fail most of the
   time, and neither is a flake or caused by 0.53.0 (measured 2026-09-23;
   FIXED in the tree 2026-09-23, not yet released -- see the ticked boxes).**

   Measured on the laptop, in isolation, `--repeat 8`, with the 0.53.0 changes
   and again with them stashed at `5c5d008`:

   | case | with 0.53.0 | at `5c5d008` |
   |---|---|---|
   | `..._reports_objects_a_restarted_peer_lost` | 5/8 fail | 5/8 fail |
   | `..._rederives_placement_after_peer_restart` | 6/8 fail | 4/8 fail |

   **So they predate this work**, and the spread between 4/8 and 6/8 is noise
   over eight runs, not a change. They pass together often enough that a single
   full-suite run reports zero, one or both — which is how this went on being
   called a flake. It is not one: it reproduces on demand.

   The "reports" case always fails at the same assertion,
   `tests/test_storage_v18.cpp:1385`:

   ```
   REQUIRE failed: unsatisfiable.size() == 1
   ```

   The line above it passes, so the barrier does correctly fail — it just does
   not say **which** object the restarted peer lost. `unsatisfiable` comes back
   empty. That contradicts the contract `DistributedStore::durability_barrier`
   states where it is declared: "ids a peer no longer holds are appended to
   `unsatisfiable` (if given) and the caller must re-put them." With an empty
   vector the caller has nothing to re-put and the publication simply fails.

   **Why, measured 2026-09-23 with `MACHA_TEST_LOG_LEVEL=debug`.** Every
   failing run logs the same thing, and it is not the re-derivation path at
   all:

   ```
   object durability quorum unavailable id=f360... required=2 durable=1 transient=yes
     replica=628c404edda8 epoch=0b1681ce gen=2 outcome="remote-not-sent"
     replica=bef45e617144 epoch=fc979619 gen=2 outcome="local-durable"
   ```

   `remote-not-sent` with the peer present in membership can only come from one
   place: the launch loop in `DistributedStore::durability_barrier` wraps
   `n_.call_async(...)` in `try { ... } catch (...) {}` and **swallows the
   exception entirely**. (The other route to that string requires the peer to
   be absent from membership, which produces `peer-unknown` instead.) So right
   after `b.restart()` the RPC cannot be sent, nobody records why, and the
   requirement is scored `transient=yes` — which returns false early and
   deliberately leaves `unsatisfiable` empty.

   That makes this **two findings**:

   - [x] **Fixed 2026-09-23:** the launch failure is recorded as
     `remote-launch-failed: <reason>` and still scored transient. With it, the
     failing runs say what the swallowed exception was, 5/5:
     **`remote-launch-failed: peer in retry backoff`**. a's dial to b failed
     while b was down, `RpcClient::observe_result` put the endpoint in backoff
     (250 ms doubling to 4 s), and `RpcClient::connection` refuses a new dial
     until it expires -- even though membership already shows b active.
     Original finding: **The `catch (...) {}` is a real defect on its own**, whatever the
     test does. The `outcome` map exists precisely so a failure says which
     peer and whether it was the epoch, the barrier or the transport that said
     no — its own comment records a bare "required=1 durable=0" spinning
     gbni-1 for half an hour on 2026-09-06. Swallowing the launch exception
     reintroduces that blindness one layer down: "could not send" and "did not
     try" become the same string. Record the reason and let it be scored.
   - [x] **Fixed 2026-09-23:** both cases now ask again until the answer is
     definitive (durable, or an id named unsatisfiable), within 30 s -- the
     pattern `..._treats_an_unreachable_peer_as_transient` already used.
     Laptop `--jobs 12 --repeat 20` over every durability-barrier case:
     **100/100**, each restart case now ~2.6 s against ~0.6 s (the backoff).
   - [ ] **Open, a product question, not changed:** an inbound session from a
     peer (proof it is up) does not clear the backoff on that peer's endpoint
     for other lanes, so after a peer restart a node refuses to dial it for up
     to 4 s. Everything that hits it is transient and retried, so it costs
     latency, not correctness. `rpc_cluster/test_storage_data_credit_reserves_viewer_headroom_and_control`
     dies on the same exception (`peer in retry backoff`, 6-8/20 on the
     laptop at `--jobs 12`) and is probably the same test shape; check before
     assuming.
     Original finding: **The tests are racing the transport, not testing the contract.**
     They wait for membership to show both nodes active after the restart,
     which is liveness, not an open data connection. The first barrier
     routinely cannot send at all, so the definitive answer the assertion wants
     (`epoch changed; object absent on peer after probe`) is never reached.
     Either wait for a reachable peer before asserting, or assert across a
     retry — the contract is "ask again", and a single call is not entitled to
     the final answer.

   **Why this was nearly missed, which is the part worth keeping.** Small samples
   lied in both directions. The case passed 6/6 at HEAD and failed 1/3 with an
   unrelated change in the tree, which reads as "the change broke it". Twenty runs
   said the opposite: 10/20 at HEAD, 1/10 with the change. At a true rate near
   50%, three runs are worth nothing and six are worth little. **Use `--repeat 20`
   before attributing an intermittent failure to a change**, and never attribute
   one on a single run.

   - [x] **Confirmed on es-1, 2026-09-23 (0.53.2 build, quiet node): both
     cases pass 20/20 serial and 20/20 at 4 parallel slots.** The platform
     split recorded on 2026-09-17 holds after all; the laptop's ~50% is not
     reproduced on the node. The `catch (...) {}` defect stands regardless.
     Original note: Everything above is macOS/clang, and
     `..._rederives_placement_after_peer_restart` was recorded in 2026-09-17 as
     failing 12/12 on macOS and passing 3/3 on es-1. That it now fails only
     4-6 times in 8 on the same machine means the earlier reading no longer
     describes it either, so re-measure both on es-1 rather than inheriting a
     platform split.



The 2026-09-08 entries below were ledgered by a pruning pass over
`ACTIVE.md`, and cover only the items that pass removed from that file.
0.24.1–0.35.0 is not otherwise ledgered here yet — see the documentation
hygiene item in `ACTIVE.md`.

## The disk resource manager, audited — 0.53.0, verified on the cluster 2026-09-23

Opened 2026-09-22 as a gate after the mechanism had been wrong twice in one
afternoon; closed by `3d40d34` and deployed the same night. The audit found it
wrong in five more places, all fixed:

1. **Every DATA read was measured with `bytes = 0`.** `StoragePool::get()`
   started its timer before the read and `DiskServiceTimer::note_bytes()` --
   the accessor that exists for exactly that -- was never called anywhere in
   the tree. Every read was judged against the flat 25 ms overhead: the
   founding bug, surviving inside its own fix. es-1 had entered and left
   pressure twelve times in the thirty-four minutes it ran 0.52.0.
2. **The torrent clamp never had law 3's second clause** -- it clamped on
   `pressured()` alone. Now clamps only when a viewer is present.
3. **Maintenance was invisible to the signal.** Pool rebalance/scrub/GC read
   backends directly, bypassing the timer. Now timed.
4. **"Viewer present" was byte credit held at that instant.** Now spans
   `maintenance.foreground_quiet`, like the rest of the system.
5. **`io_pressure_outlier_ms` (2 s) -> `io_pressure_outlier_percent` (1000)**:
   the last hardware guess, and an unequal one (396% of expectation for a
   4 MiB write, 8,000% for a 4 KiB read).

Plus `pressure_refusals` on `/api/v1/status` (the counter had existed as a
private member, incremented nowhere) and dead `records_activity()` removed.
Laws 1, 2 and 4 were confirmed rather than assumed, with the device each store
sits on measured per node (control on `nvme0n1p2`, DATA on `sdb1`, on all
three). The parts recorded rather than fixed are item -2 in `ACTIVE.md`.

**Verified in production:** zero torrent clamps on es-1 in the first 16
minutes on 0.53.0 while its `sdb` was busier by read count than during the
flapping; zero on any node since. Both regression tests fail without their
fix (checked by reverting each).

Original entry, unchanged:

-3. **The disk resource manager was audited on 2026-09-22 (0.53.0). It was
   wrong in five more places than the two already recorded; all five are
   fixed. The gate is lifted.**

   The audit was called for after the mechanism had been wrong twice on the
   day it shipped -- a threshold invented rather than derived, and law 3
   flattened so the loader yielded to a slow device with no viewer present --
   and it found that neither correction had reached the read path at all.

   **What it found, worst first.**

   1. **Every DATA read was measured with `bytes = 0`, so the founding bug was
      still live.** `StoragePool::get()` started its timer with zero bytes
      because a read's size is only known once it succeeds, and the
      `DiskServiceTimer::note_bytes()` call that exists for exactly that
      purpose was never written, anywhere in the tree. So every read was judged
      against the fixed 25 ms per-operation overhead alone, with no per-size
      allowance -- the same flat threshold the whole ratio model was built to
      replace. Measured on es-1 during the audit: `sdb` serving 53.7 reads/s at
      240 KB average and 30.9 ms average service, a healthy spinner, and the
      node entered and left pressure **twelve times in the thirty-four minutes**
      since it started 0.52.0, clamping an operator's torrent each time with
      nobody watching anything.

   2. **The torrent rate clamp never had law 3's second clause.**
      `TorrentManager::follow_device_pressure()` clamped on `pressured()`
      alone. An acquisition is durable work the user asked for, so it is
      loader-class and yields to a slow device only when a viewer would
      otherwise wait. The arbiter was corrected for this in 0.52.0; this path
      was not, and it is what produced those twelve log lines.

   3. **The signal could not see the largest consumer of the device.** Pool
      maintenance reads its backends directly rather than through
      `StoragePool::get()`, where the timer lived, so the 51.6 MB/s of
      `macha-maint` reads that saturated `sdb` on 2026-09-22 never fed the
      monitor at all. The mechanism was blind to maintenance in both
      directions: it could not bound it and could not see it. (Object-level
      repair in `DistributedStore::repair_step` does enter the arbiter, as
      speculative. It was pool-level rebalance/scrub/GC that did neither.)

   4. **"Viewer present" was narrower in the arbiter than everywhere else in
      the system** -- byte credit held at this instant, which playback does not
      hold between extents. Every gap in a stream readmitted the loader at full
      concurrency, and the viewer's next read queued behind the extent write
      the gap had just let in.

   5. **The only counter that answers "throttled or unwell" did not exist.**
      `pressure_refusals_` was a private member from the day the gate shipped,
      incremented nowhere and reported nowhere.

   **What it confirmed rather than assumed.**

   - **Law 2.** Admission-wise, there is no path by which this mechanism delays
     a viewer read. `available()` returns true for `foreground` and
     `read_ahead` before pressure is consulted, on both `acquire()` and
     `try_acquire()`, and a viewer arriving while pressure is engaged takes the
     same path; the viewer reserve is subtracted from lower-class capacity so
     background work can never occupy it. One physical path remains and is
     deliberate: `min_background` (floor 1) means one background operation may
     be on the spindle ahead of a viewer's read. That is law 3's trickle bought
     at law 2's expense, and it is now stated rather than implied.
   - **Law 1.** The control store is a separate `LocalStore` at
     `metadata_store.path`, constructed outside `StoragePool`, with no timer
     anywhere on its path, and `acquire()` throws on `FrameType::control`.
     Measured on the hardware: control sits on `nvme0n1p2` (ROTA=0) on all
     three nodes, DATA on `sdb1` (9.1 T, ROTA=1) on gbni-1 and es-1, and fi-1
     holds no extents at all. **Law 1 holds by configuration, not by
     construction** -- nothing stops an operator pointing `metadata_store.path`
     at a DATA spindle, and the FUSE spool (`/mnt/diskB/spool`) and ingest
     staging (`/mnt/diskB/ingest`) already sit on the DATA spindle, as plain
     file I/O the monitor never sees and the arbiter never bounds.
   - **Law 4.** Background work always drains: the refusal is
     `lower_active_ >= min_background`, floored at 1, so with nothing active a
     lease is always admitted, operations keep completing, and the signal can
     never starve itself of input. The hysteresis band is a latch, though:
     pressure engages above 300% and releases below 150%, so a device that
     settles anywhere between stays pressured, and the average does not decay
     without traffic. Neither wedges the node -- the trickle drains -- but
     "pressure always releases on a device that recovers" is only true if it
     recovers past 150%.

   **Still open, recorded rather than papered over:**

   - [ ] **One monitor covers a whole `StoragePool`, not one device.** A pool
     may hold several backends on several devices and this cannot tell them
     apart, so one slow backend makes the pool pressured for work bound
     anywhere in it. Harmless today -- gbni-1 and es-1 configure exactly one
     DATA backend each -- and it bites the moment a second is configured.
     Per-device pressure also needs the arbiter to know an operation's
     destination device, which it cannot: admission happens before placement
     picks a backend. The false claim in the header comment is corrected.
   - [ ] **Local disk maintenance is budgeted from a network measurement.**
     `estimated_network_bps()` feeds `local_credit` as well as
     `network_credit`, which is how a GC/repair pass helped itself to
     51.6 MB/s of one spindle. See the third bullet of the item below; no
     number is proposed here, because inventing one is what started all this.

## Maintenance outranked the ingest — the loader clock, 0.53.0, verified 2026-09-23

Opened 2026-09-22 22:00Z, closed by `3d40d34`. The maintenance busy decision
was `playback_busy || interactive_busy`, fed by clocks only `foreground` and
`read_ahead` writes touch; an ingest fed neither, so a node importing 36 GB
called itself idle and handed maintenance its idle share of the spindle the
import was waiting on, with `busy_bandwidth_fraction: 0.0` already set and
unable to help. A loader activity clock now sits beside the two viewer ones,
fed from the loader read and write paths, and is consulted by the busy
predicate, all four slice-yield predicates and the busy-pass wake-up. It is
deliberately not folded into the viewer clocks;
`test_a_loader_write_is_visible_to_maintenance_as_its_own_class` asserts both
halves.

**Verified in production, 2026-09-23 12:53Z on gbni-1:** three concurrent
imports at **13 MB/s aggregate** (810 MB in 60 s) with `macha-maint` reading
0 MB and using 0% CPU over the window, `sdb` at 34-49% utilisation. The day
before: ~2 MB/s, `macha-maint` at 51.6 MB/s, `sdb` at 91%. The "measure the
import rate again" box is ticked with that number. The `max_bandwidth: 0B`
question and the network-budgets-local-disk finding moved to `ACTIVE.md`
item -2; the restart-mid-put failure moved into item -3 there.

Original entry, unchanged:

-2. **Maintenance outranked the ingest because the mechanism to stop it was
   deaf to the loader (found 2026-09-22 22:00Z, FIXED in 0.53.0; the rate is
   NOT yet re-measured on the cluster).**

   An operator's 36 GB import ran at ~2 MB/s on gbni-1 while, measured on the
   node:

   | | |
   |---|---|
   | `sdb` utilisation | **91.2%**, aqu-sz 3.23 |
   | reads | **37 MB/s**, 151/s |
   | writes | **2.8 MB/s**, 14/s |
   | `macha-maint` thread read rate | **51.6 MB/s** |
   | torrent download | complete, directory static at 63,255 MB |

   So the disk was saturated by maintenance reads while the ingest's writes got
   2.8 MB/s. The torrent was not downloading; two rounds of fixes aimed at the
   DATA pressure gate and at namespace-node replication changed nothing,
   because neither touches maintenance.

   **Why the existing knobs did not help.** gbni-1 has
   `maintenance.busy_bandwidth_fraction: 0.0` -- maintenance is meant to get
   *nothing* while the node is busy -- and `max_bandwidth: 0B`, which is "no
   configured cap". The busy decision is `playback_busy || interactive_busy`
   (`src/service.cpp:1174-1184`), fed by `foreground_idle_for()` and
   `interactive_idle_for()`. Those clocks are written only for
   `FrameType::foreground` and `FrameType::read_ahead`
   (`src/filesystem.cpp:248-252`, `:996-998`). **An ingest is loader-class and
   feeds neither**, so during an import the node reports itself idle and
   maintenance takes its idle share of a disk somebody is waiting on.

   That is law 3 in a third place: the loader must outrank background work, and
   here background work cannot even see it.

   - [x] A loader activity clock on `DistributedStore` beside the foreground
     and interactive ones, fed from the loader read and write paths, and
     included in the maintenance busy predicate and in all four slice-yield
     predicates and the busy-pass wake-up. It is **not** folded into the viewer
     clocks, and `test_a_loader_write_is_visible_to_maintenance_as_its_own_class`
     asserts both halves: the loader clock moves, the two viewer clocks do not,
     and `viewer_recently_active()` stays false -- because viewer reserves, the
     pressure gate and the torrent clamp all key off the viewer clocks, and
     conflating them would make an import look like a viewer and gate other
     loader work behind it.
   - [ ] **Then measure the import rate again.** Nothing here is verified on
     the cluster yet; 0.53.0 is built and green on the laptop only. Everything
     claimed about throughput on 2026-09-22 was wrong at least once, so the
     only numbers worth trusting are the per-thread `/proc/<pid>/task/*/io`
     deltas and `iostat`. Take a before reading with maintenance running
     against an idle node, start an import, and confirm `macha-maint` drops to
     nothing while it runs.
   - [ ] Consider whether `max_bandwidth: 0B` should mean "no cap" at all on a
     node whose DATA backend is one spindle. The observed-bandwidth fallback
     let a GC/repair pass take 51 MB/s -- and note what the audit found behind
     that: `estimated_network_bps()` budgets `local_credit` as well as
     `network_credit`, so **local disk maintenance is rationed by a network
     measurement**. The loader clock stops maintenance during an import, which
     is the case that hurt, but it does not make the budget mean anything on a
     node with one spindle. No number is proposed here on purpose.

## A commit publishes what changed, not what exists — 0.53.1, verified 2026-09-23

Found and fixed on 2026-09-23 (`344fc28`); it was never an `ACTIVE.md` item
because the symptom arrived as `ingest failed: CONTROL retention floor
unavailable before metadata publication` on gbni-1 and then es-1, killing
torrent ingests outright, and was diagnosed live.

`retain_control` collected a commit's referenced control objects and
`put_graph_on` uploaded every one of them to every candidate peer, with no
presence check anywhere on the path. Tolerable while a graph was 65 catalogue
shards; once the namespace was tree-backed a commit referenced its whole
spine. Measured on gbni-1: **three minutes of importing, 25 commits, 5,469
control objects pushed to peers, control store grew by zero.** Over two hours
8,924 against a store of 4,162. The same defect 0.51.0 fixed on the
replication path, never applied here.

The failure it produced: 840 concurrent puts overran the connection's outbound
queue (`max_peer_outbound = 256`, shared with all callers), every candidate
peer was skipped, `retained=1` against `required=2`. The size correlation was
exact -- every commit of <=65 objects succeeded, every commit of >=828 failed,
353/354 did both -- and the failures returned *faster* than successes because
nothing was ever sent. Three `catch` blocks on the RPC path had discarded the
reason, so `peer outbound queue full` had never once been logged on any node
and the symptom read as a dead network link. The first hypothesis (the
pending-reply cap, 512) was wrong; instrumenting the swallowed exception is
what named the real one.

A commit now asks each peer which referenced objects it is missing over a new
CONTROL-plane `have_control_objects` message (`have_objects` reads
`local_store()` and cannot answer for control objects; the new handler takes
no DATA admission, per law 1) and sends only those. Bytes are read lazily. The
publication is also bounded to a quarter of the smaller of the connection's
two budgets. The test asserts via per-message-type RPC counters: 1024 sent on
the first commit, **zero on the second**, exactly 10 on the third after
deleting 10 from the peer. Rolled peers-first so the sender's probe had
someone to answer it.

**Verified in production:** every commit on gbni-1 since logs
`CONTROL graph peer=... referenced=225..316 missing=0`, `control_ms` 240-430,
`outcome=ok`; zero retention-floor failures on any node since. Note the
referenced set is still 225-316 ids per commit and growing -- cheap now, not
small; that is the batching question in `ACTIVE.md` item -3.

## The torrent alert stream has its own log level — 0.53.2, deployed 2026-09-23

`894a211`. With the process at `DEBUG`, libtorrent's DHT and tracker alerts
were 99,088 of the 99,187 lines in gbni-1's journal and had evicted the
previous night's diagnostic record within nine hours. `torrent.log_level` is
an independent threshold like `ffmpeg_log_level`: `INFO` (default) keeps
listen, DHT-bootstrap, port-mapping and warning lines; `DEBUG` bridges every
subscribed alert; `ALL` also subscribes libtorrent's internal log categories.
Emits past the process filter; applies live on reconfigure including the
session's alert mask. gbni-1's journal went from ~1,420 lines a minute to 1
with the useful lines intact. Written explicitly as `INFO` into every node's
config.

## A telemetry set cannot carry an optional field safely — fixed in 0.48.0 as TEL3, not yet deployed

Opened 2026-09-18, closed 2026-09-21 by `b821808`. Every field is now tagged
and length-delimited inside a length-delimited record, so a record's optional
fields are bounded by the record rather than by the payload, and an unknown
field is skipped by its own length. Both open items are answered: the
length-delimiting itself, and the compatibility story, which is a new magic
(`TEL3`) rather than a version byte — a node speaking the old positional
format refuses the set outright instead of misreading it, and every node moves
at once. The third item, "treat adding a telemetry field as requiring both
nodes to be upgraded together", survives as the standing deployment rule and
is no longer a workaround for this defect: TEL3 makes a straggler's set
*refused* rather than *misparsed*, which is the outcome that item wanted.

One claim in the original entry was corrected when the fix was built: the
format was **not** broken between peers of the same version. A sender writes
every field, so a same-version reader consumed exactly one record. Reverting
the framing and re-running the test said so. The real case — a set whose first
record carries a field this build has never heard of, where the record after it
must still decode — is now pinned by a test.

The worry the entry ended on ("the set that does not drop") is retired by
construction: a known field at the wrong width is now corruption rather than a
version difference, and is refused.

Original entry, unchanged:

## P1 — A telemetry set cannot carry an optional field safely (opened 2026-09-18)

`encode_telemetry_set` writes a magic, a count, and then the records
back-to-back with **no per-record length**. `decode` reads its optional
trailing fields by asking `reader.remaining()`, which in a multi-record set is
non-zero because the *next record* follows. So a decoder that knows about a
field the sender did not write consumes the next record's bytes as that field,
and the whole set fails to decode.

Gossip sends up to 64 records per set (`src/cluster.cpp:1146`), so this fires
during any rolling upgrade that adds a telemetry field, in both directions,
until every node matches. It has been true of every telemetry field added so
far -- `phase`, `api_endpoint`, `cpu_cores`, `memory_total_bytes` -- and the
comments on those fields claim a rolling-upgrade safety the format does not
provide for sets. It is only genuinely safe for a single-record set.

Found on 2026-09-18 by adding the playback budgets in 0.46.2, which produced
`persisted telemetry ignored: blob too large` on each node's first start (the
persisted cache is the reliably multi-record case). That is self-healing -- the
cache is rewritten in the new format -- and both nodes were upgraded together
to close the wire window, so nothing is currently degraded.

**The worry is not the dropped set, it is the set that does not drop.** A
misparse usually throws, because a length prefix read from the wrong offset is
absurd. It is not guaranteed to: a record could decode into plausible-looking
garbage and be believed. Nobody has looked for that case.

- [ ] Length-delimit each record inside a telemetry set, so a record's optional
  fields are bounded by the record rather than by the payload.
- [ ] Decide the compatibility story for the format change itself, which has
  the same one-upgrade cost it is fixing. A version byte in the set header is
  the obvious shape.
- [ ] Until then, treat "add a telemetry field" as requiring both nodes to be
  upgraded together, and say so wherever that pattern is documented.


## "AC-3 copied into fMP4 stalls the node" — NOT A SERVER DEFECT, closed 2026-09-21 the day it was opened

**Three client sessions independently concluded the server could not copy
(E-)AC-3 into fragmented MP4. They were wrong, and so was every mechanism I
proposed for it.** Closed by a controlled reproduction on fi-1 with libav
raised to VERBOSE, coordinated with the web client, against the real title on
the real node.

**The measurement that settled it.** Session `ed9b9eb7...`, trace `160e80a0`,
a true copy plan (`mode=remux rank=1`, the negotiator did not decline it):

```
18:45:44 pipeline start mode=remux
18:45:44 first fragment ready elapsed_ms=48 segments=1
18:45:44 pipeline startup complete elapsed_ms=48
```

**48 milliseconds** against a 15,000 ms budget — faster than the AAC control's
89 ms. Zero errors on the node. And then **no stream request ever arrived** for
that session.

**The actual cause, from the client's browser:**

```
MediaSource.isTypeSupported('audio/mp4; codecs="ac-3"')     -> false
MediaSource.isTypeSupported('audio/mp4; codecs="ec-3"')     -> false
MediaSource.isTypeSupported('audio/mp4; codecs="mp4a.40.2"') -> true
```

hls.js parsed the manifest, MSE rejected the audio codec, and it never asked
for media. The element sat at `readyState` 0 while the player's own status line
correctly showed the plan the server had built. The "six non-fatal then two
fatal errors" arrived ~39 s later, long after the pipeline was ready: hls.js
failing on the manifest, not a fetch that failed. The client had asked the
server to copy audio its browser cannot decode.

**Four hypotheses died here, all mine, none of which reached the code:**

1. **`delay_moov` / AC-3 cannot fill `dac3` on a copy.** Killed by experiment
   on es-1 before any client run: ffmpeg with macha's exact movflags copies
   AC-3 into fMP4 happily, and refuses with *"Cannot write moov atom before AC3
   packets"* **only when `delay_moov` is removed**. The flag is the fix and it
   works. Core independently killed it from the source with the better
   argument: the mechanism contradicted the comments it cited, because if a
   copy never produced a parsed packet then the 2026-09-07 fix could not have
   worked, as it works by waiting for that packet.
2. **AC-3 vs AAC in fMP4 generally.** A probe replicating macha's exact muxer
   configuration — `delay_moov` **and** `frag_custom` **and** a custom
   `AVIOContext` — produced a first fragment for both codecs, from the real
   failing title through macha's own mount, in 0.73 s.
3. **A supersession/PATCH defect with WAN contention.** Built on 8-of-8
   timeouts being on `PATCH` and none on create. Real split, but a sampling
   artefact: every create the clients made that day happened to transcode
   audio and every PATCH happened to copy it. Killed when the phone saw the
   same failure at *create* on a node that owns its extents.
4. **The first cut consumed by the delayed moov.** A comment at
   `media_engine.cpp:1435` says a flush writing the moov produces no moof, so
   the first published fragment would need a second keyframe-aligned cut.
   Measured: both codecs publish on cut 1.

**Why it is worth this much space.** The symptom was reproduced by three
independent clients on three codebases and the correlation with the audio codec
was perfect. It was still not a server defect. Every mechanism that fitted the
symptom was wrong, and the only thing that settled it was instrumenting the
running node and reading what it actually did.

The standing rule that came out of it, from the web client and worth borrowing:
**do not treat a direct-play stall as a node fault without first checking what
the element is actually pointed at.**
## P0 — Copying (E-)AC-3 into fragmented MP4 never produces a first fragment (opened 2026-09-21)

**Supersedes the supersession theory below, which was wrong.** A controlled
experiment from the web client, relayed by core, split it cleanly: on one node,
minutes apart, both created in `direct` and switched by `PATCH`:

- **H.264 + AAC stereo → remux with audio COPIED: works.** Plays, `FMP4`,
  `generation-update-ready`.
- **H.264 + AC-3 stereo → remux with audio COPIED: never plays.**
  `readyState` 0, position 0, six non-fatal hls errors over ~35 s, two fatal
  at ~59 s.

So remux is not broken and the `PATCH` path is not broken. **Copying (E-)AC-3
into fMP4 is what does not complete.** The phone sees the same family at
*create* — `503 playback_pipeline_start_failed`, "timed out waiting for first
fragmented-MP4 segment", four attempts across **two different nodes**, every
one AC-3 or E-AC-3 (2010 AC3 5.1, Avatar EAC3 5.1). Two nodes matters: it
retires the WAN-contention hypothesis, since one of them owns its extents.

**RETRACTED 2026-09-21, by experiment, before any code was changed. The
`delay_moov` mechanism below is WRONG and is kept only so nobody re-derives
it.** Two controlled experiments on es-1, against the same libavformat 61 the
server links:

1. **ffmpeg CLI, macha's exact movflags.** AAC copy into fMP4 with
   `delay_moov`: works. **AC-3 copy into fMP4 with `delay_moov`: works**,
   2.3 MB of output. AC-3 copy *without* `delay_moov`: fails with
   `"Cannot write moov atom before AC3 packets. Set the delay_moov flag to fix
   this."` — which confirms the comment is correct and the flag is doing its
   job.
2. **A probe replicating macha's muxer configuration exactly** — `delay_moov`
   **and** `frag_custom` and a custom `AVIOContext`, which is the part the CLI
   cannot reproduce. Both AAC and AC-3 wrote zero bytes at
   `avformat_write_header` (identical, so the deferred moov is not
   codec-specific) and both produced a first fragment on the first explicit
   flush: AAC 3,797 bytes, AC-3 1,296 bytes.

**So AC-3 muxes into fragmented MP4 correctly in macha's own configuration,
and the mux path is not where this fails.** Core independently reached the
same conclusion from the source, pointing out that the mechanism contradicted
the very comments it cited: if an AC-3 copy never produced a parsed packet,
the 2026-09-07 fix could not have worked, because it works by waiting for
exactly that packet.

**What the experiments do NOT rule out**, stated so the next attempt starts in
the right place: both read a file from local disk rather than through macha's
own source IO, and both used a substitute AC-3 title rather than the two that
actually failed (2010, AC3 5.1; Avatar: Fire and Ash, EAC3 5.1), which are not
on the node tested. A 5.1 layout, or the DHT-backed read path, remain
untested.

**The symptom is solid; only the explanation was not.** Three clients
reproduced AAC-copy-works against AC-3-copy-stalls on one node within minutes,
and the television measured `elapsedMs 15051.5` against that node's advertised
15,000 ms budget — `wait_for_initial_fragment` timing out exactly.

The superseded reasoning follows.

**The mechanism is in a comment this repository already carries.**
`src/media_containers.cpp:93-96` states it: *"(E-)AC-3 needs the muxer to parse
a packet before it can write the dac3/dec3 sample-entry box, which is what
`delay_moov` does. Measured on this libavformat: without it the header write
fails 'Invalid argument' (the 503s of 2026-09-07)."* And
`src/media_engine.cpp:1358-1366` sets `delay_moov` on **every** fMP4 output,
not only for those codecs.

So for an AC-3 **copy** there is no decoder and no parse step of our own, the
muxer defers `moov` until it can fill `dac3`/`dec3` from a parsed packet, and
if that never happens the init segment is never written — no init, no first
fragment, and `wait_ready(startup_timeout)` returns false. That matches the
throw site exactly: `src/playback.cpp:1578` is reached only when the wait
failed **and** `state.error` is empty **and** the pipeline is still running.
Not a mux rejection, which would have said "libav pipeline failed before first
fragment"; not an exit, which would have said "ended before first fragment".
Alive, no error, no fragment.

**THERE IS A SECOND, UNRELATED CAUSE OF THE IDENTICAL MESSAGE, and it is a
confound for the evidence above** (Android TV, 2026-09-21). That session
force-stopped its app mid-playback, leaving an orphaned session on
`10.35.1.50` for `tmdb:movie:583`. Relaunching and playing **the same title on
the same node** then failed with the same error — `POST` this time,
`elapsedMs 15026.8`, `503`, "timed out waiting for first fragmented-MP4
segment" — on a plan of **video copy + audio transcode**. No AC-3, no remux, no
audio copy anywhere in it. Deleting the orphan and replaying the same title on
the same node succeeded immediately.

n=1 each way and that session claims no mechanism. Neither do I, but one
candidate is ruled out already: **it is not the probe coalescing path**, which
throws its own distinct "timed out waiting for concurrent media inspection"
(`src/playback.cpp:1225`) rather than this message. What an orphaned session
holds that a *second session for the same media* then waits on is the open
question — retained memory held by a pipeline that was not yet reclaimed is a
candidate worth eliminating first, since it would stall without recording an
error, which is what the throw site requires.

**Consequences for the AC-3 investigation, which matter more than the second
bug itself:**
- The phone's four AC-3 failures are not clean evidence until it is known
  whether they left orphans behind — that is exactly how the television
  produced this one. Ask before treating four attempts as four data points.
- "Timed out waiting for first fragmented-MP4 segment" is now known to have at
  least two causes. Do not attribute an instance to `delay_moov` without
  checking the node for a live session on the same media.
- It is the same family as the entitlement work above: an orphan holding
  resources for one media, rather than an orphan holding the node's only
  transcode slot.

- [ ] Establish what a second session for one media waits on when an earlier
  session for that media is still live. Retained memory first.

- [x] ~~Find out whether `moov` is ever written.~~ Done by experiment: it is,
  for both codecs, in macha's exact configuration. Not the question.
- [ ] Reproduce with the **actual failing media** — a 5.1 (E-)AC-3 title —
  and through **macha's own source IO** rather than a local file. Both were
  substituted in the experiments above and both are untested.
- [ ] Then instrument the pipeline rather than the muxer: if libav writes a
  fragment when driven directly, the question is what macha's cut logic or
  first-fragment wait does differently on these sources.
- [ ] If the muxer needs a parsed frame we are not giving it, attach a parser
  or a bitstream filter on the AC-3 copy path.
- [ ] **Second, separable defect: plan selection is not deterministic.** The
  same Remux request produced `VIDEO COPY` + `AUDIO TRANSCODE · AC3 → AAC`
  once and a true `REMUX · ENG · AC3` copy minutes later on the same node —
  and **the one that played is the one that declined to copy**. If declining
  is correct, the bug may be that it sometimes does not decline.
  `max_audio_transcodes` is 4 on these nodes and the entitlement is per logical
  viewer, so slot availability differs between two attempts seconds apart and
  is a candidate for what selects between the plans.
- [ ] Until then, `carriage_facts` claims `ac3`/`eac3` are fMP4-copyable
  (`src/media_containers.cpp:115-116`) and `docs/streaming.md` repeats it. If
  the copy cannot be made to work, that claim is what is wrong and MPEG-TS is
  the documented route for those codecs.

**Ruled out and worth not re-chasing:** the web client's two remux *seek*
failures carrying the same message were `VIDEO COPY` + `AUDIO TRANSCODE` on
HEVC + E-AC-3 — audio not copied — so they are a poor match and probably a
separate fault.

## P0 — The first-fragment timeout on PATCH: LARGELY SUPERSEDED, see the AC-3 item above (opened 2026-09-21)

**Eight first-fragment timeouts on fi-1 on the day of the 0.48.0 cutover.
All eight on `PATCH`. None on create**, on a node that served 18 successful
creates in one 30-minute window. Reported independently by all four client
sessions as a seek failure, which it is — a large seek is a `PATCH` — and none
of them could see that creation never fails the same way.

The replacement path marks the outgoing generation superseded but does not stop
it before starting the replacement (`src/playback.cpp:2734-2744`), and the
comment there says so deliberately: the old generation must keep serving until
the new one can. So both pipelines are alive during the handover. On a node
that owns its extents the second one reads from local disk and wins easily. On
fi-1, which owns none, **both pull 4 MiB stripes across the WAN at 772-3431 ms
each** against a 15 s startup budget.

Viewer-visible: a control freezes for a quarter of a minute and then errors.
Clients recover, sometimes unaided, so it is survivable — but the viewer is
waiting behind a generation they have already abandoned, which is governing
law 2.

- [ ] **First: run a PATCH seek on es-1 or gbni-1**, which own their extents.
  If the timeout does not reproduce there, it is WAN contention during the
  handover window and not supersession being too expensive. **Change nothing
  before this experiment** — the mechanism above is inferred from code and
  timings, not instrumented.
- [ ] Then decide whether the outgoing pipeline should still be drawing WAN
  bandwidth once its replacement is committed.
- [ ] Do **not** just raise `startup_timeout_ms` on fi-1. It makes the viewer
  wait longer to be told the same thing.

Collated evidence from all four clients:
[what the clients report](2026-09-21-what-the-clients-report-against-0.48.0.md).

## One abandoned session held a node's only transcode slot for 30 minutes — fixed in 0.48.1, deployed 2026-09-21

Opened and closed the same day. A transcode entitlement was held until the
session was erased, so it outlived its own pipeline by `session_idle_ms`:
thirty minutes against sixty seconds. With `max_video_transcodes` at 1, one
client that crashed, was force-stopped or was reaped in the background closed
that node to transcoding for everybody for half an hour.

Measured on fi-1 the day 0.48.0 shipped: 57 session creates, zero deletes, and
three separate client sessions refused a transcode by a node nobody was
competing for.

**The fix, on the operator's decision, taking core's threshold over the one
originally proposed here.** The entitlement is released after
`streaming.transcode_entitlement_idle` — five minutes by default — of no
*stream* activity, and reacquired on resume where it may be refused. Releasing
it at pipeline reclamation instead, which was the first implementation, was
kinder to the node and crueller to the viewer: a sixty-one second pause could
lose the slot wherever anything else wanted it, and at one transcode per node
that is any second viewer at all.

Keyed on stream activity rather than control traffic, on the operator's
wording, which makes the client contract one sentence: ask for a stream object
inside the window — a playlist fetch is enough — or reacquire on resume and
risk a 429. Polling the session keeps the session alive and is deliberately not
evidence that anyone still wants media.

The release is per session: it clears only what that logical viewer holds and
is skipped entirely while any sibling session shares it.
`transcode_entitlement_idle_ms` rides telemetry to the per-node status block so
a client can time its keep-alive against the node it is actually on.

**What it does not fix, and why the client-side work still matters:** it bounds
the damage from thirty minutes to five. The session still occupies
`max_sessions` and the per-account cap until `session_idle`, and an orphan is
still the precondition for the unreproduced stale-session 503. The cause of the
orphans themselves turned out to be one bug in core — `stop()` returning
silently when an in-process map entry was missing, discarding every client's
cleanup — fixed at core `61e4d74` the same evening.

## P0 — One abandoned session holds a node's only transcode slot for 30 minutes (opened 2026-09-21)

fi-1, day of the cutover: **57 session creates, 0 DELETEs**, 18 creates in the
last 30 minutes alone. No client deletes its sessions — not one, all day,
across four client sessions. With `session_idle_ms` at 30 minutes the node
carries roughly 18 live sessions nobody wants.

**The server is not leaking.** Expiry works and returns the slot. The defect is
the interval: with `max_video_transcodes: 1`, one abandoned session that once
transcoded denies transcoding to the whole node for up to half an hour. That is
mobile's seven `resource_limit` refusals, TV's wall, and the web client's
mode switch refusing itself.

**The part that is ours.** A pipeline is reclaimed after 60 s *because no
stream request arrived* — the server has already concluded nobody is watching.
That same evidence may not release the transcode entitlement, which outlives it
thirtyfold. The contract in `docs/streaming.md` is deliberate and the reasoning
is sound (an entitlement evaporating on reclamation would break
resume-after-pause against a busy node), but on a node admitting **one**
transcode the cost of that guarantee is the entire node.

- [ ] Decide: should reclamation release the entitlement when the node is at
  its transcode limit, reacquiring on resume and accepting a refusal then? That
  trades a certain 30-minute outage for a possible refusal at resume.
- [ ] Separately, every client needs to `DELETE`. Client-side, and not a fix
  for this.

## The HTTP server without a thread per connection — 0.43.0, cluster UAT passed 2026-09-20

Plan: [the HTTP server without a thread per connection](2026-09-15-http-server-reactor-plan.md).

**The API was served by sixteen worker threads, and a worker was spent on every
kind of waiting the server does** — a kept-alive connection idling up to 15 s, a
held segment request waiting on the encoder, a slow viewer draining a segment
over the WAN, a direct-play read fetching from a remote replica. Only running a
handler is work. When the pool was gone the node stopped answering health and
status, which is a governing-law-1 violation, and `max_concurrent_holds` was 8
purely to ration that pool.

Shipped in 0.43.0 (2026-09-15): one reactor thread owns every socket and never
waits; a bounded compute pool in two lanes, control for
health/status/session/users and data for everything else; the two routes that
wait on the media pipeline return a deferred result and are woken by the segment
store instead of parking a thread. `max_concurrent_holds` became 64, a fairness
bound rather than a thread ration.

**Cluster UAT passed 2026-09-20 on the condition the plan set itself.** Deployed
to es-1 and fi-1 on 2026-09-15. Over the five days since, es-1's journal carries
**zero** `reactor stall` lines and 192 slow-request lines from the log this plan
built. It held through the 2026-09-19 loader-I/O incident, which saturated the
disk at 86% iowait and made handlers slow without the reactor ever sleeping. A
deliberate 8 GB reproduction on 2026-09-20 measured control-plane service time at
**p99 1.2 ms on-box and 2.0 ms at haproxy, with zero client aborts**, under 2,048
durable extent puts and a 4.8 s durability barrier.

The "Status took 10 s" item in `ACTIVE.md` was retired with it: the plan named
worker starvation as the remaining hypothesis and said the reactor would settle
it, and it has not recurred in five days.

**What this did not cover, found by that same incident:** the reactor never
waits, but the disk underneath it obeys no law at all. Control was protected
here; the data lane was not. That is the P0 at the top of `ACTIVE.md`.

Not in scope, as planned: `sendfile`, reactor sharding, HTTP/2, in-process TLS,
and the RPC transport, which is also thread-per-connection.

## A seek goes where it was asked to go — 0.46.0, 2026-09-18

Plan: [a seek goes where it was asked to go](2026-09-18-seek-does-what-it-is-told-plan.md).
The open remainder — the seek fast path never being taken — stayed in
`ACTIVE.md` as a P1 rather than being carried along here.

**A remux seek starts after the position asked for, by up to 9.3 s, always
forward, and the content in between is in no generation at all.** No client can
recover it. Measured on es-1 and fi-1 on 2026-09-17 across four occasions, worst
case 9,293.9 ms, confirmed independently by the browser's own media-element
duration arithmetic. The operator watched it happen. Plan:
[a seek goes where it was asked to go](2026-09-18-seek-does-what-it-is-told-plan.md).

`media_vod::indexed_plan` (`src/media_vod.cpp:41-63`) discards every keyframe
earlier than the request and takes the first survivor; the transcode paths do
the same through `nearest_keyframe_at_or_after` (`:97-105`). The alignment is
deterministic, so a client bound that rejects an over-far start cannot make
progress: 147 `session-update` calls in 33.3 s with the seek never landing.

**The governing rule, stated by the operator: the server does what it is told.**
It does not change the mode a client asked for and it does not move the position
a client asked for. Where a mode cannot begin a stream at the exact position,
the response says so instead of relocating the request.

The shape, agreed with both client sessions before it was written:
`seek_ms` (the baseline, meaning unchanged), `seek_offset_ms` and
`seek_requested_ms`, with `seek_ms + seek_offset_ms == seek_requested_ms`
exactly and the offset never negative. Transcode is frame-accurate with a zero
offset; remux takes the last keyframe at or before the request and carries the
remainder as the offset; direct is the request with a zero offset. **No mode is
ever substituted** — an earlier draft had remux fall back to transcode and the
operator rejected it as the same second-guessing as moving the seek.

- [x] Backward alignment in `indexed_plan`, and both transcode paths stop
  snapping. `nearest_keyframe_at_or_after` is deleted.
- [x] The three fields through `HlsVodPlan`/`PlaybackPlan` to `session_json`,
  plus `docs/streaming.md`, `docs/configuration.md` and the client contract
  entry in `ACTIVE.md`.
- [x] Cue density logged on a successful plan as well as a rejected one:
  entries, longest gap, median gap.
- [x] The seek fast path names the precondition it declined on, and a
  non-seek-only PATCH says which part of the request made it one.

Tests: `test_a_seek_goes_where_it_was_asked_to_go` walks the invariant through
the HTTP payload for remux, transcode and direct, on both create and `PATCH`;
`test_media_vod_index_planning_rejects_partial_indexes` and
`test_reseek_hls_vod_reuses_prepared_random_access_state` pin the planner,
including the fractional-millisecond keyframe that must not attract a seek to
itself and the empty-index case that keeps the mode rather than substituting it.

## Cluster health became a capability, and Status stopped paying for diagnostics — 0.38.5 and 0.39.1, deployed 2026-09-13

Supersedes "2. Split lightweight status from expensive diagnostics" and the
operator's request for a Status role, both in `ACTIVE.md`. The API contracts
these created are listed under "What the four client sessions now depend on"
there, because clients depend on them and this ledger is not where anyone
looks first.

- [x] **`view_status` (0.38.5).** `/api/v1/status` and `/api/v1/status/*`
  carried no role, so a session the cluster had granted *nothing* — which
  0.38.4 made a legitimate state — was still shown the node roster, every
  node's capacity, and the whole diagnostics tree. The old argument for leaving
  it ungated (an importer watching an ingest needs cluster health) survives as
  an implication instead: every capability implies `view_status`, exactly as
  every capability already implied `media_viewer`. It is the weakest
  capability — implied by everything, implying nothing — so granting it alone
  makes health public without handing out media.
- [x] **Implications resolve at mint, not only at write.** `verify()` and the
  anonymous mint path expand the stored role set, so an account written before
  a role existed gains it at its next login with no migration. Without this,
  upgrading would have taken Status from every existing account until each was
  edited by hand. Verified against the live table: no stored record carries
  `view_status` and none needs to.
- [x] **`GET /api/v1/health` (0.38.5).** Unauthenticated, role-free, answers
  during recovery, and reports only whether this node is serving — no version,
  no node id, no topology, because it is reachable by anyone wherever the API
  is. It exists because things were reaching for `/api/v1/status` to answer a
  question it was never the right route for.
- [x] **The status/diagnostics split (0.39.1).** Diagnostics was 68% of the
  live payload, but the locks were the larger cost: assembling it took one in
  nearly every subsystem on the node, several held by the busy paths that make
  someone open Status. It now lives at `GET /api/v1/status/diagnostics`, and
  the light response names that route in `diagnostics_endpoint` so a client
  reading the old shape finds a pointer rather than an `undefined`.
- [x] **A latent gating bug fixed in passing.** `Service::handle_http`
  dispatched Status *before* the role gate ran, so a `required_role()` entry
  for it was unreachable. The 0.38.5 gating worked only because the dispatch
  moved with it.

## One bad pack record no longer takes a backend offline — 0.38.3, deployed 2026-09-13

Resolves the live incident this file's sibling `ACTIVE.md` opened with on
2026-09-13. Full diagnosis, the mechanism, and the two shapes recovery now
handles are in `2026-09-13-torn-pack-header-recovery-plan.md`; the release
notes are in `CHANGELOG.md` under 0.38.3.

- [x] **gbni-1's 8 TB DATA backend was offline over 125 bytes.** A power loss
  between the pack `write()` and the durability domain's `syncfs` left the file
  extended by exactly one `pack_header_size` of zeroes. Recovery truncated two
  torn-tail shapes but threw on a third — a header present but undecodable —
  so the `LocalStore` constructor failed, the pool marked the backend offline,
  and `StoragePool::limit()` reported 0 because `token_known` was never set.
  The node then advertised `capacity=0` while reporting data storage *ready*.
- [x] **Recovery settles an undecodable header instead of refusing the pack.**
  It looks for a decodable header after the bad one (magic, then the header's
  own SHA-256, one pack, 1 MiB chunks). Nothing after it → torn tail,
  truncated. Something after it → damage inside the pack: the span is skipped
  and counted as dead bytes for compaction, the records after it are indexed,
  and the loss is logged at `error` for replica repair. Neither takes the
  backend offline.
- [x] **Verified live, not just in tests.** gbni-1 on 0.38.3 logged
  `truncated undecodable pack tail path=…pack-00000000000000001206.pack
  offset=29841717 bytes=125 zero_header=1`, brought the backend online in
  97 ms, and reported `capacity=8796093022208 used=735796755266`. The node had
  been holding **685 GiB** of authoritative data all along — the backlog's
  "gbni-1 holds 0 of 1618 artwork objects" was a consequence of the backend
  being offline, not of it being empty. `zero_header=1` confirmed the ext4
  zero-fill shape that was predicted from the writer's code path.
  Status carries `diagnostics.data_store.pack_recovery_truncated_tails: 1`.
- [x] **No hand repair was performed.** The operator's instruction was that
  Macha self-heals; the pack was left untouched and the deployed code fixed it
  on start-up.
- [x] **es-1 returned on its own** after a reboot at ~12:41 the same day and
  now runs 0.38.3. Its cause is not recoverable — the node keeps no persistent
  journal (see the P0 follow-up in `ACTIVE.md`).

## Cluster users, passwords and roles — 0.38.0, deployed 2026-09-12

Supersedes the "No authorization tiers yet" P0 in `ACTIVE.md`. Design and the
seven places the implementation diverged from it are in
`2026-09-12-cluster-users-and-roles-plan.md` (see its "What actually shipped"
section — the plan itself was not edited, so it stays usable as evidence).

- [x] **Cluster-replicated users, passwords and roles.** `UserStore`
  (`src/users.hpp`), `user_sync` RPC, `/api/v1/users`, `macha-users`.
  Passwords are scrypt with per-record parameters; the table is sealed at rest
  under an HKDF subkey, because it replicates to an offsite node. Verifying a
  password reads only the local replica — no RPC, no metadata, no catalogue —
  so a node that is alone, or whose metadata has gone read-only, still
  authenticates.
- [x] **Roles are capabilities, not a ladder.** `media_viewer`, `importer`,
  `manager`, `manage_users`; every role implies `media_viewer` and nothing else
  implies anything. Resolved at mint time, gated in one place before dispatch
  (`Service::required_role`).
- [x] **root and anonymous are ordinary accounts** created once by the founding
  node, neither renameable nor deletable. Anonymous access is the anonymous
  account's roles rather than a config key. At least one account always holds
  `manage_users`, enforced in `UserStore` rather than only the API.
- [x] **Three latent session defects found and fixed on the way.** Session
  gossip had never once run since 0.24.0 (wrong frame class on send,
  undispatched on receive); `propagate_session` blocked login for up to 30 s per
  unreachable-but-active peer; periodic gossip drew from the same memory budget
  as data work and could wedge a loader (fixed by moving session/user gossip to
  `FrameType::control`).
- [x] **Gossip re-announces every 30 s.** `broadcast_best_effort()` reports
  frames queued, not delivered, so a peer whose inbound route is not usable yet
  can be marked told having received nothing. Found during this release's own
  rollout: an upgraded node came up with an empty user table and refused every
  request until the sending node happened to restart.
- [x] **`DataResourceArbiter::acquire` no longer waits for ever.** It waits in
  no-progress windows — any release resets the window — and logs the class,
  size and used/active/waiting counts when it gives up. New
  `dht.data_credit_no_progress_deadline_ms`, 120 s.
- [x] **`test_storage_data_credit_reserves` no longer depends on the host.**
  `maintenance.background_concurrency` defaults to `hardware_concurrency() / 2`,
  so its three loader acquires deadlocked for 360 s on any machine with fewer
  than six cores — every node in this cluster — while passing on a twelve-core
  development machine. The test now states the ceiling it means to test. This is
  the "passes in isolation" class `ACTIVE.md` had already flagged; the cheap
  check (core count) was the one that found it.
- [x] **`catalogue.api.advertised_endpoint` is validated at load.** The shipped
  example had always claimed a path was "rejected at startup"; nothing checked.

Deliberately not built: **no recovery key and no recovery endpoint.** One must
be presentable without an account to be useful, which means a standing
unauthenticated path to the most privileged account in the cluster, bought with
a capability that already exists behind strictly more access — anyone who could
present one has root on a node, where `macha-users passwd root` does the same
job. The machinery (an X25519 envelope sealing the cluster key, so a key can be
verified without anything derived from it being stored) is implemented and
tested but uncalled; `test_no_recovery_route_is_exposed` stops the route being
reintroduced by accident, and `macha-recover` ships saying so.

## libtorrent port mapping stated, and an unbindable advertise refused — 0.38.2

- [x] **`torrent.upnp` and `torrent.natpmp` exposed.** libtorrent maps its own
  listen port and both default on inside libtorrent, so the session created
  router mappings regardless of configuration: a node with
  `network.upnp.enabled: false` still had 6881 mapped. Defaults unchanged
  (true), so no node's behaviour shifted — the point is that it is now
  refusable and visible.
- [x] **`torrent_listen_interfaces()` refuses an address it cannot bind.**
  0.37.2 made the session bind the node's advertised address, correct while
  that was a LAN IP. Once nodes advertised public DNS names it bound *nothing*,
  silently: `listen_interfaces` takes an IP literal or a device name, never a
  hostname. It now uses the advertised address only when it parses as an IP
  literal, and otherwise binds every interface and says so.
- [x] **Port-mapping outcomes leave DEBUG.** Success once at info, failure once
  at warn with what to do about it. gbni-1 ran for hours with "no router found"
  and nothing above debug mentioned it, while its peer counts stayed low and it
  could not seed.

## es-1 publication livelock — 0.36.8 + 0.36.9, deployed 2026-09-09

- [x] **The whole of the "es-1 publication livelock starves RPC and takes the
  node out of the cluster" item from `ACTIVE.md`.** Two defects, one in each
  release, and the item's own diagnosis of *why* the parking discipline could
  not catch it turned out to be exactly right: the work never failed, so
  nothing could park it.

  **0.36.8 — reassembly starvation and an unbounded wait.** `MessageAssembler`
  could not get a retained-memory lease to reassemble an inbound frame, threw
  `process retained-memory RPC reassembly saturated` and killed the channel
  about once a second, so peer channels died 1–2 s after connecting and
  telemetry — the only consumer that never re-dials — appeared to vanish. The
  loop is closed exactly as the item described: publication holds its bytes
  until a peer confirms, the confirmation is a frame that must be reassembled
  into the same ledger, and that reassembly is refused because publication is
  waiting. Fixed with `runtime.reassembly_memory_reserve_bytes` (32 MB),
  placed below the control/viewer waiter gate and above the loader gate and
  durable-lower budget — both halves load-bearing, since above every gate it
  inverts the deadlock and above the viewer gate it breaks governing law 2.
  Separately, the publication `DataWorkContext` carried no deadline at all, so
  the wait took the unbounded `cv_.wait` branch and all eight commit workers
  sat in `ensure_buffer_memory` holding 492 MB between them.
  `fuse.publication_no_progress_deadline_ms` (30 s) makes that a bounded
  no-progress budget that enters the ordinary retry/park path.

  **0.36.9 — the hold-and-wait itself.** Every byte of `owners.publication` is
  a `WriteHandle` extent lease. A writer is retained across clean yields and
  retryable failures so a resumed publication never replays spool bytes, and
  it keeps its leases while retained; publication scheduling is otherwise
  breadth-first, so the number of writers holding partial state was simply the
  width of the backlog — 123 leases at a 4 MiB extent, the entire durable-lower
  budget. No byte budget could fix it, which is why lowering
  `publication_inflight_bytes` from 256M to 96M had changed the inflight figure
  and left `owners.publication` at 515,899,392 unchanged.
  `fuse.publication_max_open_writers` bounds how many inodes may hold a writer,
  derived from `runtime.loader_memory_reserve_bytes`.

  **Evidence.** es-1 before: 317 publications started, **0** completed,
  `owners.publication` 515,899,392 byte-identical across restarts at 99.7% of
  the durable-lower budget, spool 8,589,783,970 draining at 0 B/s, 190 backend
  failures. After the deploy the spool drained to **0** with
  `data_publication_bytes_confirmed` reading exactly 8,589,783,970 — every
  byte — in about 45 minutes. Over the following 15 hours under live ingest the
  cluster published ~475 GB with `parked_publications` 0, `waits.loader` 0 on
  every node, peak ledger use 156 MB of 768 MB, and zero warnings or errors.
  A viewer request had been failing on this node with `viewer fragment memory
  admission unavailable` before the fix, so governing law 2 was being violated
  in practice, not merely at risk.

  Two defects **in the fix itself** were found the next day and remain open in
  `ACTIVE.md`: the no-progress counter misses `rebuild_step` and the
  non-pipelined `flush()` and counts the wrong thing anyway, and the
  open-writer bound is soft and can overshoot by up to `commit_workers - 1`.
  Neither has fired. Plan and post-deploy audit in
  [`2026-09-09-publication-hold-and-wait-plan.md`](2026-09-09-publication-hold-and-wait-plan.md);
  the handover that preceded it is
  [`2026-09-09-publication-livelock-continuation.md`](2026-09-09-publication-livelock-continuation.md).

## Self-healing disciplines — 0.29.0 → 0.32.0

- [x] Executed the whole of
  [the self-healing disciplines plan](2026-09-06-self-healing-disciplines-plan.md),
  written after one afternoon of ordinary load surfaced six P0 defects, each
  hidden behind the previous, all instances of four habits: trusting
  bookkeeping over re-derivable truth, retries that turn "not yet" into
  "forever", recovery that refuses instead of resolving, and snapshots that
  carry retirement history.
- [x] 0.29.0 — durability is re-derived from disk, not asserted from a dead
  token. This closed the `object durability quorum unavailable before
  publication` wedge: three recovered inodes on gbni-1 had retried
  `required=1 durable=0` on the same object ids for the life of each process
  while 14.5 GB sat in the spool and the WAN socket idled. The barrier now
  probes the restarted peer with the object ids and re-stamps the batch, so
  a requirement naming a dead `durability_epoch` no longer outlives that
  peer's restart.
- [x] 0.30.0 — "not yet" never becomes "forever": one `RetryPolicy` per work
  item with backoff and a terminal parked outcome an operator resolves
  (`parked_publications`, `manage/parked-publications`), replacing three hot
  loops including the ~40/s spin on `FUSE namespace advanced during data
  publication` during startup recovery. The 120 s `service_startup_timeout_ms`
  elapsed-time gate — which turned a slow-but-progressing 5-minute replay into
  an infinite crash loop eight times on gbni-1 — became a no-progress gate
  (`startup_progress.hpp`, `service_startup_no_progress_ms`, absolute ceiling
  now off by default). Regression:
  `test_service_startup_gate_waits_while_recovery_progresses`.
- [x] 0.31.0 — recovery resolves rather than refusing, with a journal fuzz
  fixture (`test_fuse_journal_fuzz_every_frame_mutation_still_starts`).
- [x] 0.32.0 — DLT7: a flags byte per topology set, so a merge delta no longer
  has to resend the whole standing conflict set (~305 KB on the cluster, ~100
  merges/day × 3 replicas of pure history growth) whenever `merge_parents`
  changes. Plus canonical tombstones and conflicts compacted out of the
  snapshot.
- [x] Evidence per discipline, and the closing two-writer / rolling-restart
  run, in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md).
  Follow-ups filed there remain open and are indexed in `ACTIVE.md`.

## Retention-check batching and cheap presence — 0.26.0, 0.32.7–0.32.10

- [x] Root-caused and fixed the live incident where a bulk movies rsync to
  `corvus-es-1` froze data publication entirely and pinned the maintenance
  thread near 100% CPU for minutes: `DistributedStore::retain_data`'s serial
  per-extent `has_on` loop, each call either a full local decrypt+hash or a
  synchronous control-plane RPC, producing a hard `retention claim … control
  RPC deadline exceeded` against an unrelated peer — a direct
  governing-law-1 violation. Plan, root-cause trace and the correction the
  test suite caught mid-implementation (`rebalance_step`/`repair_step`'s
  presence checks could *not* move to the cheap path, having no downstream
  re-verification) in
  [retention-check batching and a cheap local presence check](2026-09-06-retention-check-batching-and-cheap-presence-plan.md).
- [x] This subsumed the separately-filed scaling item that `LocalStore::valid()`
  was used as a cheap presence check while doing a full read + AES-GCM decrypt
  + SHA-256.
- [x] Measured on the real three-node cluster during a live bulk import
  (0.32.7 remote claims are `has()` not re-reads; 0.32.8 parallel barrier
  fan-out; 0.32.9 presence remembered rather than `stat`'ed; 0.32.10 CONTROL
  puts together to the nearest replica): es-1 retention avg 5,217 ms → 121 ms,
  publish avg 556 ms, no `deadline exceeded` or `peer closed` since 0.32.7.
  Numbers in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md).
  The N² retention-journal growth that run also recorded stays open in
  `ACTIVE.md`.

## Correctness and security fixes — 0.24.1 → 0.24.4

- [x] **`NodeRuntime::accept_history_checkpoint_proposal()` accepted
  unconditionally — 0.24.1.** Confirmed as the live root cause of a real
  outage, not a theoretical gap: a lagging proposer whose own survey was
  already stale got an unconditional ack from every participant for a floor
  they had moved past, then durably committed and compacted *itself* to that
  stale floor, discarding the only shared ancestry two-parent reconciliation
  had — which is exactly what produced "divergent metadata heads have no known
  common ancestor" across all three nodes on 2026-09-05 (confirmed by
  cross-node journalctl timing). `MetadataReplica::record_checkpoint_ack()`
  now validates the proposal's `floor_hash` against this replica's own
  accepted head before acking, on both the RPC and local-owner paths.
  Regression:
  `test_history_checkpoint_ack_refuses_a_floor_this_replica_has_already_superseded`.
  This prevents recurrence; it did not retroactively restore ancestry already
  discarded.
- [x] **Data race on `LocalStore::last_mutation_generation_` — 0.24.3.**
  `scan()`'s write moved under `m_`, matching `durability_barrier()`'s read.
- [x] **Cleared ingest jobs could resurrect — 0.24.3.** Seven sites (one more
  than originally scoped) in `src/ingest.cpp` moved from unguarded
  `jobs_[job.id]` to `find()`-guarded access, matching what
  `should_pause_or_cancel()` already did. Regression:
  `test_cleared_ingest_job_does_not_resurrect_while_worker_finishes`.
- [x] **`MetadataReplica::accept_commit()` held the global replica mutex
  across `persist()` — 0.24.3.** Writing the entire snapshot payload
  (potentially hundreds of MB) on the single metadata RPC worker. The
  checkpoint write now happens after `m_` is released, serialized only against
  other durable-mutation writers, mirroring the off-lock-write /
  on-lock-bookkeeping pattern `import_history()` already used.
- [x] **Wedged namespace queue on a non-retryable error — escape hatch in
  0.24.3, fixed forward in 0.24.4.** `GET/POST
  /api/v1/manage/filesystem/blocked-namespace-operation[/skip]` lets an
  operator abandon a wedged operation by exact sequence number; never
  automatic. The 0.24.3 version of this fix had a journal-bookkeeping bug that
  crash-looped a node on restart — which is what put the subsystem crash
  isolation item on the backlog.
- [x] **Non-constant-time bearer token comparison — closed as a side effect of
  0.24.0.** `http.cpp` no longer holds or compares a raw bearer token;
  `SessionManager::validate()` hashes the presented token (SHA-256) before any
  lookup, so the untrusted byte comparison no longer exists.

## Breaking config change: `mount_path` moved to `fuse.mount_path` — 0.25.0

- [x] Top-level `mount_path` became a hard startup error ("obsolete
  configuration key: mount_path (moved to fuse.mount_path)") rather than
  silently ignored. Every node's `macha.yaml` has since moved its line under
  `fuse:` — all three cluster nodes run 0.34.0.

## gbni-1 SSH unresponsiveness during builds — diagnosed, not a code defect

- [x] "Node 50 loses SSH responsiveness with Macha active during a build" was
  diagnosed as gbni-1 browning out under load (power/undervolt), not a Macha
  scheduling or memory defect. It is now a standing operational rule rather
  than an open investigation: never run `cmake --build` on gbni-1 — build on
  gbni-2 and ship the staged `make install DESTDIR=` tree — and rate-cap its
  rsync (`--bwlimit`, `nice -n 10 ionice -c3`).

## Cluster session/auth subsystem, `/api/v1/session` — 0.24.0

- [x] Shipped `POST/GET/DELETE /api/v1/session`: anonymous-only bearer-token
  session creation, introspection and revocation. Every non-exempt API route
  now requires `Authorization: Bearer <token>` — this closes the
  previously-logged "auth optional when `token_file` unset" P0 security gap
  as a side effect of replacing the mechanism entirely.
- [x] Sessions are cluster-replicated (push-on-mutation plus periodic gossip
  backstop, modelled on `TelemetryStore`) and persisted locally, with each
  node's own in-memory replica doubling as its O(1) lookup cache — no
  separate cache layer, so a pushed mutation is the invalidation.
- [x] Retired `Macha-Viewer-Session`/`X-Macha-Viewer-Session`/
  `viewer_session_id` in favour of the authenticated session id for playback
  logical-viewer correlation, and moved playback's `Idempotency-Key` from a
  request header to an `idempotency_key` query parameter.
- [x] Built extension seams for later work, not yet used: `CredentialValidator`
  (real, non-anonymous credentials) and `session_has_role()` (per-route/action
  role gating — still not wired into any route).
- [x] Fixed during joint live testing: `create()` didn't enforce
  `max_sessions` (only the gossip `apply()` path did) — a client-side timer
  bug caused a real re-mint storm against a live dev node. `create()` now
  returns `nullopt`/`429 too_many_sessions` at capacity; regression test
  `test_session_create_enforces_max_sessions` added.
- [x] Coordinated the client-side contract with the Macha UI Work session in
  parallel; verified end-to-end against a live running node (create → gated
  route with/without token → introspect → revoke → post-revoke rejection).
- [x] Closed the separately-logged "non-constant-time bearer token comparison"
  P0 security item as a side effect of this rework: `http.cpp` no longer holds
  or compares a raw bearer token at all (the old `bearer_token_`/`read_token()`
  plain `!=` compare was removed entirely). `SessionManager::validate()`
  (`src/session.cpp`) hashes the presented token with SHA-256 before any
  lookup and does a `std::map` lookup on the digest, matching the plan's
  original design reasoning — confirmed by code audit on 2026-09-05, no fix
  needed.

Evidence: `CHANGELOG.md` 0.24.0.

2026-09-05: backfilled the eight releases (0.23.4–0.23.11) that shipped after
this ledger's previous update, found stale during a full backlog
reprioritisation pass. Full detail for each remains in `CHANGELOG.md`; entries
below are intentionally brief pointers rather than re-narrated detail.

## Snap transcode seeks to source keyframes — 0.23.11

- [x] Fixed a seek deep into a transcoded title costing ~7s to first fragment
  by snapping the transcode seek to the nearest source keyframe
  (`media_vod::nearest_keyframe_at_or_after`), without reusing remux's
  whole-file density-checked `indexed_plan`, which was observed to silently
  reject the snap on long files with any sparser GOP elsewhere.
- [x] Fixed a rounding bug (`llround` → `ceil` on the keyframe timestamp) that
  was making the container seek land one keyframe early, discarding a full
  extra GOP for nothing. Verified live: ~7s → ~4s. A residual ~1.7s gap vs.
  position-zero remains unexplained and is intentionally left open — tracked
  under playback P0 item 1 above via the `media playback pipeline seek timing`
  diagnostic added to isolate it.

Evidence: `CHANGELOG.md` 0.23.11.

## Fix seek/generation-replacement stall on stale segment requests — 0.23.10

- [x] Fixed a segment request against a superseded playback generation
  blocking for the full 15s `streaming.startup_timeout_ms` before returning
  404, via a reversible `MediaSegmentStore::mark_superseded(bool)` signal
  distinct from the irreversible `cancel()`.
- [x] Not confirmed as the cause of a separately reported ~1-2s live A/V
  offset symptom — that investigation continued into the audio-drift work
  above.

Evidence: `CHANGELOG.md` 0.23.10.

## Fix audible pitch shift from drift correction, and the drift fix itself — 0.23.8/0.23.9

- [x] 0.23.8 shipped bounded audio drift compensation for transcoded playback
  (`audio_next_pts` re-anchoring), measured live at ~0.4ms drift/second before
  the fix.
- [x] 0.23.9 replaced 0.23.8's `swr_set_compensation()` mechanism (which
  worked but caused an audible pitch shift, since nudging the resample ratio
  changes pitch) with libswresample's own `async=1` + `swr_next_pts()`
  correction, which injects/drops samples rather than changing the ratio.
  Verified: healthy audio over 8 minutes, drift within ±15ms.
- [x] Known gap carried forward: no automated regression harness yet for the
  real (non-stub) transcode audio path — tracked under playback P0 item 1
  above.

Evidence: `CHANGELOG.md` 0.23.8, 0.23.9.

## Per-node advertised API address for any-node Direct Play failover — 0.23.7

- [x] `/api/v1/status` now reports `api_host`/`api_port` per node, distinct
  from the internal RPC bind address, fixing failover guessing the wrong port.
- [x] Added optional `catalogue.api.advertised_host`/`advertised_port` for
  NAT/port-forwarding. Static config-only for now — UPnP/external-IP probing
  of this endpoint remains open under P1 above.

Evidence: `CHANGELOG.md` 0.23.7.

## Safe distributed checkpoint and metadata-history compaction — 0.23.6

- [x] `MetadataReplica::compact_history_if_safe()` — an existing, already-tested
  primitive with zero production callers — is now actually invoked, via a
  leaderless propose → durable-ack → commit → prune round gated on every
  durably-known participant acknowledging the same accepted-head hash
  (mirroring the existing `all_known_reachable()` gate used for destructive
  object GC). Unbounded `history.log` growth (the direct cause of a prior
  37MB → 20.9GB blowup) is fixed; a returning node whose floor was pruned
  elsewhere now converges automatically.
- [x] No client-visible behaviour change. Confirmed directly against
  `src/metadata.cpp` during the 2026-09-05 code audit as real and complete,
  not just a changelog claim — see the still-open byte-bounded-cache/shedding
  follow-up under the structural-safety P0 tier above.

Evidence: `CHANGELOG.md` 0.23.6.

## Cluster-wide ingest/torrent job visibility and control — 0.23.5

- [x] `GET /api/v1/ingest/jobs` and `/api/v1/torrents/jobs` now answer with
  every job in the cluster (on-demand RPC survey, not replication), each
  tagged with its owning `node_id`; an unreachable peer is skipped rather than
  failing the whole request.
- [x] `POST .../jobs/{id}/{pause,resume,retry,cancel,clear}` forwards to the
  owning node when the receiving node doesn't have the job locally, preserving
  404-vs-409 semantics cluster-wide.

Evidence: `CHANGELOG.md` 0.23.5.

## Signed artwork capability URLs — 0.23.4

- [x] Catalogue responses embed a signed, short-lived capability URL
  (`?exp=&sig=`, HMAC'd with the cluster auth key) on each artwork reference,
  letting a client use a plain `<img src>` without a bearer header. Default
  TTL 24h. Note: the URL is not yet actually cache-friendly — see the P2
  catalogue item above about `exp`/`sig` needing to be quantized so repeated
  fetches produce an identical URL.

Evidence: `CHANGELOG.md` 0.23.4.

## Remove per-request connection setup from Status and streaming — 0.23.3

- [x] `http.cpp` negotiates HTTP/1.1 keep-alive by default, with bounded
  `keep_alive_max_requests`/`keep_alive_idle_timeout`, falling back to
  `Connection: close` only on explicit request, protocol/version mismatch, or
  backlog pressure. Verified directly against current `src/http.cpp`.
- [x] Also shipped in 0.23.3: truthful per-node `phase` (fixing the rolling-
  deployment incident where recovering nodes reported fabricated-looking
  healthy zeros) and serialized foreground metadata-head reconciliation
  (fixing a live 37MB → 20.9GB metadata-history blowup in under a week). The
  *aggregation* half of Status truthfulness was later found still open — see
  the P1 item above.

Evidence: `CHANGELOG.md` 0.23.3.

## Bounded terminal FUSE recovery failure

- [x] Reproduced GBNI-1's crash-recovery state in which durable spooled writes
  target a path absent from the accepted namespace.
- [x] Prevented a terminally poisoned inode from being re-admitted indefinitely,
  while preserving its journal/spool and leaving retryable failure semantics
  unchanged.
- [x] Passed 65/65 FUSE tests, 277/277 complete core tests and 4/4 runtime tests.
- [x] Deployed complete source as 0.23.1 to all four nodes. The three Linux
  binaries are byte-identical. The exact live inode-922 failure occurred once,
  stayed settled, and left the mount and local API responsive.

Evidence: [terminal FUSE recovery loop](2026-09-03-fuse-terminal-recovery-loop.md)

## Persistent logical-viewer transcode admission

- [x] Replaced physical-running-pipeline admission accounting with one retained
  video/audio entitlement per persistent logical player/UI session.
- [x] Added `Macha-Viewer-Session` reconciliation for replacement POSTs while
  preserving PATCH, generation, idempotency and ownerless ephemeral-session
  semantics.
- [x] Serialized child-pipeline handover, retained entitlement through idle
  reclaim and Direct/Remux transitions, and released it only at DELETE/expiry.
- [x] Added separate physical-pipeline diagnostics and deterministic coverage
  for replacement POSTs, repeated seeks, every playback mode, contention,
  reclamation and exact capacity release.
- [x] Passed 24/24 playback tests, 276/276 complete core tests and 4/4 runtime
  dependency tests; deployed complete source to all four nodes with identical
  Linux binaries and a healthy, writable generation-3904 clean baseline.

Evidence: [logical viewer transcode admission](2026-09-03-logical-viewer-transcode-admission.md)

## Phase 2 bounded FUSE operation metadata

- [x] Added a configurable aggregate heap admission bound for durable `DataOp`
  history, checksum-vector capacity and a concurrent publication snapshot.
- [x] Made saturation wake publication and wait on durability/retirement events;
  acknowledged work remains durable and recovery-safe.
- [x] Added current/peak/limit/wait diagnostics and hard-bound plus liveness
  regressions. The complete filesystem/FUSE suite passed 64/64.

Evidence: [Phase 2 operation-metadata bound](2026-09-02-phase-2-operation-metadata-bound.md)

This is the completed-work ledger for the current session. An item belongs here only after implementation and its stated verification are complete. Detailed design notes, exact test results, and UAT measurements remain in the linked records.

## Hydration shutdown ownership and first retained-owner diagnostics

- [x] Reproduced the live late-submit race which threw `hydration executor is
  stopping` through a scheduler `std::jthread` and aborted GBNI-1 during stop.
- [x] Made late submission a normal rejected result, completed executor-owned
  queued promises before join, and added explicit cancellation/rejection
  accounting plus a deterministic blocked-provider shutdown regression.
- [x] Added O(1) current retained-owner counters for FUSE operations, checksums,
  overlay ranges, publication snapshots, durability tickets and publication
  pipeline bytes; clean completed inodes release empty operation-vector
  capacity.
- [x] Added active playback fragment resident, spill, descriptor and VOD-plan
  ownership counters.
- [x] Passed 62/62 focused FUSE tests, a clean 260/260 parallel core run, and
  3/3 runtime tests. Loaded stable-RSS acceptance remains active.

Evidence: [shutdown and retained-owner checkpoint](2026-09-02-shutdown-and-retained-owner-checkpoint.md)

## Asynchronous identity reset and retired-node status — 0.22.2

- [x] Captured the deployed failure: resetting obsolete ES-1 identity `85ff…`
  applied its tombstone immediately but held the HTTP response for 34.7 seconds
  while synchronously publishing a 5.77 MB metadata snapshot.
- [x] Moved peer propagation and metadata auditing to a coalescing background
  worker. Reset now returns `202 Accepted` after only its small locally durable
  operational tombstone, with `audit_state: "queued"`.
- [x] Excluded retired identities from ordinary status rows, offline health,
  replica counts and capacity totals. Per-node detail preserves the durable
  record with `state: "retired"` and reset audit data.
- [x] Added a regression which holds the metadata mutation owner while reset
  admission completes in under 500 ms, plus reset propagation/unavailable-state
  and retired-status coverage. All 252 core tests and all 3 runtime tests pass.
- [x] Deployed to all four nodes. Live reset returned `202` in 2.568 ms and its
  queued metadata audit completed 312 ms later at generation 2234. Both GBNI
  observers omitted obsolete identity `85ff…` from normal cluster status. After
  recovery from a separate ES-1 disk `EIO`, all four current identities reported
  online, healthy and writable at generation 2234.

Evidence: [identity association retirement](2026-09-01-identity-association-retirement.md)

## Exact metadata reconciliation recovery — 0.22.2

- [x] Reproduced and corrected compact-delta selection for reconciliation which
  reorders an append-ordered garbage/tombstone set. Unrepresentable ordering now
  selects a full immutable record instead of emitting a byte-inexact delta.
- [x] Added one bounded local full-record fallback after compact-body rejection,
  matching remote-replica safety without retrying or loading full history.
- [x] Added regressions for both ordering rejection and local fallback; all
  250/250 core and 3/3 runtime tests passed.
- [x] Deployed complete source as 0.22.2 to all four nodes. The three identical
  Linux builds have SHA-256
  `1fc6c8473a64525da48d6fead630780615f525f22ae136dd2ff9bdf5731144d0`.
- [x] Live generation-2128 branches reconciled without reset or library loss.
  Metadata became writable/validated, ordinary queued publication advanced the
  cluster through generation 2155, and every catalogue API returned the same
  331 items and 374 artwork objects. Linux services remained active with zero
  restarts.

Evidence: [metadata reconciliation recovery](2026-09-01-metadata-reconciliation-recovery.md)

## Metadata-history bounded-memory and delta-reconciliation checkpoint

- [x] Replaced retained decrypted history payloads with a compact authenticated
  on-disk frame index and on-demand reconstruction.
- [x] Added a configurable `128M` default materialisation byte budget, retained
  the secondary 64-entry guard, and exposed history/cache byte diagnostics.
- [x] Made deterministic merges store DLT6 deltas when smaller, including
  merge-parent/conflict replacement, while preserving the immutable full record
  hash and safely supplying missing parents before full fallback.
- [x] Corrected `runtime.rss_bytes` to report current RSS on Linux/macOS and
  moved stale-Macha-mount recovery before mount-path filesystem access.
- [x] Added disk-backed large-history and four-node merge-body regressions.
  The authoritative serial suite passed 246/246 and runtime dependencies passed
  3/3. Live bounded-RSS/restart UAT remains active before loaded ingest resumes.

Evidence: [P0 metadata-history remediation](2026-09-01-metadata-history-memory-remediation.md)

## Bounded shutdown and clean cluster rejoin

- [x] Reproduced the deployed shutdown timeout and captured both maintenance
  owners that could remain blocked after stop: a synchronous outbound control
  RPC and pack compaction waiting for startup storage accounting.
- [x] Made shutdown close pending outbound routes before joining service
  maintenance, permanently reject new outbound calls once cancellation begins,
  and make the accounting wait stop-token-aware.
- [x] Correctly classified libfuse's positive signal return as an intentional
  shutdown rather than a frontend failure, while retaining negative errno and
  watchdog mount loss as fail-closed errors.
- [x] Added regressions for pending RPC cancellation, post-cancellation retry
  rejection, accounting-wait cancellation, and FUSE exit-result semantics.
- [x] Passed the final 245/245 core and 3/3 runtime suites. Live UAT on
  node 50 stopped Macha in 215 ms with systemd `Result=success`, then restarted
  it as the same node at generation 1643; node 51 independently reported it
  online at the same writable generation.

Evidence: [bounded shutdown and rejoin record](2026-09-01-bounded-shutdown-and-rejoin.md)

## Dedicated immutable media-information engine

- [x] Added an event-driven, durably hinted profiling service used by ingest,
  cataloguing, the profile API, and playback; it performs no idle polling.
- [x] Ordered and deduplicated optional scans while keeping every background
  media read in the speculative class below loader work.
- [x] Removed the client-visible playback profiling gate. A stored immutable
  profile feeds the unchanged negotiation path with zero probe reads; a genuine
  miss continues normal viewer-priority media-engine negotiation.
- [x] Coalesced concurrent scans by immutable media ID. Viewer-required
  negotiation cancels/takes over speculative ownership, and successful results
  are asynchronously persisted regardless of which path produced them.
- [x] Pruned profiles only when their immutable media ID has no live namespace
  copy, retaining one shared profile across identical aliases/copies.
- [x] Added separated profile-lookup, fallback-inspection, selection and total
  admission timing diagnostics and corrected the streaming/catalogue docs.
- [x] Passed all 242 core tests and all 3 runtime/libav tests, then built and
  deployed the source on all four nodes. Each node reported ready, writable and
  online at metadata generation 1633.

Evidence: [media-information checkpoint](2026-09-01-media-information-engine.md)

## Manual causal metadata repair and ancestry safety

- [x] Proved all three replicas held accepted generations 1552 and 1516 but no
  retained common ancestor because local history compaction had discarded the
  required bridge.
- [x] Proved generation 1552 strictly causally dominated every mutation in 1516,
  retained the same catalogue root, and contained all 622 older namespace
  entries plus 12 additions.
- [x] Added a dry-run/two-phase offline repair tool which refuses automatic,
  mergeable, concurrent, equal-clock, unstaged or under-witnessed repairs.
- [x] Backed up all three metadata replicas, staged one identical generation
  1553 two-parent record everywhere, then accepted it with nodes 50 and 51 as
  actual durable witnesses. No branch or object was discarded.
- [x] Disabled unsafe automatic history compaction until exact cluster-wide
  accepted-head identity can be proven durably.
- [x] Passed 29/29 storage/metadata tests, 234/234 core tests and 3/3 runtime
  tests. Live UAT converged all nodes, replayed queued operations, and remained
  healthy/writable overnight at generation 1569; later ordinary divergent
  branches reconciled automatically with zero conflicts.

Evidence: [manual causal repair record](2026-09-01-metadata-manual-causal-repair.md)

## Phase 1D.4 partial: transient publication cursor preservation

- [x] Retained the provisional writer, WAL cursor and resumable
  materialisation/rebuild state across retryable backend failures.
- [x] Kept failed pipelined extent payloads at their exact manifest offset and
  retried them in place, preventing both whole-generation replay and manifest
  holes.
- [x] Added a deterministic one-shot failure regression proving one publication
  start, exact one-pass spool reads, atomic visibility and exact final content.
- [x] Preserved the existing pipeline-failure invisibility contract.
- [x] Passed focused tests, filesystem/FUSE 58/58, the complete suite 229/229
  and runtime dependencies 3/3.

Evidence: [transient-failure cursor checkpoint](2026-08-31-fuse-publication-phase-1d-transient-failure-cursor.md)

## Phase 1D.4 partial: pressure-aware retirement selection

- [x] Ranked closed generations above the pressure threshold by releasable
  spool bytes per remaining replay work, with nearest completion as tie-break.
- [x] Preserved ordinary below-pressure FIFO, open-loader fallback, bounded
  quanta, viewer priority and event-driven scheduling.
- [x] Added `data_retirement_priority_selections`, counted only when a worker
  actually reorders closed work.
- [x] Proved a later small closed generation retires ahead of an earlier large
  generation and wakes a blocked writer in the pathological/full-spool test.
- [x] Passed the focused regression, filesystem/FUSE 57/57, runtime 3/3 and the
  final controlled complete suite 228/228.

Evidence: [retirement-selection checkpoint](2026-08-31-fuse-publication-phase-1d-retirement-selection.md)

## Catalogue coalesced-final-state regression correction

- [x] Retained failure-only diagnostics and proved the timed-out cache already
  held the exact final title and artwork at reconciled generation 18 while the
  test incorrectly required equality with writer-side generation 15.
- [x] Accepted newer generations only with exact final content and captured the
  integration run bound at catalogue-repair entry, before zero-grace artwork GC
  can add unrelated metadata events.
- [x] Retained the 10-second timeout, exact final search/artwork/GC assertions,
  and the separate exact one-follow-up ConvergenceDemand state-machine test.
- [x] Passed 10/10 isolated repetitions and the controlled complete suite
  228/228.

Evidence: [retirement-selection checkpoint verification](2026-08-31-fuse-publication-phase-1d-retirement-selection.md#verification)

## Phase 1D.4 partial: publication notification coalescing

- [x] Made one false-to-true spool-drain transition own the all-inode pressure
  sweep instead of rescanning for every blocked FUSE write.
- [x] Kept new durable work event-driven through durability-batch notification;
  no polling or idle owner was introduced.
- [x] Coalesced publication demand at monotonic data/namespace watermarks and
  suppressed unchanged flush/release/pressure notifications before queue work.
- [x] Added a single per-inode enqueue owner across the inode-to-queue lock
  handoff, preventing concurrent duplicate queue ownership.
- [x] Added Status counters for suppressed notifications and pressure sweeps.
- [x] Proved 1,002 notifications become two effective requests and 1,000
  suppressed duplicates; all three pressure regressions prove one sweep per
  episode.
- [x] Passed build, filesystem/FUSE 56/56, controlled complete suite 227/227 and
  runtime 3/3.

Evidence: [publication notification checkpoint](2026-08-31-fuse-publication-phase-1d-notification-coalescing.md)

## Phase 1D.2 partial: sparse changed-range reconstruction

- [x] Replaced canonical whole-file materialisation with a sparse overlay of
  sorted, merged changed byte ranges.
- [x] Reused untouched immutable extents without fetching or hashing them and
  reconstructed only touched/boundary extents under resumable loader quanta.
- [x] Preserved append-then-overwrite data, truncate/re-extension zero fill,
  exact final bytes and old-generation visibility until atomic commit.
- [x] Added completed-publication cohort counters for spool/source reads,
  immutable extent reuse and extent puts.
- [x] Added an eight-extent deterministic regression proving two touched extent
  reads, six untouched reuses, two puts and no whole-file materialisation.
- [x] Passed build, filesystem/FUSE 55/55, controlled complete suite 226/226 and
  runtime 3/3. The default 12-worker complete run is accurately retained as
  225/226 due to the resource-sensitive catalogue burst test; it passed alone
  and in the complete four-worker run.

Evidence: [sparse changed-range checkpoint](2026-08-31-fuse-publication-phase-1d-sparse-changed-ranges.md)

## Phase 1D.3 partial: node-wide DATA resource headroom

- [x] Added event-driven byte-bounded admission below RPC scheduling for
  blocking DATA object, local-store and cache work.
- [x] Reserved configurable non-borrowable foreground/read-ahead capacity while
  retaining work-conserving lower-class use of all non-reserved capacity.
- [x] Kept CONTROL completely outside DATA admission and ordered waiting loader
  work ahead of speculative work.
- [x] Covered incoming/outgoing transfer, direct repair placement, ordinary
  local reads and asynchronous fetched-object persistence; impossible
  oversized class work fails instead of waiting forever.
- [x] Added Status counters, configuration validation/documentation, isolated
  saturation/deadline/fairness tests and a real RPC/local-store saturation test.
- [x] Passed the clean complete default suite 225/225 and runtime dependencies
  3/3. The first eight-slot run was accurately retained as 223/225; both
  load-sensitive failures passed isolated and in the clean four-slot rerun.

Evidence: [DATA resource headroom checkpoint](2026-08-31-phase-1d-data-resource-headroom.md)

Deployment/UAT evidence: [three-node loaded UAT](2026-08-31-phase-1d-data-resource-headroom-uat.md)

## Aggregate spool retirement-rate estimator

- [x] Replaced the per-generation throughput EMA with cumulative physically
  retired spool bytes over one shared wall-clock epoch, so concurrent publisher
  completions contribute their aggregate capacity.
- [x] Kept provisional progress and failed reservation rollback out of the
  capacity signal and reset the internal epoch below the 50% burst threshold.
- [x] Preserved event-driven admission, hard capacity/free-space bounds,
  occupancy hysteresis and viewer priority without adding a loop or idle worker.
- [x] Exposed the rate-window numerator and elapsed milliseconds for operational
  audit and added deterministic aggregate-rate plus integrated retirement tests.
- [x] Passed filesystem/FUSE 50/50, foundations 15/15, runtime 3/3 and complete
  default 218/218 suites, including both catalogue regressions.

Evidence: [aggregate retirement-rate checkpoint](2026-08-31-spool-aggregate-retirement-rate.md)

## FUSE publication Phase 4A: bounded within-file extent pipeline

- [x] Added a configurable byte-bounded provisional extent pipeline for durable
  spool publication while leaving ordinary writes synchronous.
- [x] Preserved whole-file atomic visibility and the final aggregate durability
  barrier by merging private extent durability evidence in order.
- [x] Drained admitted work before every fairness/viewer yield so the pipeline
  is the hard bound on already-running loader I/O.
- [x] Added deterministic bound, atomicity and failure-invisibility tests plus
  operational pipeline diagnostics.
- [x] Passed build, new tests 2/2, filesystem/FUSE 50/50 and runtime 3/3. The
  complete run was accurately recorded as 216/217 because the separately
  tracked concurrent catalogue final-state test failed; it passed immediately
  in isolation.
- [x] Passed three-node UAT: 19.5 MiB/s over the complete observation window,
  40.2 MiB/s in a clean loaded interval, pipeline peak exactly two, viewer read
  gating exact, a 4 MiB direct read in 0.37 seconds, protected communications,
  and zero failures/timeouts.

Evidence: [Phase 4A implementation](2026-08-31-fuse-publication-phase-4a-extent-pipeline.md)
and [Phase 4A UAT](2026-08-31-fuse-publication-phase-4a-uat.md)

## FUSE publication Phase 0/1: diagnostics, loader priority and fair quanta

- [x] Added useful-byte, coalescing, publication, quantum/yield, admitted-byte,
  spool-pressure and RPC class/timing diagnostics.
- [x] Separated user-requested loader traffic from crash-recovery provenance and
  placed it below viewer/read-ahead work but above speculative maintenance.
- [x] Removed the false single-publisher cap for open loaders and stopped using
  the legacy recovery-worker value as a cap on restarted user ingest.
- [x] Added extent-aligned resumable publication quanta, a global admitted-byte
  budget, one retained cursor per inode, fair tail requeueing and atomic final
  metadata visibility.
- [x] Added bounded mid-generation yield/resumption machinery and its original
  FUSE-read signalling test. The later aggregate-rate UAT established that the
  FUSE classification and exclusive gate policy were wrong; Phase 1C now
  supersedes that policy while retaining the bounded-yield mechanism.
- [x] Added deterministic coverage for loader RPC ordering, recovery provenance,
  concurrent/open/closed loaders, coalescing, byte bounds, fair small-file
  progress, exact useful bytes, mid-generation viewer pre-emption, resumption,
  crash replay and confirmation.
- [x] Passed the final local build, `filesystem_fuse` 48/48, runtime 3/3 and
  complete 215/215 suites.
- [x] Passed the Phase 0/1 bounded-quantum three-node UAT: two live loader
  generations used fair bounded quanta with no amplification; loader/control
  queue waits remained sub-millisecond; a FUSE read demonstrated the old gate
  and clean resumption; and failures/timeouts remained zero. That FUSE leg is
  not accepted as viewer UAT evidence after the Phase 1C classification decision.

Evidence: [Phase 0/1 checkpoint](2026-08-31-fuse-publication-phase-0-1-checkpoint.md),
[Phase 1A loader priority](2026-08-31-fuse-publication-phase-1a-loader-priority.md),
[Phase 1B fair quanta](2026-08-31-fuse-publication-phase-1b-fair-quanta.md), and
[Phase 1A/1B UAT](2026-08-31-fuse-publication-phase-1ab-uat.md)

## Phase 0: diagnostic foundation

- [x] Added namespace admission, recovery, attempt, publication, confirmation, journal-group, record, and durability-barrier diagnostics.
- [x] Added metadata historical-request, reconstruction, and applied-delta diagnostics.
- [x] Added namespace batch/operation and convergence event/run diagnostics without introducing polling or high-cardinality logging.
- [x] Added characterization tests for one-publication-per-recovered-operation baseline behaviour and repeated linear-chain reconstruction.
- [x] Verified the Phase 0 implementation with its focused suites and recorded the continuation boundary.

Evidence: [Phase 0 diagnostic foundation](2026-08-30-phase-0-diagnostic-foundation.md)

## Phase 1: bounded shared metadata materialization

- [x] Added a bounded cache for immutable validated records and decoded snapshots, keyed by commit hash and seeded from materialized heads.
- [x] Reconstructed uncached chains from their nearest cached ancestor and retained validated intermediates.
- [x] Reused shared immutable materializations across policy validation, pruning/ancestry, committed and snapshot views, repair, catalogue, and retention consumers.
- [x] Bounded the initial cache to 64 entries while protecting current, committed, and accepted heads.
- [x] Added coverage for a 200-entry linear chain, zero replay on repeat lookup, eviction/reconstruction identity, branches, reconciliation, policy changes, and accepted-head pruning.
- [x] Ran the complete default/runtime suites for this slice, including the catalogue search/artwork/GC regression.
- [x] Performed a three-node UAT: the cluster converged, remained responsive, and returned to idle without sustained metadata work.

Evidence: [bounded record materialization](2026-08-30-phase-1-bounded-record-materialization.md), [shared decoded materialization](2026-08-30-phase-1-shared-decoded-materialization.md), and [Phase 1 three-node UAT](2026-08-30-phase-1-three-node-uat.md)

## Phase 1: materialization follow-up closeout

- [x] Audited every remaining intermediate encode and confirmed it is required
  to validate the current full-snapshot successor hash, including exact legacy
  delta-version encodings; removing it requires the deferred identity redesign.
- [x] Added a corrupt-delta regression proving a cached parent cannot validate
  a false successor, rejection cannot poison the cache, and a corrupt duplicate
  cannot displace a valid cached target.
- [x] Completed the merge, same-generation sibling, recovery authority, policy
  transition, corruption, and eviction audit against explicit deterministic
  tests.
- [x] Passed the new regression, `storage_metadata` 28/28, and the live
  same-generation sibling reconciliation test.
- [x] Passed the expanded complete default suite 205/205 and runtime
  dependencies 3/3, including both catalogue regressions.

Evidence: [Phase 1 materialization closeout](2026-08-30-phase-1-materialization-closeout.md)

## Phase 2: bounded namespace publication and durability

- [x] Replaced one-operation publication with bounded ordered, event-driven batch draining governed by configurable operation-count and encoded-byte limits.
- [x] Added ordered batch application to one mutable snapshot and metadata delta, including largest-valid-prefix failure handling and idempotent/no-op recovery.
- [x] Grouped `published` and `done` journal markers into one durability barrier per group while preserving the existing crash-compatible journal format.
- [x] Batched confirmation-safe operation families, retained unsafe mixed operations and rename as boundaries, and preserved sequence order and journal epochs.
- [x] Avoided per-operation full-snapshot work, replaced `rmdir` full-map scans with descendant lookup, and retained garbage-grace semantics.
- [x] Proved 1,000 recovered unlinks publish in four batches at a 256-operation limit rather than 1,000 generations.
- [x] Added deterministic coverage for count/byte boundaries, oversized progress, unlink-plus-parent-rmdir, idempotence, partial idempotence, semantic failure prefixes, crash prefixes, and concurrent live admission during recovery publication.
- [x] Verified partial grouped `published` and `done` crash images recover without loss, duplication, or unnecessary publication.
- [x] Ran the complete suite after durability closeout: default 194/194 and runtime-dependency 3/3, including the explicit catalogue regression.
- [x] Performed a three-node deletion UAT: 1,001 deletion operations used five publications, completed in 0.77 seconds, kept status responsive, and returned all nodes to idle.

Evidence: [Phase 2 namespace batching](2026-08-30-phase-2-namespace-batching.md), [durability closeout](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md), and [Phase 2 three-node deletion UAT](2026-08-30-phase-2-three-node-deletion-uat.md)

## Phase 3: event-driven convergence foundation

- [x] Added `ConvergenceDemand`, using a semantic epoch, latest-generation diagnostic high-water mark, and edge-triggered scheduled state.
- [x] Coalesced event bursts so events arriving during a run schedule at most one follow-up pass over the newest state.
- [x] Kept metadata/topology signals distinct from storage-only maintenance and preserved same-generation sibling/topology events.
- [x] Deferred catalogue convergence when newer metadata arrives during repair so it does not consume an obsolete intermediate view.
- [x] Added scheduler-level burst coverage proving hundreds of notices produce two runs and process the final high-water state.
- [x] Added an integrated service-level gated burst around real metadata repair. A peer publishes exactly 33 generations while the follower's repair owner is held after claiming its run; the held burst schedules no extra runs, release produces exactly one latest-state follow-up, and the follower installs the final generation and namespace.
- [x] Corrected real same-generation sibling signaling. Explicit metadata notices at the current numeric generation now advance the semantic epoch and wake convergence because they represent an accepted-head topology change; stale lower-generation notices and unchanged membership heartbeats remain non-events.
- [x] Added a two-service gated sibling test that creates distinct accepted children of one parent at the same generation, proves the remote notice advances demand without a generation increase, and verifies automatic reconciliation preserves both namespace changes in one accepted descendant.
- [x] Added an integrated catalogue burst test. While follower metadata repair is gated, a peer publishes intermediate title and poster replacements; catalogue repair performs no intermediate run, then runs exactly once against the final metadata state, exposes only the final search result and artwork bytes, and reclaims every superseded artwork object.
- [x] Corrected event-driven garbage-grace scheduling. After evaluating an immature final tombstone set, maintenance now arms one steady-clock wake at the earliest recorded wall-clock retirement expiry instead of becoming indefinitely quiescent or introducing a polling cadence.
- [x] Added a gated coalesced-delete test proving bytes survive immediately before their 750 ms grace deadline, are reclaimed after it without an unrelated event, tombstones are removed, cleanup convergence remains bounded, and the scheduler returns to a parked state.
- [x] Removed the redundant unconditional metadata announcement at the end of local publication. `NodeRuntime::accept_metadata_commit` is now the single owner of accepted-head change detection and notification.
- [x] Added a monotonic announcement diagnostic and proved 33 real publications emit exactly 33 announcements, while re-installing identical acceptance evidence emits neither a local event nor a remote rebroadcast.
- [x] Added a persisted three-replica catch-up test. A third voter stops at a shared base, two surviving voters publish 64 linear generations at W=2, and the restarted voter imports the required history, accepts the sole newest head, exposes the final namespace, and settles in at most four service convergence runs rather than one per ancestor.
- [x] Verified disconnected maintenance sleeps until a peer event and retained the catalogue search/artwork/GC regression.
- [x] Performed a three-node idle UAT at generation 1375. All three nodes remained healthy/writable with stable RPC connections, all convergence-related workers were parked, and post-torrent-pause instantaneous process CPU was 0.0% on every node.
- [x] Isolated the local pre-pause CPU use to libtorrent peer/UTP work rather than metadata, catalogue, hydration, FUSE, HTTP, or RPC work.
- [x] Performed a three-node active-burst UAT using 81 paced creates followed by 81 paced deletes. Each half used 20 metadata publications, all three nodes agreed on both the 80-entry intermediate namespace and the final empty state, status remained responsive during active work, RPC connections did not churn, and every node returned immediately to the previously proven idle state.

Evidence: [Phase 3 scheduler checkpoint](2026-08-30-phase-2-durability-and-phase-3-scheduler-checkpoint.md), [Phase 3 three-node idle UAT](2026-08-30-phase-3-three-node-idle-uat.md), and [Phase 3 three-node active-burst UAT](2026-08-30-phase-3-three-node-active-burst-uat.md)

Integrated burst verification: `rpc_cluster/test_service_metadata_repair_coalesces_real_generation_burst` passed six isolated executions; the complete `rpc_cluster` suite passed 28/28, `invariants` passed 36/36, `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` passed, and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed.

Same-generation sibling verification: `rpc_cluster/test_service_same_generation_sibling_notice_triggers_reconciliation` first reproduced the missing event, then passed eight consecutive isolated executions after the fix. The burst test passed another eight consecutive executions; the complete `rpc_cluster` suite passed 29/29, `invariants` passed 36/36, and the explicit inactivity and catalogue regressions passed.

Catalogue burst verification: `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` passed six isolated executions; the complete `hydration_catalogue` suite passed 23/23, and the real repair burst, same-generation sibling, and scheduler primitive regressions all passed explicitly.

Retention/grace verification: the new test first reproduced objects remaining indefinitely after grace, then `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace` passed five consecutive isolated executions after the fix. The complete `filesystem_fuse` suite passed 43/43, and both catalogue tests plus all three convergence regressions passed explicitly.

Acceptance rebroadcast verification: the strengthened real-repair burst first reproduced duplicate publication announcements, then passed eight consecutive isolated executions after the fix. The repeated-evidence sibling test passed five consecutive executions. The complete `rpc_cluster` suite passed 29/29 and `invariants` passed 36/36; the catalogue burst and exact garbage-grace tests also passed explicitly.

Lagging-replica verification: `rpc_cluster/test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` passed six isolated executions. The expanded complete `rpc_cluster` suite passed 30/30.

Phase 3 repository-wide checkpoint: `sh run-tests.sh build` passed the complete default suite 199/199 and runtime-dependency suite 3/3. The run explicitly included `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`, the real burst and sibling tests, the exact garbage-grace test, and the lagging-third catch-up test.

## Phase 4: bounded metadata RPC executor

- [x] Routed history import, commit storage, and acceptance RPCs to a dedicated executor rather than ordinary control or data workers.
- [x] Kept socket reading, frame assembly, queue admission, ping, membership, ordinary control handlers, and foreground DATA work independent of metadata execution.
- [x] Preserved asynchronous RPC completion: executor owners finish work and queue replies without occupying the session reader.
- [x] Bounded pending metadata work by both job count and payload bytes, returning an immediate in-band error on overload without closing the peer session.
- [x] Preserved per-peer FIFO across history/store/accept operations while permitting explicitly configured workers to execute different peers concurrently.
- [x] Defined and tested lifecycle boundaries: queued work is cancellable; running work owns its durability boundary and survives reply-route disconnect; shutdown completes a running owner and abandons queued work whose session has closed.
- [x] Kept metadata recovery from consuming fast-control, ordinary-control, or foreground DATA capacity.
- [x] Added deterministic gated tests for executor isolation, count/byte backpressure, session survival, per-peer order, cross-peer concurrency, cancellation, disconnect, and shutdown.
- [x] Set the production default to one metadata owner, 64 queued jobs, and 256 MiB of queued payload. This avoids multiplying idle workers while retaining bounded multi-worker support and per-peer FIFO for explicit configurations.
- [x] Replaced stop-and-wait history transfer with a dependency-first window of at most eight asynchronous uploads; sender diagnostics and the lagging-third test prove more than one and no more than eight requests are in flight.
- [x] Moved history-entry reconstruction, delta application, snapshot encoding/hash validation, encryption, and durable history append out of the global replica-state critical section. A separate durability owner preserves append order and a short state lock installs the validated immutable entry afterward.
- [x] Added single-flight off-lock materialization. Eight concurrent readers of a cold 200-delta chain now share exactly one reconstruction and install through a validated short lock step.
- [x] Prewarmed acceptance-policy materializations outside the replica lock and moved accepted-head public reconstruction onto the off-lock path.
- [x] Documented the closed fast-control allow-list (`ping` and `members` on CONTROL frames only), its handler constraints, and the proof required before extending it.
- [x] Fixed the event-driven follow-up edge exposed by the complete suite: a coalesced convergence run which leaves one follow-up pending now continues immediately without requiring an unrelated external wake, while disconnected settled maintenance remains parked.
- [x] Performed the Phase 4 three-node recovery UAT: a paused third replica missed a 65-operation workload, the surviving pair remained writable and responsive, the resumed replica converged to the identical 64-entry namespace, and all nodes returned to sleeping idle at the same generation without RPC churn.

Evidence: [Phase 4 bounded metadata RPC executor](2026-08-30-phase-4-bounded-metadata-rpc-executor.md) and [Phase 4 three-node recovery UAT](2026-08-30-phase-4-three-node-recovery-uat.md)

Verification: the 27-test `storage_metadata` suite passed; the strengthened cold-chain test proved one reconstruction for eight concurrent callers; and the lagging-third test proved a bounded transfer peak in `(1, 8]`. The first repository-wide run correctly exposed the lost follow-up wake in `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`; after correcting that scheduler edge, the catalogue burst and disconnected-idle regressions passed explicitly. The final repository-wide run passed the default suite 203/203 plus runtime dependencies 3/3, including both catalogue burst/search/artwork/GC regressions and the lagging-third recovery test.

## Phase 5: bounded operational diagnostics

- [x] Added accepted-head persistence write, byte, and failure counters with
  success recorded only after durable replacement completes.
- [x] Added fixed-size atomic RPC execution timing summaries by message and
  frame class, including count and total/max queue and handler microseconds.
- [x] Exposed local aggregates through Status without a sampler, polling loop,
  per-request history, metadata mutation, or gossip expansion.
- [x] Added deterministic metadata, RPC isolation/backpressure, and Status API
  assertions for the new diagnostics.
- [x] Documented bounded FUSE recovery batching, shared metadata
  materialization, metadata RPC isolation, and operational interpretation of
  the new counters.
- [x] Observed and retained one initial catalogue-burst suite timeout; the
  immediate isolated run passed, and a second complete run passed 203/203.
  Runtime dependencies passed 3/3.
- [x] After formatting and rebuilding, explicitly passed the 1,000-operation
  FUSE recovery test, lagging-third multi-node RPC recovery test, and catalogue
  sync/search/artwork/GC regression.
- [x] Verified the final handler-only timing implementation with the complete
  `rpc_cluster` group, 34/34.

Evidence: [Phase 5 operational diagnostics](2026-08-30-phase-5-operational-diagnostics.md)

## Phase 5: three-node operational diagnostics UAT

- [x] Paused one deployed replica, admitted 257 directory operations through
  MachaDFS, resumed the same process, verified identical 256-child views, and
  removed the exact fixture with all three nodes active.
- [x] Measured 12 publications for 514 create/delete operations, zero rejected
  metadata jobs, zero accepted-head persistence failures, and bounded RPC queue
  separation from CONTROL work.
- [x] Proved the lagging replica imported required history and persisted one
  final create head rather than every intermediate accepted head, with no new
  reconstruction or delta replay on any node.
- [x] Verified stable generation/counters/RSS, two canonical connections with
  zero churn, direct Status responsiveness, and fully sleeping processes after
  drain.
- [x] Preserved physical DATA GC and repeated-burst RSS as explicit remaining
  work rather than over-claiming them from a directory-only fixture.

Evidence: [Phase 5 operational diagnostics UAT](2026-08-30-phase-5-operational-diagnostics-uat.md)

## Phase 5: journal and convergence Status diagnostics

- [x] Exposed existing namespace publication and operation-journal append/barrier
  totals through an O(1), lock-free FUSE diagnostic snapshot.
- [x] Exposed convergence event/run/epoch totals and scheduled state from the
  existing edge-triggered demand object.
- [x] Kept startup and non-mounted nodes explicit through `available`, and used
  weak frontend ownership so Status cannot extend mount lifetime.
- [x] Proved exact live-path journal semantics: inode descriptor admission,
  operation admission, publication, and completion are four durable append
  groups; recovered operations retain the existing two-group expectation.
- [x] Passed the focused production-shaped API test, `filesystem_fuse` 44/44,
  `invariants` 36/36, the complete default suite 204/204, and runtime 3/3.

Evidence: [Phase 5 journal and convergence Status diagnostics](2026-08-30-phase-5-journal-convergence-status.md)

## Phase 5: journal and convergence Status UAT

- [x] Exercised 65 creates followed by 65 removals on the deployed three-node
  cluster; each half completed in three publications and under 0.3 seconds.
- [x] Accounted exactly for 520 journal records and 272 durability barriers:
  two admission barriers per operation plus grouped publication and completion
  barriers per metadata batch.
- [x] Measured 130 confirmed namespace operations in six publications, or 21.7
  operations per publication, with no retries, failures, timeouts, rejected
  metadata work, or residual queue.
- [x] Observed 23 convergence events on every node, with 8 local and 12 remote
  completed runs; all epochs drained, scheduling parked, and all nodes agreed
  at generation 1439.
- [x] Verified the fixture was absent, direct Status latency was 1-12 ms, and
  delayed process CPU returned to approximately 0.21%, 0.12%, and 0.07%.

Evidence: [Phase 5 journal and convergence Status UAT](2026-08-30-phase-5-journal-convergence-uat.md)

## Phase 5: physical-object GC separation proof

- [x] Confirmed namespace deletion completion does not wait for physical DATA
  removal; metadata publication and grouped journal confirmation remain the
  foreground durability boundary.
- [x] Confirmed physical GC uses a resumable cursor capped at 64 examined
  objects per Service slice and yields immediately to foreground playback or
  mounted-filesystem activity.
- [x] Confirmed destructive reclamation remains fenced by cluster reachability,
  stable/current metadata, catalogue liveness, retention claims, and grace.
- [x] Strengthened deterministic coverage to require a multi-slice GC pass, and
  retained the exact-deadline test proving bytes survive namespace deletion
  until grace before event-driven reclamation.
- [x] Documented the operational distinction and bounded cursor semantics in
  the durability and operations guides.

Evidence: [Phase 5 physical-object GC separation proof](2026-08-30-phase-5-physical-object-gc-proof.md)

## Plan-ledger reconciliation

- [x] Removed stale active entries for off-lock materialization, validated
  short-lock installation, concurrent single-flight deduplication, and its
  deterministic eight-reader proof; these were completed and verified in
  Phase 4.
- [x] Confirmed the final batching/recovery, convergence, RPC-isolation, and
  physical-GC semantics are recorded across `docs/durability.md`,
  `docs/metadata.md`, and `docs/operations.md`.

## Status disk-usage availability correction

- [x] Reproduced the alternating plausible/zero presentation from raw Status
  responses on all three live nodes and located the ambiguity in the API rather
  than the client.
- [x] Made unavailable per-node storage usage/free, cache measurements, and
  online-backend count structurally null instead of fabricated-looking zeros.
- [x] Made incomplete cluster storage/cache aggregates unavailable rather than
  summing missing observations as empty disks.
- [x] Preserved genuine measured zero usage as numeric with `available: true`.
- [x] Added deterministic regression coverage and passed the focused test,
  `invariants` 35/35, and the FUSE/convergence Status integration test.
- [x] Passed the complete default suite 204/204 and runtime dependencies 3/3,
  including the explicit catalogue search/artwork/GC regression.

Evidence: [Status disk-usage availability correction](2026-08-30-status-disk-usage-availability.md)

## Combined Status and repeated-burst UAT

- [x] Verified the unavailable-versus-zero disk contract before and after work
  on every deployed Status endpoint: live self measurements remained numeric,
  while unavailable remote usage, free space, cache measurements, and online
  backend count remained null.
- [x] Completed 2,064 create/remove operations in 48 publications, advancing
  all nodes from generation 1439 to 1487 with exact journal accounting, no
  failure or timeout, and no residual queue.
- [x] Filled every materialization cache to its 64-entry bound. All 3,960 new
  requests were hits; reconstructions, misses, and applied-delta totals did not
  increase, and bounded evictions occurred on every node.
- [x] Verified all convergence epochs drained, scheduling parked, exact UAT
  fixtures were absent, and delayed CPU returned to approximately 0.260%,
  0.142%, and 0.066%.
- [x] Identified that `runtime.rss_bytes` is lifetime-peak `ru_maxrss`, not
  current RSS. The memory-ceiling claim was therefore kept active pending a
  correctly defined metric and repeated post-cap observations.

Evidence: [Combined Status availability and repeated-burst UAT](2026-08-30-combined-status-rss-uat.md)

## Cluster Status telemetry aggregation correction

- [x] Found that authenticated telemetry notifications were transmitted with
  request ID zero but silently ignored by both RPC notification receive paths.
- [x] Routed telemetry notifications from both canonical connection directions
  into the existing bounded speculative executor, keeping decode/store work off
  critical socket-reader threads.
- [x] Added an edge-triggered telemetry wake on authenticated peer observation,
  so connection formation disseminates a coalesced current sample without
  waiting for the periodic local sampler.
- [x] Proved that one two-node Status request contains the connected peer's live
  numeric storage/cache/backend telemetry and a complete online aggregate; no
  client fan-out or HTTP-time network call is required.
- [x] Passed both focused regressions, `rpc_cluster` 35/35, and `invariants`
  36/36. The repository-wide run was accurately retained as 206/207 because
  the tracked catalogue-burst test timed out under suite load; it passed an
  immediate isolated rerun in 0.890 seconds, and runtime passed 3/3. A second
  complete run at four-way concurrency again finished 206/207 with the same
  test timing out at 10.415 seconds.

Evidence: [Cluster Status telemetry aggregation correction](2026-08-30-cluster-status-telemetry-aggregation.md)

## Cluster Status telemetry aggregation UAT

- [x] Queried each of the three deployed Status endpoints independently and
  confirmed that every single response contained all three connected nodes.
- [x] Verified every node was live with numeric storage/cache/backend telemetry
  and that all endpoints returned identical available online/known aggregates.
- [x] Verified the cluster was healthy and writable at generation 1487, all
  convergence demand was drained, and all metadata executor queues were empty.
- [x] Confirmed deployed telemetry notifications were executing under the
  speculative RPC class in both canonical route distributions.

Evidence: [Cluster Status telemetry aggregation UAT](2026-08-30-cluster-status-telemetry-aggregation-uat.md)

## Linux systemd install and uninstall targets

- [x] Added a Linux-default systemd install with binary, unit, configuration,
  example, and documentation paths derived consistently from CMake settings.
- [x] Added first-install-only creation of `/etc/macha/macha.yaml`; upgrades
  preserve operator changes and clearly print the operational config path.
- [x] Added a manifest-based `make uninstall` target which removes managed
  artifacts while deliberately preserving configuration, keys, state, cache,
  spool, mounts, and media data.
- [x] Replaced the legacy EnvironmentFile indirection with a direct generated
  `ExecStart` and documented service enable/disable/reload steps.
- [x] Verified a staged `/usr` install, both generated unit paths, complete
  uninstall, configuration preservation, and checksum-stable reinstall.
- [x] Corrected Debian multiarch systemd installation: cached
  `lib/<architecture>/systemd/system` defaults migrate to
  `lib/systemd/system`, while custom paths remain configurable; also removed
  the generated CMake CMP0012 warning.

Evidence: [Linux systemd install and uninstall targets](2026-08-30-linux-systemd-install.md)

## FUSE spool rate backpressure — first checkpoint

- [x] Retained the safe 16 GiB default while documenting and testing
  `fuse.max_spool_bytes` and `fuse.spool_reserve_free` as configurable policy.
- [x] Replaced logical-capacity `ENOSPC` with event-driven blocking admission;
  only an impossible single request or the independent physical reserve can
  fail for capacity.
- [x] Start publication from spool pressure even while a writer remains open,
  and pace admission from measured end-to-end publication throughput: local
  burst below 50%, progressive slowdown, and publish-rate admission by 90%.
- [x] Added O(1) spool occupancy/rate/wait diagnostics to Status.
- [x] Added focused drain-recovery and stalled-publisher regressions. The FUSE
  group passed 45/45, the complete backend suite passed 211/211, and runtime
  dependencies passed 3/3.

Evidence: [FUSE spool rate backpressure](2026-08-30-fuse-spool-rate-backpressure.md)

## Version 0.21.0

- [x] Bumped the project minor version from 0.20.0 to 0.21.0 across CMake,
  generated server metadata, the release heading, and shipped configuration and
  legacy systemd-wrapper examples.
- [x] Reconfigured and rebuilt every target successfully; generated
  `kServerVersion` is `0.21.0`, and runtime dependencies passed 3/3.

## Linux miniupnpc API 18 compatibility

- [x] Removed the compile-time dependency on `UPNP_CONNECTED_IGD` and
  `UPNP_PRIVATEIP_IGD`, which were added after miniupnpc first shipped API 18
  without changing its API-version number.
- [x] Added a version-aware compatibility classifier for the documented
  `UPNP_GetValidIGD()` ABI values, retaining private-WAN support for API 18+
  and rejecting disconnected/unknown devices.
- [x] Rebuilt successfully with UPnP enabled; the focused compatibility test
  passed and the complete `foundations` group passed 13/13.

## Weighted loader cold-setup accounting

- [x] Separated loader admission/activity tracking from the start of useful
  weighted service, so cold distributed-writer setup cannot consume the loader
  slice before producing work or manufacture a ratio-amplified cooldown.
- [x] Added a deterministic scheduler regression proving that a ten-second cold
  setup is excluded from the 25 ms service slice and 475 ms cooldown at the
  default 95:5 weighting.
- [x] Built and passed the scheduler regression on macOS and natively on both
  Linux nodes 50 and 51, together with all three spool-pressure regressions.

Evidence: [Spool progress-bootstrap admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md)

## FUSE write admission shutdown race

- [x] Closed the final byte-admission race in which shutdown could release one
  blocked writer's lease and allow another waiter to acquire newly available
  capacity after `stopping` had already become true.
- [x] Preserve the bounded-memory contract by checking shutdown after the wait
  condition clears and before accounting or copying the request payload.
- [x] The focused shutdown/admission regression passes, runtime dependencies
  pass 4/4, and the complete core suite passes 262/262 at 12-way process
  isolation.

## Playback allocator bound and decoder parallelism — versions 0.22.6–0.23.0

- [x] Removed the temporary hot-path transport ownership and allocator-sampling
  instrumentation before performance validation.
- [x] Bounded glibc arenas at process start with configurable
  `runtime.glibc_arena_max` (default four) and a hard Linux multi-wave test.
- [x] Added bounded `streaming.video_decoder_threads` (default two, range
  1–16), applied it before codec open, exposed the effective configuration in
  playback Status, and made changes restart-required.
- [x] Passed runtime dependencies 4/4 and the complete core suite 262/262 at
  12-way process isolation.
- [x] Passed four-node UAT: three smooth allocator-bound lifecycles followed by
  two smooth two-thread HEVC lifecycles. Every teardown reached zero playback
  ownership, all heap reclaim attempts succeeded, and drained RSS remained on
  a stable approximately 204–223 MiB plateau without lifecycle ratcheting.

Evidence: [Transport, allocator and decoder checkpoint](2026-09-02-transport-and-allocator-attribution-checkpoint.md)

## Spool progress-bootstrap admission

- [x] Removed the zero-rate dead zone above 50% spool occupancy by granting
  bounded one-for-one admission credit only after useful partial publication
  has successfully drained.
- [x] Preserved the configured hard limit, physical reserve, whole-file atomic
  visibility and event-driven wakeup contract; zero-byte or failed work grants
  no capacity.
- [x] Passed the focused regression and all spool-pressure tests on macOS and
  both Linux nodes, then deployed the identical binary to nodes 50 and 51.
- [x] Passed loaded UAT with rsync and real playback: rsync accepted roughly
  506 MB while publication replayed 441 MB in 30 seconds above the soft
  threshold, with bounded waits, no freeze, no ENOSPC, no backend failure and a
  healthy 3/3 cluster.

Evidence: [Spool progress-bootstrap admission checkpoint](2026-08-31-spool-progress-bootstrap-admission.md)

## Structural remediation Phase 1 — object-store concurrency and priority

- [x] Removed store-index locking from loose/packed payload I/O, AES-GCM,
  hashing, filesystem capacity inspection, loose removal, crash-accounting
  fsync and bounded pack compaction.
- [x] Preserved exact same-object single-flight and introduced logical pack
  reader leases so compaction cannot unlink a pack between index selection and
  `open(2)`.
- [x] Moved physical validation/retention/deletion off CONTROL workers and
  propagated loader/speculative/viewer classes through the shared DATA
  admission boundary, with non-blocking inner RPC admission to avoid bounded
  worker inversion.
- [x] Coalesced deferred durability ownership into exact non-dominated physical
  generation frontiers.
- [x] Passed deterministic blocked-operation, priority, recovery, corruption,
  durability and compaction regressions; final local verification is 267/267
  core plus 4/4 runtime at 12-way process isolation.

Evidence: [Object-store concurrency checkpoint](2026-09-02-object-store-concurrency-checkpoint.md)

## 2026-09-13 — rationalisation pass: work completed through 0.40.1

Moved out of `ACTIVE.md` in full rather than summarised, because in several
cases the reasoning is the record — a retraction, a declined option, or a
measurement that disproved the thing it was taken to support. Open remainders
that were buried inside these items were promoted to their own entries in
`ACTIVE.md` before the move, not carried here.

### From: P0 — Playback correctness and poor-network resilience

- [x] **An abandoned playback session holds a node's only transcode slot for
  30 minutes — found 2026-09-10, FIXED in 0.40.0 on the operator's decision of
  2026-09-13: "shorter timeout if and only if the session has never been
  usefully accessed."** `streaming.session_unused_idle_ms` (default 120 s)
  expires a session that has never served a stream object; one playlist,
  fragment, subtitle or Direct Play body earns the full `session_idle`
  permanently, so a paused or seeking player is never evicted by it. A new
  `stream_served` flag carries that, because `stream_touched` is reset by every
  `start_pipeline()` and can only say "not recently". The reaper takes the
  lesser of the two budgets, so an unused session can never outlive a used one.
  `playback/status` reports `session_unused_idle_ms` and
  `unused_sessions_reclaimed`; gated by
  `test_a_session_never_streamed_from_does_not_hold_a_transcode_slot`.
  **Not deployed** — 0.40.0 is unreleased and the cluster runs 0.39.1.
  The second option considered and **not** taken, per the same decision:
  eviction at admission, which converts admission from a guarantee into a
  lease and is client-visible. Original analysis, kept because the mechanism
  is the record:
  The video/audio transcode entitlement lives on the *session*, not the
  pipeline: `video_transcodes_locked()` (`playback.cpp:1152`) counts sessions
  whose `logical_session->video_transcode_entitled` is set and never inspects
  whether an engine is running. Two independent clocks in the reaper
  (`playback.cpp:2601`): `pipeline_idle` (60 s) reclaims the **engine**, while
  `session_idle` (**30 minutes**) is what finally erases the session and
  releases the entitlement. So pipeline reclaim does not free the slot — it
  makes the session cheap to hold while it goes on holding it.
  With `max_video_transcodes: 1` on these nodes, one session created and not
  deleted closes that node to transcoding for half an hour while the node
  looks perfectly healthy, and `reserve_resources` refuses with
  `429 resource_limit` (`playback.cpp:2780`) — there is no eviction path.
  Because the count is per *logical* session, the viewer who caused it is the
  one person who cannot observe it; the cost falls entirely on others.
  **No client-side fix closes this.** The phone client's failover creates a
  fresh session and drops the old one; `@machafoundation/core` closes all five of its
  standby paths but uses `keepalive`, which React Native ignores and Tizen 3
  does not have, so a suspended app's closing `DELETE` may never leave; and a
  client that crashes, is force-quit or loses power can never send one.
  Explicit `DELETE` releases immediately (`playback.cpp:2349-2357`, which also
  stops the pipeline and removes the session temp directory).
  Two server-side options, neither built: a shorter idle for a session never
  fetched from — `stream_touched` (`playback.cpp:617`) is set at construction
  and advanced only by a real fragment fetch, and the reaper already computes
  both that and "has no running engine" every pass, so this is a branch in an
  existing loop needing no new state; or eviction of an entitled, engineless,
  trafficless session at admission, which is more invasive because it converts
  admission from a guarantee into a lease and is client-visible.

- [x] **Complete VOD playlist with bounded segment holds — shipped and
  deployed to all three nodes 2026-09-08.** Verified live on a real transcode
  session: `PLAYLIST-TYPE:VOD` with `ENDLIST` on first fetch, an in-plan
  segment beyond the hold window refused in 0.35 ms with
  `500 segment_not_ready`, and an index past the plan still a genuine 404.
  Two numbers changed during implementation on findings from the client
  sessions, both recorded in the plan: the refusal is **500**, not 503, because
  the status is the only thing a player can read on a fragment error and every
  intermediary emits 503 for a dead service; and the hold is **6000 ms**, not
  the planned 12000, because hls.js's `fragLoadingTimeOut` is deprecated and
  inert and the tightest real deadline is media3's 8000 ms read timeout on the
  React Native music path.
  **Still open. Asked of the phone session on 2026-09-13 (operator: "there is
  [a device], talk to Macha Mobile App React Native"), and partly answered:**
  - **iOS will not be answered by that session at all** — it has only ever run
    on Android, and there is no iOS device there. iOS's time-to-first-byte
    deadline remains unread by anyone. Nobody should plan around it arriving.
  - **HTTP status never reaches that client's JavaScript**, verified against
    the expo-video v57 source: `PlayerError` is `{message: string}` with no
    status, no code, no cause, and segment/playlist requests go from the native
    player straight to the stream URL without passing through the app's HTTP
    layer. So `segment_not_ready` and a genuinely dead stream are
    indistinguishable *on that client*. The 500-over-503 reasoning still holds
    for players that read status; it buys nothing there. Any measurement has to
    come from logcat at the media3/OkHttp level or a packet capture.
  - **Statically, the hold looks right with margin.** Read from the Gradle
    cache rather than from documentation: the video path (expo-video) uses a
    bare `OkHttpClient` — connect 10 s, read 10 s; the music path
    (react-native-track-player) uses `DefaultHttpDataSource.Factory()` defaults
    — connect 8 s, read **8 s**. So the tightest deadline on that client is
    8000 ms against a 6000 ms hold: the player should receive the 500 rather
    than abort first, with 2 s of margin. **If the hold is ever raised above
    8000 ms the music path starts aborting first.** That is an argument, not a
    measurement.
  - **Blocked on the anonymous-roles experiment**: the phone cannot create a
    playback session while anonymous holds no roles, so questions 1 and 2 wait
    on either that ending or credentials for an account with `media_viewer`.

  Design, phases and the corrections made during implementation are in
  [the plan](2026-09-08-bounded-vod-playlist-and-segment-holds.md), which
  also records that the motivating bug report was retracted in full and that
  this work does not address the DTS/TrueHD cold-start latency below — which
  has itself since been retracted in full; see the item below.

- [x] **DTS/TrueHD "40-60x slower cold transcode" — RETRACTED IN FULL
  2026-09-08 by the session that raised it. There is no exotic-audio decode
  problem.** Every measurement behind this item, both the original 0.36.0
  cold-start figures and the steady-state throughput numbers added later the
  same day, was confounded by which node served the session. The client's
  endpoint registry had settled on gbni-2 — the wireless node, whose raw read
  throughput is roughly 0.55 MB/s against gbni-1's 3.31 MB/s — and its
  `EndpointBandwidth` never sampled media transfers, so the node carrying
  essentially all the bytes was the one it measured least and had nothing to
  deprioritise it with.
  Re-run on gbni-1 after the client fixed endpoint selection, same build, no
  server change: Django first frame 43.4 s -> 22.2 s -> **3.5 s**; Death Proof
  25.6 s and 0.46x -> **2.5 s and 1.00x**; Inglourious Basterds 0.14x ->
  **1.00x**; Full Metal Jacket 0.65x -> 0.95x. Headroom now builds on every
  transcode (Django 9 -> 67 s, Death Proof 9 -> 62 s) instead of pinning at
  zero. Nothing starves.
  **The TrueHD audio anomaly is retracted with it.** Death Proof decoding
  ~1.2 KB/s of audio against ~48 KB/s elsewhere was a symptom of a starved
  pipeline, not a codec fault: on the re-run it decodes 502 KB in 20 s
  (25 KB/s) with 479 video frames and zero drops. A pipeline delivering in
  7-second bursts starves audio and video alike, and the comparison was
  against titles that were not starving.
  **Kept as a lesson rather than deleted.** The measurements were real; the
  inferences on top of them kept landing on the server while the variable was
  on the client. Every "same node, same client, within 15 minutes" control
  cited here was assumed rather than recorded. The client now records which
  node served each measurement, which is what would have caught it hours
  earlier. Do not re-open this on the strength of the numbers above.

- [x] **2. Split lightweight status from expensive diagnostics — shipped in
  0.39.1, deployed 2026-09-13.** `/api/v1/status` is now `cluster`, `nodes`,
  `startup`, `connectivity`, `subsystems` and nothing else; the tree moved to
  `GET /api/v1/status/diagnostics` behind the same `view_status` role, and the
  light response names that route in `diagnostics_endpoint`. Diagnostics was
  68% of the payload live (10,091 of 14,914 bytes), but the locks were the
  larger cost: reaching those numbers took one in nearly every subsystem,
  several held by the busy paths that make someone open Status.
  **The question that prompted it is still open** — see "Status took 10 s" in
  P1 below. The split was deliberately also an experiment: a poll now takes
  none of those locks, so if Status is still slow the cause is not in the
  handler.
  (The other half of this item — `PlaybackManager::status()` holding the
  global session mutex while taking each session's subtitle-cache mutex —
  was confirmed and fixed in 0.24.2: `status()` no longer takes any
  per-session subtitle-cache lock while holding the global mutex, and that
  per-session lock is now `try_lock`-only so one busy session's cache can't
  block a `status()` call at all, only make its own count best-effort.
  Regression test: `test_status_does_not_block_on_a_contended_subtitle_cache`
  in `tests/test_media_playback.cpp`. Separately, the audit's claim that
  `public_stream_response` holds the global mutex across a full libav
  open/probe/seek/demux was re-checked against current code on 2026-09-05
  and is **not accurate** — all such work in both `public_stream_response`
  and `probe_source`/`resolve_session` already runs with the global mutex
  released.)

### From: P0 — Verified correctness defects (found 2026-09-05, code-audit-confirmed)

- [x] **Transcoded playlists declared the plan, not the media — found and
  fixed 2026-09-08 by the new timeline harness, on its first run.**
  `playlist()` emitted `#EXTINF` from `vod_segment_durations` (what was
  planned) while `Segment::duration` (what was published) was written and
  never read anywhere. The first flush of a fragmented MP4 writes the delayed
  `moov` and no `moof`, so that boundary produces no fragment and its media
  joins the next one: fragment 0 measured **6.0s of media while declaring
  2.0s**, and since a player builds its seek map by accumulating `EXTINF`,
  every later fragment sat **four seconds early on the timeline for the whole
  title**. The remux path had been given a `carry_boundary()` call for exactly
  this on 2026-09-07; the transcode branch never got one, and the carried
  value was discarded by the playlist regardless. Fixed on both sides: the
  transcode cut now carries an unproduced boundary (`media_engine.cpp`), and
  the playlist advertises published durations (`media_segments.cpp`). Gated
  by `test_transcoded_audio_and_video_carry_the_same_timeline` — reverting
  either fix fails it. Not yet confirmed against a live client on the
  cluster; the measurement is from the published fragments, not from a player.

- [x] **Every completed transcode generation finished in an error state —
  same run, same day.** `MediaSegmentStore::mark_finished()` compared
  fragment *count* against plan entries, so a run that legitimately carried a
  boundary (25 fragments for a 26-entry plan) was recorded as
  `media pipeline produced 25 fragments for a 26 fragment VOD plan`. Not
  cosmetic: `playlist()` withholds a playlist entirely once an error is set,
  so a transcode that had in fact produced all of its media ended by serving
  an **empty playlist**, and the session reported `exit_code=1`. Now compares
  published media against planned media, which is the invariant
  `publish_duration()` actually maintains.

- [x] **FUSE-mounted reads never register as viewer demand — RESOLVED in
  0.40.0 as intended behaviour, on the operator's answer of 2026-09-13: "FUSE
  is loader, not viewer."** The dead `note_viewer_activity()` declaration and
  definition are gone, the policy is written down on the `FuseFrontend` class
  itself, and the tests that used the hook to simulate viewer pressure now
  drive `FileSystem::note_foreground_activity()` directly, which is what the
  HTTP playback path does. Original finding:
  `FuseFrontend::note_viewer_activity()` is declared, documented as "called by
  the kernel adapter before viewer-critical open/read callbacks", and defined
  — but is never called anywhere. FUSE reads open with `FrameType::loader`
  unconditionally, so the viewer/loader duty-cycle gate that governing law 2
  depends on is driven exclusively by the HTTP playback path today. If any
  client reads media via the FUSE mount directly (rather than through HTTP
  streaming), it currently gets loader priority, not viewer priority. Confirm
  whether this is intentional (FUSE is documented elsewhere as
  "loader/convenience traffic") or a real gap, and wire it up or remove the
  dead declaration.

### From: P0 — Security hardening for a network-exposed cluster

- [x] **Authorization tiers — shipped in 0.38.0, deployed 2026-09-12.** See
  `COMPLETED.md`. Every route now requires a session and is gated on roles.
  Two consequences left open below.

### From: P0 — Structural ingest, metadata and retained-memory safety

- [x] **`repair_once()` holds the metadata mutation mutex across per-peer
  replication RPCs, wedging every metadata mutation on the node — found live
  on es-1, 2026-09-10; FIXED 0.37.0 (lock released across the fan-out,
  re-validated on re-acquire), gated by
  `test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes`
  via the new `stall_peer_for_tests` fixture.** Still under the lock, and
  deliberately so: discovery and `publish_commit`, each bounded by the 30 s
  control no-progress deadline rather than unbounded. `MetadataManager::repair_once()` takes
  `std::unique_lock mutation_lock(mutation_mutex_)` (`metadata_manager.cpp:1908`)
  and then, still holding it, calls `replicate_accepted_head()` once per
  active peer in three separate loops (`:1936`, `:1963`, `:2008`). Each of
  those reaches `push_history_to_peer()`, whose `remote_has` lambda issues a
  blocking `node_.call(owner, has_metadata_history_entry, …)`
  (`metadata_manager.cpp:496`) per history hash. `mutate_impl()` — the entry
  point behind every `mutate_delta()`, and therefore behind every
  `WriteHandle::commit()` — takes the *same* mutex at `:1625`. So one slow or
  unresponsive peer converts a background maintenance pass into a total stall
  of local metadata writes.
  **Measured on es-1 while wedged:** the `macha-maint` thread (LWP 650116) sat
  in `Service::loop → repair_once → replicate_accepted_head →
  push_history_to_peer → AsyncRpc::get()`, and **ten** threads were piled up
  behind it blocked in `MetadataManager::mutate_impl` on
  `pthread_mutex_lock` — the ingest worker among them, inside
  `copy_file → WriteHandle::commit → FileSystem::commit_write → commit_file`.
  Nothing had crashed: there were no `subsystem '…' thread stopped` lines in
  the journal, and `cluster.health` cheerfully reported **`healthy`** with all
  three nodes `online` throughout, which is the same
  observability gap as the "powered-off node reads as online" item in P1.
  The lock is documented as protecting "discovery, accepted-head selection, or
  reconfiguration" (`:1906`). Replication is none of those — the header of
  that very loop says "Convergence is replication, not head replacement" and a
  peer holding a different branch simply keeps it — so the peer RPCs look
  releasable, but the baseline-commit branch (`:1977`-`2019`) does mutate and
  must stay serialised. Fix by narrowing the lock to selection/commit rather
  than by putting a deadline on the RPC; a deadline only bounds how long the
  node is dead for.

- [x] **Ingest is strictly serial, so one wedged job stalls the whole queue —
  FIXED 0.37.0: `ingest.max_concurrent_jobs` (default 10), claimed-set
  ownership, dedicated catalogue poller, `concurrency` on `ingest/status`.**
  `IngestManager::loop()` (`ingest.cpp:1039`) selects a single job and calls
  `process_job()` synchronously to completion before looking at the next;
  `active_job_id_` is one `std::string` (`ingest.hpp:140`) and there is one
  worker thread (`:141`). This is what turned the metadata stall above into
  the visible symptom: **six torrent ingests on es-1, all reporting
  `queued`**, staged under `/mnt/diskB/ingest/torrents/` with staging at
  3.78 GB of a 500 GB limit — i.e. nothing resource-bound, just five jobs
  behind one that could not finish. Wanted: concurrent jobs under a
  configurable bound, `ingest.max_concurrent_jobs`, default 10.
  Note the interaction with the retained-memory items below — N concurrent
  imports means N concurrent `WriteHandle`s against the same durable-lower
  budget, so the bound is a memory knob as much as a throughput one.

- [x] **A publication whose basis went stale retried forever, silently — found
  live on gbni-1 2026-09-10, FIXED 0.37.1.** One inode failed 68 consecutive
  times with `parked_publications` at 0 and health `healthy`. Three defects in
  series: `commit_file`'s content-change guard reported a permanently stale
  publication basis as retryable `EAGAIN` (now `ESTALE`, which replays against
  a fresh writer); the park budget's density rule was unreachable at the
  backoff ceiling, 30 min / 30 s = 60 attempts against a threshold of 100 (now
  backstopped by `RetryPolicy::max_failing_duration`, default 1 h, deliberately
  separate from `failure_window` so a long WAN/wifi outage does not park every
  publication); and a long failure run was DEBUG-only (now WARN plus
  `publications_retrying_persistently` on `diagnostics.filesystem`).
  Trigger was rsync `--append-verify` appending to a file whose publication was
  in flight — a legitimate thing to do that the system mishandled.

- [x] **es-1 publication livelock starves RPC and takes the node out of the
  cluster — FIXED 0.36.8 + 0.36.9, deployed 2026-09-09, see `COMPLETED.md`.**

- [x] **`WriteHandle::drain_one_extent` waits on its extent future with no
  deadline — FIXED 0.37.0.** The real mechanism was one layer down:
  `put_impl` spilled a silent replica after `write_stall` and looked for a
  replacement, but a spilled put still counted as unfinished, so with none
  available the loop never concluded (an infinite 1 ms spin). The extent put
  now carries the pipeline's `DataWorkContext` and fails retryably once
  nothing has moved for the no-progress budget. The fault-injection hook this
  was waiting on now exists (`RpcClient::stall_peer_for_tests`); gated by
  `test_extent_put_to_a_silent_peer_fails_within_the_no_progress_budget`,
  confirmed to hang against the pre-fix code.

### From: P1 — Cluster connectivity, status and operations

- [x] **Two torrents stuck in `metadata` forever with no error — found live on
  gbni-2 2026-09-10, FIXED 0.37.2.** libtorrent's default `listen_interfaces`
  enumeration binds `eth0` and loopback but never `wlan0`; gbni-2's `eth0` is
  `NO-CARRIER`, so its session held loopback sockets alone and could reach no
  peer. Deterministic, not a startup race — a restart rebound identically. The
  engine now binds the node's advertised address (`torrent.listen_interfaces` /
  `torrent.listen_port` override), and the plugin consumes libtorrent alerts at
  all for the first time, so a loopback-only session, a failed bind, a DHT
  bootstrap or a tracker refusal is now in the journal instead of silent.
  **Still open:** none of this reaches the HTTP API. `torrents/status` says
  nothing about listen endpoints or DHT, and `TorrentJob` carries `peers`/
  `seeds` as bare counts with a free-text `error` — so a client cannot tell a
  dead session from a slow swarm. The macha-client team asked for exactly that
  on 2026-09-10 (session health with structured warning codes; per-job
  trackers, stall durations, connected-vs-candidate peers, structured errors).
  It needs new `TorrentJob` fields, a persistence-shape change and the
  cluster RPC bridge to carry them, so it is real work, not serialisation.

- [x] **Sanitizer build — shipped 2026-09-08. CI — declined by the operator,
  not deferred.** `MACHA_SANITIZE` builds the whole tree (core, executables,
  plugins, both test binaries) under `address`, `undefined`,
  `address,undefined` or `thread`; whole-tree rather than per-target because
  `macha_core` is a shared library the executables link and the plugins
  `dlopen`, so partial instrumentation would leave the interposed allocator
  and the shadow memory disagreeing across that boundary. `thread` combined
  with `address` is refused at configure time. Case deadlines now scale
  automatically under instrumentation (3x ASan, 10x TSan, `--timeout-scale` /
  `MACHA_TEST_TIMEOUT_SCALE` to override), because the declared 30/60/120s
  deadlines were chosen against an ordinary build and a spurious timeout would
  hide the report the run existed to produce. Documented in
  `tests/TESTING.md`; the LSan hook the runner already had is unchanged.
  The CI half of this item was **declined by the operator on 2026-09-08** —
  it is not a backlog item awaiting time. The gap it named is therefore real
  and standing: every regression gate remains a human running
  `./run-tests.sh` before deploying, and nothing runs TSan periodically
  unless someone does. Do not re-file CI as an open item; the sanitizer build
  is the part of it that was wanted.
  - [ ] Still open, and now cheap: no TSan run has been made yet against the
    concurrency-heavy subsystems (`net.cpp`/`metadata.cpp`/`playback.cpp`).
    The ~47-thread hand-reasoned lock ordering that made this the audit's
    biggest process gap is still unexercised by a sanitizer. ASan+UBSan
    across the full suite is green as of 2026-09-08.

### From: P2 — Raised by client teams and the operator, not yet decided (2026-09-13)

- [x] **The anonymous account has no password, and a roles-less anonymous
  account is no longer reported as "disabled" — raised by the operator and,
  independently, by the web client session; both fixed in 0.38.4 and deployed
  to gbni-1 and es-1 on 2026-09-13, verified live against the cluster's own
  roles-less anonymous account (403 `anonymous_disabled` before, 201 with
  `roles: []` after).** The reported "cannot set a password" turned out to be the right
  behaviour arrived at for the wrong reason: the API *did* permit it, and that
  was a live privilege hole. `/api/v1/users/me` needs only `media_viewer`,
  which anonymous holds at genesis, so any unauthenticated visitor could
  `PATCH` a password onto the anonymous account and then log in as it — and the
  username/password mint path never consults `session.allow_anonymous`, so the
  resulting bound session survived anonymous access being switched off.
  Anonymous now has no credential at all (`kdf` 0) and `verify` refuses the
  username outright, which also makes the random password existing clusters
  carry inert without a migration. Separately, an anonymous account with no
  roles now mints a session carrying `roles: []` instead of `403
  anonymous_disabled` — that is how a registered-users-only deployment is
  expressed, and the client needs the empty list to know to show a login.
  Details in `CHANGELOG.md` under 0.38.4.

- [x] **Status has no role of its own — requested by the operator via the web
  client session, 2026-09-13; shipped in 0.38.5 as `view_status`, with
  `/api/v1/health` added for liveness.** The operator's decision on the crux
  below was that Status *should* be gated: an anonymous visitor sees cluster
  health only if the anonymous account holds `view_status`, which it might not.
  Anything using `/api/v1/status` as a health check was using the wrong route,
  so there is now a right one — unauthenticated, role-free, and reporting only
  whether this node is serving. The upgrade problem was solved by resolving
  role implications at mint rather than only at write, so existing accounts
  gain the role on their next login with no migration. Details in
  `CHANGELOG.md` under 0.38.5.
  **Original framing, kept because the reasoning is the record:** The client gated its Status
  section on `manager`, which takes the diagnostic screen away from an ordinary
  viewer at exactly the moment it earns its place; leaving it ungated shows it
  to a session the server granted nothing. The capability being asked about is
  neither "manage the library" nor "view media" but "see the health of this
  cluster", and no role says that. A `view_status` role would.
  **What has to be decided first, because it is a reversal.** `/api/v1/status`
  is deliberately ungated today (`service.cpp:191`) and the comment there
  argues the case: an importer-only account watching an ingest is the person
  who most needs to see whether the cluster is healthy, so requiring a role
  would tell them nothing. Introducing `view_status` means that route stops
  answering for any session that lacks it — including the roles-less anonymous
  session a registered-users-only deployment now mints, which is currently how
  a client reaches Status to render a login wall at all. So the question is not
  only the role's name but whether Status becomes gated, and what an
  ungated-but-sessioned caller sees instead. Also needs: the route set
  (`/api/v1/status`, `/api/v1/status/*`, connectivity checks), whether
  `manager` implies it (roles are capabilities, not a ladder, so implication
  has to be argued rather than assumed), and what existing accounts get at
  migration. The client is unblocked — it is on `manager` today and says
  switching is a one-line change once a name ships.

- [x] **Session TTL: 30 days stands — DECIDED by the operator 2026-09-13
  ("30 days is good for now"), asked directly and answered short. Nothing to
  build. DO NOT RE-RAISE AS A DEFECT.** The consequence is understood and
  accepted: a signed-in viewer is logged out 30 days after minting, counted
  from creation rather than last use, so it expires even under daily use. What
  made that affordable is that the client half is fixed — the phone client was
  writing its token to disk and never reading it back, so the 30 days was being
  cut short by the first cold start rather than by the expiry; released as
  their 0.5.1 and verified on device.
  **Keep this fact, whatever a future scheme looks like:** `AuthSession` is
  gossiped to every node *and* persisted on each, so extending `expires_unix_ms`
  on every request would be a replicated cluster-wide write on the hot path of
  every API call. That is what forces any sliding-expiry design to use a
  threshold (extend only when less than half the TTL remains) rather than
  extending on use, and it is not visible from outside the server.
  The three schemes considered, recorded against a revisit rather than deleted:
  sliding expiry with a threshold (recommended at the time); a much longer TTL
  plus "remember me" at mint (cheapest, but a stolen token then lives a year
  unrotated); refresh tokens with short-lived bearers (real per-device
  revocation and "sign out everywhere", far more machinery than a self-hosted
  cluster needs today). Revocation already works under all three — `DELETE` is
  replicated and `credential_generation` invalidates cluster-wide.
  Original finding, kept because it is the evidence: Confirmed against the code rather than the report:
  `SessionManager::create()` (`session.cpp:177`) sets
  `expires_unix_ms = now + anonymous_ttl_` for **every** session, credentialed
  or not — the name is misleading, and a username/password mint takes no
  separate path — while `validate()` never extends it. So every signed-in viewer
  is logged out 30 days after signing in, with no warning and nothing a client
  can do about it. The client-side half (a token thrown away on every launch)
  was theirs and is fixed.
  Three schemes were put to the operator, with sliding expiry recommended:
  extend `expires_unix_ms` on use when less than half the TTL remains — the
  threshold matters because `AuthSession` is gossiped to every node, so
  extending per request would mean a replicated write per request; or a much
  longer TTL plus "remember me" at mint, cheapest, but a stolen token then lives
  a year unrotated; or refresh tokens with short-lived bearers, which buys real
  per-device revocation and "sign out everywhere" and is far more machinery than
  a self-hosted cluster needs today. Revocation already works under all three —
  `DELETE` is replicated and `credential_generation` invalidates cluster-wide.
  **Still open, and deliberately not closed by the 30-day decision: the web
  client should not hold a bearer token at all.** That is a separate question
  from how long a session lives, and the operator answered only the TTL one.
  Anything in JS-reachable storage is XSS-readable. The node already serves the
  web client, so a `Secure`, `httpOnly`, `SameSite` cookie set on a successful
  `POST /api/v1/session` and accepted alongside the `Authorization` header is
  same-origin and natural. No client can substitute for that; it is server work,
  and it belongs with the P0 security items rather than here.

- [x] **`GET /api/v1/users` returns `{"users": [...]}` while every other
  collection in the API uses `items` — CHANGED to `items` in 0.40.0 on the
  operator's decision, 2026-09-13.** Core has accepted either key since its
  0.8.0, so no client needs a release, and gbni-2 goes on emitting `users`
  until it can be upgraded. The operator's other point: the web client should
  not be reading that endpoint directly at all, since it is core's surface.
  Core checked and reports the web client uses its `UsersApi` accessor
  throughout with no envelope handling anywhere — so the accept-either shim is
  in the phone client or one of the two TV clients, and is worth finding: a
  client holding its own copy of a wire format will not notice the next change
  either. Original note: Raised independently by the mobile
  session, which read `users_api.cpp` directly; the web client had already
  built an accept-either shim after its page silently rendered nothing (reading
  `.items` off a payload without it yields `undefined`, which throws nowhere).
  Single records from `POST`/`PATCH` are returned bare, which both clients
  assumed correctly. Cheaper to settle now than after a client ships around it.
  The inconsistency was inherited from the manage endpoints rather than chosen.

- [x] **Clients cannot tell which build a node is running — DECLINED
  2026-09-13. The answer is semver and nothing else.** No git describe, no
  commit hash in Status. The consequence is accepted rather than unnoticed:
  a behavioural claim pinned to a version is only as good as the discipline
  that every behaviour change moves the version, which is what the 0.40.0 bump
  for a removed response field already demonstrates.

### From: P2 — Catalogue and media model

- [x] **Make signed artwork capability URLs actually cacheable — FIXED in
  0.40.0 on the operator's instruction, 2026-09-13.** `exp` is now quantized to
  a bucket of the TTL (rounded up to the bucket after next, so remaining
  validity is always between one and two TTLs), making the URL byte-identical
  for every request inside a bucket and letting the existing 24 h `immutable`
  header be consulted for the first time. Gated by
  `test_catalogue_artwork_url_is_stable_so_it_can_be_cached`. **Not deployed**
  — 0.40.0 is unreleased. Clients need no release; the two id-to-URL memos can
  be deleted once it ships. The "cached posters go stale after a minute or two"
  symptom that came with it is **also resolved, and was never ours**: core
  measured it to a preferred-endpoint swap restamping a different host onto the
  same artwork id, so identical bytes arrived under a new name and were
  re-downloaded (same id, same `?exp&sig`, three hosts, one served in 3 ms and
  another in 923 ms). Their sticky-artwork-host fix landed the same day, and a
  reload then issued **zero artwork requests at all** — not cache hits, no
  requests — which is this `immutable` directive finally being consulted.
  Neither fix alone would have done it: stable URLs with a wandering host still
  re-download, and a sticky host with a per-millisecond `exp` still churns the
  key. The
  measurement behind the fix: ("images load slowly, and when 'cached' they're just less slow").
  What the measurement settled, against current source, so nobody re-derives it:
  the artwork response already sends `public, max-age=86400, immutable`
  (`catalogue_api.cpp:551`, the max-age being `artwork_capability_ttl` in
  seconds), so the header is not the problem; `exp` is **not bucketed at any
  granularity** — `signed_artwork_url()` uses `unix_ms() + ttl` per call
  (`catalogue_api.cpp:99`), and `artwork_json()` runs it for every item on every
  `/items` and `/items/{id}`, twice per item since `artwork` and
  `effective_artwork` are both emitted; and a stable content identity for the
  bytes **already exists on the wire** as the artwork `id` (a SHA-256 of the
  bytes), which core already carries as `ArtworkRef.id`. Two clients
  independently built an id→url memo beside a key they already had; that was a
  core documentation gap, not a missing field, and core is fixing the comment.
  A header-free URL form is also already guaranteed — the signed capability is
  bearer-exempt (`capability_request()`, `catalogue_api.cpp:564`) — so a client
  reporting that artwork needs a header is constructing its own URL instead of
  using the payload's.
  Because there is no bucket, "cached ones go stale after a minute or two" is
  **not** a bucket expiring and has a different, unmeasured cause. Do not
  attribute it; the web client is measuring it.
  The fix, agreed in shape with core: round `exp` **up to a bucket boundary of
  the TTL** rather than to a bare hour — a naive hourly bucket would give a URL
  minted at 10:59 an hour of life instead of a day, invisibly to clients. One
  consequence core flagged: it treats a capability whose `exp` has passed as
  non-re-hostable onto other nodes, so that path will fire more often near a
  boundary. Correct behaviour, and the authenticated URLs are the recovery.
  Original filing: `exp`/`sig` are
  recomputed fresh on every `/items`/`/items/{id}` catalogue call, so the same
  artwork object gets a different query string (and therefore a different full
  URL, which browsers key their cache on) every time — the existing 24h
  `Cache-Control: public, max-age=86400, immutable` on the artwork endpoint
  never gets consulted, and posters are re-fetched over the network on every
  page load. Fix by quantizing `exp` to a coarser bucket (e.g. top of the next
  hour/day) so `sig` becomes a pure function of `(artwork_id, quantized_exp)`;
  repeated fetches within that window then return an identical URL. No
  client-side change needed. Found 2026-09-04 via `macha-client-b8`.

### From: P2 — Diagnostics and repeatable proof

- [x] **"Are any extents unavailable?" could not be answered from the running
  system — asked by the operator 2026-09-13 after gbni-2 was removed, answered
  in 0.40.1.** `diagnostics.repair` now carries `unsourceable_objects`,
  `unsourceable_sample` (up to 32 ids) and `local_unreadable_objects`, and
  repair warns once a minute per kind instead of saying nothing at all.
  **What this does not do, and the reasoning that still stands:** the count is
  evidence, not a verdict — a pull also misses on a busy peer, a failed RPC or
  an exhausted budget, so only a total that climbs across passes, or the same
  ids recurring, means loss. And it only ever sees objects *this* node should
  own: a complete cluster-wide answer still needs the join nobody has built —
  enumerate `FileSystem::live_objects()`, ask every node `StoragePool::has()`,
  report what no node holds, mapped back to paths. That was option 1 of the two
  put to the operator; he chose the logging first. Note a naive `test -f` over
  `objects/xx/yy/<id>.obj` cannot substitute for `has()`: packed objects live
  inside pack files and would every one of them read as missing.
  **Why it mattered here:** `replicas: 2` across three nodes means every object
  that reached its target still has a copy after one node leaves, so the
  expected state is under-replicated rather than unavailable. The exception is
  `min_write_replicas: 1`, which lets a write commit with a single copy — if
  that copy was gbni-2, the extent is gone, and nothing in the metadata records
  which objects only ever had one replica. Stored bytes at the time of removal:
  gbni-1 737 GB, es-1 1.82 TB.

### From: P2 — Code health and error-handling consistency (found 2026-09-05)

- [x] **`MANIFEST.sha256` — DELETED in 0.40.0.** 104 of its hashes failed
  `shasum -c`, nothing in the build referenced it, and the operator's decision
  on 2026-09-13 was that the file has no purpose. Do not reintroduce it
  without a build step that maintains it.


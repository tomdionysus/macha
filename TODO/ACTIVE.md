# Active tasks and concepts to explore

Last updated: 2026-09-08

This is the authoritative, ordered backlog. Detailed plans and UAT records in
this directory remain evidence; completed work belongs in `COMPLETED.md` and is
not repeated here. Work top-to-bottom unless new evidence changes the order.

2026-09-05: full reprioritisation pass. Two independent full-repo audits were
run: (1) every doc under `TODO/`, `COMPLETED.md` and `CHANGELOG.md` in full,
cross-referenced against each other; (2) an independent, docs-blind walk of all
of `src/`/`tests/` verifying claims directly against current code (file:line
citations). Findings were then cross-checked against each other. Consequences
of that pass:

- Items confirmed shipped (by both the changelog and direct code inspection)
  were moved to `COMPLETED.md`.
- Items this file called open that code inspection shows are now partially
  shipped were reworded rather than re-litigated from scratch.
- A new **P0 — Verified correctness defects** section and a new
  **P0 — Security hardening** section were added: these are concrete bugs and
  a concrete exposure found by reading the actual code, not carried over from
  older docs. Nothing in `src/` carries a `TODO`/`FIXME` marker — the entire
  informal backlog that would normally live in comments instead had to be
  found as verified behaviour.
- A new **P1 — Scaling cliffs** and **P2 — Code health / error-handling
  consistency** section record real, verified-but-not-yet-urgent debt (the
  system is small today; several of these are O(N) or O(N²) patterns that are
  invisible at current scale and will not stay invisible).
- This pass is still not exhaustive for the 73 dated plan docs in this
  directory — see the "Documentation hygiene" item under P2 for the specific
  staleness this audit found in those docs, root docs, and `CHANGELOG.md`.

2026-09-08: pruning pass. Every remaining item was re-checked against current
source (file:line) and against 0.24.1–0.35.0 in `CHANGELOG.md`. Items that
shipped, that a later release superseded, or that duplicated another item were
removed here and ledgered in `COMPLETED.md`; items whose evidence had moved on
were reworded down to what is actually still open rather than left carrying a
history that reads as work. What went, and why:

- The self-healing programme (0.29.0–0.32.0) and its `[x]` durability-wedge
  finding: shipped. Its three sub-items went with it — the publication hot
  loop is now under `RetryPolicy` backoff and parking (0.30.0), and the 120 s
  startup gate is now a no-progress gate (`startup_progress.hpp`,
  `service_startup_no_progress_ms`, 0.30.0). Only the duplicate-path cause
  survives, promoted to its own item.
- The DLT7 scaling item: shipped as DLT7 in 0.32.0 (`metadata.cpp:780`).
- Four `[x]` correctness/security items (0.24.0–0.24.3) and the retracted
  `LocalStore::valid()` scaling item, which was already restated in P0.
- The unlink-doesn't-abandon-publication sub-item, which was the same fix as
  the "short-circuit unlink" item in P0 structural; merged into it.
- The `mount_path` deployment rule: all three nodes run 0.34.0, so every
  config has already moved.
- The node-50 SSH item: diagnosed off-list as gbni-1 browning out under build
  load, which is now a standing operational rule, not an open investigation.
- Two claims in the "dead wiring" item that code inspection now contradicts
  (`StorageLock` is used at `cluster.hpp:77`; the scanner's
  `request_media_profiles` is invoked from `catalogue_api.cpp:441`).

The governing laws are:

1. Thou Shalt Not Make The Viewer Wait.
2. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.
3. Control traffic must remain promptly serviceable. Viewer priority is a large
   configurable share (95:5 by default), not indefinite starvation of all other work.

## P0 — Playback correctness and poor-network resilience

Execute the phased
[playback resilience and A/V sync plan](2026-09-03-playback-resilience-and-av-sync-plan.md).
This coalesces the newly observed audio drift, slow streaming, intermittent
Status latency, choppy playback, cold admission, and poor-Wi-Fi behaviour into
one causal programme rather than treating each symptom separately.

- [ ] **1. Correct A/V desynchronisation — partially shipped.** Bounded audio
  drift compensation shipped in 0.23.8/0.23.9 (libswresample `async=1` +
  `swr_next_pts()`, verified ±15ms over 8 minutes with no pitch shift); the
  transcode-seek keyframe-snap and rounding fixes shipped in 0.23.10/0.23.11.
  Still open, per the plan doc's own 2026-09-05 progress note: session-relative
  timeline origin; codec delay/priming applied exactly once by one documented
  owner; monotonic DTS/PTS across encoder flush, fragment rollover and
  generation change; bounded correction of malformed inputs; Direct/Remux
  regressions. No harness exists yet for the real (non-stub) transcode audio
  path — both the 0.23.8 and 0.23.9 defects were only caught by live
  measurement/listening, not CI. Building that harness remains this phase's
  stated exit-criterion prerequisite.
- [ ] **2. Split lightweight status from expensive diagnostics.**
  `ClusterStatusService::status_response` (`src/status_api.cpp`) still
  unconditionally computes and includes the full `diagnostics` object on
  every ordinary `/api/v1/status` call — no opt-in/expensive split yet.
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
- [ ] **4. Make transformed output bandwidth-aware.** Auto negotiation selected
  H.264/AAC but, without a client maximum bitrate, CRF output expanded a roughly
  5 Mbps source to bursts around 7–12 Mbps. Define a conservative poor-network
  default, honour client limits, and prevent a compatibility transcode from
  silently increasing delivery demand.
- [ ] **5. Build useful buffer margin.** Do not run a four-second-fragment
  producer and network delivery only one fragment ahead. Bound startup work,
  produce ahead under viewer priority, and recover from transient stalls
  without restarting or multiplying sessions. Note: `MediaSegmentStore` never
  evicts fragments and disk spill is unbounded (2026-09-05 code audit) — any
  buffer-margin work should fix that bound at the same time, not paper over it.
- [ ] **6. Remove avoidable cold transformed-planning work.** Persist complete
  immutable profiles and remux/seek-index suitability. Admission must use
  stored information, never wait on speculative profiling, coalesce background
  scans, and retain normal media-engine fallback. The "immutable profile hit"
  fast path in `probe_source()` already exists and takes no probe reads; what
  remains is moving a cold miss off the synchronous foreground request path
  (confirmed via 2026-09-05 code audit of `playback.cpp`).
- [ ] **7. Improve viewer source delivery — confirmed safe to start,
  narrower scope than it looks.** Investigated 2026-09-05: real prefetch/read-ahead
  (`ReadAheadHintProvider`, `HydrationScheduler`, `CacheHydrator`) and real
  locality preference (`DistributedStore::owners`/`ranked`, `ReplicaSelector::order`)
  already exist and do not overlap items 1/3/4/5/6 below (different files,
  confirmed by direct code inspection). The actual gap is narrower than the
  original wording: `CacheHydrator`'s worker pool fetches one extent per RPC
  with no coalescing of adjacent sequential stripes into fewer/larger transfers
  and no dedup of redundant full-stripe copies. Extend `hydration.cpp`/
  `distributed_store.cpp`'s existing fetch path rather than building new
  infrastructure.
- [ ] **8. Optimise codec throughput only after phases 1–7 are measured.** Keep
  proven bounded decoder parallelism and memory lifecycle. Evaluate hardware
  acceleration or more parallelism only against corrected transport/timelines.
- [ ] **9. Run the ordered UAT matrix.** Test Direct, Remux and Transcode; start,
  seek and quality changes; LAN and impaired Wi-Fi; cold/warm profiles; one/two
  logical viewers; and concurrent loader work. Gate on bounded A/V drift,
  prompt Status, stable playback, no ambiguous 404, exact session admission,
  bounded RSS/CPU and prompt teardown. This is one of nine exit gates across
  the whole backlog with no recorded result yet (see plan docs); none of the
  other eight block this one.

The completed logical-viewer entitlement work is the foundation: capacity is
tied to one persistent logical viewer/UI session, not each stream generation.
Keep teardown active until UAT proves that seek, quality changes, disconnects,
supersession, failure and failover cannot leak physical encoders or produce
`transcode limit reached` for one viewer.

## P0 — Verified correctness defects (found 2026-09-05, code-audit-confirmed)

None of these came from a TODO/FIXME comment — there are none anywhere in
`src/` or `tests/`. Each was independently verified against current source,
not inferred from docs. All are small and isolated; none require design work.

- [ ] **FUSE journal can hold two inodes on one path** (gbni-1: 11240 vs
  7270 on `/TV/Big.Mistakes.S01E01…mkv`, a create-over of a file whose
  previous inode still had pending data). 0.28.3 resolves it at recovery
  instead of exiting, and re-journals the loser, so this no longer stops a
  node starting — what is still unexplained is why the displacing op did not
  clear the old path in the first place (`unlink`/`rename` journal the
  descriptor *before* clearing `current_path` and rely on the op record at
  replay). Find that, and add the hand-built-journal test: 0.31.0's
  `test_fuse_journal_fuzz_every_frame_mutation_still_starts` fuzzes frame
  encoding, not this shape.
- [ ] **A FUSE mount can stop adopting the cluster namespace (live, gbni-1,
  2026-09-06) — root cause fixed in 0.28.2, three related gaps still open.**
  The mount sat two hours behind its own replica: a namespace op published
  before a restart was recovered as "unconfirmed", its effect had since been
  overwritten, and `refresh_namespace_if_stale()` refused every newer view
  while any op was unconfirmed. 0.28.2 retires published ops at recovery
  and confirms live ones by generation (`NamespaceOp::published_generation`),
  and exposes `namespace_refreshed_revision`/`namespace_available_revision`
  plus `FUSE namespace refresh deferred reason=…` so staleness is visible.
  The `stat` 0-bytes-on-two-nodes finding in the next item is plausibly the
  same mechanism (a mount holding a pre-publication entry): rechecked
  2026-09-06 15:50 before the 0.28.2 rollout, all three mounts already
  agreed on 2421711002 bytes after the day's restarts, which is what a stale
  mount (not stale data) predicts. The op that wedged gbni-1 was
  `seq=149 kind=chmod` on an 'Allo 'Allo episode. Still open:
  - [ ] Adoption is also refused while any op is *queued or in flight*
    (`fuse_frontend.cpp` `namespace-queue`/`queue-race` deferrals). An op
    retrying on a retryable backend error (write floor unavailable, peer
    down) therefore blinds the mount to every remote change for as long as
    the retry lasts. Replace the global gate with per-path protection: skip
    only the paths touched by pending ops (`snapshot_path_shadowed()` in the
    recovery path already has the exact rule), adopt everything else, and
    re-run adoption when the pending set changes.
  - [ ] A crash between `apply_namespace_backend()` returning and the
    `namespace_published` journal marker re-publishes the op on recovery
    unless `namespace_effect_confirmed()` happens to see it; a re-published
    `mkdir` gets EEXIST, which is non-retryable, which wedges the queue
    behind an operator skip. Journal the marker before reporting success,
    or treat EEXIST-with-matching-entry as achieved.
- [ ] **`rm -rf` on a FUSE-mounted directory fails with "directory not empty"
  and has no effect, and `stat()` of the identical path returns a different
  size on different nodes (live findings, 2026-09-06) — likely one root
  cause, not two.**
  - Original report: `rm -rf /mnt/machamedia/*` on `corvus-gbni-1` neither
    deletes anything nor reports a sensible per-entry error — it fails
    outright with ENOTEMPTY, which should not be possible for a plain
    recursive delete of files/directories the caller can already
    `readdir`/`stat`.
  - Corroborating finding, found independently while verifying the 0.25.0
    deploy across all three nodes: `stat` on the exact same path
    (`/mnt/machamedia/TV/Chernobyl (2019)/Chernobyl (2019) - S01E01 -
    1.23.45 (1080p BluRay x265 Silence).mkv`) returns the correct size
    (2421711002 bytes) on `corvus-gbni-1` but **0 bytes on both
    `corvus-gbni-2` and `corvus-es-1`**, reproducibly and stably (rechecked
    twice, ~15s apart, unchanged) — not a transient post-restart race.
    Meanwhile every node's own `/api/v1/status` reports the cluster as fully
    converged: identical `metadata_generation` (7552) on all three, quorum
    validated, health "healthy". So the shared/replicated metadata layer
    believes it agrees, but each node's own FUSE-facing view of at least
    this file's attributes does not actually agree — pointing at namespace/
    attribute *projection* inside `FuseFrontend`/`FileSystem` (turning
    replicated metadata into a local `getattr`/`readdir` view), not at
    metadata replication/consensus itself, which the aggregate numbers say
    is fine.
  - This was found by chance while spot-checking a handful of files during
    deploy verification, not a systematic sweep — treat "affects at least
    one file across at least two nodes" as a floor, not a ceiling, on how
    widespread this is.
  - All three nodes were freshly restarted onto 0.25.0 within the same ~90
    minute window this was found in, but nothing in 0.25.0 touches metadata/
    namespace/getattr code (it only added `run_supervised`, the shared
    `macha_core` build, and the subsystem-plugin scaffolding) — a restart is
    much more likely to have *surfaced* a pre-existing local-view bug (by
    forcing every node to rebuild its namespace projection from scratch)
    than to have introduced one, but this has not been confirmed against a
    pre-0.25.0 baseline and shouldn't be assumed either way.
  - Not yet root-caused. First places to look: whatever `getattr`/`readdir`/
    `unlink`/`rmdir` return for entries with in-flight or already-completed
    namespace mutations, and specifically why a freshly-restarted node's
    rebuilt local view would disagree with another node's for the same
    replicated metadata generation (the P1 scaling item on `readdir` being
    O(entire namespace) touches the same frontend code, but this is a
    correctness bug, not a performance one). Needs a minimal repro (a small
    directory tree, known contents, compare `stat`/`readdir` output across
    all three nodes, then delete it) before guessing further.

- [ ] **A hard failure in one subsystem takes down the entire macha process
  (new, found 2026-09-05 live incident) — foundation shipped in 0.25.0,
  FUSE/Torrent migration still open.** `FuseFrontend`'s durable-journal
  replay throws `DecodeError` on any unexpected record, uncaught, which
  crashes the whole node — including its unrelated metadata/RPC/API roles —
  not just the local FUSE mount. Lived this directly: a 0.24.3 bug in
  `skip_blocked_namespace_operation()`'s journal bookkeeping (fixed in 0.24.4)
  crash-looped `corvus-es-1` 49 times because journal replay runs in the
  constructor with no isolation. 0.25.0 shipped the mechanism (Phase 0 of
  [subsystem crash isolation via a plugin architecture](2026-09-05-subsystem-plugin-isolation-plan.md)):
  a mandatory `run_supervised` thread-entry guard on every subsystem thread
  (~30 sites), `macha_core` as a shared library, the `Subsystem`/plugin ABI,
  and `SubsystemSupervisor` (`dlopen` + version-checked load + backed-off
  retry + disable-after-N-failures), verified against real fault-injecting
  `.so`/`.dylib` test plugins. 0.28.0 shipped Phase 1: BitTorrent acquisition
  is now a real `dlopen`'d module (`libmacha-torrent`), reached through
  `TorrentService`/`SubsystemRegistry`, absent-or-faulted per node at runtime.
  This item still does not close: `FuseFrontend`'s constructor — the one that
  actually crash-looped es-1 — is exactly as unprotected as it was until
  Phase 2 moves FUSE into its own plugin and off the main thread. Single
  binary, single process throughout, no separate OS processes/IPC (considered
  and rejected).
- [ ] **Terminal durability poisoning is never cleared.** `fuse_frontend.cpp`
  sets `durability_poisoned = true` on any exception during the durability
  batch and nothing ever resets it — one transient fsync failure disables all
  writes on that node for the rest of the process lifetime. Needs an explicit,
  deliberate recovery path (even if only "restart the process"), not silent
  permanent lockout.
- [ ] **Unbounded startup wait with no escape.** `wait_for_initial_namespace()`
  (`fuse_frontend.cpp`) busy-waits in 100ms sleeps forever, with no timeout or
  cancellation, and runs from the `FuseFrontend` constructor before
  `fuse_mount`. A node whose metadata replica never becomes available hangs
  indefinitely with only a debug log line to show for it.
- [ ] **FUSE-mounted reads never register as viewer demand.**
  `FuseFrontend::note_viewer_activity()` is declared, documented as "called by
  the kernel adapter before viewer-critical open/read callbacks", and defined
  — but is never called anywhere. FUSE reads open with `FrameType::loader`
  unconditionally, so the viewer/loader duty-cycle gate that governing law 1
  depends on is driven exclusively by the HTTP playback path today. If any
  client reads media via the FUSE mount directly (rather than through HTTP
  streaming), it currently gets loader priority, not viewer priority. Confirm
  whether this is intentional (FUSE is documented elsewhere as
  "loader/convenience traffic") or a real gap, and wire it up or remove the
  dead declaration.
- [ ] **Flaky test needs a real fix, not another isolation-pass shrug.**
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
  has now failed under parallel-suite load and passed in isolation on at least
  four separate occasions across this project's history (recorded in three
  separate plan docs plus this session's own test runs). "Passes in isolation"
  has been the accepted verdict every time; the actual race has never been
  root-caused. Do that now rather than re-recording the same flake a fifth
  time.

## P0 — Security hardening for a network-exposed cluster

Found during the 2026-09-05 code audit. Macha's own `SECURITY.md` already
states the trust model plainly ("Anyone with the cluster key can authenticate
as a node and access cluster data") — these are gaps *within* that accepted
model, on the HTTP surface a client (and potentially the public internet, via
the offsite Spain node) actually talks to. Given the cluster already spans a
home network and an offsite node, this is not a hypothetical exposure.

- [ ] **Wildcard CORS header remains.** `Access-Control-Allow-Origin: *` is
  still sent unconditionally on every response, including mutating ones. Less
  severe now that every route requires a valid session bearer token (a
  malicious page can't drive the API without already possessing one), but
  still not best practice — a page that somehow obtained a token (e.g. one
  leaked to a compromised client) could use it cross-origin undetected. Stop
  sending a wildcard origin on any endpoint that doesn't strictly need it.
- [ ] **No authorization tiers yet, though the seam now exists.** 0.24.0 gave
  every session a `roles` list and a `session_has_role()` helper, but nothing
  is wired to check it: one bearer token still grants catalogue reads,
  media-file deletion, namespace deletion, and cluster identity-association
  reset (which can be wildcard: `port: 0` matches every port, an omitted
  `node_id` matches any stale identity) — `manage_api.cpp` itself advertises
  `"privileged": false` in its own discovery document while gating deletion
  behind the same token as read access. Add at least a read-only vs.
  destructive role split and gate the destructive `manage_api.cpp`/
  `catalogue_api.cpp` routes on it.
- [ ] **Unbounded JSON recursion depth.** `json.cpp`'s recursive-descent parser
  has no depth limit. Combined with the 8 MiB body cap, a deeply nested body
  on any POST/PUT can exhaust the stack. Add a depth limit.
- [ ] **Untrusted length fields drive large allocations before validation.**
  `metadata.cpp` (at least 4 sites), `telemetry.cpp` and `cluster.cpp` each
  read a length/count field off the wire and `reserve()` a container to it
  before reading any of the actual data — e.g. a 4-byte field in `metadata.cpp`
  can trigger a multi-hundred-MB allocation from one small malicious or
  corrupt message. Cap reservations to the remaining message size.
- [ ] **Unsigned identity-reset tombstones accepted from any peer.**
  `membership.cpp` checks only `epoch >= existing` before evicting a roster
  entry and tearing down routes for it.
- [ ] **Unvalidated, echoed `Content-Type` on a bearer-exempt endpoint.**
  `POST /api/v1/catalogue/items/{id}/artwork?mime=` accepts any non-empty
  `mime` query value and `GET /api/v1/catalogue/artwork/{id}` echoes it
  verbatim with `Cache-Control: public, immutable` and no `nosniff`/CSP, on an
  endpoint that doesn't require the bearer token. Validate against an
  allow-list of image MIME types.

## P0 — Structural ingest, metadata and retained-memory safety

Resume the
[structural ingest/runtime remediation](2026-09-02-structural-ingest-runtime-remediation.md)
after the immediate playback correctness blocker. Existing checkpoints remain
valid evidence, but do not prove the end-to-end invariants.

- [ ] Finish process-wide retained-memory ownership bounds for decoded metadata,
  catalogue/profile state, reconciliation retries, RPC/reassembly, object
  payloads and playback. Retest unexplained idle RSS growth and the fixed
  `hydration executor is stopping` shutdown race.
- [ ] **Confirm the decrypted history/materialisation cache is bounded by
  bytes, not just entry count, and sheds under pressure.**
  `metadata_materialization_cache_bytes` exists as a config default and is
  reported in Status, but no audit or test has watched it actually shed
  end-to-end under memory pressure. (The compaction protocol itself shipped
  in 0.23.6 and the `accept_commit()`-holds-`m_`-across-`persist()` half was
  fixed in 0.24.3; both are ledgered.)
- [ ] **Retention journal grows ~N² over a large import.** Each publication
  quantum re-claims every extent of the file, so a file published in K
  quanta writes K × extents retention entries. Recorded during the 0.32.10
  measurements in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md)
  and not acted on. The rest of that incident's programme — the serial
  `has_on` loop, the writer-side barrier, the CONTROL put fan-out — shipped
  in 0.26.0 and 0.32.7–0.32.10 and is measured on the real cluster there
  (es-1 retention avg 5,217 ms → 121 ms, no deadline-exceeded since 0.32.7).
  The other follow-ups that run recorded are also still open: present-content
  skip on writer restart
  ([plan](2026-09-06-skip-redundant-replica-writes-for-present-content-plan.md)),
  journal compaction while busy, compact extent encoding, and 9 standing
  conflicts that need a human.
- [ ] **Short-circuit unlink of an in-flight (not yet published) write —
  raised 2026-09-06, during the `has_on` incident.** Confirmed against
  the actual code: `FuseFrontend::unlink()` (`fuse_frontend.cpp:5311`) only
  detaches the pathname from `state_->paths` and journals the op; it does not
  touch the inode's `data_ops`/`durability_pending`/`unconfirmed_data_entry`
  state — nor do rename-over or truncate. `journal_data_abandoned` is reached
  only from `abandon_corrupt_data` and from *recovery*
  (`test_fuse_recovery_abandons_publication_for_file_removed_from_namespace`),
  never from the live namespace path. The
  `refresh_namespace_if_stale()` comment confirms this is deliberate today:
  "Open handles and dirty state retain the detached inode object." So an
  unlinked-but-still-publishing file's data keeps flowing through the full
  pipeline (spool → durability → distributed publish → retention) to
  completion, and only afterward does it become ordinary garbage eligible
  for the `garbage_grace` GC pass — wasted CPU/disk/network for content that
  is already known, at unlink time, to be moot. Wanted behaviour: on unlink
  of a dirty inode with no writable handles, (1) retire its unpublished
  `data_ops` and stop scheduling further publication work for it, (2) reclaim
  its spool bytes immediately rather than waiting through
  durability+publish+grace, (3) ensure the eventual durable history records
  "never existed" rather than "created then deleted," so retention/GC never
  has to process those extents at all, not even later.
- [ ] **Catalogue does not react to a committed unlink — raised 2026-09-06,
  same discussion.** When a namespace unlink is actually durably committed/
  synced (not the short-circuit-unlink case above, which is about aborting
  publication early — this is about the ordinary case where a real file is
  genuinely removed), nothing today notifies the catalogue layer to check
  whether that removal affects any catalogue item. `manage_api.cpp`'s
  `DELETE /api/v1/manage/filesystem` handler calls `hints_.erase_prefix(path)`
  (line ~805) on that one path, but that only clears scanner *hints*, not
  catalogue items, and the ordinary FUSE `unlink()` path
  (`fuse_frontend.cpp:4627`) has no catalogue awareness at all. Wanted
  behaviour: on commit, determine whether any catalogue item's full set of
  referenced files is now empty, and if so remove or update that item —
  **not** a naive "unlink one file -> delete the catalogue item," since (a)
  two namespace paths can reference the same underlying file, and (b) a
  single catalogue item may legitimately reference more than one file (e.g.
  multiple quality variants/parts). The invariant to hold: the catalogue
  (and therefore the Movies/TV UI) must never continue showing an item that
  no longer has any surviving backing file — no phantom library entries.
- [ ] **`DELETE /api/v1/catalogue/items/{id}/metadata` hangs** (separate
  issue, previously filed inside the item above). It hangs because its HTTP handler
  currently performs `repair_once()`, materialises and copies the complete
  catalogue, repeatedly scans all items to discover descendants, walks all
  artwork, and waits for the distributed metadata commit before replying. The
  endpoint must promptly validate the item/revision, durably enqueue one
  idempotent clear job, and return `202 Accepted` with a job/status `Location`;
  retries must recover the same operation. Background execution must use an
  indexed descendant closure and delta-sized mutation, remain below control and
  viewer priority, publish completion/failure explicitly, and preserve atomic
  hierarchy removal plus targeted media rematching. Add large-hierarchy,
  concurrent retry, restart/recovery, conflict, failure and latency regressions;
  neither request handling nor status polling may perform repair, full-snapshot
  copying, remote durability waits or artwork-wide scans.
- [ ] Complete durable namespace batching for mixed create/rename/unlink chains.
  Keep rename a safe singleton until crash/restart proof exists.
- [ ] Make spool backpressure smooth and visibly progressive near its configured
  limit, pacing toward measured publication/drain rate instead of alternating
  full-speed bursts and apparent freezes.
- [ ] Prove large-history, partition/sibling-head, cache-pressure,
  unclean-restart and stale-FUSE recovery, then run a guarded overnight
  four-node rsync UAT. Require bounded RSS/swap/history, automatic rejoin,
  writable metadata, no viewer/control regression and no manual mount cleanup.

Do not tune aggregate ingest throughput around known amplification. Older FUSE
throughput, heap-audit and ownership documents remain detailed evidence but are
absorbed here rather than separate active programmes.

## P1 — Cluster connectivity, status and operations

- [ ] Support multiple advertised endpoints per durable node (LAN/WAN,
  IPv4/IPv6 and configured/discovered), multiple bootstrap candidates,
  reachability-aware racing/fallback, expiry and deduplication by node identity.
  Integrate UPnP/external-IP discovery. Test poor Wi-Fi, partitions, NAT without
  hairpin, endpoint changes, simultaneous dial and commit interruption.
- [ ] Revisit UPnP/external-address discovery sources generally (currently only
  wired to the RPC port's `network.advertise`/`connectivity.advertised`); when
  this happens, fold in the 0.23.7 per-node advertised API endpoint
  (`catalogue.api.advertised_host`/`advertised_port`, `nodes[].api_host`/
  `api_port`) added for any-node Direct Play failover, which today is a static
  config-only override with no UPnP/external-IP probing of its own.
- [ ] Make client/API failover preserve one logical operation: reuse session
  idempotency keys, reconcile ambiguous POST results, fail over endpoints, and
  never turn transient transport loss into a misleading 404 or duplicate lease.
- [ ] **Unconfirmed, flagged during 0.24.0 joint client/server live testing:**
  the client's cluster discovery advertised a node as `toms-macbook-pro.local`
  rather than `127.0.0.1`, and a request to that `.local` hostname sat pending
  (possibly slow mDNS resolution, possibly a CORS mismatch for that specific
  origin). Not investigated — surfacing here since it may be the same
  advertised-endpoint-discovery gap as the item above, or a distinct client-
  side resolution issue.
- [ ] **Correct aggregated Status truthfulness and freshness — partially
  shipped.** 0.23.3 fixed the *freshness* half (a stale sample now reports
  explicit `unavailable`/`recovering` state rather than fabricated zero or
  stale-as-live data) — confirmed directly against `status_api.cpp`. **Still
  open:** the *aggregation* half. This was independently reproduced live
  *after* 0.23.3 shipped (`2026-09-03-fuse-terminal-recovery-loop.md`): node
  200 reported `state: online` while genuinely recovering at generation 0, and
  separately reported ES-1 as green at generation 0 in its own aggregate view
  while ES-1's own endpoint reported healthy/writable at generation 4507. Audit
  how a node folds a peer's telemetry into its own aggregate response — this
  is a different code path from the per-sample freshness fix. This is likely
  the same underlying gap as playback P0 item 2's Status-latency investigation
  above; resolve together rather than tracking twice.
- [ ] Add optional display-only `node_name` at `.nodes[].node_name`; configure
  `Corvus GBNI-1`, `Corvus GBNI-2`, `Corvus ES-1`, and `Corvus MacBook Pro`.
- [ ] Complete hard-kill stale-FUSE recovery proof and automatic clean rejoin.
- [ ] Diagnose faulty torrent/ingest independently so it does not obscure
  convergence and runtime measurements.
- [ ] Diagnose `ingest failed: metadata acceptance certificate durability
  floor unavailable` failures on torrent ingest once the torrent has
  downloaded, which are also unaccountably slow.
- [ ] **Add CI and a sanitizer build.** There is currently no CI configuration
  of any kind in this repo (confirmed: no `.github/`, no CI file anywhere) and
  no ASan/TSan/UBSan build option — every regression gate is a human manually
  running `./run-tests.sh` before deploying. For a system running roughly 47
  threads per mounted daemon with this much hand-reasoned lock ordering across
  `net.cpp`/`metadata.cpp`/`playback.cpp`, this is the biggest single process
  gap found in this audit. At minimum: a CI job that builds with
  `-DMACHA_WARNINGS_AS_ERRORS=ON` and runs the full suite on every push, and a
  TSan build variant to run periodically against the concurrency-heavy
  subsystems.

## P1 — Scaling cliffs (found 2026-09-05, not yet urgent at current 3-node/home scale)

These are real, verified, O(N) or worse patterns that cost nothing today and
will not stay that way. Listed here rather than P0 because nothing currently
observed ties a live symptom to them — but the worst one (`readdir`) is a
plausible contributor to "the mount feels slow with a big library" if that's
ever reported, and is worth fixing opportunistically rather than waiting for
that report.

- [ ] **`readdir` is O(entire namespace).** `fuse_frontend.cpp` iterates *all*
  paths under the global namespace mutex, taking each inode's mutex, for every
  directory listing — same pattern duplicated in `rmdir` and twice in
  `rename`. `FileSystem::NamespaceIndex` already has a parent→children index
  the frontend doesn't use for this. This is the single worst scaling property
  found in the codebase audit.
- [ ] **SHA-256 plus a heap allocation inside a `std::sort` comparator.**
  `placement.cpp`'s `fallback_score()` allocates and hashes twice per
  comparison, and `StoragePool::ranked()` — hit on every put/get/has/valid/
  remove — sorts using it.
- [ ] **`MetadataManager::node_info()` and `replica_nodes()` are O(n²) roster
  copies per metadata read** (a full roster copy per call to `node_info()`,
  called once per id inside `replica_nodes()`).
- [ ] **`CatalogueHintQueue` is a linear `find_if` over the whole map at 8
  call sites**, with no id→path index — O(N²) per scan.
- [ ] **Whole-catalogue deep copy on every single mutation.** 8 sites do
  `auto current = *current_snapshot();` (a full catalogue copy) then re-shard
  and re-encode all 64 shards for a single-item change.
- [ ] **`StagingArea::reserve()` runs a full recursive directory-size walk
  under its own mutex**, called every 500ms per active torrent and on every
  `GET /api/v1/ingest/status`.
- [ ] **`MediaInformationService::source_for()` linear-scans the entire
  filesystem snapshot** computing a media ID per entry, per lookup, on every
  publication and hint.
- [ ] **`admit_deferred()` is O(all inodes) with a per-inode lock, called
  after every publication quantum.**
- [ ] **`RetainedMemoryLedger::request_shedding_locked` linear-scans every
  live allocation under the global mutex** — exactly under memory pressure,
  which is the worst time to do it.

## P2 — Catalogue and media model

- [ ] Make negotiation representation-aware. One Macha work identity may
  reference several immutable files/assets, each with a persisted profile.
  Select the best Direct representation, then cheapest correct Remux/asset
  combination, then cheapest suitable Transcode while respecting quality,
  stream flags, availability, locality, seek cost and resource limits.
- [ ] Replace provider-shaped public IDs (`tmdb:*`) with opaque Macha work IDs.
  Keep provider IDs internal, preserve aliases during migration, and keep work
  identity separate from immutable content hashes.
- [ ] Update clients to consume immutable profiles and send a useful bandwidth
  ceiling plus a persistent logical-viewer/session identity.
- [ ] Make signed artwork capability URLs actually cacheable. `exp`/`sig` are
  recomputed fresh on every `/items`/`/items/{id}` catalogue call, so the same
  artwork object gets a different query string (and therefore a different full
  URL, which browsers key their cache on) every time — the existing 24h
  `Cache-Control: public, max-age=86400, immutable` on the artwork endpoint
  never gets consulted, and posters are re-fetched over the network on every
  page load. Fix by quantizing `exp` to a coarser bucket (e.g. top of the next
  hour/day) so `sig` becomes a pure function of `(artwork_id, quantized_exp)`;
  repeated fetches within that window then return an identical URL. No
  client-side change needed. Found 2026-09-04 via `macha-client-b8`.

## P2 — Diagnostics and repeatable proof

- [ ] Record reproducible local and four-node benchmark recipes without brittle
  default-suite wall-clock thresholds.
- [ ] Keep diagnostics bounded, snapshot-based and disabled by default when
  they perturb viewer behaviour; never instrument per packet or fragment on a
  critical thread merely to diagnose a P0.

## P2 — Code health and error-handling consistency (found 2026-09-05)

Not urgent, but real debt worth chipping away at opportunistically. No design
work needed for any of these.

- [ ] **Duplicated logic that can silently drift.** `fixed_vod_durations` is
  defined byte-identically twice (`media_engine.cpp` and
  `media_engine_common.cpp`); `fuse_frontend.cpp`'s `load_journal` reimplements
  `scan_fuse_journal_frames` inline instead of calling the shared function that
  exists specifically to be the single implementation of torn-tail semantics;
  `net.cpp`'s `PeerConnection` and `Session` are ~450-line near-duplicates;
  three near-identical `MediaInput` implementations exist in `playback.cpp`,
  `media_information.cpp` and `media_catalogue.cpp`; `catalogue_api.cpp` has
  private `json_escape`/`url_decode` duplicating `Json::dump()`/
  `http_url_decode()`.
- [ ] **Empty `catch (...) { }` blocks that discard exceptions with no
  log at all** — concentrated in `net.cpp` (29 as of 2026-09-08, up from 21)
  and `storage_pool.cpp` (10).
  The `storage_pool.cpp` cluster in particular (backend I/O failures in
  `has`/`remove`/`list`/`older_than`/`rebalance_step`/`scrub_step`) will make
  diagnosing a flaky physical disk very hard in the field.
- [ ] **Inconsistent error-signalling contracts.** `DistributedStore::put(span)`
  throws on quorum-unavailable while the sibling overload `put(id, span) -> bool`
  returns false for the identical condition; `LocalStore` mixes bool-for-no-space/
  throw-for-I/O-error/optional-for-absent/noexcept-swallow-everything across its
  own methods. `catalogue_api.cpp` catches `std::exception` and returns 503 for
  what are actually 400-shaped client errors (malformed JSON, unknown kind, bad
  artwork id, invalid `If-Match`) — contrast the correct 400/404/409 ladder in
  `manage_api.cpp`.
- [ ] **Confirmed-dead wiring, worth deleting or finishing, not leaving
  half-connected.** Re-verified 2026-09-08; two of the original five claims
  no longer hold and were dropped (`StorageLock` is used at `cluster.hpp:77`;
  the scanner's `request_media_profiles` is invoked from
  `catalogue_api.cpp:441` via `service.cpp:494`). Still true:
  `PlaybackManager`'s own copy of `request_media_profiles`
  (`playback.cpp:668`) is stored and never called; `status_api.cpp:479`
  hardcodes `metadata_replica = true` for every node regardless of actual
  role; three status fields (`ffmpeg_available`/`ffprobe_available`/
  `ffmpeg_version`, `playback.cpp:2388`) are hardcoded false/false/empty and
  marked "kept for one release" with no tracked removal date; two vestigial
  config knobs (`recovery_commit_workers`, `foreground_commit_workers`) are
  parsed, range-validated and logged but gate nothing.
- [ ] **Test-only hooks are live branches in the production hot publication
  loop.** `suspend_loader_for_tests` and `fail_publication_once_after_spool_bytes_for_tests`
  (`config.hpp`) are real conditionals compiled into the shipped binary, not
  behind a test-only build flag. Low risk today, but worth gating out of
  release builds since they're reachable via ordinary config.
- [ ] **`MANIFEST.sha256` is stale** — 104 hashes fail `shasum -c` as of
  2026-09-08 (90 when this was filed on 2026-09-05).
  Nothing in the build references it, so it currently just misinforms anyone
  who checks it. Either regenerate it as part of the release process or
  remove it.

## P2 — Documentation hygiene (found 2026-09-05, backlog-adjacent but not code)

- [ ] **Swagger/OpenAPI description of the HTTP API (operator request
  2026-09-07, optional).** Publish an OpenAPI 3 document for `/api/v1/*`
  (session, status, catalogue, playback, manage routes) and serve it from the
  daemon (e.g. `/api/v1/openapi.json` plus a Swagger UI page, or generate the
  document at build time from the route table so it cannot drift). The UI and
  site sessions consume the API by reading `status_api.cpp`/`service.cpp`
  today; a machine-readable contract would replace that. Nice-to-have, not
  on the import/stability critical path.

- [ ] `TODO/2026-09-03-playback-resilience-and-av-sync-plan.md` cites
  `TODO/2026-08-31-cluster-any-node-playback-failover.md` as tracking a
  client-side fix — that file does not exist anywhere in the repo. Either
  create it with the actual current tracking location or fix the reference.
- [ ] `TODO/COMPLETED.md` stops at 0.24.0 (last updated 2026-09-05). Thirty
  releases have shipped since — 0.24.1 through 0.35.0, including the whole
  self-healing programme (0.29.0–0.32.0), the subsystem-plugin foundation
  (0.25.0/0.28.0), the playback/streaming work (0.32.x–0.34.0) and SPA
  serving (0.35.0). The 2026-09-08 pruning pass ledgered the entries it
  removed from this file; the rest of that range is still unledgered.
- [ ] `docs/streaming.md`, `README.md`, `ROADMAP.md` and `VALIDATION.md` each
  contain claims contradicted by shipped code or by each other (keep-alive
  described as absent when it shipped in 0.23.3, a stale "0.19 metadata
  availability" README headline at 0.23.11, `ROADMAP.md` referring to "stale
  voters" from a voter model abolished in 0.19, and `VALIDATION.md`'s build
  command not matching `CONTRIBUTING.md`/`tests/TESTING.md`). None of these
  are load-bearing for engineering decisions right now, but they will mislead
  the next person who reads them at face value.
- [ ] `TODO/namespace-publication-and-metadata-efficiency.md`'s own checkboxes
  contradict its header ("Phases 0-5 ... complete" while Phase 4 shows all 16
  items unticked and Phase 3 shows 6 of 13 unticked, despite both phases having
  separate docs and a `COMPLETED.md` entry recording them as done). Reconcile
  the checkboxes with the ledger so this file stops contradicting itself.

## Deployment rule

- [ ] For every deployment, synchronise the complete source tree and all CMake
  inputs, configure after synchronisation, build nodes in parallel, and verify
  installed versions and byte-identical hashes on identical RPi hardware.

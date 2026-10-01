# Active tasks and concepts to explore

Last updated: 2026-09-30 evening, on `experiment/object-ledger-t2`. Both
nodes run 0.74.0 (T0's instrumented build); the T0 soak ends 2026-10-01
14:20Z.

This is the authoritative, ordered backlog. `COMPLETED.md` is the ledger of
finished work; `BACKLOG.md` holds the older, unverified P-1 to P2 sections;
`archive/` holds every past plan, UAT record, incident write-up, superseded
spec and handover (evidence, not requirements). Work top-to-bottom unless
new evidence changes the order.

**Start here after a clear: read [the handover](HANDOVER-2026-09-30.md),**
then the experiment section below, then the canonical spec and plan.

## First: the object ledger experiment

**This is the active work, and the only development stream.** `develop` is
frozen at `75e6f98` and is never modified by the experiment. On success the
whole experiment commit tree merges into `develop`; on failure development
resumes from `develop` and the experiment's version line ceases to exist.

- **Canonical spec**:
  [the object ledger and the component model](2026-09-29-object-ledger-and-components-spec.md).
  The most recent version is canonical; its three predecessors are in
  `archive/`, historical only. Its decision log records every operator
  decision, dated.
- **Plan**: [stage 0 implementation plan](2026-09-29-object-ledger-implementation-plan.md):
  T0 measure, T1 the instrument, P authoritative presence, T2 the thin
  slice (the claim walk through every layer), T3 the ledger, T4 the
  metadata contract, T5 component conversions into the composition root, S
  the final sweep.
- **Evidence** per step: [`object-ledger-evidence/`](object-ledger-evidence/)
  (`t0`, `t1`, `p`, `control-gate`, `t2`).
- **Where it stands** (branches are one local chain, none pushed, none
  merged into `experiment/object-ledger`, which is at `14634ba` on origin):

  | step | branch | tip | state |
  |---|---|---|---|
  | T0 measure | `experiment/object-ledger-t0` | `22082cd` | 0.74.0 deployed both nodes 07:05Z/07:13Z; soaking to 2026-10-01 14:20Z |
  | T1 instrument | `-t1` | `9ba3e09` | built; accepted with T0 |
  | P presence | `-p` | `afa3e72` | built (has() 4,129 -> 460 ns absent on fi-1) |
  | backlog fixes | `-fixes` | `759e75a` | two test defects fixed (HTTP pipelined reader; abandoned-pipeline lease) |
  | control gate | `-control-gate` | `01f2156` | question 1: control GC never uses an inventory built in the same pass |
  | `universal` | `-universal` | `b288352` | question 2: removed |
  | T2 slice | `-t2` | `eacda85` | T2a vocabulary, T2b NoIo capability, T2c wait guard + claim walk on the contracts + `Published<T>`, T2d component root + maintenance; assessment written; checked out |

- **T2 is built** (T2d: the component contract, the composition root,
  maintenance as its first component; lifecycle fixtures identical); the
  written assessment is `object-ledger-evidence/t2/assessment.md` (no kill
  criterion met; settle its point 3 before T3). Owed before acceptance: GCC
  build, the three suites and coverage on fi-1, after the soak. Then accept
  T0, merge the chain into `experiment/object-ledger` in order and push
  (standing authorisation, experiment branches only).
- **Rules for every step**: a branch cut from the previous step, merged
  when accepted, then pushed; 100% line and branch coverage of what the
  step builds or converts, mutation-proven; contracts as preconditions,
  postconditions and invariants; evidence committed under
  `object-ledger-evidence/<step>/`; decisions in the spec's decision log.
  Backlog fixes are steps on the experiment line (question 5). In-process
  testing until T5. Sanitizers are debuggers, not evidence. No CI.
  basemind for every code query.
- **The cluster** (gbni-1, fi-1) is the only Macha cluster and a disposable
  test cluster: avoid dropping the library, but not at the experiment's
  expense.
- **Open spec questions still waiting**: 6 (release over pinned roots) and 7
  (map-backed snapshots), both for after stage 0.

## Cluster state (2026-09-30)

- **gbni-1** (10.44.1.50, `macnessa.macha.network`) and **fi-1** (10.35.1.10,
  also .50 and .148) both run **0.74.0** (`680047e`, build stamp
  `0.74.0+unknown`: fi-1's tree has no git), **cluster protocol 22**.
  Metadata writable 2/2 against `metadata_min_write_replicas: 2`: restarting
  either node makes metadata read-only until it is back. Install backups
  `/root/macha-0.73.2-installed.tgz` and `/etc/macha/macha.yaml.bak-0.73.2`
  on both.
- **Observation** (T0): `/etc/macha/state/observation/observations.jsonl` on
  both nodes; `object-ledger-evidence/t0/observation_report.py` turns it
  into the kill criteria table.
- **The soak's load** (until 2026-10-01 14:20Z): root cron on each node runs
  `/root/claude-soak-round.sh` every 4 hours (fi-1 at :40, gbni-1 at :10,
  local time): three driven playback sessions (skipped while a real viewer
  plays) and 1 GiB written through FUSE to `/mnt/machamedia/claude-soak/`,
  the previous round's file removed. Log `/root/claude-soak.log`. The script
  exits after `SOAK_END`; the cron entries and the `claude-soak` directory
  are removed at the end of the soak. The operator queues torrents.
- **Git:** `develop` at `75e6f98`, two commits ahead of `origin/develop`,
  frozen. `main`/`origin/main` at `0e54e7e`. Tags stop at `0.71.0`
  (`b454c53`): 0.72.0 to 0.74.0 are untagged. Push to `develop` or `main`,
  and tag, only on the operator's word.
- **gbni-1 runs with glibc heap checking** (drop-in
  `/etc/systemd/system/macha.service.d/heap-check.conf`, a copy at
  `/root/heap-check.conf.keep` because `install-guarded.sh`'s rollback
  deletes it; cores to `/mnt/diskB/crash`) until the 2026-09-28 heap
  corruption's writer is found.
- **fi-1 is a full storage and acquisition node**: a 10 TB WD Elements USB
  drive at `/mnt/diskB` (fstab `nofail`), DATA backend `/mnt/diskB/data`
  (`limit: 8T`, `reserve_free: 64G`); scanner, ingest (staging
  `/mnt/diskB/ingest`, 500G) and torrent enabled. gbni-1 has the lower node
  id and stays the scan coordinator. The old 10G backend copy is at
  `/var/lib/macha/data.moved-20260929` (delete on the operator's word).
- **fi-1's drive has dropped off twice.** 2026-09-29 (USB link CRC errors;
  reseated onto bus 4). 2026-09-30: fi-1 rebooted 10:03Z (operator's work);
  at 10:27Z the kernel logged over-current on every USB port at once as
  the operator plugged in another USB device; the mount went to ext4
  `shutdown` with EIO. Repaired remotely 13:47-13:54Z on the operator's
  word (lazy umount, `e2fsck -f -p /dev/sdb1`, remount by UUID); Macha
  re-adopted the backend without a restart. Power (supply, hub) is the
  suspect, not the drive. A session monitor watches fi-1's kernel and Macha
  journal for USB, disk and backend events (re-arm it each session).
- **fi-1's eth0 negotiates 100 Mbit/s** on a gigabit port (cable or switch
  port, needs someone on site).
- **Repair settings on both nodes:** `idle_bandwidth_fraction: 0.9`;
  `repair_weight` 5 against `foreground_weight` 95 (defaults).
- **es-1 is offline until November 2026 at the earliest**; fi-1 is the build
  node. Data lost with es-1: many files dated 2026-08-31 and earlier have
  extents no node holds; the server still lists them as playable (item 4).
- **The operator's account for Claude**: `claude`, all roles, credentials
  on-box in `/root/.macha-claude-credentials` on both nodes. Never copy them
  off-box.

## The queue

**While the experiment runs, nothing below lands on `develop`.** A fix
becomes its own step on the experiment line, with its test, pushed when
accepted (operator, 2026-09-30, spec question 5). The pressing cases are the
test crash in item 12 and gbni-1's unexplained heap corruption, which keeps
glibc heap checking on. Items the experiment absorbs say so.

0. **Degraded operation is the normal case (operator, 2026-09-28).** "Macha
   needs to work properly degraded like this, as best it can, indefinitely."
   Review what still waits on absent replicas against that.

1. **Replication to fi-1: measure 0.73.1's pipelined pushes on an idle
   node.** Not yet seen: every measurement since the deploy had torrents or
   imports running on one node or both, so repair was on its 5% share (gbni-1:
   161 MB in the hour after 17:55Z, ~45 KB/s, 2,934 share refusals). Before
   0.73.1, with full credit and idle nodes, gbni-1 pushed ~500 KB/s, one
   object in flight. Open with it:
   - [ ] **`repair_weight`**: 5 against 95 today; 20 against 80 was offered.
     Operator to decide.
   - [ ] **Say why a node counts itself busy.** Status cannot show it: loader
     work on the node itself (a torrent publishing, an import) crosses no wire
     and is not in `nodes[].traffic`. Report the pacer's active classes
     (playback, mounted reads, loader, a peer's viewers).
   - [ ] **Log the resumed push position at INFO** at start. The 2026-09-29
     restart's resume (~1,100 objects) was inferred from the saved file and
     the counters, not proven.
   - [ ] **fi-1 pulls objects it cannot store.** With its backend offline it
     kept fetching from gbni-1 (1.3-2.7 MB/s of speculative traffic) and
     counted each as `unsourceable`. Pull only when a local backend can take
     the object; do not count a local failure as unsourceable.
   - [ ] **fi-1's `unsourceable` keeps rising** (64 by 16:51Z, still climbing
     after the disk returned; gbni-1 counts none). Find what those objects
     are: probably extents lost with es-1.
   - [ ] **Validated probes read every copy on the peer each pass**
     (`have_valid_objects`, the operator's choice over index-only). The cost
     scales with what the peer holds; the object ledger's diff-driven repair
     (a stage after stage 0) is the answer.
   - [ ] **A step's validation can run long on a busy HDD**, and the pacer's
     cooldown is 19x the turn, so steps become rare. Measure step length under
     load before changing anything.

0d. **The object ledger** moved to the top of this file as the active work.

2. **HTTP reads that may wait on a peer.** 0.73.2 moved the torrent listing
   onto the in-memory snapshot after stack traces showed `GET
   /api/v1/torrents/jobs` surveying peers' accepted heads (0.6-1.7 s on
   fi-1). Corrected 2026-09-30 (T2c, `object-ledger-evidence/t2/`): the four
   `catalogue_api.cpp` sites call `CatalogueManager::snapshot_view()`, which
   is memory-only when warm and reaches metadata only when cold; one
   `manage_api.cpp` site (730, identity-association reset for a node absent
   from live membership) calls `MetadataManager::snapshot_view()` directly.
   Every site now passes a control `WorkContext` naming its route, and the
   wait guard (record mode in production) logs any control path into a
   guarded operation. The suites drive none; the cold-catalogue case and the
   730 route need cases of their own before the list is complete. T4 splits
   `snapshot_view()` into `current()` and `converged()`. The fixes stay this
   item's.

3. **Unexplained on fi-1, 2026-09-29 ~12:00Z** (a viewer reported skips and
   pauses): a playback `PATCH` answered 503 after 19.9 s, a `DELETE` took
   11.7 s and creates 7-12 s, while fi-1 software-transcoded 1080p HEVC 10-bit
   at 83% of four cores and fetched extents from gbni-1 at 0.8-1.2 MB/s. The
   viewer recovered on its own. Also a `FUSE mount disappeared for three
   consecutive successful watchdog checks` ERROR at 16:32:23Z during a
   restart. Neither investigated.

0c. **Deleting unmatched files fails and logs nothing (operator, 2026-09-28).**
   A delete of unmatched files reported "3 of 4 files could not be
   deleted." (the client's wording) and the node logged no error. Every
   failed `DELETE /api/v1/manage/unmatched/{id}` must log its cause (code,
   path, media id, the filesystem error) so the next one is diagnosable from
   the journal. Then find why three of four failed: not yet reproduced or
   investigated.

0b. **Open from 2026-09-27, not yet fixed:**
   - **The web client gates its torrent page on the answering node's
     `/torrents/status`**: fixed in the web client (commit bd68eb7), not
     verified deployed. Now that fi-1 runs torrents the symptom is gone either
     way.
   - **60 of 296 movie posters are held by no online node.** All from items
     updated 2026-09-06..10; on es-1 or gbni-2, unknown until es-1 returns.
     Nothing re-fetches lost artwork. Written up, parked:
     [`archive/2026-09-27-missing-artwork-and-single-copy-writes.md`](archive/2026-09-27-missing-artwork-and-single-copy-writes.md).

4. **Per-file readability (designed, operator to choose where it goes).**
   Every extent of a file present on some reachable node: local
   `LocalStore::has()` (index), then one batched `have_objects` per peer;
   cache per `media_id`. Uses: a fact in `playback/media`, a clean refusal at
   session create, a manage report of damaged files by path. The first two are
   wire changes: announce to Core and every client first. The object ledger's
   `lost` query would answer it cluster-wide. Known unreadable titles listed
   as playable: The Martian `7b5743ad`, The Cannonball Run `11c474bb`, two
   from the web client's retry.
5. **A media-type context on torrent add** (operator, 2026-09-25). A `kind`
   of `movie`, `show`, `music` on `POST /api/v1/torrents/jobs` and on ingest
   submit, carried to the planner (`choose_destination` in
   `src/acquisition/ingest.cpp`), which places every file of the job by it.
   Why: Rome's extras became seven "movies" under `/Movies/`, and My Name Is
   Earl's episodes without `SxxEyy` became movies too. Send the exact shape
   to Core and every client before shipping; decide with the operator what
   happens to Rome's extras already placed.
6. **Decisions waiting on the operator** (do not act without them):
   - Metadata editor B (richer probe candidates): waits on the choice of
     fields (`TODO/archive/2026-09-28-metadata-editor-api-plan.md`). The operator
     carries editor changes to the clients himself.
   - 317 directories under `/Movies` and `/Music` on gbni-1 list empty
     (`/root/empty-dirs.txt` there): confirm which should hold a film.
   - Torrent staging option A vs B (stage 2 of the disk backend plan); stages
     3-4 of that plan.
   - A `CONTRIBUTING.md` checklist line: "A new gate may pace lower-class
     work; it may never stop it." Offered 2026-09-29.
   - Deleting fi-1's old backend copy `/var/lib/macha/data.moved-20260929`.
7. **Known defects found 2026-09-24/25, not yet fixed:**
   - `catalogue_api` still answers `503 catalogue_unavailable` for some client
     errors (artwork upload, `If-Match` parsing not re-checked); a parent cycle
     through `PUT`/`PATCH` is not checked.
   - Three transient conditions raise a bare `runtime_error` an ingest would
     fail on rather than block: `object replication quorum unavailable`
     (`src/cluster/distributed_store.cpp`), `metadata replica is still
     recovering` (`src/cluster/cluster.cpp`), `local metadata replica
     unavailable` (`src/filesystem/filesystem.cpp`). None seen failing an
     ingest yet.
   - From the acquisition API audit (`docs/acquisition.md`): the torrent and
     ingest `catalogue.state` rules disagree; a remote job's
     `catalogue.items` is empty; server-side sort of search results by
     seeders (clients own sorting); a pause or cancel landing while an ingest
     fails is overwritten.
   - Fedora and MacPorts package names in the install docs unverified.
8. **OpenAPI endpoint** (operator, 2026-09-24). Served by the API,
   switchable in config. There is no route table today; the agreed design is
   to generate the document from a declarative route table that dispatch
   actually runs from. Document the status and error codes with it.
9. **People on catalogue items -- directors, cast** (approved), with a
   required backfill: TMDB `append_to_response=credits` on the scanner's
   requests; store on `CatalogueItem`; expose in the API; a background,
   rate-limited, resumable pass over every item with a `tmdb` id and no
   credits. Announce to every client.
10. **Artwork:** a cold read is slow (1.1 s for 77 KB from gbni-1);
   `CatalogueManager::artwork` fetches the whole object before the first
   byte. Sized variants (`?w=300`) are a feature at the operator's priority.
11. **The deploy viewer check** (`build/claude-viewers.sh`) reads
   `playback/status` sessions and journal segment lines. A client polling a
   paused session still shows as a session with no segment lines; the
   operator's call each time.
12. **Test failures** (no known flakes -- each is P0 work):
    - **Rewritten 2026-10-01:** `rpc_cluster/test_three_node_cluster` (a
      scenario of thirteen claims waiting on background work) became ten
      claim tests that drive their steps (T2 README). Open: a joiner pulling
      its objects with no explicit step, and cache-to-store promotion, need
      the store's activity clock injected; the warm view's freshness
      mechanism at the metadata layer is not yet traced.
    - **Fixed 2026-10-01, watching for recurrence:**
      `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`
      segfaulted ~60 ms in, ~1.7% at `--jobs 12` (also 5/500 on `c53efd7`).
      ASan: null `this` in `DistributedStore::repair_diagnostics()` via
      `Service::repair_diagnostics()`, which read `store_` before the
      asynchronous service start had built it. The accessor now calls
      `wait_services_ready()` like its siblings (it lost `const`). 5/5 after
      the fix (operator: five runs, then watch). Any recurrence reopens it.
    - The same test fails its pacing check (`share_after > share_before`,
      `tests/test_rpc_cluster.cpp:968`) 2/100 with
      `MACHA_TEST_LOG_LEVEL=DEBUG`, 0/300 without. Likely timing under slow
      logging; not proven.
    - **P0:** `filesystem_fuse/test_fuse_recovery_thousand_operations_have_bounded_publications`
      segfaulted once in the full suite on fi-1 (2026-09-28). Not reproduced
      since: 200/200 laptop, 200/200 fi-1 (2026-09-30). Next: ASan on fi-1
      under full-suite load, after the soak.
    - Fixed on `experiment/object-ledger-fixes`:
      `media_playback/test_abandoned_transcode_pipeline_is_reclaimed_before_session`
      (a 50 ms idle lease renewed by 20 ms wall-clock sleeps; `759e75a`) and
      the HTTP test reader that kept a second pipelined response in the first
      body (`c02f913`).
    - `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt`
      (a truncated object reported present), full suite only, not
      reproduced since. Step P (authoritative presence) re-checks existence
      under the object lock in the warm-up scan and publishes presence only
      after a put completes; the original route is not proven closed.
    - `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
      twice in full macOS runs, 0/58 isolated. It reaches its state in real
      time (`object-ledger-evidence/t1/q-real-time-maintenance-tests.md`).
    - `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`
      and `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection`,
      once each on fi-1 2026-09-24, 10/10 alone.
    - The macOS-only torrent segfaults (5 of 21 in `macha-tests-torrent`).
13. **Transport backoff after a peer restart**: an inbound session does not
    clear the dial backoff for the peer's other lanes, so a node refuses to
    dial a restarted peer for up to 4 s. Costs latency, not correctness.
14. **Movie sets** (operator, 2026-09-25, "later"): many-to-many membership;
    not designed; needs a proposal first.
15. **Ebooks** (operator, 2026-09-25, "later"):
    [docs/macha-ebooks-proposal.md](../docs/macha-ebooks-proposal.md), to be
    answered with what the server would need; nothing to build yet.

Then the older ordered items below. Reconciled against 0.57.0 on 2026-09-24:
what was found already done is ledgered in `COMPLETED.md` under "backlog
reconciliation", and items only partly done now state what remains. The
P-1/P0/P1/P2 sections that follow date from 2026-09-05 to 2026-09-22 and have
not been re-verified against 0.73.2 item by item.

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
- **Gate a gdb attach on the viewer check**, not just run the check.
- **Every response has a snake_case status code; errors add a message beside
  it; clients own sorting and presentation; every API change is announced to
  Core and every client** (operator, 2026-09-24).

**Read this before trusting anything below about a client.** Four client
sessions spent 2026-09-21 testing 0.48.0 against the live cluster and
reported sixteen findings. Six were real and are recorded; the rest were
retracted, several of them client self-diagnoses that did not survive
measurement. **A client's account of itself is evidence about the client, not
a fact.**

-3. **P0: a reconciliation merge rolls committed ingest checkpoints back, and
   the ingest dies on it -- `ingest failed: concurrent file content change`
   (diagnosed 2026-09-23, NOT FIXED).**

   On es-1, three torrent-sourced ingest jobs died at 12:51:12Z, 12:51:47Z and
   12:52:07Z. What the namespace holds now versus what es-1 had committed and
   had confirmed `stored=yes` on a replica:

   | `.part` | on the mount (both nodes) | last committed by es-1 |
   |---|---|---|
   | Gremlins | 335,544,320 | 469,762,048 (12:50:57Z) |
   | Voyager S04E04 | 402,653,184 | 469,762,048 (12:52:00Z) |
   | Matrix Revolutions | 536,870,912 | 603,979,776 (12:51:38Z) |

   One to two 64 MB checkpoints gone from each. Each failure lands within two
   seconds of a `metadata histories reconciled ... conflicts=1 superseded=1`
   line; the last one in the same second.

   **Mechanism.** `src/metadata/metadata.cpp:2040-2046`: on a genuine three-way
   conflict the merge installs the **common-ancestor value** at the path and
   records a conflict ("Keep the common-ancestor value visible until explicit
   resolution"). The ingest's open `WriteHandle` still carries its last
   committed basis; the entry now has an older size and older extents under a
   newer version; `commit_file`'s basis check (`src/filesystem/filesystem.cpp:2294`)
   fails EAGAIN; `IngestManager::process_job` marks the job `failed`
   (`src/acquisition/ingest.cpp:1221`). No bytes are lost -- extents are on disk and
   `copy_file` resumes from the namespace size -- but the job is dead and a
   retry re-copies 64-128 MB.

   **Why a path only es-1 ever writes conflicts at all.** gbni-1's history for
   gens 36465-36499 is a sibling-merge storm: ~20 merges in 25 records, with
   two or three records at the *same* generation (36470 x3, 36477 x3, 36480 x3,
   36484 x3). All three nodes reconcile, each sorts the accepted heads by hash
   from its own view and folds `heads[0]`/`heads[1]`
   (`src/metadata/metadata_manager.cpp:1248-1251`), so with three or more heads they
   merge *different pairs* and manufacture sibling merges of each other's
   merges. In that braid `history_common_ancestor` (which does follow
   `merge_parents`) lands well below both heads, so es-1's checkpoint N (left)
   and the other head's copy of es-1's earlier checkpoint N-1 (right, absorbed
   via a different fold) *both* differ from a base at N-2 -> conflict -> N-2
   installed. That is the one-to-two-checkpoint regression measured.

   **It is a tree-cutover regression.** Reconciliations on es-1: 4 on
   2026-09-21, **131** on 2026-09-22, 20 by noon on 2026-09-23. Tree-backed
   merges are full records (17-34 KB, 300-400 ms) and six concurrent imports
   each committing every ~7 s collide constantly. The cutover note in -1
   already lists "a tree-native merge" for Stage F.

   **Not proven:** which path each `conflicts=1` was on. No log line names the
   conflict key, and es-1's history had compacted past the window before the
   record could be materialised. Sizes, timing and the DAG leave no other
   consistent explanation, but that last step is inference.

   **Plugging the hole, three parts, none done:**
   - [ ] **The merge rule.** When one side's entry is causally the other's
     ancestor -- per-path `version` is monotonic and right's value equals an
     earlier state of left's -- take the newer instead of declaring a
     conflict. Today ancestry seen through a stale LCA is indistinguishable
     from divergence.
   - [ ] **The ingest.** On EAGAIN from a checkpoint commit, re-base on the
     current entry and continue from its size rather than failing the job.
     Same class as the restart-mid-put failure below.
   - [ ] **The reconciler.** One merger at a time, or a deterministic pair
     choice that every node makes identically, so three heads cannot fan out
     into sibling merges. This is also where the operator's point lands:
     **publications are supposed to batch and to throttle, and did before the
     tree.** Six jobs x 64 MB checkpoints is the collision source. The only
     throttle found so far is `fuse.publication_quiet_ms`, which gates on
     *foreground viewer* activity alone and so cannot see an import; no
     coalescing layer exists in `MetadataManager`. The operator knows what
     used to batch; ask before guessing.
   - [ ] Log the conflict key when a merge records one, so the next instance
     is provable from the journal alone.

   **Also open from the same afternoon** (and **not** the durability-barrier
   defect in item -4, checked on 2026-09-23: this message comes from
   `filesystem.cpp:1215`, the *write* quorum in `put_impl` failing to reach
   `min_write_replicas`, not from the barrier, whose failure says "object
   durability quorum unavailable before publication" instead): an ingest job
   dies permanently when its node restarts mid-put -- `state='failed' error='object replication
   quorum unavailable'` -- rather than pausing and resuming. It survived three
   restarts today and failed on the fourth, so it is timing-dependent. Same
   class as the read-only-window fix that shipped in 0.57.0, which blocks and
   retries on `MetadataNotReady` only; this EIO from the object write quorum
   is not covered by it. The job is retried from the UI; completed files are
   skipped.

-2. **What the disk resource audit left open, and the batching question
   (2026-09-23).** The audit itself is closed (`COMPLETED.md`, 0.53.0); these
   are the parts it recorded rather than fixed:
   - [ ] **One `DiskServiceMonitor` covers a whole `StoragePool`, not one
     device.** A pool may hold several backends on several devices and the
     monitor cannot tell them apart, so one slow backend pressures work bound
     anywhere in the pool. Harmless today -- gbni-1 and es-1 configure exactly
     one DATA backend each -- and it bites the moment a second is configured.
     Per-device pressure also needs the arbiter to know an operation's
     destination device, which it cannot: admission happens before placement.
   - [ ] **Local disk maintenance is budgeted from a network measurement.**
     `estimated_network_bps()` feeds `local_credit` as well as
     `network_credit` (`src/service/service.cpp` maintenance loop), which is how a
     GC/repair pass once took 51.6 MB/s of one spindle. The loader clock stops
     maintenance during an import, which was the case that hurt, but the
     budget still means nothing on a node with one spindle. No number is
     proposed on purpose.
   - [ ] **The 150-300% hysteresis band is a latch.** A device that settles
     anywhere between stays pressured, and the EWMA does not decay without
     traffic. The trickle drains so it does not wedge, but "pressure always
     releases on a device that recovers" is only true past 150%.
   - Law 1 holds **by configuration, not by construction**: control on
     `nvme0n1p2` and DATA on `sdb1` on every node, but nothing stops
     `metadata_store.path` being pointed at a DATA spindle, and the FUSE spool
     and ingest staging already sit on the DATA spindle as plain file I/O the
     monitor never sees.
   - [ ] The ingest's own ceiling is now the spindle, not CPU: it reads its
     staging copy from and writes its extents to the same disk
     (`/mnt/diskB/ingest` and `/mnt/diskB`), measured at 34-49% util during a
     13 MB/s import.

-1. **What the cutover cost on 2026-09-22, in order of how close it came.**
   The cluster was re-rooted at 12:41Z. By 18:00Z four things had surfaced
   that no test had, all recorded here so the next cutover of anything is
   planned against them:
   - **The tree was collectable.** The control-store live set walked
     catalogue roots and nothing else; every tree node was an unreferenced
     object to GC. Only a 30-day `garbage_grace_ms` set as a migration safety
     net stood between the cluster and collecting the nodes that say where
     every file lives. Fixed in 0.51.0 (`collect_namespace_tree_nodes` in the
     release live set, `collect_namespace_tree_changes` for claims).
     **The grace stays at 30 days until 0.51.0 has run on all three nodes for
     a day**, then goes back to 24 h.
   - **Ingest crawled at 1.8 MB/s on an idle node.** A commit re-chunks the
     spine and replicated all ~12 nodes it touched synchronously to peers 60 ms
     away, eleven of them byte-identical to what was stored. Fixed in 0.51.0:
     a node already present is not re-replicated. Per-commit round trips ~12
     to 1-3. **Measured 2026-09-23 on gbni-1 after 0.53.0/0.53.1: 13 MB/s
     aggregate across three concurrent imports** (810 MB in 60 s), `sdb` at
     34-49%, `macha-maint` at 0. The same defect -- re-sending what the peer
     already holds -- turned out to be alive on the *retention* path too and
     was the cause of the retention-floor ingest failures; fixed in 0.53.1.
   - **A sixteen-minute stall with the wrong message on it.** Two heads at one
     generation, no commits until reconciliation merged them, and every node
     logging "delta replay ... does not reproduce the record hash" -- which
     `diagnose_unreconstructable_locked` emits without ever replaying. The
     chain replays byte-exactly (`macha-metadata-dump --objects`). The message
     now says what it checked. **The stall itself is still open**: tree-backed
     reconciliation materialises both branches and publishes a full record,
     and the cost of that on a live branch is unmeasured. Item for Stage F:
     a tree-native merge.
   - **The DATA pressure gate was wrong twice before it was right** (0.51.0,
     corrected in 0.52.0). First it compared every operation against a flat
     50 ms, so a 4 MiB extent write -- 100-200 ms on a healthy spinner -- read
     as pressure: gbni-1 declared itself pressured nine seconds after boot and
     held the import to one background lease for an afternoon. Now pressure is
     the moving average of actual against expected *for the operation's size*
     (25 ms + 120 ms/MiB, pressured above 300%, released below 150%), plus an
     absolute 2 s outlier trip. The outlier exists because the tests showed the
     ratio alone would have let the founding 17.7 s write through: against
     fifty healthy samples it moves the average to 237%, under the 300% line.
     0.51.0 also flattened law 3 by making the loader yield to pressure with no
     viewer present; it now yields only when a viewer is waiting or holding
     credit. **Superseded by the 0.53.0 audit** (`COMPLETED.md`): the read
     path was still measuring every read as zero bytes, the outlier is now a
     ratio (`io_pressure_outlier_percent`, 1000), and the torrent clamp gained
     law 3's second clause. Still untested against a genuinely pathological
     device.
   - **Every catalogue route returned 503 on every node and all clients
     reported "no API"** (0.51.0, fixed in 0.52.0). `catalogue.cpp` committed a
     resolved conflict through the non-exact `mutate()` path, which the
     tree-backed guard refuses. It was the last caller on that path and the
     line had been spotted hours earlier without being fixed. Health and auth
     answered throughout, so the app loaded empty -- worth remembering as a
     failure shape: *the server looks fine from every probe except the one the
     client actually needs.*

   **Client asks from the placement API round (2026-09-22), both sessions.**
   Verified against the code before writing down; two need nothing:
   - Per-node free disk: already on `/api/v1/status` as
     `nodes[].storage.free_bytes`. Core had not read it.
   - Node id format: `unhex` takes even-length unseparated hex, either case.
   - [ ] Structured `placement_failed`: the code shipped in 0.56.0
     (`error.reason`: `node_not_member`, `node_refused`, `node_unreachable`,
     `node_did_not_start`, `missing_uri`, `add_failed` or the peer's own
     code). Still open: the target `node_id` as a field, and the viewer-facing
     `detail` (Core) -- 0.56.0 has no `error.detail`; weigh it against the
     codes-are-primary rule. A host must never parse a message.
   - [ ] Surface the peer's own refusal text on a forwarded add. fi-1 has
     `torrent: enabled: false`, so a placement there is a correct 409 -- but
     the message says "refused the request" where the peer said "torrents not
     available on this node".
   - [ ] Optional `name` per `nodes[]` entry on `/api/v1/status` (web client).
     Needs a config knob and a `NodeTelemetry` field to travel; `host` is the
     RPC bind address and is inconsistent on this cluster (two DNS names, one
     machine name). **Operator's call** -- the operator chooses the names.
   - Both sessions independently declined a server-side "best node" and a
     name as the placement key. Core's reason stands: they already rank
     endpoints and can say which axis decided; a second ranking on different
     inputs would make placement unexplainable. Not doing it.

0. **The namespace Merkle work**, the P-1 below. **Stages B, C and E have
   shipped** (ledgered in `COMPLETED.md`): the SM14 record in 0.49.0, the
   commit path and every namespace reader and writer on the tree in 0.49.1 and
   0.50.0, and the migration tool in 0.50.0 -- the live cluster was re-rooted
   onto the tree at 12:41Z on 2026-09-22 (0.50.1: a 22.79 MiB record became
   3.97 KiB). What the cutover cost is item -1. **What remains is Stage D**
   (demand-loaded extent nodes on the `RetainedMemoryLedger`; persist
   `file_media_id`) **and Stage F** (the dependent O(N) items under P1 scaling
   cliffs, plus the tree-native merge that items -1 and -3 call for:
   reconciliation still materialises both branches and publishes a full
   record). See [the plan](archive/2026-09-17-namespace-merkle-root-plan.md), which
   carries the arithmetic that justified the work: 56 ms of CPU per namespace
   write before the tree, ~3.1 s at the 100 TB target.
1. **The other P-1, the cache-sizing invariant.** Still open, and note that
   the block-cache item under it was falsified and downgraded on 2026-09-21 —
   the cache works, it just could not be observed, and now can be.
2. **The metadata-stall P0.** Its read-only blink was root-caused and fixed on
   2026-09-21 (a hung health probe was given the whole liveness budget); the
   stall that provokes it is still unexplained. Making an ingest survive a
   read-only window shipped in 0.57.0 (`MetadataNotReady` blocks and retries
   instead of failing the job).
3. The **rejoin/cache P0** after it — worked around on all three nodes on
   2026-09-20, not fixed, and the concrete instance of the first P-1.
4. The **loader-I/O P0**. The node starves its own viewer I/O with loader
   work: one ingest took es-1 to 91% iowait and aborted twelve client requests
   at ~8 s. Governing law 2 is violated on the DATA backend, and no
   configuration available prevents it. **Its reproduction is blocked** — read
   that item's first bullet before attempting one.
5. The **P0 cluster section**. The live cluster is **three** nodes as of
   2026-09-20 evening, **all on 0.47.0** and converged at generation 31663:
   es-1, fi-1 and gbni-1. gbni-2 is defunct and the operator expects it to stay
   that way for some months (2026-09-20) — do not include it in a deploy, do
   not wait for it, and do not treat its absence as an incident. Removing a
   node is something the system does not really support, and that is the first
   item there.
6. **"What the client sessions now depend on"** near the end of this file.
   These are API contracts settled in conversation with the four client
   sessions and they exist nowhere else in this repository. Breaking one breaks
   clients that cannot be fixed from here.
7. **"Cluster state"** at the top of this file, which records node
   addresses, what is deployed, and where the branches and tags stand (the
   older snapshots are archived in `COMPLETED.md`).

None of the last three is a task list; all of them will mislead you if you assume
otherwise.

**Four suite failures were diagnosed and fixed on 2026-09-15 (0.43.0)**,
each to a written verdict in the deterministic-suite plan, step 3: the
ingest case was a product defect (two concurrent imports both creating the
shared scanner root, the loser's `EEXIST` failing its job); the divergence
case a test defect (two Services' maintenance loops reconciling the
divergence the test had just created); the edge-node placement case a test
defect (observers compared before their capacity views had converged); and
`test_three_node_cluster` a real transport deadlock, caught with gdb -- a
writer loop exiting on a broken connection left queued notifications
unanswered, and a metadata announcement waiting on one held the mutation
mutex forever -- plus a test race against asynchronous promotion. Rates
before and after, on es-1, are in the plan. The operator's rule, stated the
same day: a failing test on a node is P0 work, not a footnote in a deploy
report.

**OpenAPI is the largest piece of agreed but unstarted work, and is now P1**
(it sat at P2 until 2026-09-20, contradicting this very sentence): the operator
asked for it on 2026-09-07 and upgraded it to "soon" on 2026-09-13. Generate it
from the route table at build time so it cannot drift.

2026-09-13 (evening): rationalisation pass for a new session. Twenty-three
completed items were moved to `COMPLETED.md` in full rather than summarised —
in several cases the reasoning *is* the record: a retraction, an option the
operator declined, a measurement that disproved the thing it was taken to
support. Before moving them, open remainders buried inside completed items were
promoted to entries of their own rather than carried along or lost: the
unmeasured iOS half of the bounded-VOD work, torrent session health not
reaching the HTTP API, the TSan run nobody has made, and the web client holding
a bearer token in JS-reachable storage. One completed item was kept in place as
a numbered stub so an ordered list still reads. Nothing was deleted.

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

The governing laws are stated, with their numbering, in
[Principles and laws](../docs/principles-and-laws.md):

1. Thou Shalt Not Make Control Wait.
2. Thou Shalt Not Make The Viewer Wait.
3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.
4. Thou Shalt Not Shoot Thyself In The Foot.

## The older backlog

The P-1 to P2 sections written 2026-09-05..22 are in
[`BACKLOG.md`](BACKLOG.md), unverified against 0.73.2 item by item. The
queue above outranks them.

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

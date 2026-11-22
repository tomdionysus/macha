# Active tasks

Last updated: 2026-11-22 about 17:45Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note.** The version before this rationalisation is
`archive/2026-11-22-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md` (not ordered),
`2026-11-08-found-issues.md` (Macha's defects), `COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not
requirements). Commit hashes written in this directory before 2026-11-20 18:10Z name commits from before that day's
two history rewrites; the maintainer's backups hold the maps.

## Where it stands

- **The port is live.** Both live nodes, live-a and live-b, serve **Macha 0.95.2 on Dagda 0.3.3 and Dannan 0.9.2**
  (live-a since 2026-11-22 12:23Z, live-b since 14:28Z). The store cutover ran 2026-11-20 to 21 on 0.95.1; its record
  is `archive/2026-11-18-cutover-runbook.md`, "Done".
- **The post-cutover OOM is fixed.** live-a was killed twice (2026-11-21 22:21Z and 2026-11-22 01:08Z) by repair:
  Dagda 0.3.2's network estimate misread to 15 to 20 GiB/s, repair's burst (5 s of the estimate) read every planned
  object before sending, and RSS reached 4.7 to 5.6 GB on an 8 GB Pi. Dagda 0.3.3 fixes the estimate's sampling and
  bounds the push window; Macha 0.95.2 sets that window to an eighth of the retained-memory budget (96 MiB). On it,
  live-a held 0.97 to 1.20 GB through 76 GB of repair, the estimate steady at 10 to 12 MiB/s. The stopgaps
  (`maintenance.max_bandwidth: 16M`, a glibc `MALLOC_MMAP_THRESHOLD_` drop-in) are removed from both nodes.
- **The branch:** `port/dannan-dagda`, 0.95.3 (`3f28e08`: the import submits each hint once; the genesis password
  file durable), pushed, untagged, not merged; the live nodes run 0.95.2. `develop` and `main`
  are 0.94.2 and older; merges, tags and the release go on the maintainer's word.

## Now, in order

1. **Close out the OOM.** Watch live-a through an estimate misread on 0.95.2 (Dagda's line: RSS above about 1.3 GB, or
   the estimate more than quadrupling in one step, is a finding to send Dagda). Then stop the samplers on live-a
   (`/root/oom-watch/sampler.pid`; Dagda's `arena.pid` is Dagda's to stop) and report.
2. **Post-cutover** (the maintainer, 2026-11-22: check then GC; deploy 0.95.3; stop the samplers; clear the kits):
   - done 2026-11-22: 0.95.3 on both nodes (live-a 18:17Z, live-b 18:18Z; Status shows each on Dannan 0.9.2,
     Dagda 0.3.3); samplers stopped, logs in `build/evidence/oom-2026-11-22/`; the cutover kits removed (custody
     copies and `pre-0.95.3` kept);
   - the cutover check on live-a, read-only as root on-box (`build/cutover/diag-keys.py`): the namespace's
     1,624,424 extents all held by a reachable node, 0 unavailable, 0 unknown; repair 0 unsourceable, 0 unreadable.
     `collect_garbage: true` on live-a from 18:27Z (a restart: a SIGHUP does not reload maintenance keys).
     live-b not checked through its own API (signing in there from live-a was refused); the maintainer turned its
     GC on anyway (same replicated namespace), from 21:32Z;
   - 0.95.4 on both nodes from 21:31Z (live-a) and 21:32Z (live-b): Status shows each node's `control_storage`
     (live-a 4.74 of 8.59 GB, live-b 4.48) and the nearly-full condition; a failed catalogue install backs off;
   - the baseline comparison against `2026-11-06-pi-baselines.md` (state each node's replication state with it);
   - the `catalogue_root` merge conflict: superseded by the next catalogue root, or the maintainer resolves it at
     `/api/v1/manage/metadata/conflicts`;
   - the cutover check, then `maintenance.collect_garbage` (false on both until then);
   - clearing the nodes' kits (below), only what the kits installed.
3. **The release** (`project_release_procedure`): the pre-merge gate on a Pi (`tools/ci.sh --local` with ASan, and the
   coverage build, which gives Macha's first whole-tree figure); squash the port onto `develop`; GitHub CI green on
   `develop` (red since 2026-11-13 on the ingest/torrent ASan crash, item 4 below); debs; tag; merge to `main`.
4. **P0 failures, never tolerated as flakes.** 2026-11-22, on 0.95.2 with Dannan 0.9.2 and Dagda 0.3.3 both
   instrumented: the whole suite passes under ASan (556/556), the ingest/torrent case that keeps `develop` red
   among them.
   - The status race was the test's (`test_status_renders_each_node_from_what_it_knows`, 22 of 30 under ASan):
     fixed in `d7929ad`, 60 of 60 since.
   - The catalogue SIGSEGV (`test_catalogue_sync_search_and_artwork_gc`) not reproduced: 90 of 90 under ASan,
     and 7 full plain runs under `--jobs 3` clean for it.
   - **Heap corruption, reproduced under load (plain build, 0.95.4, `--jobs 3`):** the burst-rmdir case
     (`test_removing_empty_directories_in_a_burst_keeps_the_node_up`) SIGSEGV in 2 of 7 full runs, and
     `test_publication_writer_failure_stays_invisible` aborted once with `malloc(): unaligned tcache chunk
     detected` inside `dannan::net::RequestScheduler::stop` at the test node's stop (core read; evidence in
     `build/evidence/heap-2026-11-22/`). Glibc caught it late, so the stop is where it surfaced, not the cause.
     Alone, under ASan, both pass. The ASan suite under `--jobs 2` (3 runs) never failed those two; run 3 failed
     the ingest/torrent case on ASan's own thread-registry CHECK (as on `develop`'s CI): ASan's bookkeeping
     overwritten, which uninstrumented code (libtorrent) could do, but the plain crashes run with torrent off.
     A race under load fits both; next, TSan with Dannan and Dagda instrumented (running on build-3,
     `build/pi/macha_sanitize.sh`, PASSES=tsan).
   - Live: found-issues 1 and 2 (heap corruption, the FUSE `rm` crash) are still open.
5. **Repin onto Dannan 0.10.1 and Dagda 0.4.0** (0.4.0 carries 0.3.3's repair fix): read each release's notes and
   changelog, drop workarounds, use new APIs, record it in the repin commit.
6. **Report the Dannan and Dagda versions** in Status and the clients (`BACKLOG.md`, "Status and clients"); an API
   change, announced to Core and every client first.
7. **GoogleTest:** Macha's `MACHA_TEST` cases move on their own branch, one process per case, the count matching
   before and after; GoogleTest from Dannan's `dannan::testing` export.
8. **The stack audits** (`2026-11-20-duplication-audit.md`, `2026-11-20-no-special-node-audit.md`): the converged
   placement plan is the coordinating session's; negotiation goes through it.
9. **The deferred API intents** (`2026-11-18-port-api-announcement.md`): Dannan's and Dagda's native forms, once
   fidelity is shown and the maintainer says.
10. **Macha's defects**, `2026-11-08-found-issues.md` in its order: a full control store is silent (3; Status
    reports no control-store use, and with `collect_garbage` false nothing is reclaimed: 4,544 MiB of 8 GiB on
    live-a, 4,298 on live-b, 2026-11-22, growth logged in `build/cutover/control-store.log`), import filing (9),
    then the rest; and from the old list: the stop not checked by the release horizon's census or the tree diff
    under `census_follow`; books as the web client reported at 0.92.2. Done in 0.95.3: cancelling an ingest (4) and
    the genesis password file's fsync.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption; a Macha restart on either live node.

## Waiting on the maintainer

- The word for each item of "Post-cutover" and for the release.
- Where the Dagda 0.3.3 debs go (built by Dagda, local only).
- Carried, unchecked: the media-kind import hint; truncated pack records; the `*.pre-migration.*` archives; the
  replica journal reader with no writer left; whether the heap chase resumes with media load.

## The live nodes (read 2026-11-22)

- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a, API 192.0.2.50:7438, advertises `live-a.example.net`,
  `bootstrap: []`) and **live-b** (root@live-b.example.net, Pi 5, 16 GB, site-b, API 203.0.113.50:7438 inside only,
  inbound at :7437, bootstrap live-a). Never build on either. DATA `/mnt/diskB/data`; cores to `/mnt/diskB/crash`.
- Install under `/usr/local`: Macha, `libdannan.so.0.9.2`, `libdagda.so.0.3.3`, `dannan-custody`,
  `dannan-import-macha`, `dagda-migrate`. Layout v19. Custody sealed by systemd-creds (`dannan-root`, drop-in
  `custody.conf`); `custody: credential` in `macha.yaml`.
- Config: `maintenance.idle_bandwidth_fraction` 0.5, `max_bandwidth` 0B, `collect_garbage` false; dht replicas 2,
  write_copies 1, metadata_write_copies 1, extent 4M; control store limit 8G; heartbeat 5 s, `telemetry_interval_ms`
  10000. Observations once a minute in `/var/lib/macha/observation/observations.jsonl` (`rss_bytes`,
  `repair_estimated_network_bps`, `repair_bytes_transferred`).
- Kits left in `/root` (cleared on the maintainer's word): `stage-0.95.1`, `stage-0.95.2`, `pre-0.95.1` and
  `pre-0.95.2` (rollback binaries and `macha.yaml`), `backup-0.95.1`, `migrate-0.95.1`, `custody-0.95.0` or
  `custody-0.95.1`, live-a's `phys` and `oom-watch`; `macha.yaml.pre-max-bandwidth` beside the config. Rolling 0.95.2
  back to 0.95.1 needs no migration: `pre-0.95.2/bin.tgz`, then `ldconfig`.
- **Access:** the API needs a session; scripts in `build/baselines/`. Sync a tree to a Pi with `rsync -rlp --checksum
  --exclude '/build*' --exclude /.git --exclude CLAUDE.md --exclude CLAUDE.local.md`; no `-t`, no `--delete`.

## Builds

The shared build Pis: build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202`, build-4 `.228`; Pi 5, 16 GB,
Debian 13 arm64, shared by every session. No builds on the Mac. Claim per step on the claim board, only the cores the
step uses (1 for a test binary or packaging, N for a -jN compile), release when it ends; `claim-board wait` in the
background when none are free. Development packages may be installed (1 core claimed, nice 19, a board note);
remove only what an install manifest lists.

build-3 holds Macha's tree `/home/maintainer/macha-store-2` (`build-port`, 0.95.2, suite 556/556) and the prefixes in
`/home/maintainer/deps` (`prefix-0.9.2`, `prefix-dagda-0.3.3`, older ones). Scripts in `build/pi/`:
`macha_port_compile.sh`, `macha_package.sh` (`V`, `DAGDA`), `macha_sanitize.sh` (instrumented Dannan and Dagda).
Tests: `build-port/macha-tests --jobs 3`, or `--serial [--verbose] --filter NAME`; tee every result into a log on the
Pi. Temporary diagnostics go on the Pi's copy only.

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou Shalt Not Make Control Wait. 2. Thou
  Shalt Not Make The Viewer Wait. 3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer
  Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Everything the API and the web serve is viewer work**, including the bytes still draining to a client.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.** Absent nodes never block.
- **No vacuous truths.** An empty set never satisfies "all" or "enough".
- **Three answers, HTTP numbers below as above**; refusals carry an HTTP status and a snake_case code; nothing
  matches English text.
- **Proportion.** Account for and control what matters to the user's experience.
- **Fix forward, each mechanism proved before a change.** A Dannan or Dagda cause goes to its session with the
  mechanism; a library's release is that library's and the maintainer's. A Macha defect is fixed in Macha first.
- **Sanitizers** for a release or to hunt a named bug. **Testing**: once on a Pi; repeats only to measure a flake.
- **Coverage about 100%**: every change brings tests reaching its lines; measured at a branch's pre-merge gate.
- **Report findings before fixing.** Questions go to the maintainer through the question dialog, recommendation first.
- **Every response has a snake_case code; clients own sorting and presentation; every API change is announced to Core
  and every client before it ships.**
- **One definition per concept**: a type Dannan or Dagda defines is aliased, never twinned. **Agreement is Dannan's.**
- **Builds** only on the Pis, claimed per step, nice 19. Format with clang-format 19; clang-tidy reports nothing.
- **Git**: push `port/dannan-dagda` freely, anything else on his word; no tag, merge or PR unasked; stage named paths;
  no attribution trailers; no em dashes; versions in full x.y.z; placeholders only for hosts, addresses and people.
- **Text tools**: code lookups through basemind or Read; scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

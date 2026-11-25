# Active tasks

Last updated: 2026-11-24 about 19:00Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note.** The version before this rationalisation is
`archive/2026-11-24-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md` (not ordered),
`2026-11-08-found-issues.md` (Macha's defects), `COMPLETED.md`, `CLIENT-CONTRACTS.md`, the dated plans, `archive/`
(a record, not requirements). Commit hashes written here before 2026-11-20 18:10Z name commits from before that day's
history rewrites; the maintainer's backups hold the maps.

## Where it stands

- **Three live nodes, all on Macha 0.96.2 with Dannan 0.11.1 and Dagda 0.4.0**, healthy, 3 of 3 online and storing,
  28 TiB, two copies on two different sites (live-a site-a, live-b site-b, live-c site-c). Details below.
- **The branch:** `port/dannan-dagda` at 0.96.2 (`6ef66e5f`) plus TODO commits, pushed, untagged, not merged.
  `develop` and `main` are 0.94.2 and older; merges, tags and the release go on the maintainer's word.
- **live-c is filling:** rebalancing onto it at about 3 MiB/s over the internet (senders' upload), about 3.6 TiB to
  go on 2026-11-24 18:30Z, so about two weeks. Nothing is at risk meanwhile: every extent keeps two copies.
- **Next repin: Dagda 0.5.0 with Dannan 0.12.1 in one step, once Dagda 0.5.0 is tagged** (the coordinator: the
  group's top priority). Dannan 0.12 brings what Macha asked for (deferred requests bounded in bytes and per peer,
  readiness and health, keyed limits, the process runner for mobitool); read "Adopting 0.12.0" then. Dagda 0.5.0
  carries D5 fenced leases and D6.

## Now, in order

1. **Memory: 630 to 990 MiB of live heap no gauge counts, on every node.** On 0.96.1 resident memory was 2 to 2.4
   times the budget (live-a 2.35 GB, live-b 1.82 GB, live-c 1.06 GB, peak 1.36 GB of its 4 GB) and plateaued rather
   than climbed. 0.96.2 (2026-11-24 19:35Z, all three) logs glibc's own account (`allocator_*` gauges): just after
   start, in use 912 / 1,406 / 771 MiB against 127 / 417 / 144 MiB counted; free-but-held only 96 to 231 MiB, so
   not fragmentation. An empty standalone node (`build/pi/empty_node.sh`) holds 1 MiB, so it is the state every node
   loads, about the same whether a node stores 62 GB or 6 TB. Next (the maintainer, 2026-11-24): a heap profile
   with heaptrack on live-c (a temporary unit drop-in, `zz-heaptrack.conf`, removed afterwards), then the cause to
   whichever of Macha, Dannan or Dagda owns it.
2. **The repin** above, when Dagda 0.5.0 is tagged.
3. **The classifier and matcher** (the maintainer, 2026-11-24: greatly improve them). 0.96.1 did the episode forms,
   ordinals, bundle names, year spans and extras folders. Next: extras whose title is not in the same import (kept,
   under their show or film, never a film of their own); then `BACKLOG.md`, "Import".
4. **Skip intro and credits:** plan accepted, `2026-11-24-skip-segments-plan.md` (Chromaprint; Macha's own work; a
   `/segments` route per media). Phase 1 when the maintainer says where it falls.
5. **The release** (`project_release_procedure`): the pre-merge gate on a Pi (`tools/ci.sh --local` with ASan, and the
   coverage build); squash the port onto `develop`; GitHub CI green on `develop` (red since 2026-11-13 on the
   ingest/torrent ASan CHECK: ASan's registry overwritten, libtorrent the uninstrumented suspect); debs; tag; merge
   to `main`. Coverage at 0.95.5: 82.8% of lines; per file in `build/evidence/coverage-2026-11-23/list.txt`; untested (JSON `\u` escapes, FUSE chown/truncate/statfs, the remux packet
   path, WebVTT, playback rollback, media error answers, unmatched probing, acquisition file actions, Torznab).
6. **Report the Dannan and Dagda versions** in Status and the clients (`BACKLOG.md`, "Status and clients"); an API
   change, announced to Core and every client first. The catalogue's common error envelope (0.95.7) is held the same
   way (`CLIENT-CONTRACTS.md`).
7. **GoogleTest:** Macha's `MACHA_TEST` cases move on their own branch, one process per case, the count matching
   before and after; GoogleTest from Dannan's `dannan::testing` export.
8. **The stack audits** (`2026-11-20-duplication-audit.md`, `2026-11-20-no-special-node-audit.md`) and **the deferred
   API intents** (`2026-11-18-port-api-announcement.md`): through the coordinating session; on the maintainer's word.
9. **Macha's defects**, `2026-11-08-found-issues.md` in its order. 22 (`hosts_extents: auto` makes a node with disks
   cache-only when unreachable) waits: the maintainer, 2026-11-24, "revisit later".

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption; a Macha restart on any live node; a USB disconnect on live-c.

## Waiting on the maintainer

- Where phase 1 of skip intro and credits falls against the rest; the word for the release.
- Where the Dagda 0.3.3 debs go (built by Dagda, local only).
- Carried, unchecked: the media-kind import hint; truncated pack records; the `*.pre-migration.*` archives; the
  replica journal reader with no writer left.

## The live nodes (read 2026-11-24)

Real names and addresses are in the untracked `CLAUDE.local.md`. Never build on a live node.

- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a, API 192.0.2.50:7438 behind haproxy for https, `bootstrap: []`).
  Undervoltage logged through its boots; a power loss reset it 2026-11-24 11:34Z (the known supply problem; never
  recommend another supply). Its import sources and the hand-run scripts are in `/root/diska-import/`.
- **live-b** (Pi 5, 16 GB, site-b, API on its LAN address, inbound at :7437, bootstrap live-a, haproxy for https).
- **live-c** (Pi 5, 4 GB, site-c, its own site since 2026-11-24): bootstrap live-a and live-b; memory sized for 4 GB
  (retained 512M, reserves 128M viewer, 48M loader, 48M control); `hosts_extents: true` (set by hand, found-issue
  22). State on the NVMe; DATA on a 10 TB USB disk (JMicron 152d:0580 under UAS; it dropped once, 2026-11-24 17:10Z;
  the maintainer: leave UAS on and watch, the remedy on a repeat is `usb-storage.quirks=152d:0580:u`), ext4 labelled
  `diskB`, mounted at `/mnt/diskB` with `nofail`, the unit requiring the mount. Its own DNS updater (Manannan 0.4.0,
  one record, its own name) and haproxy with a Let's Encrypt certificate renewed by http-01 through haproxy (certbot
  on 127.0.0.1:8888). Its install kit is in the untracked `build/cutover/`.
- **All three:** Macha under `/usr/local` with `libdannan.so.0.11`, `libdagda.so.0.4.0`, `dannan-custody`,
  `dannan-import-macha`, `dagda-migrate`; custody sealed by systemd-creds; DATA `/mnt/diskB/data`, cores to
  `/mnt/diskB/crash`. dht replicas 2, write_copies 1, metadata_write_copies 1, extent 4M; control store 8G;
  `collect_garbage: true`. Observations once a minute in `/var/lib/macha/observation/observations.jsonl`.
- **Deploying:** a tarball built on a Pi (`build/pi/macha_package.sh`) and an install script per release in the
  untracked `build/cutover/stage-<version>/`, one node at a time, waiting for health before the next.
- **Access:** the API needs a session; the test account's credentials are on live-a only. Sync a tree to a Pi with
  `rsync -rlp --checksum --exclude '/build*' --exclude /.git --exclude CLAUDE.md --exclude CLAUDE.local.md`; no `-t`,
  no `--delete`.

## Builds

**No preferred node (the maintainer's rule, 2026-11-23):** take whichever Pi has the cores free now; rsync the tree
there and build, installing any missing -dev package (1 core, nice 19, a board note). Wait only when none has them,
with `claim-board wait --any`.

The shared build Pis are build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202` (Pi 5, 16 GB, Debian 13 arm64,
shared by every session); build-4 left the build cluster and is live-c. No builds on the Mac. Claim per step, only
the cores the step uses, release when it ends.

build-2 holds the current tree `/home/maintainer/macha-repin` (`build`, 0.96.1, suite 584/584) and the prefixes
`/home/maintainer/deps/prefix-dannan-0.11.1` and `prefix-dagda-0.4.0`. Scripts in `build/pi/`:
`macha_repin_build.sh` (`J`), `macha_package.sh` (`S`, `B`, `V`, `DANNAN`, `DAGDA`, `OUTDIR`), `deps_repin.sh`,
`macha_sanitize.sh`, `macha_coverage.sh`. Tests: `build/macha-tests --jobs N` (and `-runtime`, `-torrent`), or
`--serial [--verbose] --filter NAME`; tee results into a log on the Pi. Format with the Pi's clang-format.

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
- **Builds** only on the Pis, claimed per step, nice 19. clang-tidy reports nothing.
- **Git**: features on their own branch, squashed onto the port; push `port/dannan-dagda` freely, anything else on his
  word; no tag, merge or PR unasked; stage named paths; no attribution trailers; no em dashes; versions in full
  x.y.z; placeholders only for hosts, addresses and people in tracked files.
- **Text tools**: code lookups through basemind or Read; scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

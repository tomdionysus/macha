# Active tasks

Last updated: 2026-11-19 about 01:00Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-19-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md`,
`COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The port: Macha onto Dannan and Dagda, the only work until the cutover.** the maintainer, 2026-11-18: "Work autonomously until
you're ready to do the cutover on live-a and live-b." the maintainer, 2026-11-19: **do not begin the cutover**; get ready
for it. The plan is [`2026-11-18-cutover-runbook.md`](2026-11-18-cutover-runbook.md) (gates, per-node steps, the
mixed-version window, rollback); its history [`2026-11-14-port-rewrites.md`](2026-11-14-port-rewrites.md); done work
in `COMPLETED.md`.

**Where it stands (2026-11-19 04:30Z):** `port/dannan-dagda` at `8ef9496` (0.95.0), pushed, untagged. The suite is
green on the deployed libraries: 545/545 on Dannan 0.9.2 and Dagda 0.3.0 (tagged, gated). The deploy tarball is
built: `build/release-0.95.0/macha-0.95.0-arm64.tgz`, sha256 `7d9d1e0f...a062` (full hash in the runbook). The API is 0.94.2's; nothing to announce
([`2026-11-18-port-api-announcement.md`](2026-11-18-port-api-announcement.md) records what was mapped back and the
intents left as code comments).

**To be cutover-ready, in order:**
1. Done: **Dagda 0.3.0** tagged and gated; Macha builds on it (`8ef9496`); the install guides name the versions.
2. Done: **the deploy tarball** (above).
3. Done: **the mixed-version window**: the transports form one cluster (Dannan's run), the stores must not mix for
   long, so the plan keeps the mixing to minutes; the **two-version smoke test passed** 2026-11-19 (runbook: about 9
   minutes mixed, every write on both nodes, no session ended, no error). It was: Macha `main` (0.94.2, on
   Dannan 0.5.3; build-1 has `prefix-0.5.3` and a `main` tree from 2026-11-18) and 0.95.0 on two build Pis, one cluster
   key: handshake, members, telemetry, a store call and a Macha call each way, no session ended.
4. **The maintainer's decisions**, asked interactively once 1 to 3 are done, each with a recommendation: the off-node backup
   destination for each node's `/var/lib/macha` and `/etc/macha` (and who holds the custody files); mixed cluster
   or islands for the window, from the smoke test; the go for live-b; separately, the go for live-a; the 0.95.0
   tag, merges and pushes after both.

**Rulings in force** (the maintainer, interactive; the rest in "Standing rules"):
- 2026-11-18/19: the API stays 0.94.2's; Dannan's and Dagda's native forms come after Macha's functional fidelity is
  shown, each intent left as a code comment where it applies. Tombstone timing matches `main` (repair on the pass's
  clock). Dagda's read fix is option A (a read asks the other active peers). The cutover is not begun until he says.
- Fix forward, each mechanism proved before a change; a Dannan or Dagda cause goes to its session with the
  mechanism and waits for its gated fix. A library's release follows that library's rules (Dagda acts on the maintainer's word
  in its own session only).
- Macha's reachability provider is always registered; `maintenance.collect_garbage` (default false) gates only the
  sweeps; the live nodes keep it false until the cutover check (runbook).
- Writes name `ack_level` `persisted`; agreement waits for Dannan; no shims: Macha calls `dagda::Context`, the root
  keeps `dagda::Node`'s owner surface. Dannan 0.9.2 for the cutover.

**The libraries (read their repositories before relying on this):**
- **Dannan** 0.9.2 tagged. Custody by `dannan-custody(1)`; `dannan-import-macha --dry-run` is a check at the cutover,
  not a conversion.
- **Dagda** 0.3.0 in its release gate (above). Since `053ee6e`: the read asks the other active peers when hosting
  candidates are exhausted (`b997ba1`). Open on Dagda's side: stage 4's unpaced large rejoin is slower than stage 3's
  (18 to 25 s against 15 to 16 s for 26 x 4 MiB); no large-object repair comparison is settled.

**Builds:** the Pis were wiped to bare OS 2026-11-18 20:30Z and rebuilt; Macha's packages (FFmpeg, FUSE, libtorrent,
curl, zlib, clang-format-19, libmobi-tools, ffmpeg) are on build-1, build-2 and build-3 (the maintainer's approval), not build-4
(light work only). build-3: `/home/maintainer/deps/prefix-0.9.2`, `prefix-dagda-053ee6e`, `prefix-dagda-053ee6e-read`, the
tree `/home/maintainer/macha-store-2`. build-1: `prefix-0.5.3`, `prefix-0.9.2`, `prefix-dagda-053ee6e-read`,
`/home/maintainer/macha-main` (0.94.2, a temporary diagnostic patch applied: rebuild from `git archive main` before the smoke
test). Scripts in `build/pi/`: `macha_port_compile.sh` (`S`, `R`, `J`, `DANNAN`, `DAGDA`; writes only `$R.result`, so
capture its output to see an error), `deps_dagda_tag.sh`, `deps_092_053ee6e.sh`, `main_0942_build.sh`,
`macha_package.sh`. Run logs from before the wipe: `build/pi-logs-2026-11-18/`.
Sync with the rsync line below, then format changed files with `clang-format-19` on the Pi and copy them back;
**rsync never deletes**. Tests: `build-port/macha-tests --jobs 3`, or `--serial [--verbose] --filter NAME`
(`--verbose` shows a passing case's output); `MACHA_TEST_LOG_LEVEL=DEBUG` or `ALL` for the log. **Tee every result
into a log on the Pi**: a background command's output lands outside the boundary. Claim first; release when done.
Temporary diagnostics go on the Pi's copy only, never into Macha; the next rsync with `--checksum` removes them.

## Known live defects until the cutover

All stay on 0.94.2 and are fixed by the port.
- **Repair** (develop `distributed_store.cpp`): push and pull share one 16-call step budget; a large rejoin took about
  80 s where the bytes once take about 38 s (2026-11-16). Nothing is lost. Dagda's redesign replaces it.
- **A loader refused for device pressure waits for the next release** (develop `data_work.hpp:239-247`). Fixed in
  Dagda `053ee6e`.
- **Durable writes into new directories** (`src/durable_file.cpp`): a crash just after the write can lose a
  directory it made. Fixed on the port branch (`46c61ba`).

## Waiting on the operator

- The cutover decisions (Resume here, step 4).
- Dagda's and Dannan's branches pushed (both local on the maintainer's word).
- After the port: whether the heap chase resumes with media load.
- The Codecov token for the repository.
- Carried, unchecked: the media-kind import hint; truncated pack records; the `*.pre-migration.*` archives; the
  replica journal reader with no writer left.

## Open, in order (Macha, after the cutover)

1. **The heap corruption** (parked by the operator, 2026-11-14: port first; findings in
   `archive/2026-11-15-ACTIVE-before-rationalisation.md`, item 1) and the FUSE `rm` crash (found-issues 1, 2).
2. **The deferred API intents** (`2026-11-18-port-api-announcement.md`), once fidelity is shown and the maintainer says.
3. **A full control store is silent; work that cannot complete is retried without backoff** (found-issues 3).
4. **Cancelling an ingest job leaves its worker running** (found-issues 4).
5. **The stop**: the release horizon's census (`namespace_references`) and the tree diff under `census_follow` do
   not check it.
6. **Import filing**: about 430 episodes filed as films in `/Movies`; names such as `Glass Harbour ( (1982)`
   (found-issues 9).
7. **Books** as the web client reported at 0.92.2 (series counts, `published` types and nulls, split authors).
8. The genesis root-password file (`auth/users.cpp`, `initial-root-password`) is written with no fsync.
9. The rest of found-issues, items 5 to 17.
The cases once on the failing list (`availability` two, `two_node_mutual_bootstrap`, `coalesced_delete_burst`,
`accounts_converge`) all pass on the port; a recurrence is P0.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption.

## Cluster state (read 2026-11-19 00:30Z)

- **Both on 0.94.2**, health ok, running since 2026-11-13 21:47Z (live-a) and 21:10Z (live-b); rollback
  `/root/pre-0.94.2/`. DATA: live-a 5.9T of 9.1T, live-b 3.3T of 9.1T. `/var/lib/macha` 5.5G and 7.7G; 858G free
  on each NVMe root.
- **live-a** (root@192.0.2.50, Pi 5, 8 GB, site-a, hostname `node-c`): `/usr/local`, `/var/lib/macha`, DATA
  `/mnt/diskB/data`, control store limit 8G, API 192.0.2.50:7438, advertises `live-a.example.net`,
  `bootstrap: []`. Never build there.
- **live-b** (root@live-b.example.net, Pi 5, 16 GB, site-b, inbound capable at :7437): DATA `/mnt/diskB/data`;
  API 203.0.113.50:7438 (inside only); advertises `live-b.example.net`, bootstrap live-a; power failures recur,
  no remote power control. Never build there.
- **Both:** systemd 257, no TPM, no `credential.secret` yet; `maintenance.idle_bandwidth_fraction` 0.5, every other
  maintenance key at its default; `dht` replicas 2, write_copies 1, metadata_write_copies 1, extent 4M; packing 1M
  into 64M; heartbeat 5 s, `telemetry_interval_ms` 10000, `dead_after_ms` 30000; config backups beside
  `/etc/macha/macha.yaml`; cores to `/mnt/diskB/crash`; torrent on.
- **Access:** the API needs a session; live-a's genesis root password is in `/var/lib/macha/initial-root-password`
  (works on live-b too); scripts that read it are in `build/baselines/`.
- **Syncing a tree to a Pi:** `rsync -rlp --checksum --exclude '/build*' --exclude /.git --exclude CLAUDE.md
  --exclude CLAUDE.local.md`; no `-t`, no `--delete`.

## The shared build Pis

build-1 `root@192.0.2.169`, build-2 `.170`, build-3 `.202`, build-4 `.228` (light work
only): Pi 5, 16 GB, Debian 13 arm64, shared by every session. No builds on the Mac. Claim cores on the board (claim,
gate on "Taken"; `claim-board wait ... --for 90m` as a background command when none are free; release when the job
ends). Ask the maintainer before installing packages.

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou Shalt Not Make Control Wait. 2. Thou
  Shalt Not Make The Viewer Wait. 3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer
  Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Everything the API and the web serve is viewer work**, including the bytes still draining to a client.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.**
- **No vacuous truths.** An empty set never satisfies "all" or "enough".
- **Three answers, HTTP numbers below as above**; refusals carry an HTTP status and a snake_case code; nothing
  matches English text.
- **Proportion.** Account for and control what matters to the user's experience.
- **Sanitizers**: ASan, UBSan and TSan only for a release or to hunt a named bug, never as a routine gate.
- **Testing**: test the change once on a Pi; no repeat or soak runs except to measure a suspected flake.
- **Report findings before fixing; prove a mechanism before editing.** Questions and design decisions go to the
  operator through the interactive question dialog, with a recommendation first.
- **Every response has a snake_case code; errors add a message; clients own sorting and presentation; every API
  change is announced to Core and every client before it ships** (none in 0.95.0).
- **A Macha defect the port finds is fixed in Macha first**; the libraries match the fix. A Macha test retires only
  against a named library case, recorded case by case.
- **One definition per concept**: a type Dannan or Dagda defines is aliased, never twinned.
- **Agreement is Dannan's**: Macha builds no consensus, no lease standing in for one, and no workaround meanwhile.
- **Peer sessions** ask and report over cross-session messages: answer from code and nodes, say what was not checked.
- **Builds** only on the Pis, -j2 or -j3, nice 19, claimed. Never edit sources under a running build of the same
  tree. Format with clang-format 19; clang-tidy reports nothing; a suppression is a `NOLINTNEXTLINE(check)` with its
  reason beside it.
- **Git**: commit on the branch as work lands; push `port/dannan-dagda` when needed, anything else only on his word;
  no tag, merge or PR unasked; stage named paths, never `-A`; no attribution trailers; no em dashes in Macha.
- **Text tools**: code lookups through basemind; a hook blocks grep, sed, awk, head, tail and cat on local files.
  Scratch files and logs in `build/`, never `/tmp`.
- **The composition root is code**: concrete classes, constructor references, dependency order.

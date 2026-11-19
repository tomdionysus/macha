# Active tasks

Last updated: 2026-11-19 about 18:00Z, on `port/dannan-dagda`. **State a branch's content from its diff and a node's
state from its files, never from a prior note** (operator: "We can't afford drift like this."). The version before
this rationalisation is `archive/2026-11-19b-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md`,
`COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The goal:** Macha's store moved onto Dagda (on Dannan) on both live nodes, live-a and live-b, working as well
as it did; the only work until it is done. Functional fidelity first: the API stays 0.94.2's, Dannan's and Dagda's
native forms later. The plan is [`2026-11-18-cutover-runbook.md`](2026-11-18-cutover-runbook.md) (rulings, gates,
rehearsals, downtime, the steps, rollback); the port's history
[`2026-11-14-port-rewrites.md`](2026-11-14-port-rewrites.md); done work in `COMPLETED.md`.

**Do not start the cutover. Nothing touches live-a or live-b without the maintainer's word.** the maintainer, 2026-11-19 evening, the
order from here:
1. Done: **Dagda 0.3.1 tagged and pushed** (the maintainer's word, 2026-11-19; main `2793458`): the fast migration (option B:
   header-only moves with `<state>/migration-progress`, manifest passes in device order, no dry run) and the
   large-rejoin repair work (`maintenance.idle_bandwidth_fraction` now applies only while a higher class is present;
   an idle node's repair uses the whole link, `max_bandwidth` bounds it). Gate: 1172 ran, 1169 passed, 3 skipped;
   TSan clean; ASan+UBSan one test fault, fixed in the test.
2. Done 2026-11-19: **Macha on 0.3.1**: 545/545 on build-3 (`prefix-dagda-0.3.1`); docs, CHANGELOG, one test's
   setup (B held off by `max_bandwidth`, since an idle node now ignores its fraction); tarball rebuilt, sha256 in
   the runbook; the migration script read against Dagda's `docs/migration.md` at 0.3.1, no change.
3. **Next, small rehearsals** (runbook, "Small rehearsals"): propose their shape to the maintainer interactively, smallest
   first, then run them.
4. **The cutover**, on the maintainer's word: both nodes down together, live-a migrates and comes up first, then live-b;
   backups and custody copies on each node. Estimated about 29 h fully down, then about 16 h on one node.
5. After both, each on his word: the cutover check and garbage collection, the Macha tag, merges, pushes.

**Where it stands:** `port/dannan-dagda` pushed, untagged; 0.95.0 on Dannan 0.9.2 and Dagda 0.3.1, 545/545. Both nodes serve 0.94.2 (restarted 2026-11-19 15:08Z after the aborted first attempt: 57 minutes down,
nothing written; what it left on the nodes is listed in the runbook). The tarball in `build/release-0.95.0/` is the 0.3.1
build (sha256 in the runbook).

**Rulings in force** (the maintainer, interactive; the cutover's own are at the head of the runbook; the rest in "Standing
rules"):
- The API stays 0.94.2's; native forms come after Macha's functional fidelity is shown, each intent left as a code
  comment ([`2026-11-18-port-api-announcement.md`](2026-11-18-port-api-announcement.md)). Tombstone timing matches
  `main` (repair on the pass's clock). Dagda's read asks the other active peers (option A).
- Fix forward, each mechanism proved before a change; a Dannan or Dagda cause goes to its session with the
  mechanism and waits for its gated fix. A library's release is that library's and the maintainer's: Dagda acts on the maintainer's word in
  its own session only, never on a relay.
- Macha's reachability provider is always registered; `maintenance.collect_garbage` (default false) gates only the
  sweeps; the live nodes keep it false until the cutover check.
- Writes name `ack_level` `persisted`; agreement waits for Dannan; no shims: Macha calls `dagda::Context`, the root
  keeps `dagda::Node`'s owner surface. Dannan 0.9.2 for the cutover.

**The libraries (read their repositories before relying on this):**
- **Dannan** 0.9.2 tagged. Custody by `dannan-custody(1)`; `dannan-import-macha --dry-run` is a check at the cutover.
- **Dagda** 0.3.1 tagged and pushed (`2793458`); Macha builds on it.
  Dagda holds off the live nodes until the cutover is reported done.

**Builds:** the Pis were wiped 2026-11-18 and rebuilt; Macha's packages are on build-1, build-2 and build-3; installing
tooling needs only a claim. build-4 is light work only. build-3: `/home/maintainer/deps/prefix-0.9.2`,
`prefix-dagda-053ee6e`, `prefix-dagda-053ee6e-read`, `prefix-dagda-0.3.0`, `prefix-dagda-0.3.1`, the tree `/home/maintainer/macha-store-2`.
build-1: `prefix-0.5.3`, `prefix-0.9.2`, `/home/maintainer/macha-main` (0.94.2, clean, all targets). Shared machines:
check what is there before relying on it. Scripts in `build/pi/`: `macha_port_compile.sh` (`S`, `R`, `J`, `DANNAN`,
`DAGDA`; writes only `$R.result`, so capture its output to see an error), `deps_dagda_tag.sh`, `main_0942_build.sh`
(`TARGET=all` for the daemon), `macha_package.sh`. Smoke-test kit: `build/smoke/`. Cutover scripts and disk
measurement: `build/cutover/`. Sync with the rsync line below, then format changed files with `clang-format-19` on
the Pi and copy them back; **rsync never deletes**. Tests: `build-port/macha-tests --jobs 3`, or `--serial
[--verbose] --filter NAME` (`--verbose` shows a passing case's output; `--repeat N` measures a flake);
`MACHA_TEST_LOG_LEVEL=DEBUG` or `ALL`. **Tee every result into a log on the Pi**: a background command's output lands
outside the boundary. Claim first, wait for "taken", then build; release when done. Temporary diagnostics go on the
Pi's copy only, never into Macha; the next rsync with `--checksum` removes them. A Macha mount point is immutable
while unmounted (`chattr -i` before removing a test node's directory).

## Known live defects until the cutover

All stay on 0.94.2 and are fixed by the port.
- **Repair** (develop `distributed_store.cpp`): push and pull share one 16-call step budget; a large rejoin took about
  80 s where the bytes once take about 38 s (2026-11-16). Nothing is lost. Dagda's redesign replaces it.
- **A loader refused for device pressure waits for the next release** (develop `data_work.hpp:239-247`). Fixed in
  Dagda `053ee6e`.
- **Durable writes into new directories** (`src/durable_file.cpp`): a crash just after the write can lose a
  directory it made. Fixed on the port branch (`46c61ba`).

## Waiting on the operator

- The rehearsals' shape (to be proposed once Macha is rebuilt on Dagda 0.3.1); then the word to start the cutover.
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
6. **Import filing**: about 430 episodes filed as films in `/Movies`; names such as `Blade Runner ( (1982)`
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
ends). Installing tooling needs only a claim.

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

# The cutover: live-a and live-b onto Dagda (runbook)

Rationalised 2026-11-19 evening. Macha's store moves to Dagda's on both live nodes. **Not started; do not start.**
Order of work (the maintainer, 2026-11-19): (1) Dagda tagged and released first (done: 0.3.1); (2) Macha rebuilt and tested on
it; (3) small rehearsals of the migration; (4) the cutover, on his word, asked interactively.

**The maintainer's rulings for the cutover:**
- **live-b serves while live-a migrates** (the maintainer, 2026-11-19 evening, replacing "both nodes down together"):
  live-a stops and migrates while live-b stays up on 0.94.2; live-b stops just before live-a starts on
  0.95.x, then migrates; live-b comes up and joins; then the release fidelity checks. Still no mixed-version window:
  the two never serve different versions at once. The window's analysis is in
  [`archive/2026-11-19-cutover-mixed-version-window.md`](archive/2026-11-19-cutover-mixed-version-window.md).
- **live-a first, and up first**: live-a migrates and starts on 0.95.x alone; then live-b migrates and joins.
  live-a can be power-cycled by hand; live-b cannot. What live-b takes in while live-a is down is migrated
  with live-b and reaches live-a by repair once both serve.
- **Backups on the node itself**, in other directories ("We don't expect system disk failure"); the custody files the
  same.
- **The migration is Dagda's option B**: a header-only migration between two independent full manifest passes (the
  before and after proof); no dry run.
- Standing: proof on the live nodes; the API stays 0.94.2's; `maintenance.collect_garbage` stays false until the
  cutover check below.

## Gates

| Gate | State |
|---|---|
| Dagda release with the fast migration | done: **0.3.1** tagged and pushed 2026-11-19 (the maintainer's word; main `2793458`); gate 1172 ran, 1169 passed, 3 skipped, TSan clean |
| Macha on that release | done 2026-11-19: `find_package(dagda 0.3.1)`, install guides, `docs/configuration.md` (`idle_bandwidth_fraction` applies only while a higher class is present), CHANGELOG; one test's B held off by `max_bandwidth` (its fraction 0 no longer stops an idle pull) |
| Deploy tarball | `build/release-0.95.0/macha-0.95.0-arm64.tgz`, sha256 `5673bfc4ad0d44357813107dca2f6c6ce08094d5e87289315f47e09bf7dc0002`, Dagda 0.3.1 (`dagda-migrate --version`: `0.3.1 (dannan 0.9.2)`); the 0.3.0 one is replaced |
| Small rehearsals | to propose and run (below) |
| Macha suite | 545/545 on build-3 on Dannan 0.9.2 and Dagda 0.3.1 (2026-11-19 19:42Z) |
| Migration script vs Dagda 0.3.1 `docs/migration.md` | read 2026-11-19: flags, `--resume`, rerun-to-finish, compare and `RESULT: PASS` all match; no change |
| Dannan | 0.9.2 tag |
| Install script | `build/install-0.95.0.sh` (written, never run; read it before the first use) |
| Migration script | `build/cutover/migrate-0.95.0.sh` (ran its first step once; updated to three passes; check its flags against Dagda's `docs/migration.md` at the tag) |
| API announcement | none: 0.95.0 sends 0.94.2's API |
| 0.94.2 baselines | `build/baselines/results-0.94.2`; compared after each node is on 0.95.x |
| The maintainer's word | the start |

## Small rehearsals (after Dagda's release, before the cutover)

Purpose: the three passes' real rates and the resume path, with the released tool, before the nodes go down. Not yet
designed with the maintainer; to propose, smallest first:
1. **On a build Pi**: a fresh 0.94.2 node (`main`, build-1 has `prefix-0.5.3`), a few GB of 4 MiB files written
   through it, stopped; then custody, before manifest, migration, after manifest, compare; start 0.95.x on it and read
   every file back. Then the same with the migration killed part way and rerun, and a manifest pass killed and
   `--resume`d. Proves the released tool end to end against a real 0.94.2 state and the scripts as written.
2. **On the real disks, read-only, while serving** (if the maintainer wants the rate before the downtime): a before-manifest pass
   cannot run on a live node (the store's lock), so the rate there is known only from `build/cutover/phys_reads.py`'s
   figures (below) until the cutover's first pass. If far off them, that pass is stopped: a manifest pass writes
   nothing to the disk.

**Results (2026-11-19 evening, build-3 and build-4, kit `build/rehearsal/`, the 0.3.1 tarball, the scripts as written):**
- R1, one node end to end (3.1 GB, 3,620 files, 1,092 loose objects): steps 1 to 9 pass; passes 22 s, migration 2 s,
  after 22 s (147 MiB/s on NVMe), `RESULT: PASS`; 0.95.0 reads every file back; a new write reads back.
- R2, kills (5.6 GB, 5,010 files, 3,244 loose): before pass killed, resumed with `RESUME=1`; migration killed part
  way (marker `going-forward`, progress file present), 0.94.2 refused the disk, rerun finished it (2,585 already v19,
  915 moved, 0 unreadable); after pass killed and resumed; `RESULT: PASS`; 0.95.0 reads every file back.
- R3, rollback after step 7: a write on 0.95.0, then `--reverse` (3 s, 0 unreadable, marker v18), `/root/pre-0.95.0`
  restored, 0.94.2 healthy, every file back including those written on 0.95.0.
- R4, two nodes (the new order): **stopped before the migration** on a 0.94.2 defect: on a fresh 0.94.2 pair, one of
  six 96 MiB files copied into A's mount committed short (84,410,368 of 100,663,296 bytes, the bytes it has correct),
  on both nodes, with no error, nothing parked, every publication counted complete. Not seen in R1 or R2 (one node).
  Under diagnosis, reported to the maintainer; the pair is left running on build-3 and build-4.

## Downtime (estimates from the live disks, 2026-11-19; the released tool not yet run on them)

| Phase | live-a (5.9T, 1.54 M loose 4 MiB objects) | live-b (3.3T, 0.90 M) | Cluster |
|---|---|---|---|
| Before manifest | about 13 h | stopped | down |
| Migration (headers only) | about 2.7 h plus syncs | stopped | down |
| After manifest, compare | about 13 h | stopped | down |
| live-a serving, live-b's three passes | 0.95.x alone | about 7 + 1.3 + 7 h | up, one node: what only live-b holds is unavailable |
| Both on 0.95.x | serving | serving | up; repair converges them |

So about 29 h fully down, then about 16 h on one node. Measurements behind it (both nodes live, read-only): raw
sequential 147 to 234 MB/s; every loose object is one extent; with inodes warm and files visited in physical order,
whole files read at 122 to 158 MiB/s (live-a) and 127 to 148 (live-b), a header-only read 6.3 ms and 5.0 ms a
file; with cold inodes or in id order about 80 to 100 MiB/s. The released tool walks first (FIEMAP), then visits in
device order, one read at a time, authenticating on the other cores, and syncs tag writes in batches of 256.

## The first attempt (2026-11-19, aborted on the maintainer's word)

Both stopped and disabled 14:10:59Z; live-a backed up, transport files checked ("already" x3), custody made and
sealed (id `ce442e68...`); the dry run of Dagda 0.3.0's tool read at 64 MiB/s (one thread, read then compute): about
24 h a pass, four passes. Stopped at 196 GiB with nothing written; both back on 0.94.2 at 15:08Z (live-a 15:08:11Z,
live-b 15:08:42Z, `replicas=2/2`); 57 minutes down. **Left on the nodes:**
- live-a: its custody (`/var/lib/macha/identity/custody`, `/etc/credstore.encrypted/dannan-root`,
  `/var/lib/systemd/credential.secret`, copies in `/root/custody-0.95.0/`); 0.94.2 ignores it. **Next time step 5 on
  live-a is `show` only, never `init`.**
- live-a: `/root/backup-0.95.0/` (5.6 GB, stale since 0.94.2 ran again: delete and take a fresh one),
  `/root/migrate-0.95.0/` (the old script and log: replace).
- both: `/root/stage-0.95.0/` (the Dagda 0.3.0 tarball: replace with the new one), `/root/phys/` (the physical maps,
  166 MB and 97 MB: delete after the cutover).

## Crash and power cut during a migration

`dagda-migrate` is finished by running it again with the same arguments; `--reverse` goes back from any point; until
it completes the disk's marker makes Macha and the store refuse it, naming the tool. From Dagda's release the
migration keeps `<state>/migration-progress` (durable after each batch of 256): a resumed run passes over what is done
unread and reads whole the batch past that point (where a torn tag is found) and each batch's first record. A manifest
pass resumes with `--resume` from its partial and side journal. Tested by Dagda with kills after every step, forward
and back, torn writes at many points, and the real binary killed mid-manifest. **Not proven**: a real power cut (a
kill keeps the page cache), scale. Hence live-a first.

**After any crash or power cut mid-migration:** do not start Macha. Run the same command again before anything opens
the disk. The `macha` unit is disabled for the cutover, so a reboot does not start it on a half-migrated disk.

## What is deployed

Macha 0.95.x at the commit that passes on Dagda's release, RelWithDebInfo; Dannan 0.9.2 (`libdannan`,
`dannan-custody`, `dannan-import-macha`, `dannan-version`); Dagda's release (`libdagda`, `dagda-migrate`). One tarball
under `/usr/local`, with its sha256.

## Config

Both live `macha.yaml` files load under the port with no unknown key (checked 2026-11-18,
`build/cutover/config_check.cpp`). The store node gets, on both: state `/var/lib/macha`, control store
`/var/lib/macha/metadata-objects` (8 GiB, packs 1 MiB into 64 MiB), backend `/mnt/diskB/data` (live-a 10 TiB limit,
live-b 9 TiB; 4 GiB reserve), cache `/var/cache/macha` (4096 blocks), replication 2, write copies 1, metadata write
copies 1, extent 4 MiB, refresh 5 s, garbage grace 30 days, ledger cache 64 MiB, `maintenance.collect_garbage` false.
The install script adds `custody: { credential: dannan-root }`. Both keep the cluster key and no certificate.
If Dagda's release carries its repair change (idle repair at the whole link), `maintenance.idle_bandwidth_fraction`
(0.5 on both) applies only while a higher class is present: document it in `docs/configuration.md` with that release.

## The nodes (read 2026-11-19)

| | live-a (192.0.2.50, Pi 5 8 GB, hostname node-c) | live-b (live-b.example.net, Pi 5 16 GB) |
|---|---|---|
| Running | 0.94.2, restarted 2026-11-19 15:08Z, health ok | 0.94.2, restarted 2026-11-19 15:08Z, health ok |
| DATA | 5.9T of 9.1T, USB (uas), `/dev/sdb` | 3.3T of 9.1T, USB (usb-storage, queue depth 2), `/dev/sda` |
| `/var/lib/macha` | 5.5G | 7.7G |
| NVMe root free | about 850G | about 858G |
| systemd | 257, no TPM; `credential.secret` made 2026-11-19 | 257, no TPM; none yet |
| Present | `/root/stop-with-stacks.sh`, `/etc/macha/cluster.key`, `/var/cache/macha` | the same |

## The steps

**Order changed 2026-11-19 evening (rulings above); the text below is to be rewritten to it after rehearsal R4:**
step 2 stops live-a only; live-b keeps serving through live-a's steps 3 to 6; live-b is stopped and disabled
just before live-a's step 7; then live-b's steps 3 to 9. Downtime: live-b serves alone through live-a's
passes (about 29 h), then live-a alone through live-b's (about 16 h); the cluster is never fully down except
for the minutes between live-b's stop and live-a's health.

`T=/root/stage-0.95.0/root/usr/local` on each node. `M` is
`systemd-run --wait --pipe -p LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root
--setenv=LD_LIBRARY_PATH=$T/lib $T/bin/dagda-migrate --state /var/lib/macha --node-state /var/lib/macha --cluster-key
/etc/macha/cluster.key --credential dannan-root --backend /mnt/diskB/data --control /var/lib/macha/metadata-objects
--cache /var/cache/macha` (no `--credentials-dir`, no `--heads`: the metadata stays Macha's; `--wait` passes the exit
code through). Step 6 runs detached as a systemd unit by `build/cutover/migrate-0.95.0.sh`, which logs each pass's
start, end and exit code to `/root/migrate-0.95.0/run.log` and its state to `/root/migrate-0.95.0/status`. Each step
names its check; a failed check stops there and is reported before anything else runs.

**A. Both nodes, while still serving:**
1. **Stage**: remove the old `/root/stage-0.95.0/`, `/root/backup-0.95.0/`, `/root/migrate-0.95.0/`; the new tarball
   and its sha256 to `/root/stage-0.95.0/`; `sha256sum -c`; extract to `/root/stage-0.95.0/root/` (not `/`); the
   install and migrate scripts beside it. Check: `LD_LIBRARY_PATH=$T/lib $T/bin/dagda-migrate --version` names the
   release (on dannan 0.9.2).

**B. Stop both:**
2. On each: `/root/stop-with-stacks.sh`, then `systemctl disable macha`. Check: `is-active` `inactive` and
   `is-enabled` `disabled` on both.

**C. live-a (then D, live-b, the same steps 3 to 9 once live-a serves):**
3. **Back up on the node**: `/var/lib/macha` and `/etc/macha` to `/root/backup-0.95.0/` (one tarball and its sha256;
   5 minutes on live-a). Check: `sha256sum -c`.
4. **Check the transport files**: `LD_LIBRARY_PATH=$T/lib $T/bin/dannan-import-macha --dry-run /var/lib/macha`.
   Expect "already" or "absent" for all three. Any "imported": stop and send it to Dannan.
5. **Custody**. live-a (made 2026-11-19): prove it only, `systemd-run --wait --pipe --quiet -p
   LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root --setenv=LD_LIBRARY_PATH=$T/lib
   $T/bin/dannan-custody show --state /var/lib/macha --credential dannan-root` prints `{"id":"ce442e68...",...}`.
   live-b (none yet): `LD_LIBRARY_PATH=$T/lib $T/bin/dannan-custody init --state /var/lib/macha --credential
   dannan-root | systemd-creds encrypt --name=dannan-root - /etc/credstore.encrypted/dannan-root` (never pipe init's
   output anywhere else; refuse if `/var/lib/macha/identity/custody` exists), `chmod 600` the sealed file, then the
   same `show`; then the three files (`/etc/credstore.encrypted/dannan-root`, `/var/lib/systemd/credential.secret`,
   `/var/lib/macha/identity/custody`) copied to `/root/custody-0.95.0/` (700, files 600) and compared.
6. **Migrate**, node stopped throughout, manifests in `/root/migrate-0.95.0/`:
   - `M --write-manifest /root/migrate-0.95.0/before`. Check: exit 0; its rate in the first hour near 125 MiB/s, or
     stop and report (nothing has been written).
   - `M` (headers only). Check: exit 0.
   - `M --write-manifest /root/migrate-0.95.0/after`. Check: exit 0.
   - `dagda-migrate --compare-before .../before --compare-after .../after`. Check: exit 0 and `RESULT: PASS`.
7. **Install and start**: `TARBALL=/root/stage-0.95.0/<tarball> HEALTH=<api>/api/v1/health bash
   /root/stage-0.95.0/install-0.95.0.sh`. It refuses a running node or missing custody, proves the seal, installs,
   adds the unit drop-in and the `custody` key, starts, and waits up to 300 s for health as the new version; a node
   that fails is left stopped (see Rollback).
8. **Prove it serves**: health ok as the new version; Status's store figures match the disk; `startup.phase` ready; a
   known file read end to end; a catalogue artwork read; metadata writable; no `reopen on sight` line in the journal
   since start; the 0.94.2 baseline comparison (`build/baselines/`). For live-b also: both nodes active in each
   other's Status, metadata `replicas=2/2`.
9. **Re-enable**: `systemctl enable macha`.

## After both nodes

- Repair converges the two nodes' copies; watch Status's repair section until it settles.
- **The cutover check, then garbage collection**: on each node, the reachability provider's inventory against the
  namespace (every referenced object present in the inventory, nothing live in the garbage); only then
  `maintenance.collect_garbage: true`, on the maintainer's word.
- On the maintainer's word: tag Macha, merge to develop and main, push; Dagda's and Dannan's pushes are theirs.
- Once both have run cleanly for as long as the maintainer wants: remove `/root/backup-0.95.0/`, `/root/migrate-0.95.0/`,
  `/root/stage-0.95.0/`, `/root/phys/`. `/root/custody-0.95.0/` stays.

## Rollback (per node)

- **Before the migration pass ran** (a manifest pass writes nothing to the disk): `systemctl enable --now macha`
  starts 0.94.2. Custody stays; 0.94.2 ignores it. Done once, 2026-11-19.
- **After the migration pass started, before or after step 7**: stop the node; `M --reverse` returns the disk to v18
  (finished by rerunning after a crash); restore `/root/pre-0.95.0/` (0.94.2's binaries, unit and `macha.yaml`, saved
  by the install script) if step 7 ran; `systemctl enable --now macha`. `/root/backup-0.95.0/` restores
  `/var/lib/macha` and `/etc/macha` if the state itself is in doubt.
- **If live-a must go back after live-b started its migration**, live-b goes back too or both finish forward:
  never one of each serving.

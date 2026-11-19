# The cutover: live-a and live-b onto Dagda (runbook)

Rationalised 2026-11-19. Macha's store moves to Dagda's on both live nodes. Nothing here has run on a live node.
**The cutover is not scheduled** (the maintainer, 2026-11-19: "do not begin the cutover"); it starts on his word, asked
interactively.

**The maintainer's rulings for the cutover (2026-11-19):**
- **Both nodes down together**: stop Macha on both, do the cutover, bring both back. No mixed-version window (this
  overrides "never both nodes down" for the cutover only). The window's analysis and the two-version smoke test are in
  [`archive/2026-11-19-cutover-mixed-version-window.md`](archive/2026-11-19-cutover-mixed-version-window.md).
- **live-a first, and up first**: live-a migrates and starts on 0.95.0 alone; then live-b migrates and joins.
  live-a can be power-cycled by hand; live-b cannot.
- **Backups on the node itself**, in other directories: "We don't expect system disk failure." The custody files the
  same.
- Standing: proof on the live nodes; the API stays 0.94.2's; `maintenance.collect_garbage` stays false until the
  cutover check below.

## Gates

| Gate | State |
|---|---|
| Macha suite green on the deployed libraries | done: 545/545 at `8ef9496` on Dannan 0.9.2 and Dagda 0.3.0 (build-3, 2026-11-19 04:28Z) |
| Macha release | 0.95.0 at `8ef9496` on `port/dannan-dagda`, pushed; tag 0.95.0 on the maintainer's word |
| Dannan | 0.9.2 tag |
| Dagda | done: 0.3.0 tagged (local, `2a58202`, tree `af09d301` = the gated `4a4b322`); Release, ASan+UBSan, TSan passed |
| Deploy tarball | done: `build/release-0.95.0/macha-0.95.0-arm64.tgz`, sha256 `7d9d1e0fef5b0875dc3c6b819de5709b8d6c8b0ceda04fe16adf492d1126a062`, 76 MB; binaries resolve inside it |
| Install script | `build/install-0.95.0.sh` (written; read it before the first use) |
| API announcement | none: 0.95.0 sends 0.94.2's API |
| 0.94.2 baselines | `build/baselines/results-0.94.2`; compared after each node is on 0.95.0 |
| The maintainer's word | the start |

## Downtime (Dagda's estimates, unmeasured)

| Phase | live-a | live-b | Cluster |
|---|---|---|---|
| Both stopped, live-a migrating | migrating, about 22 h (5.9T) | stopped | **down** |
| live-a serving, live-b migrating | 0.95.0, alone | migrating, about 12 h (3.3T) | up, one node: what only live-b holds is unavailable |
| Both on 0.95.0 | serving | serving | up; repair converges them |

live-a's migration is the first measurement on a real disk; record every step's start and end.

## Crash and power cut during a migration

`dagda-migrate` is finished by running it again with the same arguments: records and ledgers are told old from new
by which key authenticates them, temporaries are removed first, a torn 48-byte rewrite is repaired, the manifests
resume with `--resume`, and until it completes the disk's marker makes Macha and the store refuse it, naming the tool.
`--reverse` goes back from any point. Tested by Dagda (2026-11-19): a forked run killed after each of its 25 steps,
forward and back, then finished or reversed; kills in a second run finished by a third; torn tags and pack records cut
at many points; trie rekeys stopped inside; the manifest pass stopped at every checkpoint, and the real binary
SIGKILLed mid-pass and resumed. **Not proven**: a real power cut (a kill keeps the page cache; nothing tests dropped
unsynced writes or the fsync order of new directories), scale (tiny test states; the largest run was the
2026-11-12 rehearsal on a copy of live-a's 0.92.4 state), and a real signal through the binary for the forward and
reverse passes. Hence live-a first.

**After any crash or power cut mid-migration:** do not start Macha. Run the same `M` command again, before anything
opens the disk (only the tool repairs a torn record). The `macha` unit is disabled for the cutover, so a reboot does
not try to start it on a half-migrated disk.

## What is deployed

- **Macha** 0.95.0, `8ef9496`, RelWithDebInfo.
- **Dannan 0.9.2**: `libdannan`, `dannan-custody`, `dannan-import-macha`, `dannan-version`.
- **Dagda 0.3.0**: `libdagda`, `dagda-migrate`.
One tarball laid out under `/usr/local`, with its sha256.

## Config

Both live `macha.yaml` files load under the port with no unknown key (checked 2026-11-18,
`build/cutover/config_check.cpp`). The store node gets, on both: state `/var/lib/macha`, control store
`/var/lib/macha/metadata-objects` (8 GiB, packs 1 MiB into 64 MiB), backend `/mnt/diskB/data` (live-a 10 TiB limit,
live-b 9 TiB; 4 GiB reserve), cache `/var/cache/macha` (4096 blocks), replication 2, write copies 1, metadata write
copies 1, extent 4 MiB, refresh 5 s, garbage grace 30 days, ledger cache 64 MiB, `maintenance.collect_garbage` false.
The install script adds `custody: { credential: dannan-root }`. live-b must keep the cluster key and no certificate.

## The nodes (read 2026-11-19 00:30Z)

| | live-a (192.0.2.50, Pi 5 8 GB) | live-b (live-b.example.net, Pi 5 16 GB) |
|---|---|---|
| Running | 0.94.2 since 2026-11-13 21:47Z, health ok | 0.94.2 since 2026-11-13 21:10Z, health ok |
| DATA | 5.9T of 9.1T | 3.3T of 9.1T |
| `/var/lib/macha` | 5.5G | 7.7G |
| NVMe root free | 858G | 858G |
| systemd | 257, no TPM; `credential.secret` not yet made | the same |
| Present | `/root/stop-with-stacks.sh`, `/etc/macha/cluster.key`, `/var/cache/macha` | the same |

## The steps

`T=/root/stage-0.95.0/root/usr/local` on each node. `M` is
`systemd-run --wait --pipe -p LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root
--setenv=LD_LIBRARY_PATH=$T/lib $T/bin/dagda-migrate --state /var/lib/macha --node-state /var/lib/macha --cluster-key
/etc/macha/cluster.key --credential dannan-root --backend /mnt/diskB/data --control /var/lib/macha/metadata-objects
--cache /var/cache/macha` (no `--credentials-dir`; `--wait` passes the exit code 0, 1 or 2 through). Each step names
its check; a failed check stops there and is reported before anything else runs.

**A. Both nodes, while still serving:**
1. **Stage**: the tarball and its sha256 to `/root/stage-0.95.0/`; `sha256sum -c`; extract to
   `/root/stage-0.95.0/root/` (not `/`); `build/install-0.95.0.sh` beside it. Check: `LD_LIBRARY_PATH=$T/lib
   $T/bin/dagda-migrate --version` says 0.3.0 (dannan 0.9.2).

**B. Stop both:**
2. On each: `/root/stop-with-stacks.sh`, then `systemctl disable macha`. Check: `systemctl is-active macha` is
   `inactive` and `is-enabled` is `disabled` on both.

**C. live-a (then D, live-b, the same steps 3 to 9 once live-a serves):**
3. **Back up on the node**: `/var/lib/macha` and `/etc/macha` to `/root/backup-0.95.0/` (one tarball and its sha256).
   Check: `sha256sum -c`.
4. **Check the transport files**: `LD_LIBRARY_PATH=$T/lib $T/bin/dannan-import-macha --dry-run /var/lib/macha`.
   Expect "already" or "absent" for `membership/known-nodes.bin`, `connectivity/inbound.bin`,
   `telemetry/last-known.bin`. Any "imported": stop and send it to Dannan.
5. **Custody**: `LD_LIBRARY_PATH=$T/lib $T/bin/dannan-custody init --state /var/lib/macha --credential dannan-root |
   systemd-creds encrypt --name=dannan-root - /etc/credstore.encrypted/dannan-root`. Never pipe init's output
   anywhere else (the root in clear). This first encrypt creates the host key `/var/lib/systemd/credential.secret`.
   Check: `systemd-run --wait --pipe -p LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root
   --setenv=LD_LIBRARY_PATH=$T/lib $T/bin/dannan-custody show --state /var/lib/macha --credential dannan-root` prints
   `{"id":...,"public_key":...}`. If init printed but encrypt failed: remove `/var/lib/macha/identity/custody` and run
   init again. **Custody copy**: the three files together, `/etc/credstore.encrypted/dannan-root`,
   `/var/lib/systemd/credential.secret`, `/var/lib/macha/identity/custody`, into `/root/custody-0.95.0/` (mode 700,
   files 600). Check: the three present, sizes non-zero, matching their originals.
6. **Migrate**, node stopped throughout, manifests in `/root/migrate-0.95.0/` (NVMe; about 1.2 GB):
   - `M --dry-run`. Check: exit 0.
   - `M --write-manifest /root/migrate-0.95.0/before`. Check: exit 0.
   - `M` (the long step). Check: exit 0.
   - `M --write-manifest /root/migrate-0.95.0/after`. Check: exit 0.
   - `LD_LIBRARY_PATH=$T/lib $T/bin/dagda-migrate --compare-before /root/migrate-0.95.0/before --compare-after
     /root/migrate-0.95.0/after`. Check: exit 0 and `RESULT: PASS`.
7. **Install and start**: `TARBALL=/root/stage-0.95.0/macha-0.95.0-arm64.tgz HEALTH=<api>/api/v1/health bash
   /root/stage-0.95.0/install-0.95.0.sh`. It refuses a running node or missing custody, proves the seal, installs,
   adds the unit drop-in and the `custody` key, starts, and waits up to 300 s for health as 0.95.0; a node that fails
   is left stopped on 0.95.0 (see Rollback). Check: up as 0.95.0.
8. **Prove it serves**: `/api/v1/health` ok as 0.95.0; Status's store figures (capacity and used match the disk);
   `startup.phase` ready; a known file read end to end (first bytes and size); a catalogue artwork read; metadata
   writable; no `reopen on sight` line in the journal since start; the 0.94.2 baseline comparison
   (`build/baselines/`). For live-b also: both nodes active in each other's Status, metadata `replicas=2/2`.
9. **Re-enable**: `systemctl enable macha`. Check: `is-enabled` is `enabled`.

## After both nodes

- Repair converges the two nodes' copies; watch Status's repair section until it settles.
- **The cutover check, then garbage collection**: on each node, the reachability provider's inventory against the
  namespace (every referenced object present in the inventory, nothing live in the garbage); only then
  `maintenance.collect_garbage: true`, on the maintainer's word.
- On the maintainer's word: tag Macha 0.95.0, merge to develop and main, push; Dagda's and Dannan's pushes are theirs.
- Once both have run cleanly for as long as the maintainer wants: remove `/root/backup-0.95.0/`, `/root/migrate-0.95.0/` and
  `/root/stage-0.95.0/`. `/root/custody-0.95.0/` stays.

## Rollback (per node)

- **Before step 6's migration ran**: start 0.94.2 (`systemctl enable --now macha`). Custody stays; 0.94.2 ignores it.
- **After the migration started, before or after step 7**: stop the node; `M --reverse` returns the disk and metadata
  to v18 (as long again as the migration; finished by rerunning after a crash); then restore `/root/pre-0.95.0/`
  (0.94.2's binaries, unit and `macha.yaml`, saved by the install script) if step 7 ran; `systemctl enable --now
  macha`. `/root/backup-0.95.0/` restores `/var/lib/macha` and `/etc/macha` if the state itself is in doubt.
- **If live-a must go back after live-b started its migration**, live-b goes back too (both on 0.94.2) or both
  finish forward: never one of each serving.

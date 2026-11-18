# The cutover: live-a and live-b onto Dagda (runbook, 2026-11-18)

Macha's store moves to Dagda on both live nodes. The maintainer's rulings stand: live-b first, never both nodes down at once,
deploys on his word each time, proof on the live nodes (no spare disk for a full copy). This file is the plan; nothing
in it has been run on a live node.

## What is deployed

- Macha: `port/dannan-dagda` at the commit the final suite passes on, built RelWithDebInfo on a claimed Pi, its
  version bumped with a CHANGELOG entry (the port, the durable-directory fix `46c61ba`, the API changes below).
- Dannan **0.9.2** (Dannan's word, 2026-11-18: 0.8.5 carries the ledger double free and the directory-sync loss on a
  live node). The tarball carries `libdannan`, `dannan-custody`, `dannan-import-macha` (Macha embeds the node; `dannan-node` is
  not installed).
- Dagda `feature/macha-store-gaps` at its newest gated commit (a tag cut from it once the maintainer says; Dagda proposes it
  after its release sanitizers). The tarball carries `libdagda` and `dagda-migrate`.

## Config (checked 2026-11-18 with the port's own loader, `build/cutover/config_check.cpp`)

Both live `macha.yaml` files load under the port with no unknown key. The store node is given, on both: state
`/var/lib/macha`, control store `/var/lib/macha/metadata-objects` (8 GiB, packs 1 MiB into 64 MiB), one backend
`/mnt/diskB/data` (live-a limit 10 TiB, live-b 9 TiB; 4 GiB reserve), cache `/var/cache/macha` (4096 blocks),
replication 2, write copies 1, metadata write copies 1, extent 4 MiB, the server's refresh 5 s (the heartbeat),
garbage grace 30 days, ledger cache 64 MiB, `maintenance.collect_garbage` false (the pass sweeps nothing until the
cutover check; repair and the survey run).

One addition at the cutover, on both: `custody: { credential: dannan-root }`.

## Per node (live-b, then live-a once live-b is serving)

Times are unmeasured where marked. Dagda's estimate for live-a's migration is about 22 h in all (each pass reads
every byte); nothing has measured it on that disk.

1. **Stage** the tarball at `/root/stage-V`; keep `/root/pre-V/` as `build/install-0.94.2.sh` does (binaries,
   `macha.yaml`, the state files the old install script saved).
2. **Stop** with `/root/stop-with-stacks.sh`. The other node keeps serving.
3. **Back up off-node**: the node's control store and state directory (not DATA), as the flag-day prep says.
4. **Check the transport files**: `dannan-import-macha --dry-run /var/lib/macha`. Expect "already" or "absent" for
   `membership/known-nodes.bin`, `connectivity/inbound.bin`, `telemetry/last-known.bin`. Any "imported": stop and
   send it to Dannan.
5. **Custody** (Dannan's procedure, corrected 2026-11-18 to `dannan-custody(1)`): `dannan-custody init --state
   /var/lib/macha --credential dannan-root | systemd-creds encrypt --name=dannan-root -
   /etc/credstore.encrypted/dannan-root`. Keeps `node-id`. **Never pipe init's output to a file or `tee`**: that is
   the root in clear. Prove the seal as the unit will read it: `systemd-run --wait --pipe -p
   LoadCredentialEncrypted=dannan-root dannan-custody show --state /var/lib/macha --credential dannan-root` prints
   `{"id":...,"public_key":...}`. If `init` printed and `systemd-creds encrypt` failed, remove
   `/var/lib/macha/identity/custody` and run init again (nothing has used it). **The off-node backup is three files
   kept together, offline, operator-held, one set per node**: `/etc/credstore.encrypted/dannan-root`, that host's
   `/var/lib/systemd/credential.secret` (a Pi 5 has no TPM, so the seal is the host key alone), and
   `/var/lib/macha/identity/custody`. Losing them after the migration loses the node's ledgers.
6. **Migrate** (Dagda's procedure, `docs/migration.md`), node stopped throughout, manifests on another disk (about
   1.2 GB for the two). Every run that needs the root goes under its own `systemd-run`, so the root is only ever in
   the unit's `$CREDENTIALS_DIRECTORY` (Dagda, 2026-11-18: no `--credentials-dir`; the tool reads the credential as
   `dannan-custody show` does). `M` below is
   `systemd-run --wait --pipe -p LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root
   dagda-migrate --state /var/lib/macha --node-state /var/lib/macha --cluster-key /etc/macha/cluster.key
   --credential dannan-root --backend /mnt/diskB/data --control /var/lib/macha/metadata-objects
   --cache /var/cache/macha`; `--wait` passes the tool's exit code (0, 1, 2) through.
   - `M --write-manifest <disk>/before`
   - `M` (the migration)
   - `M --write-manifest <disk>/after`, before the node starts
   - `dagda-migrate --compare-before <disk>/before --compare-after <disk>/after` (no key, runs directly): exit 0 and
     "RESULT: PASS".
7. **Install** with `build/install-0.95.0.sh` (steps 7 and 8; it refuses a running node or missing custody, proves
   the seal, and leaves a node that fails its health check stopped on 0.95.0): the port, a unit drop-in with `LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root`,
   and `macha.yaml` with the custody key.
8. **Start**; healthy within the install script's bound or roll back. Check: `/api/v1/health` ok, Status's store
   figures, both nodes active, a read of a known file, metadata writable 2/2.

## Rollback

`M --reverse` (under `systemd-run`, as above) returns the state to v18 for Macha 0.93.x/0.94.x, metadata
included; then `/root/pre-V/` (0.94.2's binaries, unit and `macha.yaml`). Custody stays (0.94.2 ignores it).

## Before the first node (not yet done)

- The suite green on the final libraries (see `ACTIVE.md`).
- API changes announced to Core and every client (`2026-11-14-port-rewrites.md`, "Before the flag day", plus
  repair's `offer_bytes` and `in_flight`).
- The maintainer's word on: the Dagda tag, the migration's downtime (no rehearsal on a copy is possible; the first live
  migration is the measurement), and the cutover itself.

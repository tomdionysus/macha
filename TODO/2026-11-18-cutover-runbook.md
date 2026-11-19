# The cutover: live-a and live-b onto Dagda (runbook)

Rationalised 2026-11-19. Macha's store moves to Dagda's on both live nodes. Nothing here has run on a live node.
**The cutover is not scheduled** (the maintainer, 2026-11-19: "not yet"; "do not begin the cutover"). Every step that touches a
node waits for his word, asked interactively, each node separately.

Rulings: live-b first; never both nodes down at once; proof on the live nodes (no spare disk, no rehearsal on a
copy); the API stays 0.94.2's; `maintenance.collect_garbage` stays false until the cutover check below.

## Gates before the first node

| Gate | State |
|---|---|
| Macha suite green on the deployed libraries | **done**: 545/545 at `8ef9496` on Dannan 0.9.2 and Dagda 0.3.0 (build-3, 2026-11-19 04:28Z) |
| Macha release | 0.95.0, `8ef9496` on `port/dannan-dagda` (release commit `fe74e9b`, then the Dagda 0.3.0 requirement), pushed; tag 0.95.0 on the maintainer's word |
| Dannan | 0.9.2 tag |
| Dagda | **done**: 0.3.0 tagged (local, `2a58202`, tree `af09d301` = the gated `4a4b322`): Release, ASan+UBSan, TSan gate passed |
| Mixed-version window | transports proved by Dannan's run (one cluster with the key; islands with a certificate alone); stores must not mix for long (Dagda and Macha, from code); plan: minutes of mixing (below) |
| Two-version smoke test | **passed** 2026-11-19 13:54 to 14:03Z (below) |
| Deploy tarball | **done**: `build/release-0.95.0/macha-0.95.0-arm64.tgz`, sha256 `7d9d1e0fef5b0875dc3c6b819de5709b8d6c8b0ceda04fe16adf492d1126a062`, 76 MB, from `8ef9496` on Dagda 0.3.0; binaries resolve inside it (`dagda-migrate --version` 0.3.0 on dannan 0.9.2) |
| Install script | `build/install-0.95.0.sh` (written; read it before the first use) |
| API announcement | none needed: 0.95.0 sends 0.94.2's API (`2026-11-18-port-api-announcement.md`) |
| 0.94.2 baselines | `build/baselines/results-0.94.2`; the comparison runs after each node is on 0.95.0 |
| The maintainer's decisions | the Dagda tag (given: 0.3.0), the off-node backup destination, the window, each node's go |

## What is deployed

- **Macha** 0.95.0, `fe74e9b` (or its tag), RelWithDebInfo, built on a claimed Pi.
- **Dannan 0.9.2**: `libdannan`, `dannan-custody`, `dannan-import-macha`, `dannan-version` (Macha embeds the node).
- **Dagda 0.3.0**: `libdagda`, `dagda-migrate`.
One tarball, `macha-0.95.0-arm64.tgz`, laid out under `/usr/local`, with its sha256.

## Config

Both live `macha.yaml` files load under the port with no unknown key (checked 2026-11-18,
`build/cutover/config_check.cpp`). The store node gets, on both: state `/var/lib/macha`, control store
`/var/lib/macha/metadata-objects` (8 GiB, packs 1 MiB into 64 MiB), backend `/mnt/diskB/data` (live-a 10 TiB limit,
live-b 9 TiB; 4 GiB reserve), cache `/var/cache/macha` (4096 blocks), replication 2, write copies 1, metadata write
copies 1, extent 4 MiB, refresh 5 s (the heartbeat), garbage grace 30 days, ledger cache 64 MiB,
`maintenance.collect_garbage` false. The install script adds `custody: { credential: dannan-root }`.

## The nodes (read 2026-11-19 00:30Z)

| | live-a (192.0.2.50, Pi 5 8 GB) | live-b (live-b.example.net, Pi 5 16 GB) |
|---|---|---|
| Running | 0.94.2 since 2026-11-13 21:47Z, health ok | 0.94.2 since 2026-11-13 21:10Z, health ok |
| DATA | 5.9T of 9.1T | 3.3T of 9.1T |
| `/var/lib/macha` | 5.5G | 7.7G |
| NVMe root free | 858G (manifests go here) | 858G (manifests go here) |
| systemd | 257, no TPM; `credential.secret` not yet made | the same |
| Present | `/root/stop-with-stacks.sh`, `/etc/macha/cluster.key`, `/var/cache/macha` | the same |

## The mixed-version window (open)

From live-b's stop until live-a runs 0.95.0, the nodes run different stacks: live-a 0.94.2 (Dannan 0.5.3,
Macha's store, v18 disk), live-b 0.95.0 (Dannan 0.9.2, Dagda's store, v19 disk). live-b is down for its migration
(Dagda's estimate for live-a's disk is about 22 h; live-b's is smaller, unmeasured), during which live-a serves
alone, single-copy writes (degraded operation is accepted). What happens once live-b is back on 0.95.0 depends on
the store answer still awaited from Dagda and on a two-version smoke test:
- **Transport (Dannan, from code, 2026-11-19; never run):** 0.5.3 and 0.9.2 share the wire (message, codec, record
  encoding identical; versions 1 and 2), and 0.9.2 admits a peer with no agreement table, so live-a is admitted.
  Two conditions: live-b keeps the cluster key and gets no certificate (a certificate without `use_cluster_key`
  offers only wire v2 and the pair refuse each other); live-b's advertise-with-deadline reports live-a `missed`
  until gossip carries the record (harmless unless Macha waits on it). Not checked by Dannan: the client's dialling and
  relay changes, the scheduler. **Run by Dannan 2026-11-19** (build-1, `dannan-node` 0.5.3 and 0.9.2 from their tags,
  loopback, 60 s each, logs in `../dannan/build/mixed-version-2026-11-19/`): with the cluster key on both and custody
  on 0.9.2, either side dialling, both lanes up, each lists the other active, echo on control and data both ways, no
  session ended; with a certificate alone on 0.9.2 they never became peers. Not covered: telemetry contents, advert
  pushes, Macha's and Dagda's types; those are this plan's smoke test.
- **Routes (checked here):** each hosted type's lane and frame rules must agree, or the receiver ends the session.
  Macha's own types are unchanged. Five store types differ between 0.94.2's registration and Dagda's: `delete_object`
  and `have_valid_objects` (0.94.2 any frame, Dagda not control), `bool_reply` (0.94.2 control lane, Dagda both
  lanes), `object_reply` (0.94.2 not control, Dagda any frame), `have_control_objects_reply` (0.94.2 control frame
  only, Dagda any). By the default frames the senders use, the requests stay within both, but a reply on the other
  lane or a promoted request would end the session. Dagda's own types (768 to 774) are unknown to 0.94.2 and not
  policed there. Dannan, 2026-11-19: the check applies to replies too, on both the sender's sealer and the receiver's
  opener, and a refusal ends the session (every call on it fails). A reply travels in its request's lane and class.
  The likeliest failure: 0.95.0 calls 0.94.2 in a class 0.94.2's reply rule forbids (`object_reply` to a request sent
  in the control class, `have_control_objects_reply` outside the control class, `bool_reply` to a data-lane
  request), and 0.94.2's own sealer ends the session. Not traced: Dagda's call sites' frame classes, promotion.
- **Store (Dagda, from code, 2026-11-19; never run): they must not mix for long.** What lines up: type numbers and
  protocol-23 payloads, Dagda serves every store type 0.94.2 sends, a principal-less put or claim is the sender's own,
  object ids, placement salts. What does not: (1) capacity adverts (`macha/capacity`, `macha/used` against
  `dagda/capacity`, `dagda/figures`), so each reads the other's capacity as 0 and ranks owners differently;
  (2) Dagda sends its own types to 0.94.2, which answers 501: its deletes (`remove_objects`) never take effect on
  live-a, its repair push (`offer_objects`) fails every pass, its pull of many falls back to single gets (works);
  (3) Dagda appends its writer to every put and claim, and 0.94.2's handlers end with `Reader::finish()`, which refuses
  trailing bytes (`src/codec.cpp:109`, checked here): **every put and claim from live-b to live-a fails**.
  Macha's metadata types are unchanged and do cross; reads across (`get_object`) work. Not checked: trie_diff
  answers across v18 and v19 ledgers. `dagda-migrate` is purely local.

**The two-version smoke test (2026-11-19, the maintainer's go; fresh test nodes, a few MB, nothing from the live nodes):** node A
Macha 0.94.2 (`main`, Dannan 0.5.3) on build-1 founding the cluster, as live-a; node B the 0.95.0 deploy tarball
itself (Dannan 0.9.2, Dagda 0.3.0, test custody from a credentials directory) on build-2, bootstrapping to A, as
live-b; replicas 2, write copies 1, metadata write copies 1, `collect_garbage` false. About 9 minutes mixed: each
wrote 8 files (1 to 6 MiB) through its FUSE mount, each read the other's back (16 of 16 sha256 match), each deleted 2,
then 3 minutes idle. Result: both online and healthy throughout, each lists the other `online` with its version;
metadata writable with `replicas=2/2`; no WARN or ERROR line on either (accounts' first-start notice aside); transport
connections made once and reused about 500 times on each, so no session ended; server rejections 0; prompt
replication 0 failures both ways; both data directories hold the same 19 large objects, so every write reached both
nodes. A answered 26 requests of types it does not know (Dagda's 768 to 774, answered 501 as Dagda predicted) without
any session ending. Settled by Dagda from code afterwards: B's prompt copies to A carry no writer, so they are
protocol 23's payload exactly and succeed (the 0 failures is real); B's write fan-out and its claims always carry its
writer, so those to A fail on the trailing bytes (500 from 0.94.2's dispatch, logged by Dagda at DEBUG only; the
server's `rejected_jobs` counts queue refusals, not handler errors, which is why it read 0). With write copies 1 the
writer's own copy meets the write, so nothing surfaced. During the window, then, live-a holds copies of live-b's
new objects with no claim from live-b; with `collect_garbage` false nothing collects them, and once live-a runs
0.95.0 its claims are its own. The window stays minutes. Not exercised: a promoted request, deletes reaching the other
node's store (garbage collection off). Logs in `build/smoke/logs/`; configs and scripts in `build/smoke/`.

**The plan for the window (recommended; the maintainer's choice at the go):** keep the mixed time to minutes. live-b migrates
while live-a serves alone on 0.94.2 (single-copy writes, accepted). live-b starts on 0.95.0 and is proved serving
(step 8), and live-a is stopped at once for its own migration, with live-b serving alone on 0.95.0 meanwhile.
What was written to only one node during either migration is unreachable from the other until both run 0.95.0 and
repair converges them (degraded operation, accepted). `maintenance.collect_garbage` stays false throughout, so
nothing is reclaimed on a partial view. The alternative, live-b left stopped until live-a's turn, is ruled out:
both would be down at once. The smoke test still runs first, to show the minutes of mixing end no session and crash
neither node.

## Per node (live-b, then live-a at once after live-b is proved serving)

Each step names its check; a failed check stops the node there and is reported before anything else runs. Record each
step's start and end time: live-b's migration is the first measurement.

1. **Stage** (node running): copy the tarball and its sha256 to `/root/stage-0.95.0/`; `sha256sum -c`; extract it to
   `/root/stage-0.95.0/root/` (not `/`). Copy `build/install-0.95.0.sh` beside it. Steps 4 to 6 run the new tools from
   there: `T=/root/stage-0.95.0/root/usr/local`, `LD_LIBRARY_PATH=$T/lib`, and under `systemd-run` the same with
   `--setenv=LD_LIBRARY_PATH=$T/lib` and `$T/bin/...` paths. Check: `$T/bin/dagda-migrate --version` says 0.3.0,
   `$T/bin/dannan-version` says 0.9.2.
2. **Stop**: `/root/stop-with-stacks.sh`. Check: `systemctl is-active macha` is `inactive`; the other node still
   answers `/api/v1/health`.
3. **Back up off-node**: `/var/lib/macha` (state and control store, not DATA) and `/etc/macha`, as one tarball with its
   sha256, to the destination the maintainer names. Check: the copy's sha256 matches at the destination.
4. **Check the transport files** (tarball's tools, run from the stage directory with `LD_LIBRARY_PATH` set to it):
   `dannan-import-macha --dry-run /var/lib/macha`. Expect "already" or "absent" for `membership/known-nodes.bin`,
   `connectivity/inbound.bin`, `telemetry/last-known.bin`. Any "imported": stop and send it to Dannan.
5. **Custody** (the stage's `dannan-custody`): `dannan-custody init --state /var/lib/macha --credential dannan-root | systemd-creds encrypt
   --name=dannan-root - /etc/credstore.encrypted/dannan-root`. Never pipe init's output anywhere else (that is the
   root in clear). This first encrypt also creates the host key `/var/lib/systemd/credential.secret`. Check: `systemd-run
   --wait --pipe -p LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root dannan-custody show --state
   /var/lib/macha --credential dannan-root` prints `{"id":...,"public_key":...}`. If init printed but encrypt failed:
   remove `/var/lib/macha/identity/custody` and run init again. **Custody backup** (off-node, offline, operator-held,
   one set per node, the three files together): `/etc/credstore.encrypted/dannan-root`,
   `/var/lib/systemd/credential.secret`, `/var/lib/macha/identity/custody`. Losing them after the migration loses the
   node's ledgers.
6. **Migrate** (Dagda's `docs/migration.md`), node stopped throughout, manifests on the NVMe root
   (`/root/migrate-0.95.0/`, about 1.2 GB for both). `M` is `systemd-run --wait --pipe -p
   LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root --setenv=LD_LIBRARY_PATH=$T/lib
   $T/bin/dagda-migrate --state /var/lib/macha
   --node-state /var/lib/macha --cluster-key /etc/macha/cluster.key --credential dannan-root --backend /mnt/diskB/data
   --control /var/lib/macha/metadata-objects --cache /var/cache/macha` (no `--credentials-dir`; `--wait` passes the
   exit code 0, 1 or 2 through):
   - `M --dry-run`. Check: exit 0.
   - `M --write-manifest /root/migrate-0.95.0/before`. Check: exit 0.
   - `M` (the migration; the long step). Check: exit 0.
   - `M --write-manifest /root/migrate-0.95.0/after`. Check: exit 0.
   - `dagda-migrate --compare-before /root/migrate-0.95.0/before --compare-after /root/migrate-0.95.0/after` (no
     key). Check: exit 0 and `RESULT: PASS`.
7. **Install and start**: `TARBALL=/root/stage-0.95.0/macha-0.95.0-arm64.tgz HEALTH=<api>/api/v1/health bash
   install-0.95.0.sh`. It refuses a running node or missing custody, proves the seal, installs, adds the unit drop-in
   (`LoadCredentialEncrypted=dannan-root:/etc/credstore.encrypted/dannan-root`) and the `custody` key, starts, and
   waits up to 300 s for health as 0.95.0; a node that fails is left stopped on 0.95.0 (see Rollback).
8. **Prove it serves**: `/api/v1/health` ok as 0.95.0; Status's store figures (capacity and used match the disk);
   `startup.phase` ready; a known file read end to end (first bytes and size); a catalogue artwork read; metadata
   writable; the other node's state as the mixed-version answer predicts; no `reopen on sight` line in the journal
   since start. Then the 0.94.2 baseline comparison (`build/baselines/`) for this node.

## After both nodes

- **The cutover check, then garbage collection**: on each node, the reachability provider's inventory against the
  namespace (every referenced object present in the inventory, nothing live in the garbage); only then
  `maintenance.collect_garbage: true`, on the maintainer's word.
- Repair converges the two nodes' copies; watch Status's repair section until it settles.
- On the maintainer's word: tag Macha 0.95.0, merge to develop and main, push; Dagda's and Dannan's pushes are theirs.

## Rollback (per node)

- **Before step 6's migration ran**: reinstall nothing; start 0.94.2 (`systemctl start macha`). Custody stays; 0.94.2
  ignores it.
- **After the migration, before or after step 7**: stop the node; `M --reverse` returns the disk and metadata to v18;
  then restore `/root/pre-0.95.0/` (0.94.2's binaries, unit and `macha.yaml`, saved by the install script) or, if step 7
  never ran, nothing to restore; start 0.94.2. The reverse reads every byte again: about as long as the migration.
- The other node keeps serving throughout; never stop it to recover this one.

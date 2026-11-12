# Active tasks

Last updated: 2026-11-08, on `develop`. **State a branch's content from its diff and a node's state from its
files, never from a prior note** (operator: "We can't afford drift like this."). The version before this
rationalisation is `archive/2026-11-08-ACTIVE-before-rationalisation.md`. Alongside: `BACKLOG.md` (everything
not in progress, by area), `COMPLETED.md`, `CLIENT-CONTRACTS.md`, `archive/` (a record, not requirements).

## Resume here

**The work is the port onto Dannan and Dagda. Read [`2026-11-08-port.md`](2026-11-08-port.md) first**: the
operator's rulings in his words, the order, where each library is, and everything the port must do.
[`2026-11-08-found-issues.md`](2026-11-08-found-issues.md) lists every issue found since 2026-11-06 and its owner.

**This session's job now:**
1. **Wait for both capability reports**, answering the libraries' questions meanwhile. Dannan's is in (0.5.0);
   its fixes for two faults this session found are on `feature/accept-faults`, for a 0.5.1 tag at the operator's
   word. Dagda is at S6 to S7, report to follow.
2. **Review both** by reading their code, not only their reports. Start from the items each report says it could not
   show, the seams in the port document, and the review notes there.
3. **Then build the port** on a branch off `develop` at or after `98b816d`, both libraries frozen, and prove it on the
   nodes, inverbeg first.

**Macha's code is frozen** until the port starts. Exceptions so far: 0.92.5, the old-code removal, 0.93.4 (all
deployed), 0.93.5 (`9153011`, `98b816d`: a damaged or unreadable `history.log` frame loses only itself) and 0.93.6
(a repair step that yielded to a viewer now wakes the loop again; it had slept up to 31 h). 0.93.6 carries both:
macnessa gate 755 + 20 + 21, live on both nodes since 2026-11-10 20:18Z, not tagged.

**Peers:** the Dannan and Dagda sessions, by `SendMessage` (`ListAgents` shows them). They work on their own
repositories, `../dannan` and `../dagda`; `../macha` is read-only to them.

## Waiting on the operator

Nothing open. Answered 2026-11-08:
- **Dannan 0.5.1**: yes, once the `shut()` handler fault is fixed and its gates pass (8dd73d2, 91a8c83 and the fix).
- **The performance comparison rule**: set on the port's first figures.
- **Window W1** and best-effort peer claims (found-issues, Macha item 5): after the port.
- **What filled the control store** (found-issues, item 3): after the port; the 8G limit stands meanwhile.
- **The cores on macnessa**: all six deleted 2026-11-08 14:06Z; capture to `/mnt/diskB/crash` stays on.
- Carried, unchecked, not now: the media-kind import hint; truncated pack records (counted read failures, report
  held, repair unchecked); the `*.pre-migration.*` archives; the replica journal reader, with no writer left.

## Open, in order (Macha, all after the freeze unless the operator says otherwise)

1. **The heap corruption and the FUSE `rm` crash** (found-issues 1, 2). Left until after the port. The first P0 of
   this series, a SIGSEGV on 0.92.4 at 2026-11-05 02:05:25Z, had no core; capture has been on since.
2. **A full control store is silent; work that cannot complete is retried without backoff** (found-issues 3).
3. **Cancelling an ingest job leaves its worker running** (found-issues 4).
4. **The stop.** The survey's and inventory walk's stop checks are in 0.93.4; the release horizon's census
   (`namespace_references`) and the tree diff under `census_follow` do not check it.
5. **Import filing:** about 430 real episodes probed as films and went to `/Movies`; folder names such as
   `Blade Runner ( (1982)` (found-issues 9).
6. **Books**, as the web client reported at 0.92.2: the Discworld series holds 3 of 49; three books send a
   timestamp `published`; 11 send `published` and `language` null, a possible regression, unexamined; "Le Guin,
   Ursula K." splits into two authors. Stage 3, per-user media state, waits on the freeze.
7. **Failing tests, each P0 when it recurs** (carried, not rechecked on the current nodes):
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`,
   `availability/test_two_nodes_survey_what_neither_holds`,
   `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor`,
   `filesystem_fuse/test_coalesced_delete_burst_wakes_at_exact_garbage_grace`,
   `rpc_cluster/test_rpc_slow_control_does_not_abort_data`,
   `rpc_cluster/test_inbound_incapable_node_is_reached_only_over_its_own_sessions`,
   `users/test_accounts_converge_across_nodes`,
   `rpc_cluster/test_rpc_metadata_executor_cancellation_disconnect_and_shutdown`. macOS torrent segfaults are the
   laptop's libraries: ignore there only.
8. The rest of found-issues, items 5 to 14.
9. **Status thread names, after the port** (operator, 2026-11-11): the port maps Dannan's threads to Macha's names in
   Status `threads[]`. Change every client (Core and each client session) to Dannan's real names, announced first,
   then drop the mapping.

**Reopen on sight:** `credit wait abandoned`; `metadata merge recorded a conflict`; `RPC stalled (control)`; heap
corruption (now seen on macnessa, 2026-11-06).

## Cluster state (2026-11-08)

- **Both on 0.93.6** since 2026-11-10 20:16Z (inverbeg) and 20:18Z (macnessa): 0.93.5's history fixes and the repair
  wake after a yield. Rollback: `/root/pre-0.93.6/`.
- **macnessa** (root@10.44.1.50, reached locally; hostname corvus-gbni-1; Pi 5, 8 GB; domain site-a): 0.93.6 under
  `/usr/local`; `/var/lib/macha`; DATA on `/mnt/diskB/data`, 6.3 TB used; control store
  `/var/lib/macha/metadata-objects`, limit 8G (was 4G, full from 2026-11-06 21:16Z to 2026-11-08 12:52Z); API on
  10.44.1.50:7438. The build node, sources in `/home/tom/macha`. NVMe SMART `FAILED` on its temperature flag only.
- **inverbeg** (root@inverbeg.macha.network, Pi 5, 16 GB, domain site-b, inbound capable, dialled at
  inverbeg.macha.network:7437): 0.93.6; DATA on `/mnt/diskB/data`, 1.68 TB used and filling; control store about
  4.0 GB, limit 8G; API on 192.168.1.50:7438.
- **Both:** `maintenance.idle_bandwidth_fraction` 0.5 (was 0.10); config backups beside `/etc/macha/macha.yaml`
  (`.pre-8g-*`, `.pre-fraction-*`). Debian 13, aarch64, Boost 1.83. Core capture to `/mnt/diskB/crash`. Torrent on,
  23 requests in the head. One tree-backed head, SM19 with the legacy clock set.
- **Access:** the API needs a session; anonymous sessions are off. macnessa's genesis root password is in
  `/var/lib/macha/initial-root-password` (root only); the same account works on inverbeg. Scripts that read it are in
  `build/baselines/` (`probe_repair.py`, `probe_state.py`, `baseline.py`).
- **Deploy:** build on macnessa with the log and `$?` written to a file; `DESTDIR=/root/stage-V cmake --install build
  --prefix /usr/local`; `tar czf /root/macha-V.tgz -C /root/stage-V usr`; on each node, one at a time,
  `/root/stop-with-stacks.sh`, then `build/install-V.sh` (from `build/install-0.92.3.sh`, untracked) with `TARBALL=`
  and `HEALTH=http://<api address>:7438/api/v1/health`. `build/deploy.sh` is stale. Rollback: `/root/pre-<version>/`.
  A config change: edit `/etc/macha/macha.yaml` with a backup beside it, then `build/baselines/restart_check.sh`
  per node, one at a time.
- **Laptop:** `build-nobooks` (rebuilt at 0.93.5) and `build-notorrent`, `-j8`, tests `--jobs 8` with `TMPDIR` under
  `build/tmp`. An Intel Mac; no torrent plugin. Scratch files and logs in `build/`. Code lookups through basemind.

## Standing rules

- **Laws** ([Principles and laws](../docs/principles-and-laws.md)): 1. Thou Shalt Not Make Control Wait. 2. Thou
  Shalt Not Make The Viewer Wait. 3. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer
  Wait. 4. Thou Shalt Not Shoot Thyself In The Foot.
- **Everything the API and the web serve is viewer work**, including the bytes still draining to a client.
- **Pace, never gate.** No subsystem waits for a higher class to go idle; a new signal feeds the existing pacer.
- **Local is always fast; the cluster converges behind.** A local operation never waits on a peer or walks the
  namespace.
- **No vacuous truths.** An empty set never satisfies "all" or "enough".
- **Three answers, HTTP numbers below as above**: "here it is", "cannot, and that will not change", or "cannot say
  now" (try later, bounded exponential backoff). Refusals carry an HTTP status and a snake_case code; nothing
  matches English text.
- **Proportion.** Account for and control what matters to the user's experience.
- **Testing**: test the change once (the touched tests on the laptop, then all three suites on macnessa, which are
  the gate), deploy, measure. No repeat runs or soak runs; sanitizers only as a targeted debugger. A laptop under
  heavy load gives no verdict on timing.
- **Report findings before fixing; prove a mechanism before editing.** Design questions go to the operator with a
  recommendation.
- **Every response has a snake_case code; errors add a message; clients own sorting and presentation; every API
  change is announced to Core and every client before it ships.**
- **Deploying**: build once on macnessa, run all three suites there, ship the `usr` tree; never both nodes down at
  once. After any rejected or interrupted command, check what actually ran.
- **Builds**: laptop `-j8` and tests `--jobs 8`. When a shared struct changes, rebuild every target. Never edit
  sources under a running build.
- **Reading the cluster**: correlate on the `Z` timestamp in the message. Record which host served a measurement.
- **The composition root is code**: concrete classes, constructor references, dependency order.
- **The DATA store is `StoragePool`** over one `LocalStore` per backend; the control store is a `LocalStore`.

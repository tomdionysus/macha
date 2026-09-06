# Self-healing programme — progress and resume file

This file is the resume point for any session (human or scheduled) working
the programme in `2026-09-06-self-healing-disciplines-plan.md`. Keep it
current at every checkpoint; a fresh session reads only this and the plan.

## Standing rules (from the operator, 2026-09-06)

- Load generation is **rsync only**, from `/mnt/diskA` on gbni-1
  (10.44.1.50) and es-1 (10.34.1.50) into the node's own FUSE mount
  `/mnt/machamedia`. `/mnt/diskA` **MUST NOT be modified** — read side only,
  never a destination, never a cleanup target. **Never use the ingest
  subsystem against `/mnt/diskA`** (untested; unknown behaviour).
- es-1 is shared with other users: never kill a process we did not start;
  run our rsync/build work under `nice -n 10 ionice -c3` there; keep builds
  at `-j2`.
- Commits allowed on `claude-work`, never push. Attribution trailer per the
  session's current instruction. Commit at every phase boundary and at any
  point where >1 h of work would otherwise be uncommitted.
- Cluster may be restarted, chaos-restarted, and **wiped from scratch** when
  state gets in the way. Wipe scope on all three nodes: `/etc/macha/state`,
  the FUSE journal (`/etc/macha/fuse-operations.log`), `/mnt/diskB/spool`,
  and the object stores under `/mnt/diskB`. Keep `/etc/macha/macha.yaml`,
  `cluster.key`, and any `*.bak-*` files.
- Frontend work goes to the separate agent **"Macha UI Work"** (SendMessage);
  brief it with the same rules and the API change in question.
- After each phase: checkpoint here + memory + commit, then tell the
  operator it is safe to `/compact`.
- Deployment shape: rsync tree (no `--delete`), `cmake --build build -j2`,
  `make -C build install`, `systemctl daemon-reload`, `systemctl restart
  macha.service`, one node at a time, health-check between. See memory
  `project-cluster-deployment` and `project-onbox-diagnostics`.

## Node facts

| node | ip | role | notes |
|---|---|---|---|
| corvus-gbni-1 | 10.44.1.50 | writer, 4 GB | `/mnt/diskA` source; temp `service_startup_timeout_ms: 1800000` in config (remove when discipline 2 lands); FUSE journal backup `fuse-operations.log.bak-20260906-dup` |
| corvus-gbni-2 | 10.44.1.51 | replica, 16 GB | no diskA |
| corvus-es-1 | 10.34.1.50 | writer, 8 GB, offsite (CEST), shared with other users | `/mnt/diskA` source; WAN ≈ 50–60 Mbps via WireGuard/EC2 |

## Phase status

- [x] Baseline: 0.28.3 on all nodes; plan committed (`273a5c4`).
- [x] **Discipline 1 — re-derive, don't assert (durability probe).** DONE:
  0.29.0 on all nodes (core `0888fe4ffff8`), UAT runs 1+2 recorded in
  `2026-09-06-self-healing-uat.md` (pass). Loose end noted there: one
  `transient=yes outcome="remote-reasserted"` line per run — a re-stamped
  requirement counted as not-yet-durable once; harmless, look at the
  `durable[key]` bookkeeping when touching the barrier next.
- [x] **Discipline 2 — one work-item retry policy + no-progress startup gate.** DONE: 0.30.0 on all nodes (`bb3697c`), UAT 2a/2b/2c recorded in `2026-09-06-self-healing-uat.md` (pass: 254 retries at 10/s → 16 retries then park at 22.8 s; operator retry drains; RPC deadline fires at 30 s; startup ceiling removed). gbni-1 config back on shipped defaults.
- [ ] Discipline 3 — resolve-on-recovery + journal fuzz fixture.
- [ ] Discipline 4 — compact tombstones/conflicts out of snapshots (+ DLT7).
- [ ] UAT record: `TODO/2026-09-XX-self-healing-uat.md` with before/after
  evidence per discipline; the demonstrative run is two concurrent rsync
  writers (gbni-1 and es-1 from their `/mnt/diskA`) with rolling restarts of
  every node mid-publication, showing no wedges, no re-sent extents, bounded
  retries visible in Status, sub-10 s restarts, and snapshot sizes
  proportional to the namespace.

## Discipline 1 — working notes

Goal: a barrier that fails on a stale placement token probes the peer for
the object ids, the peer verifies presence + flushes, returns a fresh token,
and the batch is re-stamped. Fallbacks: object absent on the peer → re-put
from a local copy if any → else discard the writer and replay from the spool.

Known evidence to reproduce: restart es-1 while gbni-1 publishes a big file;
gbni-1 then logs `quorum unavailable … replica=<es-1> outcome="remote-refused:
storage durability epoch changed"` forever (0.28.3 instrumentation).

Steps:
1. [x] Wire: `object_durability_barrier` request gains optional trailing
   `u32 count + count×32-byte ids`; server on epoch mismatch with ids →
   `reassert_durable()` each (has + flush), reply ok + epoch + fresh tokens.
2. [x] Client: `DistributedStore::durability_barrier(DurabilityBatch&, FrameType,
   std::vector<ObjectId>* unsatisfiable)` probes on `epoch changed` (and
   re-derives locally on a reopened backend), re-stamps replicas, reports ids.
3. [x] Writer: unsatisfiable → re-put from local copy else `ESTALE`; FUSE loop
   treats `ESTALE` as discard-writer-and-replay-from-spool.
4. [x] Tests: `storage_v18/test_durability_barrier_rederives_placement_after_peer_restart`,
   `…_reports_objects_a_restarted_peer_lost`; harness `restart()`. (Writer-level
   re-put test not written; covered live in step 5.)
5. [ ] Deploy 0.29.0 to all nodes; UAT: rsync a multi-GB file from
   `/mnt/diskA` on gbni-1 into its mount, restart es-1 mid-publication, expect
   `object durability re-derived … reasserted=N absent=0` on gbni-1 and
   `present=N/N` on es-1, publication completes, no `quorum unavailable`.
   gbni-1's stranded Pulp Fiction (inode 7666) clears on gbni-1's own restart.

Also in 0.29.0 (found by the suite while landing step 3): publication ENOENT
is re-derived from the decoded namespace view instead of poisoning the inode
(`publication_path_may_still_appear()`), race test 0/40, terminal test 0/10,
suite 341/341. Committed locally as 0.29.0.

## Discipline 2 — design notes (written while waiting on discipline 1's UAT)

Code facts gathered:
- `SubsystemRetryPolicy` (`subsystem_supervisor.hpp:26`): max_failures_in_window,
  failure_window, initial_backoff, max_backoff. Generalise to `RetryPolicy` in
  a new `retry_policy.hpp` with a small `RetryState` (consecutive failures,
  window deque, next_due) and `next_delay()` / `exhausted()`; the supervisor
  keeps its behaviour by adopting it.
- FUSE data loop retry: `fuse_frontend.cpp` ~3643 `if (retry) sleep 100ms` —
  fixed. Add `Inode::publication_retry` (RetryState) reset on success; delay
  = policy.next_delay(); on exhaustion → `inode->parked = {error, attempts,
  first/last failure}` and `backend_error` stays unset (parked ≠ poisoned);
  Status gets `filesystem.parked[]` (inode, path, error, attempts, since);
  manage API: `POST /api/v1/manage/filesystem/parked/{inode}/retry|abandon`.
- FUSE namespace loop: has 50 ms→5 s backoff (`:2851`) but no park except the
  operator skip on non-retryable errors; keep, and add the same park for a
  *retryable* error that exhausts the budget (blocked-op API already exists —
  extend it to list parked namespace ops).
- Startup: `Service::wait_services_ready` waits `service_startup_timeout`
  (`config.hpp:426`, default 120 s) then `_Exit(1)`. Replace with a
  no-progress gate: `NodeRuntime` exposes a monotonic `recovery_progress()`
  (bytes/frames/heads counters incremented by `recover_state` stages and by
  `MetadataReplica` load/materialise and the FUSE journal loader); the wait
  loop wakes every second and only kills when the counter has not moved for
  `service_startup_no_progress_ms` (default 120000). Keep
  `service_startup_timeout_ms` as an absolute ceiling (default 0 = none).
- RPC deadline: `net.cpp` "remains active while peer health is monitored" at
  ~2170/2190; add `rpc_no_progress_deadline` (default 30 s) for control-lane
  calls → fail with a distinct transient error.
- Log-rate rule: one line per backoff step.

Tests to write: FUSE publication against a peer refusing one object forever
→ parks within budget, other inodes unaffected, Status shows it, retry-now
works; namespace op exhausting its budget parks; startup gate does not fire
while the progress counter moves (simulate slow replay) and does fire when
it stops; RPC no-progress deadline returns within bound.

## Discipline 2 — status (0.30.0, code complete 2026-09-06 late evening)

Landed (see CHANGELOG 0.30.0 for the full list):
1. `src/retry_policy.hpp` — `RetryPolicy` + `RetryState`; supervisor adopts it.
2. FUSE data loop: per-inode `publication_retry` backoff, `Inode::parked`,
   `admit_deferred` skips backed-off inodes, `parked_publications()` /
   `retry_parked_publication()` / `abandon_parked_publication()`; Status
   `diagnostics.filesystem.parked_publications` +
   `publication_retries_backed_off`; manage API
   `GET /api/v1/manage/filesystem/parked-publications`,
   `POST .../{inode}/retry|abandon`. Config `fuse.publication_retry_*`.
3. Namespace loop: `fuse.namespace_retry_*` budget; exhaustion → EAGAIN
   `namespace_blocked_op`, keeps retrying at ceiling (ordered queue, no park).
4. Startup gate: `startup_progress.hpp` counter ticked by journal parse,
   delta apply, snapshot decode, storage scan, `mark_ready`;
   `service_startup_no_progress_ms` (120 s), `service_startup_timeout_ms` now 0.
5. RPC: `RpcClient::call(..., stall_notice, no_progress_deadline)`;
   `network.control_no_progress_deadline_ms` 30 s / `data_..._ms` 0.
Tests passing in isolation: `publication_backs_off_then_parks`,
`service_startup_gate_waits_while_recovery_progresses`,
`rpc_call_fails_after_no_progress_deadline`, plus all discipline-1 tests.

Discipline 2 is complete (all items below done 21:50; kept as the record of
what was run and where the on-box artefacts are: `/root/uat/d2-before.sh`,
`d2-after.sh`, `*.out`, `*-isolated.log` on gbni-1; UAT files under
`/mnt/machamedia/UAT/d2-before/`, `d2-after/` — leave them, they are part
of the namespace now). gbni-1's `/etc/macha/macha.yaml` is back to shipped
defaults (no `publication_retry_*`, no `service_startup_timeout_ms`).

- [x] Full suite green 345/345; committed `bb3697c` (0.30.0).
- [x] Deployed 0.30.0 to gbni-2 (21:01), es-1 (22:03 CEST), gbni-1 (21:03;
  booted in 3 s). gbni-1 config: temp `service_startup_timeout_ms: 1800000`
  removed; **UAT-only small budget added** under `fuse:` —
  `publication_retry_max_failures: 8`, `..._initial_backoff_ms: 250`,
  `..._max_backoff_ms: 5000` (backup `macha.yaml.bak-20260906-d2`). **Remove
  those three keys after the UAT** (restore the 100-in-30-min default).
- [x] UAT 2a before (0.29.0): `/root/uat/d2-before.sh` on gbni-1 — 90 s
  `nft reject` isolation; 254 retries at a flat 10/s. Recorded in UAT file.
- [~] UAT 2a after (0.30.0): `/root/uat/d2-after.sh` on gbni-1, log in
  `/root/uat/d2-after.out`. Isolation phase done: 16 retries, exponential
  backoff, both in-flight inodes (16565, 18307) parked at attempt 9 after
  22.8 s, listed by the API. Script continues: rsync 2 (Ghostbusters) must
  publish while parked, then it POSTs retry for the FIRST parked inode only
  → **after it finishes, POST retry for the other inode too** (both should
  drain; check `parked_publications=0`, spool retires). Record in UAT file.
- [ ] UAT 2b (RPC deadline): before evidence = es-1 0.29.0 `RPC stalled
  (control) … members … no_progress_ms=30000 … remains active` during the
  before-run (bounded only by dead_after), plus the 150–230 s
  `accept_metadata_commit` stalls from the plan doc. After: on gbni-1 add an
  `nft` **drop** (not reject) rule on *input* from 10.44.1.51 tcp dport 7437
  for ~60 s (do this only after d2-after.sh has finished — don't overlap
  faults) → gbni-2 should log `RPC made no progress for ~30000 ms (control)
  … cancelled for retry` and its next call succeed after the rule is removed.
- [ ] UAT 2c (startup gate): gbni-1 boots in 3 s now (journal replay linear
  since 0.28.3), so the gate is not stressed live; record that the elapsed
  ceiling is gone from config, `service_startup_no_progress_ms` governs, and
  cite `test_service_startup_gate_waits_while_recovery_progresses`.
- [x] Briefed "Macha UI Work" (msg 1e0c2bbd) on the parked-publications
  routes and status fields.
- [ ] Pre-existing WARNs seen on every boot (not 0.30.0): `FUSE async data
  publication failed inode=5806|922 error=missing` on gbni-1, `inode=2333`
  on es-1 — spool data missing for old journal entries; candidate for
  discipline 3 (resolve on recovery).
- [ ] Update UAT file + memory, commit, tell the operator `/compact` is safe.

## Next — Discipline 3: recover by resolving (+ journal fuzz fixture)

Read the plan doc's discipline-3 section first. Known live cases to drive
the design (all reproducible on the cluster today):
- Every boot on gbni-1 logs `FUSE async data publication failed inode=5806
  error=missing` and `inode=922 …` (WARN, 3× each since 19:24); es-1 logs
  the same for `inode=2333`, plus `FUSE journal recovery accepted data
  completion without published prefix inode=6435 sequence=65909` and
  `recovered durable FUSE operations pending=22997 namespace=0 inodes=1`.
  These are journal entries whose spool bytes are gone: recovery re-raises
  them on every start instead of resolving them once (tombstone the
  generation, or re-derive from the namespace whether the file is already
  published). Start by reading the recovery path in `fuse_frontend.cpp`
  (`replay_data_quantum`, the journal loader, `abandon_data`) and
  `metadata.cpp` `load_heads` / quarantine handling (memory
  `project-metadata-forensics` has the quarantine locations).
- Metadata quarantines from 2026-09-06 (see memory) — recovery that refuses
  a head instead of merging/resolving it.
Steps: (1) enumerate every `throw`/refuse in the FUSE journal recovery and
`MetadataReplica` load paths and classify resolve-vs-refuse; (2) resolve
each (drop-with-tombstone, re-derive, or quarantine-and-continue with a
status field), never re-raise on the next boot; (3) journal fuzz fixture:
truncate/corrupt/duplicate frames of a real journal copy
(`/etc/macha/fuse-operations.log.bak-20260906-dup` on gbni-1 is 129 MB —
copy a slice, don't move it) and assert recovery converges with no
repeated WARN across two boots; (4) CHANGELOG 0.31.0, docs, tests; deploy;
UAT = two boots of each node show zero repeated recovery WARNs and no
quarantine growth, with both rsync writers running.

## (original discipline-2 plan, kept for reference)

Start Discipline 2 from the design notes above: (1) `retry_policy.hpp`
(`RetryPolicy` + `RetryState`), adopt in `SubsystemSupervisor`; (2) FUSE data
loop backoff + park (`Inode::publication_retry`, `parked` state, Status
`filesystem.parked[]`, manage retry/abandon); (3) namespace loop park on
budget exhaustion; (4) startup no-progress gate (`recovery_progress()`
counter, `service_startup_no_progress_ms`); (5) RPC control-lane no-progress
deadline; tests for each; deploy; UAT = a peer refusing one object forever
(simulate by removing it on the peer after a restart) parks one inode within
budget while the rest of the cluster keeps publishing at full rate, and a
copy of gbni-1's 2026-09-06 state starts under the gate. Then brief the
"Macha UI Work" agent on the new Status/manage fields.

First live run (19:26): es-1's 6 s restart landed exactly on a barrier →
`remote-transport: send: Broken pipe` → the writer treated it as lost and
replayed the file from the spool. Fixed: transient outcomes are never
reported as unsatisfiable (`transient=yes|no` on the failure line); EAGAIN
keeps its own message. 342/342 after. gbni-1 is still publishing Pulp
Fiction (13.9 GB spool, ~8 MB/s) which holds the spool at the 16 GiB
ceiling and throttles any other writer there — rerun the UAT once that
spool has retired (spool_bytes on gbni-1 drops by ~13.9 GB).

**Evidence already captured (19:43, real workload):** gbni-1
`object durability re-derived after incarnation change reasserted=260
absent=0 peers=1`; es-1 `re-derived after epoch change present=260/260
expected=5b5b16ca current=714c3b85` — Pulp Fiction's 260 extents on es-1
re-stamped after es-1's restart, none re-sent. Recorded in
`2026-09-06-self-healing-uat.md` (run 1). Run 2 (clean, final cut, 2001: A
Space Odyssey, three spaced es-1 restarts) still to do after the deploy job
finishes (it waits for Pulp Fiction's spool to retire, then installs on
gbni-1 → gbni-2 → es-1).

Live UAT script for step 5:
1. On gbni-1: `nice -n 10 ionice -c3 rsync -a --inplace <one multi-GB file
   from /mnt/diskA> /mnt/machamedia/<dir>/` (source read-only).
2. While its publication runs (`write stage` lines on gbni-1), `systemctl
   restart macha.service` on es-1.
3. Expect on gbni-1: `object durability re-derived after incarnation change
   reasserted=N absent=0 peers=1`; on es-1: `object durability re-derived
   after epoch change present=N/N`; no `quorum unavailable`; publication
   completes (`data_publications_completed` advances; file readable on
   gbni-2 with the right size).

## Ready commit message

(committed) 0.29.0 — Re-derive durability from disk after a peer restart instead of retrying a dead token

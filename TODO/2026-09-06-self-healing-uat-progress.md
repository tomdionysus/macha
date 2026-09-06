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
- [ ] **Discipline 2 — one work-item retry policy + no-progress startup gate.** CODE COMPLETE as 0.30.0 (all five steps + tests + CHANGELOG + docs); full suite run, deploy and UAT still to do — see "Discipline 2 — status" below.
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

Remaining for discipline 2:
- [ ] Full suite green (known load-flakes: `replacement_node_recovers`,
  `mutual_bootstrap_prunes_cross_dial`, `status_collects_connected_peer_telemetry`
  — rerun in isolation before calling them failures).
- [ ] Commit 0.30.0.
- [ ] Deploy gbni-1 → gbni-2 → es-1. On gbni-1 remove the temp
  `service_startup_timeout_ms: 1800000` from `/etc/macha/macha.yaml` (the
  gate is now progress-based; backup exists as `macha.yaml.bak-20260906-startup`).
- [ ] UAT A (parking): start an rsync of one multi-GB file on gbni-1; on
  es-1 make one of its extents unpublishable-but-transient (e.g. `chmod 000`
  a backend dir briefly is too blunt — preferred: set a tiny budget on gbni-1
  via config `publication_retry_max_failures: 5`, restart es-1 repeatedly
  during the barrier, or block gbni-1→es-1 data lane with an nftables rule
  for >5 attempts) → expect `FUSE data publication parked` WARN, the inode
  in `parked-publications`, other rsync files still completing at full rate;
  then `POST .../retry` → completes. Record before/after in the UAT file.
- [ ] UAT B (startup gate): restart gbni-1 with its big journal and a
  120 s no-progress gate → boots in one attempt with `startup progress`
  lines; previously crash-looped 8× on the elapsed gate.
- [ ] UAT C (RPC deadline): grep for `RPC made no progress` after a peer
  restart mid-commit — should be seconds, not 150–230 s, and the caller's
  retry succeeds.
- [ ] Brief "Macha UI Work" (SendMessage) on the new status fields and the
  parked-publications routes.
- [ ] Update UAT file, memory, commit, then tell the operator `/compact` is safe.

## Next (original discipline-2 plan, kept for reference)

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

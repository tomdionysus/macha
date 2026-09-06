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
- [ ] **Discipline 1 — re-derive, don't assert (durability probe).** IN PROGRESS.
- [ ] Discipline 2 — one work-item retry policy + no-progress startup gate.
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

## Next

Discipline 1 step 5 (deploy 0.29.0 to all three, then the live UAT below),
checkpoint here, then start Discipline 2 (RetryPolicy + no-progress startup
gate).

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

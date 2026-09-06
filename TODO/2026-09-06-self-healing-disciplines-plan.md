# Self-healing disciplines: stop converting "not yet" into "forever"

Status: proposed 2026-09-06, after a day of live operation on the 3-node cluster.
Supersedes nothing; it reframes several open ACTIVE.md items as one programme.

## Why this exists

2026-09-06 produced six P0-class defects in one afternoon of ordinary load
(two concurrent rsync writers, a dozen routine restarts). Every one was real,
root-caused and fixed (0.28.2, 0.28.3) — and every one was only visible once
the previous one was fixed. That is the signature of a system whose failures
do not fail loudly, and it is why the day felt like whack-a-mole. The list:

| # | Symptom | Root cause | Habit |
|---|---------|-----------|-------|
| 1 | Every merge stored as a 15 MB full snapshot, 25 GB history.log | DLT6 encodes "absent conflicts" as "empty conflicts" | codec bug |
| 2 | 3–32 s write stalls after every merge | DLT5-era rule forced the next write to a full snapshot | leftover |
| 3 | FUSE mount two hours behind its own replica, on two nodes | a durable op re-confirmed by *re-observing its effect*, which a later change had overwritten | **A** |
| 4 | Node crash-looped 8× at the 120 s startup gate | quadratic tombstone replay (270 k tombstones per snapshot) | **D**, and **B** turned slow into dead |
| 5 | Node exited 2 s after ready, every 7 s, forever | recovery *threw* on two journaled inodes sharing a path | **C** |
| 6 | 14.5 GB spool never drains; WAN idle; writes throttled for 9 min | placement proven by a per-process epoch token; peer restart makes it permanently refusable; retried every 100 ms with no ceiling | **A**, **B** |

Plus, not yet fixed: three hot retry loops (`quorum unavailable`,
`namespace advanced`, `retention floor`) with no backoff; RPCs that "remain
active while peer health is monitored" for 230 s; a ~300 KB standing conflict
set carried in every merge delta; 1,600 files producing 15 MB snapshots.

What held: the content-addressed store, the durable FUSE journal and spool
(no data lost through ~15 restarts, a crash loop and a `kill -9`), the
metadata DAG (100+ concurrent-writer reconciliations, no divergence), and
plugin isolation. The foundations are right. The operational envelope is not.

## The four habits

**A. Bookkeeping is trusted over ground truth it could cheaply re-derive.**
"Bytes are on es-1's disk" is a durable fact. It is represented by
`durability_epoch_` (`cluster.cpp:143`, random per process) inside a token
that dies with the process, and `DistributedStore::durability_barrier()` has
no path to re-ask. Content addressing makes the truth one `has(id)` away. The
FUSE confirmation-by-visibility (#3) had the same shape.

**B. "Not yet" silently becomes "forever".** No retry in the FUSE data or
namespace loops has backoff, a failure budget, a parked state, or a status
row. The RPC layer logs stalls but never fails them. The only circuit breaker
is `service_startup_timeout_ms`, and it fires on *elapsed time*, so a slow but
progressing recovery becomes an infinite crash loop. `SubsystemSupervisor` is
the one component with a real policy (backoff → disable → report); nothing
else has one.

**C. Recovery refuses rather than resolves.** A stale journal descriptor
exited the process (#5). This morning's outage quarantined tens of GB of valid
history over one unreplayable head. Both WALs are designed to survive
inconsistency and then hand it to a constructor that throws.

**D. Snapshots carry retirement history, not just the namespace.** Every
generation embeds every tombstone and every unresolved conflict. Cost of read,
merge, replay and transfer therefore grows with *history*, forever, and today's
270 k / 15 MB / 300 KB are a clock, not a plateau.

## The programme: three disciplines, in this order

### 1. Re-derive, don't assert (habit A)

Rule: a durability, placement or confirmation check that fails against
recorded evidence probes the content-addressed truth and *re-stamps* the
evidence; it never retries the stale assertion.

- `object_durability_barrier` gains a probe form carrying the object ids
  behind the requirement. On `expected_epoch != durability_epoch_` the peer
  checks `local_store().has(id)` for each, runs an immediate durability flush
  on its current incarnation, and replies with a fresh
  `(epoch, domain, generation, backend_instance)` token. Old peers answer the
  extra bytes with an error and the caller falls back to option 1 below.
- `DistributedStore::durability_barrier(DurabilityBatch&)` (non-const): on
  `remote-refused: storage durability epoch changed` or a local epoch
  mismatch, probe; on success rewrite the replica in the batch so the next
  barrier is ordinary. Report unsatisfiable ids to the caller.
- `WriteHandle::commit()`: an unsatisfiable requirement (probe says the
  object is *not* present) marks its extents for re-put from the spool; the
  FUSE publication rewinds to those ranges. This is the fallback, not the
  normal path.
- FUSE namespace confirmation already moved to generation-based in 0.28.2;
  audit the remaining "observe the effect" sites (`confirm_data_from_snapshot`,
  `namespace_effect_confirmed` in the already-achieved-prefix check) and
  convert or justify each.

Acceptance: restart every node in turn while a 10 GB file publishes from a
fourth writer; publication completes with **zero re-sent extents** and zero
`quorum unavailable` lines after the probe lands. Regression tests: a barrier
against a restarted peer (new epoch) that still holds the object succeeds
without a put; one that lost the object triggers exactly one re-put of that
extent.

### 2. One work-item policy (habit B)

Rule: every retried unit of work has backoff, a failure budget, a parked
state visible in Status, and an operator action. No loop retries at a fixed
interval and no RPC waits without a deadline.

- Extract `SubsystemRetryPolicy` into a general `RetryPolicy` (initial
  backoff, ceiling, failures-in-window, park). Apply to: FUSE data publication
  (`replay_data_quantum` retries — today a fixed 100 ms), FUSE namespace
  publication (today 50 ms → 5 s, no park except operator skip), the
  `namespace advanced` and `retention floor` paths, and `repair_once`.
- Parked items surface in `/api/v1/status` `filesystem.parked` with inode,
  path, last error, attempts, first/last failure; and in a single WARN per
  park (not per attempt). Operator actions: retry-now, abandon.
- RPC deadlines: a control-lane call that makes no progress for
  `rpc_no_progress_deadline` (default 30 s) fails with a distinct error the
  caller can treat as transient; the "remains active while monitored" state
  becomes bounded. Data-lane transfers keep the stall/spill logic.
- Startup: replace the elapsed-time kill with a no-progress detector.
  Recovery reports a monotonic progress counter (journal bytes replayed,
  history frames applied, heads materialised); the gate fires only when the
  counter has not advanced for `service_startup_no_progress_ms` (default
  120 s). Remove the temporary 1800000 ms budget from gbni-1's config when
  this lands.
- Log-rate rule: a retried failure logs at most once per backoff step.

Acceptance: inject a peer that refuses one object forever; the affected
inode parks within its budget, Status shows it, other inodes' throughput is
unaffected (measure: extent quorums/min within 5 % of baseline), and the
journal carries ≤ 10 lines for it per hour. Startup on a copy of gbni-1's
2026-09-06 state (129 MB journal, 2 heads) completes without the gate firing.

### 3. Recover by resolving (habit C)

Rule: `initialise_namespace`, `load_history`/`load_heads`, and journal
parsing never throw on an inconsistency that has a deterministic resolution;
they resolve, log the pair, re-journal the outcome, and count it in Status.

- Inventory every `throw` in the two recovery paths (`fuse_frontend.cpp`
  3921–4080, `metadata.cpp` `load_history`/`load_heads`/`reconcile_recovery`)
  and classify: *resolvable* (duplicate path — done in 0.28.3; orphan spool;
  done-marker without op; delta whose parent is a peer's), *degradable*
  (unreplayable head — keep certificate, repair live, done in 0.26/0.27) or
  *genuinely fatal* (key mismatch, header corruption). Only the last may throw.
- Hand-built-journal test fixture so each resolvable case has a regression
  (today's duplicate-path fix ships without one for exactly this lack).
- Find the producer of the duplicate path (`unlink`/`rename` journal the
  descriptor *before* clearing `current_path`; something retired the op
  without the descriptor being re-journaled) and close it at the source too.

Acceptance: fuzz the journal (truncate at every frame boundary, duplicate
any frame, drop any marker) and the frontend starts every time, with the
expected WARN and no data loss for ops with a `published` marker.

### 4. Compact history out of the hot path (habit D)

Rule: a snapshot's size is a function of the live namespace. Retirement
history and resolved conflicts live in separately compacted, separately
replicated structures.

- Tombstones: move `MetadataSnapshot::garbage` to a per-node retirement log
  keyed by `retirement_id`, merged by union on reconciliation, with GC
  compaction once the sweep has consumed them (the retention/reachability
  machinery already decides when physical deletion is safe; it only needs the
  tombstone to be *findable*, not carried in every generation).
- Conflicts: resolved conflicts leave the snapshot; unresolved ones are
  capped and surfaced (Status + `manage` API) so a standing set of 300 KB
  cannot silently persist — today nobody knows the cluster has one.
- DLT7 (presence flags) so a merge delta carries only what changed
  (`2026-09-06-dlt7-presence-flags-for-branch-topology.md`).
- Reconciliation frequency: two writers today merge on almost every commit.
  Evaluate per-writer sequencing (a writer publishes onto the latest accepted
  head after a short wait for in-flight peers) before paying for a cleverer
  merge.

Acceptance: snapshot bytes ≤ 1 KB × live entries + fixed overhead on the
production namespace (today 15 MB for 1,600 entries); merge delta for a
conflict-free reconciliation back under 1 KB; cold start on the production
history under 5 s (achieved by 0.28.3 for replay; must survive the format
change).

## Ordering and sizing

1 → 2 → 3 → 4. 1 and 2 are each roughly a day and unblock the cluster's daily
use; 3 is a week with the fuzz fixture; 4 is the largest and the one that
matters most for the next month — start its design while 1–3 ship. Do not
ship 4 without 3: a format change with refuse-to-start recovery is how today
happened.

## Out of scope, deliberately

- WAN throughput (single TCP stream per peer/lane at 61 ms RTT through the
  WireGuard/EC2 relay ≈ 50–60 Mbps; BBR and multi-stream are separate).
- Per-path adoption in the FUSE frontend while local ops are queued
  (filed; a lag, not a wedge).
- Unlink/rename-over pruning of queued data publication (filed; efficiency).

## Risks

- Discipline 1 changes the durability contract: "present after restart" is
  treated as durable. Justified by the store's pack validation on open plus
  the explicit flush in the probe; write it down in `docs/operations.md`.
- Discipline 2's RPC deadline can turn a slow WAN into visible errors where
  today it is invisible waiting. That is the point, but tune the default on
  the real link before enabling it everywhere.
- Discipline 4 is a wire and on-disk format change; it needs the rolling
  compatibility story that DLT5→6 had, and the fuzz fixture from 3 first.

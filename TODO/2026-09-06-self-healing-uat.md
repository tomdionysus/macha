# Self-healing disciplines — UAT record

Companion to `2026-09-06-self-healing-disciplines-plan.md`. One section per
discipline: the failure as it happened on the cluster before, the fix, and
the same scenario replayed live after, with the journal lines that prove it.
Load is `rsync` from `/mnt/diskA` (read-only) into each node's FUSE mount.

## Discipline 1 — re-derive, don't assert (0.29.0)

### Before (2026-09-06, 0.28.3)

gbni-1 publishing *Pulp Fiction* (inode 7666, 13.9 GB spool) had placed 13
extents on es-1. es-1 was restarted at 18:13. From then on, every barrier:

```
object durability quorum unavailable id=… required=1 durable=0
  replica=333c0e3f20fa epoch=47b4f2e8 outcome="remote-refused: storage durability epoch changed"
FUSE async data publication retry inode=7666 error=object durability quorum unavailable before publication
```

~35 attempts/s once the node was otherwise idle; es-1 logged the matching
`barrier refused: epoch changed expected=47b4f2e8 current=09007637` 1,677
times in ten minutes. The bytes were on es-1's disk throughout. Every
restart of any node did this to every in-flight publication elsewhere.

### After (0.29.0)

Scenario: on gbni-1, `rsync --inplace` of
`/mnt/diskA/Movies/12.Monkeys.1995.1080p.BluRay.x264.AAC5.1.mp4`
(2,566,569,827 bytes) into `/mnt/machamedia/UAT/`; once its extents were
landing on the peers, `systemctl restart macha.service` on es-1.

Expected: on gbni-1 `object durability re-derived after incarnation change
reasserted=N absent=0 peers=1`; on es-1 `object durability re-derived after
epoch change present=N/N`; no `quorum unavailable` that is not immediately
re-derived; the file reaches its full size on gbni-2.

Observed, run 1 (19:26–19:44, first 0.29.0 cut): the es-1 restart landed
on a barrier in flight — `outcome="remote-transport: send: Broken pipe"` —
and that cut treated a transport failure as a loss and replayed the UAT
file from its spool. Fixed the same hour (transient vs definitive, see
CHANGELOG). Then, unplanned and on the real workload: *Pulp Fiction* (inode
7666, 13.9 GB, the file that was wedged all of yesterday) reached a barrier
whose batch still named es-1's pre-restart epoch, and gbni-1 logged

```
19:43:13 INFO object durability re-derived after incarnation change reasserted=260 absent=0 peers=1
```

with es-1 answering `object durability re-derived after epoch change
present=260/260`: 260 extents (~1 GB already on es-1's disk) re-stamped in
one probe, none re-sent, publication continued. Under 0.28.3 that batch
retried `epoch changed` forever.

Known remaining cost, seen while deploying the final cut: restarting the
*writer* itself (gbni-1 at 19:48, Pulp Fiction at 99%) recovers the
publication from the spool and re-puts from its last durable checkpoint —
it resumed at ~4.1 GB of 13.9 GB, ~121 extent quorums in the first three
minutes, all for objects the peers already hold. A peer restart now costs
nothing; a writer restart costs a re-send. The fix is the present-content
skip already planned (`2026-09-06-skip-redundant-replica-writes-for-present-content-plan.md`):
check `have_objects` before `put_deferred` on recovery replay.

Observed, run 2 (19:49–19:59, final 0.29.0 on all nodes):
`2001.A.Space.Odyssey…mp4` (2,558,024,294 bytes) rsync'd on gbni-1;
es-1 restarted at 19:49:56, 19:51:03 and 19:52:12 (ready in 6, 8 and 6 s).

gbni-1:
```
19:51:56 INFO object durability re-derived after incarnation change reasserted=60 absent=0 peers=1
19:53:11 INFO object durability re-derived after incarnation change reasserted=4 absent=0 peers=1
```
es-1: `object durability re-derived after epoch change present=4/4
expected=714c3b85 current=1ecb27ac`, its refusal log showing the batch had
accumulated tokens from four dead epochs (`714c3b85`, `bc7f524c`,
`a193421c`, `93d93d9b`) across the three restarts — all re-stamped.

Totals for the run: `re-derived=3 quorum-unavail=2 retries=1 replays=0
re-put=0 ERR=0`; 515 extent quorums; the file at its full size on gbni-2
at 19:59:50 (≈4 MB/s while gbni-1 was also re-sending Pulp Fiction).
One imperfection worth a look later: one `quorum unavailable … transient=yes
outcome="remote-reasserted"` line, i.e. a requirement re-stamped in the same
call still counted as not durable once and cost one extra retry.

**Verdict: pass.** Restarting a peer mid-publication no longer strands the
publication, and it does not re-send a byte.

### Tests

- `storage_v18/test_durability_barrier_rederives_placement_after_peer_restart`
- `storage_v18/test_durability_barrier_reports_objects_a_restarted_peer_lost`
- `filesystem_fuse/test_fuse_durable_journal_recovers_ordered_mutations` (0/40
  after the ENOENT re-derivation; was 1/4 failing)
- `filesystem_fuse/test_fuse_terminal_recovery_failure_is_not_readmitted` (0/10)

## Discipline 2 — one work-item policy (0.30.0)

Three habits in one discipline: a retry with no budget, a wait with no
progress condition, and a startup gate on elapsed time. Each has its own
before/after below.

### 2a. Publication retry — before (2026-09-06 20:59–21:02, 0.29.0 on gbni-1)

Scenario (`/root/uat/d2-before.sh` on gbni-1): `rsync --inplace` of
`Blade.Runner.2049…mp4` (3.3 GB) into `/mnt/machamedia/UAT/d2-before/`;
45 s in, an `nft` rule rejects every outbound connection from gbni-1 to
both peers' RPC port for 90 s, then is removed. The cluster is otherwise
untouched (no peer is restarted).

Observed: for the first 64 s the in-flight barrier waited on the peers
(nothing logged). Once membership marked them dead the publication failed
with `quorum unavailable … outcome="remote-not-sent"` and the writer
retried it on the fixed 100 ms sleep — **a flat 10 attempts/s, 254 retries
of the same inode in the remaining 26 s**, 550 log lines in the window,
peak 21 lines/s. There is no backoff, no budget, no operator signal: had
the rule stayed, so would the loop, forever, at 10/s, holding one of the
publication slots and burying the log. (This is the same loop that ran at
~35/s for a day on *Pulp Fiction* under 0.28.3.) When the rule was removed
the next attempt succeeded and the file published normally.

```
21:01:31 DEBUG object durability quorum unavailable id=63716277… required=1 durable=0 transient=yes replica=333c0e3f20fa outcome="remote-not-sent"
21:01:31 DEBUG FUSE async data publication retry inode=16565 error=object durability quorum unavailable before publication
… ×254, 10/s, until 21:01:57
```

### 2a. Publication retry — after (2026-09-06 21:04–, 0.30.0 on all nodes)

Same scenario (`/root/uat/d2-after.sh`), same file, same 90 s `nft reject`
isolation of gbni-1 from both peers, with gbni-1 configured for a small
budget so the park is reachable inside the window
(`publication_retry_max_failures: 8`, backoff 250 ms → 5 s; the shipped
default is 100 failures in 30 min, 250 ms → 30 s). Two publications were in
flight when the peers went dead: the after-run's copy (inode 18307) and the
before-run's copy, which gbni-1's 0.30.0 restart had re-published from its
spool (inode 16565).

Observed: **16 retries in the whole window instead of 254**, each one
logged with its backoff and attempt count, both inodes backing off
independently, and both parked with one `WARN` each 22.8 s after their
first failure:

```
21:06:27 DEBUG FUSE async data publication retry inode=16565 error=object durability quorum unavailable before publication retry_in_ms=250 attempts=1
21:06:27 DEBUG … inode=16565 … retry_in_ms=500 attempts=2
21:06:28 DEBUG … inode=18307 … retry_in_ms=250 attempts=1
21:06:28 DEBUG … inode=16565 … retry_in_ms=1000 attempts=3
21:06:29 DEBUG … inode=16565 … retry_in_ms=2000 attempts=4
21:06:31 DEBUG … inode=16565 … retry_in_ms=4000 attempts=5
21:06:35 DEBUG … inode=16565 … retry_in_ms=5000 attempts=6
21:06:40 DEBUG … inode=16565 … retry_in_ms=5000 attempts=7
21:06:45 DEBUG … inode=16565 … retry_in_ms=5000 attempts=8
21:06:50 WARN FUSE data publication parked inode=16565 path=/UAT/d2-before/Blade.Runner.2049…mp4 attempts=9 failing_for_ms=22758 error=object durability quorum unavailable before publication
21:06:50 WARN FUSE data publication parked inode=18307 path=/UAT/d2-after/Blade.Runner.2049…mp4 attempts=9 failing_for_ms=22757 error=…
```

89 log lines in the window (550 before). After the rule was removed the
parked inodes stayed parked — that is the point: the fault may have
cleared, but nine failures in 23 s is the operator's call, not a reason to
spin — and the API showed them:

```
GET /api/v1/manage/filesystem/parked-publications
{"parked":[{"inode":16565,"path":"/UAT/d2-before/Blade.Runner.2049…mp4","error_code":5,
  "error_message":"object durability quorum unavailable before publication","attempts":9,
  "failing_for_ms":29606,"parked_for_ms":6848,"pending_bytes":3348105554},
 {"inode":18307,"path":"/UAT/d2-after/Blade.Runner.2049…mp4", … "attempts":9, …}]}
diagnostics.filesystem: parked_publications=2 publication_retries_backed_off=16
```

_(rest of the run — a second rsync publishing to completion while the two
stay parked, then `POST …/16565/retry` and `…/18307/retry` releasing them —
pending.)_

### 2b. RPC no-progress deadline

_(pending)_

### 2c. Startup gate

_(pending)_

## Discipline 3 — recover by resolving

_(pending)_

## Discipline 4 — compact history out of the hot path

_(pending)_

## The demonstrative run

_(pending: two concurrent rsync writers, gbni-1 and es-1 from their own
`/mnt/diskA`, rolling restarts of every node mid-publication; no wedges, no
re-sent extents, bounded retries visible in Status, sub-10 s restarts,
snapshot sizes proportional to the namespace.)_

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

## Discipline 2 — one work-item policy

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

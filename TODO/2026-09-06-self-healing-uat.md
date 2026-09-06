# Self-healing disciplines — UAT record

Companion to `2026-09-06-self-healing-disciplines-plan.md`. One section per
discipline: the failure as it happened on the cluster before, the fix, and
the same scenario replayed live after, with the journal lines that prove it.
Load is `rsync` from `/mnt/diskA` (read-only) into each node's FUSE mount.

## Discipline 1 — re-derive, don't assert (0.29.0)

### Before (2026-09-06, 0.28.3)

node-c publishing *Pulp Fiction* (inode 7666, 13.9 GB spool) had placed 13
extents on node-e. node-e was restarted at 18:13. From then on, every barrier:

```
object durability quorum unavailable id=… required=1 durable=0
  replica=333c0e3f20fa epoch=47b4f2e8 outcome="remote-refused: storage durability epoch changed"
FUSE async data publication retry inode=7666 error=object durability quorum unavailable before publication
```

~35 attempts/s once the node was otherwise idle; node-e logged the matching
`barrier refused: epoch changed expected=47b4f2e8 current=09007637` 1,677
times in ten minutes. The bytes were on node-e's disk throughout. Every
restart of any node did this to every in-flight publication elsewhere.

### After (0.29.0)

Scenario: on node-c, `rsync --inplace` of
`/mnt/diskA/Movies/12.Monkeys.1995.1080p.BluRay.x264.AAC5.1.mp4`
(2,566,569,827 bytes) into `/mnt/machamedia/UAT/`; once its extents were
landing on the peers, `systemctl restart macha.service` on node-e.

Expected: on node-c `object durability re-derived after incarnation change
reasserted=N absent=0 peers=1`; on node-e `object durability re-derived after
epoch change present=N/N`; no `quorum unavailable` that is not immediately
re-derived; the file reaches its full size on node-d.

Observed: _(filled in from the live run)_

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

_(pending: two concurrent rsync writers, node-c and node-e from their own
`/mnt/diskA`, rolling restarts of every node mid-publication; no wedges, no
re-sent extents, bounded retries visible in Status, sub-10 s restarts,
snapshot sizes proportional to the namespace.)_

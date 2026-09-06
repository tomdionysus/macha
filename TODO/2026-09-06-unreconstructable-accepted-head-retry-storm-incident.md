# Unreconstructable accepted metadata head: retry storm, then the real cause

Status: **root cause found and fixed in 0.27.0** (2026-09-06), with live
detection and repair added so the failure class cannot wedge a node again
regardless of cause. 0.26.1 and 0.26.2 (retry-storm bounding) remain in
place as defence in depth. Remaining follow-up at the bottom.

## What happened

While stress-testing the same-day 0.26.0 retention-batching fix with a
deliberate concurrent-write load (movies rsynced via `corvus-gbni-1`, TV via
`corvus-es-1`, into the same namespace at once), both writer nodes fully
stalled at 2026-09-06 05:03:42 (es-1 clock): zero metadata mutations, zero
object writes, 5+ hours, no self-recovery. `corvus-gbni-2` (not a write
target) stayed healthy. Both stuck nodes spun at 2-47 lines/second on:

```
WARN media information publication failed: catalogue metadata unavailable: accepted metadata head cannot be reconstructed
WARN ignoring metadata head without a valid acceptance certificate hash=5e6ae738a6c3b9c843e8d8d38987019b80bb3cf2b14b1a4ccc0887562e2155f5
```

A restart onto 0.26.1 took the predicted recovery-seed path on both nodes:
every metadata file quarantined as `*.corrupt.1788689414780765552` (es-1)
and `*.corrupt.1788689408765265075` (gbni-1), reseed from the persistent
cache at gen 8014, resync to gen 8102 from peers. gbni-2 also carries an
older quarantine from 2026-09-04 (`*.corrupt.1788533125894846140`, 25 GB
history.log) -- the same failure class had already happened once before.

## Root cause (confirmed from on-disk evidence, not inference)

A new read-only decoder, `macha-metadata-dump` (`tools/
metadata_history_dump.cpp`), was built on each node and run against the
quarantined files with `/etc/macha/cluster.key`. All three quarantines show
the identical signature:

| node | stuck head | body | primary parent | gap |
|---|---|---|---|---|
| es-1 (2026-09-06) | `5e6ae738…` gen 8010 | delta | `37d691c7…` gen 8004 (full) | 6 |
| gbni-1 (2026-09-06) | `5e6ae738…` gen 8010 | delta | `37d691c7…` gen 8004 (full) | 6 |
| gbni-2 (2026-09-04) | `9bf75322…` gen 4969 via `98c1cd3f…` gen 4964 | delta | `11df29a2…` gen 4957 (full) | 7 |

The stuck head is always a **reconciliation merge commit stored as a delta
whose primary parent is several generations older**. That shape is
legitimate: `MetadataManager::read_group()` numbers a merge
`max(left, right)+1` and picks the *lower-hash* parent as `previous` so
concurrent reconcilers produce byte-identical merges
(`metadata_manager.cpp`, "Use the lower hash as the primary parent"). Every
history writer accepted it -- `store_commit()` and `import_history()` check
`parent.generation < child.generation`, `load_history()` even carries a
comment explaining that the primary "can therefore be more than one
generation behind". But both reconstruction walks (`materialized_locked()`
and `materialized()`, `metadata.cpp`) demanded
`child.generation == parent.generation + 1`.

The writers never noticed because their own validation applied the delta to
the parent fetched via `materialized()`, which hit the cache; the resulting
merge was then cached too. So the merge read back fine until the 64-entry /
128 MB materialization cache evicted it, at which point it was permanently
unreconstructable -- and being an accepted head, every reconciliation that
could have folded it into the main line had to materialize it first. On
restart, `load_heads()` hit the same walk and threw, quarantining the whole
replica. The existing test
`test_merge_delta_primary_may_precede_merge_generation` encodes this case as
*allowed* and passes on 0.26.x only because its reopened checkpoint *is* the
merge head, which re-seeds the cache before `load_heads()` runs.

Why the non-writing node survived: the merging node stores its own merge as
a **full** body (see "Not fixed" below), and ships the compact delta to its
peers. gbni-2 authored the surrounding merges; es-1 and gbni-1 imported the
delta.

## Fix (0.27.0)

1. **Prevent.** One shared predicate, `metadata_delta_succession_valid()`
   (`metadata.hpp`), used by all three writers and both readers. Regression
   test `test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen`
   uses a 1-byte materialization cache so nothing is served from cache; on
   0.26.x it fails at the first `accept_commit()` of the merge. The real
   quarantined histories on all three nodes decode as `anomalies=0`, every
   head `reconstructible=yes` under the fixed predicate.
2. **Detect.** `MetadataReplica::diagnose_unreconstructable_locked()` names
   the first break in the chain; it is in the exception message and in a
   single WARN/ERROR per episode.
3. **Recover live.** `load_heads()` keeps and flags an unreconstructable
   certificate instead of throwing. `MetadataManager::
   repair_unreconstructable_heads()` (maintenance cycle) fetches the record
   from any peer as a full body via the new `get_metadata_history_record`
   RPC (`MetadataReplica::full_history_record()`) and
   `MetadataReplica::reanchor_history()` appends it as a new full anchor,
   superseding the unreplayable frame in the index (`load_history()` prefers
   a full frame over a same-identity delta on restart). Tests:
   `test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live`,
   `rpc_cluster/test_unreconstructable_accepted_head_is_repaired_live_from_a_peer`.

## Earlier mitigations, still in place

- 0.26.1: 30 s per-hash cooldown in `refresh_materialized_head_in_memory_locked()`
  and `accepted_heads()` (`unreconstructable_head_retry_at_`), so a broken
  head is excluded from reads rather than re-thrown on every call. Now also
  the flag the live repair consumes.
- 0.26.2: same cooldown for `MetadataManager::read_group()`'s peer
  certificate survey (`unacceptable_head_retry_at_`). Still without a
  dedicated regression test.

## Not fixed, worth doing

- ~~**`WARN local metadata delta rejected; retrying full record generation=N`**~~
  **Fixed in 0.28.2.** The guess above (evicted parent, cured by 0.27.0) was
  wrong: the WARN kept firing on the 0.28.0 processes. What the 24 h journal
  on gbni-1 actually showed was a perfect split -- every
  `histories reconciled ... history_body=delta conflicts=0` was preceded by
  the WARN, no `conflicts=1` or `full` one ever was -- and
  `macha-metadata-dump --all` showed *peer* copies of those merges stored as
  15 MB `full` frames too (gen 8725, 8728 on gbni-1), so "accepted by peers"
  was also wrong; peer rejection just logs at DEBUG. Cause: DLT6 encodes
  `replace_merge_parents`/`replace_conflicts` as bare lists, and the decoder
  reads an absent set back as "replace with nothing". A merge whose parents
  share an unresolved conflict changes `merge_parents` but not `conflicts`,
  so `metadata_delta()` sent only the former, the replay dropped the
  standing conflict, and the exact-reconstruction check rejected it on every
  replica. `metadata_delta()` now sets both or neither and the encoder
  refuses one without the other
  (`test_metadata_merge_delta_preserves_standing_conflicts`). The same
  release also stops forcing the first write after a merge into a full
  snapshot (`clear_merge_parent_topology`, a DLT5-era rule: 42 of 212
  commits on gbni-1 in one day, 3-32 s each) --
  `test_metadata_delta_child_of_merge_commit_reconstructs` and the tail of
  `test_service_same_generation_sibling_notice_triggers_reconciliation`.
  `MetadataReplica::store_commit()` now logs the rejection reason at DEBUG.
- `import_history_from_peer()` fast path trusting `history_contains()` as
  "materializable" (flagged in the 0.26.1 write-up). Less urgent now that a
  head that turns out unreplayable is repaired live rather than wedging the
  node, but still a trust gap.
- `load_history()` still fails startup outright on an undecodable
  *non-final* frame (AES-GCM failure mid-file). With re-anchoring in place a
  more tolerant policy (skip the frame, let live repair supply the record)
  is now possible; not done here.
- Regression test for 0.26.2's cooldown.

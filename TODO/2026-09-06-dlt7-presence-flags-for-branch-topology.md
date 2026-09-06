# DLT7: presence flags for merge_parents / conflicts in compact deltas

Status: open (found 2026-09-06 while fixing the delta-rejection storm, 0.28.2)

## Problem

DLT6 encodes `replace_merge_parents` and `replace_conflicts` as two bare
lists with no presence flag, so the decoder cannot tell "absent" from
"empty" and installs both unconditionally. 0.28.2 made the encoder honest
about that -- `metadata_delta()` sets both or neither, and
`encode_metadata_delta()` refuses one without the other -- which is what
stopped every conflict-free merge (and the first write after any merge)
from falling back to a 15 MB full snapshot on every replica.

The cost of that shape: whenever `merge_parents` changes (every merge, and
the write that follows it), the delta carries the *entire standing conflict
set* even though it did not change. On the cluster today that set encodes to
~305 KB (`histories reconciled generation=8766 history_body=delta
history_bytes=305142 conflicts=0`; gbni-2's history.log shows the merge and
the post-merge write as 305142- and 304997-byte frames). Before the fix the
same conflict-free merge was a 244-byte delta -- rejected, but 244 bytes.
So ~100 merges/day x ~300 KB x 3 replicas of history growth and WAN
transfer that carries no information, growing linearly with the conflict
set. Still 50x better than the snapshot it replaced; not free.

## Fix

Add DLT7 = DLT6 plus one `u8` flags byte before the two lists:

- bit0: merge_parents present (list follows)
- bit1: conflicts present (list follows)

`metadata_delta()` sets each independently again (only what changed).
`encode_metadata_delta()` emits DLT7 whenever either is set; the "both or
neither" guard becomes DLT6-only (keep it: `import_history`/replay of
existing DLT6 bodies must keep the unconditional-install semantics, which is
what makes every DLT6 frame already on disk stay valid).
`decode_metadata_delta()`: for DLT7, `replace_*` is set only when its bit is
set. `encode_snapshot_for_delta()`: case 7 -> `encode_snapshot`.
`metadata_delta_version()` already parses the trailing digit.

Rolling-upgrade note: a pre-DLT7 node receiving a DLT7 body throws
`bad metadata delta` -> the sender's existing full-record fallback kicks in
(slow, correct). Deploy all nodes together as usual and it never happens.

## Tests

- Round trip with only conflicts changed, only merge_parents changed, both,
  neither, on a parent with a standing conflict set: replay equals
  `encode_snapshot(after)` and the encoded size for a conflict-free merge is
  back in the hundreds of bytes, not the size of the conflict set.
- `test_metadata_merge_delta_preserves_standing_conflicts` and
  `test_metadata_delta_child_of_merge_commit_reconstructs` keep passing
  (they assert `encoded[7] == '6'` today -- relax to `>= '6'`).
- A DLT6 body hand-encoded with both lists still decodes to both set
  (compatibility for on-disk history).

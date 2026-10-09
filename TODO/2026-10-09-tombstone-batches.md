# Tombstones out of the head record: batches

Status: agreed with the operator 2026-10-09; in progress.

## Why

gbni-1's head record is 2.67 MB, nearly all of it 46,092 tombstones
(`GarbageRef`: id, `retired_at_ns`, `retirement_id`; 56 B each) held for the
30-day grace; the namespace itself is in the tree (9,819 entries, 1,071,309
extent references). Every control `get_metadata` copies the record and every
commit's accept decodes it, so control work grows with recent deletes
(ACTIVE section 2 item 1).

## What tombstones are now

A delete retires its file's extents in one mutation, each with the same
`retired_at` (`filesystem.cpp` retirement, catalogue `append_garbage`). Two
branches merge by union, latest retirement per id winning
(`metadata_merge.cpp`). Maintenance erases a tombstone only when it exactly
matches what it saw, so a later retirement of the same id survives
(`maintenance.cpp` `maintain_garbage_metadata`). Ids are hashes: a tree keyed
by id would scatter one delete across most of its leaves.

## The design

1. **Batches.** The tombstones one mutation makes are one immutable control
   object, ids sorted, with their `retired_at` and `retirement_id`. A
   100-extent delete is about 5 KB. Stored and replicated as the namespace
   tree's nodes are, durable on the commit's holders before the head that
   names it.
2. **The head lists batches**: hash, count, `retired_at` per batch (48 B), so
   a few hundred deletes are 10 to 20 KB, not MB.
3. **Collection drops whole batches**: a batch's tombstones mature together,
   so maintenance removes its entry from the list. Erasing part of a batch
   (rare: an extent that came back) writes a new batch without those ids.
4. **A re-retired extent** goes in the new batch; the latest retirement wins,
   as now.
5. **Merge** is the union of the two batch lists.
6. **Lookups by id** (GC, delete, stale-garbage detection) use a local index
   on each node, an `ObjectTrie` built from the batches it reads, like the
   reference counts; its memory is its cache share.
7. **Migration**: the first start on the new format turns the existing
   tombstones into batches grouped by `retired_at`. A new head record format:
   both nodes upgrade in one deploy.

## Slices

1. The batch object and the local index (no format change).
2. The head record format: the batch list replaces the tombstone vector;
   deltas, merge, migration.
3. Maintenance and the filesystem and catalogue retirements on batches;
   measure the record and control work on gbni-1.

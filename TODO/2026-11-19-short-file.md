# A file written through the mount commits short, silently (found 2026-11-19 in rehearsal R4)

**Open. Mechanism proven 2026-11-19 22:53Z by a deterministic failing test. Present on 0.94.2 (live) and 0.95.0.**
Nothing is fixed; the maintainer decides the fix ("Report findings before fixing").

## Proven (2026-11-19 22:53Z)

`tests/test_filesystem_fuse.cpp` `test_lagging_view_keeps_a_writing_inode` (uncommitted), on build-3
`/home/maintainer/macha-store-2` (0.95.0 on Dagda 0.3.1). One node: create `/short.bin`, write 2 MiB, `fsync` (G1). The
publication guard, inside G1's `mutate_delta` before `accept_locally`, accepts a peer's branch three commits long from
G1's parent (stamped entries `/peer-0..2`) and lets `local()` cache it. Result, every run:
- G1 is accepted but `cache_record` refuses it: the peer's head is at a higher generation (`newer_than`,
  `metadata_manager.cpp:26`). The decoded view is the peer's head, which lacks G1.
- The next namespace request adopts that view. `/short.bin` holds the pre-G1 entry, `adopt_entry`'s dirty clause
  fires (`unconfirmed_data_entry` alone is enough), and the path moves to a fresh inode.
- The remaining 2 MiB is retired by `replay_data_quantum`'s no-path branch: no publication, no log.
- The heads merge cleanly (conflicts 0, the peer's directories kept) and the file stands at G1: **2,097,152 of
  4,194,304 bytes**, no error anywhere.

**The maintainer's rulings (2026-11-19, interactive):** the fix in both layers, then a new Macha version on Dagda 0.3.2 (Dannan
0.9.2) and the rehearsals R1 to R4 (plus the two-writer round) from the start. Layer (a) amended after Dagda's review,
because "own head wins" would move the view back below a peer's commits: "never go backwards, and guard FUSE". The fix:
- (a) `MetadataManager::cache_record`: a view holding this node's latest accepted commit
  (`MetadataReplica::author_accepted()`) is never replaced by one lacking it. A concurrent head is kept out of the view;
  the head-set change already wakes maintenance's fold (`announce_metadata_generation`, node event `metadata`).
- (b) FUSE: each inode records this node's latest mutation when it publishes (`published_dot`, read after the commit
  returns). In `adopt_entry`, a view that does not cover it is behind the inode: the inode keeps its path and state.
- (b2) FUSE adoption only moves forward: a view whose clock does not cover the last adopted view's clock is deferred
  (`view-behind`).
- (c) The no-path branch logs `code=fuse_writes_retired_unlinked` with the counts. There is no error to the writer,
  because a local `rm` of an open file must not fail its `close`.
- After Dagda's review: a set-aside or expired peer head would have wedged (b2), and before this fix it already held
  the view. Since 0.94.2, `newer_than` refused the own head after `aside()`.
  - Now `read_group`'s sole usable head displaces a cached record not in its past (`cache_record(..., true)`).
  - FUSE defers `view-behind` only while `metadata_fold_pending()` (several usable heads). Otherwise it warns
    `code=fuse_view_behind_adopted` and walks the namespace whole.
  - Case: `test_a_set_aside_head_leaves_the_view`.
  - **The one place the view goes backwards, by design:** when a fold cannot be made and the peer head is set aside,
    the peer's new paths leave the mount, and local inodes on them are detached, until a later merge brings them back.
    The warn line marks it.

In the field, a peer's branch arrives in bulk as the data lane comes up, and any reader caches it (`local()`,
`read_group` with one head) while a data publication is between `usable_heads()` and `accept_locally`. That matches both
cases and explains why steady rounds, once the heads have folded, do not reproduce it.

## What was seen

Two rehearsal pairs, both on the build Pis: A = build-3 192.0.2.202, B = build-4 192.0.2.228; replicas 2,
write_copies 1, extent 4 MiB, packing threshold 1M; files written with `cp -r` into the mount, checked by sha256 after
a cache drop.

| When | Version | File | Committed | Written |
|---|---|---|---|---|
| R4 first load, 20:58:43Z, a fresh pair, the data lane to B first came up at that second | 0.94.2 | `r4/big/f2.bin` | 84,410,368 | 100,663,296 |
| r5, straight after a restart of both nodes, B writing small files at the same time | 0.95.0 | `r5/big/f6.bin` | 93,847,552 | 100,663,296 |

Both: the bytes the file has are correct; both nodes agree on the short size; no error or warning in the journal;
nothing parked; `data_publications_started == completed`; the spool drained. Not seen in 0 of 30 large files on
single nodes (R1, R2), nor in 14 large files loaded later on the 0.94.2 pair, nor in 80 large files over 8 later
rounds on the 0.95.0 pair (5 plain two-writer rounds, 3 restart-then-copy rounds).

## Facts from the bytes (object id = the plaintext's SHA-256, checked)

- f2's extents 0 to 19 exist; its real extents 20 to 23 were **never stored** on A. Its committed tail is a 512 KiB
  packed record. So the commit was a mid-write snapshot and the writes after it were never handed to the store.
- The unreferenced loose objects on A and B include exact prefixes of large files' extents, at sizes those files
  passed through while being written (e.g. `r4/big/f0.bin` extent 1 first 3 MiB; `r4b/big/f0.bin` extent 15 first
  3 MiB). Every large file is published several times while written; a later commit supersedes each snapshot and leaves
  its tail unreferenced. Those files are whole; the orphans are ordinary garbage (Dagda agrees). In f2's case the later
  publication never happened.

## The candidate (Dagda's reading, checked against the code; unproven)

`src/fuse/fuse_frontend.cpp`, identical on 0.94.2 and the port:
1. A mid-write publication commits G1; `base = G1` (`:3745`).
2. A snapshot view that lacks G1 is adopted while the inode still has ops. In `adopt_entry` (`:5107-5124`) the dirty
   clause takes any content difference as a replacement (no version guard; only the open read-only clause has one),
   resets the writing inode's `published_path`, and gives the path to a fresh inode holding the view's entry.
3. `cp` keeps writing to the detached inode. Its next publication takes the no-`published_path` branch of
   `replay_data_quantum` (`:3534-3558`), which journals its ops done and drains the spool: no publication, no counter,
   no log.
4. The replacement inode later adopts G1, and G1 stands as the file.

It fits every symptom and the timing (both cases at the moment peer metadata starts arriving). **Not proven:** a
diagnostic build logging both branches (on the Pi's copy only, since removed) saw neither fire in 8 rounds, and the
short file did not recur under it. A deterministic test of the clause (commit G1 on a dirty inode, adopt a view holding
an older entry, assert the path survives and the rest publishes) is the next proof.

The lagging view is Macha's own: `available_snapshot_view()` is `MetadataManager::current()`
(`filesystem.cpp:2456`). 0.95.0 does not use Dagda's heads (the cutover migrates without `--heads`), so Dagda's
heads read-your-writes finding (its `Space::current()` can lack a node's own commit while several heads exist) is a
real Dagda property but not this defect; Dagda withdrew its repin recommendation to the maintainer on that basis and keeps its
`covers()` helper on a branch, uncommitted.

## Next

1. The deterministic test of `adopt_entry`'s dirty clause against an older view (failing test first). For any further
   run on the pair, log in the replace branch the adopted snapshot's generation beside this node's own committed
   generation, and the entry and base versions (Dagda's suggestion: a peer's head arriving at a higher generation as
   the data lane comes up would show there).
2. If it fails as expected: the mechanism report to the maintainer, with the fix's shape (a lagging view, one that does not
   include the inode's last commit, keeps the inode; only a newer, different entry is a replacement; and the
   no-path branch must never retire unpublished writes silently).
3. Whether the cutover waits on it: the maintainer's decision. The defect is in 0.94.2 already; the migration is not involved.

## The evidence, kept

The pair runs 0.95.0 (shipped libraries) with `r4/big/f2.bin` and `r5/big/f6.bin` short in its namespace; sources and
sums in `/root/rehearsal/` on both; claims at 0 cores to 2026-11-20 22:34Z. `maintenance.collect_garbage` is off on
both, so the orphans stay. `/root/rehearsal/clean.sh` on each removes everything.

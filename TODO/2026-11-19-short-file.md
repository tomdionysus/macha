# A file written through the mount commits short, silently (found 2026-11-19 in rehearsal R4)

**Open. Mechanism not proven. Present on 0.94.2 (live) and 0.95.0.** Nothing is fixed or changed; the maintainer decides any
fix once the mechanism is proved ("Report findings before fixing").

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

# Torn pack header at the tail takes a whole backend offline

Date: 2026-09-13

Formal diagnosis of the live incident recorded at the top of `ACTIVE.md`
("gbni-1 is storing nothing"), and the plan to fix it. Every claim below was
checked against current source at the cited line; nothing is carried over
from the incident note without re-verification.

## What is happening

gbni-1's only DATA backend, `/mnt/diskB` (8 TB configured), has been offline
since the node last started. The node logs once at startup:

```
WARN storage backend offline /mnt/diskB: corrupt pack header at
     /mnt/diskB/packs/pack-00000000000000001206.pack offset=29841717
INFO node data storage ready used=0 capacity=0
```

and then runs normally in every other respect. `GET /api/v1/status` shows
`macnessa storage_cap=0.0G`. The pack is 29,841,842 bytes; the offending
header sits at 29,841,717; **exactly 125 bytes** follow it, which is exactly
one `pack_header_size` (93-byte prefix + 32-byte SHA-256,
`src/local_store.cpp:32-33`).

The disks are healthy (ext4 mounted clean, no I/O errors in `dmesg`), and
only 8 packs totalling 106 MB exist on the backend. This is one bad record
tail, not a failing disk.

## Chain of consequence, with citations

1. `StoragePool::activate` (`src/storage_pool.cpp:136`) constructs the
   `LocalStore` for the backend at `:235`.
2. The `LocalStore` constructor calls `rebuild_pack_index_locked(true)`
   before anything else (`src/local_store.cpp:297`) — the packed index is
   deliberately derived from the append-only pack files at every start.
3. `rebuild_pack_index_locked` walks every pack record by record. It handles
   two torn-tail shapes when `truncate_incomplete_tail` is set:
   - fewer bytes left than a header → truncate and stop (`:761-765`);
   - a header that decodes but whose payload runs past EOF → truncate and
     stop (`:777-782`).

   A header that is **present but undecodable** — `decode_pack_header`
   returns nothing because the magic, checksum or field ranges fail
   (`:145-177`) — **throws unconditionally, ignoring the flag** (`:772-776`).
4. The exception leaves the constructor; `activate` catches it at
   `src/storage_pool.cpp:270` and calls `deactivate`, which marks the backend
   offline and logs the WARN above (`:110-134`).
5. Because `activate` threw before the install block at `:244-259`,
   `backend->token_known` was never set. `StoragePool::limit()` only counts
   backends that are `configured && token_known` (`:992-1009`), so the
   node's capacity is 0. `NodeRuntime::recover_storage` publishes that as the
   node's advertised storage (`src/cluster.cpp:380-389`) and marks data
   storage *ready* — the node reports itself healthy with nothing to store on.
6. **The node does re-probe the backend — every heartbeat — and it fails
   the same way each time.** `NodeRuntime::loop` calls `local_->refresh()` on
   every iteration (`src/cluster.cpp:1595`), which re-runs `activate` on every
   configured backend. Each attempt constructs a fresh `LocalStore`, meets the
   same header, throws the same error, and `deactivate` suppresses the log
   line because the reason has not changed (`src/storage_pool.cpp:122`). So
   the WARN appears once, the node keeps trying silently, and nothing short of
   recovery accepting the pack can bring the backend back. (An earlier draft of
   this document said no re-probe existed. That was wrong; the loop was
   missed.)

Placement consequence: with gbni-1 at 0 capacity and es-1 down, replication 2
has one eligible target. Objects are on both reachable nodes or neither, never
exactly one — the pattern the client reported.

## Why the tail looks like this

The writer (`append_pack_record_locked`, `src/local_store.cpp:586-668`)
appends a record as two `write()` calls on an `O_APPEND` fd — the 125-byte
header (`:627`), then the payload (`:628`) — and never syncs the pack itself.
Durability is the `DurabilityDomain`'s job: on Linux it is one `syncfs` over
the filesystem (`src/durability_domain.cpp:153-181`), issued after the
mutation registers a generation (`complete_mutation`, `:87-100`), and a put
is only acknowledged durable once that generation has been synced.

So between `write()` returning and the next `syncfs`, a record lives in the
page cache. On power loss ext4 (data=ordered, delayed allocation) can commit
the inode's new size while the block that was to hold the header is either
zero-filled or holds whatever the interrupted write left. Result: the file
is extended by the header's length, but the bytes there are not a header.
That is exactly this pack's shape, and it is the *expected* outcome of a
power interruption on this node — the same class of event behind the
2026-09-08 undervolt work.

**No acknowledged data is in that tail.** A generation is awarded only after
both `write()`s complete (`:632` then `complete_mutation` in the caller), and
the acknowledgement waits for `syncfs`. A record that never reached disk was
never acknowledged. Truncating it discards nothing the cluster was told it
had. The accounting checkpoint was necessarily dirty (`ensure_accounting_dirty`
runs before the first mutation of a session, `src/local_store.cpp:330-340`),
so the post-truncate walk reconciles `used` correctly.

Evidence still worth collecting on the box, before touching the file:
`tail -c 125 <pack> | xxd`. All zeros confirms the ext4 zero-fill shape; anything
else says the header was partially written. Either is handled by the fix.

## Why the writer's own rollback did not save it

`append_pack_record_locked` rolls back a *failed* append — ENOSPC, EIO, short
write — by truncating to the pre-append offset (`:633-651`). That path runs in
the live process. Power loss gives it no chance to run; that case is, by the
comment at `:638`, explicitly delegated to restart recovery — which is the
code that does not handle it.

## The defect, stated precisely

`rebuild_pack_index_locked` treats "undecodable header" as unconditional
corruption. For an append-only file with a single writer that never appends
behind a torn record (the rollback path either restores the boundary or
abandons the pack, `:639-651`), an undecodable header followed by no
decodable record is a torn tail and should be truncated exactly like the
other two torn-tail shapes. An undecodable header **followed by** decodable
records is something else — bit rot, a media fault, an external edit — and
must not be truncated, because truncation there would silently discard the
valid records after it.

The existing regression test, `test_pack_recovery_discards_incomplete_tail_record`
(`tests/test_storage_v18.cpp:642`), appends 23 bytes — fewer than a header —
and so exercises only the first branch. No test appends a header's worth of
garbage.

## Fix

### 1. Recovery settles an undecodable header without refusing the pack

Operator decision, 2026-09-13: Macha self-heals; one bad record must never
take a backend offline. So both shapes are handled in place.

In `rebuild_pack_index_locked`, when `decode_pack_header` fails:

- Scan the remainder of the file (`offset + 1 .. file_size`) for a decodable
  header: search for the 8-byte pack magic `MACHPK01`, and on each hit read
  `pack_header_size` bytes and run `decode_pack_header`. The SHA-256 over the
  prefix makes a false positive negligible, including against encrypted
  payload bytes. Bounded to one pack, read in 1 MiB chunks, hashing only on
  magic hits (`find_next_pack_header`).
- **No decodable header follows** → torn tail. With `truncate_incomplete_tail`
  set, `ftruncate` to `offset` and log
  `WARN truncated undecodable pack tail path=… offset=… bytes=… zero_header=0|1`;
  either way stop scanning this pack, as the other two branches do.
- **A decodable header follows at `next`** → damage inside the pack. Skip
  `[offset, next)`: add it to `pack_dead_bytes_` so compaction reclaims it,
  log at `error`
  `skipped unreadable pack region path=… offset=… bytes=… zero_header=…`,
  and continue indexing from `next`. Objects recorded in the span are simply
  absent from this backend; `has()` is false, so the cluster repairs them
  from replicas, and a re-put of the same object works. A `pack_remove`
  tombstone lost in the span can resurrect an earlier record of that object
  until GC reaches it — bounded and benign, and recorded in the changelog.

A genuine read error (`pra_exact` failing) still throws: that is a disk not
answering, and the existing heartbeat re-probe brings the backend back when
it does.

Diagnostics: `LocalStoreDiagnostics` gains `pack_recovery_truncated_tails`,
`pack_recovery_skipped_regions`, `pack_recovery_skipped_bytes`; summed in
`StoragePool::diagnostics()` and emitted under
`diagnostics.data_store` in Status.

### 2. Regression tests (`tests/test_storage_v18.cpp`, beside the existing one)

- `test_pack_recovery_truncates_undecodable_header_at_tail`: two packed
  objects, then append exactly 125 zero bytes. Reopen: both objects readable,
  pack back to its intact size. Repeat with 125 non-zero random bytes, and
  with 125 + 40 000 bytes (undecodable header plus a partial garbage payload)
  — all three truncate to the intact size.
- `test_pack_recovery_skips_unreadable_region_before_live_records`: three
  packed objects; flip one byte in the *second* record's checksum. Reopen:
  nothing truncated, first and third readable, second absent, diagnostics
  report one skipped region of exactly header + payload bytes. Re-put the
  second, then `compact_packs()` shrinks `used()` and all three still read.
- The existing 23-byte test stays green.

The test can't see `pack_header_size` (anonymous namespace); write `125` with
a comment deriving it, as the existing test hardcodes its 23 bytes.

### 3. Changelog and version

Entry under a new `0.38.3` heading; the bump goes in the same commit per the
release convention relayed on 2026-09-13 (still unconfirmed, but harmless to
follow here). Run the suite on a node, not only on macOS, before shipping —
0.38.0's two GCC-only failures are the reason.

## How gbni-1 heals

Operator decision: no hand repair of the pack. Deploy 0.38.3 to gbni-1 and
restart the service. On start-up recovery reads the pack, meets the header at
29,841,717, finds nothing decodable after it, truncates to that offset and
logs

```
WARN truncated undecodable pack tail path=/mnt/diskB/packs/pack-00000000000000001206.pack offset=29841717 bytes=125 zero_header=…
INFO storage backend online /mnt/diskB elapsed_ms=… accounting=reconciling
INFO node data storage ready used=… capacity=8796093022208
```

and `GET /api/v1/status` shows `macnessa storage_cap=8192.0G`, with
`diagnostics.data_store.pack_recovery_truncated_tails: 1`. `zero_header`
in that line is the evidence the box would otherwise have had to give up by
hand: `1` confirms the ext4 zero-fill shape.

Check playback sessions before the restart (live viewers; gbni-1 still
serves transcodes from remote reads even with no local storage).

**What this does and does not restore.** gbni-1 regains capacity and becomes
a replication target again, so everything held on gbni-2 gets its second
copy back over time. The ~42% of artwork whose only replica is on es-1 stays
unfetchable until es-1 returns; gbni-1 never had those objects and cannot
conjure them.

## Follow-up this diagnosis surfaces (not part of this fix)

- **A node with zero storage capacity reports itself healthy.** `state:
  "online"` with `storage_cap=0` and `data storage ready` is how this ran
  unnoticed for a day. Belongs with the "powered-off node is reported
  online" item under P1 and the maintenance-section gap under P2 diagnostics.

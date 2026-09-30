# Overlapping FUSE writer rebuild stall

Date: 2026-08-31

Status: reproduced in live UAT; diagnosis complete; correction and deterministic
test remain active.

This correction is now integrated as
[Phase 1D of the throughput plan](2026-08-31-fuse-publication-throughput-plan.md#phase-1d-make-control-and-viewer-priority-non-bypassable).

## Workload

Two rsync invocations overlapped on the same destination inode set:

- a Movies-only `--append-verify` import into `/mnt/machamedia/Movies/`; and
- a broader import containing `/mnt/diskA/Movies` into `/mnt/machamedia/`.

The second job was intentional and later exited. The original Movies job was
left running.

## Evidence

- Both Linux Macha services remained active with unchanged PIDs and zero
  systemd restarts.
- Node 50 had read 194,695,746,015 publication bytes but confirmed only
  177,561,155,534 bytes.
- The 17,134,590,481-byte difference nearly equalled the
  17,179,677,628-byte spool occupancy. The spool was only 191,556 bytes below
  its 16 GiB limit.
- Six publications had started but not completed. A 30-second clean sample
  showed no publication, retirement, acceptance, failure, or timeout movement.
- The original rsync had issued about 196.7 GB of reads and 191.9 GB of writes
  in seven hours. Therefore the low visible-file count did not represent little
  physical work; the system performed large verification and reconstruction
  work without making file generations visible promptly.
- FUSE append-verification reads commonly took 100--700 ms per request.
- A FUSE `release` waited 177.5 seconds for its accumulated local
  spool/journal durability backlog.
- Diagnostics recorded 880,909 publication requests, of which 880,799 were
  merged/coalesced, demonstrating excessive per-write scheduling churn.
- Of eight configured publication workers, one was blocked in an ext4 object
  read from `/mnt/diskB/objects`; the other seven waited. Disk B sustained
  approximately 73--84 MiB/s reads, about 80% utilisation and about 20% CPU
  iowait. There were no kernel I/O errors.

## Root cause

Interleaved writes to the same inode violate `WriteHandle`'s sequential append
assumption. The handle falls back to `materialize()`, which reconstructs the
entire existing file into temporary staging. `commit()` then calls `rebuild()`
for a materialised handle, rereading and hashing the complete logical file even
when most extents are unchanged.

The FUSE publication path also serialises `WriteHandle::commit()` behind
`publication_mutex`. Consequently one full-file reconstruction blocks commit
progress for other inodes. Atomic whole-file visibility and spool retirement
then pin every byte of each affected generation until that serial operation and
metadata confirmation finish. Full-spool backpressure correctly stops rsync,
but it cannot create retirement progress.

The overlap triggered the worst case, but the system must handle it rationally;
this is not acceptable as a user error. Slow FUSE verification reads and
close-time durability concentration are independent contributors that remain
even with one writer.

## Required correction

1. Preserve the sequential fast path across compatible interleaved append
   ranges by ordering/merging durable overlay operations before publication.
2. Detect genuinely conflicting writes explicitly and bound their scope; do
   not silently turn one changed range into a whole-file reconstruction.
3. Reuse immutable unchanged extents without materialising and rehashing their
   bytes. Stage only changed boundary/range extents.
4. Remove the global commit convoy. Long materialisation, hashing, object I/O,
   durability barriers and metadata waits must not hold a cross-inode commit
   mutex.
5. Ensure a full spool prioritises work that can retire complete generations;
   one pathological inode must not pin all retirement capacity.
6. Coalesce publication notification at durable byte/sequence boundaries rather
   than issuing hundreds of thousands of per-write requests.
7. Keep local durability semantics, atomic visibility and viewer priority
   unchanged. `materialize()`/`rebuild()` must yield at bounded byte boundaries;
   one call from inside a nominal loader quantum must never perform an
   uninterruptible whole-file read that bypasses the 95:5 scheduler.

## Deterministic reproduction

Create a large already-published file, open two writable FUSE handles, and
append compatible ranges with deliberately interleaved acceptance order. Fill a
small configured spool while a second independent file is complete. Assert:

- unchanged base extents are reused without full-file reads or rebuild hashes;
- publication work remains byte-bounded;
- the independent closed file commits and retires despite the conflicting inode;
- spool occupancy falls below the admission bound;
- both writers receive correct close/durability semantics; and
- no viewer/control worker performs the reconstruction or commit wait.

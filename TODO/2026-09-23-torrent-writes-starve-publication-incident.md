# Torrent writes starve publication: `CONTROL retention floor unavailable` (2026-09-23)

Status: diagnosed, NOT FIXED. Nothing else is worked on until it is.

## Symptom

Seven ingest jobs died between 18:59Z and 19:22Z on 2026-09-23 with
`ingest failed: CONTROL retention floor unavailable before metadata publication`
(gbni-1: 5, es-1: 2). Imports on both nodes crawled for the whole evening:
single 4 MiB extent puts took 150-390 s. Nothing locked up; nothing was
watching; fi-1 (torrents disabled, no storage) was untouched.

This is a different failure from the one 0.53.1 fixed under the same message.
0.53.1's cause was the commit re-uploading its whole control graph; every
commit today logs `missing=0`. Today's cause is below, and the message is only
where the damage surfaces.

## What was ruled out, and how

- **Not the release.** On the same 0.53.2 build es-1 did 1,673 extent puts
  overnight and 608 between 15:00Z and 18:00Z with zero DATA credit abandons;
  gbni-1 did 436 with 12. Both nodes broke at the minute they logged their
  first DATA-pressure clamp of the day: gbni-1 18:18:07Z, es-1 18:24:15Z.
- **Not damaged trees.** Zero `unreconstructable`, `does not reproduce`,
  `tree node`, `integrity failure` or `corrupt` lines on either node in 12 h.
  Zero `concurrent file content change` failures today (that is item -3, a
  separate live defect). Every death today is the CONTROL floor.
- **Not priority ordering as such.** A pure priority inversion would freeze
  the ingest; instead every wait ends at exactly 120 s, which is the
  arbiter's no-progress valve, and the import limps. The mechanism is below.
- **Not Plex, not qbittorrent.** No Plex process holds an fd on the mount on
  either node; qbittorrent-nox wrote 0 bytes in a 10 s sample.

## What is saturating the disks (measured live)

- es-1: ten unnamed threads inside the macha process (tids 2305068-2305077)
  wrote **652 MB in 10 s (~65 MB/s)**; eight of the ten were in D-state in
  `ext4_buffered_write_iter` / `folio_wait_bit_common`. Their open files:
  `/mnt/diskB/ingest/torrents/014ea49d.../My Name Is Earl/Season N/*.avi`,
  opened `O_RDWR`. That is libtorrent's disk pool (libtorrent 2.x starts
  ten by default) writing torrent payload onto the DATA spindle.
- gbni-1: the same ten-thread pool (tids 153594-153603) writing ~25 MB/s into
  the torrents added 18:21Z-19:33Z: Metallica, Bon Jovi, Guns N' Roses,
  Tiesto and Prydz discographies plus several films -- thousands of small
  FLAC/mp3 files, i.e. random writes.
- es-1 `sdb`: 91% busy, ~190 ms per write. es-1's own extent writes
  (`object write quorum ... local_ms`) were under 600 ms all day, 14 s in the
  18Z hour, 42-70 s in 19Z. gbni-1's rose to 3-8 s.
- Both nodes: `torrent.max_download_rate: 0B` (unlimited),
  `max_active: 4`. es-1 has a fast line; nothing bounds the write rate.

## The mechanism, step by step

Each step is from the code plus the journals of both nodes.

1. **Torrent I/O is invisible to every control.** libtorrent writes with its
   own fds. The DATA arbiter never admits it and the disk service monitor
   never measures it. The monitor measures only macha's own extent
   operations, sees them take 30-100x their expected time, and declares the
   device pressured. This is exactly the gap recorded under item -2:
   "the FUSE spool and ingest staging already sit on the DATA spindle as
   plain file I/O the monitor never sees."

2. **Under pressure the arbiter throttles the governed work.**
   `DataResourceArbiter::available` admits one lower-class operation at a
   time when pressured (`min_background=1`), and the loader shares that
   ceiling with speculative work (`lower_active_` counts every non-viewer
   lease). The ingest's own extent writes then queue for a credit:
   gbni-1 `local_ms=6013 total_ms=248553` is 6 s of writing and 242 s of
   waiting.

3. **The one slot is held across a peer RPC, on both nodes at once.**
   `DistributedStore::put_impl` keeps the DATA credit inside `PendingPut`
   for the whole second-replica RPC; the peer's inbound `put_object` handler
   (`cluster.cpp`, `case MessageType::put_object`) needs a credit on the
   peer. es-1's put to gbni-1 stalled from 18:55:12Z and gbni-1's put to
   es-1 from 18:55:17Z, each 4-6 minutes with no progress
   (`RPC stalled (speculative) peer=... message=put_object`), each waiting on
   the other's arbiter. `data_no_progress_deadline` is 0, so the stalled put
   itself never gives up; only the waiters behind it do.

4. **The 120 s valve turns starvation into failures.** With nothing released
   for `data_credit_no_progress_deadline` (120 s) the arbiter abandons every
   waiter: 62 `DATA credit wait abandoned ... lower_active=1/2` on gbni-1 in
   the 19Z hour, 16 on es-1. `waiting_viewers=0` throughout.

5. **The commit path is one of those waiters, and it fails silently.**
   `DistributedStore::retain_control` calls `ensure_control_local` for every
   control object the commit references before contacting any peer;
   `ensure_control_local` takes a *speculative* credit of `extent_size`
   (4 MiB) merely to check an 18 KB local object is valid. When that wait is
   abandoned it returns false with no log line, `retain_control` returns
   false, `Service` reports `outcome=control-floor-unavailable`
   (`control_ms=120004` in one case, 139741 and 202399 in others), and
   `IngestManager` marks the job failed. Bytes are not lost; the job is.

6. **The clamp meant to protect the device flips instead of holding.**
   `TorrentManager::follow_device_pressure` clamps only while
   `viewer_recently_active(foreground_quiet = 2 s)` is true and restores the
   moment it is not. es-1 logged 287 clamps and 287 restores in the 19Z
   hour; gbni-1 306 and 306, at a 2-4 s cadence. A libtorrent rate limit
   reset every two seconds limits nothing: the torrent wrote 65 MB/s
   throughout. es-1 served no playback in that hour, so what re-armed its
   2 s viewer window is not in the journal. The likely source is gbni-1
   fetching extents from es-1 with `FrameType::foreground` for its own
   viewer (an inbound foreground request marks viewer activity here).
   **Unverified.**

## The systemic reading

Ingest is one pipeline: torrent download, staging read, extent write,
replication, commit. The laws govern the last three. The first is an
unbounded ten-thread writer on the same spindle, and today it ran 30x ahead
of the loader (65 MB/s in, 34 extent puts out in four hours on es-1). When it
saturates the disk, the monitor cannot tell whose I/O did it, so the remedy
lands on the governed part of the pipeline -- the part the laws rank above
background. The harder the arbiter throttles publication, the longer the
torrent has the disk to itself. Everything downstream -- the cross-node
credit hold, the 120 s abandon, the silent `ensure_control_local` failure,
the ingest treating a transient as fatal -- is the system coping with a load
it never admitted.

## Open design questions

Decided 2026-09-23 evening: see [the backend plan](2026-09-23-torrent-disk-backend-plan.md). The questions as they were asked:

- Does the torrent feed belong under the same admission as everything else
  that writes the DATA spindle: a bounded, pressure-aware rate held for as
  long as the device is slow, rather than a clamp gated on a 2 s viewer
  window? Or should staging not share the DATA spindle at all? Law 1 holds
  by configuration, not construction, and this is the first time it bit.
- Should a DATA credit ever be held across a peer RPC? Today a slow peer
  becomes local starvation, and two nodes replicating to each other hold
  each other's only slot.
- Should the loader share the background concurrency ceiling? Law 3 says
  the loader yields only to a viewer; under pressure it currently yields to
  the ceiling.

## Consequences to fix regardless of the answers above

- `ensure_control_local` must not take a 4 MiB speculative credit to validate
  a local control object, and must log when it fails.
- An ingest whose checkpoint commit fails transiently (`MetadataNotReady`,
  EAGAIN, quorum unavailable) must pause and retry, not die. Same class as
  item -3's ingest part and the restart-mid-put failure.
- Pressure onset and release must be logged; today the only evidence of the
  pressure state is the torrent clamp line, and that only fires with a
  viewer.
- The clamp/restore flip must not happen at the viewer window's cadence.

## Evidence index

- gbni-1 journal 19:05-19:22Z: `RPC stalled`, `DATA credit wait abandoned`,
  `metadata retention barrier ... outcome=control-floor-unavailable`,
  `ingest failed` in that order, each cycle.
- es-1 journal 18:59:57Z: same sequence; `object write quorum` lines with
  `local_ms` 42482, 44944, 70598.
- Per-thread `/proc/<pid>/task/*/io` deltas and `fd` listings on both nodes,
  taken 19:45-19:55Z.
- Histogram of abandons/stalls/floor failures per window, 2026-09-22 08:00
  to 2026-09-23 23:59 local, both nodes.
- Code: `src/data_work.hpp` (`available`, `acquire`),
  `src/distributed_store.cpp` (`put_impl` launch, `retain_control`,
  `ensure_control_local`), `src/cluster.cpp` (`put_object` handler),
  `src/torrent_manager.cpp` (`follow_device_pressure`), `src/service.cpp`
  (`control_ms` window).

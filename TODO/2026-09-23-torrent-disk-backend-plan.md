# The torrent writes into the store: a macha disk backend for libtorrent

Status: **stage 1 built and tested 2026-09-24, not yet committed or
deployed** (0.54.0 in the tree): es-1 full suite 525/525 + runtime 10/10 +
`macha-tests-torrent` 10/10, the torrent binary 200/200 over 20 repeats
there, including a loopback swarm through the backend. Stages 2-4 not
started.

**Stage 2 built 2026-09-24 as 0.55.0** (es-1: 528/528 + 10/10 + 12/12,
torrent binary 120/120 over ten repeats including a loopback swarm with
publication). Built on the payload files as the assembly area ("option A");
the operator has not chosen between that and a staging format of macha's own
("option B"), and everything specific to A is behind `read_extent`. Open
risk to measure in production: one publisher thread may trail a fast
download, and a file incomplete at finish is copied as before.

**The laptop cannot host a real libtorrent session** (the swarm test fails
there, every time): `/usr/local/include/boost` is a manual Boost 1.91 install
that shadows Homebrew's 1.92, and CMake finds it
(`Boost_DIR=/usr/local/lib/cmake/Boost-1.91.0`), while Homebrew's libtorrent
2.1.1 was built against 1.92. Asio is header-only, so the test binary and the
dylib disagree about object layout and the heap is corrupted inside
`session_impl::setup_listener`. Proven with AddressSanitizer and a standalone
probe. The nodes are unaffected (Debian's Boost 1.83 and libtorrent 2.0.11
come from the same distribution). Fixing the laptop is the operator's call;
until then run torrent tests on es-1.

Previously: planned 2026-09-23. This is the fix for
[torrent writes starve publication](2026-09-23-torrent-writes-starve-publication-incident.md);
nothing else is worked on until it ships.

## Why a backend and not a knob

libtorrent 2.0.11's disk I/O is a virtual `disk_interface`, chosen by
`session_params::disk_io_constructor`; `posix_disk_io` and `mmap_disk_io`
are the two stock implementations. macha uses the default (`mmap`, ten
threads) and sets none of the disk settings. Tonight that pool wrote
65 MB/s onto es-1's DATA spindle outside every admission control, the disk
monitor blamed publication for the slowness it measured, and the arbiter
throttled the import to one slot.

The knobs (`download_rate_limit`, `aio_threads`, `max_queued_disk_bytes`,
`disk_io_write_mode`) can make libtorrent gentler, but the decision of when
still has to come from macha's measurements, and the existing clamp shows
what a rate limit re-set every two seconds achieves: nothing. A backend
makes the torrent's I/O macha's I/O. The arbiter admits it, the monitor
measures it, and the laws rank it: an acquisition is loader-class work.

## Design

### The assembly area

Per torrent job, under the staging path the job already reserves:

```
<staging>/<job>/<file_index>/<extent_index>.slot   fallocated, <= extent_size
<staging>/<job>/map                                the journal
```

A slot is one file-relative extent of one torrent file: 4 MiB, or the
file's short tail. Blocks (16 KiB) land in their slot at their offset by
`pwrite`; nothing is held in memory. A piece that spans two files splits
into two slots via libtorrent's file map; a piece that spans two extents
splits the same way. Files with priority 0 have no slots.

The map records, per slot: block bitmap, `verified`, published object id.
Discipline: data before bitmap, bitmap before `verified`, `verified` before
the id; each state change appended and fsynced with the same care the
ingest's job checkpoint gets. On a crash, unverified blocks are refetched by
libtorrent from its own resume data; nothing in the map is ever trusted
beyond what the discipline guarantees.

### Verification

The backend does not understand piece hashes. libtorrent verifies a piece
and emits `piece_finished_alert` (or `hash_failed_alert`), which
`TorrentManager` already drains. On `piece_finished`, every block the piece
covers is marked verified; on `hash_failed`, cleared. `async_hash` and
`async_hash2` read from the slots like any other read. This works for v1
and v2 torrents identically.

### Publication

**An extent is published the moment its whole range is verified**, whether
that took one piece, several, or the ends of two. Publication is
`DistributedStore::put_impl` with the job's loader-class `DataWorkContext`:
read the slot back (hot in page cache when publication is prompt), seal,
store locally, replicate by the ordinary placement. The object id goes into
the map. A retention claim is taken per published extent so a multi-day
torrent does not churn against GC grace; correctness does not depend on it
(see cleanup).

A file is committed to the namespace **once, complete**, when its last
extent has published: one `commit_file` naming the ordered extent list. No
checkpoints, so torrent ingests stop contributing to the reconciler
collisions recorded under item -3. Multi-file torrents batch their commits
(per torrent, or per interval, decided in stage 3) rather than one mutation
per file: a discography is a thousand sub-4 MiB files.

### Resume and check

`async_check_files` is answered from the map, not by re-hashing: a verified
slot with an id is confirmed by presence (`has`, locally or on a replica);
verified without an id is re-put from the slot; partial keeps its bitmap.
The extent id is already the hash of verified plaintext.

### Cleanup, unconditional

The job directory is removed when the completing commit is durable, on
cancel, and on job removal. At startup, any directory under `<staging>` that
belongs to no live job is removed. A periodic sweep repeats the startup
check. The `StagingArea` reservation the torrent already takes is the
budget, and it now covers slots, not payload files. **A published extent
that GC collects before its file is committed costs a re-put from the slot;
it never costs bytes.** That is why slots stay until commit or cancel.

### The rest of `disk_interface`

`move_storage`, `rename_file`: no-ops (the payload has no path).
`delete_files`: cleanup. `release_files`: close slot fds. `set_file_priority`:
create or drop slots. `clear_piece`: clear bitmap bits. `async_read`: from
slots (seeding is off, `active_seeds: 0`, but a read must still work).
`update_stats_counters`, `get_status`: from the map.

### Threads and admission

A bounded worker pool of macha's own (two or three on a four-core node, a
config knob stated rather than inherited), every slot write, read-back and
hash read admitted through `DataResourceArbiter` at loader class with the
job's context. When the device is pressured the loader yields to a viewer
only, per law 2; libtorrent's `max_queued_disk_bytes` then stalls peers, so
the network rate follows what the disk admits. `follow_device_pressure` and
`pressure_download_rate` are deleted.

### Time to watch (law 1)

This is the point of publishing during the download, not just its side
effect. Today a torrent is watchable after the download *and then* the
ingest copy, which at 13 MB/s takes about as long again as the download.
With per-extent publication the copy step does not exist and the replicas
are already placed when the last piece verifies: the file is playable from
any node the moment it is committed.

Sequential mode allows one step more. The prefix grows in order, so the
file can be committed as a growing-prefix checkpoint (exactly what the
ingest's `checkpoint_bytes` does today) and become playable *while* the
download runs, with libtorrent's piece deadlines keeping pieces ahead of
the viewer's position. That brings checkpoint commits back for torrent
jobs, at 64 MB per commit in sequential order -- not the six-jobs-every-7-s
storm of item -3 -- and it is the one case where a checkpoint earns its
cost. It is a stage 3 decision: measure the reconciler's cost first, and
keep it off for rarest-first jobs, whose prefix does not grow.

## Cost model

Per byte of payload, today: torrent write to staging, ingest read of
staging (cold, gigabytes later), extent write. With this: slot write, slot
read-back (hot when publication is prompt), extent write. Same three passes
on paper -- objects are sealed with AES-GCM using the object id as
associated data (`local_store.cpp:646`), and the id is the hash of the
complete plaintext, so a slot can never be adopted into the store by rename.
The saving is the cold read, the checkpoints, the ingest copy step, and one
admission layer covering everything. Do not call it one pass.

Download order decides the disk pattern, not correctness:
- `sequential_download`: near-sequential 16 KiB writes, extents complete in
  order, prompt publication, hot read-backs. Some throughput cost on thin
  swarms.
- rarest-first: scattered writes across every open slot (seek-bound on a
  spinner), extents complete late and together, cold read-backs.
Default sequential; allow rarest-first per job; measure both on the cluster.

Memory: none beyond page cache. Staging: the same reservation as today.

## Stages, each shippable

1. **Backend skeleton with admission.** `macha_disk_io_constructor`,
   bounded pool, slots as plain payload-layout files for now, every I/O
   admitted at loader class. Delete the clamp. This alone removes the
   incident's mechanism at its source.
   Tests: fault injection on the arbiter (pressured device, viewer waiting)
   shows the torrent yielding and the loader not; a repeat-cycle test that
   adds, downloads, cancels and removes a torrent leaves no fds, no slots,
   no leases.
2. **Extent assembly and per-extent publication**, sequential mode, the map,
   `piece_finished` driven verification, single complete-file commit.
   Tests: pieces smaller than, larger than and unaligned with extents; a
   piece spanning two files; a file shorter than one extent; a multi-file
   torrent of a thousand tiny files; kill -9 between data, bitmap, verified
   and id, then resume from the map; a published extent removed from the
   store before commit is re-put from the slot.
3. **Random order, batching, and watch-while-downloading.** Rarest-first
   per job; commit batching for multi-file torrents; retention claims per
   extent; the GC-grace interaction measured rather than assumed; the
   sequential-mode prefix checkpoint (see "Time to watch") behind a per-job
   switch, with piece deadlines following an active playback session.
   Tests: rarest-first on a large multi-file torrent completes with the same
   manifest as sequential; reconciler sees N commits, not N files.
4. **Cleanup proof and the long tail.** Startup and periodic sweeps; file
   priorities; v2 torrents end to end; `get_status` for the jobs API.
   Tests: a job directory with no owner is gone after start; an interrupted
   cleanup is finished by the sweep.

Also in the same release, because they are the incident's other holes:
- `ensure_control_local` must not take a 4 MiB speculative credit to check
  a local control object, and must log when it fails.
- An ingest whose commit fails with `MetadataNotReady` pauses and retries
  rather than dying.
- Pressure onset and release are logged.

## What to measure on the cluster

Before (already have, 2026-09-23): 65 MB/s torrent writes on es-1 with the
disk at 91% and 190 ms per write; extent puts at 150-390 s; 287 clamp flips
an hour; seven dead ingests.

After each stage, on the same kind of load (two nodes importing large
torrents to each other):
- sdb utilisation and per-write latency while a torrent runs.
- `object write quorum local_ms` distribution for the ingest's own puts.
- DATA credit abandons per hour (should be zero).
- Time from piece verified to extent published (stage 2).
- Bytes read back from slots that missed page cache (stage 2/3).
- Commits per torrent (stage 3).
- Any `CONTROL retention floor unavailable` (should be zero).
- **Time from torrent added to first playable second**, today against each
  stage. This is the number law 1 cares about.

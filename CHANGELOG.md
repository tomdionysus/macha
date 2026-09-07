# Current release

## 0.32.15 — Matroska direct play, and a codec list per delivery path (development)

The operator's TV decodes HEVC through its media element and fails it
through MediaSource, and `MediaSource.isTypeSupported` lies about it, so no
client-side probe catches it. One capability list could not say "HEVC yes
for the file, no for the HLS stream", and Matroska sources were never
offered as direct play at all, so an HEVC episode in an mkv was remuxed
into fMP4 and failed to decode (2026-09-07).

- `direct_container` recognises `.mkv`/`.mka` as `matroska` (`mkv` and
  `x-matroska` accepted as spellings), served as `video/x-matroska`. A
  client that lists the container gets the file over byte ranges: no
  remux, no transcode, no MSE.
- `capabilities.hls_video_codecs` narrows the video codecs for HLS
  delivery only; absent or empty it is `video_codecs`, so nothing changes
  for existing clients. The transcode target check uses the same list.
- An explicit `direct` request still hands over the source file whatever it
  is (the client asked for the bytes; it is also the operator's escape
  hatch), but the session's `warnings` list now carries a `containers`
  entry when that file's container was never advertised. A client that sent
  `direct` by default was getting raw Matroska it had not claimed to read,
  and the failure surfaced only as a decode error.

## 0.32.14 — The media playlist grows with the encoder (development)

Operator's TV after a mode switch (2026-09-07): "preparing new stream"
cleared in ~7 s, then black for 1 min 38 s. The media playlist was a VOD
list of every planned fragment (1,517 for a feature) and a fragment request
blocked until that fragment was encoded, so a native player that prefetches
deeply waited on the encoder, ~3 s each, for as many fragments as it chose to
read ahead (UI session measurement: 30 fragments ≈ the 98 s).

- The media playlist is an EVENT list of the fragments that exist, closed
  with ENDLIST once the generation has produced its last. A request that
  arrives before the first fragment exists is held (bounded by
  `startup_timeout`) rather than answered 404: an HLS player reports a
  failed playlist load as a network error, and a client that reads such
  errors as node health would see every new generation start with a
  spurious degradation signal. Seeking is unchanged: PATCH
  seek_ms creates a new generation, and the client's timeline comes from
  the session's duration, not the playlist.
- **MPEG-TS segments** for clients that cannot take fragmented MP4: a
  client sending `hls_fmp4: false, hls_ts: true` gets `.ts` segments
  (libavformat's `mpegts` muxer: Annex B H.264/HEVC, ADTS AAC, no init
  segment, `output.format` `mpegts`, playlist version 3 without
  `EXT-X-MAP`). The operator's 2017 Samsung TV plays HLS through Tizen's
  native player, which rendered fMP4 video and dropped the muxed AAC even
  with a correct `CODECS` line; direct play of the same title had sound.
  Fragment boundaries are the same planned cuts as fMP4 (one segment per
  flush before a keyframe).

## 0.32.13 — The writer keeps a copy and the second one is prompt; a background effort ceiling (development)

Operator decisions after the import findings (2026-09-07): keep
`min_write_replicas: 1` but make "replicated" a matter of minutes, not of
the repair cursor; give the daemon its own effort ceiling instead of an
external CPU quota; stop cold claims after restarts; statfs must not read 0.

- **Writer-local first copy.** `put_impl` tries the local store first, then
  placement order. Placement alone could make an offsite writer's only copy
  an upload across the WAN, its own viewers read it back across the WAN,
  and its publication rate the link's. Repair still converges the copies
  onto the placement owners afterwards.
- **Prompt second copy.** A put that stopped at the floor queues its object;
  one worker per node pushes it to the first placement owner lacking it, as
  speculative DATA work behind viewers. Until now the extra copies waited
  for the repair cursor, hours on a busy import, and a writer's death in
  that window stranded its recent data (finding #6, 129 files).
  `DistributedStore::prompt_replication_stats()` counts queued/copies/failures.
- **Background effort ceiling.** `maintenance.background_concurrency`
  (default half the hardware threads) bounds loader + speculative DATA
  leases held at once, i.e. how many extents publication and repair are
  hashing, encrypting or transferring together; viewers are never counted.
  Status: `data_resources.background_limit/background_active/
  peak_background_active`. gbni-1 no longer needs the systemd CPU quota.
- **Presence index warmed at start.** `LocalStore` reads the object
  directory names (no stat) into the presence set on start, so the first
  claim on each large file after a restart is no longer a cold stat per
  extent (2.6-16 s per quantum commit); `data_store.presence_index_entries`.
- **statfs never reports 0.** `logical_capacity` falls back to the local
  store's limit/used while the membership view is empty or has no
  capacities (the first seconds after a start showed a 0-byte mount).
- Probe: `bit_depth` is derived from the pixel format when the container
  does not report it (Matroska HEVC), so a 10-bit source no longer relies
  on the transfer alone for the negotiation gate.

## 0.32.12 — A master playlist that says what is in the fragments; 10-bit and HDR are not "hevc" (development)

Operator report via the UI session (2026-09-07): on a 2016 Samsung TV
(hls.js 1.6 over MSE on Chromium 47) a Dolby Vision profile 8 title broke
decoding in direct play, and the transcode fallback played picture with no
sound. Checked from the node: the transcode's init segment and fragments
carry h264 + AAC LC, so the server produced audio; but the stream URL
`master.m3u8` answered with the media playlist itself, so no `CODECS`
attribute ever reached the player and it inferred the source-buffer codecs
from the init segment. The TV's advertised capabilities were not logged
anywhere, and it had listed `hevc` for a 10-bit PQ source.

- `master.m3u8` is now a master playlist: `EXT-X-STREAM-INF` with
  `BANDWIDTH`, `CODECS` (RFC 6381 strings from the negotiated plan and the
  probed streams: `avc1.PPCCLL`, `hvc1.P.C.Lnnn.B0`, `mp4a.40.2`, `ac-3`,
  `ec-3`, …) and `RESOLUTION`, pointing at `media.m3u8`.
- Capabilities gain `video_bit_depth` (8-16, default 8) and `hdr`, a list
  of transfer names the client presents (`smpte2084`, `arib-std-b67`; the
  boolean `true` means both). In `auto`, a source deeper than that or with
  an HDR transfer the client did not list is transcoded, and not offered as
  direct play. The probe records `level` and `color_transfer` per stream and the
  session response exposes them on the source streams; `output.video`
  reports what is served (the copied stream's `bit_depth`/`level`/
  `color_transfer`, or `High`/8-bit/`bt709` for the H.264 transcode), so a
  client can tell a downconverted PQ source from a gate that did nothing.
- `session create complete` logs the negotiated video/audio transform and
  codec; the client's capabilities are logged at debug.
- Stored media profiles are schema 2, carrying each stream's `level` and
  `color_transfer` (the catalogue media-profile API reports both). A
  schema-1 profile with a video stream is treated as stale and regenerated
  on its next playback, then republished: the gate compared against the
  stored profile, which had neither field and a zero depth for the Dolby
  Vision title, so a client claiming 10 bits and no HDR was handed the
  copied stream (found by the UI session's one-call check, 2026-09-07).
- Audio copy stays AAC-only. A first cut of this release also copied AC-3,
  E-AC-3 and Opus; the copied E-AC-3 made libavformat's fragmented MP4
  muxer fail its header write (`Invalid argument`: the `dec3` box needs the
  first packet parsed, i.e. `delay_moov`), and every session create from a
  client listing `eac3`, the TV included, answered 503 for about an hour on
  es-1 and gbni-2 (2026-09-07 17:17-18:30). (E-)AC-3/Opus copy returns once
  the muxer path handles them; the UI session's two-capability-set check
  (with and without `eac3`) is the acceptance test.

## 0.32.11 — Remux is allowed to have long fragments; transcode uses the cores it has (development)

Viewer path, measured first (2026-09-07, all three nodes, 24 h of logs):
every VOD plan was a software transcode. Remux was tried for HEVC titles
on HEVC-capable clients and rejected as `unusable-keyframe-index` although
the Matroska cues were complete (1,533 entries on a 9 GB title). No Dolby
Vision, HDR or HEVC exclusion exists; the rejection came from a
segment-density rule.

- **Remux keyframe rule.** `media_vod::indexed_plan` rejected the whole file
  if any fragment exceeded 3x the target (12 s). Scene-cut x264/x265
  encodes have such gaps routinely. A fragment is now as long as the
  source GOP makes it, up to 90 s; only a tail over 90 s (the partial-index
  case the rule was written for) or a gap over 90 s rejects. The planner's
  rejection line now carries `keyframes=` and `longest_gap_s=`.
- **x264 threading.** `thread_count` was never set and `tune=zerolatency`
  switched x264 to sliced threading: about real time for 1080p CRF 20 on a
  4-core node, so every representation change cost 5-13 s and a mid-file
  seek could not catch up. Now frame-threaded across the hardware threads
  (`streaming.video_encoder_threads`, 0 = all) with a short look-ahead.
- **Short first fragment.** A transcode generation answers its request only
  after its first fragment is encoded; that fragment is now 2 s (later
  ones keep the segment duration), on every start and every seek.
- **Plan cache across seeks.** The VOD plan cache key no longer includes
  the seek position; a hit is re-seeked (`reseek_hls_vod`), so a
  representation change at a new position no longer re-opens, re-probes and
  re-indexes the container.

## 0.32.10 — CONTROL retention puts go together, to the nearest replica (development)

0.32.9's phase line on gbni-1: `metadata retention barrier total_ms=4286
decode_ms=13 collect_ms=1 catalogue_ms=25 data_ms=0 control_ms=4247
control_objects=65`. `retain_control` stored the catalogue graph on each
candidate one object per round trip, and after the local replica took the
candidates in membership (NodeId) order, so gbni-1 pushed 65 small objects
to es-1 across the WAN, 65 ms each, inside every catalogue mutation.

- The candidate order is local first, then measured nearest
  (`order_commit_replicas`, moved to placement.hpp so the store and the
  metadata manager share it).
- All puts to one candidate are issued together and awaited together:
  one round trip per candidate instead of one per object.
- `CONTROL retention claim objects=… required=… tried=… retained=…
  total_ms=…` is logged at debug when the claim takes 250 ms or more.

## 0.32.9 — Presence is remembered, not stat'ed; the barrier names its phase (development)

0.32.8's barrier line on gbni-1: `DATA retention barrier ids=3201 nodes=1
total_ms=16317 scan_ms=16048 short=0` for one quantum commit of a 16 GB
file, and 357 ms for the next: the presence scan is a `stat` per extent,
~5 ms each when the import has pushed the dentries out of the page cache.
The bounded verified-loose cache (4,096 entries) could not hold the file.

- `LocalStore` keeps an unbounded in-memory set of loose objects known
  present (installed by this process, or seen by a positive `has()` stat;
  forgotten on remove). `has()` answers from it before touching the disk.
- `Service::retain_metadata_publication` logs its phases at debug when it
  takes 250 ms or more: `metadata retention barrier total_ms= decode_ms=
  collect_ms= catalogue_ms= data_ms= control_ms= data_objects=
  control_objects= outcome=`. es-1 and gbni-1 both showed 9-15 s
  `retention_ms` on 93-byte deltas with no `DATA retention barrier` line,
  so the seconds are outside `retain_data()`; the next slow one says where.

Observed while here, for the operator: both writers run
`min_write_replicas: 1`, so a put is durable on one copy and the second is
background repair. That is the mechanism of finding #6 (a writer's death
strands its recent data) and it makes "replicated" a promise repair keeps
later, not the write.

## 0.32.8 — The writer's retention barrier fans out in parallel and measures itself (development)

After 0.32.7 the replica-side claim handler read 0 ms and commit fan-out
under 0.6 s on every node, and the `retention_ms` breakdown showed the
remaining seconds inside the writer's own pre-publication barrier
(`DistributedStore::retain_data`): ~1 s per quantum commit on gbni-1, 4.4 s
on es-1, 6-15 s in the minutes after a restart.

- The per-node claim RPCs (`retain_on`) run concurrently instead of one
  round trip after another; on this cluster that is one 60-200 ms hop
  instead of their sum.
- The local presence checks in `batched_have_objects` and `has_on` no
  longer take a 4 MB DATA loader lease per id: an index lookup owns no
  buffer, and thousands of leases per quantum commit queued the barrier
  behind the node's own publications.
- `DATA retention barrier ids=… nodes=… total_ms=… scan_ms=… short=…
  fallback_claims=… ok=…` is logged at debug when the barrier takes 250 ms
  or more, so the next slow one names its phase.
- `rpc_transport.peer_latency_ms` now samples only the heartbeat pings.
  0.32.7 sampled every control call, so payload size and handler time made
  gbni-1 see its wireless neighbour at 114 ms while the reverse direction
  read 4 ms; ordering by a polluted measure sent commits to the remote site.

Recorded, not changed: capacity placement does not guarantee the writer a
local copy of what it publishes (`ranked(id)` is shard/capacity order), so a
writer's put can be two remote replicas and its viewers read across the WAN;
and re-claiming every extent of a file per quantum grows the retention
journal quadratically over a large file's import.

## 0.32.7 — A remote retention claim is not a re-read; commits go to the nearest replica first (development)

The "WAN control-lane starvation" of the full-library import (195-284 s
metadata mutations, `control RPC deadline exceeded`, reconnect churn,
2026-09-07) measured on the live cluster: the link's queueing delay under
load was ~30 ms on a 58 ms RTT, but gbni-2's `retain_objects` handler took
up to 12.1 s per batch (avg 916 ms) and gbni-1's 35 KB commits took 9-12 s
against sub-second small ones.

- The replica-side `retain_objects` handler re-read, decrypted and hashed
  every id in the claim batch (`valid()`), serially, inside the writer's
  metadata mutation. A quantum commit re-claims every extent of its file,
  so the replica re-read gigabytes per 32 MB quantum, and with its disk
  saturated by the import that was minutes. It now checks index presence
  (`has()`), as the local claim path has since 0.32.3, and takes no DATA
  admission for the lookup. `have_object` (single, used by repair
  placement) still verifies.
- `publish_commit` ordered replicas after the local one by NodeId, which
  sent every gbni-1 commit across the WAN to es-1 before the LAN replica,
  and every es-1 commit to the undervolt-prone gbni-1. The transport now
  keeps a smoothed CONTROL-lane round trip per peer
  (`RpcClient::peer_latency`, exposed as
  `rpc_transport.peer_latency_ms`), and `order_commit_replicas` puts the
  local replica first, then measured peers nearest first, unmeasured last.
- Instrumented: `metadata mutate` log lines carry `retention_ms` and
  `publish_ms`; per-replica store/accept steps over 250 ms (or failing)
  log `metadata commit store|accept replica=… ms=…`; status metadata
  diagnostics gain `mutations`, `mutation_retention_ms_{total,max}` and
  `mutation_publish_ms_{total,max}`.

Still open in this area: congestion-aware pacing of the DATA lane against
control-lane RTT (the link itself was not the bottleneck this time), and the
retention journal growth of re-claiming whole files per quantum.

## 0.32.6 — Append-extents deltas, and the covered mountpoint is no longer a trap (development)

Two findings from the full-library import (2026-09-07).

**DLT8 append-extents deltas.** Each 32 MB quantum commit of a large file
re-sent the file's whole extent table (105 KB per quantum for a 13.9 GB file;
124 MB of history per node in 35 min). A file whose extent table only grew is
now carried as its new attributes plus the appended extents (~500 B), with
the base extent count checked on apply. Whole-entry upserts remain for every
other change, and DLT5-7 readers are unaffected until they see a DLT8 frame.
The retention barrier (`retain_metadata_publication`) retains the whole
resulting entry for an append, exactly as it did for the upsert it replaces,
so a touch or a quantum still needs its fresh causal retention dot before the
head advances (two rpc_cluster tests caught the first cut skipping it).

**Seventh finding: writes before the mount go to the host disk.** The FUSE
mount comes up only after local services (20-40 s after the daemon starts).
An rsync launched 25 s after a restart walked the bare mountpoint directory
and wrote 52 GB into a shared host's root disk, hidden by the mount once it
arrived, while Macha reported an idle spool. `fail_closed_mountpoint` only
cleared the directory's mode bits after mounting, which root ignores.

- `prepare_fuse_mountpoint` now runs the guard before the services start:
  it counts entries already under the covered directory (logged as an error,
  exposed as `filesystem.mountpoint_stray_entries`) and sets the immutable
  flag on it (Linux `chattr +i`; `filesystem.mountpoint_immutable`), which
  stops root too and persists across restarts, so the pre-mount window is
  closed for every later start. Verified on the cluster: a tmpfs and the
  Macha mount both attach over an immutable directory; `touch` under it
  fails with EPERM.
- The post-mount mode guard skips chmod when the flag is in place (chmod on
  an immutable inode fails, which would have refused the mount).

## 0.32.5 — Spool pacing credits drained quanta, not only retired files (development)

Sixth import finding (2026-09-07 10:30, es-1): while a multi-GB file
published quantum by quantum nothing *retired* from the spool, so the
whole-file retirement rate sample never refreshed; the last small sample
(46 KB/s) stood and the importer's writes were paced to it — 0.3 MB/s on
a link and disk good for 8 — although the spool drained 32 MB at a time.
The drained-quantum credit (`note_spool_publication_progress`) was only
consulted before the first sample existed.

- `reserve_spool_bytes` admits against drained-quantum credit whenever
  there is any, regardless of the rate sample; the rate path remains for
  the rest.

Also measured, not yet changed: each quantum commit of a large file
re-sends the file's whole extent table in its metadata delta (105 KB per
32 MB quantum for a 13.9 GB file; 124 MB of history per node in 35 min).
An append-extents delta operation (DLT8) is the fix and is next.

## 0.32.4 — Publications no longer serialize behind one WAN-bound commit (development)

Fifth finding from the full-library import (2026-09-07 04:00, gbni-1):
after 0.32.3 the store was fine but gbni-1 still published nine extents in
ten minutes with a full spool; six of its eight publication threads sat on
`FileSystem::open_writes_mutex_`, which `commit_write` (and `open_write`'s
truncation, and every namespace batch) held across a whole cluster metadata
commit — seconds each with the WAN in the path — so a node's publications
committed one at a time regardless of `commit_workers`.

- `open_writes_mutex_` now protects only the handle registry and each
  handle's `path_`: `commit_write` snapshots the path under it and commits
  outside; `open_write` truncates before taking it; `apply_namespace_batch`
  runs its mutation first and re-points open handles under the lock after.
  A rename that lands between a commit reading its path and the mutation is
  caught by the commit's own entry check (`removed while open`), the caller
  retries, and rename's fix-up has updated the path by then.

## 0.32.3 — A restart is not a write outage; a retention claim is not a re-read (development)

Third and fourth findings from the full-library import (2026-09-07 04:17–04:45,
es-1): after a restart the object store walked its whole tree to reconcile
accounting (4 min on gbni-1, 25+ min on es-1 with the import saturating
the disk) and **every put waited for it** — the spool replay, the import
and gbni-1's retention claims to es-1 all stalled, and gbni-1's data
publications queued behind the metadata mutation waiting on that claim.
Meanwhile every metadata mutation's retention step re-read, decrypted and
hashed each extent it claimed, holding the store lock long enough that all
16 HTTP workers sat in `LocalStore::has` and `/api/v1/status` did not
answer for 40 s.

- **Clean accounting checkpoints are trusted even when packs exist.**
  `ensure_accounting_dirty()` persists "dirty" before the first mutation of
  a session, so a torn pack tail can only sit behind a dirty checkpoint; the
  "any pack forces a walk" rule was redundant. A dirty checkpoint's figure is
  carried as an estimate while the walk reconciles in the background, and
  puts/removes are admitted against it (`storage accounting estimate` log
  line; the configured `reserve_free` headroom covers the bounded error);
  only pack compaction waits for the exact figure. The walk's total replaces
  the estimate, keeping the larger of the two.
- **A local retention claim checks presence, not content.** `retain_on`
  uses `has()` — the same contract discipline 1 wrote down for durability —
  instead of the full `valid()` re-read of every extent; the scrub is where
  later corruption is found.

## 0.32.2 — Namespace batches under an identity; utimens survives publication (development)

Two more findings from the full-library import (2026-09-07 02:30–03:30):

- **The namespace loop committed one operation per metadata commit.**
  Renames and mixed kinds were singleton batches because, after a crash, a
  batch's intermediate effects could not be proven from the final snapshot
  (a re-applied create + rename would overwrite the real file with an empty
  one). rsync's create-temp / utimens / rename per file therefore cost ~3
  cluster-wide commits per file at ~3/s, and every data publication waited
  behind the op naming its file: gbni-1 had 1,690 Music ops queued and eight
  publication threads parked on them with the whole in-flight budget held.
  Now every batch (up to `namespace_batch_operations` / `_bytes`) is
  published as one **atomic** metadata mutation carrying an identity in the
  snapshot's mutation-sequence clock (`MetadataMutationIdentity`: a
  FUSE-derived origin key, value = the batch's first op sequence), journaled
  as a `namespace_batch` record before publishing. Recovery and retry ask
  the clock "did this batch commit?" instead of re-deriving per-op effects;
  `mutate_delta()` applies an identity mutation only if the clock is behind
  it. A batch one op refuses falls back to publishing its head op alone (the
  old semantics) and requeues the rest. 73 recovered rsync-pattern ops now
  publish in one commit (test), and `FileSystem::apply_namespace_batch`
  gained `identity`/`atomic`.
- **A utimens applied after the writes was overwritten by the asynchronous
  publication's own timestamp** (474 of 3,770 imported Music files wrong,
  so a second rsync pass would re-copy them). The FUSE publication now
  passes the inode's current visible mtime to `WriteHandle::set_committed_mtime`,
  and recovery applies pending data metadata before a pending utimens (which
  wins only if it was admitted after the last write).
- Tests: `test_fuse_namespace_loop_batches_rsync_pattern_into_few_commits`,
  `test_fuse_namespace_batch_committed_before_crash_is_not_reapplied`,
  `test_fuse_utimens_after_write_survives_async_publication`; journal
  accounting in three batching tests updated for the identity record.

## 0.32.1 — A FUSE write waits for admission; it never returns EAGAIN (development)

Found in the first hour of the full-library import (2026-09-07 02:11, gbni-1,
4 GB node): publication held 500 MB of the 768 MB process memory budget
while 21,000 small-file operations were pending, six write admissions
timed out at the 5 s request deadline, the mount returned `EAGAIN` to
`write(2)`, and rsync — correctly — aborted the whole import
(`write failed on "…/03 Eddie, Are You Kidding_.m4a": Resource temporarily
unavailable (11)`). The budget would have been released moments later by
publication completing.

- The three FUSE admission waits (write-byte budget, operation-metadata
  budget, process retained memory) are backpressure: a writer now waits,
  in 200 ms slices so shutdown is still noticed (`EINTR`), until admitted.
  The request's own time budget starts *after* admission. A wait longer
  than 5 s logs one DEBUG line per 5 s naming the budget.
- Counters: `write_admission_waits`, `process_memory_admission_waits`
  (alongside the existing `operation_metadata_waits`).

## 0.32.0 — Compact history out of the hot path: DLT7, canonical tombstones, conflicts that leave (development)

Discipline 4 of `TODO/2026-09-06-self-healing-disciplines-plan.md`, scoped
by measurement rather than by the plan's premise. `macha-metadata-dump
--stats` (new) on the production head, 2026-09-06 23:30: 2,319,777 encoded
bytes = entries 1,958,969 (of which extent tables 1,768,067 for 36,083
extents; paths+attrs 190,902), **116 standing conflicts 335,749**,
439 tombstones 24,584. Every merge delta carried the whole conflict set
(335,961-byte "deltas"), and 4 of the last 5 reconciliations were 5–8 MB
full frames. Tombstones were 1% and are consumed by GC; the plan's per-node
retirement log is not justified by the data and is not built.

- **DLT7 metadata delta.** A flags byte gives `merge_parents` and
  `conflicts` independent presence, so a conflict-free merge carries its
  new parents and nothing of the standing conflict set (hundreds of bytes,
  not 336 KB); the first write after a merge likewise. Both-sets-changed
  still encodes as DLT6 so a rolling upgrade keeps cheap merges; a pre-0.32
  peer receiving DLT7 falls back to the full record as before.
- **Canonical tombstone order.** DLT7's third flag sorts the tombstone
  vector by ObjectId after applying the edits. Reconciliation always
  produced that order while the primary parent's vector was in append
  order, and pre-DLT7 deltas could not reorder retained tombstones — the
  cause of the full-frame reconciliations. `MetadataManager` canonicalises
  the vector on every mutation (one-time re-sort of a legacy snapshot, then
  stays sorted), so merges are deltas.
- **Superseded conflicts leave the snapshot.** A namespace conflict whose
  path no longer holds the common-ancestor value, or a catalogue-root
  conflict once the root moved on, is decided by that later mutation and
  is pruned at the next commit and at every merge
  (`prune_superseded_conflicts`, `MetadataMergeResult::conflicts_superseded`,
  reconciliation log line gains `superseded=N standing=N`).
- **Same bytes are not a conflict.** Two branches that wrote the same
  content to the same path (type, mode, ownership, size and extents equal;
  mtime/version may differ — two rsync writers publishing duplicate media)
  merge to the deterministic lesser representation instead of a conflict,
  and a pre-0.32 record of that shape settles the same way at pruning.
- **Conflicts are visible and resolvable.** `diagnostics.metadata.{conflicts,
  namespace_conflicts, catalogue_conflicts, tombstones, conflicts_superseded,
  conflicts_resolved}`; `GET /api/v1/manage/metadata/conflicts` lists the
  standing set with both alternatives; `POST …/conflicts/{id}/resolve?choice=left|right|base`
  installs one and drops the record in a single commit.
- `macha-metadata-dump --stats` materialises each reconstructible head and
  attributes its encoded bytes to entries/extents/tombstones/conflicts,
  with a conflict classification.
- Tests: `test_metadata_dlt7_presence_flags_round_trip`,
  `test_metadata_merge_over_append_ordered_tombstones_is_a_delta`,
  `test_metadata_superseded_conflicts_leave_the_snapshot`; the two DLT6
  tests now expect DLT7 without the conflict set.

## 0.31.0 — Recovery resolves, it does not refuse (development)

Discipline 3 of `TODO/2026-09-06-self-healing-disciplines-plan.md`. Local
durable state is replayed on every start, so anything recovery *refuses*
it refuses forever: the node either restarts in a loop or re-raises the
same fault on every boot. Found on the cluster on 2026-09-06: gbni-1
(inode 922, 183 MB) and es-1 (inode 2333, 6 GB) had a recovered
publication whose file had left the namespace — `error=missing` on every
boot, the inode poisoned, the spool kept, and the operation journal pinned
open at 132 MB / 178 MB and re-parsed each start, with twenty benign
`accepted data completion without published prefix` WARNs each time.

- **FUSE operation journal loader never refuses a frame.** A frame that
  does not fit the state so far — duplicated marker, marker whose
  operation is gone, non-monotonic sequence, unknown record type, undecodable
  payload — is skipped and counted (`journal_recovery_skipped_frames`), with
  one `WARN` per distinct reason. A re-journaled inode descriptor now
  replaces the earlier one instead of being a fatal duplicate. A checksum
  failure before EOF (durable middle-of-journal corruption) quarantines the
  tail to `<journal>.corrupt.<ts>.<pid>`, truncates, and starts from the
  good prefix (`journal_recovery_quarantined_bytes`); spool bytes whose
  operations were in the tail are preserved as orphans. `done` markers
  without the redundant `published` proof are DEBUG and a count in the
  recovery summary line, not a WARN each.
- **Operations for an inode with no descriptor** are retired with journaled
  `namespace_done` / `data_abandoned` markers and counted
  (`recovery_dropped_operations`); the spool is preserved as an orphan.
  Formerly fatal.
- **A publication whose file is no longer in the namespace is abandoned**
  (journaled, spool retired, `publications_abandoned`, one `WARN` with the
  last path and unpublished bytes) instead of poisoning the inode until an
  operator acts. Nothing an operator could do with it existed anyway; now
  the journal can reset once the rest of the backlog drains.
- **Path-collision loser is really re-journaled.** 0.28.3's fix stamped the
  loser with the current epoch before re-journaling it, so the re-journal
  was a no-op and the collision was re-resolved on every boot.
- **Metadata journal:** a frame that fails authentication or does not fit
  the CAS chain anywhere in the file ends the replayable prefix — the tail
  is quarantined and truncated like a torn append — instead of throwing out
  of the constructor and quarantining *every* metadata file (checkpoint,
  history, heads) over one frame. **Metadata history:** a frame that cannot
  be authenticated or decoded is skipped and counted; dependent heads are
  repaired live from peers as before.
- Status: `diagnostics.filesystem.{journal_recovery_skipped_frames,
  journal_recovery_quarantined_bytes, recovery_dropped_operations,
  publications_abandoned}`.
- Tests: `test_fuse_journal_fuzz_every_frame_mutation_still_starts` (every
  frame × truncate/drop/duplicate/corrupt, the frontend starts each time),
  `test_fuse_recovery_abandons_publication_for_file_removed_from_namespace`
  (second boot is clean), `test_fuse_durable_journal_skips_unbacked_data_done`
  (was `..._rejects_...`), `test_metadata_journal_mid_frame_corruption_truncates_not_reseeds`;
  the scanner model test now expects `corrupt_frame_offset`.

## 0.30.0 — "Not yet" never becomes "forever": retry budgets, parking, progress gates (development)

Discipline 2 of `TODO/2026-09-06-self-healing-disciplines-plan.md`. Every
"try again later" in the daemon now has either a budget or a progress
condition, so a fault that does not clear cannot hold a queue, a thread or a
restart loop indefinitely. The proven cases from 2026-09-06: a FUSE
publication retried the same refused barrier ~35/s for hours; gbni-1 was
killed by the 120 s startup gate eight times in a row while replaying a
5-minute journal; `accept_metadata_commit` calls sat "active while peer
health is monitored" for 150–230 s.

- **`RetryPolicy` / `RetryState`** (`retry_policy.hpp`): one shared budget
  shape — `max_failures_in_window`, `failure_window`, exponential
  `initial_backoff`→`max_backoff` — used by the subsystem supervisor (whose
  `SubsystemRetryPolicy` is now an alias) and everything below.
- **FUSE data publication backs off, then parks.** Each inode carries its
  own `RetryState`; a transient failure re-queues it after a per-inode
  backoff (the fixed 100 ms sleep is gone, and `admit_deferred` skips inodes
  still backed off so others publish at full speed). When the budget
  (`fuse.publication_retry_*`, default 100 failures in 30 min) is exhausted
  the inode is **parked**: bytes stay in spool+journal, it leaves the loader
  queue, one `WARN` line is logged. Operators see it in
  `diagnostics.filesystem.parked_publications` and
  `GET /api/v1/manage/filesystem/parked-publications`, and resolve it with
  `POST .../{inode}/retry` or `.../{inode}/abandon`.
- **FUSE namespace loop** uses the same budget (`fuse.namespace_retry_*`)
  instead of a fixed 50 ms→5 s ladder; because the queue is ordered it
  cannot park, so on exhaustion it reports the blocking operation as
  `EAGAIN` in `namespace_blocked_op` and keeps retrying at the ceiling.
- **ENOENT during publication is re-derived, not assumed.** A publication
  that finds its inode unnamed retries while the namespace still has a
  rename queued/in flight for it or the decoded view still shows the path,
  and is terminal only when the file is genuinely gone. (Fixes the 1-in-4
  `recovers_ordered_mutations` failure where a rename the loop had not
  learned yet poisoned the inode.)
- **Startup gate on progress, not elapsed time.** Journal parsing, delta
  application, snapshot decode, storage accounting and readiness stages tick
  `note_startup_progress()`; `Service::wait_services_ready` kills the process
  only when that counter is silent for `service_startup_no_progress_ms`
  (120 s). `service_startup_timeout_ms` is now an optional ceiling and
  defaults to 0.
- **RPC no-progress deadline.** `RpcClient::call` takes a
  `no_progress_deadline`; `NodeRuntime` applies
  `network.control_no_progress_deadline_ms` (30 s) to control calls and
  `data_no_progress_deadline_ms` (0, off) to object transfers. Past it the
  request is cancelled and fails with `RPC made no progress for N ms ...;
  cancelled for retry`, which callers already treat as transient.
- Tests: `test_fuse_publication_backs_off_then_parks_for_operator`,
  `test_service_startup_gate_waits_while_recovery_progresses`,
  `test_rpc_call_fails_after_no_progress_deadline`; `config_for` now sets
  fast retry budgets for the suite.
- Renamed `FuseFrontend::abandon_corrupt_data` → `abandon_data` (it is also
  the operator abandon path now).

## 0.29.0 — Durability is re-derived from disk, not asserted from a dead token (development)

Discipline 1 of `TODO/2026-09-06-self-healing-disciplines-plan.md`. The
proven 2026-09-06 wedge: gbni-1 had placed 13 extents of a 13.9 GB file on
es-1; es-1 was restarted; from then on every barrier gbni-1 ran was refused
with `storage durability epoch changed` and retried, at ~35/s, forever — the
batch named a placement token that died with es-1's process, while the bytes
sat on es-1's disk the whole time. Restarting *any* node did this to every
in-flight publication elsewhere that had already touched it.

- **Wire.** `object_durability_barrier` accepts an optional trailing list of
  object ids. On an epoch mismatch *with* ids the peer no longer refuses: it
  checks each id on its own disk (`StoragePool::reassert_durable`), flushes
  the holding backend on its current incarnation, and replies with the
  present ids and fresh `(epoch, domain, generation, backend_instance)`
  tokens. Without ids (a pre-0.29 requester) it refuses as before.
- **Client.** `DistributedStore::durability_barrier(DurabilityBatch&)` —
  now non-const — probes every peer whose replica answered `epoch changed`
  (and re-derives locally when its own backend was reopened), re-stamps the
  batch in place so the next barrier is ordinary, and reports only the ids a
  peer genuinely no longer holds through the new `unsatisfiable` out-param.
  One INFO line per re-derivation on each side.
- **Writer.** `WriteHandle::commit()` re-puts an unsatisfiable extent from a
  local copy when there is one, and otherwise fails with `ESTALE`, which the
  FUSE frontend treats as "discard the provisional writer and replay this
  generation from the spool" — the WAL is the one place the bytes are
  guaranteed to be. The same dead batch is never retried.
- Durability contract, written down: an object present on a node after a
  restart is durable — the pack index is rebuilt from disk on open and the
  probe flushes the current incarnation before answering. See
  `docs/operations.md`, "Durability tokens and restarts".
- Tests: `storage_v18/test_durability_barrier_rederives_placement_after_peer_restart`
  (restart a peer; barrier succeeds; batch carries the new epoch; nothing
  re-sent) and `…_reports_objects_a_restarted_peer_lost`. The test harness
  gained `StorageClusterNode::restart()`.
- **Same discipline, FUSE side.** A data publication that hits ENOENT used to
  decide "race or dead file?" from the inode's own bookkeeping, and could
  poison the inode as terminal while a rename it had not yet learned about
  was landing — one run in four of
  `test_fuse_durable_journal_recovers_ordered_mutations`, the extra barrier
  work having widened an old window. It now re-derives the answer: retry
  while any namespace op is in flight/queued/unconfirmed or the inode's
  `published_path` exists in the decoded view; terminal only when the file is
  genuinely gone from the accepted namespace (which
  `test_fuse_terminal_recovery_failure_is_not_readmitted` still requires).
  Terminal publication failures now log at WARN, not DEBUG.
- **Transient is not lost.** The first live run re-put — then replayed from
  the spool — a file's extents because the barrier ran during the six
  seconds es-1 was restarting and got `send: Broken pipe`. A barrier now
  reports an id as unsatisfiable only when every failed replica answered
  *definitively* (absent after probe, unknown peer, reopened local backend);
  transport failures, a peer mid-restart and a pre-0.29 peer are "ask
  again". `test_durability_barrier_treats_an_unreachable_peer_as_transient`;
  the failure line carries `transient=yes|no`.

## 0.28.3 — Linear tombstone replay; a node could not start after 0.28.2 (development)

Found by 0.28.2 itself. Until this morning a conflict-free merge delta was
always rejected and stored as a 15 MB full snapshot, so cold replay never
applied one. 0.28.2 made those deltas replayable — and gbni-1, restarted a
few hours later with two accepted heads whose chains contained merge deltas,
never came up again: `service startup stalled after 120000ms …
metadata=recovering`, eight times in a row. `gdb` put the recovery thread in
`MetadataReplica::load_heads → materialized_locked →
apply_metadata_delta_in_place` for the whole 120 s.

- **Cause.** `apply_metadata_delta_in_place()` ran `std::erase_if` per erased
  tombstone id and `std::find_if` per upserted one over the entire garbage
  vector. This namespace has ~1,600 files and ~270,000 tombstones (that's
  what the 15 MB snapshots are), and a reconciliation unions both branches'
  tombstones, so its delta carries 45–65 k of them: ~10¹⁰ comparisons per
  frame on a Pi, i.e. minutes, versus a 120 s startup budget.
- **Fix.** Erases go through a set, upserts through an id→index map: one pass
  each. Semantics are unchanged (all tombstones with an erased id go,
  retained order is preserved, an upsert replaces the first match in place,
  new ones append in delta order) and the exact-reconstruction checks that
  gate every stored delta still hold byte-for-byte.
- Regression: `storage_metadata/test_metadata_delta_tombstone_edits_are_linear`
  — a 200 k-tombstone snapshot with a 60 k-edit delta must replay in bounded
  time and match a naive reference on the same edits.
- **Second start blocker, hidden behind the first.** Once the replay was
  fast, gbni-1 came up, then exited two seconds later, every 7 s:
  `FUSE journal recovery produced duplicate namespace path
  /TV/Big.Mistakes.S01E01…mkv`, thrown from `initialise_namespace()` on a
  durable journal — so deterministic, and the node could never return.
  Recovery now resolves two inodes on one path the way a live rename-over
  does (a journaled inode outranks a snapshot-seeded one; between journaled
  ones the later namespace sequence wins; the loser is detached, keeps its
  data ops, and is re-journaled without the path), logging both at WARN.
  On gbni-1 the WARN named the pair: `kept_inode=11240 seq=3803` (the file
  as re-written this afternoon) versus `detached_inode=7270 seq=2758` (the
  same path's earlier inode, still holding a 398 MB unpublished spool from
  03:56) — a create-over whose displaced inode kept the path in its journal
  descriptor. Why the displacing op did not transform it at recovery is not
  yet known. No unit test yet: the collision needs a hand-built journal, and
  the live journal (backed up) is the reproduction.
- Operational note: gbni-1 was brought back with a temporary
  `service_startup_timeout_ms: 1800000` in `/etc/macha/macha.yaml` (backup
  `macha.yaml.bak-20260906-startup`); remove once 0.28.3 is on the node.
  Its FUSE journal is backed up as `/etc/macha/fuse-operations.log.bak-20260906-dup`.

## 0.28.2 — Compact deltas for merges and the write after them (development)

`WARN local metadata delta rejected; retrying full record generation=N` had
been firing on every node for days, and the 0.27.0 theory that it was an
evicted parent did not survive contact with the 0.28.0 processes. The 24 h
journal on gbni-1 split perfectly: every `histories reconciled …
history_body=delta conflicts=0` was preceded by the WARN, no `conflicts=1`
or `full` reconciliation ever was. `macha-metadata-dump --all` then showed
the same merges stored as 15 MB `full` frames on the *peers* too — peer
rejection only logs at DEBUG, which is why the incident note believed peers
accepted them.

- **Cause.** DLT6 writes `replace_merge_parents` and `replace_conflicts` as
  bare lists, and the decoder reads an absent set back as "replace with
  nothing". A reconciliation whose parents share an unresolved conflict
  changes `merge_parents` but leaves `conflicts` as they were, so
  `metadata_delta()` sent only the former; the replay dropped the standing
  conflict, the exact-reconstruction check in `MetadataReplica::store_commit()`
  failed, and every replica — local and remote — fell back to the full
  snapshot. With a standing conflict set (the cluster has had one for days)
  that was every conflict-free merge, ~60 a day, 15 MB × 3 replicas each.
- **Fix.** `metadata_delta()` sets both replacements or neither, and
  `encode_metadata_delta()` refuses a DLT6 delta carrying only one. No wire
  format change; every DLT6 body already on disk was accepted only because
  it reconstructed correctly, so it stays valid.
- **Also.** The first ordinary write after a merge was forced to a full
  snapshot (`clear_merge_parent_topology`, a DLT5-era rule from 0.19.0 that
  predates DLT6 being able to clear `merge_parents`). On gbni-1 that was 42
  of 212 commits in one day at 3–32 s each, on the FUSE write path, plus a
  15 MB frame per replica. It is now a compact delta, for both the diffing
  and the exact-delta (`mutate_delta`) callers.
- `MetadataReplica::store_commit()` logs why a delta body was rejected at
  DEBUG (`parent-not-materialized`, `succession`, `payload-mismatch`,
  `identity-mismatch`) — the full-body fallback used to leave no trace.

### FUSE mount could stop adopting the cluster namespace, permanently

Seen on gbni-1 while verifying the above: six directories created on es-1
were in gbni-1's replica, in its `MetadataManager` decoded view and in
`GET /api/v1/manage/filesystem?path=/`, but never on `/mnt/machamedia`, for
two hours and across three restarts. Every namespace queue counter was clean.

- **Cause.** `refresh_namespace_if_stale()` refuses to adopt a newer view
  while any published namespace op is *unconfirmed*, and confirmation meant
  "the op's effect is visible in the current snapshot". A recovered op whose
  effect had since been overwritten (a peer's write, a reconciliation, a
  later local op) could therefore never confirm, and one such op blocked
  adoption of everything, forever. The instrumented build said so in six
  seconds: `FUSE namespace refresh deferred reason=unconfirmed`. On rollout
  the recovery log named the culprits: gbni-1 `seq=149 kind=chmod`, and
  es-1 — which nobody had suspected — `seq=1170 kind=create`, each parked
  on a 20–40 k-op data backlog. Two of three mounts were stale.
- **Fix.** A published marker means the backend held the op durable at the
  metadata write floor, which always includes this node's replica, so
  recovery now retires every published op (logging at INFO the ones whose
  effect is no longer visible, with sequence/kind/path). Live confirmation is
  now "a decoded view at or past the generation the op was committed in is
  available" (`NamespaceOp::published_generation`), with effect visibility
  kept only as the fast path for a lagging view.
- **Observability.** `refresh_namespace_if_stale()` logs each deferral
  reason once per revision (`namespace-queue`, `no-view`, `unconfirmed` with
  the head op's identity, `view-not-newer`, `queue-race`) and each adoption
  (`FUSE namespace adopted revision=… generation=… new=… detached=…`);
  `/api/v1/status` `filesystem` diagnostics gain
  `namespace_refreshed_revision` / `namespace_available_revision` —
  refreshed < available is the definition of a stale mount.
- Regression:
  `filesystem_fuse/test_fuse_recovery_retires_published_namespace_op_whose_effect_was_superseded`.
- Tests: `storage_metadata/test_metadata_merge_delta_preserves_standing_conflicts`
  and `…/test_metadata_delta_child_of_merge_commit_reconstructs` (both fail
  before the fix at exactly the conflict check);
  `rpc_cluster/test_service_same_generation_sibling_notice_triggers_reconciliation`
  now also asserts the write after the merge is stored as a delta.

## 0.28.1 — Say when a subsystem plugin loads (development)

Found by deploying 0.28.0: the successful load was the only outcome the
supervisor did not log. Refusals, faults and non-plugin files each had a line;
a plugin that loaded and started had none, so confirming that a freshly
deployed `libmacha-torrent.so` had actually been picked up meant reading
`/proc/<pid>/maps` — `GET /api/v1/status` needs an authenticated call, which
is not what you reach for at 2am.

- `SubsystemSupervisor` now logs `subsystem plugin '<name>' loaded and running
  path=<file>` at INFO when a subsystem starts, including after a restart
  following a fault, and `subsystem plugin '<name>' loaded but its capability
  is not enabled on this node path=<file>` when a plugin declines. Every
  terminal outcome of a load now has exactly one line.
- Both are asserted in `subsystem_supervisor/test_subsystem_supervisor_loads_and_stops_a_real_plugin`
  and `…_reports_a_declining_plugin_as_unavailable`.
- Build fix carried from the 0.28.0 rollout: `subsystem_registry.cpp` used
  `std::unique_lock` with only `<shared_mutex>` included. libc++ provides it
  transitively, libstdc++ does not, so every cluster node failed to compile.

## 0.28.0 — BitTorrent acquisition moves into a real plugin (`libmacha-torrent`) (development)

Phase 1 of `TODO/2026-09-05-subsystem-plugin-isolation-plan.md`. The download
engine and its whole libtorrent linkage now live in a `dlopen`'d module
instead of inside `macha_core`, so whether a node can acquire over BitTorrent
is a runtime fact — the plugin file is present or it isn't — rather than a
property of how the binary was compiled. **Deployment change: installs now
ship `<libdir>/macha/plugins/libmacha-torrent.so` alongside the executable
and `libmacha_core`; a node that gets only the new binary loses torrent
support until the plugin is copied too.**

- **Split.** `torrent.cpp` became `torrent_common.cpp` (core: job value types,
  the API/wire JSON, the URI sanitisers and the Torznab search client — none
  of which touch libtorrent) and `torrent_manager.cpp` + `torrent_plugin.cpp`
  (the plugin). Core addresses the engine through the new abstract
  `TorrentService` (`torrent.hpp`) only; `TorrentManager` is no longer
  nameable from core. The inline `MACHA_HAVE_LIBTORRENT` branches are gone:
  the file only exists in a build where libtorrent was found.
- **Capability lookup.** New `SubsystemRegistry` (`subsystem_registry.hpp`):
  a plugin publishes what it provides, core looks it up. It hands out a
  `std::shared_ptr`, not a reference, because a supervised subsystem is
  destroyed and reconstructed in place on fault and an HTTP handler already
  inside it must not be left holding a dangling pointer. `SubsystemContext`
  gained the references the migration actually needs (`node`, `ingest`,
  `registry`), and `SubsystemSupervisor::start()` now takes the context,
  since none of those exist when `Service` is constructed.
- **Absent is not broken.** A plugin factory that returns no instance now
  means "this node is configured not to run this capability" (`torrent.enabled:
  false`): Status reports `unavailable`, with no retry and no backoff, as
  distinct from a plugin that threw. `/api/v1/torrents/*` answers 503 rather
  than assuming the engine is there; `/torrents/search` keeps working, since
  the Torznab client is core's own. `build_available` in
  `/api/v1/torrents/status` keeps its name and now answers the runtime
  question. Config validation no longer rejects `torrent.enabled` on a build
  without libtorrent — that is no longer a build-time fact.
- **No `dlclose`.** Unmapping a plugin invalidates everything of it that
  outlives the instance — a `shared_ptr`'s deleter and control block, a
  vtable, a `std::function`. Closing the library on supervisor stop segfaulted
  `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node`
  on exactly that: the last `shared_ptr<TorrentService>` released after the
  unmap. Handles are now kept for the process lifetime (a restart after a
  fault re-creates the instance from the still-loaded library); loading a new
  *build* of a plugin without restarting remains out of scope. A library with
  no entry symbol is still closed immediately — nothing of it was ever called.
- **Tests** load the real module through the real entry symbol, never a
  linked-in `TorrentManager`: `hydration_catalogue/test_torrent_failed_ingest_retry_and_pause_intent`
  drives the plugin's `Subsystem` directly (it must act on restored state
  before the worker starts), `rpc_cluster/…_from_non_owning_node` goes through
  a full `Service` with `plugin_path` pointed at the build tree, and new
  `hydration_catalogue/test_acquisition_api_without_a_torrent_plugin_reports_it_absent`
  and `subsystem_supervisor/test_subsystem_supervisor_reports_a_declining_plugin_as_unavailable`
  cover the absent and declined paths.

FUSE is unchanged and still linked into the executable; that is Phase 2.

## 0.27.0 — Root cause of "accepted metadata head cannot be reconstructed", fixed; broken heads now repaired live instead of quarantined (development)

The 0.26.1/0.26.2 outage was bounded but not explained. This release pins the
cause from the quarantined on-disk state of every affected node, removes it,
and changes what a node does if a head ever fails to reconstruct again for
any other reason. Protocol addition (one new control message), hence the
minor bump; older peers reject the new message by resetting the connection,
which the caller treats as "that peer cannot serve it" -- mixed-version
clusters keep working, they just cannot repair *from* an old peer.

- **Root cause (confirmed on disk, three occurrences).** A reconciliation
  merge commit is numbered `max(parents)+1` with the lower-hash parent as its
  primary (`MetadataManager::read_group()`), so its delta body legitimately
  sits several generations above that parent. Every history *writer*
  (`store_commit`, `import_history`, `load_history`) accepted that, and the
  existing test `test_merge_delta_primary_may_precede_merge_generation`
  encoded it as allowed -- but both *reconstruction walks*
  (`materialized_locked()` and `materialized()`) required exactly `parent+1`.
  The writer's own validation never noticed because it applied the delta to
  the parent straight from the materialization cache. Result: a merge commit
  durably written, hash-verified, readable only while it stayed in the
  64-entry cache, and permanently unreconstructable the moment it was
  evicted -- and since it was an accepted head, every reconciliation that
  could have folded it away had to materialize it first. Evidence: es-1's
  and gbni-1's quarantines both hold head `5e6ae738…` gen **8010** as a
  delta over parent `37d691c7…` gen **8004**; gbni-2's 2026-09-04 quarantine
  (25 GB) holds the identical shape (head 4969 → delta gen 4964 over gen
  4957, 22 such frames). The old test passed only because the reopened
  checkpoint happened to *be* the merge head, which seeded the cache.
- **Fix.** One succession rule, one implementation:
  `metadata_delta_succession_valid(parent, child)` (`metadata.hpp`), used by
  all three writers and both readers. The record hash binds the generation,
  so the walk only ever needed monotonicity. Every existing quarantined
  history on the cluster is fully reconstructible under it (verified with
  the new `macha-metadata-dump` tool below: `anomalies=0`, every head
  `reconstructible=yes`).
  Regression test: `test_merge_delta_with_generation_gap_reconstructs_after_cache_eviction_and_reopen`
  fails on 0.26.x at the very first `accept_commit` of the merge with a
  1-byte materialization cache.
- **Startup no longer throws the replica away over one head.**
  `MetadataReplica::load_heads()` used to throw on any unreconstructable
  certificate, sending the constructor down the recovery-seed path:
  `checkpoint.meta`/`history.log`/`heads.meta`/… all quarantined (450 MB on
  es-1, 830 MB on gbni-1, 25 GB on gbni-2) and the node reseeded from the
  persistent cache. Now the certificate is kept, the head is flagged
  (excluded from reads via the 0.26.1 cooldown), and one `ERROR` names the
  exact break (`diagnose_unreconstructable_locked()`: not indexed / parent
  absent / succession violated / cycle / unreadable frame / replay hash
  mismatch). A certificate whose *materialized* generation disagrees with it
  is still fatal -- that is a forged certificate, not a missing dependency.
- **Live repair, no restart.** New maintenance step
  `MetadataManager::repair_unreconstructable_heads()`: for each flagged head,
  ask each reachable peer for the record as a self-contained full body
  (`get_metadata_history_record = 41` → `MetadataReplica::
  full_history_record()`; the existing `get_metadata_history_entry` returns
  the peer's *stored* frame, which may be exactly the delta the caller cannot
  replay) and `MetadataReplica::reanchor_history()` it: append a full frame,
  re-point the index at it (history is append-only; `load_history()` now
  prefers a full frame over a same-identity delta rather than throwing
  "duplicate"), re-verify the acceptance certificate against the repaired
  record, clear the flag. A repair is reported as such even if a *different*
  flagged head still makes the materialized-head refresh throw.
  Tests: `test_unreconstructable_accepted_head_is_kept_at_startup_and_reanchored_live`
  (startup flagging, re-anchor of an absent head, refusal of a forged record,
  in-place supersession of an indexed frame, restart preferring the full
  frame) and `rpc_cluster/test_unreconstructable_accepted_head_is_repaired_live_from_a_peer`
  (the wire path end to end).
- **Diagnosis and the throw message.** `accepted metadata head cannot be
  reconstructed` now carries `hash=… reason=…`; the first failure of an
  episode logs one `WARN … excluded from reads pending live repair` with the
  same diagnosis. Both retry sites share the cooldown constant instead of
  duplicating it.
- **`macha-metadata-dump`** (`tools/metadata_history_dump.cpp`, installed
  next to `macha-metadata-repair`): read-only forensic decoder that never
  constructs a `MetadataReplica`, so it can examine a quarantined
  `*.corrupt.<ts>` directory: decodes `heads.meta`, walks every `history.log`
  frame, reports anomalies and legitimate generation-gap deltas, and for each
  accepted head walks the delta chain with the production predicate and
  names where materialization would fail. This is how the root cause was
  proven.
- Not changed, documented for the next person: `WARN local metadata delta
  rejected; retrying full record generation=N` precedes every reconciliation
  on the merging node -- the merging node stores each merge as a 15 MB *full*
  snapshot while shipping the compact delta to its peers. That is why
  history.log reached 25 GB on gbni-2. Separate issue; see
  `TODO/2026-09-06-unreconstructable-accepted-head-retry-storm-incident.md`.

## 0.26.2 — Bound the second, separate retry storm from the same 0.26.1 failure mode (development)

Found while writing up 0.26.1's own follow-up notes, not by further live
reproduction: a *second*, independent code path hits the same "cannot accept
this peer's metadata certificate" condition and was not covered by 0.26.1's
fix.

- `MetadataManager::read_group()`'s peer-certificate-survey loop
  (`metadata_manager.cpp`) calls `NodeRuntime::accept_metadata_commit()` for
  every observed peer head on *every* call -- i.e. on essentially every
  ordinary metadata read cluster-wide, same as `accepted_heads()`. When a
  certificate's underlying record can't be materialized locally,
  `MetadataReplica::accept_commit()` rejects it via its own early
  materialize check -- a completely separate code path from
  `accepted_heads()`/`refresh_materialized_head_in_memory_locked()`, and
  therefore untouched by 0.26.1's cooldown. This produced the `ignoring
  metadata head without a valid acceptance certificate` WARN (also seen
  during the 0.26.1 incident, interleaved with the other one) on every call,
  unthrottled.
- Fix: the same 30-second per-hash cooldown pattern, applied to this path
  (`unacceptable_head_retry_at_`, `MetadataManager`): a hash recently
  confirmed unacceptable is skipped outright on subsequent calls -- no
  repeated peer-import RPCs, no repeated rejection warning -- until the
  cooldown expires.
- Testing gap, disclosed rather than papered over: unlike 0.26.1's fix, this
  one has no dedicated regression test. `read_group()` requires a real
  multi-node RPC harness to exercise (`discover_accepted_heads()` makes
  actual network calls), and building that harness wasn't done here. Full
  test suite passes unchanged; this fix is verified by code reading and the
  same live incident's log evidence, not by a new automated test.

## 0.26.1 — Bound retry storms from an unreconstructable accepted metadata head (development)

Fixes a live 2026-09-06 incident found while stress-testing 0.26.0 with
deliberate concurrent multi-origin writes (movies from one node, TV from
another, into the same namespace at once): `corvus-es-1` and `corvus-gbni-1`
both fully stalled -- zero metadata mutations, zero object writes -- for
over 5 hours with no self-recovery, spinning `WARN media information
publication failed: ... accepted metadata head cannot be reconstructed` and
similar at 2-47 times/second.

- Root cause: `MetadataReplica::refresh_materialized_head_in_memory_locked()`
  and `MetadataReplica::accepted_heads()` both throw the instant any entry in
  the node's accepted-head set fails to reconstruct from local history, with
  no backoff. `accepted_heads()` alone is on the hottest path in the system
  -- called on essentially every metadata read, conflict reconciliation,
  catalogue publication, and checkpoint-maintenance tick -- so once one head
  became unreconstructable, every caller cluster-wide re-attempted and
  re-threw immediately, forever. The exact mechanism that made that one head
  unreconstructable was not conclusively pinned down (the likely trigger is
  a stalled `has_metadata_history_entry` RPC observed in the logs seconds
  before the stall began, interacting with peer history-import under the
  concurrent-write load) -- both known insertion paths into the accepted-head
  set (`store_commit`, `import_history`) were independently verified to
  already guard correctly against the specific chain-hole corruption that
  was the first suspect.
- Fix: both functions now rate-limit reconstruction attempts to once per 30
  seconds per hash. Within that window a still-broken head is treated as
  temporarily absent from the accepted set rather than a hard failure --
  callers that need exactly one head can converge on whichever other head is
  healthy, and ordinary reads keep serving the last good committed state
  instead of throwing on every call. This bounds the failure mode (one
  narrow reconstruction gap can no longer become an unbounded tight retry
  loop); it does not by itself explain or prevent whatever originally causes
  a head to become unreconstructable.
- Note for anyone who hits this: the fix only covers the runtime retry path.
  `MetadataReplica`'s constructor (`load_heads()`) re-validates every
  persisted accepted-head certificate at startup and is unaffected by this
  change -- a node restarted while still carrying the same broken head on
  disk will very likely fall into its existing recovery-seed fallback
  (quarantining the broken `history.log`/`heads.meta`, resuming from the
  last separately-cached committed snapshot in `recovery_required` mode)
  rather than simply resuming. `recovery_required` clears itself
  automatically on the first metadata read that completes by reconciling
  against a healthy peer (`MetadataManager::read_record_uncached()` calls
  `mark_recovered()` unconditionally on success) -- no manual repair step
  should be needed, but expect a one-time reset-and-resync, not a silent
  restart.
- Regression: `storage_metadata/test_unreconstructable_accepted_head_is_rate_limited_not_hammered`
  forces a real accepted head unreconstructable via a new test-only hook
  (`MetadataReplica::set_force_unreconstructable_for_tests`) and proves
  reconstruction is attempted once, not on every subsequent call.

## 0.26.0 — Batch and cheapen DATA retention-check presence probes, fixing a serial-decrypt/RPC scaling cliff (development)

Fixes the live 2026-09-06 `corvus-es-1` incident: a bulk movie rsync froze
data publication for minutes, pinned the maintenance thread near 100% CPU,
and produced a hard control-RPC timeout against an unrelated peer. Root
cause, evidence and phased fix are in
`TODO/2026-09-06-retention-check-batching-and-cheap-presence-plan.md`.

- `DistributedStore::retain_data()`'s per-extent candidate-presence scan --
  previously one `has_on()`/`have_object` round trip (a full local AES-GCM
  decrypt, or a synchronous network RPC) per (extent, candidate) pair, run
  serially on the thread that also drives metadata-publication acceptance --
  is now batched: candidates are grouped by node into a new `have_objects`
  wire message covering many extent IDs per request, and independent
  per-node batches run concurrently. Selection order and the
  `min_write_replicas` floor are unchanged; only the shape of how presence
  gets checked does. New `dht.retention_check_batch_size` (default 2000) and
  `dht.retention_check_concurrency` (default 8) config keys bound batch size
  and fan-out so an arbitrarily large publication cannot turn into an
  unbounded fan-out against one peer.
- `LocalStore::has()` -- already a cheap presence probe (an index lookup for
  packed objects, a single `stat()` for loose ones, never a decrypt) -- is
  now used for `has_on()`'s local candidate check instead of the
  full-decrypt `valid()`. It is also now `noexcept` and treats a zero-byte
  loose file as absent. This is safe specifically because the actual
  retention commit (`retain_on`/`retain_objects`) always re-verifies before
  persisting a claim; `rebalance_step`'s and `repair_step`'s presence checks
  have no equivalent downstream re-verification and deliberately stay on the
  full-decrypt path, so a corrupt replica still can't be counted as healthy
  placement.
- **Protocol addition:** `MessageType::have_objects`/`have_objects_reply`
  (wire values 40/118). An older peer mid-rollout answers it as an
  unrecognized message and the caller falls back to the pre-existing
  per-extent `have_object` path for that peer -- degraded speed during a
  rolling upgrade, not a correctness or availability issue.

## 0.25.0 — Foundation for subsystem crash isolation: a mandatory thread guard, a shared macha_core, and a dlopen'd plugin loader (development)

Phase 0 of `TODO/2026-09-05-subsystem-plugin-isolation-plan.md`, the design
response to 0.24.4's `corvus-es-1` crash-loop (an uncaught exception during
`FuseFrontend` construction took the entire node down, not just the FUSE
mount). Single binary, single process throughout -- no separate OS processes
or IPC. No subsystem has migrated onto the new interface yet (Torrent/FUSE
plugin migration is Phase 1/2); this release only lays the foundation.

- Added `run_supervised()`, the mandatory entry point for every
  subsystem-owned thread: it catches any exception (including non-`std::exception`
  throws) at the top of the thread body and logs instead of letting it
  escape. An exception escaping a `std::jthread`/`std::thread` lambda calls
  `std::terminate()` directly -- it never reaches any caller's `catch`, so
  this could not have been fixed by wrapping `main()` more carefully. Applied
  to all ~30 background thread/worker-pool construction sites across the
  codebase; a new regression test
  (`foundations/test_every_subsystem_thread_is_run_supervised`) scans every
  `src/*.cpp` and fails if a future thread construction bypasses it.
- `macha_core` is now a shared library (`libmacha_core`), not a static one,
  so it, the `macha` executable and every future subsystem plugin share
  exactly one copy of its classes and global/static state. Fixed the two
  link seams this broke (`make_libav_media_engine`,
  `apply_embedded_music_metadata`/`embedded_music_metadata_from_host`): the
  real FFmpeg-backed implementations now register themselves into
  `macha_core` at static-init time instead of being resolved as unresolved
  symbols by whichever executable links them, which only worked when
  `macha_core` was static. `media_engine_stub.cpp`/`media_metadata_stub.cpp`
  are removed -- their no-op behaviour is now `macha_core`'s own default
  when nothing registers a real implementation.
- Added the `Subsystem`/`SubsystemContext` interface and a versioned plugin
  ABI (`subsystem_abi.hpp`): a plugin exports one `macha_subsystem_entry()`
  symbol carrying a build-identity stamp (project version + git commit) that
  must match the running core's own before it is ever constructed -- a
  plugin built against a different core revision (a partial deploy) is
  refused at load time, not run with an incompatible ABI.
- Added `SubsystemSupervisor`: discovers `.so`/`.dylib` plugins in a
  configured directory, and keeps each one's construction/start attempt
  behind a backed-off retry loop that disables itself after too many
  failures in a window rather than crash-looping forever -- the general
  fix for the exact failure class that crash-looped `corvus-es-1`. Verified
  with real `dlopen`'d fault-injection plugins (`tests/plugins/`), not
  in-process mocks: a plugin whose `start()` always throws is retried,
  faulted and disabled without the test process itself going down; a
  plugin with a mismatched build identity is refused outright.
- Added `plugin_path` to `Config`/`config.yaml` (default: the directory
  containing the running executable), and a `subsystems` block to
  `GET /api/v1/status` reporting each loaded subsystem's name/state/restart
  count -- currently always empty, since nothing has migrated onto the
  interface yet.
- **Breaking config change:** top-level `mount_path` moved to
  `fuse.mount_path`, ahead of FUSE's own migration to this interface in a
  later phase. A top-level `mount_path` now fails startup with an explicit
  obsolete-key error instead of being silently ignored. Every deployed
  node's `macha.yaml` needs this key moved before upgrading.

## 0.24.4 — Fix a crash-loop introduced by 0.24.3's namespace-skip escape hatch (development)

- Fixed a same-night regression in 0.24.3's `skip_blocked_namespace_operation()`:
  it journals a `namespace_done` marker for an operation that was
  deliberately abandoned without ever being published, but the durable
  journal's replay validator required every `namespace_done` to have a prior
  `namespace_published` record for that sequence -- a real invariant for the
  ordinary success path, but one the new operator-skip path never satisfies
  by design. The very next process restart replayed that record, threw
  `DecodeError("invalid FUSE namespace completion marker")`, and crashed;
  because the record is durable, every subsequent restart hit the same
  failure, crash-looping indefinitely. Surfaced live in production on
  `corvus-es-1` (49 restarts before diagnosis) after deploying 0.24.3.
  Fixed by no longer requiring `namespace_published` for `namespace_done`
  during recovery, since "abandoned without publishing" is now a legitimate
  terminal state alongside "confirmed published." Regression:
  `test_fuse_journal_replays_operator_skipped_op_without_a_published_marker`.

## 0.24.3 — Fix ingest job resurrection, a metadata data race, and a wedged FUSE namespace queue (development)

- Fixed cleared ingest jobs resurrecting: seven sites in `src/ingest.cpp` read
  or wrote `jobs_[job.id]` via `operator[]`, which default-constructs a fresh
  `queued` job if an operator's `clear()` had already erased a terminal job
  while its worker was still inside `process_job()`/`plan_job()`/
  `copy_file()`/`import_job()` -- silently re-inserting a job the operator
  just removed. All seven now use `find()` and treat "already gone" the same
  as cancelled. Regression:
  `test_cleared_ingest_job_does_not_resurrect_while_worker_finishes`.
- Fixed a genuine data race on `LocalStore::last_mutation_generation_`:
  `scan()` wrote it outside `m_` while `durability_barrier()` read it under
  `m_`. The write now happens under the same lock.
- Fixed `MetadataReplica::accept_commit()` writing the full committed
  snapshot to the checkpoint file (`reset_checkpoint`, potentially hundreds
  of MB) while holding the replica's global mutex, on the metadata RPC path
  -- every other reader/writer needing only that mutex would queue up behind
  one slow fsync. The checkpoint write now happens after the mutex is
  released, serialized only against other durable-mutation writers, mirroring
  the same off-lock-write/on-lock-bookkeeping pattern `import_history()`
  already used for `write_history_frame`. Regression:
  `test_accept_commit_refreshes_checkpoint_on_both_repair_and_ordinary_paths`.
- Added an operator escape hatch for a FUSE namespace operation wedged on a
  non-retryable backend error: the publication worker previously retried the
  same operation forever on a 5s backoff with the entire queue blocked behind
  it, and no way to skip it. New `GET/POST
  /api/v1/manage/filesystem/blocked-namespace-operation[/skip]` routes (via
  new `FuseFrontend::blocked_namespace_operation()`/
  `skip_blocked_namespace_operation()`) let an operator who has independently
  confirmed it is safe explicitly abandon the exact stuck operation by
  sequence number, without falsely claiming its effect was achieved. This
  never happens automatically. Regression:
  `test_fuse_namespace_operator_skip_unwedges_a_non_retryable_backend_error`.
  Surfaced live during a `rm -rf` recovery on the production cluster; the
  actual root cause of that incident was operator error (an interrupted
  `rm -rf /mnt/machamedia`, recovered from ext4 backups), not a Macha bug.

## 0.24.2 — Fix status() blocking on a contended subtitle-cache lock (development)

- Fixed `PlaybackManager::status()` taking each session's subtitle-cache
  mutex while still holding the global session mutex, and doing so with a
  blocking lock: a single slow WebVTT extraction on one session could stall
  `status()` and, transitively via the global mutex, every other playback
  operation (create/patch/delete/cleanup). The per-session subtitle-cache
  read now happens after the global mutex is released and is `try_lock`-only,
  so a busy session simply contributes a stale/zero count to that snapshot
  instead of blocking. Regression test:
  `test_status_does_not_block_on_a_contended_subtitle_cache`
  (`tests/test_media_playback.cpp`). Separately confirmed, on code review,
  that `public_stream_response` does not hold the global mutex across libav
  work as previously suspected -- see `TODO/ACTIVE.md` item 2.

## 0.24.1 — Fix false-consensus metadata history compaction, add conflict-preserving manual repair (development)

- Fixed a real production incident: `NodeRuntime::accept_history_checkpoint_proposal()`
  acked a history-compaction floor proposal unconditionally, with no check that
  the acking replica had actually reached that floor itself. A lagging replica
  (offsite, higher latency) proposed compacting to its own stale floor; two
  already-more-advanced replicas both acked it anyway, manufacturing a false
  3/3 consensus. The lagging replica then compacted to that stale point,
  permanently discarding the only bridging history connecting it to the rest
  of the cluster's current state -- leaving every node with two accepted
  metadata heads and no common ancestor, and the catalogue fully unavailable
  cluster-wide. `MetadataReplica::record_checkpoint_ack()` now refuses to ack
  a floor unless it exactly matches the replica's own current single accepted
  head; `compact_history_if_safe()`'s own local safety check can no longer be
  fed a false premise by a peer.
- Added `plan_conflict_preserving_metadata_repair()` and three new
  `macha-metadata-repair` actions (`--plan-conflict-merge`,
  `--stage-conflict-merge`, `--accept-conflict-merge`) for the case the
  existing causal-dominance repair explicitly refuses: two heads with no
  common ancestor where neither strictly dominates the other (concurrent
  divergence). Reuses the existing three-way `merge_metadata_snapshots()`
  with a deliberately empty base -- every entry identical on both sides
  reconciles regardless of base, and any entry that actually differs becomes
  a durable first-class conflict (both alternatives preserved, nothing
  destroyed or silently chosen) exactly as ordinary reconciliation would
  record it. Refuses outright if either head has an entry the other lacks,
  since an empty base also disables the rename/move-collision detection a
  real common ancestor would drive.
- Added `macha-metadata-repair --diff-heads` to print exactly which
  namespace paths differ between two accepted heads, field by field, instead
  of only the aggregate counts the existing report prints.
- Used the above to recover the live 3-node cluster from the incident:
  restored 1545 of 1546 namespace entries immediately; the one entry that
  had genuinely diverged (a single in-progress ingest, one replica's copy a
  strict continuation of the other's) was preserved as an explicit conflict
  rather than guessed at. No media data was lost -- DATA extents are a
  separate storage class from the metadata/namespace layer this incident
  affected, and were confirmed untouched on disk throughout.

## 0.24.0 — Cluster session/auth subsystem, replacing the old viewer-session headers (development)

- Added a real `/api/v1/session` REST resource: `POST` mints a bearer-token
  session (anonymous-only for now, empty credentials), `GET` introspects the
  caller's own session, `DELETE` revokes it. The minted token is now required
  as `Authorization: Bearer <token>` on every API call except the
  already-self-authenticating signed streaming/asset capability URLs. This
  closes a standing gap where auth was optional whenever `token_file` was
  unset -- there is no more "disabled" state; every non-exempt request now
  needs a live session.
- Sessions are small (id + roles + timestamps), cluster-replicated: a create
  or revoke is pushed synchronously to every reachable peer
  (`NodeRuntime::propagate_session`), with a periodic best-effort gossip
  backstop for anything a peer missed while unreachable, modelled on the
  existing `TelemetryStore` gossip shape rather than `Membership`'s
  tombstone-only pattern (sessions need real expiry and deletion). Each node
  holds its own full replica in memory, so a local bearer-token lookup is
  always O(1) with no network round trip -- there is no separate cache layer,
  a pushed mutation *is* the cache invalidation everywhere else. Persisted to
  local disk (best-effort, alongside the existing telemetry checkpoint) so a
  restarted node doesn't need every client to re-authenticate.
- Built with two extension seams for later work: a `CredentialValidator`
  interface (only an anonymous validator exists today; real credentials are a
  drop-in replacement) and a `session_has_role()` helper (not yet wired into
  any route -- per-route/action role gating is still a separate future step).
- Retired the ad hoc `Macha-Viewer-Session`/`X-Macha-Viewer-Session` headers
  and `viewer_session_id` body field: playback's "logical viewer" correlation
  (replacing an in-flight session instead of stacking a second stream) is now
  tied 1:1 to the authenticated session, which also closes a minor spoofing
  gap (the old idempotency fingerprint trusted an arbitrary client-supplied
  key). A client wanting several independent concurrent viewers now mints one
  anonymous session per viewer.
- Moved playback session creation's `Idempotency-Key` from a request header
  to an `idempotency_key` query parameter on `POST /api/v1/playback/sessions`.
  The response no longer echoes the key back, but still reports
  `X-Macha-Idempotency: created|replayed`, since that value carries real
  information the client can't otherwise infer.
- Fixed during joint live testing with the client: `SessionManager::create()`
  never actually checked `max_sessions` -- only `apply()` (the gossip-received
  path) did. A client bug (a proactive-refresh timer whose delay silently
  overflowed on a ~30-day TTL) caused a real re-mint storm against a live dev
  node; the server handled the load fine but would have grown its local
  session replica without bound had the storm run longer. `create()` now
  returns `std::nullopt` (surfaced as `429 too_many_sessions`) once the local
  cap is reached.

## 0.23.11 — Snap transcode seeks to source keyframes, cutting seek startup cost (development)

- Live-measured a seek deep into a transcoded HEVC Main10 title costing
  ~7s to first fragment versus ~2s from position zero. Root cause: for
  transcode (unlike remux), a seek lands on the nearest source keyframe but
  then fully *decodes* (not just skips) every frame between that keyframe
  and the exact frame-accurate requested position, purely so encoding can
  start at that exact frame -- a viewer does not need that precision, and it
  can cost a full GOP's worth of software HEVC decode on slow hardware.
- Fixed by snapping the transcode seek itself to the nearest keyframe at or
  after the target (`media_vod::nearest_keyframe_at_or_after`), in both the
  cold-session-creation path and the interactive PATCH-seek fast path
  (`reseek_hls_vod`). Deliberately does not reuse remux's `indexed_plan`
  for this: that function also validates keyframe density across the
  *entire remaining file*, which is irrelevant to transcode (it lays down
  its own GOP structure regardless) and was observed to silently reject the
  snap -- with no error, just silently falling back to the old frame-accurate
  behaviour -- whenever any other part of a long file had a sparser GOP.
- Second bug caught only via live measurement showing no improvement despite
  correct-looking code: the keyframe's timestamp was rounded to milliseconds
  with `llround` (nearest), which can round a fractional-millisecond
  keyframe timestamp *down*; reconstructing microseconds from that
  truncated value then made the container seek land one keyframe *earlier*
  than intended -- a full extra GOP decoded for nothing. Fixed by rounding
  up (`ceil`) instead, everywhere a keyframe timestamp (not a raw user
  seek request) is converted to milliseconds.
- Verified live: ~7s reduced to ~4s for a fresh seek on quiet hardware
  (position zero remains ~2-2.5s). A residual ~1.7s gap versus position
  zero remains unexplained and is left for follow-up, rather than guessed
  at -- a stream-info/container-seek timing diagnostic was added
  (`media playback pipeline seek timing`) to help isolate it.
- Also fixes the interactive PATCH-seek case, though live measurement there
  was confounded by CPU contention between the old (still-encoding) and new
  pipeline during handover on constrained hardware -- a separate, likely
  larger effect for real-world scrubbing, not addressed here.

## 0.23.10 — Fix seek/generation-replacement stall on stale segment requests (development)

- Live-diagnosed via a real seek into Apollo 13 on a running node: a segment
  request against a superseded generation (e.g. client read-ahead, or a
  request already in flight when a seek arrives) could block for the full
  duration of the *replacement* pipeline's startup (`streaming.startup_timeout_ms`,
  15s default) before returning its 404. `update_session`'s seek path starts
  the replacement pipeline synchronously before stopping the old one (by
  design, to avoid a resource-accounting race -- see the ordering comment at
  the `stop_pipeline`/`start_pipeline` call site), and only `stop_pipeline(old)`
  wakes anything blocked in `MediaSegmentStore::wait_object()` on the old
  generation's store via `cancel()`. Nothing woke those waiters earlier.
- Fixed by adding `MediaSegmentStore::mark_superseded(bool)`, a lighter,
  reversible signal distinct from `cancel()`: it wakes `wait_object()` callers
  blocked on a not-yet-produced segment (so they get a prompt 404 `not_ready`)
  without stopping production or setting `finished`/`error`. `update_session`
  now marks the old generation's store superseded immediately, before
  attempting to start the replacement pipeline; if that attempt then fails,
  it is cleared again so the still-active old session keeps its normal
  long-poll behaviour. This does not change the existing resource-handover
  ordering/invariant at all -- `stop_pipeline(old)` still only runs once the
  replacement is confirmed.
- Added a regression test exercising `mark_superseded`'s wake and reversal
  behaviour directly against `MediaSegmentStore`.
- Not yet confirmed whether this was the cause of a separately reported live
  symptom (apparent ~1-2s A/V offset partway through a title) -- reproducing
  that report during investigation showed a seek triggering this exact stall
  cascading into the client's failover logic and losing playback position,
  but it was not established whether a real seek/PATCH occurred in that
  specific live report versus continuous playback simply outrunning
  real-time production. This fix addresses the confirmed stall either way;
  whether it resolves the original report remains to be observed live.

## 0.23.9 — Fix audible pitch shift introduced by 0.23.8's drift correction (development)

- 0.23.8's `swr_set_compensation()`-based correction genuinely worked (bounded
  drift as measured), but the correction mechanism itself was wrong: nudging
  the resample ratio changes pitch, and no bound on that rate is small enough
  to be inaudible to a sensitive listener — reported live as an unacceptable,
  clearly audible pitch shift. That mechanism has been removed entirely
  (`maintain_audio_drift_compensation()`, `swr_set_compensation()`, and its
  supporting counters are gone).
- Replacement: enable libswresample's own built-in correction directly
  (`av_opt_set_double(swr, "async", 1, 0)` plus `swr_next_pts()` fed the real
  source PTS each frame) — the same machinery behind ffmpeg's own `-async 1`
  and the `aresample` filter's default behaviour. This corrects by injecting
  silence or dropping samples (`swr_inject_silence()`/`swr_drop_output()`),
  never by changing the resample ratio: `max_soft_comp` (the opt-in
  pitch-bending stretch/squeeze path) is left at its disabled default and is
  never touched by this code, so a pitch shift is structurally not possible
  through this path.
- This replacement had its own near-miss during development: an initial
  attempt tried to avoid overflowing `swr_next_pts()`'s required
  `AVRational{1, in_rate*out_rate}` denominator by dividing it by
  `gcd(in_rate, out_rate)` — but that unit is a unit fraction (numerator 1)
  and a unit fraction cannot be reduced by any GCD (`gcd(1, N)` is always 1).
  The result was silently ~48000x too coarse for the common 48kHz -> 48kHz
  case, which made libswresample believe it was catastrophically far ahead
  and drop nearly all audio (measured: 5 packets survived an 8-minute
  capture). Caught in local verification before reaching any node clients
  actually watch on. Fixed by rescaling into the safe `1/in_rate` unit first,
  then multiplying by `out_rate` as plain `int64_t` arithmetic — never
  constructing the overflow-prone `AVRational` at all, matching the pattern
  ffmpeg's own `libavfilter/af_aresample.c` uses.
- Re-verified against the same real title after the fix: audio packet count
  and duration are healthy across a full 8-minute capture (no dropped audio),
  and the audio/video offset still oscillates within roughly ±15ms instead of
  growing unbounded — same drift-correction quality as 0.23.8's measurement,
  now via a mechanism that cannot shift pitch.
- Same known gap as 0.23.8: no automated regression test yet for the real
  (non-stub) transcode audio path; both this fix and the incident it fixes
  were only caught by live measurement and live listening, not CI. Building
  that harness (Phase 0 of the resilience plan) would have caught both the
  pitch shift and the near-total-silence regression before either shipped.

## 0.23.8 — Bounded audio drift compensation for transcoded playback (development)

- Transcoded audio's presentation clock (`StreamPipeline::audio_next_pts` in
  `src/media_engine.cpp`) was a free-running sample counter, seeded from the
  real source timestamp once at pipeline start and never re-anchored
  afterward — unlike video, which re-derives its PTS from the real source
  timestamp on every single frame. Any systematic mismatch between resampled
  output sample count and real elapsed source duration (resampler rounding,
  EAC3 frame timing, channel downmix) compounded without bound for the life
  of the stream. Measured live against a real title (EAC3 5.1 -> AAC stereo):
  ~0.4ms drift per second, extrapolating to hundreds of ms over a feature-length
  film — matches user reports of audible desync a few minutes into playback.
- Fix: `maintain_audio_drift_compensation()` periodically compares the
  running produced-sample count against the expected count derived from the
  current frame's real source PTS, and uses `swr_set_compensation()` to
  gradually nudge the resample ratio back toward the source timeline (bounded
  to roughly 1% speed adjustment per correction window) rather than either
  leaving the drift unbounded or snapping to a corrected PTS (which would
  produce an audible click).
- Verified against the same real title: offset now oscillates within roughly
  ±15ms across an 8-minute sample instead of growing monotonically, well
  under the threshold where A/V desync becomes perceptible.
- Known gap: no automated regression test exists yet for the real (non-stub)
  transcode audio path — `macha-tests` links a stub media engine
  (`src/media_metadata_stub.cpp`) for speed, and this fix is verified only by
  live measurement against production content, not a deterministic CI case.
  Building that harness is exactly the Phase 0 exit criterion already
  described in `TODO/2026-09-03-playback-resilience-and-av-sync-plan.md`;
  tracked there rather than duplicated here.

## 0.23.7 — Per-node advertised API address fixes any-node Direct Play failover (development)

- `GET /api/v1/status` now reports `api_host`/`api_port` on every entry in
  `nodes[]`: where clients should reach that node's HTTP/catalogue API,
  distinct from the existing `host`/`port`, which is the node's internal RPC
  bind address and was never a reliable (or even necessarily correct-protocol)
  address for REST calls. Any-node Direct Play failover was guessing peer
  ports from the RPC bind address and landing on the wrong port; it now uses
  this field instead.
- New optional `catalogue.api.advertised_host` / `advertised_port` config
  covers NAT/port-forwarding, mirroring the existing `network.advertise`
  pattern for the RPC port. `advertised_host` defaults to this node's
  resolved RPC advertise address (not the bound `listen`, which is
  conventionally a wildcard bind and not itself dialable); `advertised_port`
  defaults to the bound `port`. Distinct from the existing self-only
  `connectivity.advertised` field, which covers this node's own
  external/UPnP RPC-port connectivity, not peers' API addresses.
- Carried over the existing gossip wire's trailing-optional-field pattern
  (`NodeTelemetry`) so nodes mid-rollout on the previous version keep
  interoperating; older peers simply don't report `api_host`/`api_port` yet.

## 0.23.6 — Safe distributed checkpoint and metadata-history compaction (development)

- `MetadataReplica::compact_history_if_safe()` — an existing, tested local
  primitive that re-roots `history.log` at the sole committed accepted head —
  is now actually called in production, gated behind a new cluster-wide
  distributed checkpoint protocol. Previously it had zero callers: a
  generation-only safety check was known to be unsafe (a returning accepted
  branch could outlive the common ancestor on every replica), so history
  compaction stayed permanently disabled and `history.log` grew without
  bound.
- New leaderless propose → durable-ack → commit → prune round
  (`MetadataManager::attempt_history_checkpoint()`, new `propose_history_floor`
  / `commit_history_floor` RPCs, new durable `checkpoint-proof.meta` per
  replica). Compaction only fires once every durably-known participant has
  durably acknowledged the exact same accepted-head hash as the new ancestry
  floor — mirrors the existing `all_known_reachable()` gate already used for
  destructive object GC. A restart only ever trusts a proof that still
  validates against the replica's current committed head.
- A returning node whose own compaction floor has since been pruned
  everywhere else in the cluster (peers compacted further while it was
  unreachable) now converges automatically instead of getting stuck
  advertising an unmergeable rootless sibling forever.
- No client-visible behavior change. Existing nodes with multi-gigabyte
  `history.log` files (from the previously-unbounded retention) shrink back
  to a single root record once the cluster completes its first round.

## 0.23.5 — Cluster-wide ingest/torrent job visibility and control (development)

- `GET /api/v1/ingest/jobs` and `GET /api/v1/torrents/jobs` (list and
  single-job) now answer with every job in the cluster, not just the jobs
  owned by the node the client happened to talk to. Each job is tagged with
  its owning `node_id`. Implemented as an on-demand RPC survey of active
  peers (new `get_ingest_jobs`/`get_torrent_jobs` wire messages), not
  replication — a peer that can't be reached is skipped, not fatal to the
  request.
- `POST .../jobs/{id}/{pause,resume,retry,cancel,clear}` now works
  regardless of which node's API receives the request: if the job isn't
  owned locally, the action is forwarded to the owning node via a new
  `ingest_job_action`/`torrent_job_action` RPC and the result (including the
  updated job, still tagged with its `node_id`) is returned as if it had
  been handled locally. The existing 404-vs-409 semantics (job not found vs.
  job can't perform that action in its current state) are preserved
  cluster-wide.
- No client-visible API surface changed beyond the new `node_id` field on
  each job — existing UIs keep working unmodified, just with complete
  visibility instead of a partial, node-dependent view.

## 0.23.4 — Signed artwork capability URLs (development)

- Embed a signed, short-lived capability URL (`?exp=...&sig=...`, HMAC'd with
  the cluster auth key) directly on each artwork reference in catalogue
  responses (`GET /api/v1/catalogue/items`, `/items/{id}`, `/search`),
  alongside the existing bare `id`/`role`/`mime_type` fields. This lets a
  client load artwork via a plain `<img src>` without attaching a bearer
  header, the same way playback stream/subtitle URLs already carry their own
  embedded authorization rather than requiring a separate header. Unlike the
  session-scoped stream token, artwork has no session to anchor a validity
  window to, so the expiry is explicit and carried in the URL; default TTL is
  24h, configurable via `catalogue.api.artwork_capability_ttl_ms` — long
  enough that normal browsing/caching isn't disrupted, with an expired URL
  recovered by simply re-fetching the catalogue item. The existing
  header-authenticated `GET /api/v1/catalogue/artwork/{id}` endpoint is
  unchanged and still works with a bearer token; the signature is verified
  (not just the URL shape) before the request is ever exempted from that
  check, so an unsigned request to the same path still requires the ordinary
  bearer token when one is configured. Artwork responses also now carry
  `Cache-Control: public, max-age=<ttl>, immutable`, since the id is a
  content hash and the bytes it names never change.

## 0.23.3 — HTTP keep-alive, honest Status telemetry, and metadata/startup reliability (development)

- Implement bounded HTTP/1.1 keep-alive for the catalogue/media API server
  instead of closing every connection: reused connections are bounded by
  configurable `keep_alive_max_requests` and `keep_alive_idle_timeout_ms`,
  yield early under accept-queue backlog so a busy or idle keep-alive
  connection cannot starve a waiting new connection, and always close rather
  than reuse when a request body was not fully drained from the wire.
- Stop presenting stale peer telemetry as current: `/api/v1/status` now
  treats a live sample older than the freshness window exactly as if no live
  sample existed for `storage`/`cache`/`runtime` figures (falling back to
  durable last-known data, or explicit unavailability), rather than showing
  arbitrarily old RSS/CPU/load/capacity numbers as authoritative merely
  because a sample was once observed.
- Add a truthful per-node `phase` (`starting`, `recovering`, `ready`) to
  telemetry so a node's own in-progress local recovery — which legitimately
  reports zero capacity/usage before its storage is ready — is no longer
  indistinguishable on the wire from a genuinely empty node. A recovering
  peer now reports `state: "online"` (control-plane reachable, which is
  true) with `phase: "recovering"` and non-authoritative storage/cache
  figures instead of fabricated-looking zero, and `cluster.conditions`
  reports "one or more online nodes are still recovering" for that window.
  This directly fixes the 0.23.1 rolling-deployment incident where Status
  reported all nodes green with plausible load figures while nodes were
  stopped or 80-100 seconds into recovery.
- Serialize foreground metadata-head reconciliation (`MetadataManager::read_group()`)
  behind a dedicated `reconciliation_mutex_`, re-checked after acquisition, so
  concurrent reads observing the same accepted-head divergence produce at
  most one merge commit instead of each independently publishing its own.
  Unlike the write/repair paths, ordinary reads previously took no lock at
  all here; under real operational churn (node restarts/reconnects) this let
  redundant, mostly `Body::full` reconciliation commits accumulate on every
  concurrent read during a divergence window, which is the direct cause of a
  live node's metadata history growing from 37MB to 20.9GB in under a week.
  The underlying divergence-tolerant merge/history-fetch machinery is
  unchanged; only concurrent access to the merge-and-publish step is now
  serialized.
- Bound `Service::wait_services_ready()`, which previously waited on local
  startup with no timeout at all: a rare (reproduced at roughly 1-in-30
  startups under stress) internal stall below the readiness/subsystem
  construction path — one that neither completes nor throws — could hang a
  node forever with no diagnostic and no way for the process supervisor to
  intervene, since systemd's `Restart=on-failure` only ever triggers once a
  process actually exits. Add configurable `service_startup_timeout_ms`
  (default 120000); on timeout, log the last-known per-subsystem readiness
  state and terminate the process outright rather than attempt an ordinary
  exception unwind, which would try to join a startup thread that may be
  permanently blocked and hang identically in `stop()`. Restart-driven
  recovery replay on the next boot is what actually resolves the stalled
  state. The root cause of the underlying rare stall itself — a suspected
  lock or lost wakeup somewhere in subsystem construction/start — was not
  pinned down and remains open; this bounds its worst-case impact rather than
  eliminating it.

## 0.23.1 — bounded FUSE recovery failure (development)

- Stop terminal asynchronous publication failures from being immediately
  re-admitted as deferred work. A journal-restored inode whose accepted
  namespace path has disappeared now remains explicitly poisoned for recovery
  instead of consuming a worker and flooding logs indefinitely; unrelated FUSE
  paths and control traffic remain serviceable.
- Add a crash analogue covering durable spooled writes followed by accepted
  namespace removal, proving exactly one terminal attempt, scheduler quiescence,
  and continued access to unrelated files.

## 0.22.2 — metadata reconciliation recovery (development)

- Reject compact metadata deltas which cannot reproduce the exact immutable
  snapshot byte ordering, including reconciliation which canonicalises an
  append-ordered garbage/tombstone set.
- Give local metadata replicas the same single-shot full-record fallback as
  remote replicas when a compact body fails exact reconstruction, preventing a
  valid library from becoming permanently unavailable behind a failed merge.
- Make node identity-association reset admission asynchronous: the API returns
  `202` after its small local durable tombstone, while peer propagation and the
  cluster metadata audit execute off the request path.
- Retire reset identities from ordinary status listings, cluster health and
  capacity totals while preserving an explicit per-node `state: "retired"`
  audit response.

## 0.21.0 — asynchronous node startup (development)

- Bring the RPC control plane and status API online before local backend recovery.
- Report startup/readiness independently from cluster reachability and storage availability.
- Make metadata-history cold-start linear and add periodic full-history anchors.
- Build UPnP support against miniupnpc API 18 headers which predate the symbolic
  `UPNP_GetValidIGD()` return-value macros, while preserving private-WAN and
  disconnected-gateway classification.
- Install Linux systemd units in systemd's unit search path rather than a
  Debian multiarch library directory, including migration of the old cached
  default and warning-free generated install scripts.
- Turn configurable FUSE spool capacity into event-driven write backpressure:
  pressure starts publication, admission progressively follows measured drain
  throughput, and saturation blocks writers rather than returning logical
  `ENOSPC`.

## 0.19.0 — metadata replica availability

- Replace fixed metadata-voter placement with all-node metadata replication. Every active node is eligible to durably store namespace and catalogue-control metadata.
- Add `dht.metadata_min_write_replicas`: the literal number of distinct active metadata replicas which must durably store an immutable commit before it may be accepted. Legacy `dht.metadata_replicas` remains a migration-only alias and maps its old voter count to the equivalent former majority write floor.
- Replace the live metadata CAS/PREPARE/COMMIT state machine with immutable commit storage plus durable acceptance certificates. A replica stores a valid commit without comparing it to its current head; acceptance records the distinct durable store witnesses and survives their later absence.
- Persist a maximal accepted-head set on each replica. Independent partition branches coexist instead of competing for a single current slot; accepted ancestors fall out of the head set only when a descendant/merge commit supersedes them.
- Add protocol-20 `put_metadata_commit`, `accept_metadata_commit`, and accepted-head discovery RPCs; reject the legacy network CAS/PREPARE/COMMIT RPC family so live code cannot accidentally re-enter the old consensus model.
- Add encrypted compact metadata ancestry/history, exchange it between arbitrary replicas, collapse stale ancestor heads, and reconcile divergent accepted heads through deterministic two-parent merge commits. N simultaneous heads converge by repeated deterministic DAG merges rather than winner selection.
- Merge non-conflicting namespace changes automatically and preserve incompatible namespace/catalogue-root alternatives as durable first-class conflict records instead of silently choosing a winner.
- Invalidate metadata caches on an independent peer-observation epoch so same-generation sibling heads are discovered promptly; numeric generation alone is no longer a complete freshness signal.
- Let any active replica initiate virgin-cluster genesis. All virgin founders construct the same deterministic generation-2 root and publish it through the ordinary commit-store/acceptance path; there is no genesis coordinator, voter or PREPARE arbitration. Bootstrap-configured joiners remain fenced until their peer survey completes.
- Publish catalogue CONTROL objects against the same any-node metadata write floor and converge them opportunistically to active replicas.
- Fence catalogue CONTROL GC by catalogue-root epoch as well as object age: data-before-metadata staging survives the root epoch in which it is first observed/re-affirmed and cannot be reclaimed by a stale maintenance live-set during root publication.
- Replace metadata voter/quorum status with replica/write-floor fields while retaining deprecated 0.18 JSON aliases for client compatibility.
- Keep non-destructive DATA repair running from an accepted local branch while metadata reconciliation/validation is pending.
- Separate write availability from convergence validation: a reachable `metadata_min_write_replicas` cohort remains writable while reconciliation is pending, with validation reported independently as stability telemetry.
- Add durable causal DATA/CONTROL retention claims on physical copies. Metadata publication re-affirms referenced immutable objects before acceptance; local/remote deletion, placement eviction and GC refuse to remove a claimed copy. Missing claimed copies are repaired in bounded background slices even when the claiming branch is absent from the node's current namespace view.
- Keep GC useful during partitions: unclaimed staging/extra copies remain reclaimable, while inherited claims are released only after every currently known metadata replica is online and the accepted-head set is validated coherent. This prevents one partition from erasing an ancestor object still required by another accepted branch.
- Keep the 0.18 on-disk storage layout readable; bump the peer protocol to 20 because the metadata publication contract is intentionally incompatible with protocol 18/19.
- Fix protocol-20 compact metadata deltas so ordinary mutations retain SM12/SM13 write-floor and retention-governance state, persist completed retention baselines even with an empty migration roster, and preserve mutation-sequence counters across restart.
- Keep legacy accepted-head certificates readable during upgrade while preventing a protocol-20 branch from downgrading to legacy authority, and route background maintenance through the same virgin-cluster policy fencing as foreground discovery.
- Split dependency-free backend tests from concrete yaml-cpp/FFmpeg runtime-adapter tests so storage, cluster, filesystem, catalogue and playback policy coverage can build and run on constrained development hosts; add `run-tests.sh` to execute every test binary present in a build tree.

## 0.18.2 — cluster status telemetry

- Add optional UPnP IGD public port mapping, AWS external-IP fallback, boot/manual public endpoint self-probing, runtime advertised endpoint updates, and Status API diagnostics.

- Add live node telemetry on the authenticated peer protocol, gossiped across the cluster with boot-incarnation sequencing, freshness expiry and rolling-upgrade compatibility.
- Persist a bounded coalesced last-known telemetry cache independently on each node; routine status persistence never mutates the namespace, enters metadata quorum/CAS, or replays historical telemetry. Legacy SM9/DLT3 status records remain readable for compatibility.
- Add durable SM10/DLT4 cluster-wide endpoint→NodeId reset tombstones under `/api/v1/manage`, including endpoint-, NodeId-, and IP-only scopes, live membership/telemetry/RPC eviction, stale-gossip suppression, propagation, confirmation metadata, and audit logging.
- Improve wrong-node RPC diagnostics with endpoint, expected NodeId, and the actually authenticated NodeId.
- Add `/api/v1/status` cluster/node status surfaces with authoritative live membership, explicit metadata availability (`unavailable`, `read-only`, `writable`), known-versus-online storage/cache totals, optional runtime/load/peer/RPC observations, and explicit connectivity re-check actions. Metadata availability is logged only on state transitions.
- Preserve maintenance dependency ordering while publishing metadata availability: when metadata repair cannot establish its required state, that maintenance pass stops before catalogue/GC/repair work rather than continuing against a failed metadata validation.
- Make authenticated metadata-journal replay reproduce the same deterministic same-generation sibling-seed arbitration accepted by the live replica, preventing valid replacement-node convergence state from failing restart with `seed generation conflict`.
- Fence catalogue CONTROL-store GC to the metadata generation used to build its reachability inventory, so a concurrent catalogue commit cannot have a newly-published manifest/shard deleted by a stale maintenance live-set (including with zero garbage grace).
- Protect the data-before-metadata catalogue publication window on every metadata voter: CONTROL GC will not reclaim objects written after that voter observed its current catalogue root, so newly staged manifests/shards survive until the successor root is committed and observed even when `garbage_grace` is zero.

## 0.18.1 — acquisition recovery

- Add an explicit torrent retry action for completed downloads whose linked ingest job failed, reusing the existing persisted ingest job and staging payload without redownloading.
- Keep an operator-paused torrent paused until explicit resume instead of allowing a stale libtorrent status sample to overwrite the Macha job state.

## 0.18.0 — storage correctness reset

0.18.0 defines a fresh on-disk/storage contract and intentionally does not migrate an existing Macha namespace.

- Separate authoritative DATA from priority CONTROL/METADATA storage.
- Rework DATA placement so configured capacity participates in stable deterministic placement and full/offline preferred owners use deterministic fallbacks.
- Define `min_write_replicas` as the synchronous DATA publication floor and `replicas` as the convergence target repaired in the background.
- Add node-local encrypted small-object packing with restart index reconstruction, torn-tail recovery, tombstones and copy-on-write compaction.
- Store catalogue structure as content-addressed shards plus a manifest in CONTROL storage; publish references only after metadata-voter quorum durability.
- Store artwork as ordinary DATA, using the same placement, fallback, replication, repair and GC rules as media extents.
- Add catalogue control-object convergence across current metadata voters and defer scanner work during infrastructure/quorum outages without consuming semantic failure attempts.
- Preserve physical filesystem headroom with per-DATA-backend `reserve_free` admission.
- Retain the metadata memory-amplification corrections: shared immutable record payloads, streaming record hashes, compact/in-place deltas and reduced transient RPC copies.
- Add configurable stale Macha FUSE mount recovery before service startup with `fuse.unmount_if_mounted`.
- Bump the incompatible cluster transport/storage contract to protocol 18.
- Replace migration-era documentation with the current storage, durability and recovery invariants.

Older development history remains in Git history rather than this operational document.

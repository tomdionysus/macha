# Current release

## 0.61.0 — Torrents no longer re-check everything on restart, and paused means paused (development)

**After a restart, torrents sat in `verifying` with no progress and no ETA.**
gbni-1, 2026-09-25: the disk backend was re-hashing a paused torrent's staged
payload at the disk's full 90 MB/s, and every other torrent was queued behind
it for what would have been about an hour.

- **Resume data is kept.** Nothing was saved, so each restart re-added every
  torrent from its magnet and re-hashed every staged byte. Each job's resume
  data is now written when a check or download finishes, on pause, every
  five minutes and at shutdown, to `state/torrent/resume/<job>.resume`, and a
  restart adds from it: the pieces it names are trusted while their files are
  intact. Data for another torrent, or a damaged file, is refused and the
  torrent is checked in full as before. The first start of 0.61.0 has no
  resume data yet and still checks.
- **Paused means paused.** Every pause (operator, `staging_full`, restore at
  start, and the wait for publication after a download) was a plain pause of
  an auto-managed torrent, and libtorrent's queue manager restarts those:
  measured on fi-1, a torrent paused straight after being added was running,
  fully checked and seeding three seconds later. A held torrent is now taken
  out of the queue as well as paused, and paused and blocked jobs are added
  held at start.
- **`verify_queued`**, a new torrent job state: waiting for another torrent's
  check (libtorrent checks one at a time). `verifying` is now only the check
  in progress, with `eta_seconds` estimated from the check's own rate.

## 0.60.0 — Close a session from a page that is going away; leaving transcode frees the slot (development)

**A page reload left its session, and the node's transcode slot, held.**
A browser unloading a page does not complete a preflighted request, and a
cross-origin `DELETE` carrying a bearer is always preflighted, so the close
the web client sent on page exit never arrived (fi-1, 2026-09-25 11:31Z: the
next transcode was refused 429 until the idle rule).

- **`POST /api/v1/playback/sessions/{id}/stream/{token}/close`**, new. The same
  teardown as `DELETE`, authorised by the session's signed stream URL (the
  prefix of `stream.url`) with no `Authorization` header and no body: a CORS
  simple request, for `sendBeacon` or a keepalive `fetch` on page exit. `204`
  when closed or already gone, `404 not_found` for a live session with the
  wrong token, `405` for anything but `POST`.

**A session switched out of transcode kept the slot.** The entitlement was
released only on `DELETE`, expiry or `transcode_entitlement_idle_ms` without
stream activity, so a viewer who changed to direct play held the node's only
transcode slot while watching and refused everyone else (fi-1, 2026-09-25
08:57Z).

- A `PATCH` that leaves transcode now releases the entitlement: video when the
  new plan no longer transcodes video, audio likewise. A sibling session on the
  same logical viewer keeps it, as with the idle release. Switching back
  reacquires it and may be refused `resource_limit`, as any `PATCH` may.

## 0.59.0 — Replica repair is paced, never stopped (development)

**Repair stopped whenever the node was busy, and gbni-1 was always busy.**
From 0.53.0, any loader byte in the 2 s quiet window counted as busy, and a
busy node gave repair nothing: its byte budget was `busy_bandwidth_fraction`
(0.0, the default and the cluster's setting), and its slice ended, cancelling
anything in flight, on any viewer, mount or loader activity. With torrents
and ingest running on gbni-1 almost all the time, it was still roughly 0.9 TB
short of a copy of es-1's data when es-1 left on 2026-09-24. That data went
with es-1: files dated 2026-08-31 and earlier now fail `extent unavailable`
on both nodes.

- **Weighted, like viewer and loader.** While a viewer, the mount or the
  loader is busy, repair runs in bounded turns with a proportional cooldown,
  `repair_weight` time for every `foreground_weight` of theirs:
  `maintenance.foreground_weight: 95` and `maintenance.repair_weight: 5` by
  default, 1..10000 each, zero refused at startup. Idle, it runs within
  `idle_bandwidth_fraction` as before.
- **Its budget no longer drops to zero.** Repair earns credit at
  `idle_bandwidth_fraction` at all times, and the weights decide its share of
  the time. `busy_bandwidth_fraction` now governs rebalance and scrub only.
- **A turn ends between operations.** An extent probe, push or pull already
  in flight completes and is kept. A pull in flight when the node became busy
  was abandoned, or its bytes discarded after they had arrived.
- Maintenance wakes when repair's next turn is due instead of waiting for an
  unrelated event.

Nothing changes on the wire.

## 0.58.3 — Two files of one ingest job never share a destination (development)

**Rome's ingest failed `destination_conflict` on every retry.** The planner
checked only the filesystem for a collision, so two source files of one job
could be planned onto the same path: Rome's two seasons each had an
`Extras/Menu Art.mkv`, both planned `/Movies/Menu Art/Menu Art.mkv`, and the
second failed once the first was in (gbni-1, 2026-09-25, job `b0f01a83`).

- Destinations already given to earlier files of the job now count as taken,
  for media and sidecars.
- A job planned before this is resolved on resume: unfinished files sharing a
  destination get the next free suffix and a fresh partial, logged once each
  as `ingest destination shared within job`.

Nothing changes on the wire.

## 0.58.2 — Torrent jobs carry their info_hash; search results backed by a .torrent can be started (development)

- **`info_hash` is set.** It was declared, serialised and persisted, and never
  assigned, so every torrent job reported `null`. It is now taken from
  libtorrent's status when a torrent is added, and jobs saved without one,
  finished ones included, are backfilled from their magnet at start. Lowercase
  hex: the v1 SHA-1 when the torrent has one, otherwise the v2 SHA-256.
- **A search result backed by a `.torrent` URL can be started.** Resolving an
  `acquisition_ref` can yield a trusted provider's `.torrent` URL, but every
  placement went through the magnet-only path, so such a result always failed
  with `409 placement_failed` / `add_failed`. A resolved search result now
  goes to the path that fetches it, locally or on the node it is placed on
  (an older peer ignores the flag and behaves as before). A magnet sent by a
  client is still never fetched.

**The daytime crashes of 2026-09-24 were 0.58.1's bug.** A core from a SEGV
on gbni-1 at 2026-09-25 03:49:56Z, on 0.58.0 and 16 s after a finished
torrent was removed, has the same stack as the two shutdown aborts:
`publisher()` -> `file_storage::file_path` on a freed torrent. 0.58.1, which
removes that read, has been live on both nodes since 07:26Z.

## 0.58.1 — A torrent's publisher no longer reads a torrent that has gone (development)

**gbni-1 aborted on every service stop and crashed repeatedly on
2026-09-24.** Two core dumps, captured once core files were enabled, show the
same stack: the torrent disk backend's publisher thread calling
`file_storage::file_path` on a torrent already freed, a garbage string
length, an allocation throwing, and nothing on that thread to catch it.

libtorrent hands a disk backend its file list by reference together with the
torrent as an owner, and its own backend keeps that owner. Macha's kept only
the reference, so a publication still queued when the torrent went -- freed
at session shutdown, or removed after a finished download -- read freed
memory.

- The storage holds the torrent owner, as libtorrent's backend does.
- Extents are read back through the path planned when the torrent was added,
  never through libtorrent's file list.
- Publications queued for a removed torrent are dropped, not retried for
  ever.
- An exception in a publication is logged and retried; it can no longer end
  the process.

Tested by removing a torrent with a publication in flight and two queued: the
file list stays alive until the in-flight one finishes, the queued ones are
dropped, and it is then released. The test fails against 0.58.0. Two of the
day's five crashes followed a finished torrent's removal within two seconds;
whether the other three were this too, the next core will say.

## 0.58.0 — The server plays what it is told and chooses nothing (development)

Operator, 2026-09-24: **"The server supplies facts, operations, then does what
it's told."** Playback is by `media_id`: a catalogue item is a title, its
`media_ids` are its files, and choosing the file, the mode, the streams and
the container is the client's decision, made from
`GET /api/v1/playback/media?item_id=`.

**Breaking API change, hence a minor version.** A request that 0.57.0 accepted
is refused here: clients cannot ask for a title, only for a file. It was
first built and deployed as "0.57.1" on 2026-09-24 and renumbered before it
was tagged. **Every client must check what it sends and parses.**

- **`POST /api/v1/playback/sessions` requires `media_id` and refuses
  `item_id`**: `400 media_id_required`, `400 item_id_not_accepted`. `PATCH`
  refuses `item_id` too and switches file only by `media_id`. Until now an
  `item_id` alone made the server rank the item's files (direct over remux
  over transcode, then list order) and play the winner. A file no title
  references stays playable by its `media_id`.
- **An open choice is refused, not filled.** One candidate is a fact and is
  used. Several with no instruction, or an instruction matching none or
  several, is `400` with `status` and `error.code` `choice_required` or
  `choice_not_available`, `error.choice` (`video_stream`, `audio_stream`,
  `subtitle_stream`, `container`) and `error.choices` (the candidates).
  - `preferences.container` is **required for `remux` and `transcode`**; there
    was a silent `fmp4` default.
  - An `audio_language` or `subtitle_language` the media lacks is refused;
    it used to fall back to the default track without saying so. A language
    two streams share must be narrowed to an index.
  - New `preferences.video_stream`; the first video stream was always used.
  - `direct` refuses nothing unnamed: the file is served untouched and the
    player picks its tracks, so `output` then names no selected stream.
- **Session `options.media_ids` and `options.can_switch_media` are gone**, and
  so is `item_id` on the session. A title's files come from the facts route.
- **Facts: copy support is per stream.** `operations.copy_into_fmp4` and
  `copy_into_mpegts` (which described the first video and audio stream only)
  are replaced by `copy_into: {fmp4, mpegts}` on every video and audio stream.
  Each file now gets the whole probe allowance instead of sharing one
  deadline across a title.

**A namespace node below the metadata write floor is `MetadataNotReady`.**
On 2026-09-24 gbni-1 failed an ingest with "namespace node could not reach
the metadata durability floor" while its only peer restarted. It was a bare
`runtime_error`, so 0.57.0's block-and-retry did not apply; it now does.

## 0.57.0 — A torrent is imported once it is published, and a metadata outage no longer kills an ingest (development)

Finishes stage 2 of `TODO/2026-09-23-torrent-disk-backend-plan.md`, and
ships the three fixes that plan promised with stage 1.

**A downloaded torrent is handed to the ingest only once every extent is
published.** On 2026-09-24 Pretty Woman finished downloading with
publication still 30-odd extents behind; the ingest was submitted at once,
found an incomplete extent journal, and copied the whole film without saying
why. The torrent manager now keeps a finished torrent paused in `downloaded`
until the disk backend reports all its extents published, then submits the
ingest, which adopts them. If publication makes no progress for 10 minutes the
manager gives up waiting, logs a `WARN`, and imports anyway; what is missing
is copied.

- Logged: `torrent downloaded; import waits for extent publication ...`, then
  `torrent extents published; importing ...` (or the stall `WARN`).
- The ingest now says why it copies a file whose torrent has a journal:
  `ingest copying path=...: no published extents journalled` or `...: extent
  journal incomplete bytes=N of M`.

**An ingest that meets unwritable metadata blocks and retries instead of
failing.** `MetadataNotReady` (no metadata quorum, a DATA or CONTROL retention
floor not met) is a cluster condition that usually passes in minutes; on
2026-09-23 it killed seven ingests. The job now goes to `blocked` with
`error_code` `metadata_unavailable` and is retried every `blocked_retry`, and
it completes when metadata is writable again (tested with a peer stopped and
restarted). One `WARN ingest blocked ...` when it blocks; retries log at
`DEBUG`. An adoption that fails this way still falls back to copying,
because a missing object and an unreachable peer look the same from here.

**`ensure_control_local` no longer waits for DATA credit.** It took a 4 MiB
speculative DATA credit to check a local control object of a few KB, so under
DATA pressure the wait was abandoned and the commit that needed the object
failed with `CONTROL retention floor unavailable`. The control store is not
on the DATA device, and no other control-store access takes that credit.
When no peer can supply a missing control object it now logs `WARN control
object unavailable id=... peers_asked=N last_failure=...` instead of
returning false silently.

**DATA device pressure onset and release are logged**, once per transition:
`DATA device pressure onset slowdown_percent=...` and `DATA device pressure
released ...`.

**API: clients must check.** No field or code is new, but two states now
last longer or appear where they did not:
- A torrent job stays `downloaded` for as long as its publication takes
  (seconds to minutes) instead of passing straight to `importing`.
- An ingest job, and the torrent job linked to it, can be `blocked` with
  `error_code` `metadata_unavailable` and recover by itself, where before it
  became `failed`.

## 0.56.0 — Every response has a status code; codes are primary (development)

Operator rule, 2026-09-24: **every response carries a snake_case status code,
success included. Normal flow has the code and no message. Errors and warnings
have the code plus an English message; the code is what clients act on, the
message is for people and is never the only signal.** Clients are responsible
for sorting and presentation; the server sends data and codes.

**API changes. Every client must check what it parses.**

1. **`status` on every JSON object response.** Stamped once, centrally, on the
   HTTP worker before compression: `"status":"ok"` on a success that does not
   state its own; a handler's own `status` is kept (e.g. catalogue media-info
   `"pending"`); every `http_error` response has `"status"` equal to its
   `error.code` (e.g. `"not_found"`); any other error body gets `"error"`.
   Streams, 204/304 and non-JSON bodies are unchanged. The key is added at
   the start of the object; no existing key moves.
2. **Error envelope unchanged**, plus the top-level `status`: `error.code`,
   `error.message`, optional `error.reason` and the failure axes. There is
   no `error.detail`.
3. **`placement_failed` (409) now carries `error.reason`**: `node_not_member`,
   `node_refused`, `node_unreachable`, `node_did_not_start`, `missing_uri`,
   `add_failed`, or the peer's own code.
4. **Ingest jobs: `error_code` beside `error`** (null when none). Blocked:
   `source_unavailable`, `source_not_regular`, `source_scan_interrupted`,
   `source_changed_during_scan`, `source_disappeared`, `source_changed`,
   `source_unreadable`, `source_seek_failed`, `source_short_read`. Failed:
   `source_is_symlink`, `no_supported_media`, `destination_parent_not_directory`,
   `partial_not_file`, `destination_conflict`, `namespace_short_write`,
   `size_mismatch`, `metadata_unavailable`, `filesystem_error`,
   `import_failed` (also given to jobs recorded before this release).
5. **Torrent jobs: `error_code` beside `error`**: `restore_failed`,
   `ingest_missing`, `ingest_cancelled`, `torrent_error` (libtorrent's),
   `staging_full` (blocked), `ingest_submit_failed`, `torrent_failed` (jobs
   recorded before this release); a job that failed because its ingest did
   carries **the ingest's own code** (e.g. `no_supported_media`), else
   `ingest_failed`. While importing, a torrent mirrors its ingest's code
   and message.
6. **Catalogue hints: `result` is now a code, not a sentence**: `matched`
   (was empty), `outside_catalogue_roots`, `not_media_file`,
   `no_media_candidate`, `no_provider_match` (the "after N candidates"
   suffix is gone; `candidate_cursor` still says how far it got),
   `already_stored`, `profile_prepared`, `media_not_live`,
   `manual_existing_item`, `manual_metadata`. **`error_code` beside
   `error`**: `path_missing`, `content_not_committed`,
   `provider_budget_exhausted`, `provider_unavailable`, `provider_error`,
   `catalogue_conflict`, `catalogue_unavailable`, `catalogue_error`,
   `artwork_durability_unavailable`, `no_immutable_identity`,
   `yielded_to_playback`, `media_information_error`.
7. **Status diagnostics: `error_code` beside `error`**: `upnp`
   (`igd_not_connected`, `port_mapped_elsewhere`,
   `mapping_verification_failed`, `add_mapping_failed`, `discovery_failed`,
   `support_not_built`); `external_ip` (`lookup_failed`); `startup`
   (`recovery_failed`); node reachability items (`rpc_failed`);
   `/api/v1/catalogue/status` (`converging`, `unavailable`). `check` already
   had its code in `self_probe`.

A cluster-wide torrent add between peers now carries `error_code` too, so a
refusal on the far node reaches the client as the same code.

## 0.55.1 — Extent publication does not depend on piece alerts (development)

**On its first real torrent, stage 2 stopped publishing at 162 of 436 extents
and never resumed** (Trainspotting, gbni-1, 2026-09-24). The download
finished; a stack trace showed the publisher idle on an empty queue: the
remaining extents were never queued, because the verifications of their
pieces never reached the disk backend. The ingest fell back to copying, as
designed, so the film imported correctly -- the old way.

Verification rode on one `piece_finished_alert` per piece, and libtorrent's
alert queue is bounded and drops on overflow. Whether it dropped here cannot
be proven after the fact: `alerts_dropped_alert` went to the alert bridge at
`DEBUG` and the nodes run `torrent.log_level: INFO`.

The torrent's own have-bitfield is now the record and alerts are a hint:
every held piece is reported to the backend when a torrent is checked, when it
finishes (`torrent_finished_alert`), and every 10 s while the manager runs.
A lost alert delays an extent by at most that interval. Reporting a piece
again publishes nothing twice (tested).

`alerts_dropped_alert` is now a `WARN`, so the next occurrence says so, and
the backend logs `torrent extents all published save_path=... extents=N` when
a torrent's last extent is published.

## 0.55.0 — A torrent's extents are published as they verify (development)

Stage 2 of `TODO/2026-09-23-torrent-disk-backend-plan.md`. **The torrent's
disk backend now publishes each file-relative extent of the payload to the
store the moment every piece covering it has verified**, and records it in an
extent journal (`.macha-extents`) in the job's staging directory. When the
download finishes, **the ingest commits each file by naming those extents
instead of copying the file a second time**: one metadata commit per file,
no checkpoints, no copy pass.

- Extents are the node's `extent_size` (4 MiB), file-relative, so they are
  exactly the objects a copy would have produced, and deduplicate the same
  way. An extent may span several pieces, or parts of two.
- Publication is loader-class, durable (`put`, not deferred), and runs on its
  own thread, off the I/O strand, so a slow put never holds up the torrent's
  disk writes. A failed put is retried after 30 s.
- Verification comes from libtorrent's `piece_finished_alert`; after a resume
  or a recheck, which report no per-piece alerts, every piece the torrent
  already has is reported from `torrent_checked_alert`. Extents the journal
  already records are not published again.
- **The journal is a claim, not proof.** The commit's DATA retention barrier
  refuses a manifest naming objects the cluster does not hold; the ingest
  then logs `could not adopt published extents ...; copying instead` and
  copies. Tested both ways.
- Torrents now download **sequentially**, so extents complete in order and are
  read back for publication while still in page cache.
- Deleting a torrent's files deletes its journal.

The payload file is still the assembly area for its extents; the one function
that reads an extent back (`read_extent`) is where a staging format of
macha's own would replace it.

Watch for: `ingest adopted published extents` against `could not adopt ...
copying instead`. If publication falls behind a fast download, files still
incomplete when it finishes are copied as before.

## 0.54.1 — Artwork is downloaded once, not once a day (development)

**Every artwork URL changed at UTC midnight, and every browser downloaded every
poster again the next day.** The signed artwork URL's expiry is rounded to a
bucket of `catalogue.api.artwork_capability_ttl_ms`, which is what keeps it
byte-identical and therefore cacheable, and the response's `max-age` equals
that TTL. At the 24-hour default both rolled over daily. The default is now
**30 days**. No node set the key, so every node takes the new default.

**Artwork responses now carry an `ETag`**, the artwork's id (it is a content
hash), and a request whose `If-None-Match` matches is answered `304` with no
body **before the artwork is read at all**. So a browser revalidating a poster
it holds never pays a cold read for bytes it already has. Before this,
artwork had no validator, so an expired cache entry was a full re-download.

**`Timing-Allow-Origin: *`** on artwork, so a cross-origin client can measure
how long a poster took instead of reading zeros.

Reported by the web client session (2026-09-24): a first read of a 77 KB
poster from gbni-1 took 1.1 s at about 1 Mbit/s, and posters seemed to go
slow at random -- each one expiring on its own 24-hour clock. This release
removes the daily expiry and the re-downloads. **It does not change how long
a cold read takes.** That read waits behind loader I/O on a busy DATA disk
(the loader-I/O item in `TODO/ACTIVE.md`), which is still open.

## 0.54.0 — The torrent's disk I/O is macha's disk I/O (development)

**libtorrent now does its file I/O through a disk backend of macha's own, and
every read, write and hash it performs is admitted by the DATA arbiter at
loader class and timed into the disk service monitor.** Until now libtorrent
used its default backend: ten threads of plain file I/O that neither admission
nor the monitor ever saw. On 2026-09-23 that pool wrote 65 MB/s of torrent
payload onto es-1's DATA spindle (91% busy, ~190 ms per write). The monitor
measured macha's own extent writes taking 30-100x their expected time,
declared the device pressured, and the arbiter throttled publication to one
slot -- which gbni-1 and es-1 then each held across a put to the other.
Seven ingests died with `CONTROL retention floor unavailable before metadata
publication`. The diagnosis is in
`TODO/2026-09-23-torrent-writes-starve-publication-incident.md`.

The backend (`src/torrent_disk_io.cpp`) implements libtorrent's
`disk_interface` on the public API only, for 2.0 (the nodes) and 2.1:

- **Admitted as a loader.** An acquisition is durable work someone asked for,
  so it yields to a slow device only when a viewer would otherwise wait
  (law 3), like the ingest that follows it.
- **Measured on the right device.** The torrent's I/O is fed to the DATA
  device's monitor only when staging shares that device, which it does on
  every node today. The monitor's verdict now describes the load that is
  actually on the disk.
- **Backpressure, not a clamp.** When admission holds writes back, the
  queued-write limit (`max_queued_disk_bytes`) tells libtorrent to stop
  reading from peers until the disk drains: the network rate follows what the
  disk admits.
- **Bounded.** `torrent.disk_threads` (default 2) workers, and at most 64 open
  descriptors per torrent, where a discography is thousands of files.
- **Ordered.** One torrent's jobs run strictly in the order libtorrent issued
  them, because libtorrent may ask for a piece's hash before the writes of its
  blocks have completed.

Payload is still written in the torrent's own file layout under the save path,
so the ingest that runs after a download is unchanged. Assembling extents in
staging and publishing each one as it is verified is the next stage of
`TODO/2026-09-23-torrent-disk-backend-plan.md`.

**Removed: `torrent.pressure_download_rate`**, the rate clamp that flipped on
and off with a two-second viewer window. es-1 logged 287 clamps and 287
restores in one hour on 2026-09-23, and the torrent wrote 65 MB/s throughout.
A node that still sets the key is unaffected; the parser ignores it.

**Added: `torrent.disk_threads`** (default 2, at least 1).

Tests: a new `macha-tests-torrent` binary drives the backend through
`disk_interface` as a session does, including a loopback swarm that
downloads a hybrid v1/v2 multi-file torrent three times over and checks the
payload is byte-identical, every admission was loader class, every credit
came back, and delete left no payload and no open descriptor. 200/200 on
es-1. It links libtorrent, which `macha_core` and `macha-tests` never do.

## 0.53.2 — The torrent alert stream has its own log level (development)

**`torrent.log_level`.** With the process at `DEBUG`, libtorrent's DHT and
tracker alerts were 99,088 of the 99,187 lines in gbni-1's journal -- 99.9% --
and had evicted every line of the previous night's diagnostic record within
nine hours. The alert bridge now has its own threshold, independent of
`log_level` in the same way `ffmpeg_log_level` is: `INFO` (the default) keeps
the explicit listen, DHT-bootstrap, port-mapping and warning lines and drops
the per-alert chatter; `DEBUG` bridges every alert the session subscribes to;
`ALL` additionally subscribes libtorrent's internal tracker, peer, session,
torrent and DHT log categories. Like the ffmpeg bridge it emits past the
process filter, so a node at `INFO` can still turn it on. Applies live on
reconfigure.

## 0.53.1 — A commit publishes what changed, not what exists (development)

**Every metadata commit re-uploaded its entire control graph to every peer, on
every commit, whether or not the peer already held it.** `retain_control`
collected the commit's referenced control objects and `put_graph_on` sent all
of them, with no presence check anywhere on the path. That was tolerable while
a graph was 65 catalogue shards. Once the namespace became tree-backed a commit
referenced its whole spine, and the cost stopped scaling with what changed and
started scaling with how large the library had grown.

Measured on gbni-1 on 2026-09-22: **three minutes of importing produced 25
commits, pushed 5,469 control objects to peers, and grew the control store by
zero objects.** Over two hours it pushed 8,924 against a store holding 4,162.
All of it was already on both ends.

It is the same defect 0.51.0 fixed on the replication path — "a node already
present is not re-replicated" — which was never applied here.

A commit now asks each peer which of the referenced objects it is missing and
sends only those, over a new CONTROL-plane `have_control_objects` message.
`have_objects` could not answer this: it reads `local_store()`, so it can say
nothing about control objects. The new handler reads `control_store()` and
takes no DATA admission at all, because law 1 does not let a control index
lookup wait on the DATA arbiter. A peer too old to know the message answers
with an error and is sent the whole graph, exactly as before.

The bytes are also read lazily now. The old form materialised every referenced
object out of the control store up front, so a commit decrypted its whole spine
to discover the peer wanted none of it.

**The symptom this was found through:** `ingest failed: CONTROL retention floor
unavailable before metadata publication`, on gbni-1 and then es-1, killing
ingest jobs outright. 840 concurrent puts overran the connection's outbound
queue, every candidate peer was skipped, and the node ended with `retained=1`
against `required=2`. The publication is now bounded to a quarter of the
smaller of the connection's two budgets as well, so a large graph cannot
exhaust a resource it shares with heartbeats, metadata commits and status
traffic.

**And three `catch` blocks on the RPC path were discarding the reason a call
failed**, so a self-inflicted concurrency limit reported itself as
`no canonical RPC route to peer` — a dead network link. `peer outbound queue
full` had never once been logged on any node in the cluster. The reason now
travels with the error.

## 0.53.0 — The disk resource manager, audited (development)

The operator called a gate on this mechanism after it had been wrong twice in
one afternoon, and the audit found it wrong in five more places. Everything
here comes out of that audit, worst first. Nothing in it is verified on the
cluster yet.

**Every DATA read was measured with a size of zero, so the flat threshold this
model exists to replace was still live on the read path.**
`StoragePool::get()` starts its timer before the read, because a read's size
is only known once it has succeeded, and the `DiskServiceTimer::note_bytes()`
call that exists for exactly that purpose was never written — anywhere in the
tree. Every read was therefore judged against the fixed 25 ms per-operation
overhead alone, with no per-MiB allowance at all: precisely the 0.51.0 bug
that 0.52.0 was supposed to have removed, surviving inside the fix for it.

Measured on es-1 while auditing: `sdb` serving 53.7 reads/s at 240 KB average
and 30.9 ms average service — a healthy spinner — and the node had entered and
left pressure **twelve times in the thirty-four minutes** since it started
0.52.0, clamping an operator's torrent download each time, with nothing being
watched anywhere on the cluster. `test_a_read_is_measured_with_the_bytes_it_returned`
drives real objects through a real pool and fails without the call.

**The torrent rate clamp never had law 3's second clause.**
`TorrentManager::follow_device_pressure()` clamped the download rate whenever
the device was pressured. An acquisition is durable work the user asked for —
loader class — so it yields to a slow device only when a viewer would
otherwise wait for it. The arbiter was corrected for this in 0.52.0 and this
path was not, which is what produced those twelve log lines.

**Maintenance is now part of the service time it spends.** Pool rebalance,
scrub and GC read their backends directly rather than through
`StoragePool::get()`, where the timer lived, so the 51.6 MB/s of `macha-maint`
reads that took `sdb` to 91% utilisation on 2026-09-22 never reached the
signal that decides the device is busy. The mechanism was blind to the largest
consumer of the resource in both directions: it could not bound it and could
not see it.

**A loader is visible to maintenance as its own class.** The maintenance busy
decision was `playback_busy || interactive_busy`, fed by clocks that only
`foreground` and `read_ahead` writes touch. An ingest is loader-class and fed
neither, so a node importing 36 GB reported itself idle and handed maintenance
its idle share of the spindle the import was waiting on — with
`busy_bandwidth_fraction: 0.0` already set and unable to help. There is now a
loader activity clock beside the two viewer ones, fed from the loader read and
write paths, and consulted by the busy predicate, all four slice-yield
predicates and the busy-pass wake-up. It is deliberately **not** folded into
the viewer clocks: viewer reserves, the pressure gate and the torrent clamp all
key off those, and conflating them would make an import look like a viewer.

**A viewer between two extents is still a viewer.** "Viewer present" in the
arbiter meant byte credit held at that instant, and playback does not hold
credit between extents — so every gap in a stream readmitted the loader at full
concurrency, and the viewer's next read queued behind the extent write the gap
had just let in. The arbiter now also consults the activity clock over
`maintenance.foreground_quiet`, which is the window the rest of the system
already uses for this question.

**The last number that was a guess about hardware is gone.**
`io_pressure_outlier_ms` (an absolute 2 s) becomes
`io_pressure_outlier_percent` (1000). The absolute figure was unequal as well
as invented: 2 s is 396% of expectation for a 4 MiB write and 8,000% of it for
a 4 KiB read, so the same number meant "mildly slow" for one operation and
"catastrophic" for another. The founding 17.7 s extent write scored 3,505%
against its own expectation and still trips on its first sample, which is the
whole reason a single-sample trip exists beside the moving average.

**An operator can now see what the throttle cost.** `pressure_refusals` joins
`device_pressure_onsets` on `/api/v1/status`. The counter had existed as a
private member since the gate shipped, incremented nowhere and reported
nowhere — which is why "is this node being throttled or is it unwell" could
not be answered during the 2026-09-22 incident. `DataWorkContext::records_activity()`
was dead in the same way and is removed.

**What the audit confirmed rather than assumed**, and what it left open, is
recorded in `TODO/ACTIVE.md`: law 2 has no admission path to a delayed viewer
read; law 1 holds on the hardware (control on NVMe, DATA on the spinner, on all
three nodes) but by configuration rather than by construction; law 4 does not
wedge, though the 150–300% hysteresis band is a latch. Two things stay open —
one monitor covers a whole `StoragePool` rather than one device, and local disk
maintenance is still budgeted from a network bandwidth measurement.

## 0.52.0 — The catalogue comes back, and disk pressure means something (development)

Everything here fixes something 0.51.0 broke or got wrong on the live cluster
within the hour. Recorded in the order it hurt.

**Every catalogue route was returning 503 on every node, and all clients
reported "no API".** `catalogue.cpp` committed a resolved catalogue conflict
through the non-exact `mutate()` path, and the tree-backed guard refuses that:
a tree has no entry map to diff, so a commit must declare its change set. It
was the last caller still on that path. Health and authentication answered
normally throughout, which is why the app loaded and had nothing in it. It now
commits through `mutate_delta`, carrying its catalogue root, its garbage
upserts and the standing conflict set explicitly.

**Disk pressure was measured against a number that had been invented rather
than derived.** `io_pressure_target_ms: 50` compared the total time of every
operation against one threshold, so a 4 MiB extent write — 100–200 ms on a
healthy spinning disk — read as pressure. A node declared itself in trouble
nine seconds after boot and stayed there, which held an operator's 36 GB import
to one background lease and about 2 MB/s for an afternoon while nothing was
being watched.

Pressure is now the moving average of **actual against expected for that
operation's size**: `io_pressure_overhead_ms` (25) plus
`io_pressure_per_mib_ms` (120), with pressure above
`io_pressure_slowdown_percent` (300) and release below
`io_pressure_release_percent` (150). A 4 MiB write at 200 ms reads as 40% of
expectation. Setting `io_pressure_slowdown_percent: 0` disables the mechanism.

**A single catastrophic operation trips pressure on its own**
(`io_pressure_outlier_ms`, 2 s). The tests caught this before it shipped: the
17.7 s extent write that prompted this whole line of work moves the ratio from
19% to 237% against fifty healthy samples — *under* the 300% line. The ratio
catches sustained degradation and would have let the founding case through.

**Law 3 is enforced instead of flattened.** "Thou Shalt Not Make The
Ingester/Loader Wait, Unless It Would Make The Viewer Wait." 0.51.0 made the
loader yield whenever the device was slow, with no viewer anywhere. Now the
loader yields only when a viewer is present — waiting for credit or holding it
— and gets its concurrency back the moment the viewer leaves. Speculative work
still yields on pressure alone: it sits below the loader and nobody is waiting
for it. A viewer is never gated by pressure at all.

`device_slowdown_percent` joins the per-node data-resource block on
`GET /api/v1/status`, beside the service time and worst case, because a mean
latency cannot be judged without knowing the size of the operations behind it.


## 0.51.0 — The namespace tree is protected from the collector, and a slow disk stops being invisible (development)

**The namespace tree was collectable.** Found on the live cluster six hours
after the cutover: the control-store live set was built from catalogue roots
alone, and nothing walked `namespace_root`. Every tree node holding the
namespace — the nodes that say where every file lives — was, to reachability
GC, an unreferenced control object waiting out its grace, on all three nodes.
It had not bitten only because `garbage_grace_ms` had been raised to 30 days
that afternoon as a migration safety net. That accident was the whole margin.

The retention-release live set now carries every branch, leaf and extent-spine
node reachable from the root, beside the catalogue roots. An unreadable node
marks the whole set incomplete and nothing is released against it, because a
partial live set is exactly what lets the collector delete the namespace. Per
commit, the nodes a new root introduces acquire retention claims as changed
catalogue shards do, found by a parallel walk that never reads a subtree both
roots share and over-collects only in the safe direction.

**A commit replicates the nodes it wrote, not the nodes it touched.** An ingest
was crawling at 1.8 MB/s on a node at 0.5 load and 2% iowait — 2.6 hours for a
36 GB import with nothing saturated. A commit re-chunks the spine, so it put
about a dozen nodes of which all but the changed leaf and its path were
byte-identical to what was already stored, and each was replicated
synchronously to peers 60 ms away to be told they had it. A node already in
the local store is immutable and was replicated when first written; `put` now
checks presence first, and per-commit round trips fall from about twelve to
the changed leaf and its path.

**The replica's "not reconstructible" diagnosis was a default conclusion, not
a measurement.** `diagnose_unreconstructable_locked` checks the chain links and
that each frame is readable, and if both pass it reports "delta replay over N
frames does not reproduce the record hash" without ever replaying. That sent
an operator chasing a tree corruption that does not exist while the real event
was a branch waiting sixteen minutes on reconciliation. The message now says
what was checked. `macha-metadata-dump --objects` gains a real replay that
reconstructs every frame and names the first that diverges; on the live
cluster it reproduced all 25 byte-exactly.

**A torrent can be started on a named node.** `POST /api/v1/torrents/jobs`
takes an optional `node_id`; absent means the receiving node, as before. The
response always carries `node_id`, including for a local add. An unknown or
unreachable target is a 409 with the reason, never a quiet local download.

### A slow disk stops being invisible


**Nothing measured disk service time.** Every bound on DATA work was declared
up front — bytes in flight, concurrent operations, a maintenance bandwidth
fraction — and none was derived from the device. That is how one ordinary
ingest took a node to 91% iowait with single 4 MiB extent writes at **17.7 s**
and twelve aborted client requests, while every byte budget was satisfied. The
bookkeeping was right and the disk was gone.

**`DiskServiceMonitor`** records completion latency of DATA store operations as
an EWMA with hysteresis, fed from `StoragePool`'s `put`, `put_deferred` and
`get`. Two clock reads and a few relaxed atomics per operation, no locks and no
timer thread. It sits on the pool rather than in `LocalStore` because the
contended thing is the physical DATA backends: the control store is a different
device and must not be gated by their pressure, which is exactly what the
2026-09-20 measurements turned on — 3.3 ms on NVMe against 7.8 s on sdb1, same
lane, same workers, same moment.

**DATA admission consults it.** While a device is pressured, loader and
speculative admission for it is held to `io_pressure_min_background` leases.
**A viewer is never gated by pressure**: if the disk is slow, the person waiting
on it gets all of it. Law 3 is kept — bounded, never stopped — so a loader that
is itself the reason the disk is busy drains at a trickle instead of
deadlocking on its own publication.

**And a torrent download yields the spindle too.** This is the half that would
have made the rest prove nothing: libtorrent writes straight to its save path
and never enters `StoragePool` or the arbiter, so gating macha's own writes does
nothing about a download saturating the same disk. Measured on gbni-1 on
2026-09-22: **17 MB/s onto a 9.1 TB disk at 90% utilisation, load 13 on four
cores, with no import running at all.** The session's download rate is now
clamped to `torrent.pressure_download_rate` while the device is defending
itself and restored when it recovers.

Why the signal is indirect and why that is right: the monitor sees only
operations that pass through the pool, so an external writer raises it once
macha's own reads and writes start taking longer as a result. A disk nobody is
reading can be as busy as it likes and starve nothing. Pressure is only
meaningful when there is work to protect, and when there is, that work is
itself the probe.

**Visible, or it may as well not exist**: `device_pressured`,
`device_service_us`, `device_worst_us` and `device_pressure_onsets` join the
per-node data-resource block on `GET /api/v1/status`. The worst case is there
because a healthy-looking mean hides the single 17-second write that breaks a
viewer.

New knobs, both with an off switch that restores the previous behaviour exactly:
`dht.io_pressure_target_ms` (50), `dht.io_pressure_release_ms` (20),
`dht.io_pressure_min_background` (1) and `torrent.pressure_download_rate` (2M).

**Not yet validated against a real saturated device.** The plan's acceptance
wants a DATA-backed route holding under 250 ms p99 through a real ingest, and
the four previous reproduction attempts failed because the synthetic load was
the wrong shape. A live torrent download is the right shape, and that test is
next rather than claimed.


## 0.50.1 — What the cutover found in its first two minutes (development)

**The live cluster was re-rooted onto the tree at 12:41Z on 2026-09-22**, all
three nodes, generation 35503 becoming 35504: a 22.79 MiB record becoming
3.97 KiB over 5,221 entries and 473,923 extents. All three computed the
identical record independently — same head, same tree root, same record hash,
on three machines in three countries with no coordination between them. Both
defects below were found by watching the result rather than by reasoning about
it, and both are fixed here.

**A namespace change was not visible as a change.** gbni-1 came back on the
migrated head, adopted the namespace once, and then never saw another: a
directory created on es-1 was in gbni-1's metadata, at the same root, and
absent from its mount. Both witnesses that answer "has the namespace changed"
compared entry maps — `MetadataManager`'s cache witness and
`metadata_namespace_signature` — and under SM14 both maps are empty, so every
change reported as no change. A mount that stops seeing remote writes, and a
catalogue that stops discovering them, silently and permanently. Both now
compare the root, which is what the design was for: the namespace's identity is
already a hash over exactly its content, and comparing two 32-byte roots is
cheaper than the map comparison it replaces.

**Every delta was failing to reconstruct.** Three `local metadata delta
rejected; retrying full record` warnings in the first minute, one per commit.
The replica keeps a delta body only if replaying it reproduces the record byte
for byte, and that replay re-encodes the successor through the delta-versioned
encoder, which reached for the SM13 encoder — which refuses a namespace root.
The fallback to a full record is correct and did its job; it is also not the
point of having deltas. Tree-backed successors now encode as SM14.

Worth noting what these two have in common: neither was a crash, neither
produced a wrong answer to anything that was asked, and neither would have
appeared in a test suite that did not restart a second node and then look at
its mount. The first one in particular was invisible from the node doing the
writing, because that node's FUSE frontend already had the change locally.


## 0.50.0 — The namespace can be re-rooted onto the tree (development)

**`macha-namespace-migrate` re-roots one stopped node's namespace onto the
content-addressed Merkle tree, and a node that has been migrated serves and
writes normally.** Measured against es-1's real head on a copy of its state:
**5,201 entries and 468,850 extents, a 22.56 MiB record becoming 3.97 KiB**,
and the whole metadata history collapsing from 23.7 MB to a single 4,180-byte
frame. A stat of `/` through the migrated tree takes **0.069 ms** against the
**56 ms** a namespace write costs today.

Nothing is migrated by installing this. The tool is offline, deliberate, and
run per node with the cluster stopped; until it is run, every node behaves
exactly as it did.

**Every namespace reader and writer now works in either form.** The FUSE
frontend, catalogue scanner, media index, retention claims, the
reachability/GC live set, and all nine filesystem mutations read and write
through one pair of primitives. Each call site states whether it needs stat
data or whole entries, which matters: `file_media_id` hashes the extent list,
so a stat-only read there produces a different id that looks exactly as valid
as the right one.

**Mutations go through a working set rather than the entry map.** A batch has
to see its own earlier edits -- `mkdir /a` then `create /a/b` -- which a map
gives for free and a tree cannot. The overlay is the delta the mutation was
already recording, with `erase_entries` as the tombstone a map cannot express.

**A prefix scan descends.** A directory listing and the catalogue root scan
read the subtree rather than the library, because the tree is keyed by path and
a branch child that cannot contain the prefix is never fetched.

**History replay can rebuild a tree-backed head without asking a peer.** Replay
writes nodes locally and replicates nothing: the commit being materialised
already reached the write floor when it was made, and requiring that again
would make rebuilding a local head depend on peers being reachable.

### Two failures found before shipping, both silent by nature

**Reconciliation would have merged two trees into an empty namespace.** The
three-way merge is path-wise over three entry maps, and a tree-backed snapshot
has an empty one. Merging two empty maps does not fail -- it succeeds, reports
no conflicts, and produces an empty namespace. A reconciliation that deletes
the library and looks like agreement, on a cluster that reconciles around a
hundred times a day. The merge now refuses a tree-backed branch; the manager
materialises all three branches and re-roots the result. A merge therefore
costs what it costs today and is the one operation this work has not made
cheaper. The two manual split-brain repair planners had the same shape and
refuse too.

**`macha-metadata-dump` crashed on a migrated head**, found by rehearsing the
migration against a copy of es-1's real state rather than by reasoning about
it. It now reports a tree-backed record, and with `--objects <path>` it walks
the real tree and times a stat against it -- so the forensics tool still works
on the node an operator is most likely to be worried about.

### What a cutover requires

Every node stopped and converged on one head, the tool run on each, and the
witnesses named with `--witness`: the acceptance format will not encode a write
floor it cannot name replicas for, so the certificate carries the operator's
assertion that these nodes are being re-rooted onto this record. `--expect-hash`
is how the second node proves it computed the same record as the first, and
because the record is a pure function of the converged head, it always will
have. The previous checkpoint, journal, history, heads and acceptance proof are
quarantined under `.pre-migration.<ns>` rather than deleted.


## 0.49.1 — The namespace read and written through one door (development)

**Nothing in this release is reachable in production, and that is deliberate.**
0.49.0 added the SM14 record shape; this adds the machinery that a record with
a namespace root needs before one may exist. No snapshot carries a root, so
every path below falls through to the entry map exactly as it did.

**A commit can update the tree instead of re-serialising the library.** The
change set a mutation already carries is applied to the tree -- erase, upsert,
append, in the order `apply_metadata_delta_in_place` uses so the two cannot
disagree. Measured on a 2,520-entry library, one ordinary write rewrites **3
nodes of 102**: the leaf holding the key and the two branches above it.

The property this rests on is asserted rather than argued: the root an
incremental update produces is byte-identical to the root a full rebuild
produces. Sixty rounds of random change sets -- deletes that empty leaves,
value changes in place, inserts landing on boundary keys, some with enough
extents to force an external spine -- each compared against a build from
scratch. Two nodes that reach one namespace by different routes must agree on
its root, or the comparison that replaces `metadata_namespace_signature`
reports divergence that does not exist.

**History replay can rebuild a tree-backed head without asking a peer.** A
record whose history cannot be replayed is the 2026-09-06 outage shape --
durably written, hash-verified, unreadable the moment it leaves the
materialisation cache -- so this was the prerequisite for any SM14 record
existing at all. Replay writes nodes locally and replicates nothing: the
commit being materialised already reached the write floor when it was made,
and re-establishing that here would make rebuilding a local head depend on
peers being reachable.

**The readers that decide what garbage collection may delete read either
form.** `Service`'s retention claims and release set, and
`FileSystem::maintenance_objects_cached`. For these an empty namespace does
not mean "no files", it means "nothing is live" -- the input destructive GC
wants before it deletes. Every `MetadataSnapshotView` the manager hands out is
now gated, so a namespace that lives in a tree cannot reach a reader that has
not been converted; it raises rather than reading as empty.

**Two costs on the FUSE hot path are now stated rather than incidental.**
Existence checks go through a primitive that copies nothing, because path
resolution asking `namespace_entry` would copy a film's 12,500-element extent
list to answer a question about a key. And a stat-only read strips extents in
the map form too, rather than returning a copy of them because this snapshot
happens to be a map.

**One regression was introduced and caught here, and it is worth reading.**
Making `readdir` stat-only broke the manage API, which computes
`file_media_id()` over what `readdir` returns -- and that hashes the extent
list. A stat-only listing does not fail there: it hashes an empty list and
produces a different id that looks exactly as valid as the right one, then
matches no catalogue binding. That is this work's own failure shape from the
other direction -- not "an empty namespace read as no files" but "an empty
extent list read as a file with no content" -- and it is why trimming a read
to stat data needs an audit of what callers do with the entry rather than what
the FUSE path does with it.


## 0.49.0 — A record that points at the namespace (development)

**The metadata record can now be a pointer to the namespace rather than the
namespace itself.** SM14 carries the non-entry fields plus a 32-byte
`namespace_root` addressing a content-addressed Merkle tree, in place of the
inlined entry map. On the test fixture the payload is **590 bytes against
434,731**, and it is still 590 for a library twenty times larger, differing in
the 32 bytes it points with. Against es-1's real head the comparison is 590
against 22,525,100.

That number is the whole point of the work. Today the record *is* the
serialised namespace and its identity *is* a SHA-256 over those bytes, so
every commit re-serialises and re-hashes the entire library: 56 ms of CPU per
namespace write on 1.618 TiB, and about 3.1 s at the 100 TB the system claims
to be for. A commit's cost has to stop being a function of how much media the
cluster holds, and this is the format in which it does.

**Nothing authors one, on purpose.** `encode_snapshot` never emits SM14, no
commit path constructs or reads it, and a node that does not know the magic
refuses the payload as `bad snapshot`. This release ships the format and the
tree it addresses as dead code, reachable only from tests. Making it
authoritative is the next stage, and the migration that cuts a live cluster
over to it is the one after that.

**A snapshot carries entries or a root and never both.** Both encoders refuse
the mixed state rather than resolving it: `encode_snapshot` will not drop a
root it has no field for, and `encode_snapshot_v14` will not drop entries it
would leave behind. The failure being designed out is publishing an empty
namespace under a valid-looking hash — durable, silent, and indistinguishable
from a library that was deleted.

**The "missing root" check moved to the reader that holds a node store.**
SM13's decoder proves the namespace is a filesystem by finding `/` in the map.
SM14 has no map, and acquiring a store inside `decode_snapshot` to run a sanity
check would reintroduce exactly the materialisation this removes, so
`attach_namespace` makes the check instead.

The acceptance is a byte-exact round trip. A snapshot with garbage, node
status, identity resets, merge parents, the write floor, the participant
roster, the branch floor and the retention baseline all populated — detached,
encoded as SM14, decoded, reattached — re-encodes to the original SM13 payload
byte for byte, which covers every field without enumerating them. Separately,
and proved by counting store reads rather than asserted: a `getattr` answered
from a decoded SM14 record reads at most `depth` nodes, fetches no extent
node, and never materialises the namespace.

Every section SM8–SM13 writes conditionally is unconditional in SM14. Those
conditions exist to reproduce an older encoder's exact bytes for journal
replay, and SM14 has no older self to be byte-compatible with. The legacy
`metadata_voters` list survives the re-rooting deliberately: retiring a dead
field and moving the namespace out of the record are two decisions, and only
one of them belongs to this work.

The SM14 garbage reserve is bounded by `Reader::remaining()` rather than by
the count in the payload — the same defect the node decoder carried and which
reading it, not fuzzing it, found.


## 0.48.2 — Things a node can say about itself (development)

**Timestamps in the journal are UTC and say so.** Every line now reads
`2026-09-21 18:45:44Z` instead of node-local time with no offset. This cluster
spans timezones by design — es-1 on CEST, fi-1 on EEST, gbni-1 on BST — so
every cross-node correlation is a subtraction between two journals, done by
hand, usually during an incident. On 2026-09-21 that cost an hour: `18:45:44`
on es-1 and `19:45:44` on a client's screen were the same instant, and only a
recognisable event sequence made it cheap to spot. The operator's ruling the
same day: *"Macha absolutely needs to handle multiple timezones across sites.
They WILL be in different timezones. We should be using hard Zulu, UTC."* The
wire was already unambiguous — everything on it is `*_unix_ms` — so this is
the half people read catching up.

**The block cache reports what it has done, not only how full it is.** `hits`,
`misses` and `evictions` join `used` and `capacity` on the per-node `cache`
block of `GET /api/v1/status`.

Until now `blocks()` was the entire observable surface of that cache and it
counts writes alone, so a cache that had never returned a single byte reported
identically to one working perfectly — on telemetry, on the status API and in
the logs alike. Answering "is it serving anything?" took an hour of manual
measurement against a live node. It matters most where there is least to see:
on a node with `hosts_extents: false` the block cache is the only reason it can
serve media at all, and a dead one there is the difference between an edge node
and a proxy with extra steps.

A read that throws counts as a miss, deliberately: the caller got nothing and
goes to the network either way, and counting it as neither would make a cache
failing every read look idle rather than broken.

The counters are read from the live telemetry sample only, never the persisted
fallback that capacity and usage fall back to. They are monotonic since the
sending process started, so republishing a durable value after a restart would
be a count from a process that no longer exists, and a consumer diffing two
reads would watch them go backwards. Absent is the honest answer when there is
no fresh sample. They are also deliberately **not** summed into the cluster
rollup: an aggregate hit rate lets two healthy storage nodes drown a
storage-less edge node sitting at zero, and that node is the one the number
exists to expose.

**`GET /api/v1/status` names the node that answered it.** `node_id` on the
root, matching the `id` already in `nodes[]`. A client configured with one
address polls this and gets a cluster snapshot in which nothing says which of
those nodes produced the response, so a machine reached by two addresses is
counted as two nodes — live on this cluster, `http://10.44.1.50:7438` and
`https://macnessa.macha.network` are both gbni-1. Anything grouping by node
double-counts it, a failover can "move" to the machine it just left, and a node
selector offers the same box twice. `api_endpoint` cannot serve the purpose,
because it is the node's own advertised name, which by definition differs from
the address the client used in exactly the case that matters.


## 0.48.1 — What testing 0.48.0 found (development)

Every item here is a defect in 0.48.0 that its own testing exposed, found in
one afternoon of four client sessions exercising the new routes against the
live cluster. 0.48.0 itself is unchanged and stays as released.


**A transcode entitlement is released after five minutes with no stream
activity, instead of being held until the session is erased.** This one
predates 0.48.0 and was merely found by testing it: the entitlement outlived
its own pipeline by `session_idle_ms` — thirty minutes against sixty
seconds — so on a node where `max_video_transcodes` is 1, one client that
crashed, was force-stopped or was reaped in the background closed that node to
transcoding for everybody for half an hour. Measured on fi-1 on 2026-09-21: 57
session creates, zero deletes, and three separate client sessions refused a
transcode by a node nobody was competing for.

The new `streaming.transcode_entitlement_idle_ms` is clamped into
`[pipeline_idle, session_idle]` rather than merely read. Below the floor it
would fire the instant the engine went; at or above the ceiling the session
outlives it and it never fires at all.

**It is keyed on stream activity, not on control traffic, and that is the
contract clients need.** Polling a session keeps the session alive and is
deliberately not evidence that anyone still wants media, which is the question
the entitlement answers. So: **to hold a transcode slot across a pause, ask for
a stream object inside the window — fetching the playlist is enough and costs
no media bytes.** A viewer paused for longer loses the entitlement and
reacquires it on resume, where it may be refused. The session itself is
untouched: id, position, plan and capability all survive to `session_idle_ms`.
A long pause risks the slot, not the place.

The trade is deliberate and was taken on the operator's decision: a possible
refusal after a long pause, instead of a certain half-hour outage after any
unclean exit. A refusal at resume is visible, attributable and recoverable;
that outage was none of those.

**`resource_limit` carries failure axes at last, and they differ by path.** It
carried none at all, which left the one refusal a client can act on as the one
saying least — the account cap beside it states scope, health and its own
limit. On **create** it is now `scope: node`: no session exists yet, so trying
another node costs nothing and is right. On **update** it is `scope: request`,
because the session already exists here and is still serving its current
generation — walking would mean abandoning something that works to rebuild it
elsewhere, and a client cannot take a session with it. Both carry
`node_healthy: true` and `alternative_may_succeed: true`; on the update path
the alternative is a different instruction against this same node, a remux
instead of a transcode or a lower height.

**Two more fields reach the per-node `playback` block of
`GET /api/v1/status`.** `transcode_entitlement_idle_ms`, so a client can time
its keep-alive against the node it is actually on rather than a hardcoded
guess. And `max_sessions`, the node-wide cap — 0.48.0 published the per-account
half and not this one, which left a client able to say "another screen on this
account is playing" and unable to say "this node is full". Both are additive
under TEL3 and an older node simply omits them.

**The compiled default for `max_sessions` moves 8 to 64**, so it stops
contradicting `max_sessions_per_account` at 32. The two caps are enforced four
lines apart in `reserve_session_slot`, node-wide first, so a per-account cap
above the node-wide one can never fire and `account_session_limit` was dead
code on any node running bare defaults. Three separate client sessions found
that independently on the day 0.48.0 shipped. These are values an operator
configures; the defaults now match `macha.yaml.example` rather than
contradicting it.

**Keep `max_sessions` above `max_sessions_per_account` on every node.**
Otherwise the node-wide limit refuses first and the distinction between the two
429s — which mean opposite things to a client — is lost in the one case it
exists for.


## 0.48.0 — A playback session is a resource (development)

**This release breaks the client contract on purpose, and there is no
dual-serve window.** Every node is under our control, there is no fallback to
an older version, and the cutover is all three nodes at once. A client that has
not taken the matching release will not work against a node running this one.

**A playback session stopped being a property of the bearer token.** A `POST`
to `/api/v1/playback/sessions` creates a member every time, and returns `201`
with a `Location`. Two `POST`s on one token are now two live sessions with
different ids, both streaming. Before, the server keyed a session on the
authenticated API session and a second `POST` silently superseded whatever that
token was playing — which is not what `POST` to a collection means, and is why
a client could not hand a viewer's playback from one device to another.

**`GET /api/v1/playback/sessions` lists the caller's own sessions**, under
`items`. This is the piece that unblocks handover: it did not exist, and
without it a client that lost an id could not find its own session again. It
lists that caller's sessions on that node and nothing else — there is no
cluster-wide listing, because a session is a resource of the node producing it.

**The stream moved under the session it belongs to.**

```text
GET /api/v1/playback/sessions/{id}/stream/{token}/{generation}/{name}
GET /api/v1/playback/sessions/{id}/stream/{token}/direct
```

`GET /api/v1/playback/stream/...` is **removed**. The capability stays in the
path rather than moving to a header because it is a capability, not a
credential: media players fetch fragments without application headers.
`playback/status` and `playback/media` are different resources and did not
change.

**The per-account cap ships in the same release, not after it.**
`streaming.max_sessions_per_account` (default 32) bounds what one account may
hold on one node; over it, creation is refused `429` with code
**`account_session_limit`**, `scope: request`, `node_healthy: true`, and both
the limit and the caller's live count stated. This is not an optional extra.
`max_sessions` is node-wide only, and the one-session-per-bearer rule had been
doing the per-account job by accident — removing it without a cap is exactly
the media DoS that governs this design. Transcode entitlements share the cap's
key, so splitting one viewer into several sessions does not multiply them.

**An account-scoped refusal is deliberately distinguishable from a node-scoped
one.** A client classifies failures by scope: a node-scoped refusal makes it
walk the cluster, and an account-scoped refusal is identical on every node it
would walk to. Charging every healthy node it tries turns one account hitting
its own cap into a cluster that looks like it is failing.

**Keep `max_sessions` above `max_sessions_per_account`.** Otherwise the
node-wide limit refuses first and the account cap can never be reached — which
loses the distinction above in the one case it exists for. The shipped example
config now pairs 64 node-wide with 32 per account; the compiled defaults are 8
and 32, so a node running the default `max_sessions` must raise it.

**A superseded generation answers `410 generation_superseded`.** It carries
`scope: request`, `node_healthy: true` and `alternative_may_succeed: true` —
this node is healthy, and a different request against this same node works.
Held back from 0.47.0 until macha-client-core shipped tolerance, because core
maps an unrecognised fragment status to `unknown` and reads `unknown` as
evidence against the endpoint, so emitting it early would have charged a node
that was producing perfectly. It ships with the release that moves the routes,
so no client is ever pointed at a node whose statuses it cannot classify.

Before this, a replaced generation and a segment index that never existed
shared one `404`. Under the new routes a superseded generation stops being
exotic — every regenerate, mode switch and rebuilding seek makes one — so the
old behaviour would have turned a routine event into evidence against a healthy
node. A generation *above* the current one still answers `404`: nothing here
ever produced it.

**Telemetry stops being a positional format.** Every field is now tagged and
length-delimited inside a length-delimited record, the magic moves to `TEL3`,
and there is deliberately no compatibility with what came before: a node
speaking the old format refuses the set outright rather than misreading it.
**Every node must move together.** Each node logs one `persisted telemetry
ignored` warning on first start as the old cache is discarded and rewritten;
it is self-healing and does not recur.

The format it replaces worked between peers of one version and broke across
two, which is the only time a wire format matters. Fields were appended in
order and optional trailing ones were detected by asking whether any bytes
remained — which, in a set of up to 64 records on the gossip path, is the next
record. One added field cost a mixed-version cluster every multi-node telemetry
set it exchanged. Tags also buy smaller packets, since a default-valued field
is now omitted entirely.

**Three fields ride the new format into the per-node `playback` block of
`GET /api/v1/status`**: `max_sessions_per_account`, `pipeline_idle_ms` and
`session_idle_ms`. A client learns them about every node it might fail over to
rather than only the one it is talking to, and stops holding private copies of
this node's configuration. The account's live **count** is deliberately not
there — it is the most perishable number this API carries and that payload is
cached — so it appears only where it is computed live: on creation, on the
listing, and on the refusal.

**Security review of the whole `/api/v1/playback` prefix**, with four fixes:

- **The session control routes had no ownership check.** `GET`, `PATCH` and
  `DELETE` looked a session up by id and acted on it without asking who was
  calling, so any authenticated account that learned an id could read, re-seek
  or delete another viewer's session mid-film — and free their cap slots.
  Latent before, because an id was only ever known to the client that made it;
  practical now that a listing hands ids out. All three check ownership and
  answer **404, not 403**: whether an id exists here is not something one
  account learns about another.
- **Ownership was silently dropped by every session replacement.** A subtitle
  change, a fast-path seek and a mode change each replace the session record
  field by field, and none carried the new `account`. A replaced session
  stopped counting against the cap, so a subtitle change was a way to launder
  sessions past it.
- **The authentication exemption and the router disagreed about what a stream
  URL is.** The exemption matched `/stream/` anywhere after the session id
  while the router required it as the next segment — a path exempt from the
  bearer but routed elsewhere is an authentication bypass. Both now call one
  `parse_stream_route`.
- **Idempotency keys were a global namespace.** Any account could occupy
  another's key (`retry-1` is not hard to guess) and turn its legitimate retry
  into a `409` — a targeted denial of the retry path, which is the path a
  client is on when something has already gone wrong. Keys are now scoped per
  account.

**The stream token is compared in constant time.** It is a capability, so
`std::string`'s `==` was a secret-dependent branch. Remote timing exploitation
across a network against 256 bits of hex is not a practical attack, which is
why this was recorded rather than rushed, but the compare costs the same either
way.

**`GET /api/v1/playback/status` exposes node aggregates to any `media_viewer`**
— session count, transcode load, cached probe bytes, and now the cap limit.
This is a deliberate disclosure, accepted for a household system and useful to
clients, recorded here so it is a decision rather than a discovery.

**The shared codec stops copying what it only reads.** `view()` and
`view_bytes()` return a borrowed span rather than a `Bytes`; `raw()` and
`bytes()` keep copying, because plenty of callers hand the result onwards and
a span into an RPC payload must not outlive the decode — the borrow is opt-in
at the call site that knows its own lifetime. `string()` went through an
intermediate `Bytes` and copied twice, which is not free across the thousands
of names in a metadata snapshot. Telemetry decode now borrows throughout: at
up to 64 records, one allocation per record became none. What owns its storage
and outlives the buffer still copies once, into the thing that owns it.


## 0.47.0 — How fast this node is actually producing (development)

**A client can now tell whether a handover would close the gap before the
viewer reaches it, from one response.** `stream.production` is on the playback
session payload beside `stream.look_ahead_ms`, with four fields:
`produced_ms` (media produced by this generation, which is also the production
frontier), `producing_ms` (the encoder time it took), `produced_age_ms` (how
long ago the last fragment landed) and `producer_parked`. Absent for direct
play, which has no pipeline and therefore no rate.

The rate is `produced_ms / producing_ms`, and the wait for a join at position
`P` is `(P - produced_ms) / (rate - 1)`. The pair is raw rather than a computed
rate: a rate produced here would carry this node's smoothing and this node's
window, and the client making the handover decision needs to choose both. One
response answers it, so there is nothing to poll and nothing added to a
viewer's critical path.

**`producing_ms` is not wall clock, and the difference is the entire point.**
The producer runs to `max_ahead_segments` beyond demand and then blocks on the
gate, so a viewer watching at normal speed keeps it parked for most of the
generation's life. Elapsed time would therefore read about 1.0x however fast
the encoder is -- and 1.0x is read as "cannot outrun realtime", which refuses
exactly the handovers that would have succeeded. The Web Client measured 1.49x
on this hardware by pulling fragments flat out; this field reports that same
figure for a viewer watching normally. `producing_ms` accumulates only the
intervals in which the encoder was running, measured as the gap between one
publication returning and the next beginning, so the wait is structurally
outside the total rather than subtracted from it.

**The two figures cover the same fragments, including the first.** Timing only
the gaps *between* publications would have measured n-1 fragments while
counting the media of n, overstating the rate by n/(n-1) -- 2x at the second
fragment, which is when a handover decision actually gets made. Including the
first charges pipeline start-up to the rate, so it reads low early and settles
as the generation runs. That bias is deliberate and in the safe direction:
understating costs a handover that is deferred, overstating costs a viewer
stalled on a promise the node could not keep.

`producing_ms` is `0` until the first fragment lands, and means "no reading
yet" rather than an infinite rate. `produced_age_ms` is an age measured on the
node rather than an instant, so a client's confidence decay does not depend on
its clock agreeing with ours; read with `producer_parked`, because a large age
otherwise cannot distinguish a wedged pipeline from one comfortably ahead and
waiting for its viewer.

Documented in `docs/streaming.md` under "How fast this generation is
producing".

## 0.46.3 — The laws the code already obeyed (development)

**Three laws order every scheduling decision in this system and none of them
were written down anywhere a reader could reach.** They were stated once, in
`TODO/ACTIVE.md`, a backlog file whose own header warns it will mislead anyone
who reads it as guidance. Meanwhile the source cites them by number as settled
authority: `src/retained_memory.hpp:212` ("Governing law 2 is that the viewer
never waits"), `src/config.hpp:333` ("governing law 1, as a data structure"),
`src/fuse_frontend.hpp:491`, `tests/test_foundations.cpp:1115`. A contributor
who hit "governing law 1" in a header had nowhere to look it up. Zero
occurrences across `docs/` and every root document.

They now open `ARCHITECTURE.md`, above the design boundary, with the numbering
intact, alongside the four self-healing disciplines that had the same problem
and lived only in a dated plan file.

**The laws do not simply rank, and saying so is the substance of the section.**
Law 3 is subordinate to law 2 -- that is what its second clause is for, and why
loader work yields to a viewer rather than negotiating with one. Law 1 is not
subordinate to law 2: it is a floor law 2 may not eat through -- control's
reservation is set aside first, not carved from the viewer's share. A node serving viewers perfectly while unable to answer `ping` has
broken law 1, and peers who cannot see how well it was doing will record it as
dead. So the resolution order is law 1's floor reserved first, law 2 taking
priority within what remains, law 3 governing the rest.

**Named at the point of application** rather than stated once and left to be
spotted. The three `runtime.*_memory_reserve_bytes` settings are the laws
expressed as memory, which reframes sizing them as a decision about which class
of work is allowed to fail first. The fast-control allow-list is law 1 on the
RPC path. The DATA priority ordering is laws 2 and 3 as an execution order. The
durability-token probe is discipline 1, the recovery-resolution table
discipline 3, the retry budgets discipline 2, snapshot composition discipline 4.

**The two places a viewer genuinely waits are now labelled as the bounded
exceptions they are**: a request past the produced frontier, bounded by
`stream.look_ahead_ms`, and a held segment, bounded by `segment_timeout_ms`.
What makes them lawful is that the work in front of the viewer is its own.
Writing them down stops them being cited as precedent for a third.

`CONTRIBUTING.md` leads with a review gate built from the laws: a change to
scheduling, admission, priority, retry or recovery states which law it serves
and which discipline it follows.

**Five factual defects fixed.** `docs/configuration.md` had a corrupted
paragraph, the sentence describing `publication_quantum_bytes` split in half by
an unrelated paragraph spliced into its middle. `SECURITY.md` named
`<state_path>/genesis-root-password` for the generated root password, where
`src/users.cpp:716` writes `initial-root-password` -- anyone following it during
a recovery would have found nothing. `SECURITY.md` also claimed a metadata
minority refuses mutations rather than creating a second namespace history,
which is the opposite of the write-floor model documented everywhere else. Both
role tables omitted `view_status` and claimed every role implies `media_viewer`
as the only implication, where `src/users.cpp:150-157` has a two-step chain
ending at `view_status` -- the mechanism for exposing cluster health to an
unauthenticated client, previously undocumented. `docs/catalogue.md` described a
superseded `202`-on-miss path for the media profile, which resolves in the
foreground and returns `200`; `docs/streaming.md` had it right, so the two files
disagreed.

Also corrected: the cluster protocol version, documented as 20 in three places
against `src/net.cpp:23` at 21, and `SECURITY.md`'s "transport v7" naming a
versioning scheme the handshake no longer has.

**Removed.** A release-assembly log from 0.18.2 standing in for
`VALIDATION.md`, rewritten as the invariants the suite pins down. Version
archaeology throughout ("Before 0.41.0", "Until 0.32.11", "Since 0.29.0", a
`0.19 implementation` heading, a migration-from-0.18 section). Dated incident
measurements kept in place of the rules they were evidence for. Legacy config
keys still parsed are documented as aliases without the backstory.

**One gap documented rather than fixed.** `viewer_weight` and `loader_weight`
are validated only as 1..10000 independently
(`src/config_base.cpp:158-160`), so `viewer_weight: 5` with
`loader_weight: 95` is accepted and inverts law 2: a bulk import outranking
playback on the same node. `docs/configuration.md` now warns. Rejecting the
inversion in `validate_config` is the other half and is not in this release.

Documentation only. No behaviour changed and no code touched.

## 0.46.2 — The budgets a node will admit to (development)

**A client had to guess how long this node would take, and guessed low.** Core
carried `GENERATION_ATTEMPT_BUDGET_MS = 12000` against a hardcoded guess at our
`startup_timeout_ms` of 15000, so it abandoned a node three seconds inside that
node's own entitlement -- not occasionally, structurally, on every attempt that
ran long. Measured on 2026-09-18 on `tmdb:episode:7203311`: a seek took the
fast path in about a millisecond, the container seek cost 39 ms, and the first
fragment took 11,672 ms because it was 4K HEVC re-encoded to H.264 in software
on a Pi. Comfortably inside our bound. The client failed it anyway, deleted the
generation that landed 1.4 s later, and failover started the identical encode
on the other node. The viewer got a failure screen instead of a wait and the
cluster did the work twice.

Two `uint32` fields on `NodeTelemetry`, surfaced on the per-node entries of
`GET /api/v1/status` in a `playback` object beside `runtime`:
`startup_timeout_ms` and `segment_timeout_ms`.

**Why there and not on the session payload**, which is where `look_ahead_ms`
lives and was the obvious precedent: a client needs these for every node it
might fail over to, not only the one it is playing from, and it needs them
*before* the request they bound. It cannot learn a startup budget from the
response it is timing out on, and a node it has never used would never send one.
The cluster status payload already describes every node and a client already
reads it; these join `load1` and `cpu_cores` there as one more self-reported
fact.

**What the server does not do is compute a cluster-wide figure.** It has no
data to: telemetry carries no peer's streaming configuration, so a node asked
for the worst case across the cluster would be inventing one about peers it
cannot see, and it would be stale the moment any of them reloaded. Relaying a
node's own statement is not the same act as aggregating, and the aggregation
belongs to the only party that knows which nodes it might use.

**Absence means the node cannot say, never a default.** A node predating the
field omits them, and so does one with `streaming.enabled` false, which will
not honour a playback budget it does not run. A client must fall back to its
own conservative bound rather than read a missing field as zero -- a default
would be indistinguishable at runtime from an answer, which is the mistake the
0.46.0 seek work exists to correct. The two are also all-or-nothing on decode:
a record carrying only the first is treated as carrying neither, rather than
pairing a real startup budget with a fabricated segment one.

**A latent flaw this change exposed, and did not cause.** A telemetry set
concatenates its records with no per-record length, so a decoder reading an
optional trailing field cannot tell "this record ends here" from "the next
record begins here". Gossip sends up to 64 records per set
(`src/cluster.cpp:1146`), so during any rolling upgrade that adds a telemetry
field, a multi-record set from the other version misparses and is dropped
whole. Every telemetry field addition has had this property; adding these two
is what made it visible, as a one-off `persisted telemetry ignored: blob too
large` on each node's first start, after which the cache is rewritten in the
new format and the warning does not return. Both nodes were taken to 0.46.2
together to close the window rather than left mixed. The fix -- length-delimit
records within a set -- is its own change with its own compatibility cost and
is not in this release; it is recorded in `TODO/ACTIVE.md`.

Also in this release: the `/api/v1/health` comment claiming the route carries
no version was stale. It has carried one since 0.42.1, deliberately -- reading
what a node is running without a token is how every on-box check and deploy
verification is done -- and the recorded client contract said the same wrong
thing. Both now describe the route as it behaves, and the rule that still holds
is stated separately: no node identity, no topology, no configuration.

## 0.46.1 — The profiles a restart used to throw away (development)

**A media profile computed but not yet published was discarded on shutdown.**
`PlaybackManager::stop()` requests the publisher thread to stop and joins it;
the publisher's loop broke out on the stop token without draining
`pending_profile_publications`, and nothing else drained it. Every restart
therefore threw away the profiles probed in the seconds before it, and the next
play of those titles paid for a full foreground container inspection again --
the `immutable profile miss` path the profile cache exists to avoid. Both of
the 0.46.0 deploys did exactly that.

The publisher now drains what remains on the way out, through
`put_media_profiles()` so the whole queue costs one catalogue commit rather
than one per entry and shutdown stays prompt. A failure there is reported
rather than retried: shutdown is not the place to fight a transient CAS
conflict, but a profile lost this way is silent repeated work and should say so.

**Known limitation, stated rather than papered over.** The drain is
best-effort on a multi-replica node. `Service::request_stop()` calls
`node_.cancel_outbound_calls()` before `streaming_->stop()` runs, so a commit
needing a peer is attempted after outbound RPC has already been closed. Moving
the drain earlier to beat it is not safe -- `request_stop()` is the only thing
that cancels those RPCs, so blocking a commit there could hang shutdown on an
unreachable peer. It is deterministic on a single-replica node; on the cluster
it now logs `profiles lost at shutdown count=N` instead of losing them
silently. Making it deterministic needs a shutdown-ordering change or a
persisted queue, and neither is this release.

**Three tests were asserting a result they had not waited for**, and were
failing roughly one full-suite run in four on es-1 -- always a different test,
which is what made them look like noise rather than the four distinct races
they were. None is a product fault: in each case the node refuses correctly and
names why.

`wait_metadata_writable()` is the shared helper they were missing. Membership
convergence is not write readiness: a node can see every peer and still be
forming its metadata replica set, or be read-only behind a write-floor policy
mismatch, and a mutation is then refused with `metadata replica set forming:
waiting for bootstrap checkpoint survey` or `metadata commit durability floor
unavailable`. The floor is what to wait for and the node already publishes it
as `MetadataClusterStatus::write_available`. Applied to
`test_established_metadata_floor_ignores_misconfigured_peer` and both write
sites in `test_replication_policy_change_on_restart`.

`test_catalogue_sync_search_and_artwork_gc` waited on
`status.artwork_objects == 1`, which counts what a node's *catalogue* knows
about; the bytes behind it are a DATA object the node may still be fetching. It
now waits for the fetch rather than for the count that precedes it.

Verified by twelve full-suite runs on es-1, where the same loop had previously
produced failures in three runs out of twelve. Four other tests share the
`metadata replica set forming` race and are not fixed here; the helper they
need now exists.

## 0.46.0 — A seek goes where it was asked to go (development)

**A seek no longer starts after the position it was asked for.** A remux seek
started *later* than the request, by up to 9.3 s, always forward: the planner
discarded every keyframe earlier than the request and took the first survivor.
The content between the request and that keyframe was in no generation at all.
No client could recover it. For a viewer seek that is a skipped scene; on the
reaped-session recovery path, which rebuilds a generation at the position a
viewer has actually reached, it deletes content mid-playback.

Measured on 2026-09-17 against es-1 and fi-1, remux, a 3,951,957 ms title:
asked for 2,027,092 ms the node started at 2,028,903; asked for 908,791 it
started at 918,085. The alignment is deterministic — asked for 2,926,000 the
node returned 2,934,933 to 147 consecutive requests over 33.3 s — so a client
bound that rejects a start more than one segment ahead cannot make progress.
That is how this became a livelock rather than an inconvenience. It was
confirmed independently of any server log: the browser's media element reported
the replacement generation's duration as 3,033.9 s against a title of 3,951.957
s, a difference of 918.057 s matching the reported start to within rounding.

**The rule.** The server does what it is told. It does not change the mode a
client asked for, and it does not move the position a client asked for. Where a
mode cannot begin a stream at the exact position requested, the response says
so explicitly instead of relocating the request and reporting the relocation as
though it were what was asked for.

**Three flat fields on the playback session payload**, present on create and on
every `PATCH`, all milliseconds on the title's timeline:

- `seek_ms` — where the generation's media actually begins. This is exactly
  what the field has always meant, so no existing client changes behaviour.
- `seek_offset_ms` — how far into that generation the requested position sits.
- `seek_requested_ms` — the position the server honoured, after clamping to
  `[0, duration - 1 ms]`.

The invariant is `seek_ms + seek_offset_ms == seek_requested_ms`, exactly, in
integer milliseconds, with no tolerance and no rounding slack. `seek_offset_ms`
is never negative, so a generation always contains the position asked for.
`seek_requested_ms` exists because an exact invariant is only useful if a client
can act on it being violated, and without it a client cannot distinguish a
violation from an ordinary clamp near the end of a title — those want opposite
handling. Core's phrasing for the rule it keeps rediscovering: an unanswered
question must not read as an answer.

Per mode: transcode is exactly the request with a zero offset, because the
encoder can start on any frame; remux is the last keyframe at or before the
request with the remainder in the offset, because a stream copy has no decoder
and an fMP4 fragment's first sample must be a sync sample; direct is the request
with a zero offset, because there is no generation. The offset is zero exactly
when the mode can be frame-accurate, and a client that wants a cheap,
exactly-aligned seek asks for a position that already is a keyframe.

**The transcode paths stop snapping entirely.** They snapped forward to spare
the decoder its pre-roll on slow software decode. The pre-roll is still paid —
the decoder seeks back to the preceding keyframe and discards frames before the
origin — but it is the price of asking for a non-keyframe and it is the
client's to pay, whereas snapping lost content. `nearest_keyframe_at_or_after`
is gone.

**The mode is never substituted.** An earlier draft of this work had remux fall
back to transcode where no keyframe at or before the request existed. The
operator rejected that outright, and correctly: it is the same second-guessing
as moving the seek, and it would trade picture quality and CPU for a case the
client did not ask about. Where the index names no keyframe at or before the
request, the baseline is 0 and the offset carries the whole request — a
decodable stream's first sample is necessarily a sync sample, so a copy can
always begin at the beginning; the index simply did not name it. In practice
unreachable, since a file's first frame is virtually always indexed; it exists
so the invariant needs no escape hatch.

**Two diagnostics this investigation needed and could not have.** The keyframe
index's shape is now logged on a plan that succeeds, not only on one that is
rejected: entries, longest gap, median gap. `video_keyframe_seconds` reads the
demuxer's index, which for Matroska is the Cues, and Cues are not obliged to
name every keyframe — so an observed spread is an upper bound on the true GOP,
and offsets clustering well below the indexed gaps mean the Cues are sparse
rather than the GOP long. Separately, the seek fast path declined silently:
across a whole day on es-1 there were zero `seek fast-path` lines and nothing
recorded whether `seek_only` was false or `reseek_hls_vod` declined. Both now
name the failing precondition.

Still open, found by this work and not fixed in it: the seek fast path is never
taken on the live cluster, and `indexed_plan`'s 90 s fragment and tail bounds
are whole-file, so a sparser GOP anywhere in a long title can reject a seek
point that would play perfectly well. The transcode branch of `reseek_hls_vod`
already carried a comment warning of exactly that; it was never applied to the
remux branch. Named as a decline reason rather than asserted as the cause — the
diagnostics above will settle it. Measured cost while it is broken: 147
`session-update` calls in 34.7 s, 34.68 s of cumulative server time, ~4.2/s on
a node also serving viewers, none of them individually slow.

## 0.45.0 — The look-ahead the node actually has (development)

**The playback session now says how far ahead of the viewer it is producing.**
`stream.look_ahead_ms` is on the session payload from `POST
/api/v1/playback/sessions` and every `PATCH` of one: how far past the fragment
a client last requested a viewer may arrive and still find media already
produced. It is `null` for direct play, which has no pipeline and therefore no
frontier.

It was not knowable before. The producer runs to `highest_requested +
max_ahead_segments` and then parks, and `segment_hold_window` is deliberately
the same distance, so that product is the line between a request that is held
and one that is refused at once. Neither number was on the wire, and neither
was in `docs/configuration.md` either, so a client had nothing to bound itself
against except the defaults. A client that hardcoded 8 and 4000 against a node
configured with `max_ahead_segments: 4` would believe it had 32 s of
authorised production ahead of the frontier when it had 16, and would sit
refused at the frontier for the difference.

**That is the general case, and it is not what the bug behind this release
was.** On es-1, where the freeze was measured, `max_ahead_segments` is
explicitly 8 and `segment_duration_ms` 4000, so the client's assumed constant
was correct and the frontier really was 32 s wide. The client used the right
number and arrived past the frontier anyway. Those call for different fixes —
"the client had the wrong constant" is fixed by putting the value on the wire,
"the client arrived past a correctly-read frontier" is fixed in the client's
recovery path — and both were needed. Recorded because the first draft of this
entry conflated them, and the distinction is the more useful half.

The bug: a client recovering a reaped play session created a replacement, held
it 28 s while the viewer played out its buffer, and then asked for the fragment
at the position the viewer had reached. Measured on es-1 at 12,749 ms of frozen
picture. Nothing was wrong with the node. Creation already starts the pipeline
— `create` calls `start_pipeline` before it answers, and blocks on the first
fragment, 1,924 ms in the captured trace — and the generation was still warm
after the hold. But warming fragment 0 authorises production only as far as
fragment 8, the viewer arrived past that, and the node encoded its way forward
at roughly real time: 6.27 s and 7.59 s for single fragments, refused with
`reason=hold_timed_out` in between.

Reported as a derived duration rather than as the two knobs on purpose. A
count and a duration are two numbers the client has to multiply and then keep
in step with ours, and the derived figure stays meaningful if this bound ever
stops being counted in segments.

**`docs/configuration.md` now documents the fragment-production knobs at all**
— `segment_duration_ms`, `max_ahead_segments`, `segment_hold_window`,
`segment_memory_bytes`, `max_session_holds` and `max_concurrent_holds` — with
what the look-ahead is, why the hold window matches it, and that
`hold_timed_out` is a retryable `500` with `Retry-After: 1` rather than a
`404`: the playlist has already promised the fragment exists, and a `404`
invites an intermediary to cache the absence.

Nothing about production timing, admission or the transcode entitlement
changed. This release adds a field and a page of documentation.

## 0.44.0 — The bytes that never needed to be on the wire (development)

**The HTTP server compresses what it sends.** It did not, at all: there was no
`Content-Encoding` on any response, and the `Accept-Encoding` every browser
sends on every request was parsed into the header map and then ignored. A web
client bundle and a catalogue listing both went out in full, on every request,
including to clients on the other end of a WAN link.

Text responses are now gzipped on a lane worker — JSON from the API, and the
client's HTML, CSS and JavaScript. Typical listings and bundles fall to
between a quarter and a third of their size. Compression happens on the
compute pool and never on the reactor, which may not do CPU work on a
socket's behalf; by the time the reactor writes the headers the body is final
and `Content-Length` is already the compressed length.

**Media is deliberately untouched, and that is the point.** An extent, a
transcoded fragment and a direct-play range are already compressed, they are
served through a body source rather than a byte buffer, and the reactor sends
them straight from resident memory with no copy and no pool hop — the path
0.43.0 exists to provide. Only a complete in-memory body of a compressible
type is ever eligible, so that path is bit-for-bit what it was. Ranged and
`304` responses are excluded outright, images, fonts and wasm by type.

For the web client, a precompressed file beside the asset (`app.js.gz` next to
`app.js`) is preferred and costs nothing per request; where the build produced
none, an asset up to `compression_max_asset_bytes` is compressed on demand
instead, so a node gets the win without waiting on a client release. The two
representations never share an entity tag: one tag for two different bodies
lets a cache hand a client bytes it cannot read and makes a `304` a lie. The
API's own `rev-N` tags are left exactly alone — those are `If-Match`
concurrency tokens a client sends back on a write, not cache validators, and
the suffix convention other servers use would have corrupted them. Every
compressible response carries `Vary: Accept-Encoding` whether or not it was
compressed.

All of it is configurable under `catalogue.api`: `compression`,
`compression_min_bytes`, `compression_level` and
`compression_max_asset_bytes`. A node behind a proxy that already compresses
sets `compression: false`, which is a supported deployment rather than a
degraded one — some nodes are exposed directly and some are not, so the server
has to be correct either way. `responses_compressed` and
`compression_bytes_saved` in the diagnostics route say what it is doing.

## 0.43.1 — The namespace stops paying for slots it never fills (development)

**A decoded namespace no longer carries up to 2x allocator slack in its extent
vectors.** `entry(Reader&)` filled each entry's extent list with `push_back` and
no `reserve`, so every vector sat wherever geometric growth had last doubled it.
Measured on es-1 (1.618 TiB of library, 4,808 entries): **610,567 extent slots
holding 424,222 extents** — 10.4 MB of empty slots in a 36 MB snapshot. That
slack is permanent rather than transient, because the current and committed
materialisations are pinned in the replica's cache and exempt from its eviction
budget (`src/metadata.cpp:3203`), so it is resident on every node for as long as
the head is the head.

Decoding now reserves exactly, bounded by what the remaining input could
actually contain (49 encoded bytes per extent) so a forged or corrupt extent
count sizes nothing. The decoded snapshot fell from 36.2 MB to 25.8 MB, a
**28.8% cut in decoded namespace residency**, which at the 100 TB design target
is roughly 640 MB per node. No encoding changed, no format version moved and no
migration is involved: the same bytes decode to the same snapshot, in less
memory.

`macha-metadata-dump --stats` now also reports what a head actually costs —
decoded bytes, encoded payload, extent slots against extents in use, the
relevant `sizeof`s, and bytes per TiB of library — and `snapshot_resident_bytes`
is exposed from `metadata.hpp` so the tool charges exactly what the
materialisation cache charges. This is the measurement behind Stage A of
`TODO/2026-09-17-namespace-merkle-root-plan.md`, which the same numbers say is
still the load-bearing work: residency remains linear in the size of the
library and whole-library on every node, including the ones that store no
extents at all.

**The test suite no longer loads whatever plugins happen to be installed on the
build machine.** An ordinary test wants no subsystem plugins at all, and
`test_support.hpp` left `plugin_path` unset to say so. Unset does not mean "no
plugins": `normalize_config()` fills an absent `plugin_path` with the installed
directory, so every test process resolved it to `/usr/lib/macha/plugins` and
loaded the deployed build rather than the build under test. It went unnoticed
for as long as the two matched; bumping es-1 to 0.43.1 made the suite log
`plugin=0.43.0 core=0.43.1 ... refusing to load (partial deploy?)` and exposed
it. Test configurations now set an engaged but empty path, which is the only way
to say "builtin subsystems only".

The quickstart now names the build dependencies as a copyable command per
distribution — Debian/Ubuntu, Fedora and macOS (Homebrew) — instead of describing
them in prose and leaving the package names to the reader.

## 0.43.0 — The HTTP server without a thread per connection (development)

**The API is served by one reactor thread that owns every socket and never
waits, two bounded pools that only compute, and continuations for the
requests that used to block.** Until now the server was sixteen worker
threads, and a worker was spent on every kind of waiting the server did: a
kept-alive connection idling for up to 15 s between requests, a held segment
request waiting on the encoder, a slow viewer draining a multi-megabyte
fragment over the WAN, a direct-play range fetched from another replica.
Only running a handler was work. Three client families each holding one
idle connection took most of the pool; one deeply prefetching player could
take all of it; and when the pool was gone the node stopped answering
`/api/v1/health` and `/api/v1/status`, which is exactly the shape of the
unexplained 10 s Status response of 2026-09-13. Plan and reasoning:
`TODO/2026-09-15-http-server-reactor-plan.md`.

What changed, in the order a request meets it:

- **Sockets are non-blocking and owned by one reactor** (`macha-http-io`),
  a `poll()` loop with a per-connection state machine: reading, dispatched,
  deferred, writing. An idle kept-alive connection is a descriptor and a
  small struct. The keep-alive rules, `413`, `OPTIONS`, CORS headers and
  `Content-Length` framing are unchanged; `catalogue.api.max_connections`
  (default 1024) replaces `max_queued_connections`, which is still read.
  Pipelined requests are parsed out of the bytes already received rather
  than waited for.
- **Handlers run on a compute pool in two lanes.** The data lane
  (`catalogue.api.workers`, 16) runs catalogue, playback and web-asset
  routes and every body read that can block on a disk or a replica. The
  control lane (`catalogue.api.control_workers`, 2) runs health, status,
  session and account routes, so a node saturated serving fragments still
  says what is wrong with it: governing law 1 as a data structure rather
  than a hope. A lane with `max_queued_requests` (256) waiting answers
  `503 overloaded` with `Retry-After: 1` instead of queueing without bound;
  the body carries `service: macha` and `status: busy` alongside the error
  envelope, so a client confirming an endpoint by its health body can tell
  a busy node from a host that is not Macha at all.
  The handler contract is untouched: every route is exactly the code it was.
- **Streaming bodies are pumped, not pushed.** A resident body -- a
  transcoded fragment in the segment store -- is sent straight from memory
  with no copy and no pool hop (`HttpBodySource::resident()`). Any other
  body is read a chunk at a time on the data lane into a per-connection
  staging window of `staging_chunks` (2) × `stream_chunk_bytes` (256 KB)
  that the reactor drains as the client's TCP window allows. A viewer that
  stops reading costs two chunks and an fd; a client that closes mid-body is
  noticed on the next pass and its body source released. The number of
  simultaneous fragment sends is bounded by bandwidth and memory, not by a
  worker count: the sixteen-sends-in-progress limit is gone.
- **A held segment request costs no thread.** The playback handler asks the
  segment store for the object and, in the same locked step, subscribes to
  the next publication if it is absent (`MediaSegmentStore::
  object_or_subscribe`); it then returns an `HttpDeferral` -- a waker, a
  deadline, and the admitted hold -- and the server parks the connection.
  The store's publication fires the waker, the reactor re-runs the handler
  with `HttpRequest::resumed` set, and the hold is released with the answer
  or with the connection, whichever goes first. `streaming.
  max_concurrent_holds` rises from 8 to 64: it was rationing threads, and
  now it is the fairness and memory bound the 2026-09-08 plan said it should
  become. The admission policy is byte-for-byte what it was.
- **The rule that makes one thread safe is structural, and checked.** The
  reactor's connection state cannot name a handler, a body source, a
  `ReadHandle` or the authenticator; the only pool-filled memory it sees is
  through a pump. It takes one mutex (the inbox's, for a push or a pop) and
  logs only at startup and on accept failure. If anything on it ever sleeps,
  `reactor_stalls` in `GET /api/v1/status/diagnostics` counts it within one
  pass, with the longest pass in milliseconds beside it. A test proves the
  counter works by doing the forbidden thing once on purpose.
- **Diagnostics for the server itself**, under `diagnostics.http`: reactor
  passes and stalls; open, idle-keep-alive, writing and deferred
  connections; staged bytes; requests served, deferred, refused for overload
  and slow; and per lane, workers, busy, queue depth, peak depth and longest
  queue wait. A handler slower than `slow_request_threshold_ms` (1000) is
  logged with its route from the pool -- the instrumentation the Status
  item offered and never built.
- **`X-Robots-Tag: noindex, nofollow` on every response.** A node can
  advertise a public endpoint and everything it serves is a private
  library; the web client's own robots meta tag covers HTML and nothing
  else. Not a security control -- the session gate is that.

Nine new socket-level cases in `tests/test_http_server.cpp` cover the
promises: a body read blocked on one connection delays no other; a blocked
data lane leaves the control lane answering and refuses beyond its queue;
two clients that stop reading do not delay a third's 4 MB body on either
the pool or the resident path; two hundred idle kept-alive connections cost
nothing but descriptors; a client that closes mid-body releases its source
within a pass; a deferred request is resumed when woken and at its
deadline; the stall watchdog counts a sleeping pass exactly once; HEAD and
pipelined requests share one connection. One existing case changed meaning
rather than being deleted: `test_http_keep_alive_sheds_connection_under_
backlog` asserted that an idle connection had to be closed to free the
worker a second connection was waiting for; it is now
`test_http_idle_keep_alive_connection_costs_no_worker`, and asserts the
second connection is served while the first keeps its keep-alive.

**Two suite failures that had been called "known flakes" are fixed, with
their causes written down** (`TODO/2026-09-14-test-suite-must-be-deterministic-plan.md`,
step 3). `IngestManager::ensure_namespace_parents` now treats `EEXIST` from
`mkdir` as the directory existing and re-checks it: two concurrent imports
into a scanner root, series or artist directory that did not exist yet both
saw ENOENT and both created it, and the loser's job failed with the bare
message "exists" -- 2 in 6 runs on es-1, and reachable in production by any
two imports at once. And
`test_concurrent_reads_during_divergence_produce_one_reconciliation` no
longer runs two `Service`s, whose maintenance loops reconciled the divergence
the test had just created before or during its readers (10 in 40 on es-1;
0 in 40 after); it runs bare runtimes and one `MetadataManager`, which is
the unit its claim is about. A third case surfaced by the first clean
full run, `test_edge_node_never_owns_and_its_writes_land_on_owners`, asserted
that two observers agree on placement before their capacity views had
converged through gossip (2 in 30 on es-1); it now waits for the inputs it
depends on. Neither fix retries anything.
A fourth, `test_three_node_cluster`, hung 1 in 10 and was caught with gdb: a
real transport deadlock, described next.

**A peer going away could wedge metadata publication for good.** The RPC
writer loops (outbound `PeerConnection` and accepted `Session`) exit when
the reader marks a connection broken, and on that exit they left their
queue behind: only an explicit `close()` failed the queued frames'
promises, and a peer that simply disappeared never called it. A
`notify()` queued in that window waited on `future.get()` forever -- and
`accept_metadata_commit` announces under `MetadataManager`'s mutation
mutex, so every later metadata mutation on that node queued behind a
frame to a dead peer. Both writer loops now abandon their queue on every
exit, releasing each waiter with an error, and `notify()` stops waiting
the moment the connection is unusable and never waits beyond five
seconds. Dropped opportunistic cache and promotion writes are logged at
debug with their reason.

New configuration under `catalogue.api`: `control_workers`,
`max_connections`, `max_queued_requests`, `staging_chunks`,
`slow_request_threshold_ms`, `reactor_stall_threshold_ms`; all documented
in `docs/configuration.md` and `macha.yaml.example`. Not in this release,
deliberately: `sendfile`, a second reactor, HTTP/2, in-process TLS, and the
RPC transport, which is also thread-per-connection and could take the same
design later.

## 0.42.1 — The health route says what it is (development)

**`GET /api/v1/health` now identifies the server.** The body gains a
`service` field, the literal string `macha`, and a `version`, alongside the
existing `status`:

```json
{"service":"macha","status":"ok","version":"0.42.1"}
```

Until now the whole body was `{"status":"ok"}`, which says nothing about what
answered. That is fine for a liveness probe and useless for identification,
and identification is what a client needs when a person types an address into
an endpoint box, or when a web build probes its own origin for an API. The
case that prompted this: macha-client's own deployment requires the web host
to serve `index.html` for unknown application paths, so a host serving the
client but *not* running Macha answers this route with `200 text/html` — and
core's `checkEndpointConfiguration`, which confirms an endpoint on
`response.ok` alone, would adopt it and then fail every call against a pile of
HTML. Core already ships an `unconfirmed` result meaning "reached, but it did
not identify itself as a Macha server"; there was no way to be sure of that
distinction, so the field existed and the check behind it did not.

`service` is present in every state, both `503`s included. That is deliberate
and is the half a caller is most likely to skip: identity and readiness are
different axes, so a node reporting `starting` is one to wait for rather than
one to fall back from, and the body that says so is the one nobody parses.

**`version` is here by an explicit operator decision, over the argument for
leaving it out.** `test_users` has pinned since 0.38.5 that this route names
no build, on the grounds that it needs no token and so tells an
unauthenticated caller which known defects apply. The decision taken, and the
reason, are recorded at `health_response()` and in that test rather than left
to be rediscovered: `service` already names the product, and anyone reading
that will try the known Macha exploits regardless — the version narrows which
one they reach for, not whether they try. What it genuinely buys an attacker
is a way to index *unpatched* hosts at scale, which is a mass-scanner's
economics and not this project's threat model.

What stays out is the cluster's shape: no node id, no topology, no
capacities. That is what `view_status` is for. A client must gate on `service`
alone — asserting on `version` would break it every release — and the client
team has confirmed it does.

Nothing else about the route changed: still no session and no role (it is the
first bearer-auth exemption, checked ahead of the readiness gate), still the
same three states with `200` for `ok` and `503` for `starting` and `failed`,
still the cluster-wide CORS headers with `Access-Control-Allow-Origin: *`, and
still no `Cache-Control` — clients should keep sending `no-store`.

Mixed fleets need no flag day. The route itself dates from 0.38.5 (before that
a node answers `401`, because auth runs ahead of routing, which is not
identification and simply will not be adopted), and the client rule is "if
`service` is present it must be `macha`", so a cluster upgrades node by node.

Tested end to end against a real `Service` over HTTP with no bearer token, in
both the starting and the serving state, using the existing control-plane
startup gate to catch the `503` — the state that is otherwise never exercised.

## 0.42.0 — Nodes that cannot be connected to (development)

**A node behind CGNAT is a full participant, and an edge node can store
nothing.** Two node properties, `network.inbound_capable` and
`storage.hosts_extents`, each `true | false | auto` (default `auto`),
self-declared and gossiped in every node's record (protocol 21, a
rolling-upgrade event: mixed clusters refuse each other's handshakes as they
always have). Plan and the verified state of the transport it builds on:
`TODO/2026-09-15-inbound-incapable-nodes-plan.md`.

The transport already worked in both directions over one session: when a
node dialled a peer, the peer registered that session as an inbound route
and used it for everything, and liveness and the GC fence were satisfied by
the dialling side alone. What did not exist was any way to *not* dial a peer
that cannot be dialled, or to ask it for a lane. Now: a peer whose gossiped
`inbound_capable` is false is never dialled. `RpcClient::connection()` sends
a `dial_request{lane}` over the CONTROL session that peer opened and waits
(bounded by `connect_timeout`) for the reverse dial to arrive; with no such
session the call fails at once with the same transient error a refused dial
would have. An inbound-incapable node keeps CONTROL and DATA dialled to every
capable peer on its own account (it is the only side that can restore its
reachability), so the request path only covers the window after a drop. The
membership loop stops trying to exchange with such peers when no session
exists -- the `bootstrap: connect: Connection refused` line every heartbeat
is gone for them -- and `all_known_reachable()` no longer fences on a pair
that both accept no inbound connections, since they can never authenticate
each other directly and it is not a fault.

Two transport changes benefit every NAT'd node today. Every socket now sets
kernel keepalive at 60 s / 15 s / 4 (the OS default first probe is two hours
out, longer than any NAT keeps an idle mapping), and the health probe covers
the DATA lane as well as CONTROL, so an idle DATA session that died under an
expired mapping is closed and, on the incapable side, redialled before a
viewer's next read pays a stall.

`hosts_extents: false` is the edge node: it serves the API and media to its
own network from its block cache, publishes writes to the owners over its
own DATA sessions, and stores no extents, so `storage.data` may be omitted
entirely -- it runs an *empty* `StoragePool` (`capacity=0`, every call
answers "not present"/"no space") rather than making the store optional at
forty call sites. Placement has one choke point, `DistributedStore::ranked()`,
and it now filters the active set to hosting nodes; that single change
covers owners, `should_own`, retention candidates, repair, prompt
replication and rebalance. Universal objects are pushed to hosting nodes
only. A node that stops hosting drains through the ordinary repair push. The
capacity aggregates, `logical_capacity` and Status count hosting nodes only.

`inbound_capable: auto` resolves from evidence: once a CONTROL session to a
capable peer exists, the node sends a `dial_back_probe` naming its advertised
endpoint and the peer makes one throwaway TCP connection to it -- a distinct
`probe` transport lane that handshakes (authenticating both ends) and closes,
registering no route, so a probe can never retire a real session in
`reconcile_locked()`. Two consecutive failures resolve `false`, one success
resolves `true`; the answer is persisted under `state_path/connectivity/` so
a restart is not a placement event, re-checked every 10 minutes while false
and every hour while true, logged at INFO on change naming the deciding
peer, and rate limited per asker on the answering side. `hosts_extents: auto`
follows: false with no backends or when inbound resolves false.

Refused at start-up rather than half-working: a founding node with
`inbound_capable: false`, or a joining node whose every bootstrap peer is
known to be incapable. Status: `nodes[]` gains `inbound_capable`,
`hosts_extents`, `dialable` and, on the local node, the configured modes;
`connectivity` gains the resolution and its dial-back evidence;
`cluster.conditions` gains "N node(s) accept no inbound connections", "no
inbound-capable node hosts extents" (critical) and "replication N requires N
extent-hosting nodes; M known". The known-node roster is v3 (flags per
entry; v1/v2 read back as capable hosting nodes).

One incidental fix the flags exposed: `Membership::observe()` replaced a
peer's record with the handshake-time copy on every direct observation, so
the heartbeat ping rewound whatever fresher gossip had merged (capacity,
generation -- and now flags, which would have flapped placement every
round). The newer record wins; a direct observation only refreshes liveness.

Tests: a black-hole-advertised node (192.0.2.1, TEST-NET-1) with a short
connect timeout stands in for one behind CGNAT, no OS firewall needed. The
transport case proves control flows both ways over the one session, a
`get_object` from the hub triggers a dial request and completes, a dropped
DATA lane is redialled without a request, the hub never creates a
connection, and the probe distinguishes a dialable peer from one that is
not. The runtime case proves `auto` resolves false after two failed
dial-backs with `hosts_extents` following, survives a restart as
`persisted`, flips back once the address is dialable, and that a founding
incapable node is refused. The storage cases prove an edge node joins, is
never an owner for any key on any observer, writes to owners and reads back
through its cache; and that a node flipping hosting→non-hosting drains
through repair. Codec, roster, GC-fence and configuration rules are covered
in foundations; YAML parsing of both keys and a data-less storage section in
runtime_dependencies. Still open from the plan: whether a draining node's
*retention claims* release without special treatment (the drain test uses
unretained objects; the publication path is the next thing to exercise), and
the cluster UAT on fi-1.

## 0.41.1 — The writer's garbage collector wakes up (development)

**Superseded catalogue artwork is now reclaimed on the node that wrote it.**
0.41.0 shipped this as a P0 storage leak with the evidence but not the cause:
after a burst of artwork replacements the writing node kept all four
superseded objects indefinitely while its peer reclaimed them, both
catalogues converged and agreeing. The backlog named two candidate
mechanisms, and a `MACHA_TEST_LOG_LEVEL=DEBUG` repeat run reading the
writer's maintenance decisions ruled out both: the retention claims were
already dominated by the release clock (claim dots 4/6/9/11 against a clock
of 13, no release ever applied), and the sweep window was not being pushed
forward by the writer's own events. The writer's maintenance loop was
*asleep* -- one pass in 27 s, stage `wait`, next deadline 30 days out.

The mechanism: a pass that starts `busy` (foreground or interactive I/O
inside `maintenance.foreground_quiet`) suppresses GC, repair and rebalance,
and is supposed to hand the decision back once the foreground goes quiet.
It armed that wake-up only while the foreground was *still* inside its quiet
period at the end of the pass. A pass that started busy and ended after the
quiet period had elapsed -- with the GC window already expired too, so the
GC deadline did not apply either -- armed nothing and slept until the next
unrelated event. On the test cluster that was the scrub deadline; on a real
node it is whenever metadata next changes, which on a quiet library can be
hours, and the dead bytes sit there until then. `Service::loop` now always
bounds a busy pass's sleep by the remaining quiet period, zero included.
Measured before: 1-2 misses in 60 reps of the coalesced-burst case at DEBUG,
`busy=1 gc_quiet_ms=-2 wait_ms=2591999883`; after: 300/300 reps at DEBUG, then three
full-suite runs, 441/441 each.

The diagnostics that made this answerable stay in: the loop logs at DEBUG
why the destructive sweep did not run (only when the answer changes) and
what a sweep did; `Service::maintenance_stage()` and
`maintenance_sleep_diagnostic()` say where the loop is and what it decided
before sleeping; `RetentionStore::claims()` exposes an object's claim dots; a
publication attempt that fails after placing its claims logs its sequence;
and the coalesced-burst test prints all of it on a miss instead of "GC did
not finish in 12 s".

**`FuseFrontend::wait_for_idle` no longer reports idle with work
outstanding.** `status()` sampled the data queue, the deferred inodes and
`active_data` at three different times, while a publication cycles
deferred → queued → active → deferred under `data_queue_mutex`. An inode
could leave the place already counted and enter one not yet counted, so the
composite read idle from a mount about to re-admit work; there was also an
uncounted window between an enqueue being decided (`data_enqueue_pending`)
and the queue push. Every count is now taken under `data_queue_mutex` in one
sample (the inode list copied out first, `admit_deferred()`'s pattern, so the
queue mutex is never held under `namespace_mutex`), and a decided enqueue
counts as pending. Seen as
`filesystem_fuse/test_fuse_publication_quanta_are_fair_and_byte_bounded`
failing three assertions at once after `wait_for_idle(30s)` returned true
(1 in 2,646 case-runs). Test-only API today, but the quiescence primitive
most of the FUSE suite waits on.

## 0.41.0 — FUSE cannot take the node down any more (development)

**The mount is a supervised subsystem, and a failure in it is now a failure of
the mount.** `FuseFrontend`'s constructor replays the durable operation
journal. When that threw, it threw into `main()`'s outermost `catch`, which
logged and exited the process — metadata, RPC, the HTTP API and playback
included, none of which had anything to do with the mount. That is not a
hypothetical: a 0.24.3 bug in `skip_blocked_namespace_operation()`'s journal
bookkeeping crash-looped `corvus-es-1` 49 times before anyone could read why.
0.25.0 built the machinery to contain exactly this (`SubsystemSupervisor`,
`run_supervised`, the plugin ABI) and 0.28.0 proved it on BitTorrent, but the
subsystem that caused the incident was still wired the old way. It no longer
is.

A frontend that cannot be built now faults its own subsystem: logged with the
reason, retried with backoff, and after too many failures in the window marked
`disabled` for an operator, while the node carries on serving everything else.
`GET /api/v1/status` reports it in the `subsystems` block it has carried since
0.25.0, which until now was always empty for FUSE.

**A mount that disappears is remounted, not escalated to a process exit.** The
mount-table watchdog has detected external unmounts since 0.23.0, and what it
did about it was call for service shutdown and return exit code 8 for systemd
to restart the whole node. It now reports a subsystem fault, and the supervisor
rebuilds the mount in place: `restart_count` climbs, `last_fault` names the
cause, and nothing else on the node is interrupted. A mount that keeps dying
walks into `disabled` rather than remounting forever — `RetryState::succeeded()`
deliberately keeps its failure window, so a clean start between faults resets
the backoff without resetting the budget. The covered mountpoint stays
non-writable across the whole cycle.

**`SIGHUP` configuration reload works on a mounted node.** `main()` ran the
FUSE event loop on its own thread whenever a mount was configured, so libfuse's
signal handlers — not Macha's — owned `SIGINT`/`SIGTERM`/`SIGHUP`, and reload
was silently unavailable on precisely the nodes that mount. `main()` is now an
unconditional `sigwait` loop in every configuration, the mask installed before
`Service` exists so every thread it starts inherits it, libfuse's own workers
included. FUSE no longer installs signal handlers at all, and the mount's exit
is no longer the process's exit: the exit codes 3–8 that `run_fuse()` returned
are gone.

**The initial-namespace wait is bounded and cancellable.**
`wait_for_initial_namespace()` polled in 100 ms sleeps forever, with no timeout
and no cancellation, from inside the constructor. A node whose metadata replica
never became available hung there indefinitely with one debug line to show for
it, and on a supervised thread it would have held shutdown for just as long. It
now takes the supervisor's stop token and a new
`fuse.initial_namespace_timeout_ms` (default 10 minutes; 0 restores the old
unbounded wait). It bounds "no metadata has arrived at all", not "recovery is
slow" — the 2026-09-06 lesson, where a 120 s elapsed-time gate turned a
progressing five-minute replay into a crash loop, is why nothing else on the
startup path is timed against it.

**`libmacha-fuse.so` is a new file you must deploy.** The libfuse adapter is a
`dlopen`'d plugin now, alongside `libmacha-torrent.so`; a node that receives a
new `macha` and `libmacha_core` without it silently loses the ability to mount,
and one that receives a mismatched plugin refuses to load it on the build
stamp. Copy `bin/macha`, `lib/macha/libmacha_core.*` and `lib/macha/plugins/`
together and verify hashes across all of them. The boundary is libfuse alone:
`FuseFrontend`, the journal and the mountpoint helpers are core's own code and
stay in `libmacha_core`, so the plugin file decides whether this node can
*mount*, not whether it has a filesystem. `fuse_stub.cpp` is deleted — plugin
absence is capability absence, with nothing compiled in to stand for it.

**`RpcServer::stop` no longer runs queued requests during shutdown.** The
worker loops only return once their queue is empty and queued requests were
dropped only *after* the join, so every request that arrived just before
stop() was executed against a node whose outbound transport, retained-memory
ledger and local writer had already been stopped -- and one that blocked
there blocked shutdown for good. Two 120 s test timeouts on 2026-09-14 ended
at `shutdown: RPC sessions reaped` for exactly this reason. Queued requests
are now dropped with an error reply before the workers are joined, and the
join is logged (`RPC workers joining/joined`). The blocking handler itself
was not captured; if a shutdown hangs again, that log line is where to look.

**The test runner can measure a flake instead of remembering it.**
`--repeat N` runs every selected case N times across the parallel slots and
prints a per-case failure count; `MACHA_TEST_LOG_LEVEL=DEBUG` captures the
product's DEBUG log per case, shown only when the case fails. Two things the
suite did to itself are fixed: concurrent runners shared the same port
blocks and, since every test cluster shares one key, merged each other's
clusters (a per-run `MACHA_TEST_PORT_SALT` now separates them); and
`TempDir` kept whatever a pid-reused, runner-killed predecessor left behind,
so a fresh node could "recover" another test's state. Six cases that failed
under load were classified and fixed; the list and the numbers are in
`TODO/2026-09-14-test-suite-must-be-deterministic-plan.md`.

The case that had been waved past most often --
`hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`,
"at least five separate occasions" per the backlog -- turned out to be four
distinct defects in one test: an exact repair count the product never
promised, an assertion that gating metadata *repair* also freezes the
committed generation (it does not; replication pushes accepted commits), a
baseline sampled after its own quiescence wait rather than being the snapshot
that satisfied it, and a capture that could latch onto a repair from before
the burst it was measuring. Each was found by instrumenting the assertion
that fired and reading the captured numbers, not by re-running until green;
the numbers are in the plan. 600+ reps clean afterwards.

Its remaining GC assertion is now self-classifying: on a miss it retries for
a bounded 30 s and reports whether the objects were *ever* reclaimed, which
ones remain and on which node. That already paid for itself -- the two misses
seen (~2 in 300 reps) are not a slow sweep but the writing node reclaiming
none of the four superseded artwork objects while its peer reclaimed all of
them, both catalogues converged and agreeing. Six occurrences later the answer is
not ambiguous: `reclaimed_eventually=no` after 42 seconds, every time. The
writing node never reclaims them. That is a storage leak on the ordinary
artwork-replacement path, it is **not fixed here**, and it is now a P0 in
`TODO/ACTIVE.md` with the reproduction and the evidence rather than a test
that fails sometimes.

**Also in the supervisor, because FUSE is the first subsystem that needed it:**
`Subsystem::attach_fault_sink()` lets a subsystem report a fault its own
threads discovered after `start()` returned. Phase 1 left that half unbuilt on
purpose ("better designed against a real one than guessed at now"); a lost
kernel mount is that shape. `SubsystemSupervisor::add_builtin()` supervises a
subsystem linked into the binary, and `restarting` is now used where every
attempt after the first previously reported `starting`.

**Two behaviours moved and are worth knowing about.** Mountpoint preparation
(stale-mount recovery plus the fail-closed guard) still runs in `main()` before
any service starts, so the pre-mount window stays shut, and *also* runs before
each mount attempt, which is what a remount after an unexpected loss needs. And
the fail-closed guard now restores the mode the covered directory had before
anything guarded it, recorded once per process: a second mount attempt finds
the directory deliberately non-writable, and recording *that* as the original
would have made a later clean unmount fail it closed permanently.

Gated by `fuse_subsystem/*` — construction fault, mount loss and rebuild, clean
stop, declining without a mount path or a driver, and the real `dlopen` path —
plus `subsystem_supervisor/*` for post-start faults, builtins, and stopping
while a factory is still blocked. libfuse sits behind a `FuseMountDriver`
interface so all of that runs without a kernel mount, which is why the mount
lifecycle has test coverage at all. `foundations/test_main_owns_signals_and_never_runs_the_mount_itself`
keeps `main()` from growing the branch back. Full suite: 441 passing.

Phase 2 of `TODO/2026-09-05-subsystem-plugin-isolation-plan.md`, planned in
detail in `TODO/2026-09-14-fuse-supervised-subsystem-plan.md`. That plan's
Stage A and Stage B both landed here. Its Phase 3 audit item was struck as
already satisfied by Phase 0.

## 0.40.1 — Repair says what it cannot reach (development)

**An object no node can supply is now counted and named, instead of passed
over in silence.** `DistributedStore::repair_step()` contained no `Log::` call
of any kind. Its pull side asks for each object this node should own and does
not have; when no peer answers with the bytes, it moved on without a word, on
every pass, forever. That is precisely the shape of an unavailable extent —
the live namespace still references it and nothing in the cluster holds it —
and it is the question an operator actually has after a node leaves, which
until now could not be answered from the running system at all: Status carries
`convergence`, `data_store`, `retained_memory` and the rest, and nothing about
object availability.

`diagnostics.repair` now reports `unsourceable_objects`, a cumulative count,
alongside `unsourceable_sample`, up to 32 distinct object ids, and
`local_unreadable_objects` — the other silent case, where this node's own store
listed an object in its cursor and then could not read it back, which is what a
failing disk looks like from up here. A warning names the object and the running
total, rate-limited to one line a minute per kind: a cluster that has genuinely
lost a node can be missing a great many objects at once, and a line per object
would bury the journal at the exact moment someone needs to read it.

**It counts rather than concludes, and says so.** A pull can also miss because
a peer was busy, an RPC failed, or a budget ran out, and the next pass will try
again. A small figure that stops growing is ordinary; a total climbing across
passes, or the same ids recurring in the sample, is the signal. That distinction
is in the code comment and in the Status block, so nobody reads a 3 as data loss.

Costs nothing: both counters increment inside a loop that already exists and is
already bounded by scan, operation and byte budgets plus a yield check, on a
path that has just completed a failed remote fetch. The sample is a deque capped
at 32 behind a leaf mutex that holds nothing else, and the log call happens
outside it. Nothing new walks the namespace or the store.

Gated by `test_repair_counts_an_object_no_peer_can_supply`, which also pins that
an object that *is* present is never reported as unsourceable — a counter that
cries wolf is worse than none — and that repeated passes over the same
unobtainable object do not grow the sample. Neutering the call fails it at the
wait.

## 0.40.0 — Five operator decisions, and artwork that can finally be cached

Five operator decisions taken on 2026-09-13, each of which had been sitting in
`TODO/ACTIVE.md` as a question rather than as work.

**Artwork URLs are stable, so artwork can finally be cached.** The expiry in a
signed artwork capability URL is now quantized to a bucket of the configured
TTL instead of being minted from the instant of signing. A browser keys its
cache on the full URL including the query, so a fresh `exp` meant a fresh cache
key: every catalogue read produced a different URL for the same poster, at
millisecond granularity, for every item — twice per item, since `artwork` and
`effective_artwork` are both emitted — and the `public, max-age=86400,
immutable` header the artwork response has always sent was therefore never once
consulted. Posters were re-fetched over the network on every page load, and two
client teams independently built the same id-to-URL memo to work around it. The
expiry now rounds up to the bucket *after* next rather than to the next one, so
the URL is byte-identical for every request inside a bucket while its remaining
validity is always at least the configured TTL and at most twice it — a naive
boundary would have given a URL minted just before one a lifetime of almost
nothing, invisibly to the client holding it. Gated by
`test_catalogue_artwork_url_is_stable_so_it_can_be_cached`, which also pins that
the list and item routes, which sign separately, agree.
A separate observation for whoever picks up the remaining complaint: because
there was no bucket at all before this, "cached posters go stale after a minute
or two" cannot have been a bucket expiring, and still has an unmeasured cause.

**An abandoned playback session no longer holds a transcode slot for half an
hour.** The video/audio transcode entitlement lives on the session, not on the
pipeline, so reclaiming an idle engine at `pipeline_idle` only made an
abandoned session cheap — it went on holding the slot until the session itself
expired at `session_idle`, 30 minutes, and with `max_video_transcodes: 1` that
closed the node to transcoding for the whole of it while the node looked
perfectly healthy. No client-side fix reaches this: a suspended app's closing
`DELETE` may never leave, and a client that crashes, is force-quit or loses
power can never send one. A session that has **never** served a stream object
now expires on `streaming.session_unused_idle_ms` (default 120 s) instead.
Deliberately "never", not "not recently": one playlist, fragment, subtitle or
Direct Play body is enough to earn the full `session_idle` permanently, so a
paused or seeking player is never evicted by this clock, and both clocks run
from the session's last interaction of any kind, so a client that is still
polling or PATCHing is never evicted either. `stream_touched` could not answer
the question on its own — it is set at construction and reset by every
`start_pipeline()`, so it says "not recently" and never "not ever". The reaper
takes the lesser of the two budgets, so an unused session can never outlive a
used one whatever the configuration says. `playback/status` reports
`session_unused_idle_ms` and `unused_sessions_reclaimed`. Gated by
`test_a_session_never_streamed_from_does_not_hold_a_transcode_slot`.

**`GET /api/v1/users` answers under `items`, like every other collection in the
API.** It answered under `users` until now, inherited from the manage endpoints
rather than chosen, and each client had to learn that separately. Single
records from `POST`/`PATCH` are unchanged and still bare. `@machafoundation/core`
has accepted either key since its 0.8.0, so no client needs a release; gbni-2
continues to emit `users` until it can be upgraded, which the same code covers.
The envelope is now pinned by a test.

**FUSE traffic is loader traffic, and the code no longer pretends otherwise.**
`FuseFrontend::note_viewer_activity()` was declared, documented as "called by
the kernel adapter before viewer-critical open/read callbacks", defined — and
called by nothing but tests. Reads through the mount open with
`FrameType::loader` unconditionally, and the operator confirmed that is correct:
a mount is a convenience and an import path, not a viewer, and the viewer
priority it would claim belongs to real playback. The hook is gone, the policy
is written down where the class is declared, and the tests that used it to
simulate viewer pressure now drive `FileSystem::note_foreground_activity()`
directly, which is what the HTTP playback path actually does.

**`MANIFEST.sha256` is deleted.** 104 of its hashes no longer matched, nothing
in the build referenced it, and its only remaining function was to misinform
anyone who ran `shasum -c` against it.

The version was opened because 0.39.1 removed a field from a
response that clients poll — `diagnostics` is no longer on `/api/v1/status` —
and that is a breaking, client-visible change which a patch number understated.
Pre-1.0, it earns the minor. The next piece of work starts from this number.

**The cluster runs 0.39.1, and that is correct rather than drift.** No code
sits between the two versions. Redeploying purely so the nodes report the new
string is cosmetic, and a rolling restart costs a viewer interruption, so it
was deliberately not done.

Where the 0.39 line ended up, deployed to gbni-1 and es-1 and verified live:

- `/api/v1/health` — unauthenticated liveness, and the contract every client
  now probes.
- `/api/v1/status` — `view_status`, and lightweight: no diagnostics tree.
- `/api/v1/status/diagnostics` — the same tree as before, on its own route.
- `anonymous` — no password, cannot be given one, and a roles-less anonymous
  account mints a session that grants nothing rather than being reported as
  anonymous access switched off.
- A torn pack tail truncates instead of taking an 8 TB backend offline.

gbni-2 has none of it. It is four releases behind, its sshd offers password
authentication only, and it needs console access.

## 0.39.1 — Polling Status no longer pays for diagnostics (development)

`/api/v1/status` computed and returned the whole `diagnostics` tree on every
call. Clients poll that route. Measured against the live cluster, diagnostics
was **10,091 of 14,914 bytes — 68% of the payload** — but the bytes are the
small half of the problem. Reaching those numbers means touching most of the
node's subsystems: the RPC client and the RPC server, the storage pool, the
retained-memory ledger, the data-resource arbiter, the metadata replica, the
FUSE frontend, the user table. Each has its own lock, and several of those
locks are held by precisely the busy paths that make someone reach for Status
in the first place. A poll had no business taking any of them.

- **`GET /api/v1/status` is now the lightweight view**: `cluster`, `nodes`,
  `startup`, `connectivity`, `subsystems`. Membership, telemetry and readiness
  the node already holds decoded — a handful of short mutexes, no I/O, no
  network, and not one diagnostics lock.
- **`GET /api/v1/status/diagnostics` is the expensive half**, unchanged in
  content and shape (`{"diagnostics": {...}, "generated_at_unix_ms": ...}`).
  Same `view_status` role as the rest of the status tree.
- The lightweight response carries **`diagnostics_endpoint`** naming that
  route. A client that was reading `diagnostics` from `/api/v1/status` would
  otherwise get `undefined` and no explanation — the silent-nothing failure
  this project has been bitten by before — so the pointer travels with the
  payload rather than living only in this file.

This is also an experiment with a result either way. If Status is still
occasionally slow now that a poll touches none of those locks, the cause is
not inside the handler, and the next place to look is the HTTP worker pool:
16 workers, a 15 s keep-alive idle timeout, and a backlog check that only runs
*between* requests rather than while a worker is blocked waiting for the next
one on an idle connection.

- Tests: `test_status_is_light_and_diagnostics_have_their_own_route` asserts
  the split by what the polled route *touches* rather than by how big it is —
  no diagnostics-owned field is reachable through it — and that the new route
  is not swallowed by the `/api/v1/status/nodes/` prefix beside it. Four
  existing tests moved their diagnostics assertions to the new route via a
  shared `status_diagnostics_response()` helper.

## 0.39.0 — A line under the 0.38 series (development)

No code change. 0.38.3–0.38.5 were written, deployed and verified against the
live cluster in one sitting, and they are coherent enough to name as a series:
**a node no longer loses a disk, an account, or its own diagnostic screen to a
question nobody had thought to ask it.**

- **0.38.3** — one torn 125-byte pack tail no longer takes an 8 TB backend
  offline. Found live: gbni-1 had been advertising 0 G while holding 685 GiB.
- **0.38.4** — `anonymous` has no password and cannot be given one, closing a
  path by which any unauthenticated visitor could mint themselves a credentialed
  session that outlived `allow_anonymous: false`. An anonymous account with no
  roles now mints a session that grants nothing, instead of being reported as
  anonymous access being switched off.
- **0.38.5** — cluster health became a capability (`view_status`) rather than an
  ungated route, and liveness got a route of its own (`/api/v1/health`).

Deployed to gbni-1 and es-1. **gbni-2 is stranded on 0.38.1** — its sshd offers
password authentication only and needs console access — so it carries none of
the above and is the one node where `/api/v1/status` is ungated, `anonymous`
can still be given a password, and `/api/v1/health` 404s.

What this line is drawn *for*: the next piece of work is splitting the
lightweight status view from the expensive diagnostics tree, which changes the
shape of a response every client polls, and that is better started from a named
boundary than from the middle of a run.

## 0.38.5 — Cluster health is a capability, and liveness is its own route (development)

`/api/v1/status` carried no role. The argument for that was sound as far as it
went — an importer watching an ingest is the person who most needs to know
whether the cluster is healthy, and gating the diagnostic screen behind
`manager` takes it away at exactly the moment it earns its place. What it could
not express is the other case: a session the cluster granted *nothing* — a
roles-less `anonymous` session, which 0.38.4 made a legitimate and useful state
— was still shown the node roster, every node's capacity and usage, the
metadata generation, subsystem states and the whole diagnostics tree.

**New role `view_status`**, and the status routes now require it. The old
argument is kept as an implication rather than as an absent gate: every
capability implies `view_status`, exactly as every capability already implied
`media_viewer`, so no account that could see health before loses it. It is the
weakest capability — implied by everything, implying nothing — so granting it
alone is how an operator makes cluster health public without handing out media.
An account granted nothing has it too: nothing.

- `GET /api/v1/status` and `/api/v1/status/*` require `view_status`.
  `POST .../connectivity/check` requires `manager`: it is not a read, it makes
  this node dial every peer on the caller's say-so.
- **Implications are now resolved at mint, not only at write.** `verify()` and
  the anonymous mint path run `expand_roles()` over the stored set, so an
  account written before `view_status` existed gets it on its next login with
  no migration and no rewrite of the user table. Without this, upgrading would
  have taken Status away from every existing account until each was edited by
  hand.
- Genesis `root` gains it (`all_roles()`), and it is grantable through the
  users API like any other role.

**New route `GET /api/v1/health`**, unauthenticated and role-free, for the
things that were reaching for `/api/v1/status` to answer a question it was
never the right route for: is this node serving. `200 {"status":"ok"}` when it
is, `503 {"status":"starting"}` or `{"status":"failed"}` when it is not — the
HTTP status carries the same answer as the body, so a probe that parses nothing
still works. It deliberately reports nothing else: it is reachable wherever the
API is reachable, by anyone, so it carries no version, no node identity and no
topology. Anything beyond liveness is a question about the cluster and needs
the capability that says so.

One ordering bug fixed in passing: `handle_http` dispatched Status *before* the
role gate ran, so a `required_role()` entry for it would have been unreachable
had one existed. Status now dispatches after the gate, and still ahead of the
services-ready check — a node that is still recovering is exactly when it is
asked what is wrong.

- Tests: `test_status_needs_view_status_and_health_needs_nothing` (roles-less
  session refused with the role named, `view_status` alone admitted to status
  and refused media, an importer still admitted, and health answered with no
  token and nothing in it), `test_role_implications_reach_accounts_written_before_them`
  (a record rewritten the way a pre-0.38.5 node stored it still gains the role
  at mint, with no other widening), and the role-implication cases in
  `test_roles_are_capabilities_not_a_ladder`.
- **Clients must stop using `/api/v1/status` as a health check.** It now needs
  a session *and* a role; `/api/v1/health` needs neither.

## 0.38.4 — Anonymous has no password, and no roles is not the same as disabled (development)

Two things about the `anonymous` account were wrong, and they were the same
mistake seen from different sides: treating it as an ordinary account that
happens to be reached unusually, rather than as the one account that is only
ever reached without credentials.

**Anonymous can no longer be logged into, and has no password to log in with.**
`session.allow_anonymous: false` guards the no-credentials mint path only. The
username/password path never consulted it, so anyone who knew the anonymous
account's password could `POST /api/v1/session` as `anonymous` and receive a
session carrying its roles **with anonymous access switched off** — and that
session was an ordinary bound one, so it outlived the switch.

Nobody knew that password, because genesis generated a random one and told no
one. But the API invited an admin to set one (`set_password` was advertised
true for anonymous, with a comment calling it harmless), and worse:
`/api/v1/users/me` requires only `media_viewer`, which anonymous holds at
genesis, and a self `PATCH` carrying a password set the credential and handed
back a fresh token. **Any unauthenticated visitor could give the anonymous
account a password of their choosing and then log in as it.** That is the hole;
the pointless random password was the thing that made it look harmless.

So anonymous now has no credential at all — `kdf` 0, no salt, no hash, via the
new `UserStore::create_without_password` — and:

- `UserStore::verify` refuses the `anonymous` username outright, through the
  same dummy-KDF path as an unknown user so the refusal is not distinguishable
  by timing. This is what makes the credential a cluster created before 0.38.4
  already carries inert, with no migration and no write: the record can keep
  its old hash, and nothing will ever reach it.
- `UserStore::update` refuses a password change on anonymous, so the API, the
  CLI and any future caller are all covered by one rule in one place — the
  same discipline as the last-user-manager check beside it.
- `UsersApi` returns `409 no_password` and reports `mutable.set_password:
  false`, so a client does not draw a field the server will refuse.
- `macha-users passwd anonymous` refuses with the reason rather than a bare
  failure from two layers down.

Roles stay entirely ordinary on anonymous: they are the only control over what
an unauthenticated visitor may do.

**An anonymous account with no roles now mints a session that grants nothing.**
It previously reported `403 anonymous_disabled` — the same answer as anonymous
access being switched off. Those are different states and a cluster may
legitimately be in either: "visitors may connect but may do nothing" is how a
registered-users-only deployment is expressed, and the client needs the empty
role list to know to put a login in front of the viewer. Conflating them told
the client "log in" when the truthful answer was "you are in, and this cluster
grants visitors nothing" — and since the client had no session at all, every
subsequent request 401'd and the symptom read as an unreachable cluster.
`disabled` now means `allow_anonymous: false`, or no anonymous account on this
node, and nothing else. Found by the web client session against gbni-1 after
the operator removed `media_viewer` from anonymous.

- Tests: `test_anonymous_has_no_password_and_cannot_be_given_one` (every
  password refused including the empty one, the store refusing an update, the
  `/users/me` escalation refused, the mutability flag), and
  `test_anonymous_with_no_roles_still_mints_a_powerless_session` (roles-less
  mint succeeds with `roles: []`, and only `allow_anonymous: false` yields
  `disabled`).
- Also removes a dead sealing helper in `tools/users_admin.cpp` that every
  mutation has routed through `UserStore` since 0.38.0.

## 0.38.3 — A bad pack record no longer takes the backend offline (development)

gbni-1 ran for a day advertising 0 G of storage. Its 8 TB DATA backend had
gone offline at start-up over one record in one 29 MB pack:

```
WARN storage backend offline /mnt/diskB: corrupt pack header at
     .../pack-00000000000000001206.pack offset=29841717
INFO node data storage ready used=0 capacity=0
```

Exactly 125 bytes — one pack header's length — followed the last good record,
and they did not decode. That is what a power loss leaves when it lands
between the pack `write()` and the durability domain's `syncfs`: ext4 commits
the file's new length while the block that was to hold the header is
zero-filled or partial. Nothing past that point was ever acknowledged, so the
tail holds no data the cluster was told it had. Recovery handled two torn-tail
shapes — fewer bytes than a header, and a header whose payload runs past the
end — and truncated both; a header that was *present but undecodable* threw
instead, the `LocalStore` constructor failed, and the pool marked the backend
offline. The node then reported data storage *ready* with `capacity=0`, and
the node loop's periodic re-probe re-ran the same recovery into the same throw
every heartbeat, logging nothing after the first time. With es-1 also down,
replication 2 had one eligible target, which is why a client saw 42% of
artwork on both reachable nodes or neither.

Recovery now settles an undecodable header by looking for a decodable one
after it (magic match, then the header's own SHA-256), reading the rest of
that one pack in bounded chunks:

- **Nothing decodable follows:** a torn tail. Truncated and logged like the
  other two shapes (`truncated undecodable pack tail … zero_header=0|1`).
- **A decodable record follows:** damage inside the pack — bit rot, an
  external edit. Truncating would discard the live records behind it and
  refusing the pack would take the whole backend offline over one record.
  The unreadable span is skipped and counted as dead bytes for compaction,
  the records after it are indexed normally, and the loss is logged at
  `error` (`skipped unreadable pack region … bytes=N`). Objects recorded in
  the span are absent from this backend and are repaired from replicas; a
  tombstone lost there can resurrect an earlier record of the same object
  until GC reaches it.

Neither shape takes the backend offline any more. A real I/O error while
reading a pack still does, and the existing re-probe brings the backend back
once the disk answers again.

- Status: `diagnostics.data_store.{pack_recovery_truncated_tails,
  pack_recovery_skipped_regions, pack_recovery_skipped_bytes}`, summed across
  online backends; non-zero skipped figures mean this node lost objects it
  once held.
- Tests: `test_pack_recovery_truncates_undecodable_header_at_tail` (a
  zero-filled header, a garbage header, and a garbage header with a partial
  payload — all truncate to the intact boundary and the pack stays writable),
  `test_pack_recovery_skips_unreadable_region_before_live_records` (a flipped
  checksum byte in the middle record: nothing truncated, neighbours live, the
  lost object re-storable, compaction reclaims the span).
- Nothing was done to gbni-1's pack by hand. Deploying this release and
  restarting the node is the repair: recovery truncates the tail itself.

## 0.38.2 — libtorrent's port mapping is stated, not assumed (development)

libtorrent maps its own listen port with UPnP and NAT-PMP, and both default to
on inside libtorrent. Macha set `enable_dht` and `enable_lsd` from
configuration but never touched these, so the session created router mappings
regardless of what the rest of the configuration said: a node with
`network.upnp.enabled: false` still had libtorrent mapping 6881, which no
setting mentioned and nothing could refuse.

New `torrent.upnp` and `torrent.natpmp`, both defaulting to true — which is
what libtorrent was doing anyway, so no node changes behaviour. The point is
that it is now refusable, and visible to anyone reading the configuration.

They are deliberately separate from `network.upnp`, which maps the cluster RPC
port through Macha's own miniupnpc client. The two map different ports for
different reasons and an operator may reasonably want one without the other;
inheriting would have silently changed what a node does on upgrade.

Found while diagnosing torrents that loaded and then froze. The freeze itself
was `torrent_listen_interfaces()` handing libtorrent the node's advertised
address after that address became a public DNS name: `listen_interfaces` takes
an IP or a device, never a hostname, so the session bound nothing at all and
sat in `dl metadata` for ever with no error — the same silence 0.37.2 was
written to remove, from a new cause. Two nodes were unwedged by setting
`torrent.listen_interfaces` explicitly; the derivation itself is not yet
fixed.

## 0.38.1 — Version bump (development)

No functional change since 0.38.0. The version is incremented so the build
running on the cluster is distinguishable from the one 0.38.0 first described,
which matters while nodes are being rolled forward at different times.

## 0.38.0 — Cluster users, passwords and roles; login works on a node that is alone (development)

Macha had sessions but no people. Every session carried a `roles` list and a
`session_has_role()` helper that nothing called, so one bearer token granted
catalogue reads, media deletion, namespace deletion and cluster
identity-association reset alike, and `manage_api.cpp` advertised
`"privileged": false` while gating deletion behind the same token as reads.
This release adds the accounts, and gates the routes on what those accounts
may do.

The design constraint that shaped everything: **logging in must not depend on
the cluster being healthy.** Users, passwords and roles are cluster-replicated
state, but verifying a password reads only this node's memory and its own
disk -- no RPC, no metadata, no catalogue. A node whose metadata has gone
`read_only`, or which is temporarily alone, still authenticates every account
it knows about. That is deliberate: the subsystem most likely to be sick when
you need to log in must not be in the login path. Putting users in the
metadata layer was considered and rejected for exactly this reason.

**Two accounts exist from the moment a cluster is founded.** The node with no
bootstrap peers -- the same test `MetadataReplica` already uses to decide
whether its genesis record is authority -- creates `root` (every role) and
`anonymous` (`media_viewer`) on first start and never again. root's generated
password is written to `<state_path>/genesis-root-password`, mode 0600, and the
log says where it is rather than what it is: a log line is shipped, rotated and
read by more people than that file is. Neither account can be renamed or
deleted; everything else about them is ordinary. The check is "the table has
never held anything", tombstones included, so deleting root does not cause the
next restart to mint a new one with full privileges.

**No recovery key, and that is a decision.** A key would have to be
presentable without an account to be useful, which means a standing
unauthenticated path to the most privileged account in the cluster, on a
surface that includes an offsite node -- bought with a capability that already
exists behind strictly more access, since the only party who could present one
is the operator, who has root on a node and can run `macha-users passwd root`.
The machinery (an X25519 envelope sealing the cluster key, so a key could be
checked without anything derived from it being stored) is implemented and
tested but unused: the design is sound for a model where the operator cannot
get a shell, and that model does not exist here. `macha-recover` ships, says
so, and points at the command that does work.

**At least one account always holds `manage_users`.** Removing the role from
the last account that has it, or deleting that account, is refused with its own
error code -- root included, whose roles are otherwise ordinary. The rule is
about the role rather than any particular account, so it moves as the role
moves, and it is enforced in `UserStore` rather than only at the API so no
second caller can route around it. A client is told which roles are pinned to
an account rather than being handed a blanket "roles are read-only": the rest
stay editable.

**Anonymous is a real account, not a special case in the auth path.** A session
minted with no credentials is bound to that user and carries its current roles,
so changing what an unauthenticated visitor may do is an ordinary edit of an
ordinary account, effective on the next session rather than on restart. This
matters for televisions, which have no practical way to type a password:
whatever the anonymous account holds is what a TV can reach. `allow_anonymous:
false` turns the mechanism off entirely.

**Roles are capabilities, not a ladder.** Importing does not imply managing,
and managing does not imply handing out accounts. Every role implies
`media_viewer`, and that is the only implication:

    media_viewer  read all media, playback, cluster status
    importer      acquire content (torrents, ingest)
    manager       files, namespaces, catalogue matches, identity-association reset
    manage_users  add, edit and remove accounts

They are resolved once when a session is minted, so a route gate is a single
lookup on the session the caller already presented, checked in one place before
dispatch rather than per handler -- a check a new route can forget to add is
not a gate. `POST /api/v1/session` and `GET /api/v1/session`/`/api/v1/status`
are the exceptions: the first needs no token at all (it is how you get one) and
the last two need a token but no role, so that a client can ask "is my session
live and is this node up" for any account, including one that holds only
`manage_users`.

Three things in the session subsystem turned out to be broken, and all three
are fixed here because this work depends on them:

- **Session gossip had never once run.** `session_sync` was broadcast on
  `FrameType::speculative`, but `class_allowed()` permits a non-control frame
  type only for bulk or priority-data messages and `session_sync` was on
  neither list, so every gossip tick since 0.24.0 threw "message used an
  invalid frame type" into a `catch` that logs at debug. The receiving side was
  equally dead: both inbound readers dispatched only `telemetry` as a
  notification and silently discarded everything else with a zero request id.
  Session replication had been relying entirely on the synchronous push, with
  no working backstop at all.
- **Login blocked on unreachable peers.** `propagate_session` called every peer
  membership still considered active, serially, each to
  `control_no_progress_deadline` (30 s). One peer that was up but not answering
  -- the wireless node, mid-blip -- stalled every login by that long, which is
  precisely the situation in which you need to log in. It is now a best-effort
  notify that queues on open connections and returns; the local merge has
  already happened and the gossip tick is the backstop, so there was never
  anything to wait for.
- **Gossip competed with data work for memory, and could deadlock it.**
  `FrameType::speculative` maps to `MemoryClass::speculative`, the same budget
  loaders and viewers draw from, and both the send (`try_notify`) and the
  receive (`RpcServer::admit_locked`) take a lease from it. Adding session and
  user gossip to the one pre-existing speculative sender (telemetry) was enough
  to starve a node with a tight data budget: on an aarch64 node a loader
  acquire blocked in `DataResourceArbiter::acquire` and never returned. Session
  and user gossip now ride `FrameType::control`, which has its own reserve and
  cannot take memory data work needs. The cost, stated because it is real, is
  that gossip is written ahead of viewer traffic -- acceptable only because
  these payloads are a few hundred bytes and are sent solely when something
  changed. Telemetry deliberately still rides speculative.

  Relatedly, gossip no longer re-announces unchanged state on every tick: every
  inbound notification costs a real RPC admission slot on every peer, so both
  subsystems send when the set changed or a peer appears that has not been
  told -- tracked by peer id, not by count, since a count changes on every
  membership flap -- and otherwise only every 30 s. That interval is the
  guarantee, not a fallback: `broadcast_best_effort()` reports how many frames
  it QUEUED, not how many were delivered and applied, so a peer whose inbound
  route is not usable yet (precisely the state a peer is in while it restarts)
  can be marked told having received nothing. Found during this release's own
  rollout, where an upgraded node came up with an empty user table and refused
  every request until the sending node happened to restart.

**Upgrading an existing cluster: upgrade every node promptly.** A session
minted by a pre-0.38 node carries `roles: ["anonymous"]`, and `anonymous` is no
longer a role -- it is an account. An upgraded node therefore refuses such a
session with 403 on every route. The session wire format is compatible across
the upgrade (a payload of purely anonymous sessions still encodes as
`MACHSES1`), but the role semantics are not, so a client that obtains a session
from an old node and presents it to a new one is refused until the whole
cluster is on 0.38.0. There is no compatibility shim for this today.

Also here: `AuthSession` gains `user_id` and `credential_generation`, so a
password change or a deletion retires every session it minted, on every node,
by replicating one record rather than enumerating sessions. A role change does
the same, because a session carries the roles it was minted with and a demotion
that left them alive would not take effect until they expired. The session wire
format picks itself per payload -- a payload of purely anonymous sessions still
encodes as `MACHSES1`, byte-for-byte what 0.37.x emits -- so a pre-0.38 peer
keeps merging sessions across a rolling upgrade.

Passwords are scrypt (N=2^15, r=8, p=1) with the parameters stored per record,
so they can be raised later without invalidating anyone. The users file is
sealed at rest under an HKDF subkey, because these hashes replicate to every
node including one that is physically offsite. `macha-users` administers the
table offline for bootstrap and recovery; it reads passwords from the terminal,
never from argv.

**A DATA credit wait can no longer hang for ever.** `DataResourceArbiter::acquire`
with no caller deadline waited unconditionally, so a caller holding credit
while acquiring more hung silently and permanently rather than failing. It now
waits in no-progress windows: any release anywhere resets the window, so a
waiter behind genuine work still waits as long as it takes, and only a wholly
stalled arbiter gives up -- logging the class, the byte size, and the used /
active / waiting counts, instead of returning an indistinguishable empty
optional. New `dht.data_credit_no_progress_deadline_ms`, 120 s, 0 restores the
old unbounded wait.

This surfaced because `test_storage_data_credit_reserves_viewer_headroom_and_control`
deadlocked for 360 s on every four-core node while passing on a twelve-core
development machine: `maintenance.background_concurrency` defaults to
`hardware_concurrency() / 2`, and the test's three loader acquires cannot all
be admitted below six cores. The test now states the ceiling it means to test
rather than inheriting one from the host -- a test that depends on the machine
it runs on is not a test.

Unrelated but found while checking a client's assumptions:
`catalogue.api.advertised_endpoint` was accepted without validation, while the
shipped example config has always said a path is "rejected at startup". It now
is, along with a missing scheme, an unbracketed IPv6 literal, and credentials
or a query in the authority. That string is handed to clients verbatim and
every request URL is built from it, so a malformed one used to fail somewhere
far away with no trace of where it came from.

New: `session.allow_anonymous`, `session.max_users`,
`session.max_concurrent_password_checks`, `session.failed_login_attempts`,
`session.failed_login_lockout_ms`. `GET /api/v1/status` reports the user count,
tombstones and a table hash, so cross-node convergence is visible the way
`metadata_generation` is.

## 0.37.2 — The torrent engine binds a routable interface, and says so when it cannot (development)

Two magnets sat in `metadata` on gbni-2 for hours with `peers: 0`, `seeds: 0`
and an empty `error`. Nothing was failing; there was simply no socket that
could reach anything. libtorrent's default `listen_interfaces`
(`0.0.0.0:port,[::]:port`) is expanded by its own device enumeration, and on
these nodes that binds `eth0` and loopback but never `wlan0`. gbni-2's `eth0`
is `NO-CARRIER` — it is the wireless node — so its session held `127.0.0.1:6881`
and `[::1]:6881` and nothing else. es-1 and gbni-1 were unaffected only because
their `eth0` is live. A restart did not help: the binding is deterministic, not
a startup race.

- The engine now binds the node's advertised address, which is correct
  whichever device carries it and is already per-node correct in
  configuration. `torrent.listen_interfaces` overrides it in libtorrent's own
  syntax (a device name such as `wlan0:6881` is accepted); `torrent.listen_port`
  defaults to 6881 and is validated nonzero. The decision is
  `torrent_listen_interfaces()` in `macha_core`, not the plugin, so it is
  testable without libtorrent.
- **The plugin never consumed libtorrent alerts at all** — no `pop_alerts`, no
  alert mask, anywhere. A session that bound nothing usable, failed to
  bootstrap DHT, or was refused by every tracker reported precisely nothing:
  the journal held one "plugin loaded" line and the API's `error` field stayed
  empty. `drain_alerts()` now logs listen success/failure, DHT bootstrap and
  port mapping, with peer and tracker churn at debug so it cannot bury them.
  `set_alert_notify` wakes the worker, so a settled manager still drains.
- A session holding only loopback sockets is reported once, plainly, as a
  configuration fault. It is a static property known at startup and it
  invalidates every job on the node at once — the difference between "this will
  never work" and "this is slow", which nothing previously distinguished.

## 0.37.1 — A publication with a stale basis replays instead of retrying forever (development)

Two hours after 0.37.0 went out, gbni-1 had one inode that had failed **68
consecutive times** and could never succeed. `parked_publications` read 0,
`cluster.health` read `healthy`, and the only evidence was a DEBUG line every
30 seconds. rsync `--append-verify` had appended to a file whose publication
was still in flight; the entry advanced underneath the writer, and from then on
every retry re-ran an identical, doomed comparison. Three separate defects had
to line up, and all three are fixed here.

- **A stale basis is now replayable, not retryable.** `commit_file` rejects a
  commit when the entry has moved past the basis the handle captured
  (`filesystem.cpp:2248`). For a publication that rejection is permanent — the
  retry keeps the same writer — so it now reports `ESTALE`, which the frontend
  already handles by dropping the writer and replaying the generation from the
  spool against current state. Foreground handles keep `EAGAIN`: they stay
  open, the content genuinely did change under them, and retrying is
  meaningful. Kernel-facing semantics are unchanged.
- **The park budget was arithmetically unreachable.** The density rule ("more
  than N failures inside the window") cannot fire once backoff caps, because a
  window only ever holds `failure_window / max_backoff` attempts. The shipped
  publication policy was 30 min / 30 s = 60 possible attempts against a
  threshold of 100, so a permanently failing file retried forever by
  construction. `RetryPolicy` gains `max_failing_duration` (default 1 h): an
  item that has not succeeded once within it parks, however sparsely it is
  retried, measured over the current unbroken run rather than its whole
  history. This is deliberately *not* `failure_window` — that answers "is this
  flapping?", this answers "is this ever going to work?", and tying them
  together would park every publication whenever the wireless node or the WAN
  link is out for longer than the flap window. Settable per policy from YAML.
- **A long failure run is now visible.** Crossing ten consecutive failures logs
  WARN with inode, path, run length and error, repeating every twenty
  thereafter, and increments `publications_retrying_persistently` — reported on
  `diagnostics.filesystem`. Non-zero means a file is failing repeatedly but has
  not yet exhausted its budget: the state that was previously invisible.

The retry-budget test drives `RetryState::failed()` with simulated time and
asserts the pre-fix policy survives 400 failures across 3.3 simulated hours
without parking, which is the hole itself. The stale-basis test was confirmed
to fail against the old `commit_file`.

## 0.37.0 — Background metadata repair no longer wedges the node's writes; ingest runs concurrently (development)

On 2026-09-10 es-1 had six torrent ingests all reading `queued`, nothing
running, staging at 3.78 GB of 500 GB, and `cluster.health` saying `healthy`.
A backtrace showed the ingest worker inside `WriteHandle::commit` blocked on
`MetadataManager::mutation_mutex_`, together with nine other threads. The
holder was the maintenance thread: `repair_once()` took that mutex and then,
still holding it, replicated the accepted head to every peer — a blocking RPC
per history hash per peer. One peer that stopped answering turned a background
convergence pass into a stall of every local metadata mutation.

- `repair_once()` releases `mutation_mutex_` across the replication fan-out.
  Replication is idempotent and additive by the reconciliation contract
  ("convergence is replication, not head replacement"), so it needs none of the
  serialisation that discovery, head selection and the baseline commit do.
  After re-taking the lock the pass re-validates that the head it selected is
  still current and abandons itself otherwise, so the baseline commit can never
  build generation+1 on a superseded parent. Discovery and `publish_commit`
  still RPC under the lock — a write *is* its replication — and those calls are
  bounded by the control no-progress deadline.
- Ingest was strictly serial: one worker, `process_job()` to completion before
  the next. It now runs a pool bounded by `ingest.max_concurrent_jobs`
  (default 10, 1..64, applied at start). Jobs are claimed under the lock that
  selects them; catalogue-completion polling has its own thread so busy
  importers cannot starve it; show/pause/resume/cancel/clear are unchanged.
  `GET /api/v1/ingest/status` gains `concurrency.{max_jobs,active_jobs,
  peak_active_jobs}` — without it a queue stalled behind one wedged job is
  indistinguishable from an idle one.
- `WriteHandle::drain_one_extent` could wait forever: `put_impl` spilled a
  silent replica after `write_stall` and sought a replacement, but the spilled
  put still counted as unfinished, so with no replacement available the loop
  spun at 1 ms indefinitely. The extent put now carries the pipeline's
  `DataWorkContext` and fails — retryably, into the existing backoff-and-park
  discipline — once nothing in the pipeline has moved for the no-progress
  budget. Slow-but-moving transfers re-arm the window; only a put where nothing
  at all advances fails.
- A test-only silent-peer fixture, `RpcClient::stall_peer_for_tests`
  (all messages or one type), holds outbound calls unresolved with
  `idle_for()` advancing as it would on a dead link. This is the fault-injection
  hook the backlog recorded as missing; both fixes above are gated by it, and
  each regression was confirmed to fail against the pre-fix code.
- `test_concurrent_reads_during_divergence_produce_one_reconciliation` was
  racing the maintenance loop through the 250 ms metadata cache (a pass between
  the two sibling commits cached one sibling; every reader returned it
  unmerged). It now runs with no cache TTL and measures what its name says.

## 0.36.9 — Bound how many files may hold a publication writer at once (development)

es-1 held 515,899,392 bytes of publication-owned retained memory for hours,
byte-identical across restarts, at 99.7% of the durable-lower budget, with 137
publications started and none completed. No budget setting moved it: lowering
`fuse.publication_inflight_bytes` from 256M to 96M changed the inflight figure
and left `owners.publication` unchanged, because that setting bounds queue
admission, not writer buffers.

Every byte of `owners.publication` is a `WriteHandle` extent lease. A
publication writer is deliberately retained across clean yields and retryable
failures so a resumed publication never replays spool bytes, and it keeps
those leases for as long as it is retained: one extent buffer being filled,
plus the pipeline. A yield that does not land on an extent boundary leaves one
behind, and publication scheduling is otherwise breadth-first, so the number of
writers holding partial state is simply the width of the backlog. At a 4 MiB
extent, 492 MiB is 123 such leases — against three running workers that can
hold at most three each. Then every writer needed one more extent and none
could release one: hold-and-wait. Any budget fills the same way, which is why
no budget helped.

- **`fuse.publication_max_open_writers`** bounds how many inodes may hold a
  writer, derived so all of them can hold their worst case inside
  `runtime.loader_memory_reserve_bytes` — `loader_reserve / (extent_size +
  publication_pipeline_bytes)`, never below `commit_workers`. A writer waiting
  on the ledger is then only ever waiting for control/viewer work, which
  releases. Past the bound the scheduler is depth-first over the already-open
  set, which is what drains a backlog anyway.
- **The no-progress deadline watched the wrong counter.**
  `data_publication_quanta` increments when a quantum is *admitted*, and on a
  wedged node a failure frees a slot which admits the next file — so every
  failure re-armed every other waiter's window, and the deadline serialised
  into one failure per budget instead of failing every stuck worker. That is
  the "different inode every 30 s, always `attempts=1`" log shape. It now
  watches extent retirements and commits, the events that actually release
  publication memory, reported as `data_publication_progress_events`.
- **The deferred-retry due time now also arms the timer on the non-empty-queue
  path.** Before the bound a non-empty queue always had a running worker to
  notify it; with the bound the queue can be full of inodes that are not
  admissible while the only inodes that could release a writer are backed off.
- Status gains `open_publications`, `peak_open_publications`,
  `publication_max_open_writers`,
  `data_publication_selections_under_writer_cap` and
  `data_publication_progress_events`.

Measured on one backlog, unbounded against bounded at 4: 20 writers open at
once filling 15.76M of a 16M durable-lower budget, 18 retryable failures and
25.1 s wall clock, against 4 writers open, no failures and 6.8 s. On the live
cluster the 8.59 GB spool that had not moved in hours drained completely, every
byte confirmed, with no failures and nothing parked.

Not fixed here and still live: `WriteHandle::drain_one_extent` waits on its
extent future with no deadline and no cancellation check, so a stalled put
blocks publication silently — the same shape one layer down.

## 0.36.8 — A wedged publication pipeline can fail, retry and park (development)

Two defects left a node publishing nothing for hours while reporting itself
healthy, with `parked_publications` reading 0 and the spool draining at 0 B/s.

**Reassembly starvation.** `MessageAssembler` could not get a retained-memory
lease to reassemble an inbound frame, threw `process retained-memory RPC
reassembly saturated` and killed the channel, about once a second. Every peer
channel died 1–2 s after connecting, so requests re-dialled constantly and
telemetry — the only consumer that never dials — appeared to vanish, which is
why this first looked like a network fault. The loop is closed: publication
holds its bytes until a peer confirms the write, that confirmation arrives as a
frame which must be reassembled into the same ledger, and the reassembly is
refused because publication is waiting.

`runtime.reassembly_memory_reserve_bytes` (32 MB) gives reassembly a small
dedicated slice. Its placement is load-bearing in both directions: below the
control/viewer waiter gate, because a queued viewer outranks reassembly
unconditionally; above the loader gate and the durable-lower budget, because
those are what it deadlocks against. An earlier attempt gave reassembly
priority over every gate and simply inverted the deadlock, starving
publication on a node receiving from two peers.

**Publication waited with no deadline at all.** The publication
`DataWorkContext` was built without one, so the wait took the unbounded
`cv_.wait` branch and all eight commit workers sat in `ensure_buffer_memory`
holding 492 MB between them. Nothing ever failed, so the 0.30.0
retry-and-park discipline could not see it.
`fuse.publication_no_progress_deadline_ms` (30 s) is a no-progress budget, not
a time limit on publishing: progress anywhere re-arms it, so a slow node is
never failed for being slow, while a pipeline where nothing advances at all
fails with `EAGAIN` and enters the ordinary retry/park path. Zero restores the
old unbounded wait.

Measured before → after: saturation events ~1/s → 0; canonical connections
oscillating 0↔2 → stable; peer telemetry age climbing past 700 s → 1.2–4.7 s.

## 0.36.7 — A node stays visible while it is busy, and two title parsers stop lying (development)

Three unrelated faults found the same day, all of them things the operator
could see and the software could not explain.

**Peer telemetry was blanked rather than labelled.** `node_json()` populated
`runtime` only for a sample fresher than `max(heartbeat * 3, 5s)` — 15s here —
so one sample crossing that line removed `uptime_ms`, `rss_bytes`, `load1`,
`process_cpu_percent`, `cpu_cores`, `memory_total_bytes`, `peers_*` and
`rpc_connections_*` in a single step, leaving `"runtime": {}`. On the live
cluster every WAN pair was in exactly that state, with samples ~4.9 minutes
old. Storage and cache figures still go unavailable when stale, because those
are what a consumer sums into a cluster total and a stale one would be a
fabricated number; the runtime figures are honest measurements of the sending
process at a stated instant, and `telemetry_freshness` and `live_age_ms`
already say how old they are.

**A healthy peer could report `metadata_generation: 0`.** The emitter
preferred the membership record unconditionally, so a membership entry
carrying 0 beat fresh telemetry carrying the real generation. Both are
sightings of the same monotonic counter, so it now takes the larger and falls
back to the durable value only when neither source has one.

**Telemetry gossip went quiet exactly when it mattered.** It was sent only
after 2s free of foreground *and* read-ahead work, and then only onto an
entirely idle writer, so a node became invisible to its peers while it was
busy or in trouble. Both gates are gone: gossip runs every tick, and a small
notification (at most 64 KiB, while the writer holds under 1 MiB pending) may
queue behind existing work. It stays on the SPECULATIVE class and the writer
still picks the most urgent frame first, so none of this can delay operational
RPC. The cadence is now `network.telemetry_interval_ms`, default 10s, and a
demand-driven wake still publishes sooner but never more than once a second —
without that floor the loop had no minimum spacing at all, since its wait
returns whenever a peer observation moves the demand counter.

**"Blade Runner 2049" was catalogued as "Blade Runner", and rejected.** Any
`19xx`/`20xx` in a filename was read as the release year, so the search title
was truncated and the only candidate TMDB returned — the 1982 film — failed
the year comparison. A number the calendar has not reached is title text, and
both year scanners now say so; `strip_release_noise` also learned `hdrip`,
`xvid`, `divx` and `brrip`, without which the title kept its release tags.

**Music matched nothing when the filename decorated the title.** All 61
unmatched music files reported "no metadata provider match after 4
candidates": `(feat. …)`, `(Live)`, `(Spotify Bonus Tracks)` and the like went
into the MusicBrainz query verbatim and returned no results at all, and the
acceptance threshold of 120 needs an exact agreement on both title and artist
— which the provider's own credit style ("Avicii feat. Sandro Cavazza" against
a path saying "Avicii") denied. The decorated title is still tried first,
since a remix or live cut is a genuinely distinct recording; an undecorated
search is the fallback when the precise one finds nothing, scoring 85 against
an exact 100, and a primary-artist agreement scores 70. A title-only
agreement still lands at 95 and is still refused.

## 0.36.6 — Status reports how much RAM a node has (development)

`runtime.memory_total_bytes` joins `cpu_cores` in the node payload. The
closest existing field was the wrong quantity by orders of magnitude:
`rss_bytes` is this process's own resident set, a few hundred megabytes on a
machine with tens of gigabytes, and under a label reading "memory" it would
have been plausibly and badly wrong.

- **Total, not available.** On Linux "available" is dominated by page cache,
  so a node that has just served a large file looks starved while being
  perfectly healthy — and serving large files is the entire workload here.
- **Display only.** Nothing schedules or ranks on it, deliberately: total RAM
  would prefer a large thrashing node over a small idle one.
- Omitted when it cannot be determined, so a consumer renders "unknown"
  rather than a node claiming to have no memory — the same rule `cpu_cores`
  follows.
- An optional trailing telemetry field, so a node that has not been upgraded
  simply does not report one and needs no coordination during a rolling
  upgrade.

## 0.36.5 — Coverage that measures something (development)

`MACHA_TEST_COVERAGE` had been in `CMakeLists.txt` for some time and had never
produced a report. It could not: two independent faults, either of which alone
was enough.

- **It did not link.** `macha_core` was given the coverage compile flags but
  not the link flags, in both the GCC and the Clang branch, so the shared
  library referenced the profile runtime and never resolved it. That is why
  the repository contains no coverage artifact of any kind — not that nobody
  had bothered, but that nobody could.
- **It recorded nothing.** With linking fixed, the first full run reported
  **0.0% across every file the main suite touches, with 377 tests passing**.
  Every case runs in a forked child, and `child_run` leaves through
  `std::_Exit` — correctly, since that is what stops a forked child flushing
  buffers inherited from the parent — but counters are written by an `atexit`
  handler, so a child that never runs one records nothing at all. The children
  now reset counters on entry and dump them before `_Exit`, compiled in only
  under coverage. The reset matters as much as the dump: a child inherits the
  parent's accumulated counts at `fork()`, so without it every case would
  re-report the parent's startup as its own.

`./run-coverage.sh` builds instrumented, runs the suite and reports, taking
the same arguments as `run-tests.sh`. The two toolchains need entirely
different machinery and it selects per compiler rather than asking the
operator to: Clang writes a profile per process, read back with
`llvm-profdata`/`llvm-cov`; GCC writes `.gcda` counters beside the objects,
read back with `gcov`. Neither `lcov` nor `gcovr` is installed on the cluster
nodes, so the GCC summary is aggregated from plain `gcov` output rather than
depending on a tool that would have to be installed on a Pi first.

The build defaults to `Debug`, because at `-O3` inlining makes a coverage
report describe the optimiser's view rather than the code's, and case
deadlines scale 3x for the same reason the sanitizer build scales them.
Documented in `tests/TESTING.md`, which described `MACHA_SANITIZE` in detail
and had never mentioned coverage at all.

Verified on both toolchains. What it does not cover: the subsystem plugins,
since instrumentation here is a per-target property rather than the
whole-program one a sanitizer needs, so plugin-only code reads as uncovered
whether it is tested or not.

## 0.36.4 — One API endpoint, stated rather than guessed (development)

**Breaking, and not detectable by looking for a field.** `nodes[].api_host`
and `nodes[].api_port` are removed from `/api/v1/status` and replaced by
`nodes[].api_endpoint`, a complete URL. The absence of `api_host` is
indistinguishable from an old node that never reported it, which is why this
carries a version bump: clients cannot sniff for it.

The pair could not express a scheme, so a client discovering peers had to
invent one — `@machafoundation/core` hardcoded `http://`. That is wrong in both
directions on a TLS deployment: a browser on an HTTPS page blocks every
discovered peer as mixed content, and a native client sends plaintext to a
node the viewer deliberately put behind TLS.

- `catalogue.api.advertised_host` and `advertised_port` become
  `catalogue.api.advertised_endpoint`, a URL. `listen`/`port` are unchanged
  and still describe the inner bind. The two are independent on purpose: with
  a proxy terminating TLS in front of the API, Macha serves plain http on
  `listen`:`port` while clients must be told `https://host:443`, and neither
  the scheme nor the port of the outer address is derivable from the bind.
- A path is rejected at startup with an explicit error. Fronting a node at a
  subpath is not a supported deployment, and the failure it would otherwise
  produce is silent — a client treating the endpoint as an origin drops the
  path and 404s against a node that looks correctly configured, in the one
  deployment that has a proxy.
- Unset defaults to `http://` this node's resolved RPC advertise address and
  the bound API port, so a node with nothing in front of it needs no
  configuration. A bare IPv6 literal is bracketed, since an unbracketed one
  cannot be parsed back out of a URL.
- Telemetry from a node predating this puts a bare hostname in the field.
  That is not an endpoint, so anything without a scheme decodes as "not
  reported" rather than becoming a value a client would concatenate a guessed
  scheme onto.

`nodes[].host` and `nodes[].port` are untouched. They are the RPC address, a
separate plane that is never proxied, and they have a live consumer in the
identity-association reset.

## 0.36.3 — Status says how many cores a node has (development)

`load1` and `process_cpu_percent` are both per-core quantities, and this
cluster is deliberately non-uniform hardware. A `load1` of 2.67 is a
struggling two-core box and an idle eight-core one, and a
`process_cpu_percent` above 100 is that same fact stated the other way. Both
numbers have been in the node payload for some time and neither could be
compared between nodes.

- `runtime.cpu_cores` now reports the node's hardware thread count, so a
  consumer can divide by it and get figures that mean the same thing
  everywhere. Requested by the web and React Native client sessions, which
  want to stop handing a transcode to a node that is already saturated —
  the client spent an afternoon routing every session to the slowest node in
  the cluster and producing measurements that described the routing rather
  than the server.
- It is omitted from the payload when unknown rather than reported as zero,
  so a consumer abstains instead of dividing by it.
- On the wire it is an optional trailing telemetry field, so a node that has
  not been upgraded yet simply does not report one. Every node is in that
  position during a rolling upgrade, which is exactly when reading a core
  count out of whatever bytes followed would matter.

## 0.36.2 — A complete playlist, and a wait that cannot wedge the node (development)

Two changes that were made together but are independent of each other: the
media playlist becomes a complete VOD list with bounded holds behind it, and
the retained-memory ledger stops charging RPC reassembly against the budget
that reassembly exists to release.

**The playlist.** `media.m3u8` is now a complete `#EXT-X-PLAYLIST-TYPE:VOD`
list — every planned fragment, closed with `#EXT-X-ENDLIST`, served on the
first fetch with no readiness gate and byte-identical on every later fetch of
the same generation. The duration is known because the source was probed, so
this is the spec-correct form, and with `ENDLIST` present a player stops
polling: one playlist fetch per generation instead of hundreds.

`EXTINF` is the plan rather than the measured length, necessarily — an
unproduced fragment has no measured length and a VOD list may not be revised.
That is only honest because 0.36.1 made the plan predict the output exactly;
confirmed client-side on a 1,748-fragment title whose declared durations sum
to the film's duration, with fragment 0 declaring 2 s and delivering 1.96 s.

**The wait.** Because the playlist promises fragments that do not exist yet, a
request for one is held rather than refused — as an explicitly acquired
resource, never an implicit blocked thread, so an async `HttpServer` would be
an improvement rather than a rewrite. Three tests in order: beyond
`segment_hold_window` nothing is working toward the fragment; past
`max_session_holds` the session has had its share; past
`max_concurrent_holds` the node has. Any of them answers immediately, and a
refusal never advances the producer's demand watermark. `init.mp4` takes the
same path, since a playlist served up front sends the client for it before the
muxer has written it.

A refusal is `500 segment_not_ready` with `Retry-After` and `Cache-Control:
no-store`; a broken generation is `503 stream_failed`. That assignment is
deliberately the opposite of what the spec suggests, and the reasoning is
recorded in `docs/streaming.md` because it looks like a mistake otherwise: a
client cannot read the JSON body on a fragment error, so the status is the
discriminator, and a discriminator readable only as a status has to be one
nothing else on the path emits. Every proxy emits `503` for a dead service, so
`503` meaning "hold, stay here" would make a dead node look like a busy one
and suppress failover silently. `segment_timeout_ms` is 6000, under the
tightest client deadline actually read from a shipped artifact (media3's 8000
ms read timeout) rather than from documentation, which was wrong twice.

**The ledger.** Publication holds retained bytes until a peer confirms the
write, and that confirmation arrives as an RPC message which must first be
reassembled into the same ledger. Charged against the same durable-lower
budget the two meet: publication fills it, reassembly is refused, the peer
channel drops, so nothing confirms and nothing is released — and a restart
re-enters it within minutes, because publication resumes from the spool.
Observed live: 508 MB held of a 512 MB budget, 49,680 reassembly refusals, a
576-byte FUSE admission waiting 35 minutes, telemetry 18 minutes stale in both
directions, and a transcode on another node dying with `extent unavailable`
two hops downstream. Reassembly is now exempt from that budget; the control
and viewer reserves still apply and `MessageAssembler` already bounds
incomplete reassembly independently. After the fix the affected node confirmed
40.6 GB and drained its spool from 1.72 GB to 122 MB with no further refusals.

## 0.36.1 — A transcode's first fragment is the short one it planned (development)

The early `moov` flush was switched off whenever any stream was transcoded,
because it was driven from the demux copy loop and a transcoded stream's
first packet arrives from an encoder instead. So on transcode the delayed
`moov` was written at the first real fragment boundary and consumed it: the
pipeline produced one fewer fragment than planned, and fragment 0 carried
twice its planned media.

- Each pipeline now records its own first muxed packet, which is true for a
  copied and a transcoded stream alike, so transcode gets the same early
  flush remux has had. Measured on a 100 s transcode: 26 fragments for a
  26-entry plan where it was 25, 11 for 11 where it was 10, and fragment 0
  back to 1.96 s from 6.0 s.
- Time to first fragment improves with it. The short startup fragment
  (`kStartupFragmentSeconds`) exists precisely so the first response is
  quick, and on transcode it had been silently merged away.
- `tests/test_transcode_timeline.cpp` gains the invariant this establishes:
  one fragment per planned entry, and a first fragment that is the short one
  it was planned as. It is the prerequisite for serving a complete playlist
  before anything is published — a plan that cannot predict its own output
  makes such a playlist wrong from its first line. That work is designed but
  not started: see
  `TODO/2026-09-08-bounded-vod-playlist-and-segment-holds.md`.

## 0.36.0 — The playlist says what the fragment holds (development)

Deployed to all three cluster nodes on 2026-09-08: gbni-1, gbni-2 and es-1
run an identical binary, verified by hash, with the torrent plugin loading on
each and metadata generations converging. gbni-1's copy was built on gbni-2
and shipped as a staged `/usr` tree, since compiling on gbni-1 browns it out.


A transcoded title's playlist described the plan rather than the media, and
nothing had ever compared the two, because no test had ever run the real
encoder. Building the harness that compares them -- the stated Phase 0
prerequisite of the playback resilience plan -- found both defects below on
its first run.

- **A fragment now declares the media it actually carries.** The first flush
  of a fragmented MP4 writes the delayed `moov` and no `moof`, so that planned
  boundary produces no fragment and its media joins the next one. The producer
  already computed the true length for that case (`publish_duration()` carries
  an unproduced boundary into the fragment that absorbed its media); the
  playlist ignored it and emitted the plan instead, and `Segment::duration`
  was written and never read by anything. Measured on a 100 s transcode:
  fragment 0 carried 6.0 s and declared 2.0 s. A player builds its seek map by
  accumulating `EXTINF`, so every later fragment sat four seconds early on the
  timeline for the rest of the title -- seeks landed four seconds off, and the
  further in, the more obviously.
- **The transcode cut carries an unproduced boundary, like remux already did.**
  The remux path was given this on 2026-09-07 for the identical hazard; the
  transcode branch was not, and is the branch every compatibility playback
  takes.
- **A completed generation no longer finishes in an error state.**
  `MediaSegmentStore::mark_finished()` compared fragment count against plan
  entries, so a run that correctly carried a boundary (25 fragments for a
  26-entry plan) recorded an error. `playlist()` withholds a playlist entirely
  once an error is set, so a transcode that had produced all of its media
  ended by serving an empty one, with `exit_code=1`. It now compares published
  media against planned media, which is the invariant the producer maintains.
- **New: `tests/test_transcode_timeline.cpp`.** Drives the real libav pipeline
  with no injected engine, over a synthesized deterministic source longer than
  90 seconds with a non-zero audio start, AAC priming and a seek, and measures
  the published fragments back through libav rather than trusting the
  pipeline's own bookkeeping. Gates stream start alignment, per-stream media
  span, per-fragment declared-vs-actual duration, the accumulated playlist
  timeline, and a clean finish. It does not measure pitch: a resample-ratio
  change of the kind 0.23.8 shipped keeps the timeline honest while changing
  how the audio sounds, and that remains a listening test.
- **New: `MACHA_SANITIZE`.** Builds the whole tree -- core, executables,
  plugins, both test binaries -- under `address`, `undefined`,
  `address,undefined` or `thread`. Whole-tree rather than per-target because
  `macha_core` is a shared library the executables link and the plugins
  `dlopen`; partial instrumentation would leave the interposed allocator and
  the shadow memory disagreeing across that boundary. `thread` with `address`
  is refused at configure time rather than later by the compiler. Case
  deadlines scale automatically under instrumentation (3x ASan, 10x TSan;
  `--timeout-scale` / `MACHA_TEST_TIMEOUT_SCALE` to override), because the
  declared 30/60/120 s deadlines were chosen against an ordinary build and a
  spurious timeout would hide the report the run existed to produce. The full
  suite is green under `address,undefined` with no sanitizer or leak report.

## 0.35.0 — Serve the web client at the root (development)

A node can now serve the built web client itself, so the client and the API
it talks to are one origin and there is no second server to deploy,
configure or keep in step.

- New `web:` configuration section. `web.root` names a directory of built
  assets; `web.index` is the document a client route resolves to
  (`index.html` by default); `web.enabled` turns serving off without
  unconfiguring it. Unset by default, and a node with no `web.root` answers
  non-API paths exactly as before.
- A path naming a file under the root is served as that file, with a
  content type, an `ETag` and `Cache-Control: public, max-age=3600`.
  Everything else is answered with the index document under `no-cache`, so
  a deep link reaches the client rather than the server's 404 -- which is
  what a single-page application needs, and the index must be revalidated
  or a deploy stays invisible.
- **The API namespace is never served from here.** Everything under `/api`,
  not merely `/api/v1`, remains the server's, 404s included: an API call
  must not come back as an HTML page that a JSON parser will choke on, and
  reserving the whole prefix keeps a later API version from being swallowed
  by the client's fallback.
- Client assets are served without a bearer token, since a browser has none
  until the client has loaded and asked for one. Only the configured root is
  reachable: request paths are checked one segment at a time and a segment
  that is empty, `.`, `..`, or begins with a dot is refused, so a request
  can neither climb out nor read build leftovers such as `.env`. A refused
  path falls through to the index rather than a 404, so probing cannot be
  used to learn whether a file exists.
- The client is served while local services are still recovering, because
  it is static files and depends on none of them: it loads and shows what
  Status reports rather than failing to load at all. A configured root that
  does not exist answers `503 web_client_unavailable` rather than 404, and
  starts serving as soon as the files appear.

## 0.34.0 — The playback contract says what it did

Collects the night's playback work into one release. Four defects, each
found by comparing two sides' idea of the same fact rather than by reading
either side alone, and each invisible for as long as one side stayed
silent (2026-09-07/08).

- A session and the facts endpoint name the container actually served, in
  one vocabulary, for every source the catalogue admits -- see 0.33.2.
- An AAC transcode carries a channel configuration a browser can parse,
  which is what put 5.1 titles back on Chrome and on every Chromium
  WebView -- see 0.33.3.
- `mode` means the whole transform on an update as well as on creation, so
  a client naming one field is not refused for a combination the server
  assembled out of the session's history -- see 0.33.4.
- A refusal says why (`reason`), and the mode table is enforced rather
  than reinterpreted -- see 0.32.18 and 0.33.0.

## 0.33.4 — Naming a mode restates the whole transform (development)

A session update naming only `mode` was refused for a contradiction the
server assembled itself. The per-stream and quality instructions stored at
creation outlived the mode they belonged to, so `{"mode":"direct"}` was
judged against an `audio: transcode` the client had never sent in that
request and answered "direct copies every stream". Because the client's
chooser answers most of this library with transcode-and-copy-the-video,
that was every session: the Direct and Remux controls failed for viewers
nearly everywhere, and the failed update tore the session down and dropped
them back to the browse screen (found by the UI session, 2026-09-08).

- `mode` is the shorthand for the whole transform, so naming it now clears
  `video`, `audio`, `max_height` and `max_bitrate` unless the same update
  restates them. An update naming both sets both. This is the rule the
  session already applied internally when asking what a different mode
  would do, and it makes the shorthand mean the same thing regardless of
  what the session was created as.
- Creating a session is unaffected: there is nothing to clear.

## 0.33.3 — AAC transcodes carry a channel configuration browsers can parse (development)

A regression from 0.32.19, found by the UI session and reproduced on Silo
S03E01. Keeping the source's channel layout on the AAC encoder was right in
intent -- a codec change is not a downmix -- but it handed the encoder
5.1(side), which is what E-AC-3 decodes to and is not one of AAC's standard
channel configurations. The encoder then described the arrangement in a
Program Config Element and set `channelConfiguration` to 0, a 26-byte
AudioSpecificConfig with libavcodec's own comment string inside it.

Chrome's MP4 parser rejects that config outright. Per the MSE spec the
failed append ends the MediaSource with a decode error, every later append
fails, and hls.js answers by rebuilding the MediaSource and retrying: the
same 2.4 MB segment was refetched 58 times in 46 seconds behind a spinner.
Safari and the television's native player never saw it, because they take
E-AC-3 copied into MPEG-TS; every Chromium browser and WebView did, which
means the web client and the Android/Google TV host (2026-09-08).

- The AAC encoder is now opened with AAC's standard layout for the source's
  channel count -- the surround pair at the back, not the side. The channel
  count is unchanged and the resampler maps the source into it, so this is
  a relabelling and not a downmix. Measured against libavcodec on the
  cluster: `5.1` gives a five-byte config with `channelConfiguration` 6,
  `5.1(side)` a 26-byte Program Config Element with 0.
- An encoder that opens but still reports `channelConfiguration` 0 is
  refused and reopened as stereo, so an unparseable configuration is never
  served even from a build whose encoder disagrees with this table.
- Seven channels have no standard configuration at all; they are carried as
  7.1 with the eighth channel silent, which loses nothing.
- A runtime test asks this build's own encoder for every channel count from
  1 to 8 and fails if any answers with `channelConfiguration` 0.

## 0.33.2 — One file for the container vocabulary (development)

Answering a client's question about `output.container` turned up a source
this server serves and declines to name: the catalogue admits `.avi`,
`.wmv`, `.mpg`, `.ts` and `.m2ts` as video, and the playback container
table knew none of them, so a `direct` session on the six AVI files in the
library reported `"container": ""` while happily serving the bytes. The
two lists were the same truth written down twice, in two files, by two
different pieces of work (2026-09-07).

- New `src/media_containers.{hpp,cpp}` holds the vocabulary: which file
  names are video and which audio, what container a name and a probed
  format mean, the Content-Type for serving a source unchanged and for the
  files a session generates, which codecs fMP4 and MPEG-TS each carry as a
  copy, and which subtitle codecs convert to WebVTT. They are facts about
  file formats, so nothing there takes an engine type, opens a file or
  reads configuration. Adding a format is one row.
- `output.container` and the facts entries now name AVI, ASF, MPEG-PS,
  MPEG-TS, WAV, AIFF and ADTS sources. A format neither table knows is
  reported under libav's own name for it rather than as the empty string,
  so the field is never blank for a source that probed.
- The catalogue now admits `.mka` and `.oga` as audio, which follows from
  the two lists becoming one; playback already served both. No file in the
  library has either extension.

## 0.33.1 — Say which container was actually served (development)

MPEG-TS turned out to be the only carriage a 2017 Samsung would play:
copied HEVC black-screened in fragmented MP4 on its native player and was
rejected outright through MediaSource, and copied E-AC-3 stuttered on one
path and was rejected on the other. In MPEG-TS both stream copies play
untouched. The container is therefore a decision worth making and worth
being able to check (2026-09-07).

- The session reports `output.container`: `fmp4` or `mpegts` for an HLS
  session, the source's own container for a `direct` one. It was the one
  instruction a client could ask for and never see confirmed, and the same
  release found two modes being reported as something other than what was
  performed, so a request is not evidence.
- `operations` gains `copy_into_mpegts` beside `copy_into_fmp4`. The two
  containers do not carry the same codecs: TS takes MPEG-2 video and
  MP3/MP2 audio that fMP4 will not, fMP4 takes AV1 and Opus that TS will
  not, and both take H.264, HEVC, AAC, AC-3 and E-AC-3.
- A copy into MPEG-TS the container cannot carry is now refused up front,
  as it already was for fragmented MP4, rather than failing in the muxer.
- A VOD planning or subtitle deadline reports `source_read_timed_out`
  instead of arriving with no reason attached.

## 0.33.0 — A mode must describe what it is doing (development)

The operator's ruling on the legal permutations, enforced. `direct` and
`remux` copy every stream; `transcode` re-encodes at least one and may copy
the other. Eleven of the twenty-seven mode/video/audio combinations were
being accepted against that rule, and two of them came back reported as a
mode other than the one requested (2026-09-07).

- `remux` with a stream set to `transcode` is now `400`; it used to be
  accepted and reported as a transcode. `transcode` with both streams
  copied is now `400`; it used to be accepted and reported as a remux. A
  session that names a mode it is not performing misleads everything
  downstream of it, so the request is refused rather than reinterpreted.
- `direct` with a stream set to `transcode`, or with `max_height` /
  `max_bitrate`, is now `400`. Those were silently ignored.
- `remux` with `max_height` / `max_bitrate` is now `400` for the same
  reason: a quality change is a re-encode, and remux copies.
- The mixture is asked for as `{"mode": "transcode", "video": "copy"}`.
- The session's `options` no longer carry per-stream transforms or quality
  into the question they ask about a *different* mode, which would have
  answered "remux unavailable" for a session that had a `max_height` set.

## 0.32.19 — The first fragment is one segment, and a transcode is not a downmix (development)

Two faults found by ffprobing what a remux actually emits, rather than
trusting the 201 (2026-09-07).

- The fragmented MP4 muxer delays its `moov` until it has seen a packet of
  every stream, because the (E-)AC-3 sample entry can only be filled from
  one. The flush that writes that `moov` produces no `moof`: the media
  buffered up to it stays buffered and joins the next fragment. That flush
  was landing on the first planned segment boundary, so segment 0 carried
  two segments of media while the playlist declared one, and every later
  segment sat 10 s early on the player's timeline. The `moov` is now
  flushed as soon as every stream has been written, which costs nothing,
  and a boundary that produces no fragment carries its length into the
  fragment that does, so the playlist cannot describe media that is not
  there.
- The AAC encoder was fixed at two channels and 192 kbit/s, so a 5.1 source
  came back stereo whenever the audio was re-encoded. It now keeps the
  source's channel layout, at 64 kbit/s per channel, falling back to stereo
  only if the encoder refuses the layout. A client that asked for a codec
  change did not ask for a downmix.

## 0.32.18 — A failure says which failure it was (development)

A node that has lost its path to the cluster can still open its API and
still fail to read a single byte of a film. Until now that came back
indistinguishable from a corrupt file, so a client had no way to tell "ask
another node" from "stop asking anyone" (2026-09-07, gbni-2 partitioned).

- `MediaError` carries a `MediaFailure`: `unreadable` when this node could
  not read the bytes, `unsupported` when the bytes were read and are not
  media this build can demux, `timed_out` when reading missed the deadline.
  libav flattens every read failure into `EIO`, so the underlying read error
  is kept and preferred over libav's account of it.
- The catalogue media profile, the playback facts endpoint and playback
  stage failures all report a `reason` alongside the message:
  `source_unreadable`, `source_unsupported`, `source_read_timed_out`. The
  server states it and acts on none of it.
- The facts endpoint no longer collapses a partly-readable item into a
  `404`. It reports the media it could read and lists the rest under
  `unavailable`, each with its own reason.

## 0.32.17 — The catalogue media profile answers with facts, not 503 (development)

The profile endpoint answered `503 profile_unavailable` whenever no profile
had been persisted yet, which made "nobody has looked at this file" look
like "this file is broken". The client's chooser reads that endpoint, so a
title nobody had played was unplayable until a background worker got to it.

- With no stored profile the endpoint now probes at foreground priority,
  persists the result and returns it. `404` means the media is not on this
  node; `422` means the file could not be probed.
- Both priority classes hold: a profile already scanned is returned from
  the persisted copy, and a viewer never waits on background work.

## 0.32.16 — Remove client capabilities from the API (development)

The server had been deciding how to play media on the client's behalf,
which is not a server function. It reports what the media is and performs
what it is told.

- `negotiate()` becomes `plan_for(probe, preferences)`. `mode` is required
  and must be `direct`, `remux` or `transcode`; there is no `auto`.
- `ClientCapabilities` and every parser, description and warning built on
  it are gone. The server does not ask what a client can play.
- The media profile gains Dolby Vision profile and base-layer
  compatibility (schema 3), so the client can choose on the facts that
  actually decide the question.

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

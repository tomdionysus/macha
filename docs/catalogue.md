# Catalogue

## Authority

The catalogue is distributed cluster metadata. Scanner/provider processes produce candidate media descriptions, but the committed catalogue is represented by content-addressed CONTROL objects referenced from namespace metadata.

External providers are enrichment inputs, not recovery authorities.

## Structure

The catalogue is split into 64 deterministic shards. Item ID determines its shard. A small manifest contains the optional `ObjectId` for each shard.

```text
namespace metadata
      |
      v
catalogue manifest (CONTROL)
      |
      +--> shard 00 (CONTROL)
      +--> shard 01 (CONTROL)
      ...
      +--> shard 63 (CONTROL)

catalogue item
      |
      +--> media IDs / metadata
      `--> artwork ObjectIds (DATA)

immutable media ID
      `--> validated playback profile (format, duration, bitrate and streams)
```

A mutation rewrites only affected shards plus the manifest rather than serializing the complete catalogue for every item.

## Immutable media profiles

An event-driven media-information worker accepts durable hints from ingest,
cataloguing and the profile API. It orders and deduplicates those hints, probes
with speculative/background reads below loader priority, and publishes the
result against the immutable content-derived `macha:` identity rather than a
mutable path. It does no work while the queue is idle.

Playback uses a valid stored profile without media-object reads. If one is
missing, profile generation remains an optimisation rather than an admission
prerequisite: normal media negotiation continues through the configured media
engine. That viewer-required inspection takes over any speculative scan for the
same immutable media, concurrent callers share the flight, and success is
published asynchronously for future sessions.

Clients can read the validated profile without opening the media:

```text
GET /api/v1/catalogue/media/{url-encoded-macha-media-id}/profile
```

The response contains `schema_version` (currently 3), `media_id`, `format`,
`size` (the file's bytes), `duration_ms`, aggregate `bitrate`, and per stream
`index`, `type`, `codec`, `profile`, `language`, `width`, `height`,
`channels`, `sample_rate`, `bit_depth`, `level`, `color_transfer`,
`dolby_vision_profile`, `dolby_vision_compatibility`, `default`, `forced`,
`bitrate` and `attached_picture`. Every stream field is present, with `0` or
`""` for a fact the stream does not have. It is served with private immutable
cache headers and the media ID as its `ETag`: a media ID names its bytes, so
none of this changes. `size` is `null` when this node cannot find the file,
and that answer is served `no-cache`. `format` is libav's name for what the
file is; the playback `container`, which can depend on the file's name, is on
session facts only.

A client playing a file directly (Direct Play) can turn the byte ranges it
holds into times with the file's keyframe byte index:

```text
GET /api/v1/catalogue/media/{url-encoded-macha-media-id}/keyframes
```

```json
{"status": "ok", "schema_version": 1, "media_id": "macha:...",
 "container": "mp4", "offsets": "sample",
 "size_bytes": 1425529460, "duration_ms": 6443500,
 "streams": [
   {"index": 0, "type": "video", "codec": "hevc", "entries": [[0, 48], [2002, 1043377]]},
   {"index": 1, "type": "audio", "codec": "aac", "entries": [[0, 1040]]}]}
```

Each entry is `[time_ms, byte_offset]`, from the container's own index:
video keyframes, and audio samples at most one per second of media. A
stream's `index` is its container stream index: the same number as
`source.streams[].index` and `selected.video_stream` / `selected.audio_stream`
on a playback session, so a client can keep only the streams it is playing.
Times are the index's decode times (DTS), not presentation times: for video
with B-frames a keyframe's time is early by its composition offset, typically
tens to a few hundred milliseconds, so they place bytes but are not
frame-exact. Each
stream's entries are sorted by byte offset (times are not guaranteed to rise
in that order across an interleave). `offsets` says what an offset points
at: `sample` (MP4: the sample's exact position) or `cluster` (Matroska: the
Cluster holding the entry, at or just before it). Between entries the mapping
is the client's to interpolate; past the last entry the file ends at
(`duration_ms`, `size_bytes`). A Matroska file often cues only its video, so
its audio list may hold one entry or none.

The index is built once per media id: after the media's background profile,
or on the first request for a file profiled before it existed. It is stored
as an immutable DATA object referenced from the catalogue, like artwork, and
is never built on the playback path. The response is `immutable` with the
media id as its `ETag`. Codes: `400 bad_media_id`, `404 not_found` (this node
cannot find the file), `422 keyframes_not_supported` (a container that keeps
no byte index: only MP4 and Matroska do), `422 keyframes_failed` (the file
could not be read, with the failure axes of a profile failure). One index is
built at a time on a node.

Pre-session availability is guaranteed for a `macha:` identity, so a miss is
not normally a deferral. The order is:

| condition | response |
|---|---|
| not a `macha:` identity | `400 bad_media_id` |
| a stored profile exists | `200` |
| no stored profile | probed there and then at foreground priority, persisted, `200` |
| no stored profile, and this node has no media engine (streaming off) | `404 media_engine_unavailable`: not here; another node may have an engine or the profile |
| the probe failed | `422 profile_failed`, with `error.reason` when the engine said why (`source_unreadable`, `source_unsupported`, `source_read_timed_out`) |
| the media is not resolvable on this node | `202` with `{"status": "pending", "media_id"}`, `Retry-After: 1` and `Location` if background profiling was accepted, otherwise `404 not_found` |

The `202` is a residual fallback rather than the ordinary miss path. It is
advisory in either case: it does not prevent a client from starting normal
playback negotiation immediately.

## Commit protocol

A catalogue mutation reads the current metadata root and uses optimistic concurrency.

Before publishing a successor root:

- newly introduced artwork references are verified through ordinary DATA reads;
- changed shard objects are content-addressed and stored on at least `metadata_min_write_replicas` active nodes;
- the successor manifest is stored on at least `metadata_min_write_replicas` active nodes;
- namespace metadata is CAS-updated from the expected old root to the new root.

A conflicting namespace/catalogue generation retries as a conflict. A metadata/control durability outage is infrastructure unavailability and causes scanner work to defer without consuming semantic/provider attempts.

## Control convergence

The configured metadata write floor is enough to commit. Maintenance separately converges the current manifest and all referenced shards to every active metadata replica.

If an active metadata replica loses a control object, the missing immutable object is fetched from another active replica. The committed root remains valid while enough reachable replicas satisfy the configured metadata write floor; maintenance subsequently converges control objects to all active replicas.

Control garbage collection uses its own live set and grace period. It does not interact with DATA placement.

## Artwork

Artwork is DATA, not CONTROL.

`stage_artwork()` content-addresses the downloaded bytes and writes them through the normal distributed DATA store. It therefore obeys the same rules as media extents:

- capacity-aware preferred owner;
- deterministic fallback when an owner/backend is full or unavailable;
- `min_write_replicas` publication floor;
- repair toward `replicas`;
- ordinary DATA reachability GC;
- optional local small-object packing.

A full node can read artwork remotely without first promoting it into its own full authoritative DATA store.

The catalogue item records role, MIME type and `ObjectId`; it never records a pack filename/offset.

## Scanner hints

Namespace discovery produces persisted/coalescing path hints. Provider work is bounded and processed in batches. Prepared matches are reconciled together rather than committing one complete catalogue per media file.

Failures are classified:

- provider/content/parsing failures consume the hint's bounded semantic attempts;
- catalogue CAS conflicts defer briefly;
- metadata/control write-floor or DATA availability failures defer without incrementing semantic failure count.

This prevents a temporary cluster outage from permanently marking otherwise valid media as failed.

A batch reads one namespace snapshot for all its hints. A hint created after that snapshot was taken may name a file the snapshot cannot contain; it is deferred with `path_not_yet_visible` and looked at again in the next batch, rather than failed with `path_missing`. Clearing the ingest job that raised a hint removes only that origin: a `failed` hint stays, as the record that its file was never catalogued.

`GET /api/v1/catalogue/hints` lists every hint with its `state` (`queued`,
`processing`, `deferred`, `catalogued`, `no_match`, `failed`), attempt and
failure counts, `candidate_cursor`, `provider`, `media_id`,
`catalogue_item_ids` and `origins`. `result` is a code
(`matched`, `outside_catalogue_roots`, `ignored_term`, `not_media_file`,
`no_media_candidate`, `no_provider_match`, `already_stored`,
`profile_prepared`, `media_not_live`, `manual_existing_item`,
`manual_metadata`, or `null`), and `error_code` sits beside `error`
(`path_missing`, `path_not_yet_visible`, `content_not_committed`, `provider_budget_exhausted`,
`provider_unavailable`, `provider_error`, `catalogue_conflict`,
`catalogue_unavailable`, `catalogue_error`, `artwork_durability_unavailable`,
`no_immutable_identity`, `yielded_to_playback`, `media_information_error`).
The `error` text is for people; act on the codes.

## HTTP routes

Reads need `media_viewer`; every mutation needs `manager`.

Every item returned carries `availability` (`complete`, `partial`, `unavailable` or `unknown`) and, for a set, `availability_members`: how much of it the reachable cluster holds, as described in [Files and availability](files.md#catalogue-items). They are not part of the item: a `PUT` or `PATCH` body that names them is ignored there.

- `GET /api/v1/catalogue/status` — `ready`, `metadata_generation`, `known_metadata_generation`, `root`, `items`, `artwork_objects`, `local_artwork_objects`, `last_sync_unix_ms`, and `error_code` (`converging`, `unavailable`) beside `error`.
- `GET /api/v1/catalogue/items?type=...&parent=...` and `GET /api/v1/catalogue/search?q=...&limit=...` (limit up to 1000, default 50) — `{"items": [...]}`. Search also takes `kind`, which may repeat (`kind=movie&kind=show`; `movie`, `show`, `season`, `episode`, `artist`, `album`, `track`), and `parent`, which keeps only that item's children; both filter before `limit`, and an unknown kind is `400 bad_kind`.
- `GET|PUT|PATCH|DELETE /api/v1/catalogue/items/{id}` — the item carries its revision as `ETag: "rev-N"`; `PUT`, `PATCH` and `DELETE` honour `If-Match` with that value and answer a stale one with `409 conflict`.
  - `PUT` replaces the item's descriptive fields; `media_ids` and `artwork` change only when the body names them, so an edit that leaves them out keeps the item's files and artwork. `PATCH` changes only the fields present, and `null` clears an optional one; an unknown item is `404 not_found`.
  - A `parent_id` must name an existing item of the right kind (season under show, episode under season, album under artist, track under album; movie, show and artist take none): otherwise `400 parent_not_found` with `parent_id`, or `400 bad_parent_kind` with `kind` and `parent_kind`. A body that is not a usable item is `400 bad_item`.
  - An edit locks the item against the scanner (`external_ids.macha_metadata_locked`) unless the body says `"lock": false`, which removes the lock.
- `DELETE /api/v1/catalogue/items/{id}/metadata` — clears the item's metadata and queues its media for rematching.
- `POST /api/v1/catalogue/items/{id}/artwork?role=...&mime=...` — stores the body as artwork (DATA, below) and answers `201` with `role`, `id`, `mime_type`.
- `GET /api/v1/catalogue/artwork/{object id}` — artwork bytes. Items carry signed artwork URLs (`?exp=...&sig=...`) that need no bearer token and stay byte-identical inside a TTL bucket, so a browser cache keeps them; the response is `immutable` with the object id as its `ETag`.
- `GET /api/v1/catalogue/media/{id}/profile` — above.
- `GET /api/v1/catalogue/hints` — above.

## Maintenance liveness

Catalogue maintenance exports two different live sets:

- artwork `ObjectId`s join the ordinary DATA live set;
- manifest/shard `ObjectId`s join the CONTROL live set.

Physical GC runs only when the catalogue/metadata view is sufficiently current to make those sets safe.

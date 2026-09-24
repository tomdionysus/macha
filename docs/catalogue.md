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
`duration_ms`, aggregate `bitrate`, and per stream `index`, `type`, `codec`,
`profile`, `language`, `width`, `height`, `channels`, `sample_rate`,
`bit_depth`, `level`, `color_transfer`, `dolby_vision_profile`,
`dolby_vision_compatibility`, `default`, `forced`, `bitrate` and
`attached_picture`. Every field is present, with `0` or `""` for a fact the
stream does not have. It is served with private immutable cache headers and the
media ID as its `ETag`.

Pre-session availability is guaranteed for a `macha:` identity, so a miss is
not normally a deferral. The order is:

| condition | response |
|---|---|
| not a `macha:` identity | `400 bad_media_id` |
| a stored profile exists | `200` |
| no stored profile | probed there and then at foreground priority, persisted, `200` |
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

`GET /api/v1/catalogue/hints` lists every hint with its `state` (`queued`,
`processing`, `deferred`, `catalogued`, `no_match`, `failed`), attempt and
failure counts, `candidate_cursor`, `provider`, `media_id`,
`catalogue_item_ids` and `origins`. Since 0.56.0 `result` is a code rather than
a sentence (`matched`, `outside_catalogue_roots`, `not_media_file`,
`no_media_candidate`, `no_provider_match`, `already_stored`,
`profile_prepared`, `media_not_live`, `manual_existing_item`,
`manual_metadata`, or `null`), and `error_code` sits beside `error`
(`path_missing`, `content_not_committed`, `provider_budget_exhausted`,
`provider_unavailable`, `provider_error`, `catalogue_conflict`,
`catalogue_unavailable`, `catalogue_error`, `artwork_durability_unavailable`,
`no_immutable_identity`, `yielded_to_playback`, `media_information_error`).
The `error` text is for people; act on the codes.

## HTTP routes

Reads need `media_viewer`; every mutation needs `manager`.

- `GET /api/v1/catalogue/status` — `ready`, `metadata_generation`, `known_metadata_generation`, `root`, `items`, `artwork_objects`, `local_artwork_objects`, `last_sync_unix_ms`, and `error_code` (`converging`, `unavailable`) beside `error`.
- `GET /api/v1/catalogue/items?type=...&parent=...` and `GET /api/v1/catalogue/search?q=...&limit=...` (limit up to 1000, default 50) — `{"items": [...]}`.
- `GET|PUT|DELETE /api/v1/catalogue/items/{id}` — the item carries its revision as `ETag: "rev-N"`; `PUT` and `DELETE` honour `If-Match` with that value and answer a stale one with `409 conflict`.
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

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

Catalogue hint processing precomputes the playback profile using loader-priority
reads and commits it in the same batched catalogue reconciliation as descriptive
metadata. Playback performs a bounded, coalesced first-use probe only when that
profile is genuinely absent. Profile validity is tied to the content-derived
`macha:` identity, never merely to a mutable path.

Clients can read the validated profile without opening the media:

```text
GET /api/v1/catalogue/media/{url-encoded-macha-media-id}/profile
```

The response contains `schema_version`, `media_id`, `format`, `duration_ms`,
aggregate `bitrate`, and every stream's codec/profile, language, bitrate,
dimensions/audio properties and default/forced/attached-picture flags. It is
served with private immutable cache headers. `404 profile_not_available` is temporary;
session admission can still use the bounded first-use fallback.

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

## Maintenance liveness

Catalogue maintenance exports two different live sets:

- artwork `ObjectId`s join the ordinary DATA live set;
- manifest/shard `ObjectId`s join the CONTROL live set.

Physical GC runs only when the catalogue/metadata view is sufficiently current to make those sets safe.

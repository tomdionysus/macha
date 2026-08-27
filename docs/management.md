# Management API

`Manage` is the human control surface for catalogue exceptions and the MachaDFS namespace. It does not maintain a parallel metadata database: operations are translations onto the existing catalogue, catalogue-hint and MachaDFS primitives.

## Unmatched media

`GET /api/v1/manage/unmatched` returns only terminal semantic `no_match` records that still resolve to the same immutable `macha:` media identity. Deferred provider outages, queued work, active matching, files outside configured catalogue roots, and stale paths are not presented as files requiring manual matching.

For one unmatched record:

- `GET /api/v1/manage/unmatched/{id}` returns the failure plus local filename/tag probe hypotheses;
- `GET /api/v1/manage/unmatched/{id}/matches?q=...` searches existing catalogue leaf items as prospective manual bindings;
- `POST /api/v1/manage/unmatched/{id}/match` binds the immutable media identity to an existing movie, episode or track;
- `POST /api/v1/manage/unmatched/{id}/manual` creates normal manual catalogue metadata (including show/season or artist/album hierarchy as required) and binds the file;
- `POST /api/v1/manage/unmatched/{id}/retry` reopens normal scanner work at manual-rescan priority;
- `DELETE /api/v1/manage/unmatched/{id}` deletes the media file through MachaDFS and clears the exception record.

Every destructive/resolution operation verifies that the current path still has the media identity recorded when matching failed. A replaced or moved path therefore returns a conflict instead of acting on different bytes.

Artwork for manually created metadata uses the ordinary catalogue artwork endpoint. It remains DATA and follows normal placement, replication, repair and GC.

## MachaDFS namespace

`GET /api/v1/manage/filesystem?path=/...` lists the MachaDFS directory exactly as represented by namespace metadata. Catalogue bindings are optional annotations and never gate browsing.

Namespace mutations use the ordinary MachaDFS operations:

- `POST /api/v1/manage/filesystem/mkdir`
- `POST /api/v1/manage/filesystem/rename`
- `DELETE /api/v1/manage/filesystem?path=/...`

Rename is also the move primitive. It changes namespace metadata, including a directory subtree, without copying or re-hashing unchanged media extents. The management API defaults to no-replace semantics so a stale browser cannot overwrite an existing target accidentally. Empty-directory removal follows ordinary MachaDFS `rmdir` semantics.

Unmatched hint paths are migrated with management-initiated namespace renames, while their immutable media identities remain unchanged.

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

### Parked publications

A FUSE write whose publication keeps failing transiently is retried with
exponential backoff and, once it exhausts its retry budget (see
`fuse.publication_retry_*` in the configuration guide), is parked: its bytes
stay in the spool and journal and it leaves the loader queue so the rest of
the cluster keeps publishing.

- `GET /api/v1/manage/filesystem/parked-publications` → `{"parked": [{inode, path, error_code, error_message, attempts, failing_for_ms, parked_for_ms, pending_bytes}]}`
- `POST /api/v1/manage/filesystem/parked-publications/{inode}/retry` — reset the retry budget and re-queue the publication (`204`; `409 not_parked` if that inode is not parked).
- `POST /api/v1/manage/filesystem/parked-publications/{inode}/abandon` — drop the unpublished generation from the spool, exactly as a corrupt spool record is dropped (`204`; `409 not_parked` if not parked).

`diagnostics.filesystem.parked_publications` and
`diagnostics.filesystem.publication_retries_backed_off` in `GET /api/v1/status`
carry the counts.

## Management root and cluster identity associations

`GET /api/v1/manage` is the stable root for management capabilities. Existing catalogue and MachaDFS management resources remain beneath this prefix, and future privileged administrative actions should be added here rather than creating unrelated top-level mutation APIs.

Cluster-wide endpoint identity resets are exposed as:

- `POST /api/v1/manage/identity-associations/reset`
- `POST /api/v1/manage/nodes/{node_id}/identity-association/reset`

The general action accepts `host` plus optional `port`, optional `node_id`, and optional `reason`. Omitting `node_id` is intentional: an administrator can clear an association even after the stale node identity is no longer known. Omitting `port` creates a host/IP-wide reset covering every advertised port on that host.

A reset is a durable distributed tombstone, not node deletion. It removes matching live membership, telemetry and RPC endpoint associations and closes matching cached sessions. Persisted node status and MachaDFS data are retained. Retired identities are excluded from the ordinary cluster node list, health and capacity totals; `GET /api/v1/status/nodes/{node_id}` retains an explicit `state: "retired"` audit view.

Association reset is also a recovery action and therefore does not require
metadata to be writable. The request synchronously applies only the small,
locally durable operational tombstone and returns `202 Accepted` with
`audit_state: "queued"`. Peer propagation and the cluster-metadata audit run
asynchronously and can never extend request latency. The response therefore
reports `metadata_persisted: false` and a null `metadata_generation`; these
fields describe the queued audit rather than failure of the already-effective
local reset. Reachable peers persist the operational tombstone during
background propagation and subsequent identity-reset exchanges.

The tombstone is a freshness boundary. Pre-reset gossip cannot recreate the invalidated mapping, and stale `NodeInfo` references are rejected rather than silently routed to a replacement node. A subsequent directly authenticated peer may establish a fresh association at the endpoint. Reset records contain an epoch, reset timestamp, initiating node, and optional reason for operational auditability; applying the same or an older epoch is idempotent.

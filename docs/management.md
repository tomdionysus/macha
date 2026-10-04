# Management API

`Manage` is the human control surface for catalogue exceptions and the MachaDFS namespace. It does not maintain a parallel metadata database: operations are translations onto the existing catalogue, catalogue-hint and MachaDFS primitives.

## Unmatched media

`GET /api/v1/manage/unmatched` returns only terminal semantic `no_match` records that still resolve to the same immutable `macha:` media identity. Deferred provider outages, queued work, active matching, files outside configured catalogue roots, and stale paths are not presented as files requiring manual matching. The response is `{count, items, conflicts}`; each item's `result` is the code the scanner recorded (for example `no_provider_match`, `no_media_candidate` or `media_not_live`), not a sentence. `conflicts` lists every file bound to more than one movie, episode or track, as `{media_id, item_ids}`, except a multi-episode file bound to several episodes of one season.

For one unmatched record:

- `GET /api/v1/manage/unmatched/{id}` returns the failure plus local filename/tag probe hypotheses;
- `GET /api/v1/manage/unmatched/{id}/matches?q=...` searches existing catalogue leaf items as prospective manual bindings;
- `POST /api/v1/manage/unmatched/{id}/match` with `{catalogue_item_id}` binds the immutable media identity to an existing movie, episode or track (`400 not_playable_item` for any other kind); with `{ref}` it matches to a provider record instead (see below);
- `POST /api/v1/manage/unmatched/{id}/manual` creates normal manual catalogue metadata (including show/season or artist/album hierarchy as required) and binds the file (see below);
- `POST /api/v1/manage/unmatched/{id}/retry` reopens normal scanner work at manual-rescan priority (`202`);
- `DELETE /api/v1/manage/unmatched/{id}` deletes the media file through MachaDFS and clears the exception record.

Every destructive/resolution operation verifies that the current path still has the media identity recorded when matching failed. A replaced or moved path therefore returns `409 stale_unmatched` instead of acting on different bytes.

A match by provider reference fetches the record, builds its hierarchy (reusing items already catalogued under the same ids), stages the provider's default artwork and binds the file, exactly as a scan match does. `ref` is one of:

- `tmdb:movie:<id>`;
- `tmdb:tv:<id>` with `season_number` and `episode_number`;
- `musicbrainz:release:<mbid>` with `track_number` and optionally `disc_number`.

It answers `200` with `status: matched`, `leaf_item_id` and the `items` written. Refusals leave the file unmatched: `400 bad_ref` (not one of the forms above), `400 not_playable_ref` (a show or release without its numbers), `400 provider_not_configured` (no TMDB token, or MusicBrainz disabled), `404 provider_not_found` (no such record, or no episode or track with those numbers), `503 provider_unavailable` (the provider could not be reached or failed). Editor requests use their own provider connections, outside the scanner's per-batch request budget; MusicBrainz requests from the editor and the scanner share one pacing of one a second.

## Provider search

`GET /api/v1/manage/providers/search?q=...&kind=movie|show|album&year=&artist=&limit=` searches the metadata provider for a kind: TMDB for `movie` and `show`, MusicBrainz releases for `album` (`artist` narrows to releases credited to that artist). `limit` is 1..50, default 10. It answers:

```json
{"status": "ok", "results": [
  {"ref": "tmdb:movie:335984", "provider": "tmdb", "kind": "movie",
   "title": "Blade Runner 2049", "year": 2017, "overview": "...",
   "catalogue_item_id": "tmdb:movie:335984"}
]}
```

`ref` is what a match by provider reference takes. `year` is `null` when the provider gives none; an album result adds `artist`. `catalogue_item_id` is present when the catalogue already holds the item a match would write. Codes: `400 bad_query` (no `q`), `400 bad_kind`, `400 bad_year`, `400 bad_limit`, `400 provider_not_configured`, `503 provider_unavailable`.

## Provider artwork

`GET /api/v1/manage/providers/artwork?ref=...&role=...` lists the images a provider has for one role of a reference:

- `tmdb:movie:<id>` and `tmdb:tv:<id>`: `poster`, `backdrop`;
- `tmdb:tv:<id>` with `season_number`: `poster` (the season's);
- `tmdb:tv:<id>` with `season_number` and `episode_number`: `still`;
- `musicbrainz:release:<mbid>`: `cover` (the Cover Art Archive's front images).

It answers `{"status": "ok", "options": [{option_id, role, width, height, language, preview_url}]}`. `width`, `height` and `language` are `null` when the provider does not say; `preview_url` is the provider's own small image, for the client to show directly.

`POST /api/v1/manage/providers/artwork/choose` with `{item_id, role, option_id}` fetches that option at full size, stores it as catalogue artwork and makes it the item's only artwork for the role. The reference is the item's own: a TMDB movie or show is its own record, a season or episode is its show's with its own numbers, an album is its MusicBrainz release. An item with none (a manual item) names one with `ref` (and `season_number`, `episode_number`). The choice locks the item against the scanner unless the body says `"lock": false`. It answers `{"status": "chosen", "item": ...}`.

Codes, for both: `400 bad_ref`, `400 bad_role` (not a role that reference offers), `400 bad_number`, `400 provider_not_configured`, `404 provider_not_found`, `503 provider_unavailable`; for a choice also `404 not_found` (no such item), `400 no_provider_ref`, `404 option_not_found` (not an option the provider lists for that role now).

## Provider release tracks

`GET /api/v1/manage/providers/musicbrainz/releases/{mbid}/tracks` lists the tracks of one MusicBrainz release, `{mbid}` being the id in a `musicbrainz:release:<mbid>` reference. It answers:

```json
{"status": "ok", "tracks": [
  {"disc_number": 1, "track_number": 4, "title": "...", "length_ms": 215000,
   "recording_id": "<mbid>"}
]}
```

Tracks are in the release's own order: its media in sequence, each medium's tracks in sequence. `disc_number` is the medium's position and `track_number` the track's position on that medium, the numbers a match by provider reference takes. `title` is the track's title on this release, which may differ from its recording's title. `length_ms` is the track's length, or its recording's when the track has none; `recording_id` is the MusicBrainz recording. `disc_number`, `track_number`, `length_ms` and `recording_id` are `null` when MusicBrainz gives none. A release without media answers an empty `tracks`.

Codes: `400 bad_ref` (`{mbid}` is not a MusicBrainz id), `400 provider_not_configured`, `404 provider_not_found` (no such release), `503 provider_unavailable`. The request to MusicBrainz shares the one-a-second pacing of every other MusicBrainz request the node makes, and waits its turn rather than being refused; a release already fetched is answered without one.

Provider search, artwork and release tracks need the manager role even to read, since they make the node call the provider on the caller's say-so.

## Manual entry

Manual entry names its parents either by title or by id. By id, the item joins an existing hierarchy, such as a show the scanner matched, instead of creating a `manual:` one beside it:

- an episode takes `season_id` (the episode takes that season's number), or `series_id` plus `season_number`, which reuses that show's season with the number or creates one under the show; otherwise `series`, `series_year`, `season_number` as titles;
- a track takes `album_id`, or `artist_id` plus `album`, which reuses that artist's album with the title (and `year`, when given) or creates one under the artist; otherwise `artist`, `album` as titles.

Existing parents are referenced, never rewritten. A named id that does not exist is `404 parent_not_found`; one of the wrong kind is `400 bad_parent_kind`. Both state `error.parent_id`; `bad_parent_kind` adds `error.expected_kind` and `error.parent_kind`. The unmatched record is left in place when a request is refused.

Every item manual entry writes carries the metadata lock (`external_ids.macha_metadata_locked = "1"`), so a later scan does not overwrite it, unless the body says `"lock": false`.

A manual item may bind a file anywhere in MachaDFS. A complete catalogue scan unbinds from manual items only files gone from the namespace altogether; the item itself stays, with no files if its last one went.

Artwork for manually created metadata uses the ordinary catalogue artwork endpoint. It remains DATA and follows normal placement, replication, repair and GC.

## MachaDFS namespace

`GET /api/v1/manage/filesystem?path=/...` lists the MachaDFS directory exactly as represented by namespace metadata. Catalogue bindings are optional annotations and never gate browsing.

Namespace mutations use the ordinary MachaDFS operations:

- `POST /api/v1/manage/filesystem/mkdir`
- `POST /api/v1/manage/filesystem/rename`
- `DELETE /api/v1/manage/filesystem?path=/...`

Rename is also the move primitive. It changes namespace metadata, including a directory subtree, without copying or re-hashing unchanged media extents. The management API defaults to no-replace semantics so a stale browser cannot overwrite an existing target accidentally. Empty-directory removal follows ordinary MachaDFS `rmdir` semantics.

Unmatched hint paths are migrated with management-initiated namespace renames, while their immutable media identities remain unchanged.

A FUSE namespace operation wedged on a non-retryable backend error blocks the publication queue behind it, and is never skipped automatically. `GET /api/v1/manage/filesystem/blocked-namespace-operation` reports it (`sequence`, `kind`, `path`, optional `destination_path`, `error_code`, `error_message`, `blocked_for_ms`; `404 not_found` when nothing is blocked), and `POST /api/v1/manage/filesystem/blocked-namespace-operation/skip?sequence=N` abandons it (`204`; `409 not_blocked` unless `N` is the operation still blocked).

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

## Metadata conflicts

When two branches of the namespace changed the same path (or the catalogue
root) differently and are reconciled, the merge keeps the common-ancestor
value visible and records both alternatives as a first-class conflict. A
conflict leaves the snapshot in one of two ways: a later mutation of its
subject decides it (any write to or removal of the path, or a new catalogue
root — the later write *is* the resolution, and the record is pruned at the
next commit or merge), or an operator resolves it here.

- `GET /api/v1/manage/metadata/conflicts` → `{"generation": N, "conflicts": [{id, kind: "namespace_entry"|"catalogue_root", key, left_head, right_head, base, left, right}]}` — for a namespace entry `base`/`left`/`right` are `{type, size, mtime_ns, version, extents}` or `null` (absent on that side); for a catalogue root they are object ids or `null`.
- `POST /api/v1/manage/metadata/conflicts/{id}/resolve?choice=left|right|base` — installs that alternative for the subject and drops the record in one metadata commit (`204`; `409 not_standing` if the conflict is no longer standing; `400 bad_choice`). Both routes answer `503 metadata_unavailable` while no metadata snapshot is available.

`diagnostics.metadata.{conflicts, namespace_conflicts, catalogue_conflicts}`
in `GET /api/v1/status` are the standing counts;
`conflicts_superseded` and `conflicts_resolved` count the ones that left the
snapshot since this process started. `diagnostics.metadata.tombstones` is
the retirement-tombstone count carried in the snapshot.

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

## Accounts and roles

Every HTTP route requires a session bearer token from `POST /api/v1/session`. The only routes reachable without one are that route itself, `GET /api/v1/health`, the web client's static files, and capability URLs that carry their own authority in the path or query (playback stream URLs and signed artwork URLs). A session carries the roles of the account behind it, and each route is gated on those roles in one place before dispatch. Hiding a section in a client is presentation; the gate is the enforcement.

Roles are capabilities rather than a ladder — importing does not imply managing, and managing does not imply handing out accounts. There are exactly two implications: `importer`, `manager` and `manage_users` each imply `media_viewer`, and `media_viewer` implies `view_status`.

| role | grants |
| --- | --- |
| `view_status` | cluster and node health |
| `media_viewer` | every read, playback, and your own password |
| `importer` | acquisition and ingest |
| `manager` | files, namespaces, catalogue edits and matches, identity-association reset, status connectivity checks |
| `manage_users` | add, edit and remove accounts |

`view_status` is the weakest capability: everything implies it, it implies nothing, and it is grantable on its own. That is what makes cluster health independently addressable — an operator who wants it visible to unauthenticated visitors grants the `anonymous` account `view_status` and nothing else, while an account granted nothing at all cannot see health either. Implication is resolved when a session is minted rather than when the account is written, so a change to these rules reaches accounts created before it without a migration.

Two accounts exist on every cluster. `root` holds every role; `anonymous` is what an unauthenticated visitor is, and holds `media_viewer` at first. Neither can be renamed or deleted, and `anonymous` has no password and cannot be given one (`409 no_password`); in every other respect they are ordinary accounts. Anonymous access is controlled by editing the `anonymous` account's roles, not by configuration, so it takes effect on the next session rather than on restart — this is what decides what a television, which cannot practically type a password, is able to reach. `session.allow_anonymous: false` turns the mechanism off entirely.

At least one account always holds `manage_users`. Removing the role from the last account that has it, or deleting that account, is refused with `last_user_manager` — `root` included, whose roles are otherwise ordinary. The rule is about the role, not any particular account, so it moves as the role moves.

### Routes

- `GET /api/v1/users` — list. Credentials are write-only: this and every other route returns account records without password material.
- `POST /api/v1/users` — `{username, password, roles}`.
- `GET|PATCH|DELETE /api/v1/users/{id}` — `PATCH` accepts `password` and/or `roles`.
- `GET|PATCH /api/v1/users/me` — anyone's own account. `PATCH` accepts `password` only; a `roles` change here is `403`, since otherwise it would be an escalation route for every account. Changing your own password returns a fresh session in the same response (`token`, `token_type`, `session_id`), so you are not signed out by your own change. An anonymous session has no account here and gets `404 no_account`.

Every user record carries a `mutable` block stating what may be changed about it — `rename`, `delete`, `set_password`, `set_roles`, and `required_roles` for roles pinned to that account. Read it rather than testing the username: a client that hardcodes `root` breaks the moment these names are configurable, and disables the wrong controls everywhere at once.

A password change, a role change or a deletion retires every session that account had minted, on every node, as the record propagates. A role change does this deliberately: a session carries the roles it was minted with, so a demotion that left them alive would not take effect until they expired.

### Bootstrapping and recovery

A node founding a new cluster creates both accounts on first start and writes root's generated password to `<state_path>/initial-root-password`, mode 0600.

A node with bootstrap peers is joining rather than founding, so it creates neither. If the cluster it joins holds no accounts, it starts with an empty user table, which means nothing can sign in — the node says so at startup and reports `accounts_initialised: false` in `GET /api/v1/status`. Stop one node and run:

```
macha-users <state_path> <cluster.key> init
```

This performs exactly what a founding node performs. Start the node and the accounts replicate to the rest of the cluster.

If root's password is lost and no `manage_users` account can sign in, reset it the same way, with the node stopped:

```
macha-users <state_path> <cluster.key> passwd root
```

Recovery is deliberately offline only, through `macha-users` on a stopped node. An online recovery key or endpoint would have to be presentable without an account to be useful, which means a standing unauthenticated path to the most privileged account in the cluster; and anyone able to use it already has root on a node, where the command above does the same job. `macha-users` itself is not a weakness: it needs the node's state directory and the cluster key, which is root on a node — and that party already holds every byte in the cluster.

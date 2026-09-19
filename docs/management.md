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

## Metadata conflicts

When two branches of the namespace changed the same path (or the catalogue
root) differently and are reconciled, the merge keeps the common-ancestor
value visible and records both alternatives as a first-class conflict. A
conflict leaves the snapshot in one of two ways: a later mutation of its
subject decides it (any write to or removal of the path, or a new catalogue
root — the later write *is* the resolution, and the record is pruned at the
next commit or merge), or an operator resolves it here.

- `GET /api/v1/manage/metadata/conflicts` → `{"generation": N, "conflicts": [{id, kind: "namespace_entry"|"catalogue_root", key, left_head, right_head, base, left, right}]}` — for a namespace entry `base`/`left`/`right` are `{type, size, mtime_ns, version, extents}` or `null` (absent on that side); for a catalogue root they are object ids or `null`.
- `POST /api/v1/manage/metadata/conflicts/{id}/resolve?choice=left|right|base` — installs that alternative for the subject and drops the record in one metadata commit (`204`; `409 not_standing` if the conflict is no longer standing; `400 bad_choice`).

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

Every HTTP route requires a session bearer token from `POST /api/v1/session`, which is the only route reachable without one. A session carries the roles of the account behind it, and each route is gated on those roles in one place before dispatch. Hiding a section in a client is presentation; the gate is the enforcement.

Roles are capabilities rather than a ladder — importing does not imply managing, and managing does not imply handing out accounts. There are exactly two implications: `importer`, `manager` and `manage_users` each imply `media_viewer`, and `media_viewer` implies `view_status`.

| role | grants |
| --- | --- |
| `view_status` | cluster and node health |
| `media_viewer` | every read, playback, and your own password |
| `importer` | acquisition and ingest |
| `manager` | files, namespaces, catalogue matches, identity-association reset |
| `manage_users` | add, edit and remove accounts |

`view_status` is the weakest capability: everything implies it, it implies nothing, and it is grantable on its own. That is what makes cluster health independently addressable — an operator who wants it visible to unauthenticated visitors grants the `anonymous` account `view_status` and nothing else, while an account granted nothing at all cannot see health either. Implication is resolved when a session is minted rather than when the account is written, so a change to these rules reaches accounts created before it without a migration.

Two accounts exist on every cluster. `root` holds every role; `anonymous` is what an unauthenticated visitor is, and holds `media_viewer` at first. Neither can be renamed or deleted, and in every other respect they are ordinary accounts. Anonymous access is controlled by editing the `anonymous` account's roles, not by configuration, so it takes effect on the next session rather than on restart — this is what decides what a television, which cannot practically type a password, is able to reach. `session.allow_anonymous: false` turns the mechanism off entirely.

At least one account always holds `manage_users`. Removing the role from the last account that has it, or deleting that account, is refused with `last_user_manager` — `root` included, whose roles are otherwise ordinary. The rule is about the role, not any particular account, so it moves as the role moves.

### Routes

- `GET /api/v1/users` — list. No password material is ever returned; there is no route that reads a credential back.
- `POST /api/v1/users` — `{username, password, roles}`.
- `GET|PATCH|DELETE /api/v1/users/{id}` — `PATCH` accepts `password` and/or `roles`.
- `GET|PATCH /api/v1/users/me` — anyone's own account. `PATCH` accepts `password` only; a `roles` change here is `403`, since otherwise it would be an escalation route for every account. Changing your own password returns a fresh token in the same response, so you are not signed out by your own change.

Every user record carries a `mutable` block stating what may be changed about it — `rename`, `delete`, `set_password`, `set_roles`, and `required_roles` for roles pinned to that account. Read it rather than testing the username: a client that hardcodes `root` breaks the moment these names are configurable, and disables the wrong controls everywhere at once.

A password change, a role change or a deletion retires every session that account had minted, on every node, as the record propagates. A role change does this deliberately: a session carries the roles it was minted with, so a demotion that left them alive would not take effect until they expired.

### Bootstrapping and recovery

A node founding a new cluster creates both accounts on first start and writes root's generated password to `<state_path>/initial-root-password`, mode 0600.

An existing cluster upgrading into the accounts system does not, because it has bootstrap peers and is therefore not founding anything. Such a node starts with an empty user table, which means nothing can sign in — the node says so at startup and reports `accounts_initialised: false` in `GET /api/v1/status`. Stop one node and run:

```
macha-users <state_path> <cluster.key> init
```

This performs exactly what a founding node performs. Start the node and the accounts replicate to the rest of the cluster.

If root's password is lost and no `manage_users` account can sign in, reset it the same way, with the node stopped:

```
macha-users <state_path> <cluster.key> passwd root
```

There is deliberately no recovery key and no recovery endpoint. One would have to be presentable without an account to be useful, which means a standing unauthenticated path to the most privileged account in the cluster; and anyone able to use it already has root on a node, where the command above does the same job. `macha-users` itself is not a weakness: it needs the node's state directory and the cluster key, which is root on a node — and that party already holds every byte in the cluster.

# Torrents belong to the cluster -- plan (2026-09-27)

Operator decisions, 2026-09-27:

1. A torrent is added to the **cluster**, queues there, and is downloaded by
   **any capable node**. The queue lives in cluster metadata.
2. Design the whole thing, and the whole API, up front; ask Core and the
   clients for blockers before building, so they refactor once.
3. Removal after completion is **off by default**, and when on it takes a
   **delay from 0 (at once) to 24 h**.

Earlier the same day the operator also asked for: job lists served by the
answering node from memory (no per-request peer survey), the node a torrent
runs on shown in the API, a choice of node when adding, and a route listing
the torrent-capable nodes for a selector. All of that is folded in below.

## Why

- **Reads wait on the WAN.** Every `GET /api/v1/torrents/jobs` (and ingest's,
  and every lookup of a job the answering node does not hold) surveys every
  peer synchronously. On 2026-09-27 that was 0.1-0.5 s against gbni-1 and
  1-5 s earlier the same day, all of it fi-1's round trip -- and fi-1 cannot
  even run torrents (`torrent.enabled: false`).
- **A job belongs to one node.** It lives in that node's `jobs.json`, so a
  node without the torrent plugin answers every `/torrents/*` route 503, a
  torrent cannot move when its node dies, duplicate detection is per node,
  and where a download runs is decided by which address the client used.
- **Nothing clears a finished job**, and clearing by hand blocks the request
  for 6-34 s on the payload delete (measured 2026-09-27, stacks in the
  session notes) and deletes the catalogue's own failed hints with it
  (Colony S02E13, now uncatalogued with no hint left to retry).

## The model

Three layers, each with one job.

**1. The request: durable, cluster-wide, in metadata.** A new snapshot
collection `torrent_requests`, keyed by request id, holding only what must
survive any node and changes rarely:

| field | written by | notes |
|---|---|---|
| `id` | adder | 128-bit random hex, the job id clients already use |
| `info_hash` | adder | v1 hex, else v2; the cluster-wide duplicate key |
| `source` | adder | canonical magnet; for a `.torrent` search result, the metainfo bytes as a CONTROL object id (never the fetch URL -- it may carry credentials) |
| `name` | adder, then owner | display name from the magnet `dn`, replaced by the metainfo name once known |
| `pinned_node_id` | adder | optional: only this node may claim it |
| `remove_after_ms` | adder, PATCH | null = never; 0..86 400 000 |
| `bytes_total` | owner | once metadata is known; lets schedulers skip a torrent that cannot fit them |
| `claim` | scheduler | `{node_id, epoch, claimed_unix_ms}` |
| `desired` | API | `active` / `paused` / `cancelled` -- operator intent |
| `phase` | owner | `awaiting_node`, `downloading`, `importing`, `completed`, `failed`, `cancelled` |
| `ingest_job_id`, `error_code`, `error` | owner | |
| `created_unix_ms`, `completed_unix_ms` | adder / owner | |

About five metadata writes per torrent (add, claim, hand-off to ingest,
completed, removed). Progress, rates, peers and the libtorrent sub-states
(`metadata`, `verifying`, ...) never go into metadata: each metadata write
costs seconds and, with two replicas at a floor of two, needs both nodes.

**Merges are deterministic, never operator conflicts.** Metadata writes are
not compare-and-swap (docs/metadata.md): concurrent writes merge afterwards,
and history can branch during a partition. So `torrent_requests` merges
per request as a join:

- immutable fields never differ (same id, same adder);
- `desired`: `cancelled` beats everything; otherwise the later write wins,
  by the request's own `desired_changed_unix_ms`, ties broken by node id;
- `phase` is monotonic -- `awaiting_node < downloading < importing <
  completed`, with `failed` and `cancelled` terminal -- and a merge takes
  the furthest, except that a claim with a higher epoch resets it;
- `claim`: the higher epoch wins; within an epoch, the earlier
  `claimed_unix_ms`, then the lower node id. The node whose claim lost stops
  its download and deletes its staging at once.

A partition in a cluster of three or more can therefore let two nodes claim
the same request, both download, and -- if both reach import before the
branches meet -- both import, the second landing as `... (2).mkv`. That needs
a partition plus a race; it wastes bandwidth and leaves a duplicate file, and
never loses or corrupts anything. Two mitigations make it rarer: a node
imports only while its claim still wins in its own current head, and the
claim ranking below keeps healthy nodes from racing at all.

**2. The claim: a scheduler on every capable node.** Each node running the
torrent subsystem watches `torrent_requests` in its local snapshot. A request
is claimable when `phase` is `awaiting_node`, `desired` is `active`, the
node is not excluded by `pinned_node_id`, and the node has a free slot
(`torrent.max_active`) and staging room for `bytes_total` when known. To keep
healthy nodes from racing, capable nodes rank each request by rendezvous hash
of (request id, node id); a node claims at once when it is the best-ranked
capable node with room in its own view, and otherwise only after
`rank x 30 s` with the request still unclaimed.

A claim holds while its node is an active member. A node absent from
membership for `torrent.claim_lease` (default 10 min, so a restart or a
brief wifi drop does not move a download) loses it: any capable node may
claim with `epoch + 1`, and starts from zero -- staging and resume data are
node-local. A node that returns to find its claim superseded deletes its
staging for that request.

A torrent whose size turns out larger than the claiming node's whole
`ingest.staging_limit` releases its claim (`phase` back to `awaiting_node`,
`bytes_total` recorded) so a node with more room can take it; if no capable
node could ever hold it, it fails with `staging_too_small`.

**3. Live progress: in memory, on every node.** Each node keeps every peer's
live torrent state (bytes, rates, peers, the libtorrent sub-state) in
memory, refreshed by a background loop over the existing `get_torrent_jobs`
RPC every 5 s, and merges it with the metadata record at read time. A node
learns which peers are torrent-capable from the same replies: a node without
the plugin already answers "torrents not available". The same loop holds
every peer's ingest jobs, so `GET /api/v1/ingest/jobs` stops surveying too.

**Capability and room in gossip.** Schedulers and the nodes route need each
capable node's free staging and active-slot count without asking; both ride
the existing telemetry gossip as two new fields.

## Execution on the owner

The owning node's TorrentManager is driven by its own claims instead of by
`POST` calls: a claim becomes a local job (libtorrent add, held or released
per `desired`), and the local job's phase transitions are written back to the
request. Everything 0.63.0 made structural stays: one job per info hash, one
retire path, per-job fault isolation, escalation to the supervisor.

`jobs.json` becomes the owner's execution cache (resume data, save path, the
ingest link), no longer the record of what exists. At upgrade each node
publishes its existing jobs as requests already claimed by itself, one
metadata write.

## Removal after completion

`completed` means the ingest completed and the catalogue has settled (no
hint of that ingest still `queued`, `processing` or `deferred`). A request
with `remove_after_ms` set is removed at `completed_unix_ms +
remove_after_ms` by its owner: the torrent job, the ingest job and the
staging payload go, and the request leaves metadata. `failed` and `cancelled`
requests are never removed automatically.

Default: `torrent.remove_on_complete_after`, **null (off)**; a per-request
`remove_after_ms` overrides it, 0 to 86 400 000.

Two prerequisites, both needed for manual clear too:

- **The payload delete leaves the request thread.** Clear and cancel mark
  the payload for deletion and return; a persisted deletion queue on the
  owner removes it, retrying until it is gone. Staging keeps counting it
  against `ingest.staging_limit` until it really is.
- **Clearing a job no longer deletes catalogue hints.** A hint's origins are
  provenance, not ownership; hints retire by the catalogue's own rules.

## The API (0.64.0)

Every route works on every node, answered from the local snapshot and the
in-memory view; none surveys peers at request time.

`POST /api/v1/torrents/jobs`

```json
{"magnet": "magnet:?xt=...", "node_id": null, "remove_after_ms": null}
```

`acquisition_ref` as today instead of `magnet`. `node_id` pins the request
to one node (it must be torrent-capable); absent or null lets the cluster
choose. `remove_after_ms`: absent copies the cluster default into the request
(so the stored value is always explicit); null means never; otherwise
0..86400000. Answers `202`:

```json
{"status": "ok", "id": "...", "info_hash": "...", "node_id": null, "job": {...}}
```

`node_id` is the pin, null when the cluster chooses; `job` is the full job.
The 202 is sent only once the request is accepted into metadata, which puts
it on `metadata_min_write_replicas` nodes, so a list from any node that has
adopted that head includes it (read-your-writes across nodes). Refusals:

| status | code | `error.reason` | when |
|---|---|---|---|
| 409 | `torrent_already_added` | | a request for this info hash exists anywhere in the cluster (any phase, until removed); `id` names it, `node_id` its owner or null while unclaimed |
| 409 | `placement_failed` | `node_not_member` / `node_not_torrent_capable` | the pin cannot run torrents |
| 400 | `bad_request` | | `remove_after_ms` outside 0..86400000 |
| 503 | `metadata_unavailable` | | metadata is not writable (with two replicas and a floor of two, either node down); `error.scope` `cluster`, `alternative_may_succeed` false |

`GET /api/v1/torrents/jobs` and `GET /api/v1/torrents/jobs/{id}` -- each job:

```json
{"id": "...", "info_hash": "...", "name": "...",
 "phase": "downloading", "state": "downloading",
 "desired": "active", "desired_changed_unix_ms": 0, "desired_applied": true,
 "desired_blocked_reason": null,
 "node_id": "a3c9...", "pinned_node_id": null,
 "bytes_total": 0, "bytes_completed": 0, "progress": null,
 "download_rate": 0, "upload_rate": 0, "peers": 0, "seeds": 0, "eta_seconds": null,
 "live_as_of_unix_ms": 1790000000000,
 "remove_after_ms": null, "remove_at_unix_ms": null,
 "ingest_job_id": null, "catalogue": {...},
 "error_code": null, "error": null,
 "created_unix_ms": 0, "completed_unix_ms": null, "updated_unix_ms": 0}
```

- `phase` is the cluster-level position (from metadata) and always present.
  `state` is today's fine-grained state, from the owner's live view, and
  reads `awaiting_node` while no node has claimed it.
- `node_id` is the node running it, null while unclaimed.
- `desired` is operator intent. The owner applies it within one
  `refresh_interval_ms` of seeing it and sets `desired_applied`; still
  false after `2 x refresh_interval_ms` means a stale pending indicator.
- `desired_blocked_reason` says when nothing will act on the intent:
  `owner_unreachable` (the owner is not reachable from the answering node),
  `pinned_node_unavailable` (pinned to a node that is not an active
  torrent-capable member), `no_capable_node` (unclaimed and no capable node
  is accepting); null when the intent will be applied.
- `state` is `awaiting_node` exactly when `phase` is.
- The live fields are null when the owner's live view is unavailable, and
  `live_as_of_unix_ms` says how old they are.
- The list response adds `"refresh_interval_ms": 5000`.

Actions `POST /api/v1/torrents/jobs/{id}/{pause|resume|retry|cancel|clear}`
record intent and answer **202** with the full job (`desired` updated), not
200 with the new state: the owner applies it asynchronously. `clear` answers
`202 {"status": "ok", "cleared": true}`. An unclaimed request can be paused,
cancelled or cleared.

With metadata unwritable, pause/resume/cancel on a **claimed** job still
work: the answering node forwards to the owner, which applies at once and
journals the intent until metadata is writable, then publishes it. Adds,
PATCH, and actions on unclaimed requests answer `503 metadata_unavailable`.

`PATCH /api/v1/torrents/jobs/{id}` with `{"remove_after_ms": ...}` changes
removal on an existing request; with `{"node_id": "..."|null}` it re-pins
or unpins a request that is still `awaiting_node` (otherwise `409
invalid_state`). A request pinned to a node that has left stays
`awaiting_node` with `desired_blocked_reason` `pinned_node_unavailable`
until it is re-pinned, unpinned or cancelled.

`GET /api/v1/torrents/nodes` -- the torrent-capable nodes, for a selector:

```json
{"status": "ok", "refresh_interval_ms": 5000, "default_remove_after_ms": null,
 "nodes": [{"node_id": "...", "host": "...", "local": true, "reachable": true,
            "as_of_unix_ms": 0, "max_active": 4, "active_jobs": 1,
            "accepting": true, "not_accepting_reason": null,
            "staging": {"limit_bytes": 0, "disk_bytes": 0, "reserved_bytes": 0, "free_bytes": 0}}]}
```

Only torrent-capable nodes are listed. `default_remove_after_ms` is
`torrent.remove_on_complete_after` (null = off), what an add without
`remove_after_ms` gets. `not_accepting_reason`: `slots_full`, `staging_full` or `draining`
(`torrent.accept_new_jobs: false`: the node keeps its claims, takes no new
ones, and stays listed).

`GET /api/v1/ingest/jobs` keeps its shape, served from the in-memory view,
plus `"sources": [{"node_id", "local", "reachable", "as_of_unix_ms"}]` so a
node whose jobs are stale or missing is visible rather than silently absent.
`reachable: false` means that node's jobs are shown from the last successful
poll at `as_of_unix_ms` (possibly stale); a node never reached contributes no
jobs and has `as_of_unix_ms` null.

## Build order

1. **In-memory peer view** (ingest and torrent live state), capability
   learned from replies, `/torrents/nodes`, ingest list from memory.
2. **Metadata collection** `torrent_requests` with its join merge; delta and
   snapshot encoding (next DLT version, cluster protocol 22, all nodes
   upgrade together); migration of existing jobs; add/list/detail from
   metadata.
3. **Schedulers, claims, leases, failover**; the owner driven by claims;
   actions as intent.
4. **Asynchronous payload deletion; hints kept on clear; removal after
   completion**; the catalogue batch race fixed alongside (a hint enqueued
   after its batch's snapshot waits for the next batch).

Each step ships with tests; 2 and 3 with fault-injection tests (branching
claims merge to one owner; a dead owner's request moves after the lease; a
superseded owner deletes its staging).

## Open questions for the clients

Sent to Core, web, Android TV and mobile, 2026-09-27; answers below.

- **Mobile** (2026-09-27): no blockers. It has no torrent or ingest code and
  reads status and playback only, through core.
- **Core** (2026-09-27): no blockers. Core reads lists from one node and sends
  each action to one node, so lists answered by any node fit, and dropping the
  503 from plugin-less nodes removes a workaround. Its points, and what the
  design now says:
  - *One node down blocks pause and cancel, even for a job running on the
    surviving node.* The owning node applies pause/cancel/resume to a job it
    has claimed **at once**, and journals the intent locally until metadata
    is writable, then publishes it. The `desired` merge (cancel dominates,
    otherwise the later `desired_changed_unix_ms`) takes a late write
    correctly. Any other node forwards the action to the owner. Adds, claims,
    PATCH and actions on unclaimed requests still need metadata; their
    `503 metadata_unavailable` carries `scope: cluster` and
    `alternative_may_succeed: false`.
  - *Absent vs null `remove_after_ms` would diverge if the default changed.*
    Absent copies the cluster default into the request at add time, so every
    stored value is explicit; no `remove_after_source` field is needed.
  - *Keep the full job on 202.* Yes.
  - *`state` `awaiting_node` must coincide with phase `awaiting_node`.* By
    construction.
  - *When has a pending indicator gone stale?* Jobs carry
    `desired_changed_unix_ms` and `desired_applied` (the owner has acted on
    the current `desired`). An owner applies within one
    `refresh_interval_ms` of seeing the intent; not applied after
    `2 x refresh_interval_ms` means something is wrong.
  - *Selector: `accepting`, and draining rather than disappearing.* Each node
    carries `accepting` and `not_accepting_reason` (`slots_full`,
    `staging_full`, `draining`); `torrent.accept_new_jobs: false` drains a
    node (it keeps its claims, takes no new ones) and it stays listed.
- **Web client** (2026-09-27): no hard blockers. Settled: the add's 202
  returns only after metadata acceptance and carries the full job
  (read-your-writes across nodes); action 202s carry the full job;
  `desired_blocked_reason` (`owner_unreachable`, `pinned_node_unavailable`,
  `no_capable_node`) so a pending indicator can say it is stuck;
  `default_remove_after_ms` on `/torrents/nodes`; nodes lists only capable
  nodes; PATCH `node_id` re-pins/unpins while `awaiting_node`; the 409
  carries `node_id` (null when unclaimed); `sources[].reachable: false` means
  shown-but-stale. The web will follow `refresh_interval_ms` (polls every
  1.5 s today) and show unknown live fields as unknown, not 0.
- **Android TV** (2026-09-27): no blockers. It calls no `/torrents/*` route
  and no `/ingest/jobs`; ingest is out of scope for the TV by operator ruling.

**Spec final 2026-09-27. All four phases built the same day as 0.64.0**
(`ClusterJobView`, `TorrentCoordinator`, `torrent_request.{hpp,cpp}`,
SM15/SM16/DLT9, protocol 22, staging trash, hint fixes). Differences from
the spec above, to announce with the release: `not_accepting_reason` also
reads `unreachable`; `error.scope` gains the value `cluster`; PATCH answers
200; a claimed job with no live view reads `state` = its phase name; new
job `error_code` `adopt_failed`; new hint `error_code`
`path_not_yet_visible`; the config key is
`torrent.remove_on_complete_after_ms`.

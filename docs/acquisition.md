# Acquisition and ingest

Two kinds of job bring media into Macha. An **ingest job** copies files from a local path into the MachaDFS namespace and hands them to the catalogue. A **torrent job** downloads a BitTorrent payload into the node's staging area and, once it has finished, submits that payload as an ingest job of its own. Both are served under `/api/v1/ingest/` and `/api/v1/torrents/`, and both are cluster-wide: any node lists and acts on every node's jobs.

The server reports state and codes only. The order of any list, how progress is shown and what a code means to a person are the client's business.

## Roles, errors and the envelope

Every route needs a session bearer token (see [Accounts and roles](management.md#accounts-and-roles)). The role is decided by method alone:

| method | role |
|---|---|
| `GET` | `media_viewer` |
| `POST` | `importer` |

A missing role is `403 forbidden`.

**Every JSON object response carries a top-level snake_case `status`.** A success has `"status": "ok"`. An error has the error's code as its `status` and an `error` object beside it:

```json
{
  "status": "invalid_state",
  "error": {
    "code": "invalid_state",
    "message": "job cannot perform that action in its current state"
  }
}
```

Act on `error.code`. The `message` is English text for people and may change. Some errors add `error.reason`, a second snake_case code that says why; it is listed below wherever a route uses it.

These errors can come from any route here, before the route itself runs:

| status | code | when |
|---|---|---|
| 401 | `unauthorized` | no valid bearer token |
| 403 | `forbidden` | the session lacks the role |
| 503 | `service_recovering` | the node's local services are still recovering |
| 503 | `startup_failed` | the node's startup failed |
| 503 | `overloaded` | the node is busy (`status` is `busy`, with `Retry-After: 1`) |
| 400 | `bad_json` | the request body is not valid JSON |
| 400 | `bad_request` | the body is not a JSON object, a field has the wrong type, or the operation threw (the message says which) |
| 404 | `not_found` | no such route under `/api/v1/ingest/` or `/api/v1/torrents/` |

An empty request body counts as `{}`.

## Ingest

```text
GET  /api/v1/ingest/status
GET  /api/v1/ingest/jobs
POST /api/v1/ingest/jobs
GET  /api/v1/ingest/jobs/{id}
POST /api/v1/ingest/jobs/{id}/pause
POST /api/v1/ingest/jobs/{id}/resume
POST /api/v1/ingest/jobs/{id}/cancel
POST /api/v1/ingest/jobs/{id}/clear
```

### Status

`GET /api/v1/ingest/status` describes the ingest engine **on the node that answers**:

```json
{
  "status": "ok",
  "enabled": true,
  "cleanup": {
    "delete_owned_source_on_clear": true,
    "delete_external_source_on_clear": false,
    "delete_owned_source_on_cancel": true
  },
  "staging": {
    "path": "/var/lib/macha/tmp/ingest",
    "limit_bytes": 107374182400,
    "disk_bytes": 5368709120,
    "reserved_bytes": 2147483648,
    "accounted_bytes": 7516192768
  },
  "concurrency": {
    "max_jobs": 10,
    "active_jobs": 1,
    "peak_active_jobs": 3
  }
}
```

| field | meaning |
|---|---|
| `enabled` | `ingest.enabled` on this node |
| `cleanup.*` | the configured source-cleanup policy (see [Clear and cancel](#what-clear-and-cancel-delete)) |
| `staging.path` | the staging directory |
| `staging.limit_bytes` | `ingest.staging_limit` |
| `staging.disk_bytes` | bytes currently on disk under the staging directory |
| `staging.reserved_bytes` | bytes reserved by running downloads for what they have still to fetch |
| `staging.accounted_bytes` | `disk_bytes + reserved_bytes` |
| `concurrency.max_jobs` | `ingest.max_concurrent_jobs` |
| `concurrency.active_jobs` | jobs a worker holds right now |
| `concurrency.peak_active_jobs` | the highest `active_jobs` since this process started |

`active_jobs` is what tells a queue that is stalled behind a held job apart from an idle one: both show every job `queued`, but only the stalled one has workers busy.

### Submit a job

`POST /api/v1/ingest/jobs`

```json
{
  "path": "/mnt/import/Some Film (1999)",
  "display_name": "Some Film",
  "delete_source_on_clear": false
}
```

| field | required | meaning |
|---|---|---|
| `path` | yes | a file or directory on the node that receives the request |
| `display_name` | no | defaults to the last component of the resolved path |
| `delete_source_on_clear` | no | overrides `ingest.cleanup.delete_external_source_on_clear` for this job. `remove_source` is accepted as an alias |

The job is created on the node that receives the request, because the path is that node's.

The response is `202`:

```json
{"status": "ok", "id": "4f1c0e9a2b7d4c6e8a1f3b5d7c9e0a12"}
```

| status | code | when |
|---|---|---|
| 400 | `bad_request` | `path` is missing or empty; ingest is disabled on this node; the resolved path is outside `ingest.source_roots`; the path does not exist |

These four share one code; only the message tells them apart.

### List and inspect

`GET /api/v1/ingest/jobs` answers with every ingest job in the cluster as the answering node knows it (see [Cluster-wide behaviour](#cluster-wide-behaviour)):

```json
{"status": "ok", "jobs": [...], "refresh_interval_ms": 5000,
 "sources": [{"node_id": "...", "local": true, "reachable": true, "as_of_unix_ms": 1790000000000}]}
```

Each item is an [ingest job](#the-ingest-job-object) without `files` and without `catalogue.items`, plus `node_id`. `sources` says where the jobs came from and how old they are: the answering node's own are live (`local: true`); another node's are from its last poll, at most about `refresh_interval_ms` old. `reachable: false` means that node's jobs are shown from its last successful poll at `as_of_unix_ms` and may be stale; a node never reached contributes no jobs and has `as_of_unix_ms` null.

`GET /api/v1/ingest/jobs/{id}` answers the one job, with `files` and `catalogue.items` when the answering node owns it, plus `node_id`. `404 not_found` if no node it has heard from has it.

### Actions

`POST /api/v1/ingest/jobs/{id}/{action}`, where `action` is `pause`, `resume`, `cancel` or `clear`. No body.

- `pause`, `resume` and `cancel` answer `200` with the job as it now stands on the node that owns it, including `node_id`. That is the owning node's own answer, not a guess by the node that relayed it.
- `clear` removes the job and answers `200` with `{"status": "ok", "cleared": true}`.

| status | code | when |
|---|---|---|
| 404 | `not_found` | the action is not one of the four, or no reachable node has the job |
| 409 | `invalid_state` | the job exists but the action is not allowed in its state (see the table below) |
| 400 | `bad_request` | the action failed on this node, for example a source it was told to delete could not be removed |

## The ingest job object

```json
{
  "id": "4f1c0e9a2b7d4c6e8a1f3b5d7c9e0a12",
  "node_id": "a3c95e0f7d2b41e8b6c4d0f19e7a2b58",
  "source_type": "filesystem",
  "source_ref": null,
  "display_name": "Some Film",
  "source_path": "/mnt/import/Some Film (1999)",
  "source_owned": false,
  "delete_source_on_clear": false,
  "state": "importing",
  "bytes_total": 2147483648,
  "bytes_completed": 536870912,
  "files_total": 2,
  "files_completed": 0,
  "rate_bytes_per_second": 41943040,
  "eta_seconds": 39,
  "progress": 0.25,
  "current_file": "/mnt/import/Some Film (1999)/Some Film.mkv",
  "current_destination": "/Movies/Some Film (1999)/Some Film.mkv",
  "catalogue": {
    "total": 0,
    "pending": 0,
    "catalogued": 0,
    "no_match": 0,
    "failed": 0,
    "state": "waiting"
  },
  "created_unix_ms": 1790000000000,
  "updated_unix_ms": 1790000042000,
  "error_code": null,
  "error": null
}
```

| field | type | meaning |
|---|---|---|
| `id` | string | 32 hex characters |
| `node_id` | string | the node that owns and runs the job |
| `source_type` | string | `filesystem` for a submitted path, `torrent` for a torrent's payload |
| `source_ref` | string or null | for `torrent`, the torrent job's `id`; otherwise null |
| `display_name` | string | |
| `source_path` | string | the resolved source path on the owning node |
| `source_owned` | bool | the source is Macha's own (a torrent payload in staging) rather than the operator's |
| `delete_source_on_clear` | bool | whether `clear` deletes the imported source (see below) |
| `state` | string | see [Ingest job states](#ingest-job-states) |
| `bytes_total`, `bytes_completed` | integer | bytes to copy and bytes copied. Zero until the source has been scanned |
| `files_total`, `files_completed` | integer | files in the plan and files committed |
| `rate_bytes_per_second` | integer | smoothed copy rate; 0 when not copying |
| `eta_seconds` | integer or null | null when there is no rate to estimate from. `rate_bytes_per_second` and `eta_seconds` are not persisted, so they read 0 and null after the owning node restarts until copying resumes |
| `progress` | number or null | `bytes_completed / bytes_total`, capped at 1; null while `bytes_total` is 0 |
| `current_file`, `current_destination` | string or null | the file being copied and where it goes; null when none |
| `catalogue` | object | see below |
| `created_unix_ms`, `updated_unix_ms` | integer | milliseconds since the Unix epoch |
| `error_code` | string or null | why the job is `blocked` or `failed`; null otherwise |
| `error` | string or null | the English message beside `error_code` |
| `files` | array | single-job `GET` only; see below |

`catalogue` counts the job's imported files through the catalogue:

| field | meaning |
|---|---|
| `total`, `pending`, `catalogued`, `no_match`, `failed` | file counts |
| `state` | `processing` while the job is `cataloguing` or any file is pending; otherwise `waiting` if nothing has been counted and the job is not `completed`; otherwise `completed_with_issues` if any file failed or matched nothing; otherwise `completed` |
| `items` | single-job `GET` only, and only filled when the answering node owns the job; for a job on another node it is an empty array |

Each `catalogue.items` entry:

| field | type | meaning |
|---|---|---|
| `id` | string | the catalogue hint |
| `path` | string | the imported file |
| `state` | string | `queued`, `processing`, `deferred`, `catalogued`, `no_match`, `failed` |
| `provider`, `media_id`, `result` | string or null | the match, when there is one |
| `priority` | integer | |
| `attempts` | integer | |
| `error_code`, `error` | string or null | why this file failed |
| `catalogue_item_ids` | array of strings | catalogue items the file became |

Destinations are compared ignoring case: a folder that already exists under another spelling is reused as spelt (a lowercase release goes into `/Movies/The Martian (2015)/`, not a new `/Movies/the martian (2015)/`), and a file whose name differs from one already there only by case gets a ` (2)` suffix like any other collision.

Each `files` entry: `source_path`, `destination_path` (strings), `size`, `copied` (integers), `completed`, `skipped`, `catalogue_candidate` (bools). A file that is not a catalogue candidate (a sidecar) is imported but not sent to the catalogue.

### Ingest job states

| state | meaning | terminal |
|---|---|---|
| `queued` | waiting for a worker | no |
| `scanning` | building the file plan from the source | no |
| `importing` | copying files into the namespace | no |
| `cataloguing` | every file copied; waiting for the catalogue to finish with them | no |
| `paused` | stopped by an operator | no |
| `blocked` | stopped by a condition that may pass; **retried by itself** | no |
| `completed` | copied and catalogued | yes |
| `cancelled` | stopped by an operator, partial files removed | yes |
| `failed` | stopped by an error that retrying the same job will not fix | yes (`resume` restarts it) |

What moves a job:

- A worker picks up a `queued` job, or a `blocked` job whose `updated_unix_ms` is at least `ingest.blocked_retry_ms` old. At most `ingest.max_concurrent_jobs` run at once.
- A job with no file plan goes to `scanning`, then `importing`. A job that already has a plan (a resumed or retried one) goes straight to `importing` and continues from the files and bytes already committed.
- When every file is copied the job goes to `cataloguing` if the catalogue still has any of them pending, otherwise straight to `completed`. A `cataloguing` job becomes `completed` when nothing is pending.
- A job that was `scanning` or `importing` when its node stopped is `queued` again when the node starts.

How the owner's job responds to each action, once the owner applies it (the API's own rules are under [Actions](#actions-1)):

| action | allowed from | result |
|---|---|---|
| `pause` | `queued`, `scanning`, `importing`, `paused`, `blocked` | `paused` |
| `resume` | `paused`, `blocked`, `failed` | `queued`, with `error_code` and `error` cleared |
| `cancel` | anything except `completed` and `cancelled` | `cancelled` |
| `clear` | `completed`, `cancelled`, `failed` | the job is removed |

`resume` on a `blocked` job does not wait for `ingest.blocked_retry_ms`.

### Ingest error codes

A `blocked` job is retried every `ingest.blocked_retry_ms` until it succeeds or an operator acts. Every blocked code recovers by itself when its cause passes:

| `error_code` (blocked) | cause |
|---|---|
| `metadata_unavailable` | cluster metadata is not writable (no metadata quorum, or a DATA or CONTROL retention floor not met) |
| `source_unavailable` | the source path does not exist |
| `source_not_regular` | the source is neither a regular file nor a directory |
| `source_scan_interrupted` | the scan could not finish; the source may be unavailable |
| `source_changed_during_scan` | the source changed or disappeared while it was scanned |
| `source_disappeared` | a source file disappeared while it was copied |
| `source_changed` | a source file changed while it was copied |
| `source_unreadable` | a source file could not be opened for reading |
| `source_seek_failed` | a source file could not be positioned to resume a copy |
| `source_short_read` | a source file returned fewer bytes than expected |

`metadata_unavailable` makes the job `blocked`, and the job completes once metadata is writable again.

| `error_code` (failed) | cause |
|---|---|
| `source_is_symlink` | the source is a symbolic link |
| `no_supported_media` | the source contains no supported media |
| `destination_parent_not_directory` | a component of the destination path exists and is not a directory |
| `partial_not_file` | the job's partial file in the namespace is not a file |
| `destination_conflict` | the destination appeared with an unexpected type or size |
| `namespace_short_write` | a write into the namespace was short |
| `size_mismatch` | the committed file's size differs from the source |
| `filesystem_error` | any other namespace error |
| `import_failed` | any other error; also given to jobs recorded as failed before error codes existed |

### What clear and cancel delete

`clear` always removes the job's partial files and its catalogue hints. It deletes the source only when `delete_source_on_clear` is true **and** the job is `completed` or its source is owned. For an external (operator's) source it deletes only the files the job actually imported, then any directories under the submitted path that this left empty; files it did not recognise are left in place.

`cancel` removes partial files. For an owned source it also deletes the source when `ingest.cleanup.delete_owned_source_on_cancel` is true. An external source is never deleted by `cancel`.

## Torrents

```text
GET  /api/v1/torrents/status
GET  /api/v1/torrents/search?q=...
GET  /api/v1/torrents/nodes
GET  /api/v1/torrents/jobs
POST /api/v1/torrents/jobs
GET  /api/v1/torrents/jobs/{id}
POST /api/v1/torrents/jobs/{id}/pause
POST /api/v1/torrents/jobs/{id}/resume
POST /api/v1/torrents/jobs/{id}/retry
POST /api/v1/torrents/jobs/{id}/cancel
POST /api/v1/torrents/jobs/{id}/clear
```

The download engine is the `libmacha-torrent` subsystem plugin. **Every route answers on every node**, whether or not the plugin runs there: lists and lookups come from the answering node's view of the cluster, and actions and adds go to the node concerned. Only an add that must run on the answering node itself needs the plugin there; see [When the torrent subsystem is not running](#when-the-torrent-subsystem-is-not-running).

### Torrent-capable nodes

`GET /api/v1/torrents/nodes` lists the nodes that run the torrent subsystem, for choosing where an add goes:

```json
{"status": "ok", "refresh_interval_ms": 5000, "default_remove_after_ms": null,
 "nodes": [{"node_id": "...", "host": "...", "local": false, "reachable": true,
            "as_of_unix_ms": 1790000000000, "accepting": true, "not_accepting_reason": null,
            "max_active": 4, "active_jobs": 1,
            "staging": {"limit_bytes": 0, "disk_bytes": 0, "reserved_bytes": 0,
                        "accounted_bytes": 0, "free_bytes": 0}}]}
```

A node without the plugin is not listed. `accepting` is whether it would take a new job now; when false, `not_accepting_reason` is `slots_full` (`active_jobs` has reached `max_active`), `staging_full` (no staging room left), `draining` (`torrent.accept_new_jobs: false`: it keeps its jobs and takes no new ones) or `unreachable`. Another node's figures are from its last poll (`as_of_unix_ms`). `default_remove_after_ms` is `torrent.remove_on_complete_after_ms`, null when off. Node names are not here: join `node_id` with `nodes[]` from `GET /api/v1/status`.

### Status

`GET /api/v1/torrents/status`, about the node that answers:

```json
{"status": "ok", "enabled": true, "build_available": true, "search_enabled": true}
```

| field | meaning |
|---|---|
| `enabled` | the plugin is running here and `torrent.enabled` is on |
| `build_available` | the plugin is loaded and running here |
| `search_enabled` | at least one search provider is configured and enabled on this node |

### Search

`GET /api/v1/torrents/search?q=some+film`

```json
{
  "status": "ok",
  "results": [
    {
      "acquisition_ref": "9b2e7c41d0a84f36b5e1c8d27a603f94",
      "provider": "prowlarr",
      "title": "Some Film 1999 1080p",
      "size_bytes": 8589934592,
      "seeders": 120,
      "leechers": 14,
      "published": "Mon, 01 Sep 2026 12:00:00 +0000"
    }
  ],
  "provider_errors": {
    "other-indexer": "Torznab returned HTTP 502"
  }
}
```

The query is sent to every enabled Torznab provider on this node. A provider that fails is reported in `provider_errors` (provider name to message) and does not fail the search; the others' results are still returned. With no provider configured the answer is empty `results` and empty `provider_errors`.

| field | type | meaning |
|---|---|---|
| `acquisition_ref` | string | opaque; what `POST /api/v1/torrents/jobs` takes to start this result |
| `provider` | string | the configured provider name |
| `title` | string | |
| `size_bytes`, `seeders`, `leechers` | integer or null | null when the provider did not say |
| `published` | string or null | the provider's publication date, as the provider wrote it |

The server returns results ordered by `seeders`, highest first, with unknown counts last.

**An `acquisition_ref` is valid for 30 minutes, and only on the node that ran the search.** The magnet or `.torrent` URL behind it never leaves the server. Up to 4096 references are held per node; beyond that the ones closest to expiry are dropped.

`400 bad_request` if `q` is missing or empty.

### Start a job

A torrent is added to the **cluster**: the add is recorded as a request in cluster metadata, it waits there, and any node that runs the torrent subsystem may claim and download it. A node that runs no torrents takes adds like any other.

`POST /api/v1/torrents/jobs`

```json
{"magnet": "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Some+Film",
 "node_id": null, "remove_after_ms": null}
```

| field | required | meaning |
|---|---|---|
| `magnet` | one of these two | a magnet URI |
| `acquisition_ref` | one of these two | a reference from a search **on this same node**. `magnet` wins if both are given. A search result's `.torrent` is read by a torrent-capable node (this one, or one it asks) and recorded as its canonical magnet |
| `node_id` | no | **pins** the job to that node, which must run the torrent subsystem. Absent or null lets the cluster choose |
| `remove_after_ms` | no | remove the job this long after it completes: 0 (at once) to 86400000 (24 h). Absent copies the cluster default (`default_remove_after_ms` on [`/torrents/nodes`](#torrent-capable-nodes)) into the job; null means never |
| `paused` | no | `true` records the job already paused (`desired: paused`), in the same metadata write as the add, so it never starts before a pause could land: a node may claim it but adopts it held, so it is never checked and never downloads until resumed. Absent, null or `false` is as before. It sets nothing else: a pin still names the only node that may claim it, and `remove_after_ms` still counts from completion. An add of a torrent already present answers `409 torrent_already_added` and leaves that job as it is |

The response is `202`, sent only once the request is accepted into metadata, so a list from any node includes it:

```json
{"status": "ok", "id": "7d0c2f5e9a1b4e38b6d4a0c1f2e3d4b5",
 "info_hash": "0123456789abcdef0123456789abcdef01234567", "node_id": null, "job": {...}}
```

`node_id` is the pin, null when the cluster chooses; `job` is the new [torrent job](#the-torrent-job-object), `phase` `awaiting_node`.

| status | code | `error.reason` | when |
|---|---|---|---|
| 400 | `bad_request` | | neither `magnet` nor `acquisition_ref` is a string; `node_id` is not 32 hex characters; `remove_after_ms` is not null or 0..86400000 |
| 404 | `not_found` | | `acquisition_ref` is unknown here or has expired |
| 409 | `placement_failed` | `node_not_member` | `node_id` is not an active member of the cluster |
| 409 | `placement_failed` | `node_not_torrent_capable` | `node_id` does not run the torrent subsystem |
| 409 | `placement_failed` | `add_failed` | the URI is not an acceptable magnet or names no info hash, or no torrent-capable node could read the `.torrent` |
| 409 | `torrent_already_added` | | a job for this torrent (the same info hash) exists anywhere in the cluster, in any phase until it is cleared |
| 503 | `metadata_unavailable` | | cluster metadata cannot be written; `error.scope` is `cluster` and `error.alternative_may_succeed` false, since every node would answer the same. On a cluster whose write floor is its whole membership, one node down stops adds |

The refusal of a duplicate names the job that holds the torrent, in the fields a `202` uses; `node_id` is the node running it, or null while it awaits one:

```json
{"status": "torrent_already_added",
 "error": {"code": "torrent_already_added", "message": "job 7d0c2f5e9a1b4e38b6d4a0c1f2e3d4b5 already holds this torrent"},
 "id": "7d0c2f5e9a1b4e38b6d4a0c1f2e3d4b5", "node_id": "a3c95e0f7d2b41e8b6c4d0f19e7a2b58"}
```

To download it again, clear that job first.

### Who downloads it

Every torrent-capable node runs a scheduler. It claims a waiting job when the job is not paused, is not pinned elsewhere, and the node has a free slot (`torrent.max_active`) and staging room. Nodes rank each job the same way, and a node that is not the preferred one waits 30 s per place in that ranking before claiming, so healthy nodes do not race for it. The claiming node downloads under the job's `id` and writes its progress back: `downloading`, `importing`, `completed` or `failed`.

A claim holds while its node is a cluster member. **A node absent for 10 minutes loses it**, and another capable node claims the job and starts it again from nothing (staging and resume data are the node's own). A node that returns to find its claim taken over deletes its copy.

Metadata writes merge, so two nodes can change one job at once; every field merges by a fixed rule and none ever needs an operator: `cancel` beats any pause or resume, a job never moves backwards within one claim, and of two claims the later takeover wins, then the earlier claim. In a cluster of three or more nodes, a network partition can let two nodes download the same job; if both reach import before the partition heals, the second file lands beside the first as `... (2)`. Nothing is lost or corrupted.

### List and inspect

`GET /api/v1/torrents/jobs` answers with every torrent job in the cluster, from the answering node's copy of the metadata with each owner's live figures laid over it: `jobs` (each a [torrent job](#the-torrent-job-object)), `sources` (the torrent-capable nodes whose live figures are shown, as in the ingest list) and `refresh_interval_ms`. It never waits on another node.

`GET /api/v1/torrents/jobs/{id}` answers the one job, or `404 not_found`.

### Actions

`POST /api/v1/torrents/jobs/{id}/{action}`, where `action` is `pause`, `resume`, `retry`, `cancel` or `clear`. No body.

**Actions record intent**: they answer `202` with the whole job, `desired` already changed, and the owning node applies it within a few seconds; `state` follows. `desired_applied` says when it has. `clear` answers `202 {"status": "ok", "cleared": true}`.

| action | allowed | result |
|---|---|---|
| `pause` | not `completed`, `failed`, `cancelled` | `desired` `paused` |
| `resume` | `desired` is `paused` | `desired` `active` |
| `cancel` | not `completed`, `cancelled` | `desired` `cancelled`; a job no node holds is `cancelled` at once |
| `retry` | `failed` with a linked ingest | the owner resumes the ingest |
| `clear` | `completed`, `failed`, `cancelled`, or not yet claimed | the job is removed; its owner removes the download and the payload |

`404 not_found` for an unknown action or job, `409 invalid_state` for one the job does not allow, `503 node_unreachable` for a `retry` whose owner cannot be reached.

**With metadata unwritable**, `pause`, `resume` and `cancel` on a job some node holds still work: the answering node forwards them to the owner, which applies them at once and publishes them when metadata is writable again. Everything else answers `503 metadata_unavailable` (scope `cluster`).

`PATCH /api/v1/torrents/jobs/{id}` with `{"remove_after_ms": null | 0..86400000}` changes removal on an existing job; with `{"node_id": "..." | null}` it re-pins or unpins a job that still awaits a node (otherwise `409 invalid_state`). It answers `200` with the job.

### Removal after completion

A job with `remove_after_ms` set is removed that long after it completes -- after its ingest has completed and its catalogue work has settled. The torrent job, its ingest job and the staging payload go, and the job leaves the cluster's list. `failed` and `cancelled` jobs are never removed by themselves. Off unless set, per job or with `torrent.remove_on_complete_after_ms`.

## The torrent job object

```json
{
  "id": "7d0c2f5e9a1b4e38b6d4a0c1f2e3d4b5",
  "info_hash": "0123456789abcdef0123456789abcdef01234567",
  "name": "Some Film 1999 1080p",
  "phase": "downloading",
  "state": "downloading",
  "desired": "active",
  "desired_changed_unix_ms": 1790000000000,
  "desired_applied": true,
  "desired_blocked_reason": null,
  "node_id": "a3c95e0f7d2b41e8b6c4d0f19e7a2b58",
  "pinned_node_id": null,
  "live_as_of_unix_ms": 1790000419000,
  "remove_after_ms": null,
  "remove_at_unix_ms": null,
  "completed_unix_ms": null,
  "bytes_total": 8589934592,
  "bytes_completed": 4294967296,
  "download_rate": 5242880,
  "upload_rate": 262144,
  "uploaded_total": 104857600,
  "peers": 31,
  "seeds": 12,
  "catalogue": {
    "total": 0,
    "pending": 0,
    "catalogued": 0,
    "no_match": 0,
    "failed": 0,
    "state": "waiting"
  },
  "eta_seconds": 820,
  "progress": 0.5,
  "ingest_job_id": null,
  "created_unix_ms": 1790000000000,
  "updated_unix_ms": 1790000420000,
  "error_code": null,
  "error": null
}
```

| field | type | meaning |
|---|---|---|
| `id` | string | 32 hex characters |
| `info_hash` | string | the torrent's v1 info hash, else its v2, in lowercase hex |
| `phase` | string | where the job stands in the cluster: `awaiting_node`, `downloading`, `importing`, `completed`, `failed`, `cancelled`. Always present |
| `state` | string | the owner's fine-grained state, see [Torrent job states](#torrent-job-states); `awaiting_node` exactly when `phase` is |
| `desired` | string | what the operator asked for: `active`, `paused`, `cancelled` |
| `desired_changed_unix_ms` | integer | when `desired` last changed |
| `desired_applied` | boolean | the owner has acted on the current `desired`. Still false after twice `refresh_interval_ms` with no `desired_blocked_reason` means something is wrong |
| `desired_blocked_reason` | string or null | why nothing will act on `desired`: `owner_unreachable`, `pinned_node_unavailable` (pinned to a node that is not an active torrent-capable member), `no_capable_node` (waiting and no capable node is accepting); null when it will be applied |
| `node_id` | string or null | the node running the job; null while it awaits one |
| `pinned_node_id` | string or null | the node the job is pinned to, if any |
| `live_as_of_unix_ms` | integer or null | how old the live figures are (the owner's rates, bytes, peers, `catalogue`, `publication`); null, with those figures null too, when no live view of the owner is available |
| `publication` | object or null | how far the owner has published the torrent's verified extents into the store: `{published_extents, extents, published_bytes, bytes, progress_age_ms}`. A finished download is handed to the ingest only once every extent is published, so the ingest adopts them and completes in seconds; until then the job sits in `downloaded` and this is its progress (fraction `published_extents / extents`, amounts in bytes). `progress_age_ms` is how long ago `published_extents` last advanced, on the owner's clock at `live_as_of_unix_ms`: a fraction that stops rising with a growing age is stalled, not slow, and after 10 minutes without progress the owner imports anyway and the ingest copies what is missing. Present from the first verified piece until the ingest is submitted, rising during the download too; "publishing now" is `published_extents < extents`. null when the owner reports none (no live view, or no extent publication for the job) |
| `waiting_reason` | string or null | why a job that is not moving is waiting, as a code: `extent_publication` (downloaded, waiting for `publication` to complete before the import). null otherwise |
| `remove_after_ms` | integer or null | see [Removal after completion](#removal-after-completion) |
| `remove_at_unix_ms` | integer or null | when it will be removed, once completed |
| `completed_unix_ms` | integer or null | when it completed |
| `name` | string | the torrent's name: the magnet's `dn` until the owner has its metadata |
| `bytes_total`, `bytes_completed` | integer | while downloading, the wanted payload and how much of it is held. **Once the job is linked to an ingest, these are the ingest's copy counts** |
| `download_rate` | integer | bytes per second. Once linked, the ingest's copy rate |
| `upload_rate`, `uploaded_total` | integer | bytes per second, and bytes uploaded all-time |
| `peers`, `seeds` | integer | connected peers and seeds |
| `catalogue` | object | the linked ingest's catalogue counts, with the same fields as the ingest's `catalogue` minus `items`. `state` is `processing` if anything is pending; otherwise `completed_with_issues` or `completed` if anything was counted; otherwise `waiting` |
| `eta_seconds` | integer or null | null when there is no rate to estimate from. Once linked, the ingest's |
| `progress` | number or null | `bytes_completed / bytes_total`, capped at 1; null while `bytes_total` is 0 |
| `ingest_job_id` | string or null | the ingest job the payload was submitted as; null until then |
| `created_unix_ms`, `updated_unix_ms` | integer | milliseconds since the Unix epoch |
| `error_code` | string or null | why the job is `blocked` or `failed`; null otherwise |
| `error` | string or null | the English message beside `error_code` |

`upload_rate`, `peers` and `seeds` are not refreshed after the payload has been handed to the ingest; they keep the last values sampled from the download. `download_rate`, `upload_rate`, `peers`, `seeds` and `eta_seconds` are not persisted and read 0 or null after the owning node restarts.

### Torrent job states

| state | meaning | terminal |
|---|---|---|
| `queued` | added and not yet in one of the states below | no |
| `metadata` | fetching the torrent's metadata from peers | no |
| `downloading` | downloading the payload | no |
| `verify_queued` | waiting to check pieces already on disk: the download engine checks **one torrent at a time**, and another is being checked | no |
| `verifying` | checking pieces already on disk. `eta_seconds` is the check's own estimate, `progress` counts the pieces found valid so far | no |
| `downloaded` | the payload is complete; **waiting for its extents to be published** before import | no |
| `importing` | the payload has been submitted as an ingest job, which is queued, scanning or importing | no |
| `cataloguing` | the linked ingest is `cataloguing` | no |
| `paused` | stopped by an operator, or the linked ingest is `paused` | no |
| `blocked` | stopped by a condition that may pass; **recovers by itself** | no |
| `completed` | the linked ingest is `completed` | yes |
| `cancelled` | stopped by an operator | yes |
| `failed` | stopped by an error | yes (`retry` restarts an import failure) |

**Before handover** the state follows the download engine: `metadata`, `downloading`, `verify_queued`, `verifying`, `downloaded`, or `queued` for anything else. A job that was `metadata`, `downloading`, `verify_queued`, `verifying` or `downloaded` when its node stopped is `queued` again at start, and the download resumes from what is on disk.

**A restart resumes from what was already verified.** The node keeps each job's resume data, saved when a check or download finishes, on pause, every five minutes and at shutdown, and restarts from it. Without usable resume data (a missing or damaged file) the payload on disk is checked in full, which on a large torrent takes minutes to an hour and queues every other check behind it.

**`paused` means paused.** A paused or `blocked` torrent is held out of the download engine's queue entirely: it is not checked, downloaded or seeded until it is resumed.

**Handover.** A finished download is paused in `downloaded` until every extent of its payload has been published into the store the ingest commits into. Then its ingest job is submitted, `ingest_job_id` is set and the job becomes `importing`; the ingest adopts the published extents rather than copying the bytes again. This takes seconds to minutes. If publication makes no progress for 10 minutes the job is submitted anyway, and whatever was not published is copied. If there is nothing to wait for (the payload is not being published), submission is immediate.

**After handover** the torrent job mirrors its ingest:

| linked ingest | torrent job | `error_code` |
|---|---|---|
| `queued`, `scanning`, `importing` | `importing` | the ingest's (normally null) |
| `paused` | `paused` | the ingest's |
| `blocked` | `blocked` | the ingest's, for example `metadata_unavailable` |
| `cataloguing` | `cataloguing` | the ingest's |
| `completed` | `completed` | null |
| `failed` | `failed` | the ingest's own code, or `ingest_failed` if it has none |
| `cancelled` | `failed` | `ingest_cancelled` |
| no longer exists | `failed` | `ingest_missing` |

So a linked torrent job that is `blocked` with `metadata_unavailable` recovers on the ingest's schedule, as the ingest does. **A `failed` torrent job follows its ingest back:** if the linked ingest is resumed by any route (the torrent's `retry`, the ingest's own `resume`, or a peer's action), the torrent job leaves `failed` and mirrors it again, and releases its staging when the ingest completes. The linked ingest job also appears in `GET /api/v1/ingest/jobs`, with `source_type` `torrent`, `source_ref` the torrent job's `id` and `source_owned` true. Acting on it there (cancelling or clearing it) is reflected in the torrent job as the table says.

Operator actions:

| action | allowed from | result |
|---|---|---|
| `pause` | anything but `completed`, `cancelled`, `failed`. Once linked, only if the ingest can be paused (so not while `cataloguing`) | `paused`; a linked ingest is paused too |
| `resume` | `paused`, `blocked`. Once linked, only if the ingest can be resumed | `importing` if linked, otherwise `queued`; `error_code` cleared |
| `retry` | `failed`, and only when the linked ingest is itself `failed` | `importing`; the ingest is resumed |
| `cancel` | anything but `completed`, `cancelled` | `cancelled`; a linked ingest is cancelled too |
| `clear` | `completed`, `cancelled`, `failed` | the job is removed |

A failure before handover (`torrent_error`, `torrent_fault`, `ingest_submit_failed`, `restore_failed`, `duplicate_torrent`) cannot be retried; cancel or clear it and add the torrent again.

### Torrent error codes

| `error_code` | state | cause | recovers by itself |
|---|---|---|---|
| `staging_full` | `blocked` | the remaining download does not fit within `ingest.staging_limit`; the download is paused | yes, when the staging area has room |
| `torrent_error` | `failed` | the download engine reported an error on the torrent | no |
| `ingest_submit_failed` | `failed` | the finished payload could not be submitted to ingest | no |
| `restore_failed` | `failed` | the job could not be re-added to the download engine when the node started | no |
| `duplicate_torrent` | `failed`, or `cancelled` | another job holds the same torrent, for example one added through two nodes at once; the older job keeps it | no |
| `adopt_failed` | `failed` | the node that claimed the job could not start it | no |
| `torrent_fault` | `failed` | the download engine faulted on this job; the job's download is removed and every other job carries on. A linked job still follows its ingest | no; clear it and add the torrent again |
| `ingest_failed` | `failed` | the linked ingest failed without a code | via `retry` |
| `ingest_cancelled` | `failed` | the linked ingest was cancelled | no |
| `ingest_missing` | `failed` | the linked ingest no longer exists | no |
| `torrent_failed` | `failed` | the job failed with an error message and no more specific code | no |
| any ingest code | `blocked` or `failed` | mirrored from the linked ingest, as above | as the ingest does |

### What clear and cancel delete

`cancel` removes the torrent from the download engine and releases its staging reservation. When `ingest.cleanup.delete_owned_source_on_cancel` is true it also deletes the downloaded payload.

The payload is moved into the staging area's `.trash` at once and deleted in the background, so the request returns promptly however large the payload; it counts against `ingest.staging_limit` until it is gone.

`clear` on a linked job clears the linked ingest, which deletes the payload under the ingest rules (it is an owned source, so `ingest.cleanup.delete_owned_source_on_clear` decides). On a job with no ingest, `clear` deletes the payload when `ingest.cleanup.delete_owned_source_on_clear` is true. If the linked ingest cannot be cleared, the torrent job is not either (`409 invalid_state`).

## Cluster-wide behaviour

**A torrent job belongs to the cluster** (see [Who downloads it](#who-downloads-it)). **An ingest job belongs to the node that runs it**, and every node can list and act on every other node's.

- **Every node keeps a view of the cluster's jobs.** Torrent jobs come from its copy of the metadata. Its own live figures are read directly; every other member's live torrent figures and ingest jobs are polled in the background every `refresh_interval_ms` (5 s). Lists and lookups answer from that and never wait on another node.
- **Listing** (`GET .../jobs`): the answering node's own jobs plus every other member's from the view, with `sources` saying how old each node's are and whether it was reachable.
- **Inspecting** (`GET .../jobs/{id}`): answered from the view; `404 not_found` if no node the answering node has heard from owns it. A job placed on another node very recently is known at once when the add went through the answering node, and otherwise within `refresh_interval_ms`.
- **Acting** (`POST .../jobs/{id}/{action}`): the action runs on the owning node, found in the view, and the job returned is that node's answer. `503 node_unreachable` if the owner cannot be reached.
- `node_id` on every job object names the owner.
- **Placement**: an ingest job is always created on the node that received the `POST`, because its `path` is that node's. A torrent job is claimed by a torrent-capable node, or only by `node_id` when it is pinned.

## When the torrent subsystem is not running

On a node where the `libmacha-torrent` plugin is not installed, declined to start (`torrent.enabled: false`), is faulted, or is disabled (see [Subsystem plugins](operations.md#subsystem-plugins)):

- `GET /api/v1/torrents/status` answers `200` with `enabled: false` and `build_available: false`.
- `GET /api/v1/torrents/search` works as normal.
- Lists, lookups and actions work as on any other node, from its view of the cluster.
- `GET /api/v1/torrents/nodes` does not list it.
- Adds are taken as on any node and claimed by a node that runs torrents; an add pinned to this node is refused with `409 placement_failed`, reason `node_not_torrent_capable`.
- Jobs it had claimed keep their claim for 10 minutes and are then taken over by another capable node.

Ingest is part of core and is not affected. With `ingest.enabled` off the ingest routes still answer: status reports `enabled: false`, lists include other nodes' jobs, and `POST /api/v1/ingest/jobs` is `400 bad_request`.

## Configuration

These keys govern what is described here. Their defaults and ranges are in [Streaming, ingest and acquisition](configuration.md#streaming-ingest-and-acquisition) and [`macha.yaml.example`](../macha.yaml.example).

- `ingest.enabled`, `ingest.source_roots`, `ingest.staging_path`, `ingest.staging_limit`, `ingest.max_concurrent_jobs`, `ingest.blocked_retry_ms`, `ingest.copy_chunk_bytes`, `ingest.checkpoint_bytes`
- `ingest.cleanup.delete_owned_source_on_clear`, `ingest.cleanup.delete_external_source_on_clear`, `ingest.cleanup.delete_owned_source_on_cancel`
- `torrent.enabled` (requires `ingest.enabled`), `torrent.max_active`, `torrent.max_download_rate`, `torrent.max_upload_rate`, `torrent.disk_threads`
- `torrent.search.providers` (`name`, `type: torznab`, `url`, `api_key_file`, `max_results`)

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
| `delete_source_on_clear` | no | overrides `ingest.cleanup.delete_external_source_on_clear` for this job. `remove_source` is accepted as an older name for the same field |

The job is created on the node that receives the request, because the path is that node's. There is no placement field.

The response is `202`:

```json
{"status": "ok", "id": "4f1c0e9a2b7d4c6e8a1f3b5d7c9e0a12"}
```

| status | code | when |
|---|---|---|
| 400 | `bad_request` | `path` is missing or empty; ingest is disabled on this node; the resolved path is outside `ingest.source_roots`; the path does not exist |

These four share one code; only the message tells them apart.

### List and inspect

`GET /api/v1/ingest/jobs` answers `{"status": "ok", "jobs": [...]}` with every ingest job on every reachable node (see [Cluster-wide behaviour](#cluster-wide-behaviour)). Each item is an [ingest job](#the-ingest-job-object) without `files` and without `catalogue.items`, plus `node_id`.

`GET /api/v1/ingest/jobs/{id}` answers the one job, with `files` and `catalogue.items`, plus `node_id`. `404 not_found` if no reachable node has it.

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

Operator actions:

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

Before 0.57.0 `metadata_unavailable` made the job `failed`. It is now `blocked` and the job completes once metadata is writable again.

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
GET  /api/v1/torrents/jobs
POST /api/v1/torrents/jobs
GET  /api/v1/torrents/jobs/{id}
POST /api/v1/torrents/jobs/{id}/pause
POST /api/v1/torrents/jobs/{id}/resume
POST /api/v1/torrents/jobs/{id}/retry
POST /api/v1/torrents/jobs/{id}/cancel
POST /api/v1/torrents/jobs/{id}/clear
```

The download engine is the `libmacha-torrent` subsystem plugin. Status and search are served by core and answer on every node. **Every other route answers `503 unavailable` on a node where the plugin is not running**, whatever the state of the other nodes; see [When the torrent subsystem is not running](#when-the-torrent-subsystem-is-not-running).

### Status

`GET /api/v1/torrents/status`, about the node that answers:

```json
{"status": "ok", "enabled": true, "build_available": true, "search_enabled": true}
```

| field | meaning |
|---|---|
| `enabled` | the plugin is running here and `torrent.enabled` is on |
| `build_available` | the plugin is loaded and running here. The name predates 0.28.0, when it meant "compiled in" |
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

`POST /api/v1/torrents/jobs`

```json
{"acquisition_ref": "9b2e7c41d0a84f36b5e1c8d27a603f94", "node_id": "a3c95e0f7d2b41e8b6c4d0f19e7a2b58"}
```

or

```json
{"magnet": "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Some+Film"}
```

| field | required | meaning |
|---|---|---|
| `magnet` | one of these two | a magnet URI |
| `acquisition_ref` | one of these two | a reference from a search **on this same node**. `magnet` wins if both are given |
| `node_id` | no | the node that should download it. Absent or null means the node receiving the request |

The response is `202`, and always names where the job went:

```json
{"status": "ok", "id": "7d0c2f5e9a1b4e38b6d4a0c1f2e3d4b5", "node_id": "a3c95e0f7d2b41e8b6c4d0f19e7a2b58"}
```

A named node that cannot take the job is refused; the job is never started somewhere else instead.

| status | code | `error.reason` | when |
|---|---|---|---|
| 400 | `bad_request` | | neither `magnet` nor `acquisition_ref` is a string; `node_id` is not a string or not 32 hex characters |
| 404 | `not_found` | | `acquisition_ref` is unknown here or has expired |
| 409 | `placement_failed` | `node_not_member` | `node_id` is not an active member of the cluster |
| 409 | `placement_failed` | `node_unreachable` | the named node could not be reached |
| 409 | `placement_failed` | `node_refused` | the named node answered but did not take the request |
| 409 | `placement_failed` | `node_did_not_start` | the named node did not start the job and gave no code |
| 409 | `placement_failed` | `missing_uri` | the named node received no URI |
| 409 | `placement_failed` | `add_failed` | the target node could not add the torrent: torrent support is disabled there, the URI is not an acceptable magnet, or the job could not be recorded |

### List and inspect

`GET /api/v1/torrents/jobs` answers `{"status": "ok", "jobs": [...]}` with every torrent job on every reachable node whose torrent subsystem is running. Each item is a [torrent job](#the-torrent-job-object) plus `node_id`.

`GET /api/v1/torrents/jobs/{id}` answers the one job plus `node_id`, or `404 not_found`.

### Actions

`POST /api/v1/torrents/jobs/{id}/{action}`, where `action` is `pause`, `resume`, `retry`, `cancel` or `clear`. No body. The responses and errors are the same as for [ingest actions](#actions): the job with `node_id` for the first four, `{"status": "ok", "cleared": true}` for `clear`, `404 not_found` for an unknown action or job, `409 invalid_state` for an action the job's state does not allow.

## The torrent job object

```json
{
  "id": "7d0c2f5e9a1b4e38b6d4a0c1f2e3d4b5",
  "node_id": "a3c95e0f7d2b41e8b6c4d0f19e7a2b58",
  "name": "Some Film 1999 1080p",
  "info_hash": null,
  "state": "downloading",
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
| `node_id` | string | the node that owns and runs the job |
| `name` | string | the torrent's name; empty until its metadata has arrived |
| `info_hash` | null | always null: the server does not record it |
| `state` | string | see [Torrent job states](#torrent-job-states) |
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
| `verifying` | checking pieces already on disk | no |
| `downloaded` | the payload is complete; **waiting for its extents to be published** before import | no |
| `importing` | the payload has been submitted as an ingest job, which is queued, scanning or importing | no |
| `cataloguing` | the linked ingest is `cataloguing` | no |
| `paused` | stopped by an operator, or the linked ingest is `paused` | no |
| `blocked` | stopped by a condition that may pass; **recovers by itself** | no |
| `completed` | the linked ingest is `completed` | yes |
| `cancelled` | stopped by an operator | yes |
| `failed` | stopped by an error | yes (`retry` restarts an import failure) |

**Before handover** the state follows the download engine: `metadata`, `downloading`, `verifying`, `downloaded`, or `queued` for anything else. A job that was `metadata`, `downloading`, `verifying` or `downloaded` when its node stopped is `queued` again at start, and the download resumes from what is on disk.

**Handover, since 0.57.0.** A finished download is paused in `downloaded` until every extent of its payload has been published into the store the ingest commits into. Then its ingest job is submitted, `ingest_job_id` is set and the job becomes `importing`; the ingest adopts the published extents rather than copying the bytes again. This takes seconds to minutes. If publication makes no progress for 10 minutes the job is submitted anyway, and whatever was not published is copied. If there is nothing to wait for (the payload is not being published), submission is immediate.

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

So a linked torrent job that is `blocked` with `metadata_unavailable` recovers on the ingest's schedule, as the ingest does. The linked ingest job also appears in `GET /api/v1/ingest/jobs`, with `source_type` `torrent`, `source_ref` the torrent job's `id` and `source_owned` true. Acting on it there (cancelling or clearing it) is reflected in the torrent job as the table says.

Operator actions:

| action | allowed from | result |
|---|---|---|
| `pause` | anything but `completed`, `cancelled`, `failed`. Once linked, only if the ingest can be paused (so not while `cataloguing`) | `paused`; a linked ingest is paused too |
| `resume` | `paused`, `blocked`. Once linked, only if the ingest can be resumed | `importing` if linked, otherwise `queued`; `error_code` cleared |
| `retry` | `failed`, and only when the linked ingest is itself `failed` | `importing`; the ingest is resumed |
| `cancel` | anything but `completed`, `cancelled` | `cancelled`; a linked ingest is cancelled too |
| `clear` | `completed`, `cancelled`, `failed` | the job is removed |

A failure before handover (`torrent_error`, `ingest_submit_failed`, `restore_failed`) cannot be retried; cancel or clear it and add the torrent again.

### Torrent error codes

| `error_code` | state | cause | recovers by itself |
|---|---|---|---|
| `staging_full` | `blocked` | the remaining download does not fit within `ingest.staging_limit`; the download is paused | yes, when the staging area has room |
| `torrent_error` | `failed` | the download engine reported an error on the torrent | no |
| `ingest_submit_failed` | `failed` | the finished payload could not be submitted to ingest | no |
| `restore_failed` | `failed` | the job could not be re-added to the download engine when the node started | no |
| `ingest_failed` | `failed` | the linked ingest failed without a code | via `retry` |
| `ingest_cancelled` | `failed` | the linked ingest was cancelled | no |
| `ingest_missing` | `failed` | the linked ingest no longer exists | no |
| `torrent_failed` | `failed` | a job recorded as failed before error codes existed | no |
| any ingest code | `blocked` or `failed` | mirrored from the linked ingest, as above | as the ingest does |

### What clear and cancel delete

`cancel` removes the torrent from the download engine and releases its staging reservation. When `ingest.cleanup.delete_owned_source_on_cancel` is true it also deletes the downloaded payload.

`clear` on a linked job clears the linked ingest, which deletes the payload under the ingest rules (it is an owned source, so `ingest.cleanup.delete_owned_source_on_clear` decides). On a job with no ingest, `clear` deletes the payload when `ingest.cleanup.delete_owned_source_on_clear` is true. If the linked ingest cannot be cleared, the torrent job is not either (`409 invalid_state`).

## Cluster-wide behaviour

A job belongs to the node that runs it, and every node can list and act on every other node's jobs.

- **Listing** (`GET .../jobs`): the answering node's own jobs, then each other active member's, asked for in turn. A member that cannot be reached, or that has no running torrent subsystem, contributes nothing, and the response does not say that any member was skipped.
- **Inspecting and acting** (`GET .../jobs/{id}`, `POST .../jobs/{id}/{action}`): the answering node looks locally first, then asks each active member until one owns the id. The action runs on the owning node, and the job returned is that node's answer. If no reachable member owns the id the answer is `404 not_found`, which is also what a job on an unreachable node gets.
- `node_id` on every job object names the owner.
- **Placement**: an ingest job is always created on the node that received the `POST`, because its `path` is that node's. A torrent job goes to `node_id` if given, otherwise to the node that received the `POST`.

## When the torrent subsystem is not running

On a node where the `libmacha-torrent` plugin is not installed, declined to start (`torrent.enabled: false`), is faulted, or is disabled (see [Subsystem plugins](operations.md#subsystem-plugins)):

- `GET /api/v1/torrents/status` answers `200` with `enabled: false` and `build_available: false`.
- `GET /api/v1/torrents/search` works as normal.
- Every other `/api/v1/torrents/` route answers `503 unavailable`, including the cluster-wide list. Ask a node whose plugin is running to see or act on torrent jobs elsewhere in the cluster.
- The node's own torrent jobs are invisible to the rest of the cluster until the subsystem is running again.

Ingest is part of core and is not affected. With `ingest.enabled` off the ingest routes still answer: status reports `enabled: false`, lists include other nodes' jobs, and `POST /api/v1/ingest/jobs` is `400 bad_request`.

## Configuration

These keys govern what is described here. Their defaults and ranges are in [Streaming, ingest and acquisition](configuration.md#streaming-ingest-and-acquisition) and [`macha.yaml.example`](../macha.yaml.example).

- `ingest.enabled`, `ingest.source_roots`, `ingest.staging_path`, `ingest.staging_limit`, `ingest.max_concurrent_jobs`, `ingest.blocked_retry_ms`, `ingest.copy_chunk_bytes`, `ingest.checkpoint_bytes`
- `ingest.cleanup.delete_owned_source_on_clear`, `ingest.cleanup.delete_external_source_on_clear`, `ingest.cleanup.delete_owned_source_on_cancel`
- `torrent.enabled` (requires `ingest.enabled`), `torrent.max_active`, `torrent.max_download_rate`, `torrent.max_upload_rate`, `torrent.disk_threads`
- `torrent.search.providers` (`name`, `type: torznab`, `url`, `api_key_file`, `max_results`)

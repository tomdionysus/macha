# Playback start reports progress; the client decides how long to wait

Status: plan, operator-approved direction (2026-09-27: "Guessing the time is
asinine. We need a better system to inform the client." / "We'll do this").
Wire shape below must be announced to Core and every client before any code
ships. Clients enter release lockdown after this.

## The problem

Create and PATCH are one blocking request (`src/playback/playback.cpp`):

1. Plan the generation (`prepare_hls_vod`): probe, keyframe index, and the
   source reads that needs, possibly remote. No clock.
2. Start the pipeline and wait for the first fragment
   (`wait_for_initial_fragment`), bounded by `streaming.startup_timeout_ms`
   (15 s). At 15 s the pipeline is stopped and the request answers 503
   `playback_pipeline_start_failed` "timed out waiting for first
   fragmented-MP4 segment", however close it was.

The client sees nothing until one of those ends. `docs/streaming.md` already
states the consequence, about a client budget shorter than the node's:
"discards a transcode that was about to succeed ... manufacturing a
viewer-visible failure out of a node that was working". The server does this
to itself. It is the same mistake the 2026-09-06 startup gate made: an
elapsed-time bound on work that was progressing
(`startup_progress.hpp`, Discipline 2).

### The case that forced it

TV client, 2026-09-27, The Martian: PATCH from the 1280x534 file to the 4K
HEVC HDR remux `1e763547` (47 Mbps), transcode to 1440p H.264, TrueHD to AAC
7.1, `seek_ms` 2008000.

| node | seek | result |
| --- | --- | --- |
| fi-1 | 2008000 | 503 in 15.0 s (trace `be72a4fa`) |
| fi-1 | none | 200 in 7.4 s |
| gbni-1 | 2008000 | 503 in 16.3 s (trace `f3c9d4e3`) |

Likely cause, consistent with all three and with the code, not yet measured
directly: a transcode seek is frame-exact (`seek_offset_ms` 0), so the
decoder seeks back to the preceding keyframe and decodes and discards every
frame up to the origin (`media_engine.cpp`, `run_pipeline` read loop;
documented under "Where a seek actually starts"). 4K HEVC 10-bit software
decode on a Pi makes that pre-roll long.

Controls on gbni-1 (TV client, same evening), each a create on the 1280x534
file at seek 1990000 then one PATCH:

| PATCH | result |
| --- | --- |
| 4K, max_height 1440, no seek | 200 in 10.6 s |
| 4K, max_height null, seek 2008000 | 503 in 16.5 s (trace `82e05a3b`) |
| 1280x534, transcode, seek 2008000 | 200 in 3.8 s |

The seek into the 4K HEVC source misses with or without the output cap; the
no-seek 4K start and the small file's seek both fit. Consistent with
pre-roll decode as the cost; the pre-roll counter (below) is what will prove
it. Caveat from the client: all three creates returned the same session id
(`cd941acb...`) although it deleted in between, so the PATCHes may have hit
one reused session. Unexplained; see "Also to check".

## Design

### 1. Opt-in, so the lockdown is safe

`?start=async` on `POST /api/v1/playback/sessions` and on
`PATCH /api/v1/playback/sessions/{id}`, a query parameter like
`idempotency_key`. Without it nothing changes: the request blocks, and
`startup_timeout_ms` bounds the first fragment exactly as today and as
Status publishes. A client that never adopts it is unaffected.

### 2. Answer at acceptance

With `start=async` the request answers once the session is admitted
(ownership, caps, entitlements, media resolved), before planning:

- `POST`: `202`, status `playback_starting`, `Location`, the session payload
  with a `start` object (below) and no stream URLs yet.
- `PATCH`: `202`, status `playback_starting`. The current generation keeps
  serving and is **not** marked superseded until the replacement is ready
  (today it is marked up front). The payload carries the current generation
  as now, plus `pending` with the replacement's `start` object.

Refusals that are decidable at admission (`account_session_limit`,
`resource_limit`, `media_id_required`, ...) still answer synchronously with
their existing codes.

### 3. `start`: stage codes and raw counters

On `GET /api/v1/playback/sessions/{id}` (and in `pending` during a PATCH):

```json
"start": {
  "stage": "preroll",
  "progress_seq": 412,
  "progress_age_ms": 180,
  "elapsed_ms": 9350,
  "source_bytes_read": 188743680,
  "preroll_decoded_ms": 3120,
  "preroll_total_ms": 5005,
  "first_fragment_media_ms": 0,
  "first_fragment_target_ms": 6000
}
```

Stages, snake_case codes, in order: `planning`, `preroll` (transcode only:
decoding from the keyframe to the origin), `encoding` (producing the first
fragment), `ready`, and `failed`. Counters are facts, never estimates and
never formatted text: the client computes rates and fractions and presents
them (the server gives data and codes only). A counter a stage cannot
measure is absent, not zero. `progress_seq` increments on any counter
change; `progress_age_ms` is the age of the last increment, measured on the
node.

On `failed`: `error` with the existing stage error codes
(`playback_pipeline_start_failed`, ...) and the stage that stalled.

Once `ready`, a POST's session payload gains its stream URLs exactly as a
blocking create returns them; a PATCH's replacement becomes the session
(new `generation`), the old pipeline stops, and `pending` disappears.

### 4. Long-poll, not blind polling

`GET /api/v1/playback/sessions/{id}?after=<progress_seq>&wait_ms=<n>`
answers as soon as `progress_seq` exceeds `after` or the stage changes, or at
`wait_ms` with the unchanged state. `wait_ms` is the client's, capped by the
node (published, below) only so a parked request cannot pin a connection
indefinitely. The reactor server already parks requests as continuations
(`HttpDeferral`), so a long-poll costs an fd, not a thread.

### 5. Cancel

- A pending create: `DELETE /api/v1/playback/sessions/{id}`, as now.
- A pending PATCH: `DELETE /api/v1/playback/sessions/{id}/pending` abandons
  the replacement and leaves the current generation playing. A second PATCH
  while one is pending replaces the pending one.

### 6. The server fails on no progress, not on elapsed time

For `start=async`, `streaming.startup_no_progress_ms` replaces
`startup_timeout_ms`: the start fails only when `progress_seq` has not moved
for that long. Published beside the others in each node's Status `playback`
block, with `start_wait_max_ms` (the long-poll cap). Absent means the node
does not support `start=async`.

## What has to be measured before the shape is final

The counters above are what the code can see, but none is exported today:

- `source_bytes_read`: `InputIoState` in `media_engine.cpp` sees every read;
  planning (`prepare_hls_vod`) reads through its own input.
- `preroll_decoded_ms` / `preroll_total_ms`: the read loop knows the origin
  (`seek_target_us`) and each decoded frame's pts; the total needs the
  keyframe the container seek landed on.
- `first_fragment_media_ms`: the encoder's output pts before the first cut.

Each becomes an atomic on the engine session and the planner, read by the
playback layer. Confirm on a node that each moves during a real 4K seek
start before announcing the names.

## Order

1. Measure the controls above; confirm pre-roll is the cost.
2. Export the counters (no wire change); verify they move on a node.
3. Announce the exact shape to Core and every client; wait for their checks.
4. Implement async create, then async PATCH, then long-poll and cancel.
   Tests: a slow fake engine that progresses past `startup_timeout_ms` still
   starts under `start=async`; one that stalls fails at
   `startup_no_progress_ms` with the stalled stage; the blocking path is
   byte-for-byte unchanged; PATCH keeps the old generation serving until
   `ready`; cancel of a pending PATCH leaves the old generation intact.
5. Update `docs/streaming.md` (create, PATCH, budgets, startup).

## Also to check

The TV client saw three creates, with DELETEs between, return one session
id. The server mints a fresh id per POST, and a reused `idempotency_key`
after the session ends should answer `409 idempotency_expired`, so either
the DELETEs did not take effect or the replay path returns an ended
session. Get the client's key usage and the DELETE status codes, then
reproduce.

## Meanwhile

A client that wants a cheap transcode seek can ask for a keyframe position;
the pre-roll is then near zero. Whether the keyframe positions are exposed
to clients today is unchecked.

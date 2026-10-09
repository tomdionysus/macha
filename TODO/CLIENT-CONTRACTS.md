# What the client sessions depend on

Contracts settled with the client sessions (from 2026-09-13), kept here
because they were agreed across sessions and exist nowhere else in this
repository. A server change that breaks one breaks four clients at once.

**0.90.64 (2026-10-09; NOT YET ANNOUNCED: no Core or client session was
running; announce at the next one).** `POST /api/v1/users`, and `PATCH
/api/v1/users/{id}` or `/api/v1/users/me` when the body sets a password, can
answer `429 try_later` with `Retry-After: 1` when every password slot on the
node is in use, exactly as `POST /api/v1/session` already does. A client
retries after a second; nothing was changed. A roles-only `PATCH` never gets
it.

**0.90.35 (announced 2026-10-06 to Core and every client; Core's type
restored to it in `b4a549f` on 2026-10-08).** `diagnostics.repair.paced_by`
in `GET /api/v1/status/diagnostics` lists `viewer`, `loader` and
`peer_viewer` (in place of `playback`, `mounted_filesystem`, `loader`,
`peer_playback`); Core's type names those three and stays open for codes it
does not name. The web client labels them in `repairPaceText`. A 0.90.40
that would have reverted the names never shipped.

**0.63.0 (announced 2026-09-27 to Core, web, Android TV and mobile, before
deploy).** `POST /api/v1/torrents/jobs` can answer `409
torrent_already_added` with the holding job's `id` and `node_id` at top level
(same fields as the 202) and no `error.reason`; a job holds its torrent in any
state until cleared. New torrent job `error_code`s `duplicate_torrent` and
`torrent_fault` (both `failed`, not retryable). New always-present
`threads` array in `GET /api/v1/status` (`name`, `running`, `restarting`,
`faults`, `last_fault_code`, `last_fault`, `last_fault_unix_ms`).

**Mobile walks and charges healthy nodes on any mid-stream player error, and
has done all along** (core, 2026-09-21). This is not a contract and not a
request; it is a standing client defect the server session needs to know
about, because it shapes what node-health evidence from a mobile viewer is
worth. macha-client-rn has no status-to-kind mapping at the player layer at
all - playback errors arrive through expo-video's `statusChange` as a message
string with no code - so on `status === 'error'` the provider calls
`failoverSource` unconditionally, picks another node, and **records a failure
against the node it left**. A routine superseded generation therefore costs a
healthy node a mark in that client's ranking.

`410`'s axes (`node_healthy: true`, `alternative_may_succeed: true`) exist to
prevent exactly this and mobile cannot read them. **0.48.0 does not cause it
and does not worsen it** - mobile is equally blind to the `404` it gets today,
and core verified there is no status-dependent branch anywhere on that path -
but the release makes it legible. Two consequences worth holding:

- **Do not read a mobile client's endpoint-failure record as evidence about a
  node.** It may be a seek that regenerated, not a fault.
- The fix is client work, scheduled by core. The seek case is usually already
  invisible (`repositionTo` repoints before refetching, and
  `errorBlamesEndpoint` declines failover while a seek is outstanding), so the
  exposure is mid-stream errors that are not seeks.

Negotiated with the `@machafoundation/core` session and relayed by it to the
web, Android TV and mobile clients. None of it exists anywhere else in this
repository, and a server change that breaks one of these breaks four clients at
once. Recorded here because the conversation that settled them was
cross-session and will not be in the next session's context.

- **`GET /api/v1/health` is the liveness contract.** No token, no role, works
  during recovery. `200 {"status":"ok"}` when serving, `503` with `starting` or
  `failed` when not, and the HTTP status carries the same answer as the body.
  Core probes it every 10 s for latency ranking, failover and the endpoint
  pre-save gate. It must stay unauthenticated. **Corrected 2026-09-18: it does
  carry `version`, and that is intended** -- it has since 0.42.1, and reading a
  node's running version without a token is how every on-box check and every
  deploy verification is done. The rule it still keeps is the one that matters:
  no node id, no topology, nothing about the cluster. Anything beyond "is this
  node serving, and what is it running" needs `/api/v1/status` and
  `view_status`. The code comment at `src/service/service.cpp:242` now says it carries
  the running version, deliberately (checked 2026-09-24).
- **An old node answers `401`, not `404`**, to that route, because
  authentication happens before routing. Core falls back to
  `/api/v1/catalogue/status` on *any* answer that is not a liveness answer,
  which retires itself once no node needs it. This fact is why a 404-only
  fallback would have fired on every node except the one that needs it.
- **`view_status` gates the Status screen and nothing operational.** It is in
  core's `UserRole` and `USER_ROLES` with a test pinning the order. Health,
  ranking, failover and the connection gate are all indifferent to it.
- **A role-less session gets `403` from `/api/v1/status`, never a reduced
  payload.** There is no reduced-view code path; the gate is above the handler.
  Two client reports of "200 with an empty roster" were gbni-2 (ungated 0.38.1)
  misattributed to gbni-1 - see the endpoint-attribution note below.
- **`/api/v1/status` no longer carries `diagnostics`** (0.39.1). Core never
  typed that block, so it is unaffected; a client whose Status screen reads
  diagnostics needs `/api/v1/status/diagnostics`.
- **`GET /api/v1/session` needs a session and no role**, and re-checks
  `credential_generation` on every request, so it catches revocation and not
  only expiry. A `401` there can mean the account changed underneath the token;
  a client must not tell a viewer their session merely timed out.
- **A PATCH naming `mode` clears `video`, `audio`, `max_height` and
  `max_bitrate`** (`playback.cpp:248`). Unchanged since 0.34.0 and confirmed
  against 0.39.1. Android TV has been told to delete its local rule; if they
  can produce a refusal from a body containing `mode` and nothing else, that is
  a real regression and should be treated as one.
- **A role-less session learning no cluster membership is correct**, per the
  operator: such a session sees only the endpoint it was configured with.
- **`GET /api/v1/users` answers under `items`**, like every other collection,
  from 0.40.0. Single records from `POST`/`PATCH` stay bare. Core has accepted
  either key since its 0.8.0, so no client needed a release.
- **`stream.look_ahead_ms` on the playback session payload** (0.45.0) is how far
  past the fragment it last requested a client may arrive and still find media
  already produced: `max_ahead_segments` x `segment_duration_ms`, `null` for
  direct play. Added because neither knob was on the wire or in the
  configuration reference, so a client could only hardcode 8 and 4000 and
  under-run against a node configured differently. Clients bound themselves
  against this rather than against the defaults. It follows `reconfigure()`, so
  it is read per session rather than cached across a node's lifetime.
- **`seek_ms`, `seek_offset_ms` and `seek_requested_ms` on the playback session
  payload** (0.46.0), on create and on every `PATCH`, all milliseconds on the
  title's timeline. `seek_ms` is where the generation's media begins, which is
  exactly what it has always meant; `seek_offset_ms` is how far into that
  generation the requested position sits; `seek_requested_ms` is the request the
  server honoured after clamping to `[0, duration - 1 ms]`. The invariant is
  `seek_ms + seek_offset_ms == seek_requested_ms`, exactly, in integer
  milliseconds, with no tolerance and no rounding slack, and the offset is never
  negative. The server does not move a position a client asked for and does not
  substitute a mode a client asked for: transcode and direct are frame-accurate
  with a zero offset, remux takes the last keyframe at or before the request and
  publishes the remainder. The offset is zero exactly when the mode can be
  frame-accurate, so a client wanting a cheap aligned seek asks for a position
  that already is a keyframe. Core types all three as `number | undefined`
  because an older node omits them, and deletes its `activationPosition`
  undefined branch - the invariant makes that state unreachable.
- **A node reports the playback budgets it enforces** (0.46.2) on the per-node
  entries of `GET /api/v1/status`, in a `playback` object beside `runtime`:
  `startup_timeout_ms` and `segment_timeout_ms`. They are each node's statement
  about itself, relayed like `load1` and `cpu_cores`; no node computes a
  cluster-wide figure, because telemetry carries no peer's streaming
  configuration and it would be inventing one. A client that needs a worst case
  across candidates composes it itself, since only the client knows which nodes
  those are. **Absent means the node cannot say** -- an older node, or one with
  streaming disabled -- and must never shorten a client's own budget, nor be
  filled in from another node's figure. Deliberately not on the session
  payload, unlike `look_ahead_ms`: these bound the request that creates the
  session, so a client cannot learn them from the response it is timing out on,
  and a node it has never used would never report them. Agreed with the core
  session on 2026-09-18 after it showed that a `playback/status` placement
  could not ride its existing health probe without regressing latency ranking
  for role-less sessions.
- **`pipeline_idle_ms` bounds how long a client may hold a generation before
  first requesting media.** A transformed session whose stream has been idle for
  `streaming.pipeline_idle_ms` has its physical pipeline reclaimed; the logical
  session and its entitlement survive, but the producing pipeline does not. The
  default is 60,000 ms, the configured minimum is 10,000, and the live value is
  already reported by `GET /api/v1/playback/status` as `pipeline_idle_ms` -- so
  a client with a standby or handover window must read it rather than assume 60 s,
  exactly as it must for `look_ahead_ms`. Core's standby windows (8 s transcode,
  30 s otherwise) sit inside the default, but that relation was implicit until
  2026-09-18 and nobody had written it down. **What is NOT yet settled is what
  happens to a fragment request arriving after reclamation** -- see the P1 above;
  until that is answered no client should treat a reclaimed generation as
  recoverable.
- **`session_unused_idle_ms` means "never served a stream object", not "idle".**
  Asked by the Android TV session on 2026-09-20 after a paused **direct-play**
  session survived 4.5 minutes against a 120,000 ms `session_unused_idle_ms`.
  That is correct and deliberate. A session carries a `stream_served` flag set
  the first time it serves **any** stream object -- playlist, fragment,
  subtitle or a Direct Play ranged body -- and never cleared, across seeks and
  quality changes. Once set, the session gets the full `session_idle_ms`
  (30 min default); only a session that has *never* fetched media is held to
  the short clock. Direct play sets it explicitly, with the reason in the code:
  "a single ranged body can outlive several idle windows without another
  request, so this must count as having been used." Both clocks measure from
  the session's last interaction of any kind, so a client still polling or
  PATCHing is never evicted by either. The short clock exists to stop a session
  created and abandoned from holding a transcode entitlement, not to reap
  paused viewers.
- **A fragment past the look-ahead is refused, not missing.** `500
  segment_not_ready` with `Retry-After: 1` and `Cache-Control: no-store`, logged
  as `reason=hold_timed_out`; never a `404`, because the playlist has already
  promised the object exists and a `404` invites an intermediary to cache the
  absence. Retrying is correct and succeeds as production advances. Production
  is sequential, so asking for a distant index does not skip the fragments
  before it -- where the gap exceeds the look-ahead, a new generation seeked to
  the arrival point is cheaper than making the current one catch up (measured
  2026-09-17 on es-1: 1.92 s cold start against ~9 s of catch-up for 28 s).
- **A signed artwork URL is stable for up to 24 hours and valid for 24-48.**
  From 0.40.0 `exp` is quantized to a bucket of the TTL, rounded up to the
  bucket *after* next: `(now / ttl + 2) * ttl`. The invariant is "always between
  one and two TTLs", not "between 24 and 48 hours" - the bucket *is* the TTL, so
  reconfiguring `artwork_capability_ttl` moves the bound with it. With the
  default 24 h TTL this lands on a UTC midnight, because the bucket is a
  multiple of 86,400,000 ms from the epoch and the epoch is itself a UTC
  midnight. The minimum remaining validity is 86,400,001 ms, one millisecond
  *over* `max-age=86400`, so a cached copy can never outlive the signature that
  names it. Measured live: 418 refs, 418 identical URLs.
  The artwork `id` has always been the SHA-256 of the bytes and is the stable
  identity clients should key an image cache on; the signed URL is transport.
  The route is bearer-exempt with a valid signature, so a payload's `url` works
  in a bare `<img src>` with no headers.
- **Session lifetime is 30 days from mint, and that is decided** (operator,
  2026-09-13): counted from creation rather than last use, so a session expires
  even under daily use. No sliding expiry, no refresh tokens. Do not re-raise it
  as a defect.
- **`/api/v1/status/diagnostics` gained a `repair` block** in 0.40.1
  (`unsourceable_objects`, `unsourceable_sample`, `local_unreadable_objects`).
  No client types it today.
- **Record which host served a measurement before reporting it.** Two separate
  client reports today were gbni-2 attributed to gbni-1, and the retracted
  DTS/TrueHD investigation in September was the same mistake at larger scale.
  The node is in the URL; there is no excuse for losing it.

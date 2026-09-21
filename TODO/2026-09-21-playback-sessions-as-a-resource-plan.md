# Plan: a playback session is a resource, not a property of the bearer

Date: 2026-09-21

Status: **Agreed with the operator 2026-09-21, not yet implemented.** The route
shape below is settled. Nothing in `src/` has changed.

Two operator constraints govern every choice here:

- **No identifier in this system is client-generated.** Not a session id, not a
  stream id, not a viewer key. The server mints identifiers; a client is given
  one and uses it. `idempotency_key` is not a counter-example: it is an
  idempotency token, not an identifier, and it names a *request*, not a thing.
- **Backward compatibility is not a design input.** Every node is under our
  control, there is no fallback to an old version, and this release breaks the
  contract deliberately. Design the right shape and migrate the clients.

## The defect

A playback session is currently a property of the auth session rather than a
resource of its own. `create` resolves the logical viewer from the bearer's
session id:

```cpp
// src/playback.cpp:2288
auto logical_session = logical_session_for(request.session->id);
```

`logical_session_for` (`:902-914`) keys a map on that string, and
`session_for_logical_locked` (`:916-921`) returns **the** session belonging to
a logical viewer — singular, by linear scan, first match wins. So one bearer
token has exactly one playback session, and a second `POST` supersedes the
first instead of coexisting with it.

That is what makes the Web Client's handover impossible: a second tab, a second
device, or a resumed session on the same account cannot hold a viewer of its
own. It is also not what `POST` to a collection means.

## The route table

Today (`src/playback.cpp:2774-2789`):

```
POST   /api/v1/playback/sessions
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
GET    /api/v1/playback/stream/{id}/{token}/{generation}/{name}
GET    /api/v1/playback/stream/{id}/{token}/direct
GET    /api/v1/playback/status
GET    /api/v1/playback/media
```

After:

```
POST   /api/v1/playback/sessions                                   201 + Location
GET    /api/v1/playback/sessions                                   the caller's live sessions
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
GET    /api/v1/playback/sessions/{id}/stream/{token}/{generation}/{name}
GET    /api/v1/playback/sessions/{id}/stream/{token}/direct
GET    /api/v1/playback/status                                     unchanged
GET    /api/v1/playback/media                                      unchanged
```

`GET /api/v1/playback/stream/...` is removed outright. There is no dual-serve
window and no deprecation period.

## What each change buys

**1. `POST` to the collection creates a member, every time.** This is the whole
of "multiple sessions per auth session" and it introduces no new identifier.
The server mints the id exactly as it does now (`creation_response`, `:876-889`,
already sets `Location: /api/v1/playback/sessions/{id}`); what changes is that
the new session is not a replacement for the caller's previous one.

**2. `GET` on the collection is what actually unblocks handover.** It does not
exist today — the router matches `POST` on the bare path and `{id}` under the
slash, so a collection `GET` falls through to `404`. With it, handover needs no
new concept: a second client lists the account's live sessions and adopts one
by id. Discovery plus a state transition on an existing resource is the
RESTful spelling of handover. Without it, a client that loses its id cannot
find its own session and the only recovery is to strand it until it expires.

Answers under `items`, like every other collection in this API — that is
already a settled client contract.

**3. The stream becomes a subresource of the session.** Today
`/api/v1/playback/stream/{id}/...` is a second top-level root that re-states
the session id, so one resource is addressable from two unrelated places.
Nesting states the true relationship: a stream exists only inside a session,
and `DELETE /sessions/{id}` is visibly the thing that tears it down.

**4. The token stays in the path.** It is a capability, not a credential:
media players fetch segments without application headers, which is the same
reason the operator's "no non-standard HTTP headers" rule exists. It sits
immediately before the part it authorises, so the URL reads as
"this session, proven by this token, this generation, this object".

## What changes in the code

- `handle_api` (`:2774-2789`): route the new table. The `sessions/` prefix now
  has to split on `/` rather than reject any path containing one, and dispatch
  `stream` as a subresource.
- `public_stream_response` (`:1903-1913`): parse the id and token from their
  new positions. The session lookup and token comparison (`:1917-1919`) are
  unchanged.
- `logical_session_for` (`:902-914`): stop being fed `request.session->id`.
  The `LogicalViewerSession::client_key` field survives but now carries the
  server-issued playback session id.
- `session_for_logical_locked` (`:916-921`): a logical viewer now owns *many*
  sessions. The signature returns one session by linear scan over every
  session on the node; it needs to become an explicit index, or to disappear
  where the caller really wants "this session" rather than "the session of
  this viewer".
- Collection `GET`: needs the account behind a session. Sessions currently
  record the logical viewer, not the account, so the owning account has to be
  carried on `Session` for the listing to be filterable and for the cap below.

## Admission, and the constraint that governs it

**The moment one bearer can hold N sessions, nothing bounds one account.**
`reserve_session_slot` (`:1266-1271`) is node-wide — `config.max_sessions` —
and there is no per-account dimension at all. Today the one-session-per-bearer
rule was doing that job by accident. Removing it without a cap is how a rogue
client launches a media DoS against the node, which the operator named as the
governing constraint for this work.

So the per-account cap is not a follow-up item: it ships in the same change.
It is law 1's second clause as admission control — not making a viewer wait
also means not letting one viewer make another wait.

`reserve_resources` (`:1278+`) already meters transcodes per logical viewer via
`video_transcode_entitled`/`audio_transcode_entitled`. Those entitlements are
per *logical viewer*, so splitting one viewer into many sessions multiplies the
entitlements unless the cap and the entitlement share a key. Decide that key
once, here, and use it in both places.

## Supersession becomes explicit

Today supersession is implicit: the second `POST` wins because there is only
one slot. With a real collection that behaviour disappears, and what replaces
it is a policy on the collection — a per-account cap that refuses, rather than
a silent replacement that succeeds. A refusal a client can see beats a
replacement it cannot.

This also settles the direct-session exemption already on the backlog: direct
sessions are exempt from supersession but still counted against the cap, which
is expressible only once the cap exists.

## What the clients must do

Breaking, deliberately, in four places:

1. Segment and direct-play URLs move under the session. Clients that build
   these themselves must rebuild them; clients that follow the URL the server
   hands them do not care.
2. A second `POST` no longer replaces the first. A client that relied on
   re-`POST`ing to reset its session must `DELETE` and create, or `PATCH`.
3. A client may now hold several sessions and must track which id it is
   talking about. "The session" is no longer inferable from the token.
4. A cap refusal is a new outcome on create.

The four client sessions need this brief before the release, not after it.

## Tests

- A second `POST` on one bearer yields two live sessions with distinct ids,
  and both stream.
- The collection `GET` lists exactly the caller's sessions, under `items`, and
  does not list another account's.
- A stream URL under one session id cannot be used to reach another session's
  objects, with or without a valid token for the other session.
- The per-account cap refuses the N+1th create while a second account is
  unaffected, and the refusal names the cap.
- Transcode entitlements are not multiplied by splitting one viewer into
  several sessions.
- `DELETE /sessions/{id}` tears down that session's stream and leaves the
  caller's other sessions running.

## Out of scope

- `GET /api/v1/playback/status` and `GET /api/v1/playback/media`: different
  resources, unchanged.
- `410 generation_superseded`, still held pending core's tolerance.
- The seek contract, which is settled separately and is unaffected by where
  the session id sits in the path.

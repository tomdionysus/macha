# Plan: a playback session is a resource, not a property of the bearer

Date: 2026-09-21

Status: **Agreed with the operator 2026-09-21, in progress.** The route shape
is settled. Reviewed by the `@machafoundation/core` session the same day; its
findings are folded in below and marked where they changed the design. Core is
co-ordinating the client transition and will brief the other three clients.

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

**The listing is node-local, and that is a decision, not an omission** (raised
by core, settled by the operator 2026-09-21). `GET /api/v1/playback/sessions`
answers for the node that served the request. A client that wants the
account's sessions cluster-wide asks each node it knows.

The bytes were never the objection: a session id is 32 hex characters, ~200
bytes with its fields, and the control lane already gossips a full telemetry
set every 10 s — `propagate_users` broadcasts the *entire* user table on that
tick for exactly this reason. Three arguments decided it:

- **A cluster-visible id promises that any node can act on it, and none can.**
  A session owns node-local resources — a generation directory, a transcode
  slot, a live pipeline, an `engine_session`. `PATCH` and `DELETE` must execute
  where the pipeline is, so a cluster-addressable id implies control-call
  forwarding: new machinery, and a control call whose latency depends on a
  second node's health. That lands on the lane that is already the top P0.
- **The interesting state is hot and gossip cannot carry it.** `touched` moves
  on every segment request. A 10 s roster would carry membership but never
  liveness, so a client would adopt an id that died eight seconds ago — worse
  than not finding it.
- **A session on a dead node would be listed and unusable**, and telling the
  difference needs per-node reachability, which is the fan-out it was trying
  to avoid.

**Provenance falls out for free, and core requires it.** Because the listing is
per-node, a client knows which node each session came from, which is what it
needs to probe, regenerate or release one. Core's `sessionAlive` throws
`Playback generation <id> has no endpoint provenance` without it, and core
already keys sessions as `${endpoint.id}::${nodeSessionId}` — node-scoping is
its existing model, not a new burden. **Stream ids stay node-local too**, and
under this change stop being ids at all: the stream is a subresource and
inherits the session's scope by construction.

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

### The refusal needs its own code (core, 2026-09-21)

`ResourceLimitError` already answers **429** (`src/playback.cpp:3052`), so the
4xx half is right today — but the code is the generic `resource_limit`, shared
with the node-wide session limit and both transcode limits. **An account cap
must be distinguishable from a node limit**, because core classifies failures
by scope: a node-scoped refusal makes it walk the cluster, and an account-scoped
one is identical on every node. Charging every healthy node it walks through
`recordEndpointFailure` turns one account hitting its cap into a cluster core
believes is failing.

So: 429, a distinct code (`account_session_limit`), and the refusal carries the
limit and the caller's current count. If the three-valued `scope` field lands,
`account` is the value this needs — core names this as its forcing case.

### An abandoned session cannot be deleted, and the node keeps counting it

**This is the finding that most constrains the number** (core, from a web
client failover measured against a node killed at the socket, 2026-09-21).

```
source-failover-start
session-stop                DELETE /api/v1/playback/sessions/{id}
session-stop-failed         TypeError: Failed to fetch
failed-session-close-retry  endpointId=http://10.35.1.50:7438 attempts=1
```

The session being abandoned lives on the node that just failed, so **the
cleanup's target is the thing that failed**. Not bad luck — the defining shape
of failover. Core retries on a ladder, gives up, and the session stays live
from that node's point of view. One twenty-minute run left sessions on fi-1,
es-1, gbni-1 and via ramaroja: a cascade strands one on every node it abandons.

A cap that counts those refuses the create that the failover depends on, during
an outage, which is the worst moment and the hardest case to provoke
deliberately. It presents as "failover works in testing and fails in anger".

**One caveat bounds how bad it is; the second one I offered was wrong and is
corrected here.** A node that truly dies loses its sessions with it: the
session map is in-memory, so a restart clears them. The case that actually
strands is a node *alive but unreachable from that client* — a partition, a
CGNAT path, a proxy fault — where the node is fine and still counting. The web
client demonstrated exactly that by isolating a node **inside the browser**:
the process never died, the map stayed intact, and because the session had been
*playing*, the 120 s unused-idle never applied and it held its slot for the
full `session_idle_ms`.

**The second caveat was wrong. I claimed a cascade strands at most one session
per node, so the cap would only bite on a client returning to a node it had
abandoned. `ramaroja` is an haproxy front**, so two entries in a client's
endpoint registry can be the same node under two names. A cascade can strand
**two sessions on one node**, and no client can detect that they are the same
node. Any cap reasoning that assumes one-strand-per-node is unsound; this one
did.

**The live numbers make this urgent rather than theoretical, and they are a
finding about the system as it stands today, not about this change.** On es-1:

```
max_sessions: 8             # node-wide, every account together
max_video_transcodes: 1
session_idle_ms: 1800000    # 30 minutes
```

`session_unused_idle` is 120 s, but that applies only to a session that never
served an object. **An abandoned session that was playing holds its slot for
the full 30 minutes**, because a paused viewer and an abandoned one are
indistinguishable from the server's side — which is precisely why the 30
minutes exists. So a node-wide budget of 8 can be consumed by stranded
sessions, today, before this change adds standbys to the picture.

**`max_sessions: 8` on es-1 needs revisiting in the same breath as the new
cap.** This change raises consumption per viewer, so the node-wide number and
the per-account one have to be chosen together.

**The scarce resource is not the session record.** A session is a map entry; a
transcode is a core. es-1 admits **one** video transcode. That reframes the
cap's job: it exists to stop a rogue client minting unbounded cheap records,
while the expensive resource is already bounded separately and per viewer. A
cap whose job is bounding cheap records can afford to be generous — which is
what dissolves the tension in the next section.

### The cap number has a floor, and it is not small (core, 2026-09-21)

**Core holds more than one session per account by design.** `alternateSessions`
is a map: the live generation plus one or more standbys, because a standby is
prepared on another node *before* the live one dies — that is how seamless
failover works. During a failover core can briefly hold three: the dying
session, the standby, and the new one.

So one viewer, one film, one device is routinely **2** sessions and transiently
**3**, all on one account and all correct. A household with two televisions
needs **6** before anyone does anything unusual. A cap of 2 or 3 would not
present as a cap; it would present as seamless failover mysteriously ceasing to
work at the moment it is needed — a silent break in the feature this is all in
service of.

**Corrected the same day: 3 is one client's floor, not anyone's ceiling.** The
phone client holds **1, transiently 2** — no standby; both warm-standby
attempts were tried and reverted. So a cap justified as "core needs 3" must not
be set at 3. And **adoption through the new collection listing costs budget by
design**: a client that adopts a session while holding its own is 2, even if it
otherwise looks like a client that holds 1. The handover feature this plan
exists to enable is itself a consumer of the cap.

**A per-account cap is a cap on a household.** From the Android TV client's
seat: two televisions, a phone, and whoever is on the web client — four viewers
on one account before a single standby exists, and comfortably into double
figures during any disturbance once core's standby discipline applies to the
coordinator-driven ones. That session reads anything under 8 as tight and would
rather the limit were per viewer-session than per account.

**The key is an open decision for the operator** (raised 2026-09-21, not
settled). The plan already says the cap and the transcode entitlement must
share a key; "which key" is the same question:

- **Per account** is the only thing that answers the stated threat — *"a rogue
  client cannot under any circumstances launch a media DoS"*. A per-viewer cap
  is no defence at all, because a rogue client simply claims more viewers.
- **Per account also caps a family**, which is the objection, and it is a real
  one at four viewers before standbys.
- The reframing above is what reconciles them: the cap bounds *cheap records*,
  and the expensive resource — transcodes, one on es-1 — is bounded separately
  and per viewer. A generous per-account cap plus the existing transcode
  admission gives the rogue-client protection without capping the household.

The default therefore has to clear a plausible household's transient peak by a
wide margin, and the stranded-session case above means it must also survive a
failover cascade returning to a node. **Operator's number to set.**

**The number is 32, and the default carries its own arithmetic** (set
2026-09-21; the cap is the node's and configurable as
`streaming.max_sessions_per_account`). Four viewers before any standby exists;
two per viewer steady and three transiently during a failover, so four viewers
in disturbance is twelve; strands hold a slot for the whole of `session_idle`
and a cascade can leave more than one on the same node. Twelve plus a
cascade's worth of strands is what it has to clear **on the worst day**, not
the average one. Zero disables the bound.

**Strands are normal, not exceptional**, and the cap has to be chosen that way
(web client, relayed 2026-09-21): *a client that cannot reach a node cannot
release its session, by construction.* That is not a client defect awaiting a
fix — it is what failover **is**. The node you are walking away from is the one
you cannot talk to.

**Where the limit and the count live.** The limit is configuration and appears
on `GET /api/v1/playback/status`, which clients already read and cache per
node. The count is state, and it is the most perishable number this API
carries — it moves whenever anyone on the account starts or stops anything,
from a device neither end can see. So the count appears **only on surfaces
computed live at the moment of the response**: the creation payload, the
collection listing, and the refusal. It is deliberately **absent from
`playback/status`**, because that is the surface clients cache, and a count
that can only arrive fresh cannot be read stale. Core asked for an age on the
count instead; an age measured at emission is always zero, and what actually
ages is the client's own copy, which the client knows better than this node
does.

**Publish the limit, do not make core discover it by refusal.** Core would
rather read the cap and the current count before it plans than learn them by
being refused at the worst possible moment. Given it, core stops preparing
standbys it knows will be refused, and a host can say "no standby available,
account at its session limit" — which is actionable — instead of showing a
failover that simply failed. This is also why the cap and the transcode
entitlement must share a key: a standby holds a transcode slot on its node,
which is why core releases transcode standbys after 8 s and remux ones after
30.

## Supersession becomes explicit

Today supersession is implicit: the second `POST` wins because there is only
one slot. With a real collection that behaviour disappears, and what replaces
it is a policy on the collection — a per-account cap that refuses, rather than
a silent replacement that succeeds. A refusal a client can see beats a
replacement it cannot.

This also settles the direct-session exemption already on the backlog: direct
sessions are exempt from supersession but still counted against the cap, which
is expressible only once the cap exists.

## What the clients must do — mostly nothing (core, 2026-09-21)

Core checked this against its own source rather than estimating, and three of
the four items I expected to break are no-ops:

1. **Stream URLs: no-op, and now verified in all four repositories.** Core
   builds none — `grep -rn "playback/stream" src` finds only the barrel export.
   `MachaPlaybackResolver.streamUrl()` absolutises whatever the server returns
   and never composes a path, and `hlsWalk` resolves playlist-relative
   references against that URL, which is ordinary HLS and follows the move.
   Every repository was then grepped for composed playback paths: web has four
   hits, all test fixtures; phone has only relative module imports and one
   composed URL built from core's exported `LIVENESS_PATH`; Android TV has two
   comments and no code, Kotlin engine and scripts included. **No client builds
   one.** The URL half of this break costs the fleet some test fixtures.

   **The condition, now core's written commitment: core keeps handing back
   absolute, server-supplied URLs.** Every downstream consumer feeds a native
   player or a downloader rather than a `fetch`, so a relative URL would break
   all of them at once, and silently.
2. **Re-`POST` to reset: no-op.** `regenerate` already releases the old session
   before creating — the DELETE-then-create this plan prescribes — and
   `failover`/`prepareAlternate` create on a different node.
3. **Tracking which session: no-op.** Core has keyed sessions by explicit id all
   along, `${endpoint.id}::${nodeSessionId}`. "The session" was never inferred
   from the bearer there. Core's backlog recorded that it survived
   one-session-per-bearer on three incidental facts; **this change removes that
   exposure rather than creating one.**
4. **The cap refusal is the one real new outcome**, and it must not be walked or
   charged. See the refusal-code section above.

So the client-visible cost of this change is far lower than it looked when the
plan was written. What is left is the cap, and the sequencing below.

## Security review of /api/v1/playback (2026-09-21)

Asked for by the operator once the routes started moving. Every route under the
prefix, against authentication, authorisation, input validation, information
disclosure and resource exhaustion.

### Fixed in this change

**1. No ownership check on the session control routes.** `get_session`,
`update_session` and `erase_session` looked a session up by id and acted on it
without asking who was calling — `get_session` and `erase_session` did not even
receive the request. Any authenticated account that learned an id could read,
re-seek or **delete another viewer's session mid-film**, and the per-account cap
would have meant nothing because a stranger could free your slots.

Pre-existing, but latent: under one-session-per-bearer an id was only ever known
to the client that made it. This change makes sessions plural, long-lived and
**enumerable through a listing that hands ids out**, which turns it from latent
into practical. All three now check ownership and answer **404, not 403** —
whether an id exists on this node is not something one account gets to learn
about another.

**2. Ownership was silently dropped by every session replacement.** A session
is replaced wholesale on a subtitle change, a fast-path seek and a mode change,
each copying fields one at a time — and the new `account` field was copied by
none of them at first. The consequence is worse than a 404 on the owner's next
request: a replaced session **stops counting against the per-account cap**, so
a subtitle change becomes a way to launder sessions past the limit. Caught by
an existing test rather than by inspection. All three replacement paths now
carry it.

**3. The authentication exemption and the router disagreed about what a stream
URL is.** The exemption matched `/stream/` anywhere after the session id while
the router required it as the exact next segment. A path that is exempt from
the bearer but routed somewhere other than the stream handler is an
authentication bypass, and two spellings of "is this a stream request" is how
that arrives. Both now call one `parse_stream_route`. The old predicate was a
bare `starts_with` on the old top-level prefix, which was safe only because the
stream lived at a root of its own; nested under the session it is not.

**4. Idempotency keys were a global namespace.** `idempotent_creations` was
keyed on the client-chosen key alone, so any authenticated account could occupy
another's key and turn that account's legitimate retry into a
`409 idempotency_conflict`. Keys are often predictable (`retry-1`). Not a
takeover — `creation_fingerprint` includes the auth session id, so a stolen key
conflicts rather than replaying someone else's session — but a targeted denial
of the retry path, which is the path a client is on when something has already
gone wrong. Now scoped per account.

### Checked and clean

- **Path traversal on the stream object name.** `..` is rejected, and any `/`
  is rejected before the store lookup, so an absolute name cannot reach a
  filesystem join. The subtitle branch parses an index rather than building a
  path.
- **Cross-account idempotency replay.** The fingerprint carries the auth
  session id, so a replay attempt conflicts rather than handing over another
  account's session id and token.
- **Token strength.** HMAC-SHA256 under the cluster auth key, 256 bits, hex.
- **Idempotency map growth.** Bounded by live sessions: entries are erased on
  the creation error path and when their session is erased.
- **Role mapping.** Everything under the prefix requires `media_viewer`
  (`src/service.cpp:235`), including creation.

### Open, not fixed, with a recommendation

- **The stream token is compared with `!=` on a `std::string`**
  (`src/playback.cpp`, `session_stream_response`), which is not constant time.
  It is a capability, so the comparison is a secret-dependent branch. Remote
  timing exploitation across a network against a 256-bit hex token is not a
  practical attack, which is why this is recorded rather than rushed — but a
  constant-time compare is two lines and removes the question.
- **`GET /api/v1/playback/status` exposes node aggregates to any
  `media_viewer`**: session count, transcode load, cached probe bytes, and now
  `max_sessions_per_account`. A viewer learns how busy the node is and how many
  sessions other people hold in total. Defensible for a household system and
  useful to clients; flagged because it is a deliberate disclosure rather than
  an accident, and the operator should say so rather than discover it.

### A note on shape, per the operator

**No new headers and no protocol extensions.** Everything added here is a field
in the existing JSON envelope: the cap on the listing and on `playback/status`,
the limit and count inside the existing `error` object on the refusal. Status
codes are the standard ones — 201 with `Location`, 429 for the cap, 404 for an
id the caller does not own, 409 for an idempotency conflict. The two
`X-Macha-*` headers on creation predate this work and are unchanged.

## Sequencing (core drives; agreed 2026-09-21)

**Core ships tolerance first, the clients take it, then the nodes move.** This
is the standing rule in this pair of repos and it has been broken twice.

1. **Core and the web client** release tolerance for `410
   generation_superseded`, the cap status, and adoption provenance. Corrected
   by the web client on 2026-09-21: *a `410` on a segment never reaches core as
   a status.* hls.js raises it and the web client's own classifier sorts it
   first — `500` is a hold, `404` is not-found, and everything else falls to a
   network-degradation branch reported as `stream`, which is evidence against
   the endpoint. Same failure mode as core's, one layer lower, and it needs its
   own `410` branch.
2. The clients take that release. Core briefs all three and co-ordinates.
3. The routes move here.

**Core's `410` tolerance is built on its `develop`** (2026-09-21). It maps
`SOURCE_SUPERSEDED_STATUS = 410` to the existing `not-found` kind rather than a
new one, deliberately: the required action is identical — the object is gone,
the node is fine, ask the session route — and `not-found` already carries the
obligation that an adapter must not tear the presentation down. A seventh kind
would put that obligation behind a value existing hosts meet as `default`, so
an un-updated host would read `410` as unhandled and condemn a healthy node,
which is the exact failure the tolerance exists to prevent. Three tests, two
verified red against the branch.

**It is not published to npm, and that needs the operator's word.** The route
move here is gated on the clients being on a published core that carries it.

**`410 generation_superseded` should be bundled into this release.** It is
already held pending exactly this tolerance, and a coordinated route break is
the right release to carry it — rather than holding it for a second flag day.
No client is then ever pointed at a node whose statuses it cannot classify.

## Tests

- A second `POST` on one bearer yields two live sessions with distinct ids,
  and both stream.
- The collection `GET` lists exactly the caller's sessions, under `items`, and
  does not list another account's.
- A stream URL under one session id cannot be used to reach another session's
  objects, with or without a valid token for the other session.
- The per-account cap refuses the N+1th create while a second account is
  unaffected; the refusal is 429 with the account-specific code, and carries
  the limit and the current count.
- Three concurrent sessions on one account — core's transient failover peak —
  are admitted under the default cap. This is the case a too-small number
  breaks silently.
- Transcode entitlements are not multiplied by splitting one viewer into
  several sessions.
- `DELETE /sessions/{id}` tears down that session's stream and leaves the
  caller's other sessions running.

## Out of scope

- `GET /api/v1/playback/status` and `GET /api/v1/playback/media`: different
  resources, unchanged.
- Cluster-wide session listing or a cluster-addressable session id: decided
  against above, with reasons.
- The seek contract, which is settled separately and is unaffected by where
  the session id sits in the path.

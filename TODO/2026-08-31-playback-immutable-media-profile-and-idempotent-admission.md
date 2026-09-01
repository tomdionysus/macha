# Playback immutable media profiles and idempotent admission

Date: 2026-08-31

Status: dedicated engine implemented, tested and deployed; live latency UAT remains.

## Live trigger and causality

Macha 0.21.0 synchronously admitted an explicitly Direct Play request in
9.016 seconds on node 50 and 11.351 seconds on node 51 for immutable media
`macha:bd5c2e129cd4703329f3ff2d3038df9ffab592ed98f21afe9425e14afdae16a9`.
The node-50 trace attributed about 9.002 seconds to libav probing: nine serial
256 KiB reads at separated offsets each caused a complete remote 4 MiB extent
read.

The latency has two distinct causes:

1. **Structural defect:** `PlaybackManager::probe_cache` is process-local,
   starts empty, is populated only by session admission and has no in-flight
   owner. Every cold process and concurrent cold caller therefore probes again.
   Catalogue discovery records the immutable media ID and provider/title data,
   but not the full playback `MediaProbeResult`. This predates and is independent
   of the current FUSE publication work.
2. **Load-sensitive multiplier:** rsync/publication traffic can make each remote
   extent read slower. The observed 9--11 second total must not be treated as an
   unloaded storage baseline. This work removes unnecessary reads; it does not
   assume every observed per-extent latency is permanent.

The same trace exposed an independent API correctness defect: after the client
abandoned an eight-second POST observation, the server completed a new session
about one second later. Session creation currently has no request identity, so
the client cannot safely distinguish failure from a committed response.

## Durable media-profile contract

Store one complete normalized playback profile in the cluster-replicated
catalogue snapshot, keyed by the content-derived `macha:` media identity. The
profile contains:

- profile schema version and completeness marker;
- container/format;
- exact duration representation and aggregate bitrate; and
- every stream's index, type, codec, profile, bitrate, dimensions, channels,
  sample rate, bit depth, language, default/forced flags and attached-picture
  flag.

Validity rules:

- a profile is usable only for its exact media-ID key;
- `macha:` identity is derived from file size plus the immutable extent
  manifest, so changed content produces a cache miss by construction;
- path-only compatibility identities retain the existing version/size/mtime
  evidence and are not promoted into the durable immutable profile map;
- unknown schema, non-finite/invalid duration, duplicate/invalid stream indexes,
  invalid enum values, missing format/codec data required for negotiation, or
  impossible numeric fields make only that profile unusable;
- authenticated catalogue-object corruption continues to fail at the existing
  content-addressed control-store boundary; a semantically incomplete profile
  is treated as a miss and queued for optional background regeneration; and
- successful background probes are validated before publication and become
  reusable cluster-wide.

The catalogue codec must read the existing MCAT0018 representation and a new
version containing profiles. Writing the new representation is a coordinated
cluster upgrade boundary: old 0.21.0 nodes cannot consume a new catalogue root.
Deployment must therefore upgrade all metadata/catalogue readers before profile
publication is enabled, or add an explicit capability gate if rolling mixed
versions are required.

## Probe ownership and scheduling

Use one process-wide media-profile resolver for background catalogue indexing:

- catalogue/hint processing probes missing profiles as speculative background
  work and commits them with the same immutable media binding where practical;
- playback checks validated cluster metadata but never generates a profile in
  its foreground request;
- a genuine miss queues background work and returns an immediate retryable
  response rather than waiting for a probe;
- concurrent background requests for the same immutable ID use hint/probe
  coalescing and never duplicate source reads;
- background probing remains bounded by the configured probe timeout and
  shutdown cancellation;
- success is cached and published; failure wakes every waiter and leaves no
  false valid profile; and
- ordinary session admission performs no media-object reads when a valid
  immutable profile exists.

Expose validated profiles at `GET /api/v1/catalogue/media/{media_id}/profile`.
Clients may cache the response only by immutable media ID. A missing profile is
temporary: catalogue work should precompute it in the background. A direct
profile GET returns `202 Accepted`, and session creation returns the immediate
retryable `425 profile_pending`; neither request waits for profile generation.

The stored profile must feed the existing `negotiate`, `session_json`, output,
options and stream-selection code unchanged. No parallel approximation of
Direct/Remux/Transcode policy is permitted.

## Idempotent session creation

Accept a client-generated `Idempotency-Key` on session-creation POSTs. Parse the
request into its semantic selector, normalized capabilities, preferences and
initial seek before deriving a request fingerprint.

- Same key plus same semantic fingerprint joins an in-flight creation or
  returns the existing session and generation without reserving another slot,
  probe, transcode or lease.
- Same key plus different semantics returns `409 idempotency_conflict` while the
  prior association is live.
- Session identity and authorization material must be deterministically derived
  from a domain-separated cluster secret, request key and semantic fingerprint.
  This permits a cold/restarted node to recover generation 1 for an identical
  POST without making one node a permanent owner.
- In-process pipeline/session objects remain ephemeral and node-local. The
  client continues to own media choice, preferences, queue, position and
  failover policy.
- The design remains compatible with a future ownerless, expiring
  cluster-visible existence/authorization record. Such a record may reconcile
  authorization/generation but must not become server-owned playback state.
- Requests without a key retain the existing non-idempotent API for compatibility
  during the client migration window. Responses should echo the accepted key or
  an explicit idempotency status without changing the existing JSON semantics.

## Tests required before implementation

- Valid cached immutable profile creates a session with zero media-engine probe
  calls and zero probe-source reads.
- A new PlaybackManager/cold service instance reuses cluster-persisted metadata,
  not merely an in-memory warm entry.
- Repeated session creation reuses the same profile.
- Concurrent cache misses enqueue/coalesce exactly one background probe and
  return without waiting.
- Replacing media creates a different identity and cannot reuse the old profile.
- Cached and independently background-probed session responses are semantically identical after
  removing intentionally dynamic trace/session URL fields.
- Direct, Remux and Transcode negotiation decisions match the existing probed
  path across representative capabilities/preferences.
- Unknown, incomplete and semantically invalid profiles queue safely without
  synchronous source reads.
- Same-key concurrent, completed and cold-manager retries return the same
  session/generation; different semantics conflict; no-key compatibility remains.
- Failure/cancellation releases pending session/transcode reservations and wakes
  idempotency/probe joiners.

## Diagnostics and UAT

Log and expose counters/timing for immutable metadata lookup, cluster-profile
hit, queued background generation, pipeline selection and total admission. Keep
the existing trace ID and stage-specific failures.

After all nodes are upgraded, measure on nodes 50 and 51:

1. cold process with an already catalogued/profiled immutable item;
2. repeated session creation;
3. one deliberately uncached cold miss and a concurrent duplicate, proving
   both return immediately while one background job is queued;
4. explicit Direct, representative Remux and representative Transcode;
5. same-key retry after deliberately abandoning the first client connection;
6. rsync loaded and unloaded comparisons, clearly separating elimination of the
   structural probe from load-sensitive physical I/O.

Acceptance: the profiled Direct Play path performs no media-object reads and
normally admits within hundreds of milliseconds or less on the LAN. No timeout
increase is part of the solution.

## 2026-08-31 implementation checkpoint

Completed and tested:

- the detailed profile codec and semantic validation, keyed only by immutable
  `macha:` identity, with backward decoding of MCAT0018 catalogue shards;
- cluster-catalogue persistence and cold-`PlaybackManager` reuse with no media
  engine probe call;
- non-blocking first-use miss handling, successful background profile
  publication and concurrent cold-miss coalescing;
- changed-content identity isolation and cached/probed response equivalence; and
- `GET /api/v1/catalogue/media/{media_id}/profile`, including immutable cache
  headers and the complete playback-relevant stream profile.

Focused tests passed: the cold-manager/changed-identity/endpoint regression, the
concurrent-miss regression, and the catalogue scanner/codec integration test.
At that point background catalogue/loader precomputation, idempotent session
POSTs, remaining policy/corruption matrices, full-suite verification and live
latency UAT remained.

Subsequent work in this checkpoint completed background profile generation in
the catalogue hint batch using speculative/background reads and the same process-wide
probe coalescer as playback. It also added `Idempotency-Key` session creation:
same-key semantic duplicates join/replay one creation, conflicting reuse returns
`409 idempotency_conflict`, and cluster-key-derived credentials allow a cold node
to reconstruct the same session ID, authorization token and generation without
becoming a permanent owner. The response additions are `generation`, echoed
`Idempotency-Key`, and `X-Macha-Idempotency`; requests without the header retain
their prior behavior.

Local verification after these changes: build succeeded, all 231 core tests
passed, and all 3 runtime/libav dependency tests passed.

Final non-blocking correction: media-profile generation is speculative
background catalogue work, never foreground playback or loader work. Production
session admission checks the immutable catalogue profile without opening the
media; a miss queues generation and immediately returns `425 profile_pending`
with `Retry-After`. The profile endpoint similarly returns `202 Accepted` after
queueing. Explicit re-requests reopen terminal hint records so a cleared or
previously incomplete profile cannot remain pending forever. The synchronous
resolver remains injectable only in isolated policy tests; the production
`Service` always supplies the non-blocking queue callback, and the service-level
regression verifies no probe call occurs on a miss.

Final local verification: build succeeded, the focused playback suite passed
15/15, the complete core suite passed 233/233, and the runtime/libav dependency
suite passed 3/3. One high-concurrency run transiently failed
`test_fuse_operation_journal_admission_is_bounded_while_busy`; that unrelated
test then passed four isolated repeats and the complete six-slot rerun.

Deployment checkpoint: the tested 0.22.0 source was compiled and installed on
nodes 50 and 51; both `/usr/bin/macha` files exactly match their respective
build outputs and have the same SHA-256
`88e4816a9b21c511c7a28beedfb7e2f0ba83c61665d694abd4250f943a98d692`.
Both systemd services are enabled and active, and the local 0.22.0 node 200 was
started from `build/macha`. Aggregated status confirms all three nodes online,
all reporting 0.22.0 and local metadata generation 1552.

Live profile/session UAT is blocked, not passed: the cluster reports metadata
unavailable because its accepted heads have no known common ancestor. Historical
journal evidence proves this predates deployment (node 51 logs it from at least
19:25, including failed playback at 19:26). The upgraded cluster remains safely
fail-closed; no metadata reset, winner selection, or destructive repair was
attempted. Complete cold/repeated profile latency and idempotency UAT after the
separate accepted-history defect is repaired.

The attempted node-50 profile request confirmed the boundary precisely: it
returned `503 catalogue_unavailable` in 44 ms with `divergent metadata heads
have no known common ancestor`. This is a prompt fail-closed response, not the
old nine-second synchronous media probe, but it cannot validate profile-cache
hits until catalogue authority is restored.

## 2026-09-01 advisory-profile fallback correction

The first 0.22.0 implementation made speculative profile generation too
authoritative: session admission returned `503 profile_unavailable` when the
background scanner could not accept work. On a newly joined ES-1 this produced
`immutable media profile is unavailable and could not be queued`, even though
the configured in-process libav engine could still probe and stream the media.

The corrected contract is:

- a valid immutable profile remains the preferred zero-read admission path;
- a genuinely queued or running background job returns immediate
  `425 profile_pending` with `Retry-After`, before idempotency ownership,
  session reservation, media-source opening, probing, planning or pipeline work;
- if speculative profiling is unavailable or has reached a terminal state,
  session admission continues through the existing bounded media-engine probe
  and unchanged Direct/Remux/Transcode planning path;
- the profile GET endpoint still returns `202 profile_pending` while work is
  outstanding and may return `503 profile_unavailable` for a terminal or
  unavailable advisory job; that metadata endpoint does not decide whether
  playback is possible; and
- an explicit request for the same immutable profile no longer reopens the
  same terminal catalogue hint forever. A changed immutable identity remains
  new work.

Regression coverage proves unavailable profiling falls back and admits,
terminal failure stops returning endless pending responses, a pending response
performs zero engine probes and zero VOD preparation, and retrying the same
idempotency key after failure creates exactly one session and subsequently
replays the same session ID and generation.

Verification: focused profile tests passed 6/6, the complete core suite passed
238/238 (including `test_catalogue_sync_search_and_artwork_gc`), and runtime
libav/configuration tests passed 3/3. The correction was then built, installed
and started on nodes 10.44.1.50, 10.44.1.51 and 10.34.1.50, and node
10.44.1.200 was restarted from the tested local build. All four live nodes
reported Macha 0.22.0 and metadata generation 1574; the cluster was writable.
Real ES-1 playback remains the operator UAT gate.

## 2026-09-01 superseding media-information contract

The earlier `425 profile_pending` session contract in this document is
superseded. Profiles are advisory optimisation data and never a client-visible
playback prerequisite. A dedicated event-driven service now owns durable hints,
priority ordering, immutable-ID deduplication, speculative scans, publication
and last-live-copy pruning. Ingest and catalogue paths only submit hints.

Session negotiation consumes a valid stored profile with no media reads. On a
genuine miss it continues the normal bounded media-engine path as viewer work;
concurrent requests share a flight, and viewer negotiation cancels/takes over a
speculative owner. Success is published asynchronously for reuse. Only the
standalone profile GET endpoint returns `202 profile_pending`, and clients may
start playback negotiation immediately.

Final verification and deployment details, including the immediate ES-1 smoke
UAT, are recorded in
[the dedicated media-information checkpoint](2026-09-01-media-information-engine.md).

# Playback immutable media profiles and idempotent admission

Date: 2026-08-31

Status: investigation/design; tests must precede risky probe/session changes.

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
  degrades to a bounded probe miss rather than poisoning the catalogue; and
- successful fallback probes are validated before publication and become
  reusable cluster-wide.

The catalogue codec must read the existing MCAT0018 representation and a new
version containing profiles. Writing the new representation is a coordinated
cluster upgrade boundary: old 0.21.0 nodes cannot consume a new catalogue root.
Deployment must therefore upgrade all metadata/catalogue readers before profile
publication is enabled, or add an explicit capability gate if rolling mixed
versions are required.

## Probe ownership and scheduling

Use one process-wide media-profile resolver shared by catalogue indexing and
playback:

- catalogue/hint processing probes missing profiles as loader work and commits
  them with the same immutable media binding where practical;
- playback first checks validated cluster metadata, then the local validated
  cache;
- a genuine viewer miss owns one bounded foreground probe;
- concurrent misses for the same immutable ID join that in-flight result using
  condition-variable completion, never duplicate source reads;
- each waiter retains its own admission deadline; the probe itself remains
  bounded by the configured probe timeout and shutdown cancellation;
- success is cached and published; failure wakes every waiter and leaves no
  false valid profile; and
- ordinary session admission performs no media-object reads when a valid
  immutable profile exists.

Expose validated profiles at `GET /api/v1/catalogue/media/{media_id}/profile`.
Clients may cache the response only by immutable media ID. A missing profile is
temporary: catalogue/loader work should precompute it in the background, while
session creation retains the bounded, coalesced first-use fallback.

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
- Concurrent cache misses execute exactly one probe and wake all callers.
- Replacing media creates a different identity and cannot reuse the old profile.
- Cached and fallback-probed session responses are semantically identical after
  removing intentionally dynamic trace/session URL fields.
- Direct, Remux and Transcode negotiation decisions match the existing probed
  path across representative capabilities/preferences.
- Unknown, incomplete and semantically invalid profiles fall back safely.
- Same-key concurrent, completed and cold-manager retries return the same
  session/generation; different semantics conflict; no-key compatibility remains.
- Failure/cancellation releases pending session/transcode reservations and wakes
  idempotency/probe joiners.

## Diagnostics and UAT

Log and expose counters/timing for immutable metadata lookup, local hit,
cluster-profile hit, coalesced wait, fallback probe, pipeline selection and total
admission. Keep the existing trace ID and stage-specific failures.

After all nodes are upgraded, measure on nodes 50 and 51:

1. cold process with an already catalogued/profiled immutable item;
2. repeated session creation;
3. one deliberately uncached cold miss and a concurrent duplicate;
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
- bounded first-use fallback, successful profile publication and concurrent
  cold-miss coalescing;
- changed-content identity isolation and cached/probed response equivalence; and
- `GET /api/v1/catalogue/media/{media_id}/profile`, including immutable cache
  headers and the complete playback-relevant stream profile.

Focused tests passed: the cold-manager/changed-identity/endpoint regression, the
concurrent-miss regression, and the catalogue scanner/codec integration test.
At that point background catalogue/loader precomputation, idempotent session
POSTs, remaining policy/corruption matrices, full-suite verification and live
latency UAT remained.

Subsequent work in this checkpoint completed background profile generation in
the catalogue hint batch using loader-priority reads and the same process-wide
probe coalescer as playback. It also added `Idempotency-Key` session creation:
same-key semantic duplicates join/replay one creation, conflicting reuse returns
`409 idempotency_conflict`, and cluster-key-derived credentials allow a cold node
to reconstruct the same session ID, authorization token and generation without
becoming a permanent owner. The response additions are `generation`, echoed
`Idempotency-Key`, and `X-Macha-Idempotency`; requests without the header retain
their prior behavior.

Local verification after these changes: build succeeded, all 231 core tests
passed, and all 3 runtime/libav dependency tests passed.

# Complete VOD playlist with bounded segment holds

Date: 2026-09-08

Operator decision, taken after a design discussion the same day: transformed
playback should serve a **complete VOD playlist immediately**, and make the
wait for not-yet-produced media the server's problem, bounded and safe. The
growing EVENT playlist is wanted eventually, but not now — a stable, fully
planned, bounded playlist comes first.

## Why, honestly

This change was prompted by a report from the Macha UI session that the EVENT
playlist broke the client's scrubber and made past-frontier seeking
impossible. **That report was retracted in full.** The client already reads
`duration_ms` from the session response and displayed 1:31:46 with a scrubber
maximum of 5,506,272 ms on the exact session reported as broken; past-frontier
seeks already work through `PATCH seek_ms`, and the failing measurement had
set `video.currentTime` directly, bypassing the client's own seek path. A
follow-up hypothesis that the observed `levelLoadError` was an hls.js
`levelLoadingTimeOut` was also retracted — hls.js reports an expired deadline
as `levelLoadTimeOut`, a distinct value, and the log says `levelLoadError`.

So none of the originally reported symptoms justify this change, and the
record should not pretend otherwise. What justifies it is separate:

- **It is the spec-correct form.** HLS VOD playlists are complete and closed.
  Our duration is known — we probed it. `EVENT` is for live-to-VOD recording
  where the duration genuinely is not yet known.
- **It is what comparable just-in-time servers do.** Plex, Jellyfin and Emby
  all compute a complete playlist from known runtime and segment length before
  any segment exists, then block the segment request while the encoder catches
  up.
- **It removes the playlist poll.** With `ENDLIST` present, hls.js stops
  polling entirely: one fetch per session instead of hundreds. That removes a
  continuous stream of opportunities for a level-load failure to be read as
  node health — the one part of the UI session's report that survives, and
  which is still unexplained (a `levelLoadError` with no HTTP status on either
  side; every poll captured returned 200 and the node's journal recorded no
  404 or 503 all day).
- **The error moves to a better channel.** A playlist failure is
  `levelLoadError`, which the client weighs as node health and which promoted
  a live session off gbni-1 onto the WAN node. A fragment failure is
  `fragLoadError`, with its own retry policy and no failover consequence.

What this change explicitly does **not** fix: the 43–47 s cold first segment
on DTS/TrueHD sources. That is over every client timeout, so no amount of
holding rescues it. It is tracked separately as its own P0 and is not a
prerequisite for this work — but neither should this work be described as
improving it.

**Retracted 2026-09-08, after this plan shipped.** That 43–47 s figure was
confounded by node choice: the client had settled on the wireless node, whose
read throughput is roughly a sixth of the wired one's. Re-measured on gbni-1
with no server change, the same title's first segment is 3.5 s. There is no
DTS/TrueHD decode problem, and the paragraph above should be read as a
correctly-scoped disclaimer about a fault that turned out not to exist rather
than as a description of one. The deadline reasoning it cites was separately
wrong too — `fragLoadingTimeOut` is deprecated and inert; see phase 4.

## What 0.36.0 already established

This plan depends on one property that did not hold until today, and now does:

**The plan predicts the output exactly, one fragment per planned entry.** The
early `moov` flush was disabled for any transcoded stream, because it was
driven from the demux copy loop and a transcoded stream's first packet arrives
from an encoder instead. So on transcode the delayed `moov` was written at the
first real fragment boundary and consumed it, merging fragments 0 and 1: the
pipeline produced 25 fragments for a 26-entry plan and fragment 0 carried 6.0 s
of media where 2.0 s was planned. Each pipeline now records its own first muxed
packet, so transcode gets the same early flush remux always had. Measured after
the fix: 26 fragments for a 26-entry plan, 11 for 11, fragment 0 back to
1.96 s.

That matters twice over. A playlist written before anything is published can
only carry planned durations, so the plan has to be right or the playlist lies
from its first line. And a 2 s first fragment is materially less to wait for
than a 6 s one, which is the only startup-latency lever this plan has.

Gated by `tests/test_transcode_timeline.cpp`, which checks fragment count
against plan count and every declared duration against the media actually
carried.

## Design

### The playlist

- `#EXT-X-PLAYLIST-TYPE:VOD`, every planned entry, `#EXT-X-ENDLIST`, served on
  the first fetch with no readiness gate. `playlist()` no longer requires a
  published fragment, and the HTTP layer no longer holds the request on
  `wait_ready`.
- `EXTINF` comes from the plan, necessarily: an unproduced fragment has no
  measured length, and a VOD playlist must be immutable across fetches, so a
  produced fragment cannot be described differently from an unproduced one.
  This is only honest because of the 0.36.0 fix above.
- `EXT-X-TARGETDURATION` from the longest planned duration.

### One hold path for any requested object

`MediaSegmentStore::wait_object` currently returns immediately for any
non-segment name (`if (!parsed) return object(name);`), so `init.mp4` would
answer 404 the instant the playlist is served early — the client asks for it
before the first `moof` has been written. Extend the wait to cover init
publication so there is a single hold path for anything a client can request,
rather than a special case per object kind.

### Admission policy

Three independent tests, in order. Any failure is an immediate refusal, never
a hold:

1. **Window.** Hold only if the requested index is within
   `streaming.segment_hold_window` (default 8) of the produced frontier. Not
   arbitrary: it is the distance `max_ahead_segments` already permits the
   producer to run ahead, so a request inside it is one production is
   authorised to reach and a request outside it is one nothing is working
   toward.
2. **Per-session holds.** At most `streaming.max_session_holds` (default 2)
   outstanding per session — one in flight plus one prefetch. This alone kills
   the 0.32.14 scenario: a native player that queues thirty requests gets one
   hold and twenty-nine immediate refusals, instead of thirty sequential waits
   on the encoder.
3. **Global hold budget.** At most `streaming.max_concurrent_holds`
   (default 8) across the node.

Refused requests must **not** advance `highest_requested`. Noting an index we
declined would drag the producer's authorised window forward on behalf of a
request we refused to serve.

### Why the global budget exists, and what changes when the server goes async

`HttpServer` uses a fixed worker pool — `catalogue.api.workers`, 16 on the
cluster — shared by every route: Status, catalogue, manage and playback alike.
A held request occupies one of those workers for the whole of its wait. With
`max_sessions: 8` and an 8-segment window, the unbounded worst case is 64 held
requests against 16 workers, and a single deeply prefetching player can take
all 16 by itself. At that point the node stops answering Status and control
traffic, which is a direct governing-law-3 violation. This is very likely part
of what the 98 s black screen actually was: not only the player waiting on the
encoder, but the pool exhausted while it did.

It is also the honest caveat on "this is what Plex and Jellyfin do". They do —
on async runtimes, where a held request costs a continuation and not a thread.
Ours costs a thread. The pattern is right; our runtime does not give it away.

**Build it so async is an improvement, not a rewrite.** The rule is that a
hold must be an *explicitly admitted resource*, acquired before waiting and
released after, in the shape `DataResourceArbiter` credit and
`RetainedMemoryLedger` leases already use here — never an implicit consequence
of a thread happening to block. Then:

- the admission policy above is pure and unchanged under async;
- the deadline is a property of the request, not of a thread;
- only the waiting primitive is replaced (block on a condition variable ->
  register a continuation);
- the global budget survives as a fairness and memory bound rather than a
  thread-protection one, and its default can rise substantially.

Nothing in this plan should encode "a hold costs a worker" anywhere except in
the default value of the budget and a comment saying why it is that low.

**Note for whoever moves `HttpServer` to async:** revisit
`streaming.max_concurrent_holds`. Its default is small because it is rationing
a 16-thread pool. Under async the constraint becomes memory and fairness, and
the natural value is much larger. That is the main reason the default is
conservative today, and steady-state playback on a 4-core node transcoding at
roughly real time will sit at the frontier often, so holds are the normal case
rather than the exception — this cap will bind in ordinary use, and going
async is what lifts it.

### The refusal

- **Not `404`.** The resource is not absent — the playlist promises it exists
  — it is not ready. 404 invites an intermediary to cache it and some players
  treat it as terminal.
- `Retry-After: 1` and `Cache-Control: no-store`.
- A distinct error code (`segment_not_ready`) from the existing
  `stream_failed`, so a client can tell "come back" from "this generation is
  broken".

**Superseded during implementation, 2026-09-08.** This section originally said
the refusal was `503`. It is **`500 segment_not_ready`**, and `stream_failed`
keeps `503`. Two findings from the client sessions forced it, in order.

First, the code cannot be read where it matters. hls.js's `XhrLoader` surfaces
a failed fragment as `{code: xhr.status, text: xhr.statusText}` — the JSON body
is absent from the error event and `response.data` is `undefined`. A header is
no better: reachable only through `networkDetails`, the raw `XMLHttpRequest`,
which is undocumented coupling that breaks outright if the default loader ever
becomes `FetchLoader`. So the discriminator has to be the status code, which is
the one field every loader reports identically.

Second, given that, `503` is the wrong status to carry "hold". Every proxy,
tunnel and load balancer emits `503` when a service is genuinely down. A client
taught that `503` means "not made yet, stay on this node" reads a dead node as
a healthy one and never fails over — silent, not self-correcting, and hardest
to diagnose precisely where an intermediary makes it most likely. The inverse
error, misreading an infrastructure `500` as a hold, costs one pointless retry.
The faults are not symmetric. `500` is origin-generated in practice, so it is
the status nothing else on the path emits.

Checked rather than assumed: nothing currently fronts the nodes — all three
serve `:7438` directly — but haproxy is installed and running on es-1, the
WAN-facing node, with a stock config and no bound frontends. The hazard is one
configuration change away rather than hypothetical.

Both statuses stay in 5xx deliberately: hls.js's `retryForHttpStatus()` returns
false for 4xx and status 0, so a 4xx would stop its retries outright. And
`Retry-After` is inert on the fragment path — hls.js reads that header only in
its content-steering loader, on 429 — so it is sent because it is correct HTTP,
not because the design depends on it.

## Client contract change

The four client sessions (UI, `@macha/core` NPM package, React Native, Site)
must expect:

- the media playlist arrives complete with `ENDLIST` on first fetch and does
  not change afterwards — no polling required or useful;
- a fragment or init request may answer `503 segment_not_ready` with
  `Retry-After`, meaning "not yet, retry", and this must **not** be weighed as
  node health or count toward failover;
- `503 stream_failed` continues to mean the generation is broken.

Ask the UI session to confirm hls.js's actual retry policy for a 5xx on a
fragment in their installed version, rather than relying on the documented
default.

## Phases

- [x] **1. One hold path.** `wait_object` waits for init as well as segment
  indices. Test: an init request issued before the first `moof` is held and
  then served, not 404'd.
- [x] **2. Complete playlist.** `PLAYLIST-TYPE:VOD`, all planned entries,
  `ENDLIST`, served with no readiness gate; remove the `wait_ready` hold in the
  playlist route. Tests: a playlist fetched before any fragment exists is
  complete and closed; it is byte-identical on a later fetch.
- [x] **3. Admission policy and budget.** Window, per-session limit, global
  budget, as an explicitly acquired and released token. Refusals do not
  advance `highest_requested`. Tests: within-window request holds and is
  served; beyond-window refuses immediately; a session at its hold limit
  refuses without waiting; budget exhaustion refuses without waiting; a
  released hold makes budget available again.
- [x] **4. Configuration.** `streaming.segment_timeout_ms`, new at **8000**.
  New: `segment_hold_window`, `max_session_holds`, `max_concurrent_holds`.
  Documented in `macha.yaml.example` and `docs/streaming.md`.

  Two corrections to what this phase originally said. There was no existing
  `segment_timeout_ms` to change "15000 -> 12000"; the 15000 is
  `startup_timeout_ms`, a different setting, left alone. And the 12000 was
  chosen against hls.js's `fragLoadingTimeOut` of 20 s, which the UI session
  then established is **deprecated and inert** -- the compatibility shim
  migrates it only when it is set in user config, which the client does not
  do. The deadline that actually governs is
  `fragLoadPolicy.default.maxTimeToFirstByteMs`, 10 s, read out of hls.js
  1.6.18's source. A 12 s hold therefore answers *after* the client has
  already aborted: the request sends no bytes, trips the time-to-first-byte
  abort, never receives the 503, and takes the timeout path instead -- 4
  retries at 0 ms delay, each aborting again, so one held fragment becomes
  ~5 requests over ~50 s and then goes fatal. That is worse than the
  behaviour this plan replaces. 8000 fits under the real deadline with margin
  for the ~63 ms WAN round trip to es-1. A 503 that arrives promptly is
  retried sensibly (6 attempts, 1 s backing off to 8 s), so answering inside
  the deadline is the whole game. `Retry-After` is inert on hls.js's fragment
  path -- it is read only by the content-steering loader, on 429 -- so it is
  sent because it is correct HTTP, not because anything here depends on it.
- [x] **5. Rewrite the three EVENT regressions.** `test_media_playlist_waits_
  for_the_first_fragment`, `test_media_segment_store_backpressure_and_spill`
  and `test_segment_store_mpegts_mode_has_no_init_and_ts_names` currently
  assert the 0.32.14 contract. They are not deleted — they are re-expressed
  against the mechanism that now guards the same incident: a deeply
  prefetching client must not be able to occupy the node.
- [x] **6. Full suite plus `test_transcode_timeline.cpp`, then deploy all
  three nodes** (operator chose all-at-once; the cluster is test-only).
- [x] **7. Notify the four client sessions** of the contract change above.

## Exit criteria

- A playlist fetched before any fragment exists is complete, closed, and
  identical to every later fetch of the same generation.
- An init or segment request inside the window is held and served rather than
  refused; one outside it, or over either limit, is refused immediately with
  `503 segment_not_ready` and never occupies a worker.
- With every session holding its maximum, `GET /api/v1/status` still answers
  promptly. This is the governing-law-3 gate and the reason the budget exists;
  it must be an actual test, not an assumption.
- No client-visible regression in time to first frame on a title whose source
  audio is not DTS or TrueHD.

## Deferred, deliberately

- **LL-HLS parts (`EXT-X-PART`).** "Wait then send" upgrades to "wait then
  stream" only for MPEG-TS, which is a byte stream. An fMP4 fragment is a
  `moof`+`mdat` pair whose `moof` carries the fragment's sample table and
  cannot be written until the fragment is complete, so there is no meaningful
  prefix to send early. The standard answer is smaller independently
  deliverable parts, which is a feature rather than a refactor. The seam is
  fine — `HttpBodySource` already exists — but do not plan on partial delivery
  of a single fMP4 fragment.
- **Async `HttpServer`.** See the note above; wanted, not now.
- **The growing EVENT playlist**, wanted eventually, per the operator.

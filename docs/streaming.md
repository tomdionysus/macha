# Streaming

The playback API is instruction-based. **The server reports what a file is and performs what it is asked for; it does not choose.** A client reads the media facts, decides what to do with them against its own decoder, and instructs. Media probing, remuxing and transcoding run in-process through the FFmpeg libraries behind `MediaEngine`; no `ffmpeg` or `ffprobe` subprocess is launched.

There is no `auto` mode. `preferences.mode` is required on every session, and a missing or unrecognised mode is `400 bad_playback_request`.

## Modes

1. **direct** — the original file over HTTP byte ranges, exactly as stored: no container change and no re-encode;
2. **remux** — every elementary stream copied into an HLS container;
3. **transcode** — at least one stream re-encoded (H.264 video, AAC audio) into an HLS container. An AAC encode keeps the source's channel layout: a codec change is not a downmix.

The per-stream instructions name what happens to each stream:

| field | values | meaning |
|---|---|---|
| `preferences.mode` | `direct`, `remux`, `transcode` | required |
| `preferences.video` | `copy`, `transcode` | what happens to the video stream |
| `preferences.audio` | `copy`, `transcode` | what happens to the audio stream |
| `preferences.container` | `fmp4`, `mpegts` | HLS segment container; required for `remux` and `transcode` |
| `preferences.video_stream` | stream index | which video stream |
| `preferences.audio_stream` | stream index | which audio stream |
| `preferences.audio_language` | language code | the audio stream in that language, when exactly one has it |
| `preferences.subtitle_stream` | stream index | which subtitle stream, if any |
| `preferences.subtitle_language` | language code | the subtitle stream in that language, when exactly one has it |

**The server chooses nothing** (operator, 2026-09-24: "The server supplies facts, operations, then does what it's told"). Where a choice has exactly one candidate it is a fact, and it is used: a file with one audio stream plays that stream without being named. Where there are several and the instruction does not name one, or where it names something the media does not have, the session is refused with the candidates, so the client can choose:

```json
{
  "status": "choice_required",
  "error": {
    "code": "choice_required",
    "message": "this media has 2 audio streams: name one with preferences.audio_stream",
    "choice": "audio_stream",
    "choices": [1, 4],
    "scope": "request",
    "node_healthy": true,
    "alternative_may_succeed": false
  }
}
```

`code` is `choice_required` (several candidates, none named, or a language several share) or `choice_not_available` (the index or language names nothing this media has; `choices` lists what it does have). `choice` is `video_stream`, `audio_stream`, `subtitle_stream` or `container`; `choices` holds stream indexes, or the two container names. Both are `400`. A language the media lacks is never answered with a different track: until 0.58.0 it silently fell back to the default one. `direct` is the exception that proves the rule: the file is served untouched and the player picks its own tracks, so an unnamed stream is not refused there, and `output` then describes no selected stream.

**The mode has to describe what is being done.** `direct` and `remux` copy every stream; `transcode` re-encodes at least one and may copy the other. `transcode` is the permissive mode, and it is how a mixture is asked for.

| mode | video | audio | |
|---|---|---|---|
| `direct` | `copy` | `copy` | the source file, untouched |
| `remux` | `copy` | `copy` | repackaged, every stream copied |
| `transcode` | `transcode` | `transcode` | both re-encoded |
| `transcode` | `copy` | `transcode` | audio only re-encoded |
| `transcode` | `transcode` | `copy` | video only re-encoded |

So "copy the video, re-encode the audio" is `{"mode": "transcode", "video": "copy"}`. Omitting `video`/`audio` takes the mode's own default: copy for `direct` and `remux`, re-encode for `transcode`.

**What the server refuses.** Only what is impossible or what misdescribes itself, never what a client said it could not play:

- a missing or unknown `mode`, or an unknown `video`/`audio`/`container` value;
- `direct` with a stream set to `transcode`, or with any `max_height`/`max_bitrate`;
- `remux` with a stream set to `transcode`, or with a `max_bitrate` or a `max_height` below the source height — a quality change is a re-encode, and remux copies;
- `transcode` with both streams copied — nothing is being re-encoded, so it is a remux or a direct;
- a copy into a container that cannot carry that codec (fragmented MP4 carries H.264, HEVC and AV1 video, and AAC, AC-3, E-AC-3 and Opus audio);
- a transcode when the node has no encoder for the target.

Every refusal in that list is `400 bad_playback_request` with `scope: request` and `alternative_may_succeed: false`, except the container one: that is `422 copy_not_supported` with `scope: node` and `alternative_may_succeed: true`, because the request is coherent, another build may carry the codec, and a transcode of that stream would succeed here. A refusal names the rule. The server does not quietly reinterpret a mode into the one that would have worked, because a session that reports a mode it is not performing misleads everything downstream of it.

The session reports the container it actually served in `output.container`: `fmp4` or `mpegts` for an HLS session, and the source's own container for a `direct` one. A request is not evidence of what was performed, so read it there. The `direct` vocabulary is the same one the facts endpoint uses for a source container, so the two are comparable.

**What the server does not do.** It does not ask what the client can play, and there is no `capabilities` field. Whether a device can decode what it asked for is the client's business; the server reports the facts and carries out the instruction. A client that asks for `direct` on a file it cannot demux gets the file.

## Media facts

Facts come from one probe of the file, are persisted in the catalogue media profile, and are reported identically wherever they appear:

| field | notes |
|---|---|
| `container` | the resolved container family: `matroska`, `webm`, `mp4`, `avi`, `asf`, `mpeg`, `mpegts`, `mp3`, `flac`, `ogg`, `adts`, `wav`, `aiff`. **Match on this**, not on `format`. A format the server does not know is reported under libavformat's own name for it, so the field is never empty for a source that probed. |
| `format` | the raw libavformat demuxer name list, e.g. `matroska,webm`. It names every container that demuxer handles, so a Matroska file lists `webm` too; matching it directly is how a WebM-capable device ends up being handed a Matroska file. |
| per stream | `index`, `type`, `codec`, `profile`, `language`, `width`, `height`, `channels`, `sample_rate`, `bit_depth`, `level`, `color_transfer`, `dolby_vision_profile`, `dolby_vision_compatibility`, `bitrate`, `default`, `forced`, `attached_picture` |

`bit_depth` is derived from the pixel format when the container does not carry it. `color_transfer` is the transfer function name (`smpte2084` for PQ, `arib-std-b67` for HLG, absent for SDR or unread). The Dolby Vision fields appear only on a stream carrying a configuration record; compatibility `1` is HDR10-compatible, `2` is SDR, `4` is HLG.

These facts are available **before** a session exists, which is what makes an instruction possible:

```text
GET /api/v1/playback/media?media_id=<macha: or path: identity>
GET /api/v1/playback/media?item_id=<catalogue item>
```

It returns, per media, the identity, path, size, `container`, `format`, `duration_ms`, `bitrate`, the full stream list, and an `operations` object describing what this node can do with that file: `direct` (always true) and `transcode_video` / `transcode_audio` reflecting the encoders present in this build. Whether a stream can be copied into a container is a fact about that stream, so every video and audio stream carries its own `copy_into` object with `fmp4` and `mpegts` booleans. Until 0.58.0 `operations` carried `copy_into_fmp4` and `copy_into_mpegts` for the first video and audio stream only, which answered for whichever track happened to be first. No session is created and no pipeline starts. Each file gets the whole probe allowance; one slow file no longer pushes the rest into `unavailable`.

**This is how a title is played.** A catalogue item is a title; its `media_ids` are its files, and each file has its own facts. The client reads them with `?item_id=`, chooses the file, the mode, the streams and the container, and creates the session with that file's `media_id`.

The two containers do not carry the same codecs. MPEG-TS predates the fMP4 arrangement and is where the carriage of these codecs was first defined, so it takes MPEG-2 video and MP3/MP2 audio that fMP4 will not, while fMP4 takes AV1 and Opus that TS will not. Both carry H.264, HEVC, AAC, AC-3 and E-AC-3. A player that refuses a codec in one container may accept the same bytes in the other, which is a client's decision to make from these two facts.

`operations` is a fact, not a decision. It is answered by the node that received the request, from that node's build, about that one file, so it is per node and per source and must never be cached as a property of the cluster. Nodes on different builds legitimately give different answers, and the node the client will actually stream from is the only one whose answer matters.

The response is `{"media": [...]}`, plus `item_id` when one was asked for. When one media of an item cannot be read, the others are still reported and the failures are listed separately in `unavailable`, each with its `media_id`, a `reason` (one of the three below, or `not_found` when the identity did not resolve here) and a `message`. When none can be reported the whole request fails: `422 facts_unavailable` with the last `reason` in `error.reason`, or `404 not_found` when nothing resolved.

## Why a request could not be answered

Failures carry a `reason` so the client can tell situations apart that would otherwise look identical. The server states it and does nothing with it; deciding whether to ask another node, transcode from elsewhere, or stop, is the client's.

| `reason` | meaning | what it implies |
|---|---|---|
| `source_unreadable` | this node could not read the file's bytes | the file may be perfectly good elsewhere: another node is worth asking |
| `source_unsupported` | the bytes were read and are not media this build can demux | no node running this build will do better |
| `source_read_timed_out` | reading did not finish inside the deadline | transient; a retry may succeed |

On session creation and update `source_unsupported` is reported as `422` and the other two as `503`, each as a `playback_probe_failed` or `playback_pipeline_start_failed` error carrying the `reason`. The facts endpoint answers all three as `422 facts_unavailable` and the catalogue media profile as `422 profile_failed`, each with the `reason`. Whether a failure is worth retrying is a client judgement, not a server one.

The same stream facts are on the catalogue media profile (`GET /api/v1/catalogue/media/<macha id>/profile`, `schema_version` 3) for any media with an immutable identity. The profile carries `format` but not `container` or `operations`, and states every stream field, with `0` or `""` where a fact is absent, rather than omitting it. That pre-session availability is intentional and guaranteed for `macha:` identities: with no stored profile the endpoint produces one there and then, at foreground priority, and persists it. It answers `404` only when the media is not on this node, and `422` with a `reason` when the file could not be probed. The reading order is: the persisted profile if one exists, otherwise a foreground probe whose result is persisted for later readers; background profiling of unwatched media runs at a lower priority and yields to a viewer.

An older stored profile that predates a fact (a schema below the current one, on a video stream) is treated as stale and regenerated on that media's next playback.

## Media engine boundary

FFmpeg streams marked `AV_DISPOSITION_ATTACHED_PIC` are metadata images, not playable video. Macha preserves that disposition in probe results, reports the stream as non-playable/other media, and excludes it from video selection. This prevents an MP3 with embedded album art from being promoted into an HLS/fMP4 video pipeline.

`PlaybackManager` owns policy and `MediaEngine` owns media operations. The interface contains media concepts (`probe`, `PlaybackPlan`, `MediaEngineSession`, fragments and subtitle extraction), not FFmpeg options. The current implementation is `LibavMediaEngine`, but another implementation can replace it without changing the HTTP/session API.

Each media source is an immutable seekable `MediaInput`. The libav input `AVIOContext` maps read/seek callbacks directly onto the pinned Macha `ReadHandle`. There is no loopback HTTP hop and no FUSE dependency. Replacing or renaming a pathname after playback starts does not silently change the extents underneath that session.

Probe and subtitle readers are deliberately *not* registered with `PlaybackTracker`; only the actual direct/transformed playback reader drives read-ahead/current-file/catalogue hydration. Stable `macha:` media IDs are indexed by metadata generation, so selecting among catalogue representations does not rescan and re-hash the whole namespace on every session creation.

Transformed output uses the MP4 muxer with fragmented-MP4 flags and a custom output `AVIOContext`. Top-level MP4 init and media fragments are published directly into `MediaSegmentStore`. The HTTP server reads playlists/fragments from that store. No temp directory is polled for readiness.

A transformed session is served as a master playlist (`master.m3u8`: one `EXT-X-STREAM-INF` with `BANDWIDTH`, `CODECS` and `RESOLUTION`) pointing at a media playlist (`media.m3u8`). The media playlist is a complete `EXT-X-PLAYLIST-TYPE:VOD` list: every planned fragment, closed with `#EXT-X-ENDLIST`, served on the first fetch with no readiness gate and byte-identical on every later fetch of the same generation. The duration is known because the source was probed, so this is the spec-correct form; `EVENT` is for live-to-VOD recording where the duration genuinely is not yet known. With `ENDLIST` present a player stops polling entirely, so a generation costs one playlist fetch rather than hundreds.

`EXTINF` is the plan rather than the measured length, necessarily: an unproduced fragment has no measured length, and a VOD playlist must be immutable across fetches, so a produced fragment cannot be described differently from an unproduced one. That is only honest because the pipeline emits exactly one fragment per planned entry; `tests/test_transcode_timeline.cpp` gates it by measuring every declared duration against the media the fragment really carries.

Because the playlist promises fragments that do not exist yet, a request for one is held rather than refused — but only as an explicitly admitted resource, under three independent tests applied in order. A request beyond `segment_hold_window` fragments of the produced frontier is refused: nothing is working toward it. A session already holding `max_session_holds` requests is refused, which is what stops a deeply prefetching player queueing thirty requests against the encoder. A node already holding `max_concurrent_holds` requests is refused. Any of those is an immediate answer, never a wait, and a refused request does not raise the producer demand watermark — noting an index the server declined to serve would authorise production to run toward it. An admitted hold waits up to `segment_timeout_ms`.

A refusal is `500` with error code `segment_not_ready`, `Retry-After: 1` and `Cache-Control: no-store` — never `404`. `error.reason` says which test refused it: `beyond_hold_window`, `session_hold_limit`, `hold_budget_exhausted`, or `hold_timed_out` for an admitted hold whose `segment_timeout_ms` ran out. The resource is not absent, since the playlist promises it exists; it is not ready, and a `404` invites an intermediary to cache the miss while some players treat it as terminal. A broken generation is `503 stream_failed`.

The assignment is deliberately the inverse of what the HTTP spec suggests, and a client must not "correct" it. Many players expose only the status code on a fragment error — not the body, not the headers — so the status has to carry the meaning on its own. `503` cannot: every proxy and load balancer emits it when a service is down, so a client taught that `503` means "hold, stay here" would read a dead node as a healthy one and never fail over. Misreading an infrastructure `500` as a hold costs one wasted retry instead. Both stay 5xx because a 4xx stops most players retrying at all. `init.mp4` takes the same hold path as a fragment.

`segment_timeout_ms` must stay below the client's time-to-first-byte deadline. A held request sends no bytes, so a client that gives up first never sees the refusal and takes its timeout path, which retries hard and then fails — worse than not holding at all. The tightest deadline among the clients in use is 8 s, so the 6000 default clears it with margin and **8000 is a hard ceiling on this knob**. Read the deadline out of the client artifact before changing it: the published documentation for these clients has been wrong about which setting governs.

A held request costs no thread. The handler asks the segment store for the object and, in the same locked step, subscribes to the next publication if it is absent; it then hands the server a deferral -- what it is waiting for, its deadline, and the admitted hold -- and returns. The server parks the connection and re-runs the handler when the store publishes or the deadline passes. `max_concurrent_holds` (64) is therefore a fairness and memory bound rather than a worker-thread ration: it bounds how many requests may be waiting on encoders at once across every session. Steady-state playback on a four-core node transcoding at roughly real time sits at the frontier often, so holds are the ordinary case rather than the exception.

For stream-copy video, the VOD planner uses the demuxer's keyframe index and chooses random-access boundaries near `segment_duration_ms`. Matroska/WebM Cues are explicitly materialised through the demuxer's seek path before that index is inspected, because probing alone may expose only a partial early-file index. The resulting plan is rejected if any advertised fragment would be grossly larger than the configured target, preventing a partial index from turning the unindexed remainder of a movie into one fragment. A transformed seek starts at the last indexed keyframe at or *before* the requested position, so the first advertised segment is independently decodable and the generation still contains the position that was asked for. The remainder is reported as `seek_offset_ms` rather than added silently to the request (see "Where a seek actually starts" below). A remux whose keyframe index is unusable fails rather than silently changing mode. A fragment is as long as the source GOP makes it, up to 90 seconds; only a gap or tail beyond that rejects the plan, because scene-cut encodes routinely exceed a fixed multiple of the target. Transcoded video uses the encoder GOP cadence as its VOD boundary plan.

Stream-copy timestamps are normalised only after rescaling into the MP4 stream's final muxer timebase. Missing PTS/DTS are synthesised conservatively and equal/backwards DTS values are advanced with a persistent per-stream timeline correction. Legitimate PTS-before-DTS composition offsets are preserved rather than clamped; fragmented MP4 is emitted with signed composition-time offsets enabled. Repairs that actually modify timestamps are logged with per-stream counters.

`stream.look_ahead_ms` is how far past its last requested fragment a viewer may arrive and still find media already produced: `max_ahead_segments` x `segment_duration_ms`, and `null` for direct play, which has no pipeline and so no frontier.

**Read it per session; do not hardcode it.** It is serialised from the live configuration, and both knobs apply on a `reload_config` without restarting playback, so it can change for a session already in flight with no `PATCH` to announce it. A client that assumed the defaults against a node configured with a shorter window would silently under-run.

Production is sequential, so arriving beyond the look-ahead does not skip the intervening fragments: the node encodes its way there at roughly real time while the viewer waits. Where the gap exceeds the look-ahead, creating a new generation seeked to the arrival point is cheaper than making the current one catch up.

### How fast this generation is producing

`stream.production` carries what a client needs to answer "if I join at position P, can this node reach it before the viewer does". It is absent for direct play, which has no pipeline.

| Field | Meaning |
| --- | --- |
| `produced_ms` | Media produced by this generation so far -- the production frontier, in media time |
| `producing_ms` | Encoder time spent producing it, with parked intervals excluded |
| `produced_age_ms` | How long ago the last fragment was published |
| `producer_parked` | Whether the producer is blocked on the look-ahead gate |

The rate is `produced_ms / producing_ms`, and the wait for a join at `P` is `(P - produced_ms) / (rate - 1)`. The pair is raw on purpose: a rate computed on the node is a rate with the node's smoothing and the node's window baked in, and a client deciding whether to hand over needs to choose those itself. One response answers it -- there is nothing to poll, and nothing added to a viewer's critical path.

**`producing_ms` is not wall clock, and must not be replaced by it.** The producer runs to `max_ahead_segments` beyond demand and then blocks, so a viewer watching at normal speed keeps it parked for most of the generation's life. Wall clock would therefore report about 1.0x however fast the encoder is -- and 1.0x is read as "cannot outrun realtime", which defers a handover that would have worked. `producing_ms` accumulates only the intervals in which the encoder was actually running.

The two figures cover the same fragments, including the first, so pipeline start-up is charged to the rate. That reads low early and settles as the generation runs. The bias is deliberately in the conservative direction: understating costs a handover that is deferred, overstating costs a viewer stalled on a promise the node could not keep.

`producing_ms` is `0` until the first fragment lands. **A client must read that as "no reading yet", not as an infinite rate.**

`produced_age_ms` is an age measured on the node rather than a timestamp, so it does not depend on the client's clock agreeing with ours. Read it with `producer_parked`: a large age means two opposite things -- a pipeline that has wedged, or one that is comfortably ahead and waiting for this viewer -- and only the flag distinguishes them.

The segment store remains a bounded producer/consumer queue. Once the producer is `max_ahead_segments` beyond actual client demand it blocks on a condition variable and resumes when later fragment indexes are requested. This prevents a fast remux from pulling an entire movie through the DHT while keeping VOD playlist semantics independent of producer progress. Resident generated fragments are bounded by `segment_memory_bytes`; old consumed fragments can spill below `temp_path`.

Video scaling and audio resampling are initialised from actual decoded-frame properties rather than assuming the decoder knows the final pixel/sample format at open time. This matters for containers/codecs whose format details are discovered only during decoding.

## Startup and diagnostics

Session creation has explicit server-side stages. INFO/DEBUG logs use a short trace token, for example:

```text
playback[a1b2c3d4] session create start
playback[a1b2c3d4] probe start ...
playback[a1b2c3d4] probe complete ... elapsed_ms=...
playback[a1b2c3d4] pipeline start ...
playback[a1b2c3d4] first fragment ready elapsed_ms=...
playback[a1b2c3d4] session create complete ... elapsed_ms=...
```

`probe_timeout_ms` places one wall-clock bound on the complete representation-selection pass; candidate media IDs share the remaining budget. The same deadline is propagated through `ReadHandle` into remote DHT object fetches, where an outstanding media RPC is cancelled when it expires. `startup_timeout_ms` independently bounds the wait for the first transformed fragment. A 503 therefore identifies whether inspection or transformed-output startup failed instead of presenting as one long opaque request.

Playback/probe/seek object reads are foreground traffic. Foreground demand is
recorded before storage access begins and lower-priority DATA RPC execution
retains reserved worker capacity for it. Asynchronous FUSE publication is loader
traffic: under simultaneous demand the configurable default viewer/loader share
is 95:5, so publication continues without being allowed to consume the
execution/storage service needed to start or seek a stream. Either class borrows
unused DATA capacity work-conservingly when the other is idle.

This is [governing law 2](principles-and-laws.md#scheduling-laws) in the playback
path: no other class of work may be the reason a viewer waits.

### The two places a viewer does wait

Law 2 forbids another class of work making a viewer wait. It does not promise
that a viewer never blocks on its own stream being produced, and there are
exactly two places where it does. Both are bounded, both are published to the
client, and neither is precedent for a third.

**Arriving beyond the produced frontier.** Production is sequential, so a client
that requests a fragment further ahead than the node has produced waits while
the node encodes its way there at roughly real time. The bound is
`stream.look_ahead_ms` on the session payload, which is why that field exists
and why a client must read it per session rather than hardcode it. Past the
window, creating a new generation seeked to the arrival point is cheaper than
making the existing one catch up.

**A held segment request.** A complete VOD playlist promises fragments that do
not exist yet, so a request for one inside the hold window is held rather than
refused. The bound is `segment_timeout_ms`, published per node on
`GET /api/v1/status`.

In both cases the wait is on production that this viewer itself demanded, which
is the distinction that matters: the work in front of it is its own. A change
that makes a viewer wait on anything else — a publication, a repair, a scrub, a
catalogue scan — is a law-2 violation however favourable its throughput numbers
are.

Publication traffic has its own loader transport class below viewer foreground
and read-ahead but above speculative maintenance. Restarting Macha does not
demote user-requested spool data: journal recovery is provenance for validation
and crash safety, while the resumed publication remains loader work.

Seek-only updates are determined by effective session policy rather than JSON shape. A client may resend its current `preferences` object together with `seek_ms`; when those preferences are unchanged Macha reuses the active immutable media probe and reusable VOD/random-access plan instead of performing a fresh container-planning pass.

## Public HTTP behaviour

The HTTP server is one reactor thread that owns every socket and never waits on anything but `poll()`, plus two bounded compute pools -- a control lane for health, status, session and account routes, and a data lane for catalogue, playback and web assets, so control traffic never queues behind fragment traffic. Response bodies may be in-memory or streaming sources. A resident body (a transcoded fragment) is sent straight from memory; any other body is read a chunk at a time on the data lane into a small per-connection staging window that the reactor drains as the client's TCP window allows, so a slow viewer costs an fd and two chunks rather than a thread, and the number of simultaneous fragment sends is bounded by memory and bandwidth rather than by a worker count.

Connections are HTTP/1.1 keep-alive by default, reused for up to `keep_alive_max_requests` and closed after `keep_alive_idle_timeout_ms` of silence; an idle connection costs nothing but its descriptor. Every response carries `X-Robots-Tag: noindex, nofollow`. `GET /api/v1/status/diagnostics` reports the server under `http`: reactor passes and stalls, open, idle, writing and deferred connections, staged bytes, and each lane's workers, queue depth and longest queue wait. `reactor_stalls` counting up means something on the reactor slept, which nothing may.

Session creation and control require the session bearer token from `POST /api/v1/session`. Returned stream URLs use a separate high-entropy capability in the path so native players can fetch direct files, playlists and fragments without the bearer token. Stream capabilities expire with the playback session.

Every JSON object response carries a top-level snake_case `status` (0.56.0): `"ok"` on success, unless the handler states its own. An error's `status` is its `error.code`, except the two playback errors built outside the common envelope — `account_session_limit` and the `playback_*_failed` stage errors — whose `status` is `"error"`. Branch on `error.code`, which is present on every error. Stream bodies, `204` and non-JSON responses carry no `status`.

## Status

```text
GET /api/v1/playback/status
```

Reports enablement, current session/transcode counts, media-engine backend/version and encoder availability. Legacy executable-discovery status fields are reported as false/empty; the active backend/version and encoder capabilities are the authoritative status fields.

### The budgets a node enforces, and where a client reads them

A client must bound its own attempt on a node against the budgets that node enforces, and it needs them for every node it might fail over to, not only the one it is playing from. Those two facts decide where they are published: on the **per-node entries of `GET /api/v1/status`**, beside `runtime`, in a `playback` object.

```json
"playback": {
  "startup_timeout_ms": 15000,
  "segment_timeout_ms": 6000,
  "pipeline_idle_ms": 60000,
  "session_idle_ms": 1800000,
  "max_sessions_per_account": 32,
  "max_sessions": 64,
  "transcode_entitlement_idle_ms": 300000
}
```

- **`startup_timeout_ms`** — how long this node may take to bring a transformed generation's first fragment up (`streaming.startup_timeout_ms`).
- **`segment_timeout_ms`** — how long it holds a request for a fragment that is not ready yet (`streaming.segment_timeout_ms`).
- **`pipeline_idle_ms`** — how long a physical remux/transcode pipeline survives without valid current-generation traffic (`streaming.pipeline_idle_ms`).
- **`session_idle_ms`** — how long a logical session survives without control or valid stream activity (`streaming.session_idle_ms`). This is also how long a session abandoned on an unreachable node keeps its slot.
- **`max_sessions_per_account`** — the per-account cap, described under [what one account may hold](#what-one-account-may-hold-on-one-node). The limit only; the live count is never here.
- **`max_sessions`** — the node-wide session cap, every account together. Its refusal is `resource_limit` and means something different from the one above: this node is full, rather than this account is. Added in 0.49.0.
- **`transcode_entitlement_idle_ms`** — how long a session may hold a transcode entitlement with no stream activity before the node releases it. Added in 0.49.0; see [keeping a transcode slot across a pause](#keeping-a-transcode-slot-across-a-pause).

`pipeline_idle_ms`, `session_idle_ms` and `max_sessions_per_account` arrived in 0.48.0. Clients had been holding private copies of the first two, hardcoded against this node's defaults, which is exactly the failure `look_ahead_ms` was added to stop.

These are each node's statement about **itself**, relayed like `load1` and `cpu_cores`. No node computes or reports a cluster-wide figure: it has no data to do so, since telemetry carries no peer's streaming configuration. A client that needs a worst case across the nodes it might use composes it from these, because only the client knows which nodes those are.

They are **not** on the session payload, unlike `stream.look_ahead_ms`. That field is needed during playback, once a session exists; these bound the request that creates the session, so a client cannot learn them from the response it is timing out on — and a node it has never used would never report them at all.

**Absence means the node cannot say**, never a default: an older node predating the field, or one with `streaming.enabled` false, omits them rather than reporting zero. A client must fall back to its own conservative bound and must never shorten a budget on the strength of a missing field, nor substitute another node's figure, which is a fact about that node.

The two directions of error are not symmetric. A client budget longer than the node's merely waits longer than necessary. A budget shorter than it abandons the node inside its own entitlement, discards a transcode that was about to succeed, and starts the identical encode elsewhere — manufacturing a viewer-visible failure out of a node that was working. Read the figure rather than guessing it, and err long.

Every value here applies on a live `reload_config` without a restart, so a client should refresh rather than cache once, and treat a cached figure as a floor rather than a settled fact.

## Create a session

```text
POST /api/v1/playback/sessions?idempotency_key=<opaque key>
Content-Type: application/json
Authorization: Bearer <session token>
```

`media_id` is required, and playback is by `media_id` only: a title is not playable as such, its files are, and choosing one is the client's decision. A request naming `item_id` is refused with `400 item_id_not_accepted`, and one naming no `media_id` with `400 media_id_required`; the same holds for `PATCH`, where a `media_id` switches the file being served. Until 0.58.0 an `item_id` alone made the server rank the item's files (direct over remux over transcode, then list order) and play the winner. `path:/logical/file` is also accepted as a media identity, and a file no title references is playable by its `media_id` like any other.

**A playback session is a resource, not a property of the bearer.** A `POST`
to the collection creates a member, every time. Two `POST`s on one bearer
token yield two live sessions with different ids, both streaming, neither
disturbing the other. The response is `201` with a `Location` header naming
the new session.

This changed in 0.48.0 and it breaks what came before. Until then the server
keyed a playback session on the authenticated API session and a token had at
most one, so a second `POST` silently superseded whatever that token was
already playing. A client that needed two concurrent generations needed two
API sessions. Both of those facts are gone: hold as many as you need, within
the per-account cap below.

**The server mints the id.** There is no request field that proposes one, no
viewer-session header, and no `viewer_session_id` body field — `idempotency_key`
is a request token, not an identifier. A client that loses an id recovers it
from the collection listing rather than reconstructing it.

**Idempotency is a separate mechanism** and is a **query parameter**, not a
header:

```text
POST /api/v1/playback/sessions?idempotency_key=<opaque key>
```

Reuse one key for one logical creation attempt — after a timeout, disconnect
or failover — and the same key with the same request replays the same session,
capability and `generation` rather than creating another. The same key with a
*different* request returns `409 idempotency_conflict`. A retry that arrives
while the first creation is still running waits for it, and answers
`503 idempotency_in_progress` if it does not finish within the probe and
startup budgets; a key whose session has since ended answers
`409 idempotency_expired`. The key is 1 to 256
visible ASCII characters; anything else is `400 bad_idempotency_key`. The
fingerprint includes the bearer token, so a key cannot replay across API
sessions. Keys are scoped per account, so one account cannot occupy another's
key and turn its legitimate retry into a conflict. The response reports
`X-Macha-Idempotency: created|replayed`.

Omitting the key makes each `POST` a distinct creation, which now means a
distinct session. Idempotency prevents a *duplicate* session on a retry; it is
not what lets you hold two, and it is not needed for that.

### What one account may hold on one node

```json
"account": { "sessions": 3, "max_sessions": 32 }
```

Creation and the collection listing both carry this block: what this account
holds on this node right now, and what it may hold. Over the cap, creation is
refused with `429` and code **`account_session_limit`**, stating both numbers
as `error.sessions` and `error.max_sessions`, with `scope: request`,
`node_healthy: true` and `alternative_may_succeed: false`.

**Those two fields matter as much as the status.** An account-scoped refusal
is identical on every node, so a client must not walk the cluster looking for
one that will accept — and must not charge the refusing node's health for it.
That is the difference between this and the node-wide `max_sessions`, whose
`429` is node-scoped and *is* worth taking elsewhere.

The count is deliberately absent from `GET /api/v1/status`, which clients
cache. It is the most perishable number this API carries — it moves whenever
anyone on the account starts or stops anything, on a device neither end can
see — so it appears only where it is computed live: on creation, on the
listing, and on the refusal. The **limit** is on `/api/v1/status` for every
node, because a client planning a failover needs it about nodes it has not
talked to yet.

Budget for it honestly. A coordinator-driven client holds a live session plus
a standby per viewer and transiently three during a failover; a client that
adopts a session through the listing holds two by design; and an abandoned
session cannot always be deleted, because the `DELETE`'s target is often the
node that just became unreachable — that session holds its slot until
`session_idle_ms` expires it.

Session admission first reads the immutable media profile from cluster metadata,
which requires no media-object reads. A miss never produces a client-visible
profiling gate: Macha continues normal media negotiation using its configured
media engine. Concurrent inspection of the same immutable file is coalesced,
and a viewer takes over an already-running speculative scan instead of waiting
behind background work. The successful result is published asynchronously for
later sessions. Clients should still reuse the same idempotency key after a
timeout or disconnect.

### Keeping a transcode slot across a pause

**A paused session loses its transcode entitlement after
`transcode_entitlement_idle_ms` of no stream activity, and reacquires it on
resume — where it may be refused.** The session itself is untouched: its id,
position, plan and capability all survive to `session_idle_ms`. What a long
pause risks is the *slot*, not the place.

**To hold the slot, ask for a stream object inside that window.** Fetching the
playlist is enough and costs no media bytes — any request on the stream path
refreshes the session's stream activity. Polling the session with
`GET /api/v1/playback/sessions/{id}` does **not**: it keeps the session alive
but is deliberately not stream activity, because "has this session asked for
media recently" is the question the entitlement is answering.

**Read the interval off the node rather than hardcoding it.** It is
`playback.transcode_entitlement_idle_ms` on the per-node block of
`GET /api/v1/status`, published for the same reason as the other budgets: a
client that assumes a figure and meets a node configured differently gets the
failure it was trying to avoid. Absent means the node does not say, so keep a
conservative local bound and never lengthen one on a missing field.

**Why this exists.** The entitlement used to be held until the session was
erased, so it outlived its own pipeline by `session_idle_ms` — thirty minutes
against sixty seconds. On a node where `max_video_transcodes` is 1, one client
that crashed, was force-stopped or was reaped in the background closed that
node to transcoding for everybody for half an hour. A refusal after a long
pause is visible, attributable and recoverable; that outage was none of those.

**If refused on resume**, the `429` carries `scope: request` on the update
path — do not walk the cluster, the session is pinned to this node — with
`alternative_may_succeed: true`. A remux, or a lower `max_height`, will
usually start immediately.

### Generations, and what supersession does to a client

A **generation** is one produced stream for a session. A seek, a quality or
track change, and a media switch each end the current generation and begin a
new one. (A second `POST` no longer does: it makes a separate session.) The
generation number is in the stream path, so every URL a client holds belongs
to exactly one:

```text
/api/v1/playback/sessions/<session_id>/stream/<capability>/<generation>/segment-000042.m4s
```

**The stream is a subresource of the session it belongs to.** The capability
sits immediately before the part it authorises, and it stays in the path
rather than moving to a header because it is a capability, not a credential —
media players fetch segments without application headers. The top-level
`/api/v1/playback/stream/...` route was removed in 0.48.0.

When a generation is superseded its producing pipeline is stopped and its
segments stop resolving. **Every URL the client still holds for that
generation answers `410 generation_superseded`**, usually within about a
second, including requests already queued or in flight. The `session_id` and
the stream capability survive; only the generation changes.

So the effect on a client is: any read-ahead it had queued against the old
generation fails, and it must take the new `stream.url` from the response that
caused the supersession and resume from there. Content is not lost — the new
generation contains the position that was asked for — but the client's own
buffer of pending requests is invalidated and must be reissued.

**The three statuses on the stream path mean three different things, and a
client must not collapse them:**

- **`500 segment_not_ready`** — the fragment exists in this generation but is
  not produced yet. Wait and retry. Deliberately never a `404`.
- **`410 generation_superseded`** — the generation existed here and was
  replaced. Permanent. Stop retrying, re-read the session, use the new
  `stream.url`. The refusal carries `scope: request`, `node_healthy: true` and
  `alternative_may_succeed: true`: **this node is healthy and a different
  request against it will work.** Do not walk the cluster — no other node has
  this session, so a walk collects the same refusal from every node it tries
  and charges each one for it.
- **`404 not_found`** — nothing here ever produced that: a generation above
  the current one, an unknown session, or a bad capability.

`410` arrived in 0.48.0. Before it, a superseded generation and a segment that
never existed shared one `404`, which made every regenerate — and a regenerate
is routine — indistinguishable from a fault, and left a client reasonably
retrying something that would never come back.

Minimal request:

```json
{
  "media_id": "path:/Movies/Example.mkv"
}
```

A transformed session may also include `seek_ms` in the initial POST. This is the preferred resume path because the first HLS generation is created at the requested position instead of creating a generation at zero and immediately replacing it with PATCH:

```json
{
  "media_id": "path:/Movies/Example.mkv",
  "seek_ms": 5040000
}
```

An instruction, with the optional stream and quality fields:

```json
{
  "media_id": "path:/Movies/Example.mkv",
  "preferences": {
    "mode": "transcode",
    "container": "fmp4",
    "max_height": 720,
    "max_bitrate": 8000000,
    "audio_language": "eng"
  }
}
```

`max_height` and `max_bitrate` are instructions, not capabilities. `max_bitrate`, or a `max_height` below the source height, makes the video a re-encode, and combining it with `video: copy` is a `400`; a `max_height` at or above the source height changes nothing.

The response separates requested preferences, resolved playback, original source metadata and actual output metadata. The `/api/v1` schema is currently owned by Macha and Macha Client and may be changed in place while there are no third-party implementations. A representative response is:

```json
{
  "status": "ok",
  "session_id": "...",
  "generation": 1,
  "media_id": "macha:...",
  "mode": "transcode",
  "duration_ms": 5400000,
  "seek_ms": 0,
  "seek_offset_ms": 0,
  "seek_requested_ms": 0,
  "preferences": {
    "mode": "transcode",
    "video": "copy",
    "audio": null,
    "container": "fmp4",
    "max_height": null,
    "max_bitrate": null,
    "video_stream": null,
    "audio_stream": null,
    "subtitle_stream": null,
    "audio_language": "",
    "subtitle_language": ""
  },
  "selection": { "video_stream": 0, "audio_stream": 1, "subtitle_stream": -1 },
  "source": {
    "path": "/Movies/Example.mkv",
    "format": "matroska,webm",
    "size": 123456789,
    "bitrate": 8200000,
    "streams": [
      { "index": 0, "type": "video", "codec": "hevc", "profile": "Main", "width": 1920, "height": 1080, "bitrate": 7500000 },
      { "index": 1, "type": "audio", "codec": "eac3", "language": "eng", "channels": 6, "sample_rate": 48000, "bitrate": 640000 }
    ]
  },
  "output": {
    "format": "fmp4",
    "container": "fmp4",
    "video": { "source_stream": 0, "transform": "copy", "codec": "hevc", "width": 1920, "height": 1080, "profile": "Main", "bitrate": 7500000 },
    "audio": { "source_stream": 1, "transform": "transcode", "codec": "aac", "channels": 6, "sample_rate": 48000, "bitrate": 384000 }
  },
  "stream": {
    "url": "/api/v1/playback/sessions/<session_id>/stream/<capability>/<generation>/master.m3u8",
    "mime_type": "application/vnd.apple.mpegurl",
    "look_ahead_ms": 32000,
    "subtitle_url": null,
    "production": { "produced_ms": 48000, "producing_ms": 32000, "produced_age_ms": 120, "producer_parked": false }
  },
  "options": {
    "modes": ["direct", "remux", "transcode"],
    "quality_heights": [720, 480, 360],
    "audio_streams": [],
    "subtitle_streams": [],
    "can_seek": true,
    "can_change_quality": true
  },
  "trace_id": "a1b2c3d4",
  "account": { "sessions": 1, "max_sessions": 32 }
}
```

`preferences` echoes the instruction as given; the top-level `mode` is the mode being performed, which is always the mode asked for, since a mode that misdescribes itself is refused rather than reinterpreted. `trace_id` and `account` are on the creation response only; `GET` and `PATCH` return the session without them, and the listing carries `account` once beside `items`. `source.streams` describes the original elementary streams. `output.video`/`output.audio` describe the selected source stream, whether it is copied or transcoded, and the actual output codec/geometry/audio format. A CRF H.264 transcode has no fixed video bitrate and therefore omits `output.video.bitrate` unless an explicit target bitrate is in force. Returned stream URLs are relative to the API origin.

### Where a seek actually starts

The server does what it is told. It does not change the mode a client asked for, and it does not move the position a client asked for. Where a mode cannot begin a stream at the exact position requested, the response says so explicitly instead of relocating the request and reporting the relocation as though it were the answer.

Three flat fields on the session payload, present on create and on every `PATCH`, all milliseconds on the title's timeline:

- **`seek_ms`** — where the generation's media actually begins: the first sample the client receives. This is exactly what the field has always meant, so a client that reads only it is unaffected.
- **`seek_offset_ms`** — how far into that generation the requested position sits.
- **`seek_requested_ms`** — the position the server honoured, after clamping to `[0, duration - 1 ms]`.

The invariant, exactly, in integer milliseconds, with no tolerance and no rounding slack:

```text
seek_ms + seek_offset_ms == seek_requested_ms
```

`seek_offset_ms` is never negative, so a generation always contains the position asked for and nothing between the request and the stream start can go missing. `seek_requested_ms` exists because an exact invariant is only useful if a client can act on it being violated, and without it a client cannot distinguish a violation from an ordinary clamp near the end of a title; those want opposite handling.

Per mode:

- **Transcode.** `seek_ms` is exactly the requested position and `seek_offset_ms` is always `0`. The encoder can start on any frame, so it does. The decoder still seeks back to the preceding keyframe for pre-roll and discards decoded frames before the origin; on slow software decode that can add seconds to startup. That is the price of asking for a non-keyframe and it is the client's to pay.
- **Remux.** `seek_ms` is the last indexed keyframe at or before the request; `seek_offset_ms` is the remainder. A stream copy has no decoder and an fMP4 fragment's first sample must be a sync sample, so this is the only split the container permits. The client attaches at `seek_offset_ms` within the first fragment, so the pre-roll is fetched but never presented.
- **Direct.** `seek_ms` is the request and `seek_offset_ms` is `0`. There is no generation; the client byte-ranges the source.

The offset is therefore zero exactly when the mode can be frame-accurate. A client that wants a cheap, exactly-aligned seek asks for a position that is already a keyframe.

The mode is never substituted. A remux request stays remux, including when its keyframe situation is awkward: where the index names no keyframe at or before the request, the baseline is `0` and the offset carries the whole request. A decodable stream's first sample is necessarily a sync sample, so a copy can always begin at the beginning; the index simply did not name it. (Separately, a remux whose keyframe index is unusable *as a segment plan* still fails; it never falls back to transcode. That is remux being unplannable, not a seek being moved.)

The baseline is never a keyframe *after* the request. Aligning forward would leave the content between the request and that keyframe in no generation at all, unrecoverable by any client: a skipped scene on a viewer seek, and deleted content on the reaped-session recovery path, which rebuilds a generation at a position a viewer has actually reached.

## List, inspect, change and stop a session

```text
GET    /api/v1/playback/sessions          the caller's live sessions
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
```

**The collection `GET` is how a client finds a session it has lost the id
for**, and it is what makes handover between clients on one account possible:
it answers under `items`, like every other collection here, plus the `account`
block. It lists exactly the caller's own sessions on this node and nothing
else. There is no cluster-wide listing — a session is a resource of the node
producing it, and no node can enumerate another's.

**A session belongs to one account, and the control routes enforce it.** An
id belonging to another account answers **`404`, not `403`**, on `GET`,
`PATCH` and `DELETE`: whether an id exists on this node is not something one
account gets to learn about another. This matters more than it used to, since
the listing now hands ids out.

`DELETE` tears down that one session and leaves the caller's others running.
Delete sessions you have finished with: an undeleted one holds its slot
against the per-account cap until it idles out.

### Closing a session as the page goes away

```text
POST /api/v1/playback/sessions/{id}/stream/{token}/close
```

**New in 0.60.0.** The same teardown as `DELETE`, authorised by the session's
signed stream URL instead of a bearer: `{id}/stream/{token}` is the prefix of
the `stream.url` every session response carries. It takes no `Authorization`
header, no other custom header and no body (or a `text/plain` one), so it is a
CORS simple request and needs no preflight. That is the point of it: a browser
unloading a page does not complete a preflighted request, and a cross-origin
`DELETE` with a bearer is always preflighted, so a reload used to leave its
session -- and any transcode slot -- held until the idle rule. Send it with
`navigator.sendBeacon` or `fetch(..., {keepalive: true})` on page exit, and keep
using `DELETE` for every other close.

- `204` when the session was closed, **and** when it was already gone: the
  outcome the caller wanted.
- `404 not_found` when a live session is named with the wrong token, as on the
  stream routes.
- `405 method` for anything but `POST`.

It grants nothing the stream URL did not already grant: whoever holds that URL
can already read the stream.

`PATCH` keeps the same logical session ID and capability but may replace the underlying media-engine generation. A PATCH naming `mode` restates the whole transform: `video`, `audio`, `max_height` and `max_bitrate` are cleared unless that same PATCH restates them, so `{"mode":"direct"}` means direct rather than direct-plus-whatever-the-session-was-created-with. Name every field you mean in the same request; a PATCH that names both a mode and a per-stream transform sets both. A PATCH that changes only `subtitle_stream` and/or `subtitle_language` is special: it keeps the active A/V generation and stream URL unchanged and updates only the external segmented-WebVTT resource. Explicit stream indexes are validated; an invalid selection is rejected rather than falling back silently. `options.modes`, `options.quality_heights`, `options.audio_streams` and `options.subtitle_streams` are generated by negotiating each candidate against the current source/capabilities/preferences, so clients should render those arrays rather than inventing controls locally. Every PATCH response is the authoritative new session state.

Examples:

```json
{
  "preferences": {
    "audio_stream": 2,
    "subtitle_stream": 5
  }
}
```

```json
{
  "preferences": {
    "mode": "transcode",
    "max_height": 720,
    "max_bitrate": 4000000
  }
}
```

```json
{
  "seek_ms": 5040000
}
```

A seek or quality/track/media change creates a new internal generation. Old generation URLs stop resolving while the session ID/capability remain stable. `media_id` may also be supplied to switch representation explicitly.

Subtitles are off by default. Select one by stream index or language. `stream.subtitle_url` points to a small Macha subtitle manifest (`format: macha-webvtt-segments`) whose `segment_durations_ms` follows the active playback timeline. Individual `segment-N.vtt` resources are extracted lazily and cached only when requested, so selecting subtitles never requires scanning the complete media file before the first cue can appear. For transformed playback the subtitle segment plan follows the active HLS VOD segment durations; direct playback uses the configured streaming segment duration. `options.subtitle_streams` contains only WebVTT-convertible text tracks; bitmap formats such as PGS are not advertised through this mechanism.

## Browser player

A direct MP4 can be assigned directly to a normal HTML `<video>` element. For transformed output, use the returned HLS URL. Browsers with native HLS support can consume it directly; other browser clients can feed the same fragmented-MP4 HLS through an MSE HLS implementation such as hls.js. This distinction belongs in the client platform adapter, not the React playback screen or server resolver.

## Resource limits and cleanup

`max_sessions`, `max_sessions_per_account`, `max_video_transcodes` and `max_audio_transcodes` are enforced independently. `max_sessions` bounds the node; `max_sessions_per_account` bounds one account on it, and its refusal is the distinct `account_session_limit` described above, because a client must treat the two differently. Transcode entitlements belong to a session: each session created is its own logical viewer, which a `PATCH` replacement inherits, so two sessions transcoding hold two entitlements even on one account. Transcode limits count those entitlements, not seeks, replacement generations or physical encoder processes. Once acquired, a logical session retains its entitlement through changes that still transcode, and releases it on DELETE or the page-exit close, on session expiry, **after `transcode_entitlement_idle_ms` with no stream activity** (new in 0.49.0; it used to be held until the session was erased), or **when a `PATCH` leaves transcode** (new in 0.60.0): a session switched to direct or remux gives up its video entitlement, and one whose audio is no longer transcoded gives up its audio entitlement. Switching back to transcode reacquires it like any other `PATCH`, and may then be refused with `resource_limit`. The release is per session: it clears only what that logical viewer holds, and is skipped while another session record still shares the same logical viewer. Admission reserves pending session/transcode capacity before pipeline startup, so simultaneous POST/PATCH requests cannot race through a limit before either session becomes visible. `video_transcodes` and `audio_transcodes` in status report those admission entitlements; `running_video_transcode_pipelines` and `running_audio_transcode_pipelines` separately report live physical encoders. Hitting a limit returns HTTP 429, and **the two 429s are not interchangeable**. `account_session_limit` is identical on every node, so a client must not walk the cluster on it. `resource_limit` is this node's property, and its failure axes differ by path: on **create** it is `scope: node` — another node may have capacity, so walking is right — while on **update** it is `scope: request`, because the session already exists here and walking would mean abandoning a generation that is still serving. Both carry `node_healthy: true` and `alternative_may_succeed: true`: on the update path the alternative is a different instruction against this same node, such as remux instead of transcode or a lower `max_height`. In every case the viewer's current playback is untouched by the refusal. Malformed/incompatible playback requests return `400 bad_playback_request`, a copy the segment container cannot carry returns `422 copy_not_supported`, missing media/session state returns 404, a superseded generation returns 410, and media-engine failures return 503 (422 when the source is `source_unsupported`). Probe and pipeline-start failures use stage-specific error codes (`playback_probe_failed` or `playback_pipeline_start_failed`), include `trace`/`stage` (and `reason` when the engine gave one) in the `error` object, and return the same values in `X-Macha-Playback-Trace` and `X-Macha-Playback-Stage` for correlation with `playback[trace]` server logs.

Logical sessions expire after `session_idle_ms` without control or valid
current-generation stream activity. Expiry cancels the in-process pipeline and
removes any spilled generated fragments. Explicit `DELETE` performs the same
cleanup immediately.

A session that has never served a stream object expires instead after the much
shorter `session_unused_idle_ms` (120 seconds by default), because the
transcode entitlement is held by the session rather than by the pipeline and is
therefore not released by idle-pipeline reclamation. One stream request of any
kind — playlist, fragment, subtitle or Direct Play body — moves the session to
the full `session_idle_ms` for the rest of its life, so a paused or seeking
player is never subject to the shorter clock.

Physical remux/transcode pipelines have a shorter independent
`pipeline_idle_ms` lease (60 seconds by default). Valid current-generation
playlist, fragment and subtitle requests renew it. Superseded-generation
(`410`) and otherwise invalid stream requests do not, so a client retry loop
cannot retain an abandoned encoder. Reclamation is event-driven, never interrupts an active
stream HTTP request, and leaves the logical session available until
`session_idle_ms` for client reconciliation. `GET /api/v1/playback/status`
reports `pipeline_idle_ms` and the cumulative `idle_pipelines_reclaimed` count.
While a pipeline exists, `GET` on the session adds `engine_running`,
`segments_ready` and, when set, `engine_exit_code` and `engine_error`. After
physical reclamation, `GET` still returns the logical session, without those
fields; a client that resumes it can `PATCH` the session (including its
current preferences/position) to create a fresh physical generation.

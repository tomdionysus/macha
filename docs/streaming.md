# Streaming

The playback API is instruction-based. **The server reports what a file is and performs what it is asked for; it does not choose.** A client reads the media facts, decides what to do with them against its own decoder, and instructs. Media probing, remuxing and transcoding run in-process through the FFmpeg libraries behind `MediaEngine`; no `ffmpeg` or `ffprobe` subprocess is launched.

There is no `auto` mode. `preferences.mode` is required on every session, and a missing or unrecognised mode is `400`.

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
| `preferences.container` | `fmp4` (default), `mpegts` | HLS segment container |

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
- `direct` or `remux` with a stream set to `transcode`, or with `max_height`/`max_bitrate` — a quality change is a re-encode, and those modes copy;
- `transcode` with both streams copied — nothing is being re-encoded, so it is a remux or a direct;
- a copy into a container that cannot carry that codec (fragmented MP4 carries H.264, HEVC and AV1 video, and AAC, AC-3, E-AC-3 and Opus audio);
- a transcode when the node has no encoder for the target.

A refusal names the rule. The server does not quietly reinterpret a mode into the one that would have worked, because a session that reports a mode it is not performing misleads everything downstream of it.

**What the server does not do.** It does not ask what the client can play, and there is no `capabilities` field. Whether a device can decode what it asked for is the client's business; the server reports the facts and carries out the instruction. A client that asks for `direct` on a file it cannot demux gets the file.

## Media facts

Facts come from one probe of the file, are persisted in the catalogue media profile, and are reported identically wherever they appear:

| field | notes |
|---|---|
| `container` | the resolved container family: `mp4`, `matroska`, `webm`, `mp3`, `flac`, `ogg`. **Match on this**, not on `format`. |
| `format` | the raw libavformat demuxer name list, e.g. `matroska,webm`. It names every container that demuxer handles, so a Matroska file lists `webm` too; matching it directly is how a WebM-capable device ends up being handed a Matroska file. |
| per stream | `index`, `type`, `codec`, `profile`, `language`, `width`, `height`, `channels`, `sample_rate`, `bit_depth`, `level`, `color_transfer`, `dolby_vision_profile`, `dolby_vision_compatibility`, `bitrate`, `default`, `forced`, `attached_picture` |

`bit_depth` is derived from the pixel format when the container does not carry it. `color_transfer` is the transfer function name (`smpte2084` for PQ, `arib-std-b67` for HLG, absent for SDR or unread). The Dolby Vision fields appear only on a stream carrying a configuration record; compatibility `1` is HDR10-compatible, `2` is SDR, `4` is HLG.

These facts are available **before** a session exists, which is what makes an instruction possible:

```text
GET /api/v1/playback/media?media_id=<macha: or path: identity>
GET /api/v1/playback/media?item_id=<catalogue item>
```

It returns, per media, the identity, path, size, `container`, `format`, `duration_ms`, `bitrate`, the full stream list, and an `operations` object describing what this node can do with that file: `direct` (always true), `copy_into_fmp4` with separate `video` and `audio` booleans, and `transcode_video` / `transcode_audio` reflecting the encoders present in this build. No session is created and no pipeline starts.

`operations` is a fact, not a decision. It is answered by the node that received the request, from that node's build, about that one file, so it is per node and per source and must never be cached as a property of the cluster. Nodes on different builds legitimately give different answers, and the node the client will actually stream from is the only one whose answer matters.

When one media of an item cannot be read, the others are still reported and the failures are listed separately in `unavailable`, each with its `media_id`, a `reason` and a message.

## Why a request could not be answered

Failures carry a `reason` so the client can tell situations apart that would otherwise look identical. The server states it and does nothing with it; deciding whether to ask another node, transcode from elsewhere, or stop, is the client's.

| `reason` | meaning | what it implies |
|---|---|---|
| `source_unreadable` | this node could not read the file's bytes | the file may be perfectly good elsewhere: another node is worth asking |
| `source_unsupported` | the bytes were read and are not media this build can demux | no node running this build will do better |
| `source_read_timed_out` | reading did not finish inside the deadline | transient; a retry may succeed |

`source_unsupported` is reported as `422`. The other two keep their transport-level status, `503` on the playback endpoints and `422` on the catalogue media profile, because whether they are worth retrying is a client judgement, not a server one.

The same facts are on the catalogue media profile (`GET /api/v1/catalogue/media/<macha id>/profile`, `schema_version` 3) for any media with an immutable identity. That pre-session availability is intentional and guaranteed for `macha:` identities: with no stored profile the endpoint produces one there and then, at foreground priority, and persists it. It answers `404` only when the media is not on this node, and `422` with a `reason` when the file could not be probed. The reading order is: the persisted profile if one exists, otherwise a foreground probe whose result is persisted for later readers; background profiling of unwatched media runs at a lower priority and yields to a viewer.

An older stored profile that predates a fact (a schema below the current one, on a video stream) is treated as stale and regenerated on that media's next playback.

## Media engine boundary

FFmpeg streams marked `AV_DISPOSITION_ATTACHED_PIC` are metadata images, not playable video. Macha preserves that disposition in probe results, reports the stream as non-playable/other media, and excludes it from video selection. This prevents an MP3 with embedded album art from being promoted into an HLS/fMP4 video pipeline.

`PlaybackManager` owns policy and `MediaEngine` owns media operations. The interface contains media concepts (`probe`, `PlaybackPlan`, `MediaEngineSession`, fragments and subtitle extraction), not FFmpeg options. The current implementation is `LibavMediaEngine`, but another implementation can replace it without changing the HTTP/session API.

Each media source is an immutable seekable `MediaInput`. The libav input `AVIOContext` maps read/seek callbacks directly onto the pinned Macha `ReadHandle`. There is no loopback HTTP hop and no FUSE dependency. Replacing or renaming a pathname after playback starts does not silently change the extents underneath that session.

Probe and subtitle readers are deliberately *not* registered with `PlaybackTracker`; only the actual direct/transformed playback reader drives read-ahead/current-file/catalogue hydration. Stable `macha:` media IDs are indexed by metadata generation, so selecting among catalogue representations does not rescan and re-hash the whole namespace on every session creation.

Transformed output uses the MP4 muxer with fragmented-MP4 flags and a custom output `AVIOContext`. Top-level MP4 init and media fragments are published directly into `MediaSegmentStore`. The HTTP server reads playlists/fragments from that store. No temp directory is polled for readiness.

A transformed session is served as a master playlist (`master.m3u8`: one `EXT-X-STREAM-INF` with `BANDWIDTH`, `CODECS` and `RESOLUTION`) pointing at a media playlist (`media.m3u8`). The media playlist is an `EXT-X-PLAYLIST-TYPE:EVENT` list of the fragments that exist, closed with `#EXT-X-ENDLIST` when the generation produces its last one; a request that arrives before the first fragment exists is held until it does, bounded by `startup_timeout_ms`, rather than answered `404`. Advertising fragments that have not been encoded lets a player queue against the encoder, which cost one 2017 television a 98-second black screen. Fragment bytes remain lazy: a request for a valid future fragment raises the producer demand watermark and waits.

For stream-copy video, the VOD planner uses the demuxer's keyframe index and chooses random-access boundaries near `segment_duration_ms`. Matroska/WebM Cues are explicitly materialised through the demuxer's seek path before that index is inspected, because probing alone may expose only a partial early-file index. The resulting plan is rejected if any advertised fragment would be grossly larger than the configured target, preventing a partial index from turning the unindexed remainder of a movie into one fragment. A transformed seek is aligned to the first indexed keyframe at or after the requested position so the first advertised segment is independently decodable. A remux whose keyframe index is unusable fails rather than silently changing mode. A fragment is as long as the source GOP makes it, up to 90 seconds; only a gap or tail beyond that rejects the plan, because scene-cut encodes routinely exceed a fixed multiple of the target. Transcoded video uses the encoder GOP cadence as its VOD boundary plan.

Stream-copy timestamps are normalised only after rescaling into the MP4 stream's final muxer timebase. Missing PTS/DTS are synthesised conservatively and equal/backwards DTS values are advanced with a persistent per-stream timeline correction. Legitimate PTS-before-DTS composition offsets are preserved rather than clamped; fragmented MP4 is emitted with signed composition-time offsets enabled. Repairs that actually modify timestamps are logged with per-stream counters.

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

Publication traffic has its own loader transport class below viewer foreground
and read-ahead but above speculative maintenance. Restarting Macha does not
demote user-requested spool data: journal recovery is provenance for validation
and crash safety, while the resumed publication remains loader work.

Seek-only updates are determined by effective session policy rather than JSON shape. A client may resend its current `preferences` object together with `seek_ms`; when those preferences are unchanged Macha reuses the active immutable media probe and reusable VOD/random-access plan instead of performing a fresh container-planning pass.

## Public HTTP behaviour

The catalogue/playback HTTP server uses a bounded accepted-connection queue and configurable worker pool. Response bodies may be in-memory or streaming sources, so direct media and generated segments are not assembled into one huge response. Several clients and several simultaneous HLS fragment requests can be active independently.

The server currently closes each HTTP connection after its response. Concurrency comes from independent workers rather than HTTP keep-alive/multiplexing.

If the API has a permanent Bearer token, session creation and control still require it. Returned stream URLs use a separate high-entropy session capability in the path so native players can fetch direct files, playlists and fragments without learning the permanent API token. Stream capabilities expire with the session.

## Status

```text
GET /api/v1/playback/status
```

Reports enablement, current session/transcode counts, media-engine backend/version and encoder availability. Legacy executable-discovery status fields are reported as false/empty; the active backend/version and encoder capabilities are the authoritative status fields.

## Create a session

```text
POST /api/v1/playback/sessions
Content-Type: application/json
Idempotency-Key: <client-generated logical request key>
Macha-Viewer-Session: <client-generated persistent player key>
```

Use either `item_id` or `media_id`. `item_id` allows the resolver to choose among every media representation attached to the catalogue item. `path:/logical/file` is also accepted as a media identity.

Clients should generate one opaque `Idempotency-Key` for each logical creation
attempt and reuse it after a timeout, disconnect or failover. The same key and
normalized request joins or replays the same session ID, capability and
`generation`; using that key for different request semantics returns
`409 idempotency_conflict`. The response echoes `Idempotency-Key` and reports
`X-Macha-Idempotency: created|replayed`. Omitting the header preserves the
legacy non-idempotent behaviour.

`Macha-Viewer-Session` has a different, longer lifetime from
`Idempotency-Key`: use one stable opaque value for the lifetime of a player/UI
session, while using a fresh idempotency key for each distinct creation
attempt. A later POST with the same viewer-session key atomically replaces the
player's current generation while retaining its session ID and transcode
entitlement. This makes POST-based seek/reload recovery equivalent to PATCH for
admission purposes. The key can instead be supplied as `viewer_session_id` in
the JSON body; if both forms are present they must agree. Clients should prefer
the header and must not share a key among simultaneous independent viewers.
Omitting it preserves the legacy behaviour in which each POST is a distinct
logical session.

Session admission first reads the immutable media profile from cluster metadata,
which requires no media-object reads. A miss never produces a client-visible
profiling gate: Macha continues normal media negotiation using its configured
media engine. Concurrent inspection of the same immutable file is coalesced,
and a viewer takes over an already-running speculative scan instead of waiting
behind background work. The successful result is published asynchronously for
later sessions. Clients should still reuse the same idempotency key after a
timeout or disconnect.

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
    "mode": "remux",
    "audio": "transcode",
    "container": "fmp4",
    "max_height": 1080,
    "max_bitrate": 8000000,
    "audio_language": "eng"
  }
}
```

`max_height` and `max_bitrate` are instructions, not capabilities: either one makes the video a re-encode, and combining either with `video: copy` is a `400`.

The response separates requested preferences, resolved playback, original source metadata and actual output metadata. The `/api/v1` schema is currently owned by Macha and Macha Client and may be changed in place while there are no third-party implementations. A representative response is:

```json
{
  "session_id": "...",
  "item_id": "tmdb:movie:...",
  "media_id": "macha:...",
  "mode": "remux",
  "duration_ms": 5400000,
  "seek_ms": 0,
  "preferences": {
    "mode": "remux",
    "video": null,
    "audio": "transcode",
    "container": "fmp4",
    "max_height": null,
    "max_bitrate": null,
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
    "format": "mp4",
    "video": { "source_stream": 0, "transform": "copy", "codec": "hevc", "width": 1920, "height": 1080, "bitrate": 7500000 },
    "audio": { "source_stream": 1, "transform": "transcode", "codec": "aac", "channels": 2, "sample_rate": 48000, "bitrate": 192000 }
  },
  "stream": { "url": "/api/v1/playback/stream/...", "mime_type": "application/vnd.apple.mpegurl", "subtitle_url": null },
  "options": {
    "modes": ["transcode"],
    "quality_heights": [720, 480, 360],
    "media_ids": ["macha:..."],
    "audio_streams": [],
    "subtitle_streams": [],
    "can_seek": true,
    "can_change_quality": true,
    "can_switch_media": false
  }
}
```

`preferences` echoes the instruction as given; the top-level `mode` is what that instruction amounts to (a session with any stream being encoded reports `transcode`, whatever shorthand was used). `source.streams` describes the original elementary streams. `output.video`/`output.audio` describe the selected source stream, whether it is copied or transcoded, and the actual output codec/geometry/audio format. A CRF H.264 transcode has no fixed video bitrate and therefore omits `output.video.bitrate` unless an explicit target bitrate is in force. Returned stream URLs are relative to the API origin.

## Inspect, change and stop a session

```text
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
```

`PATCH` keeps the same logical session ID and capability but may replace the underlying media-engine generation. A PATCH that changes only `subtitle_stream` and/or `subtitle_language` is special: it keeps the active A/V generation and stream URL unchanged and updates only the external segmented-WebVTT resource. Explicit stream indexes are validated; an invalid selection is rejected rather than falling back silently. `options.modes`, `options.quality_heights`, `options.audio_streams` and `options.subtitle_streams` are generated by negotiating each candidate against the current source/capabilities/preferences, so clients should render those arrays rather than inventing controls locally. Every PATCH response is the authoritative new session state.

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

`max_sessions`, `max_video_transcodes` and `max_audio_transcodes` are enforced independently. Transcode limits count logical viewer entitlements, not seeks, replacement generations or physical encoder processes. Once acquired, a logical session retains its entitlement through Direct/Remux/Transcode changes and physical idle-pipeline reclamation, then releases it exactly once on DELETE or session expiry. Admission reserves pending session/transcode capacity before pipeline startup, so simultaneous POST/PATCH requests cannot race through a limit before either session becomes visible. `video_transcodes` and `audio_transcodes` in status report those admission entitlements; `running_video_transcode_pipelines` and `running_audio_transcode_pipelines` separately report live physical encoders. Hitting a limit returns HTTP 429. Malformed/incompatible playback requests return 400, missing media/session state returns 404, and media-engine failures return 503. Probe and pipeline-start failures use stage-specific error codes (`playback_probe_failed` or `playback_pipeline_start_failed`), include `trace`/`stage` in the JSON body, and return the same trace in `X-Macha-Playback-Trace` for correlation with `playback[trace]` server logs.

Logical sessions expire after `session_idle_ms` without control or valid
current-generation stream activity. Expiry cancels the in-process pipeline and
removes any spilled generated fragments. Explicit `DELETE` performs the same
cleanup immediately.

Physical remux/transcode pipelines have a shorter independent
`pipeline_idle_ms` lease (60 seconds by default). Valid current-generation
playlist, fragment and subtitle requests renew it. Obsolete-generation and
otherwise invalid stream requests do not, so a client retry loop cannot retain
an abandoned encoder. Reclamation is event-driven, never interrupts an active
stream HTTP request, and leaves the logical session available until
`session_idle_ms` for client reconciliation. `GET /api/v1/playback/status`
reports `pipeline_idle_ms` and the cumulative `idle_pipelines_reclaimed` count.
After physical reclamation, `GET` still returns the logical session with no
running engine; a client that resumes it can `PATCH` the session (including its
current preferences/position) to create a fresh physical generation.

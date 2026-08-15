# Streaming

0.7.0 adds negotiated media streaming to the existing HTTP API. The public API deals in media capabilities, playback plans and sessions. Media probing, remuxing and transcoding run in-process through the FFmpeg libraries behind `MediaEngine`; no `ffmpeg` or `ffprobe` subprocess is launched.

## Modes

The resolver considers all media representations bound to a catalogue item and prefers the cheapest compatible plan:

1. **direct** — serve the original logical Macha file with HTTP byte ranges;
2. **remux** — stream-copy compatible elementary streams into fragmented-MP4 HLS;
3. **transcode** — encode only streams that require conversion, then emit fragmented-MP4 HLS.

The current transformed output is one HLS rendition using an `init.mp4` plus `.m4s` fragments. Adaptive bitrate is not implemented yet. Video is copied when the source can be represented in fMP4, the client reports the codec as supported, and no requested quality conversion is required. Audio is copied when AAC is acceptable. The current encode targets are H.264 video and AAC audio; negotiation fails rather than producing a codec the client did not advertise.

## Media engine boundary

`PlaybackManager` owns policy and `MediaEngine` owns media operations. The interface contains media concepts (`probe`, `PlaybackPlan`, `MediaEngineSession`, fragments and subtitle extraction), not FFmpeg options. The 0.7.0 implementation is `LibavMediaEngine`, but another implementation can replace it without changing the HTTP/session API.

Each media source is an immutable seekable `MediaInput`. The libav input `AVIOContext` maps read/seek callbacks directly onto the pinned Macha `ReadHandle`. There is no loopback HTTP hop and no FUSE dependency. Replacing or renaming a pathname after playback starts does not silently change the extents underneath that session.

Probe and subtitle readers are deliberately *not* registered with `PlaybackTracker`; only the actual direct/transformed playback reader drives read-ahead/current-file/catalogue hydration. Stable `macha:` media IDs are indexed by metadata generation, so selecting among catalogue representations does not rescan and re-hash the whole namespace on every session creation.

Transformed output uses the MP4 muxer with fragmented-MP4 flags and a custom output `AVIOContext`. Top-level MP4 init and media fragments are published directly into `MediaSegmentStore`. The HTTP server reads playlists/fragments from that store. No temp directory is polled for readiness.

Stream-copy timestamps are normalised only after rescaling into the MP4 stream's final muxer timebase. Missing PTS/DTS are synthesised conservatively and equal/backwards DTS values are advanced with a persistent per-stream timeline correction. This is necessary around backward keyframe seeks and also after timebase conversion, where two distinct source timestamps can quantise to the same MP4 tick. Any repair is logged with per-stream counters.

The segment store is a bounded producer/consumer queue. Once the producer is `max_ahead_segments` beyond actual client demand it blocks on a condition variable and resumes when later fragment indexes are requested. This prevents a fast remux from pulling an entire movie through the DHT. Resident generated fragments are bounded by `segment_memory_bytes`; old consumed fragments can spill below `temp_path`.

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

## Public HTTP behaviour

The catalogue/playback HTTP server uses a bounded accepted-connection queue and configurable worker pool. Response bodies may be in-memory or streaming sources, so direct media and generated segments are not assembled into one huge response. Several clients and several simultaneous HLS fragment requests can be active independently.

The server currently closes each HTTP connection after its response. Concurrency comes from independent workers rather than HTTP keep-alive/multiplexing.

If the API has a permanent Bearer token, session creation and control still require it. Returned stream URLs use a separate high-entropy session capability in the path so native players can fetch direct files, playlists and fragments without learning the permanent API token. Stream capabilities expire with the session.

## Status

```text
GET /api/v1/playback/status
```

Reports enablement, current session/transcode counts, media-engine backend/version and encoder availability. The old `ffmpeg_available`, `ffprobe_available` and `ffmpeg_version` fields are retained as false/empty for one compatibility release only.

## Create a session

```text
POST /api/v1/playback/sessions
Content-Type: application/json
```

Use either `item_id` or `media_id`. `item_id` allows the resolver to choose among every media representation attached to the catalogue item. `path:/logical/file` is also accepted as a media identity.

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

A browser can describe its actual capabilities:

```json
{
  "media_id": "path:/Movies/Example.mkv",
  "capabilities": {
    "containers": ["mp4"],
    "video_codecs": ["h264"],
    "audio_codecs": ["aac", "mp3"],
    "hls_fmp4": true,
    "max_width": 3840,
    "max_height": 2160
  },
  "preferences": {
    "mode": "auto",
    "max_height": 1080,
    "max_bitrate": 8000000,
    "audio_language": "eng"
  }
}
```

Default capabilities are intentionally conservative: MP4, H.264, AAC/MP3 and fragmented-MP4 HLS. `mode` is `auto`, `direct`, `remux` or `transcode`. A forced mode fails rather than silently choosing another mode.

The response includes the session ID, selected media representation, mode/MIME/stream URL, probed source/stream details, selected copy/transcode decisions, available representations/tracks, and an optional WebVTT subtitle URL. `stream_url` is relative to the API origin.

## Inspect, change and stop a session

```text
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
```

`PATCH` keeps the same logical session ID and capability but may replace the underlying media-engine generation. Explicit stream indexes are validated; an invalid selection is rejected rather than falling back silently.

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

Subtitles are off by default. Select one by stream index or language; supported text subtitle codecs are converted in-process to a separate WebVTT resource.

## Browser player

A direct MP4 can be assigned directly to a normal HTML `<video>` element. For transformed output, use the returned HLS URL. Browsers with native HLS support can consume it directly; other browser clients can feed the same fragmented-MP4 HLS through an MSE HLS implementation such as hls.js. This distinction belongs in the client platform adapter, not the React playback screen or server resolver.

## Resource limits and cleanup

`max_sessions`, `max_video_transcodes` and `max_audio_transcodes` are enforced independently. Admission reserves pending session/transcode capacity before pipeline startup, so simultaneous POST/PATCH requests cannot race through a limit before either session becomes visible. Hitting a limit returns HTTP 429. Malformed/incompatible playback requests return 400, missing media/session state returns 404, and media-engine failures return 503. Probe and pipeline-start failures use stage-specific error codes (`playback_probe_failed` or `playback_pipeline_start_failed`), include `trace`/`stage` in the JSON body, and return the same trace in `X-Macha-Playback-Trace` for correlation with `playback[trace]` server logs.

Sessions expire after `session_idle_ms` without control or stream activity. Expiry cancels the in-process pipeline and removes any spilled generated fragments. Explicit `DELETE` performs the same cleanup immediately.

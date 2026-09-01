# Streaming

The public playback API deals in media capabilities, playback plans and sessions. Finite transformed media uses immutable HLS VOD manifests with lazy fragment generation. Media probing, remuxing and transcoding run in-process through the FFmpeg libraries behind `MediaEngine`; no `ffmpeg` or `ffprobe` subprocess is launched.

## Modes

The resolver considers all media representations bound to a catalogue item and prefers the cheapest compatible plan:

1. **direct** — serve the original logical Macha file with HTTP byte ranges;
2. **remux** — stream-copy compatible elementary streams into fragmented-MP4 HLS;
3. **transcode** — encode only streams that require conversion, then emit fragmented-MP4 HLS.

The current transformed output is one HLS rendition using an `init.mp4` plus `.m4s` fragments. Adaptive bitrate is not implemented yet. Video is copied when the source can be represented in fMP4, the client reports the codec as supported, and no requested quality conversion is required. Audio is copied when AAC is acceptable. The current encode targets are H.264 video and AAC audio; negotiation fails rather than producing a codec the client did not advertise.

## Media engine boundary

FFmpeg streams marked `AV_DISPOSITION_ATTACHED_PIC` are metadata images, not playable video. Macha preserves that disposition in probe results, reports the stream as non-playable/other media, and excludes it from video selection. This prevents an MP3 with embedded album art from being promoted into an HLS/fMP4 video pipeline.

`PlaybackManager` owns policy and `MediaEngine` owns media operations. The interface contains media concepts (`probe`, `PlaybackPlan`, `MediaEngineSession`, fragments and subtitle extraction), not FFmpeg options. The current implementation is `LibavMediaEngine`, but another implementation can replace it without changing the HTTP/session API.

Each media source is an immutable seekable `MediaInput`. The libav input `AVIOContext` maps read/seek callbacks directly onto the pinned Macha `ReadHandle`. There is no loopback HTTP hop and no FUSE dependency. Replacing or renaming a pathname after playback starts does not silently change the extents underneath that session.

Probe and subtitle readers are deliberately *not* registered with `PlaybackTracker`; only the actual direct/transformed playback reader drives read-ahead/current-file/catalogue hydration. Stable `macha:` media IDs are indexed by metadata generation, so selecting among catalogue representations does not rescan and re-hash the whole namespace on every session creation.

Transformed output uses the MP4 muxer with fragmented-MP4 flags and a custom output `AVIOContext`. Top-level MP4 init and media fragments are published directly into `MediaSegmentStore`. The HTTP server reads playlists/fragments from that store. No temp directory is polled for readiness.

Finite media is presented as HLS VOD. Before the transformed pipeline starts, the media engine prepares the complete segment-duration plan. The playlist is therefore complete and immutable from its first response, contains `#EXT-X-PLAYLIST-TYPE:VOD` and `#EXT-X-ENDLIST`, and never exposes a moving event/live edge. Fragment bytes remain lazy: a request for a valid future segment raises the producer demand watermark and waits for sequential generation to reach that fragment.

For stream-copy video, the VOD planner uses the demuxer's keyframe index and chooses random-access boundaries near `segment_duration_ms`. Matroska/WebM Cues are explicitly materialised through the demuxer's seek path before that index is inspected, because probing alone may expose only a partial early-file index. The resulting plan is rejected if any advertised fragment would be grossly larger than the configured target, preventing a partial index from turning the unindexed remainder of a movie into one fragment. A transformed seek is aligned to the first indexed keyframe at or after the requested position so the first advertised segment is independently decodable. If automatic remux has no usable keyframe index, it may fall back to H.264 video transcode only when the client advertised H.264 and the encoder is available; an explicitly forced `remux` still fails rather than silently changing mode. Transcoded video uses the encoder GOP cadence as its VOD boundary plan.

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
```

Use either `item_id` or `media_id`. `item_id` allows the resolver to choose among every media representation attached to the catalogue item. `path:/logical/file` is also accepted as a media identity.

Clients should generate one opaque `Idempotency-Key` for each logical creation
attempt and reuse it after a timeout, disconnect or failover. The same key and
normalized request joins or replays the same session ID, capability and
`generation`; using that key for different request semantics returns
`409 idempotency_conflict`. The response echoes `Idempotency-Key` and reports
`X-Macha-Idempotency: created|replayed`. Omitting the header preserves the
legacy non-idempotent behaviour.

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
    "mode": "auto",
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

`preferences.mode` records what the user selected, including `auto`; top-level `mode` is what negotiation actually resolved. `source.streams` describes the original elementary streams. `output.video`/`output.audio` describe the selected source stream, whether it is copied or transcoded, and the actual output codec/geometry/audio format. A CRF H.264 transcode has no fixed video bitrate and therefore omits `output.video.bitrate` unless an explicit target bitrate is in force. Returned stream URLs are relative to the API origin.

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

`max_sessions`, `max_video_transcodes` and `max_audio_transcodes` are enforced independently. Admission reserves pending session/transcode capacity before pipeline startup, so simultaneous POST/PATCH requests cannot race through a limit before either session becomes visible. Hitting a limit returns HTTP 429. Malformed/incompatible playback requests return 400, missing media/session state returns 404, and media-engine failures return 503. Probe and pipeline-start failures use stage-specific error codes (`playback_probe_failed` or `playback_pipeline_start_failed`), include `trace`/`stage` in the JSON body, and return the same trace in `X-Macha-Playback-Trace` for correlation with `playback[trace]` server logs.

Sessions expire after `session_idle_ms` without control or stream activity. Expiry cancels the in-process pipeline and removes any spilled generated fragments. Explicit `DELETE` performs the same cleanup immediately.

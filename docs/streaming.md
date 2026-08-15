# Streaming

0.7.0 adds negotiated media streaming to the existing HTTP API. The public API deals in media capabilities, playback plans and sessions. FFmpeg is an implementation detail behind `MediaEngine`; the current `FfmpegProcessEngine` can later be replaced by an in-process libav implementation without changing the playback API.

## Modes

The resolver considers all media representations bound to a catalogue item and prefers the cheapest compatible plan:

1. **direct** — serve the original logical Macha file with HTTP byte ranges;
2. **remux** — stream-copy compatible elementary streams into fragmented-MP4 HLS;
3. **transcode** — encode only streams that require conversion, then emit fragmented-MP4 HLS.

The current transformed output is one HLS rendition using an `init.mp4` plus `.m4s` fragments. Adaptive bitrate is not implemented yet. Video is copied when the source is H.264, HEVC or AV1, the client reports that codec as supported, and no requested quality conversion is required. Audio is copied when it is AAC and supported by the client. The current encode targets are H.264 video and AAC audio; negotiation fails rather than producing a codec the client did not advertise.

## Media engine boundary

`MediaEngine` owns source probing and transformed-output production. `MediaEngineSession` owns the lifetime of one active producer. No FFmpeg argv or process identifier is exposed above this layer. The initial backend executes `ffprobe` and `ffmpeg` directly with an argument vector; it never invokes a shell.

FFmpeg reads the logical Macha file through a loopback-only private HTTP source with byte-range support. This keeps source reads seekable and routes them through the normal filesystem, distributed-store, playback-tracking and hydration path. The private source URL contains an opaque per-session capability token. The source lease pins the resolved `FsEntry` snapshot: replacing or renaming a pathname after playback starts does not silently change the extents underneath that session.

Generated HLS files are kept below `streaming.temp_path`. A producer is paused when it runs more than `max_ahead_segments` ahead of the highest fragment requested by the client and resumed as demand advances. This prevents a fast remux from pulling an entire large file through the DHT simply because playback started.

## Public HTTP behaviour

The catalogue/playback HTTP server uses a bounded connection queue and a configurable worker pool. Response bodies may be in-memory or streaming sources, so direct media and generated segments are not assembled into a single `std::string` before sending. Several clients and several simultaneous HLS fragment requests can therefore be active independently.

The server currently closes each HTTP connection after its response. Concurrency comes from independent workers rather than HTTP keep-alive/multiplexing.

If the API has a permanent Bearer token, session creation and control still require it. Returned stream URLs use a separate high-entropy session capability in the path so native players can fetch the direct file, playlists and fragments without learning the permanent API token. Stream capabilities expire with the session.

## Status

```text
GET /api/v1/playback/status
```

Reports enablement, current session/transcode counts and the discovered FFmpeg/ffprobe status.

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

The response includes:

- `session_id`;
- selected `media_id`;
- `mode`, `mime_type` and `stream_url`;
- source duration/bitrate/format;
- every probed elementary stream;
- selected stream indexes and copy/transcode decisions;
- available mode and media-representation options plus described audio/subtitle tracks;
- optional `subtitle_url` containing WebVTT.

`stream_url` is relative to the API origin. Direct mode uses the original media MIME type. Remux/transcode use `application/vnd.apple.mpegurl`.

## Inspect, change and stop a session

```text
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
```

`PATCH` keeps the same logical session ID and stream capability but may replace the underlying media pipeline. This lets a player change track, quality or transcode policy without knowing whether the backend process can be reconfigured in place. Explicit stream indexes are validated; an invalid selection is rejected rather than falling back silently.

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

A large seek restarts the transformed producer at the requested media timestamp and increments the internal generation. Old generation URLs stop resolving, while the session ID/capability remain stable. `media_id` may also be supplied to switch representation explicitly.

Subtitles are off by default. Select a subtitle by `subtitle_stream` or `subtitle_language`; the current engine extracts it to a separate WebVTT URL where conversion is supported.

## Browser player

A direct MP4 can be assigned directly to a normal HTML `<video>` element. For transformed output, use the returned HLS URL. Browsers with native HLS support can consume it directly; other browser clients can feed the same fragmented-MP4 HLS through an MSE HLS implementation such as hls.js. This distinction belongs in the client platform adapter, not in the React playback screen or server resolver.

## Resource limits and cleanup

`max_sessions`, `max_video_transcodes` and `max_audio_transcodes` are enforced independently. Hitting a limit returns HTTP 429. Malformed/incompatible playback requests return 400, missing media/session state returns 404, and media-engine failures return 503. This allows many cheap direct/remux sessions while keeping expensive encoders bounded.

Sessions expire after `session_idle_ms` without control or stream activity. Expiry stops the media-engine session, removes source capabilities and deletes generated files. Explicit `DELETE` performs the same cleanup immediately.

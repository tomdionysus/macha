# Installation and configuration

The complete example is `macha.yaml.example`. The important rules are short:

- Every node uses the **same key contents**. The path may differ.
- `network.advertise` must be reachable from the other nodes. `listen: 0.0.0.0` does not make `127.0.0.1` a useful advertised address.
- `dht.replicas` and `dht.metadata_replicas` are cluster policy and must be identical on every node. They may be changed by stopping the whole cluster, changing every node, then restarting it. Do not roll replica-count changes through a running cluster; mixed policies are unsupported.
- `dht.extent_size` is fixed for an existing namespace.
- Bootstrap may be one-way or symmetric. A node with configured bootstrap peers will not create a new namespace while none of them is reachable.
- Storage backend directories must already exist. Missing paths are treated as missing disks, not created automatically.
- `state_path` is not bulk storage. Put it on reliable local system storage.
- `failure_domain` describes shared fate. Machines in the same site should normally use the same value.

`log_level` is one of `ALL`, `DEBUG`, `INFO`, `WARN` or `ERROR`; default `INFO`. `DEBUG` currently includes expensive write-path diagnostics and is not suitable for throughput measurements.

Start with:

```sh
macha --config /etc/macha.yaml
```

Send `SIGHUP` to reload local storage-backend, cache and hydration configuration. Replica-count changes require a coordinated cluster stop/edit/restart; the old metadata quorum commits the new voter/data policy when the cluster comes back. `extent_size` cannot change for an existing namespace.

A YAML file is required. These CLI options override values from that file for one invocation:

```text
--config FILE
--state-path PATH
--key-file FILE
--cache-path PATH --cache-blocks N
--bootstrap HOST[:PORT]          repeatable
--listen ADDRESS
--advertise HOST
--failure-domain NAME
--port PORT
--replicas N
--metadata-replicas N
--extent-size SIZE               1M..64M; compiled default 16M
--read-ahead N
--connect-timeout MS             TCP connection attempt only
--max-frame-size SIZE             4K..4M; default 256K
--control-stall-notice MS        DEBUG notice only; 0 disables
--data-stall-notice MS           DEBUG notice only; 0 disables
--metadata-cache MS
--mount PATH
--log-level LEVEL
```

RPC duration itself is unbounded. Stall notices are observability thresholds; they do not cancel requests. Health/control traffic uses the same peer connection at absolute highest priority. A peer is marked dead only after that unified transport cannot establish liveness within `dead_after_ms`.

## Streaming configuration

Streaming shares the catalogue HTTP listener. `streaming.enabled: true` therefore requires `catalogue.api.enabled: true`. The subprocess backend uses `ffprobe` for source inspection and `ffmpeg` for HLS remux/transcode and subtitle extraction.

```yaml
catalogue:
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7438
    # token_file: /etc/macha-api.token
    max_request_bytes: 8M
    workers: 16
    max_queued_connections: 128
    stream_chunk_bytes: 256K

streaming:
  enabled: true
  ffmpeg: ffmpeg
  ffprobe: ffprobe
  # temp_path: /var/lib/macha/tmp/playback
  max_sessions: 8
  max_video_transcodes: 1
  max_audio_transcodes: 4
  session_idle_ms: 1800000
  startup_timeout_ms: 10000
  segment_duration_ms: 4000
  max_ahead_segments: 8
```

`max_sessions` limits logical playback sessions. Video and audio transcodes have separate lower limits because direct play and remux sessions are much cheaper. `max_ahead_segments` bounds how far a transformed producer is allowed to run ahead of client segment demand; the FFmpeg process is paused and resumed without changing the playback session.

Changes to streaming limits and timing are reloaded by `SIGHUP`. Enabling/disabling streaming, changing the media-engine executables, or changing its temporary path requires a restart.

See [`macha.yaml.example`](../macha.yaml.example) for the complete example.

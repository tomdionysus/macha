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

`log_level` is one of `ALL`, `DEBUG`, `INFO`, `WARN` or `ERROR`; default `INFO`. `DEBUG` emits low-volume operational/performance diagnostics. `ALL` additionally enables per-object backend/object transfers, complete FUSE request/result traces, payload hashes and detailed extent/write traces; `ALL` is intentionally expensive and is not suitable for throughput measurements.

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

## Catalogue scanner

`catalogue.scanner.interval_ms` is the periodic safety scan interval. Committed namespace changes schedule a scan after `catalogue.scanner.rescan_debounce_ms` (default 10000 ms, valid 1000..600000). Further mutations reset that quiet-period timer, but `catalogue.scanner.rescan_max_delay_ms` (default 60000 ms, minimum 1000 and not less than `rescan_debounce_ms`) caps total deferral from the first unscanned mutation. Catalogue metadata written by the scanner itself is excluded from the namespace-content signature and does not cause a catalogue rescan.

## Streaming configuration

Streaming shares the catalogue HTTP listener. `streaming.enabled: true` therefore requires `catalogue.api.enabled: true`. Macha links `libavformat`, `libavcodec`, `libavutil`, `libswscale` and `libswresample` directly. It does not run the `ffmpeg` or `ffprobe` commands. Legacy `streaming.ffmpeg` and `streaming.ffprobe` keys from the first 0.7.0 build are accepted and ignored.

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
  # temp_path: /var/lib/macha/tmp/playback
  max_sessions: 8
  max_video_transcodes: 1
  max_audio_transcodes: 4
  session_idle_ms: 1800000
  startup_timeout_ms: 15000
  segment_duration_ms: 4000
  max_ahead_segments: 8
  segment_memory_bytes: 64M
  probe_bytes: 8M
  probe_analyze_duration_ms: 5000
  probe_timeout_ms: 20000
```

`max_sessions` limits logical playback sessions. Video and audio transcodes have separate lower limits because direct play and remux sessions are much cheaper. `max_ahead_segments` bounds how far the in-process producer may run ahead of client demand. For transformed finite media the VOD playlist itself is complete from first publication; only fragment materialisation is bounded. Producers block on the segment store and wake as the player requests later fragments; no process suspension is involved.

Generated init/media fragments are published to memory. `segment_memory_bytes` bounds resident generated-segment memory per session; sufficiently old consumed fragments may spill below `temp_path` and remain directly addressable. `temp_path` is therefore overflow storage, not the signalling mechanism between the media engine and HTTP server.

Source inspection is deliberately bounded because a probe may cause distributed extent reads. `probe_bytes` limits libavformat probing, `probe_analyze_duration_ms` limits media-time analysis, and `probe_timeout_ms` is the wall-clock guard for the complete media-representation inspection pass. All candidate media IDs share that deadline. It is propagated into Macha extent reads and outstanding remote object RPCs, so neither multiple representations nor a peer that stops making progress can multiply the session-creation delay. A timed-out probe fails with a stage-specific 503. Successfully probed immutable media IDs are cached.

`maintenance.no_progress_backoff_ms` controls the quiescent backoff used after repair, local rebalance or scrub makes no progress. The default is 30000 ms. This prevents a settled node from repeatedly walking hot metadata merely because its byte credit has reached one extent.

Changes to streaming session limits and timing are reloaded by `SIGHUP`. Enabling/disabling streaming, changing fragment-memory/probe policy or changing `temp_path` requires a restart.

See [`macha.yaml.example`](../macha.yaml.example) for the complete example.

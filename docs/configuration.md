# Installation and configuration

The complete example is `macha.yaml.example`. The important rules are short:

- Every node uses the **same key contents**. The path may differ.
- `network.advertise` must be reachable from the other nodes. `listen: 0.0.0.0` does not make `127.0.0.1` a useful advertised address.
- `dht.replicas` and `dht.metadata_replicas` are cluster policy and must be identical on every node. They may be changed by stopping the whole cluster, changing every node, then restarting it. Do not roll replica-count changes through a running cluster; mixed policies are unsupported.
- `dht.min_write_replicas` is cluster write policy and should be identical on every node. It is the degraded-mode durable floor for immutable extent writes (default `1`); healthy writes still use at least the normal replica quorum. `dht.write_stall_ms` (default `2500`) controls when a no-progress preferred PUT is hedged onto the next deterministic placement candidate. Desired replication is restored by background repair. Metadata continues to use its voter quorum.
- `dht.extent_size` is fixed for an existing namespace.
- Bootstrap may be one-way or symmetric. A node with configured bootstrap peers will not create a new namespace while none of them is reachable.
- Storage backend directories must already exist. Missing paths are treated as missing disks, not created automatically.
- `state_path` is not bulk storage. Put it on reliable local system storage.
- `failure_domain` describes shared fate. Machines in the same site should normally use the same value.

`log_level` is one of `ALL`, `DEBUG`, `INFO`, `WARN` or `ERROR`; default `INFO`. `DEBUG` emits low-volume operational/performance diagnostics. `ALL` additionally enables per-object backend/object transfers, complete FUSE request/result traces, payload hashes and detailed extent/write traces; `ALL` is intentionally expensive and is not suitable for throughput measurements.

`ffmpeg_log_level` is independent and defaults to `ERROR`. It accepts `QUIET`, `PANIC`, `FATAL`, `ERROR`, `WARN`, `INFO`, `VERBOSE`, `DEBUG` or `TRACE`. libav output is routed through Macha's logger rather than written directly to stderr. The two thresholds deliberately do not constrain one another: for example, `log_level: INFO` with `ffmpeg_log_level: DEBUG` shows detailed FFmpeg diagnostics while keeping Macha's own DEBUG messages suppressed.

Start with:

```sh
macha --config /etc/macha.yaml
```

Send `SIGHUP` to reload local storage-backend, cache, hydration and logging configuration. Replica-count changes require a coordinated cluster stop/edit/restart; the old metadata quorum commits the new voter/data policy when the cluster comes back. `extent_size` cannot change for an existing namespace.

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
--min-write-replicas N
--write-stall MS
--extent-size SIZE               1M..64M; compiled default 16M
--read-ahead N
--connect-timeout MS             TCP connection attempt only
--max-frame-size SIZE             4K..4M; default 256K
--control-stall-notice MS        DEBUG notice only; 0 disables
--data-stall-notice MS           DEBUG notice only; 0 disables
--metadata-cache MS
--mount PATH
--log-level LEVEL
--ffmpeg-log-level LEVEL
```

RPC duration itself is unbounded. Stall notices are observability thresholds; they do not cancel requests. Health/control traffic uses the same peer connection at absolute highest priority. A peer is marked dead only after that unified transport cannot establish liveness within `dead_after_ms`.

## Mounted filesystem

0.13.0 makes FUSE a bounded local frontend rather than a synchronous projection of distributed Macha. The kernel-facing layer keeps a stable inode graph, local write spool and ordered publication queues. No FUSE callback waits for metadata quorum, remote replica placement, checkpoint propagation, repair or catalogue work. When a request cannot be satisfied inside its configured deadline it fails to the kernel; accepted local work continues or is reconciled asynchronously where that is semantically valid.

FUSE policy is configured separately from persistent filesystem-root metadata:

```yaml
fuse:
  allow_other: false
  entry_timeout_ms: 1000
  attr_timeout_ms: 1000
  negative_timeout_ms: 500
  absolute_request_timeout_ms: 15000
  timeouts:
    lookup_ms: 1000
    namespace_ms: 3000
    read_ms: 10000
    write_ms: 5000
    sync_ms: 5000
    lifecycle_ms: 2000
  request_workers: 24
  max_pending_requests: 4096
  commit_workers: 8
  foreground_commit_workers: 1
  publication_quiet_ms: 5000
  max_pending_operations: 4096
  hydration_priority: 2000
  read_ahead_extents: 2
  hint_lifetime_ms: 5000
  write_through_cache: true
  fail_closed_mountpoint: true
  watchdog_interval_ms: 1000
```

The six operation-class timeout values must be positive and may not exceed `absolute_request_timeout_ms`; that ceiling is itself limited to 30000 ms. For read-only lookup/read work they remain hard cooperative deadlines. Mutating requests may be rejected with `ETIMEDOUT` while still queued, but once a write, namespace mutation, sync or lifecycle operation starts Macha waits for its real result instead of returning an ambiguous timeout while local side effects continue. `request_workers` is 6..256 so every class always has an independent execution lane. Queue saturation returns bounded backpressure (`EAGAIN`) rather than allowing a kernel request to wait indefinitely.

From 0.14.9, local FUSE mutation recovery uses `state_path/fuse-spool/operations.log` together with the per-inode spool files. Successful namespace mutation admission includes an fsynced operation record. Successful file writes include an fsynced byte-spool payload followed by an fsynced write descriptor; this is intentionally stronger and potentially slower than merely buffering the bytes in the OS page cache. `fsync` re-syncs this local state and requests publication, but cluster-wide quorum/replica convergence remains asynchronous. At startup Macha overlays pending journal state on the local committed metadata snapshot and resumes publication. A missing/short referenced spool or a non-empty `inode-*.spool` with no journal history causes FUSE frontend startup to fail rather than guessing whether the bytes were acknowledged. Therefore an upgrade from an older build should be performed after pending FUSE publication has drained; an old non-empty spool without 0.14.9 journal history cannot be reconstructed safely. The journal is compacted only when its durable pending-operation count reaches zero, so sustained non-converging FUSE work can grow it.

`commit_workers` is the maximum asynchronous extent-publication concurrency once the mount is genuinely quiet. While any writable FUSE handle remains open, or any FUSE activity occurred within `publication_quiet_ms`, only `foreground_commit_workers` publishers are admitted (default one). The default quiet period is five seconds so a short rsync inter-file gap cannot unleash the full publication fan-out. Final metadata commits are serialised even when several workers prepare immutable extents concurrently.

Reads combine the committed immutable manifest with pending local operations and carry the FUSE read deadline into remote extent retrieval. Kernel demand is also emitted as a high-priority `HydrationHintProvider` run into Macha's existing persistent-cache hydrator. Identical object fetches are already coalesced by `DistributedStore`, so FUSE demand and predictive hydration can share one transfer. `read_ahead_extents` controls the additional ordered hint window; it does not create a separate FUSE cache. With `write_through_cache: true`, extents produced by asynchronous FUSE publication are also inserted into the ordinary persistent block cache.

`fuse.refresh_interval_ms` was removed in 0.14.5 because namespace synchronisation is no longer polled. Existing configurations containing that key remain accepted and the key is ignored. Namespace-facing requests refresh lazily when their metadata generation is stale. `watchdog_interval_ms` remains a real timer because unexpected mount disappearance is external OS state rather than an in-process event.

`fail_closed_mountpoint` protects the covered directory after the mount is established. If FUSE/macFUSE disappears unexpectedly, the naked mountpoint remains non-writable so a continuing `rsync` fails instead of silently writing into the host directory. An independent OS mount-table watchdog requests Macha shutdown on mount loss. A deliberate clean unmount restores the original directory permissions.

Short `entry_timeout_ms`, `attr_timeout_ms` and `negative_timeout_ms` values are kernel namespace/attribute caches only; each may be zero and may not exceed 5000 ms. `kernel_cache` remains disabled. On macOS, namespace keys remain byte-preserving while lookup uses canonical-equivalence-insensitive matching and `readdir` emits decomposed (D-form) names required by macFUSE.

## Catalogue scanner

Catalogue work is now queued persistently. Namespace traversal is a hint source and reconciliation authority; provider lookup is performed by catalogue hint workers. The built-in source priorities are ingest 100, explicit/manual rescan 80, namespace mutation 50 and periodic scan 10. These are scheduler priorities rather than correctness levels: all hints use the same provider matching rules.


`catalogue.scanner.interval_ms` is the periodic safety-check interval. Committed namespace changes schedule a scan after `catalogue.scanner.rescan_debounce_ms` (default 10000 ms, valid 1000..600000). Further mutations reset that quiet-period timer, but `catalogue.scanner.rescan_max_delay_ms` (default 600000 ms, minimum 1000 and not less than `rescan_debounce_ms`) caps total deferral from the first unscanned mutation. Catalogue metadata written by the scanner itself is excluded from the namespace-content signature and does not cause a catalogue rescan. The last reconciled signature and next safety deadline are persisted in `state_path/catalogue/scanner.state`, so restart/coordinator election is not itself a scan trigger. A safety deadline first compares the authoritative namespace signature; an unchanged namespace advances the deadline without walking every provider root.

Online metadata-provider work is also bounded. `catalogue.scanner.max_provider_requests_per_scan` defaults to 32 (valid 1..10000). The scanner checks the budget between complete provider lookups rather than aborting a search/detail operation halfway through, then reconciles completed discoveries and schedules a continuation after `catalogue.scanner.provider_batch_delay_ms` (default 30000 ms, valid 1000..3600000). Semantic provider misses are cached in memory for the configured provider lifetime; network/HTTP failures are not negative-cached. Artwork byte downloads use the existing `max_artwork_bytes` limit and occur only for the bounded set of discoveries produced by the provider pass.

Scanner roots are provider properties, not a global list. `providers.movies.roots`, `providers.tv.roots` and `providers.music.roots` define independent parsing domains. Their metadata resolver settings are nested under the provider (`tmdb` for movies/TV and `musicbrainz` for music). 0.10.3 intentionally removes the old scanner-wide `roots` and top-level `providers.tmdb` / `providers.musicbrainz` layout.

## Streaming configuration

Streaming shares the catalogue HTTP listener. `streaming.enabled: true` therefore requires `catalogue.api.enabled: true`. Macha links `libavformat`, `libavcodec`, `libavutil`, `libswscale` and `libswresample` directly. It does not run the `ffmpeg` or `ffprobe` commands. Legacy `streaming.ffmpeg` and `streaming.ffprobe` keys from the first 0.7.0 build are accepted and ignored.

`catalogue.api.client_io_timeout_ms` bounds the lifetime of an incomplete request and blocking socket I/O performed by a catalogue HTTP worker. It defaults to 30000 ms and accepts 1000..300000 ms. Streaming response writes use the same socket timeout so a client that stops reading cannot retain a worker indefinitely.

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
    client_io_timeout_ms: 30000
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

`maintenance.no_progress_backoff_ms` controls the quiescent backoff after repair, local rebalance or GC reaches a settled pass. The default is 300000 ms (5 minutes). Metadata/catalogue control verification is still capped at a 30-second cadence. Proactive physical integrity scrub is separate: `maintenance.scrub_interval_ms` defaults to 2592000000 ms (30 days), and `scrub_fraction` (default `0.02`) controls its rate only while a scrub campaign is due. The next due time is persisted under `state_path/maintenance/scrub.next`; the first run after this policy is introduced schedules the first campaign one interval ahead rather than immediately scanning the complete store. Ordinary object reads still authenticate AES-GCM and verify the content SHA-256, and placement/replica repair is unaffected.

Changes to streaming session limits and timing are reloaded by `SIGHUP`. Enabling/disabling streaming, changing fragment-memory/probe policy or changing `temp_path` requires a restart.

See [`macha.yaml.example`](../macha.yaml.example) for the complete example.


### Ingest cleanup policy

`ingest.cleanup.delete_owned_source_on_clear` defaults to `true`. An owned source is staging controlled by Macha, currently a completed torrent payload. Successful owned sources remain present after namespace import and catalogue processing until the operator explicitly clears the terminal job; clear then removes the source.

`ingest.cleanup.delete_external_source_on_clear` defaults to `false`. Filesystem/USB sources are therefore preserved when their completed job is cleared unless the job explicitly requested `delete_source_on_clear: true`. When external deletion is enabled, Macha removes only source files recorded as successfully imported in that job and then prunes empty directories; unrelated/unrecognised files in the submitted tree are never removed by Clear.

`ingest.cleanup.delete_owned_source_on_cancel` defaults to `true`. Cancelling a torrent or another owned acquisition removes its partial/staging payload immediately. These policies may be reloaded for newly submitted jobs; each job persists its resolved `delete_source_on_clear` decision.

Generic ingest jobs expose copy progress followed by catalogue progress. After all namespace files are committed, a job enters `cataloguing`; it becomes `completed` once every associated catalogue hint is terminal, even when the terminal result contains `no_match` or `failed` entries. Use `POST /api/v1/ingest/jobs/{id}/clear` to remove a terminal job. Torrent jobs expose the same linked catalogue summary and use `POST /api/v1/torrents/jobs/{id}/clear`.

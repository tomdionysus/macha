# Configuration

Macha uses one YAML configuration file per node. Node-local paths/endpoints/capacities may differ. Cluster policy and the cluster key must agree across participating nodes.

The 0.18 storage schema is intentionally incompatible with the old top-level storage sequence. A configuration using `storage: [ ... ]` is rejected.

## Required identity and paths

```yaml
state_path: /var/lib/macha
key_file: /etc/macha.key
fuse:
  mount_path: /srv/media
```

`state_path` contains node identity, namespace metadata state, maintenance state and—unless overridden—the metadata control-object store. `key_file` must contain the same secret material on every node in a cluster. `fuse.mount_path` is optional; omit it to run without a FUSE mount.

A fresh state namespace is required. Non-empty unversioned state is refused.

```yaml
service_startup_no_progress_ms: 120000
service_startup_timeout_ms: 0
```

Startup is gated on **progress**, not elapsed time. Journal replay, metadata
materialisation and storage accounting each tick a process-wide progress
counter as they work; the process is terminated for its supervisor (systemd
`Restart=on-failure`) only when that counter has not moved for
`service_startup_no_progress_ms`. `service_startup_timeout_ms` is an optional
absolute ceiling on top and is off by default, so a five-minute replay on a
small node never turns into a crash loop.

## Runtime memory bound

```yaml
runtime:
  glibc_arena_max: 4
  retained_memory_bytes: 768M
  control_memory_reserve_bytes: 64M
  viewer_memory_reserve_bytes: 192M
  loader_memory_reserve_bytes: 64M
```

`retained_memory_bytes` is the process-wide admission budget for heap objects
which survive an asynchronous boundary, including FUSE write/operation state,
RPC queues and active playback fragment stores. The three reserves are
headroom within that total. Durable loader work cannot consume viewer or
control headroom; speculative work also preserves a loader floor.
Reconstructible caches may borrow otherwise idle capacity only when they can be
shed before higher-priority admission. These limits govern owned allocations,
not the allocator's RSS bookkeeping or the on-disk FUSE spool.

On Linux/glibc, Macha applies this process-wide allocator arena limit before it
creates service or codec threads. It prevents successive short-lived transcode
threads from creating an ever-growing set of retained arenas. The default is
four and the accepted range is 1–64. Increasing it may reduce allocator
contention on large machines at the cost of a larger retained-memory ceiling.
The setting is accepted but has no effect with non-glibc allocators, including
macOS. Changing it requires a process restart; live reload rejects a different
value rather than claiming to apply a limit after worker arenas already exist.

## Authoritative storage

```yaml
storage:
  data:
    backends:
      - path: /mnt/media-a/macha-data
        limit: 10T
        reserve_free: 4G
      - path: /mnt/media-b/macha-data
        limit: 10T
        reserve_free: 4G
    packing:
      threshold: 1M
      target_size: 64M

  metadata:
    # Omit path to use <state_path>/metadata-objects.
    path: /var/lib/macha/metadata-objects
    limit: 4G
    packing:
      threshold: 1M
      target_size: 64M
```

`storage.data.backends` is required and must be a sequence. Each backend requires `path` and non-zero `limit`.

`reserve_free` is physical free-space protection, not part of logical DHT capacity. DATA admission stops before crossing it.

Packing may be disabled by setting both `threshold` and `target_size` to zero. Otherwise `target_size` must be at least `threshold`.

The metadata store has an independent limit and never consumes DATA quota.

## DHT policy

```yaml
dht:
  replicas: 1
  metadata_min_write_replicas: 2
  min_write_replicas: 1
  write_stall_ms: 2500
  extent_size: 4M
  data_inflight_bytes: 128M
  data_viewer_reserve_bytes: 32M
  read_ahead: 3
  metadata_cache_ms: 250
  metadata_materialization_cache_bytes: 128M
  retention_check_batch_size: 2000
  retention_check_concurrency: 8
```

- `replicas`: desired converged authoritative DATA copies.
- `min_write_replicas`: durable DATA copies required before foreground publication; must be `<= replicas`.
- `metadata_min_write_replicas`: minimum distinct active nodes that must durably accept a namespace/control mutation before publication. Every node is metadata-capable; this is a write durability floor, not a voter count or convergence target. The legacy `metadata_replicas` key is accepted only for 0.18 migration and is translated to its former majority write floor.
- `write_stall_ms`: how long a stalled preferred DATA placement may block before deterministic fallback is attempted.
- `extent_size`: maximum ordinary file extent size. It is unrelated to small-object pack allocation.
- `data_inflight_bytes`: node-wide byte budget for blocking DATA object reads, writes, and transfers.
- `data_viewer_reserve_bytes`: non-borrowable headroom inside that budget for foreground playback and read-ahead. Loader and speculative work remain work-conserving within the rest of the budget, but cannot consume this reserve. It must be smaller than `data_inflight_bytes`, and the difference must fit at least one `extent_size` object.
- `metadata_materialization_cache_bytes`: bounded process-memory budget for decoded metadata snapshots and their immutable records. Historical payloads remain in `history.log` and are read on demand. The default is `128M`.
- `retention_check_batch_size`: extent IDs per `have_objects` presence-check RPC issued while planning a DATA retention claim (e.g. accepting a metadata publication). Must fit within `network.max_frame_size` (32 bytes/id plus a small header).
- `retention_check_concurrency`: maximum `have_objects` batches in flight at once, across all peers combined, from a single retention claim. Bounds fan-out against any one peer for an arbitrarily large publication.

Replica policy should be identical across the cluster and changed as a coordinated cluster operation. `metadata_min_write_replicas: 2` means any two active nodes, not two preselected nodes.

## Network

```yaml
network:
  listen: 0.0.0.0
  advertise: media-node-2.example.net
  port: 7437
  failure_domain: site-a
  heartbeat_ms: 5000
  dead_after_ms: 30000
  connect_timeout_ms: 2500
  max_frame_size: 256K
  control_no_progress_deadline_ms: 30000
  data_no_progress_deadline_ms: 0
```

`advertise` must be reachable by peers.

`control_no_progress_deadline_ms` bounds how long a synchronous control-lane
call may sit with no bytes moving in either direction before it is cancelled
and fails with a transient `RPC made no progress ...; cancelled for retry`
error. Callers retry under their own retry policy, so a peer that accepted a
request and then wedged costs one deadline per attempt rather than an
indefinite wait. `data_no_progress_deadline_ms` does the same for object
transfers and is off (`0`) by default because those already have their own
stall/spill handling. `failure_domain` should be identical for nodes that share the same physical/site failure boundary.

Bootstrap entries are discovery seeds:

```yaml
bootstrap:
  - media-node-1.example.net:7437
```

A configured joining node does not invent a separate namespace merely because its bootstrap peer is temporarily unreachable.

## FUSE

```yaml
fuse:
  allow_other: false
  spool_path: /var/spool/macha/fuse
  operation_journal_path: /var/lib/macha/fuse-operations.log
  publication_quantum_bytes: 32M
  publication_inflight_bytes: 256M
  publication_pipeline_bytes: 8M
  viewer_weight: 95
  loader_weight: 5
  max_spool_bytes: 16G
  spool_reserve_free: 2G
  unmount_if_mounted: true
  fail_closed_mountpoint: true
  watchdog_interval_ms: 1000
```

`unmount_if_mounted` controls startup recovery from an unclean daemon exit. When true, Macha inspects the mount table before starting cluster/storage services and removes a mount only if it is identified as Macha/FUSE. An unrelated filesystem at the configured mount path is a hard startup error.

When false, any existing Macha mount is a hard startup error.

`fail_closed_mountpoint` guards the host directory the mount covers. Before cluster and storage services start (the mount itself comes up only after them, typically 20-40 s later), Macha marks that directory immutable on Linux, the `chattr +i` flag, so nothing, root included, can create files in it while no mount is present. An rsync started 25 s after the daemon otherwise walks the bare directory and fills the host disk with files the mount then hides (52 GB on a shared host's root disk, 2026-09-07). The flag stays set after Macha stops; run `chattr -i` on the directory if it must ever be removed. On filesystems without the flag the mode bits are cleared instead, which does not stop root. Entries already present under the directory are logged as an error at startup and counted as `filesystem.mountpoint_stray_entries` in the status API; `filesystem.mountpoint_immutable` reports whether the flag is in place. To inspect or clean stray entries while Macha is mounted, bind-mount the host root elsewhere (`mount --bind / /mnt/rootview`) and look under the mount path there.

`spool_path` can contain the full accepted-but-not-yet-published write backlog and must be sized accordingly. `operation_journal_path` contains the ordered durable descriptors needed to interpret that spool. `max_spool_bytes` is configurable and defaults to 16 GiB. It is a bounded backlog budget rather than a logical `ENOSPC` point: writes burst at local-spool speed below 50% occupancy, pressure starts publication, and admission is progressively paced from measured completed-publication throughput until it matches that throughput by 90% occupancy. At the bound, writers sleep on publication/retirement events instead of polling or failing. A single write larger than the complete bound is rejected, and `spool_reserve_free` can still return `ENOSPC` to protect physical free space.

`max_pending_write_bytes` bounds copied FUSE write payloads before they enter
the disk spool (32 MiB by default). Admission waits before copying when this
budget is occupied, so the request-count limit cannot translate into an
unbounded heap commitment during a slow or saturated spool.

`max_operation_metadata_bytes` (64 MiB by default) bounds the conservative
heap charge for durable write/truncate descriptors, their checksum vectors,
and a simultaneously active publication snapshot. When full, new mutations
wake publication and wait for a real retirement event; acknowledged work is
never dropped.

Data publication is fairly time-sliced by bytes. A generation retains its
provisional writer and exact spool cursor after each
`publication_quantum_bytes` (32 MiB by default), returns to the loader queue,
and becomes visible only after its final metadata commit. The quantum must be
an extent-size multiple. `publication_inflight_bytes` (256 MiB by default) is
also a quantum multiple and bounds aggregate concurrently admitted publication
work. `publication_pipeline_bytes` bounds provisional extent puts concurrently
started for one file. When omitted it is two extents, capped at one quantum
(8 MiB with the documented 4 MiB extent configuration). An explicit value must
be an extent-size multiple no larger than a quantum or eight extents; lowering it reduces the
loader I/O which may already be in flight when viewer demand arrives, while
raising it can improve bulk-import throughput on higher-latency storage. These byte bounds work independently of
`commit_workers`. All FUSE reads and writes are loader/convenience traffic and
do not manufacture viewer demand. Genuine streaming reads receive relative
`viewer_weight` service while publication receives `loader_weight` service
(95:5 by default). The weights are positive relative values, not a static
bandwidth cap: either class borrows all unused capacity while the other is idle.
Under simultaneous demand, bounded loader bursts yield at 256 KiB spool chunk
boundaries and receive a proportional event-driven cooldown; loader progress is
never stopped indefinitely.

FUSE spool policy is fixed when the frontend starts; changing these values
requires a server restart. Status exposes current bytes, configured limit,
measured publication rate, its aggregate retirement window, cumulative throttle
waits, quantum/yield counts, and peak admitted publication bytes beneath
`diagnostics.filesystem`.
The same object reports pending/peak write-request bytes and fixed extent
executor worker, queue, active and peak counts.

### Retry budgets and parking

```yaml
fuse:
  publication_retry_max_failures: 100
  publication_retry_window_ms: 1800000
  publication_retry_initial_backoff_ms: 250
  publication_retry_max_backoff_ms: 30000
  namespace_retry_max_failures: 200
  namespace_retry_window_ms: 1800000
  namespace_retry_initial_backoff_ms: 50
  namespace_retry_max_backoff_ms: 5000
```

A data publication that fails with a transient error is re-queued after an
exponential backoff (`initial_backoff` doubling up to `max_backoff`); the
backoff is per inode, so other files keep publishing at full speed. When one
inode has failed `max_failures` times inside `window_ms` it is **parked**: its
bytes stay in the spool and journal, it leaves the loader queue, the daemon
logs one `WARN` line, `diagnostics.filesystem.parked_publications` counts it,
and `GET /api/v1/manage/filesystem/parked-publications` lists it with the
last error, attempt count and how long it has been failing. An operator
resolves it with `POST .../parked-publications/<inode>/retry` (fresh budget)
or `POST .../parked-publications/<inode>/abandon` (drops the unpublished
generation, exactly as a corrupt spool record would be dropped). Parking is
never applied to definitive failures — those are handled at once — nor to the
namespace queue, which is ordered and therefore cannot skip an entry: on
budget exhaustion it reports the blocking operation as `EAGAIN` in
`namespace_blocked_op` and keeps retrying at the ceiling backoff.

The remaining FUSE worker/timeout fields bound local kernel-facing work. They do not turn `fsync()` into a promise of cluster-wide convergence; accepted local state is made crash-recoverable first and distributed publication continues asynchronously.

## Streaming

```yaml
streaming:
  enabled: true
  max_sessions: 8
  max_video_transcodes: 1
  max_audio_transcodes: 4
  video_decoder_threads: 2
  video_encoder_threads: 0
  pipeline_idle_ms: 60000
  session_idle_ms: 1800000
```

`pipeline_idle_ms` releases an abandoned physical remux/transcode encoder after
valid current-generation stream requests stop (60 seconds by default). The
logical session remains reconcilable until `session_idle_ms`; invalid or stale
generation retries do not renew the physical lease. This bounds leaked
transcode capacity after a client disappears on an unreliable network without
shortening the logical session lifetime.

`video_encoder_threads` sets the x264 frame-thread count per video transcode; `0` (the default) uses every hardware thread. Until 0.32.11 the encoder ran single-file in sliced-thread mode, at about real time for 1080p on the four-core nodes, so every representation change cost 5-13 s and a mid-file seek could not catch up. The first fragment of a transcode generation is 2 s (later ones the configured segment duration) so the request is answered after 2 s of encoding.

`video_decoder_threads` bounds decoder parallelism per transformed video in the
range 1–16. It defaults to two so viewer work can use otherwise-idle CPU without
allowing libav to choose an unbounded automatic value. Together with
`max_video_transcodes`, it also bounds the process-wide requested decoder thread
count. Changing it requires a process restart.

## Cache

```yaml
cache:
  path: /var/cache/macha
  max_blocks: 4096
  prefer_metadata: true
```

Cache is best-effort and never satisfies DATA or metadata durability.

## Maintenance

```yaml
maintenance:
  interval_ms: 1000
  foreground_quiet_ms: 2000
  busy_bandwidth_fraction: 0.0
  idle_bandwidth_fraction: 0.10
  cpu_target: 0.10
  background_concurrency: 0
  initial_bandwidth: 32M
  max_bandwidth: 0B
  scrub_fraction: 0.02
  scrub_interval_ms: 2592000000
  no_progress_backoff_ms: 300000
  garbage_grace_ms: 86400000
```

`background_concurrency` is the node's background effort ceiling: how many loader or speculative DATA operations (publication and repair, one extent each) may hold an admission lease at once. `0` means half the hardware threads, minimum 1. Viewer work is never counted, so playback and read-ahead are unaffected; what it bounds is how much CPU (hashing, encryption) and I/O the node spends on its own import and repair traffic at once. The status API reports the ceiling and its use as `data_resources.background_limit` / `background_active` / `peak_background_active`.


Maintenance performs DATA repair/rebalance/GC/scrub and catalogue control convergence/GC. Foreground media and mounted MachaDFS activity take priority.

## Catalogue

Catalogue scanner/API configuration is optional. Scanner roots are provider-specific:

```yaml
catalogue:
  scanner:
    enabled: true
    providers:
      movies:
        enabled: true
        roots: [/Movies]
        tmdb:
          enabled: true
          token_file: /etc/macha-tmdb.token
      tv:
        enabled: true
        roots: [/TV]
        tmdb:
          enabled: true
          token_file: /etc/macha-tmdb.token
      music:
        enabled: true
        roots: [/Music]
        musicbrainz:
          enabled: true
          contact: https://example.invalid/macha
```

Artwork fetched by providers is ordinary DATA. Catalogue manifest/shards are CONTROL.

## Streaming, ingest and acquisition

Streaming, ingest and BitTorrent configuration remain independent of the storage authority model. The complete set of fields is shown in [`../macha.yaml.example`](../macha.yaml.example).

## Web client

`web.root` names a directory of built web-client assets to serve at the server's root. It is unset by default, and a node with no `web.root` answers non-API paths exactly as before: `404`.

```yaml
web:
  enabled: true
  root: /var/lib/macha/web
  index: index.html
```

| field | meaning |
|---|---|
| `enabled` | `true` by default; set `false` to stop serving a client whose `root` is still configured. |
| `root` | Directory of built assets. Only this directory is served. |
| `index` | The document served for any path that is not a file under `root`. Must be a file name inside `root`, not a path. Defaults to `index.html`. |

A path that names a file under `root` is served as that file, with a content type from its extension, an `ETag`, and `Cache-Control: public, max-age=3600`. Every other path is answered with the index document under `Cache-Control: no-cache`, so a deep link like `/library/artist/x` reaches the client, which resolves the route itself once it has loaded. The index is revalidated on every load because it names the current asset bundle; without that a deploy stays invisible until the browser decides otherwise.

**The API namespace is never served from here.** Everything under `/api` — not merely `/api/v1` — remains the server's, including its `404`s: a client asking for an endpoint that does not exist is told so in JSON rather than handed an HTML page its parser will choke on. Reserving the whole prefix means a later API version cannot be silently swallowed by the client's fallback.

Client assets are served without a bearer token, since a browser has none until the client has loaded and asked for one. `root` must therefore contain only material meant to be public. Nothing outside it is reachable: request paths are checked one segment at a time and a segment that is empty, `.`, `..`, or begins with a dot is refused, so a request can neither climb out of the root nor read build leftovers such as `.env` or `.git`. A refused path falls through to the index rather than to a `404`, so probing cannot be used to learn whether a file exists.

The client is served while local services are still recovering, because it is static files and depends on none of them. That is deliberate: the client loads and shows what `/api/v1/status` reports, rather than failing to load at all during a recovery.

A node configured with a `root` that does not exist, or one with no index document, answers `503 web_client_unavailable` rather than `404` — a misconfigured node says so instead of pretending the route was never there, and starts serving as soon as the files appear, without a restart.

## Logging

`log_level` accepts `ALL`, `DEBUG`, `INFO`, `WARN`, or `ERROR`. `ffmpeg_log_level` is an independent libav threshold and can be more verbose than the Macha application threshold.

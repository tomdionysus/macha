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
  hosts_extents: auto    # true | false | auto
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

`hosts_extents` says whether this node is ever a DATA placement owner or
fallback holder. `false` is the edge node: it serves the API and media to its
own network from its block cache, publishes writes to the owners, and stores
no extents at all, so `storage.data` may be omitted entirely. `auto` (the
default) resolves to `false` when no data backends are configured or the node
turns out to accept no inbound connections (see `network.inbound_capable`),
and to `true` otherwise. The resolved value is gossiped, so every peer makes
the same placement decision. A node that stops hosting drains its existing
copies to the owners through ordinary repair.

`storage.data.backends` must be a sequence when present, and is required when
`hosts_extents` is `true`. Each backend requires `path` and non-zero `limit`.
`hosts_extents: false` with backends configured is accepted with a warning:
they will drain and stay empty. The metadata store remains required on every
node.

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
  inbound_capable: auto    # true | false | auto
  failure_domain: site-a
  heartbeat_ms: 5000
  telemetry_interval_ms: 10000
  dead_after_ms: 30000
  connect_timeout_ms: 2500
  max_frame_size: 256K
  control_no_progress_deadline_ms: 30000
  data_no_progress_deadline_ms: 0
```

`advertise` must be reachable by peers, unless the node accepts no inbound
connections at all.

`inbound_capable` says whether peers can connect *to* this node. `false` is
the node behind CGNAT, a corporate NAT, or any network where exposing a port
is not permitted: it connects out to its peers, they answer over the sessions
it opened, and they never dial its advertised endpoint (which is then only a
routing key; `advertise`, `upnp`, `external_ip` and `connectivity_check` are
irrelevant and warned about). When such a peer needs a lane the node has not
opened, it asks over the CONTROL session and the node dials it. `auto` (the
default) is decided by asking a peer to dial back: two consecutive failures
resolve `false`, one success resolves `true`; the answer is persisted under
`state_path/connectivity/` so a restart is not a placement event, and it is
re-checked every 10 minutes while `false` and every hour while `true`. A
founding node (no `bootstrap`) with `inbound_capable: false` is refused at
start-up: nothing could ever join it. Every transport socket also keeps TCP
keepalive probing at 60 s, which keeps NAT mappings warm for any NAT'd node.

`telemetry_interval_ms` is how often a node gossips its telemetry set to its
peers, and therefore how stale another node's view of it can be in
`/api/v1/status` (`nodes[].live_age_ms` reports the actual age). Lowering it
makes peer figures track a busy cluster more closely at proportionally more
control-lane traffic; a set is roughly 200 bytes per node it carries, capped
at 64 nodes. A readiness transition or a peer observation publishes sooner
than the configured cadence, but never more than once per second, so a
reconnecting peer cannot turn this into a send loop.

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
  publication_no_progress_deadline_ms: 30000
  publication_inflight_bytes: 256M
  publication_pipeline_bytes: 8M
  publication_max_open_writers: 0
  viewer_weight: 95
  loader_weight: 5
  max_spool_bytes: 16G
  spool_reserve_free: 2G
  unmount_if_mounted: true
  fail_closed_mountpoint: true
  watchdog_interval_ms: 1000
  initial_namespace_timeout_ms: 600000
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
`publication_no_progress_deadline_ms` bounds a publication worker blocked on
retained-memory admission. It is a no-progress budget rather than a time limit
on publishing: any worker in the pipeline completing a quantum re-arms it, so a
slow node is never failed for being slow, while a pipeline where nothing at all
advances fails with `EAGAIN` and enters the ordinary publication retry/park
path instead of waiting forever. `0` restores the old unbounded wait.

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
`commit_workers`.

`publication_max_open_writers` bounds how many files may hold a provisional
writer at once. A writer is retained across clean yields and retryable failures
so a resumed publication never replays spool bytes, and it keeps its
retained-memory extent leases -- one filling buffer plus the pipeline -- for as
long as it is retained. Publication scheduling is otherwise breadth-first, so
without this bound the number of writers holding partial state is simply the
width of the backlog: on one node that reached 123 leases, the entire
durable-lower budget, after which every writer needed one more extent and none
could release one. No byte budget fixes that, because any budget fills the same
way. When omitted the bound is derived so that the open set's worst case fits
`runtime.loader_memory_reserve_bytes` -- `loader_memory_reserve_bytes /
(extent_size + publication_pipeline_bytes)`, never fewer than `commit_workers`.
The bound is soft: the scheduler tests the count before selecting an inode and
the worker opens the writer afterwards, so concurrent workers can overshoot it
by up to `commit_workers - 1` (9 against a bound of 8 was observed live). Size
the reserve with that headroom in mind, and read `peak_open_publications`
rather than assuming the configured value was never exceeded.
Past the bound the scheduler is depth-first over the already-open set, which is
what drains a backlog anyway. Status reports `open_publications`,
`peak_open_publications`, `publication_max_open_writers` and
`data_publication_selections_under_writer_cap`;
`data_publication_progress_events` counts what actually releases publication
memory (extent retirements and commits) and is the field to read when asking
whether a pipeline is moving at all. All FUSE reads and writes are loader/convenience traffic and
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
  session_unused_idle_ms: 120000
  segment_duration_ms: 4000
  max_ahead_segments: 8
  segment_hold_window: 8
  segment_memory_bytes: 67108864
  max_session_holds: 2
  max_concurrent_holds: 64
```

`max_ahead_segments` (8) is how far beyond the highest fragment index a client
has actually requested the producer is allowed to run before it parks on a
condition variable, and `segment_duration_ms` (4000) is the target length of
one fragment. Their product is the node's look-ahead: the distance past the
last fragment a client asked for at which a viewer may arrive and still find
media already produced. Arriving beyond it is not an error, but the fragment
does not exist yet and production is sequential, so the node has to encode its
way there at roughly real time while the viewer waits.

`segment_hold_window` (8) is deliberately the same distance. A request inside
it is one production is authorised to reach, so it is held; a request outside
it is one nothing is working toward, so it is refused at once with a retryable
`500 segment_not_ready` and `Retry-After: 1` rather than occupying a hold.
A held request that reaches `segment_timeout` without the fragment arriving is
refused the same way, with `reason=hold_timed_out` in the node log.

**A client must not hardcode these two numbers.** `stream.look_ahead_ms` on
the playback session payload reports their product for that session, so a
client bounds itself against the node it is actually talking to; see
`docs/streaming.md`. A client that assumed the defaults against a node
configured with `max_ahead_segments: 4` would believe it had 32 seconds of
authorised production ahead of the frontier when it had 16.

`segment_memory_bytes` (64 MiB) bounds resident generated fragments; older
consumed fragments spill below `temp_path` rather than stalling production.
`max_session_holds` (2, one in flight plus one prefetch) and
`max_concurrent_holds` (64) bound held requests per session and node-wide.

`pipeline_idle_ms` releases an abandoned physical remux/transcode encoder after
valid current-generation stream requests stop (60 seconds by default). The
logical session remains reconcilable until `session_idle_ms`; invalid or stale
generation retries do not renew the physical lease. This bounds leaked
transcode capacity after a client disappears on an unreliable network without
shortening the logical session lifetime.

`session_unused_idle_ms` (120 seconds by default) is the expiry for a session
that has **never** served a stream object — no playlist, no fragment, no
subtitle, no Direct Play body. The transcode entitlement belongs to the
session rather than to the pipeline, so reclaiming an idle encoder does not
release it: until the session itself is erased the slot stays taken, and with
`max_video_transcodes: 1` a session created and never used closes the node to
transcoding for the whole of `session_idle_ms`. A client that crashes, is
force-quit, loses power or is suspended with its closing `DELETE` unsent
cannot release it, so this is bounded on the server. The condition is "never
used", not "not used recently": a single stream request earns the full
`session_idle_ms` permanently, so a paused or seeking player is never evicted
by this clock, and both clocks measure from the session's last interaction of
any kind, so a client that is still polling or PATCHing is safe as well. The
effective value is the lesser of this and `session_idle_ms`, so lowering
`session_idle_ms` alone is safe. `GET /api/v1/playback/status` reports
`session_unused_idle_ms` and the cumulative `unused_sessions_reclaimed`.

`video_encoder_threads` sets the x264 frame-thread count per video transcode; `0` (the default) uses every hardware thread. Until 0.32.11 the encoder ran single-file in sliced-thread mode, at about real time for 1080p on the four-core nodes, so every representation change cost 5-13 s and a mid-file seek could not catch up. The first fragment of a transcode generation is 2 s (later ones the configured segment duration) so the request is answered after 2 s of encoding.

A seek is never moved to suit the node's configuration. A transcode generation
begins exactly where the client asked; a remux generation begins at the last
indexed keyframe at or before the request and publishes the remainder as
`seek_offset_ms`, so `segment_duration_ms` and the source's GOP change how much
pre-roll a client fetches but never which content a generation contains. See
"Where a seek actually starts" in `docs/streaming.md` for the invariant.

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

### API server

```yaml
catalogue:
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7438
    workers: 16
    control_workers: 2
    max_connections: 1024
    max_queued_requests: 256
    client_io_timeout_ms: 30000
    stream_chunk_bytes: 256K
    staging_chunks: 2
    keep_alive_max_requests: 100
    keep_alive_idle_timeout_ms: 15000
    slow_request_threshold_ms: 1000
    reactor_stall_threshold_ms: 50
    compression: true
    compression_min_bytes: 1K
    compression_level: 6
    compression_max_asset_bytes: 4M
```

The server is one reactor thread that owns every socket, plus two pools of threads that only compute (see `docs/streaming.md`, "Public HTTP behaviour"). `workers` is the data lane: catalogue, playback, web assets, and every body read that can block on a disk or a replica. `control_workers` is the control lane: health, status, session and account routes, so they are answered while the data lane is saturated. `max_connections` bounds open connections; an idle kept-alive connection is a descriptor and a small struct, not a thread. `max_queued_requests` bounds how many requests may wait for a lane worker before the reactor answers `503 overloaded` with `Retry-After: 1`. `staging_chunks` is how many `stream_chunk_bytes` chunks a streaming response may hold ahead of a slow client, so streaming memory is at most connections × `staging_chunks` × `stream_chunk_bytes`. A handler slower than `slow_request_threshold_ms` is logged with its route; a reactor pass longer than `reactor_stall_threshold_ms` is counted in diagnostics as a stall, which should never happen.

`compression` gzips text responses on the way out: JSON from the API, and the web client's HTML, CSS and JavaScript. It applies only to complete in-memory bodies above `compression_min_bytes`, and only for a client whose `Accept-Encoding` asks for it. Media is never compressed — it is already compressed, it is streamed rather than buffered, and the reactor sends it from resident memory without a copy. Neither are images, fonts or wasm, for the same reason. `compression_level` is the zlib level, 1 to 9; 1 gives most of the ratio for a fraction of the CPU, which is what a Pi-class node wants. Every compressible response carries `Vary: Accept-Encoding` whether or not it was compressed, so a shared cache keys the two representations apart.

For the web client, a precompressed file sitting next to the asset (`app.js.gz` beside `app.js`) is preferred and costs no CPU per request. When the client build did not produce one, an asset up to `compression_max_asset_bytes` is compressed on demand instead; larger ones are streamed unchanged. The compressed and uncompressed forms of an asset never share an entity tag, so revalidation cannot return the wrong one.

Set `compression: false` on a node that sits behind a proxy which already compresses. That is a supported deployment, not a degraded one, and the counters `responses_compressed` and `compression_bytes_saved` in the diagnostics route say what the setting is actually doing.

`max_queued_connections` (pre-0.43.0) is still read, as `max_connections`.

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

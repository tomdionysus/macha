# Configuration

Macha uses one YAML configuration file per node. Node-local paths/endpoints/capacities may differ. Cluster policy and the cluster key must agree across participating nodes.

The 0.18 storage schema is intentionally incompatible with the old top-level storage sequence. A configuration using `storage: [ ... ]` is rejected.

## Required identity and paths

```yaml
state_path: /var/lib/macha
key_file: /etc/macha.key
mount_path: /srv/media
```

`state_path` contains node identity, namespace metadata state, maintenance state and—unless overridden—the metadata control-object store. `key_file` must contain the same secret material on every node in a cluster.

A fresh state namespace is required. Non-empty unversioned state is refused.

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
  read_ahead: 3
  metadata_cache_ms: 250
```

- `replicas`: desired converged authoritative DATA copies.
- `min_write_replicas`: durable DATA copies required before foreground publication; must be `<= replicas`.
- `metadata_min_write_replicas`: minimum distinct active nodes that must durably accept a namespace/control mutation before publication. Every node is metadata-capable; this is a write durability floor, not a voter count or convergence target. The legacy `metadata_replicas` key is accepted only for 0.18 migration and is translated to its former majority write floor.
- `write_stall_ms`: how long a stalled preferred DATA placement may block before deterministic fallback is attempted.
- `extent_size`: maximum ordinary file extent size. It is unrelated to small-object pack allocation.

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
```

`advertise` must be reachable by peers. `failure_domain` should be identical for nodes that share the same physical/site failure boundary.

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
  max_spool_bytes: 16G
  spool_reserve_free: 2G
  unmount_if_mounted: true
  fail_closed_mountpoint: true
  watchdog_interval_ms: 1000
```

`unmount_if_mounted` controls startup recovery from an unclean daemon exit. When true, Macha inspects the mount table before starting cluster/storage services and removes a mount only if it is identified as Macha/FUSE. An unrelated filesystem at the configured mount path is a hard startup error.

When false, any existing Macha mount is a hard startup error.

`spool_path` can contain the full accepted-but-not-yet-published write backlog and must be sized accordingly. `operation_journal_path` contains the ordered durable descriptors needed to interpret that spool. `max_spool_bytes` is configurable and defaults to 16 GiB. It is a bounded backlog budget rather than a logical `ENOSPC` point: writes burst at local-spool speed below 50% occupancy, pressure starts publication, and admission is progressively paced from measured completed-publication throughput until it matches that throughput by 90% occupancy. At the bound, writers sleep on publication/retirement events instead of polling or failing. A single write larger than the complete bound is rejected, and `spool_reserve_free` can still return `ENOSPC` to protect physical free space.

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
`commit_workers`. Viewer demand prevents admission of a
new quantum and an already running publisher checks the viewer gate between
256 KiB spool chunks.

FUSE spool policy is fixed when the frontend starts; changing these values
requires a server restart. Status exposes current bytes, configured limit,
measured publication rate, its aggregate retirement window, cumulative throttle
waits, quantum/yield counts, and peak admitted publication bytes beneath
`diagnostics.filesystem`.

The remaining FUSE worker/timeout fields bound local kernel-facing work. They do not turn `fsync()` into a promise of cluster-wide convergence; accepted local state is made crash-recoverable first and distributed publication continues asynchronously.

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
  initial_bandwidth: 32M
  max_bandwidth: 0B
  scrub_fraction: 0.02
  scrub_interval_ms: 2592000000
  no_progress_backoff_ms: 300000
  garbage_grace_ms: 86400000
```

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

## Logging

`log_level` accepts `ALL`, `DEBUG`, `INFO`, `WARN`, or `ERROR`. `ffmpeg_log_level` is an independent libav threshold and can be more verbose than the Macha application threshold.

# Operations

## Starting a fresh cluster

Create the configured state, DATA backend, cache and FUSE spool parent directories on the intended filesystems. Do not point Macha at a populated unversioned state/backend: the storage contract deliberately refuses it.

Copy the same cluster key to every node. Configure reachable advertised addresses and consistent DHT policy. Start at least `dht.metadata_min_write_replicas` nodes before expecting namespace/catalogue mutations to commit.

## Capacity

DATA capacity is a cluster property derived from eligible node capacities and the desired replica count. For R=1, heterogeneous node capacities aggregate; a small node does not define the whole cluster's capacity. For larger R, the same bytes must fit on multiple distinct owners/failure domains, so logical capacity is necessarily lower.

Each local backend also has two independent admission limits:

- configured DATA `limit`;
- physical `reserve_free` that must remain available on the underlying filesystem.

CONTROL/METADATA uses a separate store/quota and must be monitored independently.

## When a DATA backend fills

A full backend stops admitting new DATA. Local `StoragePool` tries another ranked backend if configured. Distributed placement tries another ranked node when necessary. Existing objects remain readable.

Do not increase metadata/control quota merely to work around a full DATA disk; those are intentionally different resources.

## Metadata write-floor loss

With fewer than `metadata_min_write_replicas` active nodes, namespace/catalogue mutations cannot publish. Nodes may continue serving a persisted/readable snapshot where the operation supports it.

Catalogue scanner infrastructure failures are deferred rather than counted as semantic provider failures. Once the write floor is available, work resumes.

## Maintenance

Maintenance is bounded and low priority. It performs:

- missing DATA replica repair;
- local backend rebalance toward preferred placement;
- DATA reachability GC;
- catalogue control-object convergence and CONTROL GC;
- scheduled physical integrity scrub.

A complete no-progress pass backs off instead of repeatedly scanning a settled store. Foreground playback and mounted MachaDFS activity suppress speculative work according to policy.

## Garbage collection

Reachability from committed metadata is authority. Unreferenced objects are not immediately deleted: they must age past `maintenance.garbage_grace_ms`. This protects failed publications and convergence lag.

Namespace deletion and physical reclamation are deliberately separate. Once a
namespace batch is durably accepted and its operation journal is confirmed, the
FUSE operation can complete without waiting for DATA objects to be unlinked.
Physical GC later walks a persistent local-store cursor in slices of at most 64
objects. A slice yields as soon as playback or mounted-filesystem activity
appears; destructive work is also fenced on complete cluster reachability,
stable metadata, catalogue liveness, retention claims, and the configured grace
period. A completed no-progress sweep parks until a real event or an exact grace
deadline rather than polling the settled object store.

DATA and CONTROL live sets are separate. Packed dead records become reclaimable bytes and are removed by pack compaction.

## Pack recovery

Pack indexes are not authoritative files. Startup scans pack records to reconstruct the index. A torn final record is truncated. If a compaction crash leaves both old and replacement packs, later replacement records win and old duplicate bytes can be reclaimed.

Unexpected authenticated-record corruption is an integrity fault and should be investigated rather than skipped.

## FUSE restart recovery

The local spool and operation journal are the authority for FUSE mutations that were acknowledged locally but not yet observed committed.

If the previous process died while mounted, the OS may retain a stale FUSE mount. With:

```yaml
fuse:
  unmount_if_mounted: true
```

startup removes only a mount identified as Macha before any service startup, verifies it disappeared, then proceeds. A different filesystem mounted at that path is never automatically removed.

Preserve the FUSE spool/journal when diagnosing recovery errors. A missing journal-referenced byte range is a data-safety fault.

FUSE spool pressure is normal backpressure, not a capacity fault. Below half of
`fuse.max_spool_bytes` a copy may burst at local disk speed. Above that point
the frontend publishes durable prefixes and progressively paces new admission
against measured end-to-end publication throughput; at the configured bound it
waits for a real publication/retirement notification. Inspect
`diagnostics.filesystem.spool_bytes`, `spool_limit_bytes`,
`spool_publish_rate_bytes_per_second`, `spool_throttle_waits`, and
`spool_throttle_wait_ms`. If publishing is stalled, writers intentionally stay
blocked and consume no polling loop; physical `spool_reserve_free` exhaustion
remains an `ENOSPC` safety condition.

Large-file publication is resumable rather than an indivisible worker job.
Inspect `data_publication_quanta`, `data_publication_yields`, and
`data_publication_peak_inflight_bytes` under `diagnostics.filesystem`. The
configured per-file bound and observed high-water mark are exposed as
`data_publication_pipeline_limit_bytes` and
`data_publication_peak_pipeline_extents`. A healthy
bulk import normally has more quanta than completed generations, useful-byte
counters that do not repeatedly read the same prefixes, and a peak no greater
than `fuse.publication_inflight_bytes`. Metadata visibility remains whole-file:
a yielded provisional generation is not visible to viewers.

Recovery publication is event-driven. The worker drains compatible operations
in bounded ordered batches (by default at most 256 operations and 256 KiB of
encoded operation data), publishes the largest valid prefix, and groups journal
durability markers. It does not need a periodic maintenance scan. Rename and
unsafe mixed-operation dependency chains remain singleton boundaries until
their durable batch identity is defined and crash-tested.

## Memory monitoring

Metadata mutation is designed to avoid namespace-sized copy amplification, but memory is still an operational signal. On Linux, a useful sampler is:

```bash
while PID=$(pgrep -xo macha); do
  printf '%s ' "$(date '+%H:%M:%S')"
  awk '
    /^VmSize:/  {printf "vmsize=%s%s ",$2,$3}
    /^VmRSS:/   {printf "rss=%s%s ",$2,$3}
    /^RssAnon:/ {printf "anon=%s%s ",$2,$3}
    /^VmSwap:/  {printf "swap=%s%s ",$2,$3}
    END {print ""}
  ' /proc/$PID/status
  sleep 1
done
```

Repeated recovery/catalogue mutation should not produce monotonic namespace-sized growth.

## Diagnostics

`DEBUG` logs show placement, metadata write-floor, catalogue and maintenance state without the per-object volume of `ALL`. `ALL` is intended for targeted tracing and can be expensive on active systems.

A DATA failure should be diagnosed as placement/admission/durability; a metadata failure as metadata/control write-floor durability. Keeping those failure domains distinct is intentional and should be preserved in logs and tooling.

`GET /api/v1/status` includes local, process-lifetime aggregate diagnostics
under `diagnostics`:

- `metadata` reports historical reconstruction/cache totals and accepted-head
  persistence writes, encoded bytes, and failures;
- `rpc_server` reports current metadata queue jobs/bytes, active and rejected
  jobs, plus request count, total/max queue wait, and total/max handler time in
  microseconds, grouped by wire message and frame class;
- `filesystem` reports lock-free FUSE operation, publication, DATA durability,
  and operation-journal append/barrier totals when this process owns a mounted
  frontend; and
- `convergence` reports semantic events, scheduled/completed runs, requested and
  completed epochs, the diagnostic generation high-water mark, and whether a
  run is currently scheduled.

These are bounded counters, not a request history. Reading them does not start
a sampler, publish metadata, or add gossip traffic. Derive an interval rate or
average from differences between two status samples; a process restart resets
the totals. The filesystem snapshot reads only atomics in O(1); it does not call
the fuller frontend status path which can inspect live inode state. A rejected
metadata request increments the rejection counter but is not counted as
executed handler work.

`filesystem.available` is false on a process without the mounted frontend and
during mount startup/teardown. `convergence.available` is true as soon as the
Service exists, including while storage recovery is still in progress. An equal
`runs_scheduled`/`runs_completed`, equal requested/completed epoch, and
`scheduled: false` describe a drained edge-triggered scheduler.

Operation-journal barrier totals describe successful journal appends, not every
filesystem barrier in the process. A new live inode normally records its inode
descriptor and operation admission before returning, then `published` and
`done`: four journal append barriers. Restart recovery begins after admission is
already durable, so a one-operation recovered publication records only the
grouped `published` and `done` barriers.

## RPC execution isolation

The fast-control executor has a deliberately closed allow-list: only CONTROL-frame
`ping` and `members` requests may run there. These handlers must remain bounded,
in-memory operations: they may read already-published state, but must not perform
filesystem access, durability barriers, network fan-out, metadata reconstruction,
or other work whose latency depends on backlog size.

Adding a message to the fast-control allow-list requires a test which blocks the
ordinary and metadata executors and proves the handler completes from published
memory alone. Metadata mutations (`put_metadata_history_entry`,
`put_metadata_commit`, and `accept_metadata_commit`) always use the bounded,
dedicated metadata executor. All other CONTROL messages use the ordinary control
executor; object work remains on the priority-aware DATA executors.

DATA execution priority is viewer foreground, viewer read-ahead, user loader,
then speculative maintenance. Durable FUSE spool publication uses the loader
class even when its journal records were reconstructed after restart. Recovery
provenance affects replay validation and cache policy, not scheduling priority.

## Cluster status and telemetry

`GET /api/v1/status` merges two deliberately different telemetry planes:

- authoritative in-memory cluster membership for node identity, liveness, endpoint and durable storage state;
- optional ephemeral authenticated peer telemetry for runtime/load/cache detail;
- a bounded coalesced last-known telemetry cache persisted independently on each node.

Telemetry never defines cluster membership or metadata durability. Ephemeral
telemetry is gossiped with boot-incarnation and sequence ordering and is not
journalled as a metrics history. An authenticated peer connection triggers a
coalesced telemetry dissemination event; periodic local metrics sampling also
provides bounded eventual retry. Delivery is best-effort and may be dropped
under useful load. Received notifications enter the bounded speculative RPC
executor, so decoding and telemetry-store mutation do not occupy socket-reader
threads. The last-known cache is periodically replaced only after a long
interactive-idle interval and never mutates the MachaDFS namespace or enters
metadata publication. The API marks observations as live, stale, unavailable,
or last-known so an online node cannot disappear merely because optional
telemetry was dropped.

Storage and cache byte objects include an `available` boolean. When coherent
telemetry is unavailable, Status may still report membership-known storage
capacity, but `used_bytes` and `free_bytes` are `null`; cache byte fields and
`storage_backends_online` are also `null`. Cluster storage/cache aggregates are
unavailable if any included node lacks a measurement, rather than treating the
missing node as zero usage. A genuinely empty measured disk is distinct:
`available` is true and `used_bytes` is the numeric value `0`.

Cluster capacity distinguishes known durable/cache capacity from the portion
currently online. One Status endpoint reports every known node from local
membership plus the telemetry already disseminated over authenticated cluster
connections; the HTTP request never calls peers, and clients do not need to
fan out. Per-node status reports authoritative membership endpoint/storage
fields and enriches them with telemetry when available. Metadata availability
is owned and published by `MetadataManager` as exactly `unavailable`,
`read-only`, or `writable`; Status consumes that state and may demote a
previously writable view immediately if fewer than
`metadata_min_write_replicas` active replicas remain, but never promotes to
writable merely from peer connectivity.

Metadata availability logging is transition-only and canonical, for example `metadata availability changed state=writable previous=read-only reason="metadata write durability floor available"`. Routine negative checkpoint acknowledgements are silent because they are normal convergence decisions; transport/checkpoint exceptions remain diagnostic.

`POST /api/v1/status/connectivity/check` and the node-specific equivalent perform diagnostic connectivity checks without changing cluster configuration. State-changing administrative operations belong under `/api/v1/manage`.

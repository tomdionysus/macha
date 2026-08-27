# Operations

## Starting a fresh cluster

Create the configured state, DATA backend, cache and FUSE spool parent directories on the intended filesystems. Do not point Macha at a populated unversioned state/backend: the storage contract deliberately refuses it.

Copy the same cluster key to every node. Configure reachable advertised addresses and consistent DHT policy. Start at least enough metadata voters to form a majority before expecting namespace/catalogue mutations to commit.

## Capacity

DATA capacity is a cluster property derived from eligible node capacities and the desired replica count. For R=1, heterogeneous node capacities aggregate; a small node does not define the whole cluster's capacity. For larger R, the same bytes must fit on multiple distinct owners/failure domains, so logical capacity is necessarily lower.

Each local backend also has two independent admission limits:

- configured DATA `limit`;
- physical `reserve_free` that must remain available on the underlying filesystem.

CONTROL/METADATA uses a separate store/quota and must be monitored independently.

## When a DATA backend fills

A full backend stops admitting new DATA. Local `StoragePool` tries another ranked backend if configured. Distributed placement tries another ranked node when necessary. Existing objects remain readable.

Do not increase metadata/control quota merely to work around a full DATA disk; those are intentionally different resources.

## Metadata quorum loss

Without a metadata-voter majority, namespace/catalogue mutations cannot commit. Nodes may continue serving a persisted/readable snapshot where the operation supports it.

Catalogue scanner infrastructure failures are deferred rather than counted as semantic provider failures. Once quorum returns, work resumes.

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

`DEBUG` logs show placement, quorum, catalogue and maintenance state without the per-object volume of `ALL`. `ALL` is intended for targeted tracing and can be expensive on active systems.

A DATA failure should be diagnosed as placement/admission/durability; a metadata failure as voter/control durability. Keeping those failure domains distinct is intentional and should be preserved in logs and tooling.

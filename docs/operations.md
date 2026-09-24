# Operations

## Starting a fresh cluster

Create the configured state, DATA backend, cache and FUSE spool parent directories on the intended filesystems. Do not point Macha at a populated unversioned state/backend: the storage contract deliberately refuses it.

Copy the same cluster key to every node. Configure reachable advertised addresses and consistent DHT policy. Start at least `dht.metadata_min_write_replicas` nodes before expecting namespace/catalogue mutations to commit.

## Capacity

DATA capacity is a cluster property derived from eligible node capacities and the desired replica count. For R=1, heterogeneous node capacities aggregate; a small node does not define the whole cluster's capacity. For larger R, the same bytes must fit on multiple distinct owners/failure domains, so logical capacity is necessarily lower.

The mount reports that logical figure to `statfs` (what `df` shows): the largest amount that can be stored `dht.replicas` times across distinct failure domains (distinct nodes when there are fewer domains than replicas), counting only extent-hosting nodes, with used space shown as physical usage divided by the replica count.

Each local backend also has two independent admission limits:

- configured DATA `limit`;
- physical `reserve_free` that must remain available on the underlying filesystem.

CONTROL/METADATA uses a separate store/quota and must be monitored independently.

## When a DATA backend fills

A full backend stops admitting new DATA. Local `StoragePool` tries another ranked backend if configured. Distributed placement tries another ranked node when necessary. Existing objects remain readable.

Do not increase metadata/control quota merely to work around a full DATA disk; those are intentionally different resources.

## Metadata write-floor loss

With fewer than `metadata_min_write_replicas` active nodes, namespace/catalogue mutations cannot publish. Nodes may continue serving a persisted/readable snapshot where the operation supports it.

Catalogue scanner infrastructure failures are deferred rather than counted as semantic provider failures. An ingest job that meets unwritable metadata (no write floor, or a DATA or CONTROL retention floor not met) goes to `blocked` with `error_code` `metadata_unavailable` and is retried every `ingest.blocked_retry_ms` rather than failing. Once the write floor is available, work resumes.

## Maintenance

Maintenance is bounded and low priority. It performs:

- missing DATA replica repair;
- local backend rebalance toward preferred placement;
- DATA reachability GC;
- catalogue control-object convergence and CONTROL GC;
- scheduled physical integrity scrub.

A complete no-progress pass backs off instead of repeatedly scanning a settled store. Foreground playback, mounted MachaDFS activity and loader (ingest) activity suppress speculative work according to policy.

## Garbage collection

Reachability from committed metadata is authority. Unreferenced objects are not immediately deleted: they must age past `maintenance.garbage_grace_ms`. This protects failed publications and convergence lag.

Namespace deletion and physical reclamation are deliberately separate. Once a
namespace batch is durably accepted and its operation journal is confirmed, the
FUSE operation can complete without waiting for DATA objects to be unlinked.
Physical GC later walks a persistent local-store cursor in slices of at most 64
objects. A slice yields as soon as playback, mounted-filesystem or loader
activity appears; destructive work is also fenced on complete cluster reachability,
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

### What recovery resolves on its own

This is
[discipline 3, recover by resolving](principles-and-laws.md#self-healing-disciplines).
Recovery never refuses to start over the *contents* of the journal; only a
missing or unreadable journal header is fatal. Everything else has a
deterministic resolution, is applied once, journaled so the next start does
not see it again, logged at `WARN` with one line per situation, and counted
under `diagnostics.filesystem` in `GET /api/v1/status`:

| situation | resolution | counter |
|---|---|---|
| a frame that does not fit the state so far (duplicated marker, marker whose operation is gone, non-monotonic sequence, unknown record type, undecodable payload) | skipped | `journal_recovery_skipped_frames` |
| a checksum failure before EOF (middle-of-journal corruption) | the tail from that frame is copied to `<journal>.corrupt.<ts>.<pid>` and truncated; the prefix is recovered; spools for operations in the tail are preserved as orphans | `journal_recovery_quarantined_bytes` |
| an incomplete or checksum-invalid final frame (torn append) | trimmed | — |
| an operation whose inode has no descriptor | retired with journaled `done`/`abandoned` markers; its spool is preserved as an orphan | `recovery_dropped_operations` |
| a spool shorter than its journal, or missing | that generation is abandoned; the file keeps its last published content | — (`dropped FUSE spool generation` line) |
| a publication whose file is no longer in the accepted namespace | abandoned (journaled, spool retired) — live or recovered | `publications_abandoned` |
| two inodes resolving to one path | the newer keeps the path; the other is re-journaled without it | — |

A non-zero counter after a restart is worth a look at the `WARN` lines it
came with, but it is not an outage: the node is up and the rest of the
backlog is publishing. The quarantined tail and `*.orphan.*` spool files are
kept (within `fuse.max_orphan_bytes`) for diagnosis and can be deleted once
understood.

The metadata journal follows the same rule: a frame that fails
authentication or does not fit the CAS chain ends the replayable prefix —
that tail is quarantined and truncated like a torn append, and the replica
starts from the state before it. A metadata history frame that cannot be
authenticated or decoded is skipped (`metadata history skipped frames`) and
any accepted head that needed it is repaired live from peers.

FUSE spool pressure is normal backpressure, not a capacity fault. Below half of
`fuse.max_spool_bytes` a copy may burst at local disk speed. Above that point
the frontend publishes durable prefixes and progressively paces new admission
against measured end-to-end publication throughput; at the configured bound it
waits for a real publication/retirement notification. Inspect
`diagnostics.filesystem.spool_bytes`, `spool_limit_bytes`,
`spool_publish_rate_bytes_per_second`, `spool_throttle_waits`, and
`spool_throttle_wait_ms`. The rate is aggregate physically retired spool bytes
over one continuously pressured wall-clock window; its current numerator and
elapsed time are exposed as `spool_publish_rate_window_bytes` and
`spool_publish_rate_window_ms`. If publishing is stalled, writers intentionally stay
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
  run is currently scheduled;
- `data_resources` reports DATA admission by class (viewer, loader,
  speculative), the background lease ceiling and its use, and the disk
  pressure monitor (below);
- `repair` reports objects repair could not source from any peer and local
  copies that could not be read; and
- `data_store` reports local store presence-index and pack-recovery counters.

`rpc_transport`, `retained_memory`, `http` and `auth` sit beside them.

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

### DATA device pressure

Every DATA store operation, including the torrent disk backend's reads, writes
and hashes when staging shares the DATA device, is timed against what an
operation of its size should cost (`dht.io_pressure_*`; see
[configuration](configuration.md)). Under pressure speculative work stands
aside and loader work is held to `io_pressure_min_background` leases only
while a viewer is present; a viewer is never gated. Each transition is logged
once at `INFO`:

```
DATA device pressure onset slowdown_percent=… last_percent=… last_us=… last_bytes=…
DATA device pressure released slowdown_percent=… last_percent=… last_us=… last_bytes=…
```

`diagnostics.data_resources` carries `device_pressured`, `device_service_us`,
`device_worst_us`, `device_slowdown_percent`, `device_pressure_onsets` and
`pressure_refusals`, so a throttled node can be told apart from an unwell one.

## RPC execution isolation

This is how [governing law 1](principles-and-laws.md#scheduling-laws) is enforced on
the RPC path: control traffic stays answerable whatever else the node is doing,
because it is how the cluster and the operator find out anything at all. A node
that cannot answer `ping` under load is indistinguishable from a dead one.

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
then speculative maintenance — laws 2 and 3 as an execution order. Durable FUSE
spool publication uses the loader class even when its journal records were
reconstructed after restart. Recovery provenance affects replay validation and
cache policy, not scheduling priority: work the user asked for does not become
speculative merely because the process restarted before finishing it.

## Subsystem plugins

Optional subsystems ship as `dlopen`'d modules rather than being compiled into
the server: BitTorrent acquisition
(`<libdir>/macha/plugins/libmacha-torrent.so`) and the FUSE mount
(`<libdir>/macha/plugins/libmacha-fuse.so`). The directory scanned is
`plugin_path`, which defaults to this build's private plugin directory, and
every module is checked against the running core's build stamp (project
version plus git commit) before it is called — a plugin from a different
build is refused and logged rather than loaded.

Only the libfuse adapter is in the FUSE plugin. The frontend that owns the
mount's durable spool and journal is core's own code and stays in
`libmacha_core`, so the plugin file decides whether this node can *mount*,
not whether it has a filesystem.

The practical consequences for an operator:

- **Deploy the plugins with the binary.** A node that receives only a new
  `macha` and `libmacha_core` silently loses every capability whose plugin was
  not copied across, and a node that receives only a new plugin refuses to
  load it on the build stamp. Copy `bin/macha`, `lib/macha/libmacha_core.*`
  and `lib/macha/plugins/` together, and verify hashes across nodes for all
  of them, not just the executable.
- **Read the state from `GET /api/v1/status`**, in the always-present
  `subsystems` block. Each entry is named for the plugin file it came from
  (`libmacha-torrent`), so the name says which file to look for on disk, and
  carries `state` (`unavailable`, `starting`, `running`, `faulted`,
  `restarting`, `disabled`), `restart_count` and `last_fault`.
  `unavailable` and `disabled` are different facts and matter:

  | state | meaning | action |
  |---|---|---|
  | `unavailable` | No plugin file, or the plugin declined to start because this node is configured not to run it (`torrent.enabled: false`, no `fuse.mount_path`). | None; this is the configured outcome. Install the plugin or turn the setting on if it was meant to run. Enabling a capability whose plugin is missing is not a configuration error: the node starts, reports the subsystem `unavailable`, and serves everything else. |
  | `running` | Loaded and started. | None. |
  | `faulted` | The last construct/start attempt threw; it is being retried with backoff. | Read `last_fault`; if it persists it becomes `disabled`. |
  | `disabled` | Too many failures in the window, or refused at load (build-stamp mismatch, unreadable file). | Needs an operator: fix the cause and restart the process. Nothing retries automatically. |

- **A faulted subsystem does not take the node down.** Metadata, RPC, the HTTP
  API and playback keep running; the capability itself is withdrawn while it
  is not running, so `/api/v1/torrents/*` answers 503 rather than failing in
  an unhelpful way. `/api/v1/torrents/search` is served by core and keeps
  working regardless. The FUSE manage endpoints
  (`/api/v1/manage/filesystem/blocked-namespace-operation`,
  `parked-publications`) answer "nothing to report" while the mount is
  faulted rather than erroring.
- **A lost mount is a `faulted` subsystem, not a process exit.** A FUSE mount
  that disappears under a running node (`umount -l`, a kernel module reload)
  is remounted in place, with `restart_count` climbing and `last_fault` naming
  the cause; the node never stops serving. A mount that keeps failing walks
  into `disabled` like any other subsystem rather than remounting forever, and
  the covered mountpoint stays non-writable throughout.
- **`SIGHUP` configuration reload works on a mounted node**, because the mount
  runs on its own thread rather than owning the process's signal handling.

## Durability tokens and restarts

This section is
[discipline 1, re-derive don't assert](principles-and-laws.md#self-healing-disciplines),
in its most load-bearing form: the durability contract treats "present after a
restart" as durable, justified by the store's pack validation on open plus an
explicit flush inside the probe.

A publication proves that its extents reached the write floor with placement
tokens: `(node, durability epoch, domain, generation, backend instance)`. The
epoch is fresh for every process and the backend instance for every reopen,
so a token can only be *checked* by the incarnation that issued it. A token
that outlived its incarnation is not a failure: the writer's barrier sends the
object ids to the peer, the peer answers from its disk — an object present
after a restart is durable, because the pack index is rebuilt from the packs
on open and the probe flushes the current incarnation before replying — and
hands out fresh tokens. Journal evidence on the writer:
`object durability re-derived after incarnation change reasserted=N absent=M
peers=P`; on the peer: `object durability re-derived after epoch change
present=N/M`. Only `absent` objects cost anything: they are re-put from a
local copy if one exists, otherwise the generation is replayed from the FUSE
spool (`FUSE async data publication replay …`). Restarting a node therefore
does not strand publications in flight on other nodes.

## Nodes behind CGNAT, and edge nodes

A node can declare, or discover, that it cannot be connected to
(`network.inbound_capable`), and that it stores no extents
(`storage.hosts_extents`). Both are gossiped with the node, so every peer
dials and places the same way.

What works on a node that accepts no inbound connections: everything. It
mounts the namespace, plays media, ingests, and hosts extents if configured
to. It opens a CONTROL and a DATA session to every capable peer and keeps
them open; peers answer over those sessions and never dial it. If a peer
needs a lane the node has not opened, it sends a `dial_request` over the
CONTROL session and the node dials. Every transport socket keeps TCP keepalive
probing at 60 s (15 s apart, four probes), and the health probe covers the
DATA lane as well as CONTROL, so a NAT mapping that expires underneath an idle
lane is noticed and redialled before a viewer needs it.

The shape rules:

- Extents live only where they can be fetched from. A node with
  `inbound_capable: false` and `hosts_extents: auto` hosts nothing; its
  writes go to the owners over its own DATA sessions. Forcing
  `hosts_extents: true` on it is allowed (a hub plus one hidden storage site
  is a legitimate cluster) with a warning: those extents are reachable from
  inbound-capable peers only.
- Two nodes that both accept no inbound connections never need a path to
  each other. Metadata reaches both through the capable replicas, and the
  destructive-GC fence (`all_known_reachable`) excludes such pairs rather than
  counting them as a fault.
- There is no relay. A cluster with no inbound-capable storage node is
  refused: a founding node with `inbound_capable: false`, or a joining node
  whose every bootstrap peer is known to be incapable, exits at start-up.
- `dht.replicas` must be satisfiable from extent-hosting nodes alone.

How to read Status: `nodes[]` carries `inbound_capable`, `hosts_extents` and
`dialable` for every node, plus `inbound_capable_mode` / `hosts_extents_mode`
(the configured values) on the local node. `connectivity` carries the local
resolution (`inbound_capable`, `inbound_capable_source` -- `configured`,
`persisted`, `default` or `probe:<peer>` -- and the last dial-back result under
`dial_back`). `cluster.conditions` reports `N node accepts` / `N nodes accept
no inbound connections` (information), `no inbound-capable node hosts extents`
(critical) and `replication N requires N extent-hosting nodes; M known`
(degraded). A `DEBUG` line is logged for every `dial_request` round trip and
every dial-back probe.

Every node in a cluster must run the same protocol version, so introducing a
node that declares either bit to a cluster that predates them is a rolling
upgrade of the whole cluster, not a per-node change.

An `auto` resolution is sticky: it is persisted under
`state_path/connectivity/inbound.bin`, changes only after two consecutive
failed dial-backs (or one success), and a change logs at `INFO` naming the
peer that decided it. The reprobe cadence is 10 minutes while incapable and
one hour while capable, so an added or removed port forward is picked up
without a restart.

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
metadata publication. Each node's `telemetry_freshness` is `live`, `stale`,
`last_known` or `unavailable`, so an online node cannot disappear merely
because optional telemetry was dropped.

The wire format is `TEL3`: every field tagged and length-delimited, and a
default-valued field omitted, so a newer node can add a field an older one
skips. There is no compatibility with the positional formats before it.

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

A telemetry sample only feeds `storage`/`cache`/`runtime` figures once it is
both fresh (within the freshness window) and self-reported `ready`; a stale
sample, or a fresh one whose sender reports itself `starting`/`recovering`,
falls back to the durable last-known figures exactly as if no telemetry
existed. `state` (`online`/`offline`/`retired`) reflects membership/control-plane
reachability, which is not itself a lie during a node's own local recovery;
the separate `phase` field (`starting`, `recovering`, `ready`, or `unknown`
without trustworthy telemetry) is the truthful signal for whether an online
peer's numbers can be trusted yet. This is what stops a node's own in-progress
recovery — which legitimately reports zero capacity/usage before its local
storage is ready — from briefly looking like real data loss in the cluster
aggregate; `cluster.conditions` reports "one or more online nodes are still
recovering" for that window instead.

Metadata availability logging is transition-only and canonical, for example `metadata availability changed state=writable previous=read-only reason="metadata write durability floor available"`. Routine negative checkpoint acknowledgements are silent because they are normal convergence decisions; transport/checkpoint exceptions remain diagnostic.

`POST /api/v1/status/connectivity/check` and the node-specific equivalent perform diagnostic connectivity checks without changing cluster configuration. State-changing administrative operations belong under `/api/v1/manage`.

## Unreconstructable accepted metadata heads

An accepted head whose record cannot be replayed from local `history.log`
(missing ancestry, an unreadable frame, a chain that does not reproduce the
record hash) is not a restart-and-quarantine event. The replica keeps the
durable acceptance certificate, excludes that head from reads for a 30-second
cooldown at a time, and logs one line naming the exact break:

```
WARN metadata accepted head cannot be reconstructed locally; excluded from reads pending live repair hash=… generation=… during=… reason=…
ERROR metadata accepted head is not reconstructible locally; keeping it for live repair from peers hash=… generation=… reason=…   (at startup)
```

Maintenance then repairs it live: each reachable peer is asked for the record
as a self-contained full body (`get_metadata_history_record`) and the head is
re-anchored in place (`INFO metadata history re-anchored … ` followed by
`INFO metadata accepted head repaired from peer …`). No operator action is
needed unless no peer can materialize the record either, in which case the
`WARN`/`DEBUG metadata head repair:` lines say so on every maintenance cycle.

`macha-metadata-dump KEY_FILE HISTORY_LOG [HEADS_META] [--all] [--stats]
[--tree] [--objects PATH]` is a read-only forensic decoder for a stopped
node's or a quarantined (`*.corrupt.<timestamp>`) metadata directory. It never
constructs a replica, so it cannot trigger recovery. It decodes every history
frame, reports anomalies, and for each accepted head walks the delta chain with
the production succession rule and states where materialization would fail.
`--stats` prints what each reconstructible head is made of. A tree-backed head
carries only its root; `--objects` names the control object store so the tree
can be walked, the delta chain actually replayed (naming the first frame that
diverges), and a stat timed against it.

## Manual metadata ancestry repair

`macha-metadata-repair` is an offline recovery tool, not a daemon maintenance
path. With no option it reports committed/accepted heads and retained ancestry.
Its causal-merge operation is intentionally manual and narrowly fenced: it
requires exactly two accepted heads with no retained common ancestor and one
head's durable mutation clock must strictly dominate the other. It refuses
ordinary mergeable histories, concurrent/equal clocks, unstaged acceptance and
insufficient distinct witnesses.

Stop every Macha node and make a recoverable copy of each configured
`state_path/metadata` directory before use. Run `--plan-causal-merge` against
every replica and require the generation, repair hash, primary, dominant and
subsumed hashes to match exactly. Then run `--stage-causal-merge` everywhere.
Only after the same record is durably staged on the named witness nodes may
`--accept-causal-merge STATE_PATH KEY_FILE WITNESS...` be run on every replica.
Start the whole cluster and verify one accepted descendant, writable metadata,
matching local generations and zero unresolved reconciliation conflicts.

This operation does not choose the numerically newest head. The state comes
only from strict causal dominance, while the subsumed accepted head remains an
authenticated additional parent of the repair record. Concurrent heads require
a separate conflict-preserving repair and must not use this command.

That repair is `--plan-conflict-merge`, `--stage-conflict-merge` and
`--accept-conflict-merge STATE_PATH KEY_FILE WITNESS...`, run in the same
plan-everywhere, stage-everywhere, accept-everywhere order. The tool also
offers `--diff-heads`, and `--export-acceptance` / `--import-acceptance` to
carry one accepted head's certificate to a replica that already holds the
record. Run it with no arguments for the full usage.

## Re-rooting the namespace onto the tree

A cluster founds with the namespace inlined in its metadata record.
`macha-namespace-migrate STATE_PATH KEY_FILE` re-roots one stopped node's
namespace onto the content-addressed Merkle tree in the control store, after
which the record carries only the tree's root and a commit rewrites the
changed leaf and the branches above it. Once re-rooted, a node stays
tree-backed; nothing migrates by installing a release.

It is offline and deliberate. Stop every node, let them converge on one head
first, then run it on each node: the new record is a pure function of the
converged namespace, so every node computes the same one independently.
`--expect-hash` makes a node refuse any record other than the one the first
node produced, `--witness NODE_ID` (once per node, at least the write floor)
names the nodes being re-rooted, `--adopt FILE` installs a record written by
`--export-record` on the node migrated first (for a node that stopped a few
commits behind it, after proving this node's namespace produces the same tree
root), and `--dry-run` installs nothing. The previous checkpoint, journal, history, heads and
acceptance proof are kept beside the originals with a `.pre-migration.<ns>`
suffix rather than deleted; no extent is touched.

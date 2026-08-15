![Macha Logo](gfx/macha_logo.svg)

# Macha

*Macha — <span lang="ga">Macha</span> /ˈmˠaxə/ — approximately 'MAKH-uh'*

Macha is a small C++20 distributed filesystem for large media files.
It mounts as a normal filesystem, stores files as immutable content-addressed extents, and replicates those extents across ordinary machines.

There is no permanent master. Nodes share one cluster key, discover membership through bootstrap peers, and converge placement and replicas in the background.

0.6.2 is usable, but deliberately narrow. It is built for large mostly-immutable video and music files, not as a complete general-purpose POSIX filesystem.

## What it does

- Linux FUSE3 and macOS macFUSE from the same filesystem core.
- Multiple storage disks per node.
- Automatic replication, repair and rebalance as nodes or disks appear and disappear.
- Persistent non-DHT read cache, suitable for SSD.
- Priority-based speculative cache hydration driven by pluggable hint engines.
- Replica-aware extent retrieval that can use several healthy replicas in parallel across independent reads.
- Playback-assisted replication: a remotely fetched block can become the local replica without another download.
- Durable namespace checkpoints on every active node, so a destroyed node can be replaced and repopulated from surviving replicas.
- A cluster-wide media catalogue whose titles, hierarchy, search data and artwork are kept complete on every node.
- Optional filesystem catalogue scanner using TMDB for TV/movies and MusicBrainz/Cover Art Archive for music.
- Optional HTTP/JSON catalogue access and mutation API.
- Authenticated encrypted transport and encrypted storage.
- No cloud service, account system or permanent coordinator.

The core dependencies are C++20, OpenSSL, yaml-cpp and libcurl. FUSE is optional at build time but required to mount the filesystem.

## Two-node local demo

This is intentionally disposable. It uses two metadata voters to exercise the same replicated metadata path as a real cluster. Both nodes are required for namespace writes; for fault-tolerant metadata use an odd voter count such as three.

Build first.

Linux:

```sh
sudo apt install build-essential cmake pkg-config libssl-dev libyaml-cpp-dev libcurl4-openssl-dev libfuse3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

macOS, with macFUSE already installed:

```sh
brew install cmake openssl@3 pkg-config yaml-cpp curl
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCURL_ROOT="$(brew --prefix curl)"
cmake --build build -j
```

Create two local nodes and one shared key:

```sh
mkdir -p demo/node1/{state,storage,cache,mnt}
mkdir -p demo/node2/{state,storage,cache,mnt}
openssl rand -hex 32 > demo/cluster.key
chmod 600 demo/cluster.key
```

`demo/node1.yaml`:

```yaml
state_path: demo/node1/state
key_file: demo/cluster.key
mount_path: demo/node1/mnt
log_level: INFO

storage:
  - path: demo/node1/storage
    limit: 10G

cache:
  path: demo/node1/cache
  max_blocks: 256

network:
  listen: 127.0.0.1
  advertise: 127.0.0.1
  port: 7437
  failure_domain: demo-1

dht:
  replicas: 2
  metadata_replicas: 2
  extent_size: 4M

bootstrap:
  - 127.0.0.1:7438
```

`demo/node2.yaml` is the same policy with different local paths and port:

```yaml
state_path: demo/node2/state
key_file: demo/cluster.key
mount_path: demo/node2/mnt
log_level: INFO

storage:
  - path: demo/node2/storage
    limit: 10G

cache:
  path: demo/node2/cache
  max_blocks: 256

network:
  listen: 127.0.0.1
  advertise: 127.0.0.1
  port: 7438
  failure_domain: demo-2

dht:
  replicas: 2
  metadata_replicas: 2
  extent_size: 4M

bootstrap:
  - 127.0.0.1:7437
```

Start node 1, then node 2, in separate terminals:

```sh
./build/macha --config demo/node1.yaml
./build/macha --config demo/node2.yaml
```

Copy a file into either mount. With both nodes idle, the second authoritative copy appears automatically.

The example uses 4 MiB extents. The compiled default remains 16 MiB; 4 MiB is the recommended media setting at present because it gives finer-grained reads, retries and rebalance.

## Configuration rules

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

## How files are stored

A file is namespace metadata plus an ordered list of immutable extents.

```text
pathname
   |
   v
metadata manifest
   |
   +--> extent A --+
   +--> extent B --+--> deterministic DHT owners
   +--> extent C --+          |
                              +--> node A / local disk
                              +--> node B / local disk
                              +--> node C / local disk
```

Extents are addressed by SHA-256. Sequential writes publish completed extents as they are filled; the whole file is not held in memory. Random writes use a temporary file under `state_path/tmp`; commit rebuilds the extent manifest from staged bytes, with unchanged content deduplicating by object ID.

Reads try, in order:

1. authoritative local storage;
2. persistent local cache;
3. another DHT node.

Sequential readers retain the current decrypted extent. Speculative reads are scheduled through the cache hydrator rather than by per-handle futures. Read-ahead, the remaining extents of the current file, and catalogue-predicted next media all submit ordered hints with independent priorities.

Remote extent retrieval is replica-aware. Independent foreground reads choose among the configured replica set using current load, recent transfer latency and failures. Speculative hydration uses the same measurements but prefers idle replicas and yields to foreground work. Concurrent requests for the same object are coalesced; if playback needs an extent already being hydrated, that transfer is promoted rather than duplicated. A successful remote foreground fetch is still persisted asynchronously, and if this node should own the extent the same bytes can become the authoritative replica.

## Nodes and disks

A DHT node may have several local storage backends. The cluster sees one node whose placement weight is the aggregate configured capacity of its adopted backends. A second capacity-weighted shard layer chooses the local authoritative disk.

Each adopted backend has a `.macha.backend` marker and a matching identity under `state_path/backend-identities`.

If an adopted disk disappears temporarily, it goes offline but keeps its placement weight. Reads and writes fall through to surviving disks without making a transient unmount redefine the whole placement map. If the disk returns with the expected marker, local rebalance converges objects back to their intended proportional placement.

Adding or removing a backend in YAML and sending `SIGHUP` changes capacity without changing node identity. A newly adopted backend gains a proportional share of local placement; removing it from configuration removes that share. Current free space is never used as a placement weight.

## Node replacement and recovery

`state_path` contains identity and control-plane state. Deleting it means **this is a new node**, even if it starts on the same machine and storage paths.

Committed namespace checkpoints are therefore retained on every active node, not only metadata voters. If an old voter is permanently lost and a fresh replacement joins, the surviving nodes can reconstruct the voter set from an agreed committed checkpoint. The replacement then walks the recovered live-object set and pulls the extents it should own.

This is intentionally not a partition escape hatch. Recovery waits until apparently-active peers are either reachable or age out under `dead_after_ms`, requires the surviving committed checkpoints to agree, and requires fresh replacement node(s) for the missing voter seats. If an old voter quorum still exists, normal quorum recovery wins.

Losing only `storage` is simpler: namespace and node identity survive, and normal object repair restores missing replicas. Losing `state_path` is replacement, not disk failure.

## Metadata and split brain

Namespace metadata is a versioned encrypted CAS record held by a configured voter set. Reads and mutations require majority evidence from that set.

For three voters:

```text
3 healthy       quorum 2; read/write
2 healthy       quorum 2; read/write
1 healthy       no mutation quorum
```

A metadata minority fails rather than inventing a second history. Read-only access may fall back to the last valid local snapshot when quorum is unavailable; mutations do not.

The current metadata voter set and data replica count are persisted in the namespace. Replica counts may be changed on a coordinated whole-cluster restart; the old voter majority commits the new policy, then ordinary repair converges existing objects to the new data replica count. `extent_size` remains fixed for the lifetime of the namespace.

## Catalogue and JSON API

The catalogue is cluster metadata. Every node converges the complete catalogue snapshot and every referenced artwork object, independently of `dht.replicas`. Catalogue synchronisation runs ahead of ordinary media repair, and catalogue reads synchronise on demand. A node reports `ready: true` only when it has the current catalogue and all referenced artwork locally.

The optional scanner walks configured roots in the distributed filesystem and populates this catalogue automatically. TV and movie metadata comes from TMDB. Music metadata comes from MusicBrainz, with album covers from Cover Art Archive. Provider integration is behind a small interface rather than built into the scanner.

Only the lowest active node ID runs a scan, so a normally configured cluster does not make the same provider requests from every node. Enable the scanner consistently on all nodes if you want automatic failover of that role. Already-bound files are identified by their stable `macha:<sha256>` media identity and are not looked up or downloaded again on every pass. Scanner-owned entries are removed when their final media binding disappears; manually-created catalogue records are not garbage-collected by the scanner.

Filename/path recognition is intentionally simple and conservative. Typical forms are `Show/Season 02/Show.S02E05.Title.mkv`, `Movie.Title.2024.mkv`, and `Artist/Album/01 - Track.flac`; `CD 2`/`Disc 2` music directories are also recognised. Unrecognised files are ignored. Embedded audio/video tags are not parsed in 0.6.2. A failure to read any configured scan root aborts that pass rather than treating the missing root as an empty library.

TMDB needs an API Read Access Token. Put the token alone in a file readable by Macha. MusicBrainz does not need an API key, but requires a meaningful contact string and is rate-limited by the provider; Macha spaces its MusicBrainz API requests accordingly. Configure only curated media roots:

```yaml
catalogue:
  scanner:
    enabled: true
    interval_ms: 21600000
    roots:
      - /Movies
      - /TV
      - /Music
    max_artwork_bytes: 16M
    providers:
      tmdb:
        enabled: true
        token_file: /etc/macha-tmdb.token
        language: en-GB
        image_size: w500
      musicbrainz:
        enabled: true
        contact: https://github.com/tomdionysus/macha
        cover_size: "500"
```

Movies download poster and backdrop artwork. TV downloads show poster/backdrop, season poster and episode stills. Music downloads the front album cover. Images are stored as immutable Macha objects and committed with the catalogue, so every active node receives the actual image bytes rather than depending on provider URLs at display time.

Enable the API locally:

```yaml
catalogue:
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7438
    # token_file: /etc/macha-api.token
    max_request_bytes: 8M
```

Useful endpoints are:

```text
GET    /api/v1/catalogue/status
GET    /api/v1/catalogue/items?type=show&parent=...
GET    /api/v1/catalogue/search?q=expanse
GET    /api/v1/catalogue/items/{id}
PUT    /api/v1/catalogue/items/{id}
DELETE /api/v1/catalogue/items/{id}
POST   /api/v1/catalogue/items/{id}/artwork?role=poster&mime=image/jpeg
GET    /api/v1/catalogue/artwork/{sha256}
```

Item mutations support `If-Match: "rev-N"` and return an `ETag`. If `token_file` is configured, clients must send that file's contents as a Bearer token. Keep a remotely exposed API authenticated and firewall-restricted.

Replacing or deleting the last reference to artwork records its object ID in committed metadata garbage. After `maintenance.garbage_grace_ms` (24 hours by default), every node removes its local copy; a disconnected node performs the same deletion after it rejoins. A still-live reference always wins.

## Repair and maintenance

Media-object placement uses a virtual 32-bit shard space derived from the content-addressed object ID. No 2^32-entry map is materialised. Each node receives an exact shard-slot quota from its advertised usable capacity, subject to the configured replica count. The first replica choices therefore follow capacity rather than node count: a 10 TiB node receives roughly 1,280 times the ownership of an 8 GiB node when topology permits it. `failure_domain` remains a stronger placement constraint: replicas prefer distinct domains before additional nodes in the same domain.

Capacity is based on adopted configured backend limits, not live free space. Ordinary writes therefore do not move placement boundaries. Adding/removing storage changes the capacity topology and background repair/rebalance converges the affected shard ownership. Within a node, the single local authoritative copy uses capacity-weighted rendezvous over the stable shards, so adding a backend moves only shards won by that backend. Deterministic capacity-aware fallbacks are used when a preferred node or disk cannot currently store an object.

For `R` replicas, reported logical capacity is the largest `L` satisfying `sum(min(C_i, L)) >= R*L`, where `C_i` is node capacity (or failure-domain aggregate capacity when enough distinct domains exist). This correctly reports about 10.004 TiB for 10 TiB + 10 TiB + 8 GiB at `R=2`, but only 8 GiB for 10 TiB + 8 GiB at `R=2`.

Repair works both ways:

- existing owners push toward the current owner set;
- new or replacement nodes pull live objects they should own.

Background work is budgeted in bytes, not a fixed number of extents. The scheduler uses observed transfer rate, foreground activity and CPU load. Network repair, local disk rebalance and scrub have separate credits. With the default `busy_bandwidth_fraction: 0.0`, foreground I/O pauses background WAN repair.

## Transport

Each peer pair has one persistent authenticated bidirectional TCP connection. It multiplexes all health, control and object traffic with 64-bit request IDs. Connections are canonical by authenticated node identity, not endpoint text; simultaneous cross-dial deterministically keeps one physical connection and drains the duplicate before closing it.

Protocol v7 transfers logical RPCs as variable-length AES-256-GCM frames. `network.max_frame_size` is an upper bound, negotiated to the lower peer limit during the authenticated handshake; the default is 256 KiB and the allowed range is 4 KiB..4 MiB. Frames are not padded to that size. Storage extent size is independent of transport frame size.

Frame type is the sole source of transport priority: `control` > `foreground` > `read_ahead` > `speculative`. There is no separate numeric priority on the wire. The sender re-runs scheduling after every frame, so health/control and foreground data can pre-empt lower-priority transfers at frame boundaries. Speculative traffic is entitled only to otherwise spare transport capacity. A transfer may be promoted without changing request ID; subsequent frames use the more urgent frame type.

The v7 handshake uses ephemeral X25519 authenticated with HMAC from the shared cluster key and negotiates the frame ceiling. Directional keys are derived with HKDF-SHA256. Server dispatch likewise separates control execution from data work and always chooses foreground before read-ahead before speculative queued data.

Nodes must be mutually reachable at their advertised addresses. There is no STUN, TURN, UPnP or NAT hole punching.

## Cache and hydration

The persistent cache is deliberately not part of DHT ownership.

- It survives restart.
- It does not count toward replica quorum.
- It is bounded by block count and evicted approximately LRU.
- It can live on SSD while authoritative storage lives on HDD.
- Cached blocks can later be promoted if placement makes this node an owner.
- With `prefer_metadata: true`, the latest valid namespace snapshot is also kept outside the media-block limit.

The cache hydrator consumes ordered hints from independent engines. The built-in engines are:

- `read_ahead`: the immediate sequential window after the current read position;
- `current_file`: the rest of the file currently being read;
- `catalogue`: the next TV episode, crossing a season boundary when required, or the next movie in the same collection.

Hints that refer to the same ordered run are merged and their priorities reinforce each other. Scheduling uses weighted virtual time across runs, so the current file normally advances fastest without starving a predicted next item. A speculative run is sequential: the hydrator will not fetch a later extent while an earlier missing extent in that run is unavailable. Hydration keeps up to `max_inflight` extent requests active and rebuilds the hint set continuously. Read-ahead/current-file transfers use the read-ahead transport class; catalogue prediction uses speculative transport. Either yields immediately to foreground frames at the transport scheduler.

Hydration requires the persistent cache to be enabled. If the cache is disabled, foreground reads continue normally but speculative hints do not trigger network fetches.

`dht.read_ahead` remains the size of the immediate read-ahead hint window. Engine enablement and priority are configured separately under `hydration`:

```yaml
hydration:
  enabled: true
  interval_ms: 100
  active_timeout_ms: 30000
  catalogue_lookahead: 1
  engines:
    read_ahead:   { enabled: true, priority: 1000 }
    current_file: { enabled: true, priority: 700 }
    catalogue:    { enabled: true, priority: 300 }
```

Catalogue media bindings may use the stable `macha:<sha256>` media identity derived from file size plus the ordered extent manifest. That identity survives a namespace rename. Raw paths and `path:/...` remain accepted as a practical fallback for manually-created catalogue records.

## Filesystem limits

The implemented filesystem operations cover ordinary media-library use: files and directories, create/open/read/write/truncate/unlink, mkdir/rmdir, rename, chmod/chown, timestamps, stat/statfs, directory enumeration, flush and fsync.

0.6.2 does **not** implement symlinks, hard links, extended attributes, distributed advisory locks, full sparse-file semantics, or stable POSIX inode identity across every rename case. Access time is not tracked. Concurrent appenders use file-version CAS rather than a globally serialized append stream.

A failed upload may leave unreachable immutable extents. Online garbage collection only removes objects known to have been dropped from committed metadata after a conservative grace period.

## On-disk layout

```text
<state_path>/
    .macha.lock
    node.id
    backend-identities/
    metadata/
        current.meta
        committed.meta
    tmp/

<storage backend>/
    .macha.backend
    objects/ab/cd/<sha256>.obj

<cache path>/
    objects/ab/cd/<sha256>.obj
    metadata/current.meta
```

Object writes use unique temporary names, `fsync`, and atomic rename. The state path is exclusively locked so two processes cannot use one node identity at once.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The integration suite covers transport/crypto, bidirectional RPC and connection deduplication, metadata quorum and replacement recovery, catalogue join synchronisation/search/artwork GC, filename probing/provider resolution/scanner reconciliation, multi-node placement, disk loss/return, cache persistence, automatic new-owner pull, playback-assisted promotion, corruption repair and restart.

## Service files

- `systemd/macha.service`
- `systemd/macha.conf.example`
- `macos/macha.plist.example`

The systemd unit supports `systemctl reload macha`, which sends `SIGHUP`.

## Security

Anyone with the cluster key is a trusted cluster member. Keep it secret and back it up separately. There is no online key rotation or per-node revocation in 0.6.2. See `SECURITY.md`.

## License

GPL-3.0-or-later. See `LICENSE`.

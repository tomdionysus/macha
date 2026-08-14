![Macha Logo](gfx/macha_logo.svg)

# Macha

*Macha — <span lang="ga">Macha</span> /ˈmˠaxə/ — approximately 'MAKH-uh'*

Macha is a small C++20 distributed filesystem for large media files.
It mounts as a normal filesystem, stores files as immutable content-addressed extents, and replicates those extents across ordinary machines.

There is no permanent master. Nodes share one cluster key, discover membership through bootstrap peers, and converge placement and replicas in the background.

0.4.0 is usable, but deliberately narrow. It is built for large mostly-immutable video and music files, not as a complete general-purpose POSIX filesystem.

## What it does

- Linux FUSE3 and macOS macFUSE from the same filesystem core.
- Multiple storage disks per node.
- Automatic replication, repair and rebalance as nodes or disks appear and disappear.
- Persistent non-DHT read cache, suitable for SSD.
- Playback-assisted replication: a remotely fetched block can become the local replica without another download.
- Durable namespace checkpoints on every active node, so a destroyed node can be replaced and repopulated from surviving replicas.
- Authenticated encrypted transport and encrypted storage.
- No cloud service, account system or permanent coordinator.

The core dependencies are C++20, OpenSSL and yaml-cpp. FUSE is optional at build time but required to mount the filesystem.

## Two-node local demo

This is intentionally disposable. It uses two metadata voters to exercise the same replicated metadata path as a real cluster. Both nodes are required for namespace writes; for fault-tolerant metadata use an odd voter count such as three.

Build first.

Linux:

```sh
sudo apt install build-essential cmake pkg-config libssl-dev libyaml-cpp-dev libfuse3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

macOS, with macFUSE already installed:

```sh
brew install cmake openssl@3 pkg-config yaml-cpp
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
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

Send `SIGHUP` to reload only local storage-backend and cache configuration. Replica-count changes require a coordinated cluster stop/edit/restart; the old metadata quorum commits the new voter/data policy when the cluster comes back. `extent_size` cannot change for an existing namespace.

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
--control-stall-notice MS        DEBUG notice only; 0 disables
--data-stall-notice MS           DEBUG notice only; 0 disables
--metadata-cache MS
--mount PATH
--log-level LEVEL
```

RPC duration itself is unbounded. Stall notices are observability thresholds; they do not cancel requests. Peer death is decided independently by the health lane and `dead_after_ms`.

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

Sequential readers retain the current decrypted extent and prefetch future extents. A remote foreground fetch is persisted asynchronously; if this node should own that extent, the same bytes are promoted into authoritative storage.

## Nodes and disks

A DHT node may have several local storage backends. The cluster sees one node with aggregate capacity; a second rendezvous-hash layer chooses which online disk holds each local authoritative object.

Each adopted backend has a `.macha.backend` marker and a matching identity under `state_path/backend-identities`.

If a disk disappears, the backend goes offline. The node remains available. Repair can reconstruct missing local replicas onto surviving disks. If the disk returns with the expected marker, it rejoins and local placement converges again.

Adding or removing a backend in YAML and sending `SIGHUP` changes the local pool without changing node identity.

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

## Repair and maintenance

Object placement uses rendezvous hashing over object ID and stable node ID, with `failure_domain` used to prefer distinct failure domains before ordinary ranking.

Repair works both ways:

- existing owners push toward the current owner set;
- new or replacement nodes pull live objects they should own.

Background work is budgeted in bytes, not a fixed number of extents. The scheduler uses observed transfer rate, foreground activity and CPU load. Network repair, local disk rebalance and scrub have separate credits. With the default `busy_bandwidth_fraction: 0.0`, foreground I/O pauses background WAN repair.

## Transport

Each peer pair has up to three persistent authenticated connections:

- `health` for liveness;
- `control` for membership and metadata;
- `data` for bulk objects.

Each lane is bidirectional and multiplexes requests with 64-bit request IDs. Connections are canonical by authenticated node identity, not endpoint text. If both nodes dial the same lane, both sides deterministically keep the same physical connection and drain the duplicate before closing it.

Server work is also split into health, control and data queues so bulk transfers cannot starve liveness or namespace traffic.

The v4 handshake uses ephemeral X25519 authenticated with HMAC from the shared cluster key. The transport lane is part of the authenticated handshake, so duplicate arbitration is complete before the first RPC. Directional keys are derived with HKDF-SHA256 and frames use AES-256-GCM.

Nodes must be mutually reachable at their advertised addresses. There is no STUN, TURN, UPnP or NAT hole punching.

## Cache

The persistent cache is deliberately not part of DHT ownership.

- It survives restart.
- It does not count toward replica quorum.
- It is bounded by block count and evicted approximately LRU.
- It can live on SSD while authoritative storage lives on HDD.
- Cached blocks can later be promoted if placement makes this node an owner.
- With `prefer_metadata: true`, the latest valid namespace snapshot is also kept outside the media-block limit.

## Filesystem limits

The implemented filesystem operations cover ordinary media-library use: files and directories, create/open/read/write/truncate/unlink, mkdir/rmdir, rename, chmod/chown, timestamps, stat/statfs, directory enumeration, flush and fsync.

0.4.0 does **not** implement symlinks, hard links, extended attributes, distributed advisory locks, full sparse-file semantics, or stable POSIX inode identity across every rename case. Access time is not tracked. Concurrent appenders use file-version CAS rather than a globally serialized append stream.

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

The integration suite covers transport/crypto, bidirectional RPC and connection deduplication, metadata quorum and replacement recovery, multi-node placement, disk loss/return, cache persistence, automatic new-owner pull, playback-assisted promotion, corruption repair and restart.

## Service files

- `systemd/macha.service`
- `systemd/macha.conf.example`
- `macos/macha.plist.example`

The systemd unit supports `systemctl reload macha`, which sends `SIGHUP`.

## Security

Anyone with the cluster key is a trusted cluster member. Keep it secret and back it up separately. There is no online key rotation or per-node revocation in 0.4.0. See `SECURITY.md`.

## License

GPL-3.0-or-later. See `LICENSE`.

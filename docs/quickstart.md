# Quick start: two-node fresh cluster

This creates a disposable two-node cluster on one machine. It is intended to demonstrate configuration and storage semantics, not benchmark performance.

## Build

Install CMake, a C++20 compiler, OpenSSL, curl, zlib, yaml-cpp, FUSE/macFUSE and the FFmpeg 6+ development libraries. Pick the line for your system:

```bash
# Debian 13+ / Ubuntu 24.04+ (on Ubuntu, enable universe first)
sudo apt update && sudo apt install build-essential cmake pkg-config \
  libssl-dev libcurl4-openssl-dev libyaml-cpp-dev zlib1g-dev \
  fuse3 libfuse3-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev \
  libtorrent-rasterbar-dev libminiupnpc-dev

# Fedora
sudo dnf install gcc-c++ cmake pkgconf-pkg-config \
  openssl-devel libcurl-devel yaml-cpp-devel zlib-devel \
  fuse3 fuse3-devel ffmpeg-free-devel \
  rb_libtorrent-devel miniupnpc-devel

# macOS (Homebrew)
xcode-select --install
brew update && brew install cmake pkgconf openssl@3 curl yaml-cpp \
  ffmpeg@7 libtorrent-rasterbar miniupnpc
brew install --cask macfuse
```

libtorrent-rasterbar (2.0 or later) and miniupnpc are optional: without them the BitTorrent acquisition plugin and UPnP port mapping are simply not built. Everything else is required. Do not install a second Boost alongside the one your libtorrent-rasterbar was built with: the torrent plugin compiled against mismatched Boost headers can crash at runtime. Debian 12 and Ubuntu 22.04 ship FFmpeg versions that are too old. For Arch, MacPorts, the FFmpeg version check and the full installation procedure, see [install-linux.md](install-linux.md) and [install-macos.md](install-macos.md).

Then:

```bash
cmake -S . -B build
cmake --build build -j
./run-tests.sh build
```

`run-tests.sh` runs every test executable the build produced: `macha-tests`, plus `macha-tests-runtime` and `macha-tests-torrent` when their dependencies were found.

## Create a cluster key

```bash
mkdir -p demo
printf 'replace this with a random cluster secret\n' > demo/cluster.key
chmod 600 demo/cluster.key
```

## Node 1

Create `demo/node1.yaml`:

```yaml
state_path: demo/node1/state
key_file: demo/cluster.key

storage:
  data:
    backends:
      - path: demo/node1/data
        limit: 2G
        reserve_free: 64M
    packing:
      threshold: 1M
      target_size: 64M
  metadata:
    path: demo/node1/control
    limit: 256M
    packing:
      threshold: 1M
      target_size: 32M

cache:
  path: demo/node1/cache
  max_blocks: 128

network:
  listen: 127.0.0.1
  advertise: 127.0.0.1
  port: 7437
  failure_domain: node1

dht:
  replicas: 1
  metadata_min_write_replicas: 2
  min_write_replicas: 1
  extent_size: 4M

fuse:
  mount_path: demo/node1/mount
  unmount_if_mounted: true

catalogue:
  scanner:
    enabled: false
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7438
```

## Node 2

Create `demo/node2.yaml`:

```yaml
state_path: demo/node2/state
key_file: demo/cluster.key

storage:
  data:
    backends:
      - path: demo/node2/data
        limit: 8G
        reserve_free: 64M
    packing:
      threshold: 1M
      target_size: 64M
  metadata:
    path: demo/node2/control
    limit: 256M
    packing:
      threshold: 1M
      target_size: 32M

cache:
  path: demo/node2/cache
  max_blocks: 128

network:
  listen: 127.0.0.1
  advertise: 127.0.0.1
  port: 7439
  failure_domain: node2

dht:
  replicas: 1
  metadata_min_write_replicas: 2
  min_write_replicas: 1
  extent_size: 4M

bootstrap:
  - 127.0.0.1:7437

fuse:
  mount_path: demo/node2/mount
  unmount_if_mounted: true

catalogue:
  scanner:
    enabled: false
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7440
```

Create the paths explicitly:

```bash
mkdir -p demo/node{1,2}/{state,data,control,cache,mount}
```

Start node 1, then node 2 in separate terminals:

```bash
sudo ./build/macha --config demo/node1.yaml --plugin-path build/plugins
sudo ./build/macha --config demo/node2.yaml --plugin-path build/plugins
```

`--plugin-path` points an uninstalled build at its own subsystem plugins (the FUSE mount and BitTorrent acquisition). Without it the server scans the install prefix's plugin directory, and the mount is missing or refused.

Check that each node is serving:

```bash
curl http://127.0.0.1:7438/api/v1/health
curl http://127.0.0.1:7440/api/v1/health
```

Node 1 has no bootstrap peers, so it founds the cluster and creates the `root` and `anonymous` accounts on first start. Root's generated password is written to `demo/node1/state/initial-root-password` (mode 0600, owned by the user that started the node). Read it, sign in, change the password and delete the file. Node 2 joins and receives the accounts by replication. See [Management](management.md#bootstrapping-and-recovery).

With `replicas: 1`, the 2 GiB node does not cap the 8 GiB node. DATA objects have one desired authoritative owner and may fall through to the other node when their preferred owner cannot admit them. With `metadata_min_write_replicas: 2`, both nodes are required for metadata publication in this two-node demonstration; in a larger cluster any two active replicas can satisfy the same floor.

For a production cluster, choose replica/failure-domain policy according to the failures you intend to survive; do not infer production durability from this R=1 example.

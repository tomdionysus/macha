# Quick start: two-node fresh cluster

This creates a disposable two-node cluster on one machine. It is intended to demonstrate configuration and storage semantics, not benchmark performance.

## Build

Install CMake, a C++20 compiler, OpenSSL, yaml-cpp, FUSE/macFUSE and the FFmpeg development libraries required by the project, then:

```bash
cmake -S . -B build
cmake --build build -j
./build/macha-tests
```

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
mount_path: demo/node1/mount

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
mount_path: demo/node2/mount

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
sudo ./build/macha --config demo/node1.yaml
sudo ./build/macha --config demo/node2.yaml
```

With `replicas: 1`, the 2 GiB node does not cap the 8 GiB node. DATA objects have one desired authoritative owner and may fall through to the other node when their preferred owner cannot admit them. With `metadata_min_write_replicas: 2`, both nodes are required for metadata publication in this two-node demonstration; in a larger cluster any two active replicas can satisfy the same floor.

For a production cluster, choose replica/failure-domain policy according to the failures you intend to survive; do not infer production durability from this R=1 example.

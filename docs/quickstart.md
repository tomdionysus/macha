# Quick start

This is a disposable two-node local demo. It uses two metadata voters so both nodes are required for namespace writes. Use an odd voter count such as three for fault-tolerant metadata.

## Build

Raspberry Pi OS / Debian 13 (Trixie):

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libssl-dev libyaml-cpp-dev \
  libcurl4-openssl-dev libfuse3-dev libavformat-dev libavcodec-dev \
  libavutil-dev libswscale-dev libswresample-dev libtorrent-rasterbar-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`libtorrent-rasterbar-dev` enables the integrated BitTorrent acquisition component. If it is omitted, Macha still builds the generic resumable filesystem ingest engine, but torrent acquisition is disabled.

macOS, with macFUSE already installed. Homebrew `ffmpeg@7` supplies the libav headers, libraries and pkg-config metadata; Macha does not execute the command-line program. The formula is keg-only, so Macha's CMake file discovers its Homebrew prefix automatically:

```sh
brew install cmake openssl@3 pkgconf yaml-cpp curl ffmpeg@7 libtorrent-rasterbar
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCURL_ROOT="$(brew --prefix curl)"
cmake --build build -j
```

If you deliberately use another keg or non-Homebrew FFmpeg build, expose its `lib/pkgconfig` directory through `PKG_CONFIG_PATH` before configuring.

## Create the demo cluster

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

catalogue:
  scanner:
    enabled: false
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7440

streaming:
  enabled: true
```

`demo/node2.yaml`:

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

Start each node in a separate terminal:

```sh
./build/macha --config demo/node1.yaml
./build/macha --config demo/node2.yaml
```

Copy a file into either mount. With both nodes idle, the second authoritative copy appears automatically.

```sh
mkdir -p demo/node1/mnt/Movies
cp /path/to/movie.mp4 demo/node1/mnt/Movies/demo.mp4
```

The example uses 4 MiB extents. The compiled default remains 16 MiB; 4 MiB is the recommended media setting at present because it gives finer-grained reads, retries and rebalance.

## Try playback

A path can be used directly as a media identity, so the demo does not need a catalogue scanner or provider credentials. An H.264/AAC MP4 will normally resolve to direct play; other compatible containers are remuxed to fragmented-MP4 HLS and incompatible streams are transcoded as required.

```sh
curl -sS \
  -H 'Content-Type: application/json' \
  -d '{"media_id":"path:/Movies/demo.mp4"}' \
  http://127.0.0.1:7440/api/v1/playback/sessions
```

The response contains `stream_url`, `mime_type`, the selected streams and available playback options. Prefix the relative `stream_url` with `http://127.0.0.1:7440`. Direct streams support HTTP byte ranges. Transformed streams return an HLS playlist and fragmented MP4 segments.

Delete the session when the player is finished:

```sh
curl -X DELETE http://127.0.0.1:7440/api/v1/playback/sessions/SESSION_ID
```

See [Streaming](streaming.md) for capability negotiation, quality/track changes and seeking.

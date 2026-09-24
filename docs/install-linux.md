# Installing Macha on Linux

This guide builds the Macha server, FUSE filesystem, media support, UPnP
support and BitTorrent acquisition from source, then installs the supplied
systemd service.

## Requirements

Macha requires:

- CMake 3.20 or later and a C++20 compiler
- OpenSSL, libcurl, zlib and yaml-cpp development files
- FFmpeg 6 or later development libraries (`libavformat` and `libavcodec`
  60, `libavutil` 58, `libswscale` 7 and `libswresample` 4, or later)
- pkg-config and POSIX threads

FUSE 3 is required for the mounted filesystem. `miniupnpc` and
libtorrent-rasterbar 2.0 or later are optional at build time, but are needed
for UPnP port mapping and BitTorrent acquisition respectively. CMake reports
clearly when any optional feature is unavailable.

libtorrent is compiled against a particular Boost, and the BitTorrent plugin
must be built against the same Boost headers. Use the Boost development
package your distribution's libtorrent-rasterbar package depends on, and do
not install a different Boost version alongside it: a plugin built against
mismatched Boost headers can crash at runtime.

### Debian 13 (Trixie) and later

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config \
  libssl-dev libcurl4-openssl-dev libyaml-cpp-dev zlib1g-dev \
  fuse3 libfuse3-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev \
  libtorrent-rasterbar-dev libminiupnpc-dev
```

Debian 12's standard repository contains FFmpeg 5, which is too old for
Macha. Use Debian 13 or a later release, or provide a complete FFmpeg 6+
development installation from a trusted repository.

### Ubuntu 24.04 LTS and later

Enable `universe` if it is not already enabled, then install:

```bash
sudo add-apt-repository universe
sudo apt update
sudo apt install build-essential cmake pkg-config \
  libssl-dev libcurl4-openssl-dev libyaml-cpp-dev zlib1g-dev \
  fuse3 libfuse3-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev \
  libtorrent-rasterbar-dev libminiupnpc-dev
```

Ubuntu 22.04's standard FFmpeg 4.4 packages do not meet Macha's requirement.

### Fedora

```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config \
  openssl-devel libcurl-devel yaml-cpp-devel zlib-devel \
  fuse3 fuse3-devel ffmpeg-free-devel \
  rb_libtorrent-devel miniupnpc-devel
```

The similarly named `libtorrent` package is not libtorrent-rasterbar. Macha
needs Fedora's `rb_libtorrent-devel` package.

### Arch Linux and Manjaro

Arch packages include their development files:

```bash
sudo pacman -Syu --needed base-devel cmake pkgconf \
  openssl curl zlib yaml-cpp fuse3 ffmpeg \
  libtorrent-rasterbar miniupnpc
```

Package names and available versions can change between distribution
releases. Before building, this command verifies the FFmpeg API floor that
Macha uses:

```bash
pkg-config --exists 'libavformat >= 60' 'libavcodec >= 60' 'libavutil >= 58' \
  'libswscale >= 7' 'libswresample >= 4' && echo "FFmpeg API is suitable"
```

## Build and test

Run configure and compilation as your normal user. Do not use `sudo` for
either step:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`ctest` runs `macha-tests`, plus `macha-tests-runtime` and
`macha-tests-torrent` when their dependencies were found; `./run-tests.sh
build` runs the same executables directly.

Review the configure output. A full build reports `FUSE mount plugin
enabled`, `UPnP port mapping enabled` and `BitTorrent acquisition plugin
enabled`.

## Install and configure

Only installation requires root privileges:

```bash
sudo cmake --install build
```

With the `/usr` prefix, this installs:

- `/usr/bin/macha`
- `/usr/bin/macha-users`, `macha-recover`, `macha-metadata-dump`,
  `macha-metadata-repair` and `macha-namespace-migrate` (administration tools)
- `/usr/lib/macha/libmacha_core.so`
- `/usr/lib/macha/plugins/libmacha-fuse.so` (when FUSE 3 was found)
- `/usr/lib/macha/plugins/libmacha-torrent.so` (when libtorrent was found)
- `/usr/lib/systemd/system/macha.service`
- `/etc/macha/macha.yaml.example`
- `/etc/macha/macha.yaml` (copied from the example only if absent)
- documentation under `/usr/share/doc/macha`

`libmacha_core` and the plugin directory are part of the deployment, not
optional extras: an upgrade that copies only the executable leaves the node
running against the old core, or without the capabilities whose plugins were
not copied. See [Subsystem plugins](operations.md#subsystem-plugins).

The installer prints the actual configuration and service paths. An existing
`macha.yaml` is always preserved during reinstall or upgrade.

Edit `/etc/macha/macha.yaml` for this node. In particular, set its advertised
address, storage paths, capacities and bootstrap peers; on the first node of
a new cluster, remove the sample `bootstrap` list, since only a node with no
bootstrap peers founds the cluster. Copy the same secret
cluster-key file to every node, but keep node-local paths and addresses local.
The sample configuration uses `/etc/macha.key`.

Create every configured state, cache, spool, mount and storage directory
before starting Macha. Storage directories are deliberately not auto-created:
a missing disk must not silently become a directory on the root filesystem.
See [Configuration](configuration.md) for the complete schema.

## Start the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now macha.service
systemctl status macha.service
```

Follow startup and runtime logs with:

```bash
journalctl -u macha.service -f
```

Check that the node is serving (the sample configuration binds the API to
`127.0.0.1:7438`):

```bash
curl http://127.0.0.1:7438/api/v1/health
```

The first node of a new cluster, the one with no `bootstrap` peers, creates
the `root` and `anonymous` accounts on first start and writes root's
generated password to `<state_path>/initial-root-password` (with the sample
configuration, `/var/lib/macha/initial-root-password`), mode 0600. Read it,
sign in, change the password and delete the file. Nodes that join through
`bootstrap` receive the accounts by replication. See
[Management](management.md#bootstrapping-and-recovery).

If `systemctl` cannot find the unit, confirm that the configured installation
prefix was `/usr`, then inspect the path printed by `cmake --install`. The
normal unit location is `/usr/lib/systemd/system/macha.service`.

## Upgrade

Build the new source as your ordinary user, then reinstall and restart:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo systemctl restart macha.service
```

## Uninstall

```bash
sudo systemctl disable --now macha.service
sudo cmake --build build --target uninstall
sudo systemctl daemon-reload
```

Uninstall removes the installed programs, libraries, plugins, systemd unit
and documentation, but deliberately preserves configuration, keys, state,
cache, spool, mounts and media data.

## Distribution package references

- [Debian packages](https://packages.debian.org/)
- [Ubuntu packages](https://packages.ubuntu.com/)
- [Fedora packages](https://packages.fedoraproject.org/)
- [Arch Linux packages](https://archlinux.org/packages/)


# Installing Macha on macOS

Macha builds natively on Intel and Apple Silicon Macs. This guide provides
separate Homebrew and MacPorts dependency paths. Choose one package manager
for a build and do not mix their libraries in the same CMake build directory.

## Requirements

Install Apple's command-line developer tools first:

```bash
xcode-select --install
```

Macha requires CMake 3.20 or later, a C++20 compiler, OpenSSL, libcurl,
zlib, yaml-cpp and FFmpeg 6 or later development libraries. zlib comes with
the macOS SDK. macFUSE supplies the mounted filesystem. miniupnpc enables UPnP
port mapping, and libtorrent-rasterbar 2.0 or later enables BitTorrent
acquisition.

The BitTorrent plugin must be built against the same Boost headers that
libtorrent-rasterbar was built with: Homebrew's `boost` for Homebrew's
libtorrent-rasterbar, MacPorts' for MacPorts'. Do not point CMake at a
different Boost installation; a plugin built against mismatched Boost headers
can crash at runtime.

macFUSE may require approval in **System Settings > Privacy & Security** and a
restart before its system extension can load.

## Homebrew dependencies

Install [Homebrew](https://brew.sh/) if necessary, then install the build and
runtime dependencies:

```bash
brew update
brew install cmake pkgconf openssl@3 curl yaml-cpp \
  ffmpeg@7 libtorrent-rasterbar miniupnpc
brew install --cask macfuse
```

`ffmpeg@7` is keg-only. Macha's CMake configuration detects its Homebrew
prefix on both `/usr/local` Intel installations and `/opt/homebrew` Apple
Silicon installations; it does not need to be force-linked.

Configure and build as your normal user:

```bash
cmake -S . -B build-homebrew \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DMACHA_INSTALL_SYSTEMD=OFF
cmake --build build-homebrew --parallel
ctest --test-dir build-homebrew --output-on-failure
```

Review the configure output. A full build reports the Homebrew FFmpeg,
libtorrent and miniupnpc prefixes, followed by `FUSE mount plugin enabled`,
`UPnP port mapping enabled` and `BitTorrent acquisition plugin enabled`.

## MacPorts dependencies

Install the appropriate [MacPorts](https://www.macports.org/install.php)
package for the installed macOS release, update the ports tree, then install:

```bash
sudo port selfupdate
sudo port install cmake pkgconfig openssl3 curl yaml-cpp \
  ffmpeg7 libtorrent-rasterbar miniupnpc
sudo port install macfuse +fs_link
```

The `+fs_link` variant makes the macFUSE framework visible at its conventional
system location. MacPorts installs FFmpeg 7's pkg-config files below its
versioned `libexec` directory, so expose that directory while configuring:

```bash
export PKG_CONFIG_PATH="/opt/local/libexec/ffmpeg7/lib/pkgconfig:/opt/local/lib/pkgconfig:/opt/local/share/pkgconfig"
cmake -S . -B build-macports \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/local \
  -DCMAKE_PREFIX_PATH=/opt/local \
  -DMACHA_INSTALL_SYSTEMD=OFF
cmake --build build-macports --parallel
ctest --test-dir build-macports --output-on-failure
```

Keep `PKG_CONFIG_PATH` set when reconfiguring that build directory. If you
configured MacPorts with a non-default prefix, substitute that prefix for
`/opt/local` throughout these commands.

## Install

Never run CMake's configure or build steps with `sudo`; doing so leaves
root-owned generated files and can cause later `configure_file: Operation not
permitted` failures. Elevate only the installation step.

For the Homebrew build:

```bash
sudo cmake --install build-homebrew
```

This installs the executable at `/usr/local/bin/macha`, the administration
tools (`macha-users`, `macha-recover`, `macha-metadata-dump`,
`macha-metadata-repair`, `macha-namespace-migrate`) beside it,
`libmacha_core.dylib` under `/usr/local/lib/macha`, the FUSE and BitTorrent
plugins that were built under `/usr/local/lib/macha/plugins`, and the
configuration at `/usr/local/etc/macha/macha.yaml`. The library and plugin
directory are part of the installation: an upgrade that copies only the
executable leaves it running against the old core, or without its plugins.
For the MacPorts build:

```bash
sudo cmake --install build-macports
```

That installation uses the same layout under `/opt/local`, with
`/opt/local/bin/macha` and `/opt/local/etc/macha/macha.yaml`. The installer
prints the exact paths and preserves an existing configuration during
reinstall or upgrade.

Edit the installed `macha.yaml` for this node. Set its advertised address,
storage paths, capacities and bootstrap peers (on the first node of a new
cluster, remove the sample `bootstrap` list, since only a node with no
bootstrap peers founds the cluster); install the same secret
cluster-key file on every node. Create all configured state, cache, spool,
mount and storage directories before starting the server. See
[Configuration](configuration.md) for the complete schema.

## Run interactively

Running in the foreground is the simplest first test. A FUSE mount commonly
requires root privileges, depending on the macFUSE configuration:

Homebrew installation:

```bash
sudo /usr/local/bin/macha --config /usr/local/etc/macha/macha.yaml
```

MacPorts installation:

```bash
sudo /opt/local/bin/macha --config /opt/local/etc/macha/macha.yaml
```

Verify that the node is serving (the sample configuration binds the API to
`127.0.0.1:7438`), then stop it with `Ctrl-C` once it has also mounted:

```bash
curl http://127.0.0.1:7438/api/v1/health
```

The first node of a new cluster, the one with no `bootstrap` peers, creates
the `root` and `anonymous` accounts on first start and writes root's
generated password to `<state_path>/initial-root-password`, mode 0600. Read
it, sign in, change the password and delete the file. Nodes that join through
`bootstrap` receive the accounts by replication. See
[Management](management.md#bootstrapping-and-recovery).

## Run with launchd

The install includes `macha.plist.example` in its documentation examples.
Copy it to `/Library/LaunchDaemons/macha.plist`, then edit both the executable
and configuration paths to match the Homebrew or MacPorts installation above:

```bash
sudo cp /usr/local/share/doc/macha/examples/macha.plist.example \
  /Library/LaunchDaemons/macha.plist
sudo chown root:wheel /Library/LaunchDaemons/macha.plist
sudo chmod 644 /Library/LaunchDaemons/macha.plist
sudo plutil -lint /Library/LaunchDaemons/macha.plist
sudo launchctl bootstrap system /Library/LaunchDaemons/macha.plist
```

For a MacPorts-prefix installation, the example is under
`/opt/local/share/doc/macha/examples/` instead. To stop and unload it:

```bash
sudo launchctl bootout system /Library/LaunchDaemons/macha.plist
```

## Upgrade and uninstall

Reconfigure and rebuild as your normal user, run the tests, then repeat the
appropriate `sudo cmake --install` command. Restart the launchd service if it
is in use.

To remove the installed programs, libraries, plugins and documentation while
preserving configuration and runtime data:

```bash
sudo cmake --build build-homebrew --target uninstall
```

Use `build-macports` instead when that was the selected build. Unload and
remove a launchd plist separately if you installed one. Configuration, keys,
state, cache, spool, mounts and media data are intentionally retained.

## Package references

- [Homebrew ffmpeg@7](https://formulae.brew.sh/formula/ffmpeg@7)
- [Homebrew libtorrent-rasterbar](https://formulae.brew.sh/formula/libtorrent-rasterbar)
- [Homebrew miniupnpc](https://formulae.brew.sh/formula/miniupnpc)
- [Homebrew macFUSE](https://formulae.brew.sh/cask/macfuse)
- [MacPorts ffmpeg7](https://ports.macports.org/port/ffmpeg7/)
- [MacPorts libtorrent-rasterbar](https://ports.macports.org/port/libtorrent-rasterbar/)
- [MacPorts miniupnpc](https://ports.macports.org/port/miniupnpc/)
- [MacPorts macFUSE](https://ports.macports.org/port/macfuse/)

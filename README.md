<img src="gfx/macha_logo.svg" alt="Macha Logo" width="418">

# Macha

*v0.62.1*

*Macha — Old Irish /ˈmˠaxə/ — approximately “MAKH-uh”*

Macha is a C++20 distributed filesystem (MachaDFS) and media server for large,
mostly immutable video and music libraries. Files are split into encrypted
content-addressed extents, placed across ordinary machines, exposed through
FUSE, indexed in a distributed catalogue, and served directly or through
in-process FFmpeg remux/transcode pipelines.

There is no permanent master, cloud service or account system. Nodes share a
cluster key, discover peers through bootstrap endpoints, and converge data
placement and replicas in the background.

## This repository, and what sits around it

This repository is **the node** — the daemon that stores, replicates and serves
media. The `macha` binary is the server: DFS, catalogue, HTTP API and playback
engine. The FUSE mount and BitTorrent acquisition are
[subsystem plugins](docs/operations.md#subsystem-plugins) it loads at start,
so a node without a plugin file simply lacks that capability. Alongside it sit
offline tools: `macha-users` for accounts,
`macha-metadata-dump` and `macha-metadata-repair` for forensics, and
`macha-namespace-migrate` for re-rooting a namespace onto the tree. A cluster is
several nodes sharing a key.

Players are separate projects and are not in this tree. They reach a node over
its HTTP API, and the division of responsibility between them is deliberate and
worth understanding before changing either side:

> **The server reports what a file is and performs what it is asked for; it
> does not choose.** A client reads the media facts, decides what to do with
> them against its own decoder, and instructs.

So the node has no notion of device capability, no `capabilities` negotiation,
and refuses only what is impossible or misdescribed — never what a client said
it could not play. Whether a device can decode what it asked for is the
client's business. [Streaming](docs/streaming.md) is the normative statement of
that contract, including the error codes and the reasoning behind them; treat
it as the interface document when working on either side.

Because clients decide, they carry real logic, and a shared TypeScript core
(`@machafoundation/core`) implements the parts every player needs — node discovery and
ranking, session lifecycle, playback negotiation. Phone and TV players build on
that core. A change to the wire shape, an error code or a default is a change
to those projects too, and is worth saying out loud rather than leaving to be
discovered.

## Governing laws

Four laws govern Macha, and they are the first thing to understand about why
the node is built the way it is. They are shared by the server, the client
core and every client, with the same numbering, and cited by number in the
source. The first three order every scheduling, admission and priority
decision; the fourth is a veto over all of them rather than a rank among them:

1. **Thou Shalt Not Make Control Wait.**
2. **Thou Shalt Not Make The Viewer Wait.** And no viewer may be allowed to
   make another viewer wait.
3. **Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
   Viewer Wait.**
4. **Thou Shalt Not Shoot Thyself In The Foot** — no operation, code path or
   subsystem may leave the node in a state it cannot recover from on its own.
   The test: if this goes wrong on the node furthest away, does it come back
   without me?

Alongside them sit five self-healing disciplines governing behaviour when
something is wrong: re-derive rather than assert, give every retried work item
a backoff and a parked state, recover by resolving rather than refusing, keep
snapshot size a function of the live namespace rather than of history, and
never set a bound smaller than one unit of the work it bounds.

[Principles and laws](docs/principles-and-laws.md) states all nine, with the
conceptual principles they come from; [Architecture](ARCHITECTURE.md#governing-laws)
says how each one applies in the node.

## Storage model

Three explicit storage classes, with separate durability rules:

- **DATA** — media extents, artwork, subtitles and other immutable payload.
  Placed by the DHT across eligible node/backend capacity; a full preferred
  owner falls through to the next deterministic candidate.
- **CONTROL/METADATA** — namespace metadata and catalogue manifests/shards.
  Dedicated priority storage and an any-node write durability floor, so
  ordinary DATA quota can never block them.
- **CACHE** — opportunistic, non-authoritative copies. Cache contents never
  satisfy DATA or metadata durability.

Every known node is metadata-capable; there is no privileged voter subset.
`dht.min_write_replicas` is the foreground DATA publication floor,
`dht.replicas` the desired converged replica count, and
`dht.metadata_min_write_replicas` the independent floor for publishing a
namespace or control mutation. Divergent metadata heads are reconciled
automatically, non-conflicting namespace changes are merged, and incompatible
alternatives are preserved as durable conflicts rather than silently picking a
winner.

[Storage](docs/storage.md) and [Durability](docs/durability.md) are the precise
contract.

## Installing

Full instructions, including distribution dependencies, supported FFmpeg
versions, systemd setup, upgrades and removal:
[Linux](docs/install-linux.md) · [macOS](docs/install-macos.md) ·
[Quick start](docs/quickstart.md).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo make -C build install
```

On Linux this installs the executables, the private `libmacha_core` library
and plugins under `lib/macha`, a `macha.service` unit, the documentation and an
initial `/etc/macha/macha.yaml` — copied from
[`macha.yaml.example`](macha.yaml.example) only when no configuration exists,
so upgrades never overwrite operator changes. Edit it, create the referenced
storage/cache/state/spool paths and the cluster key, then start the service.
Uninstall (`sudo make -C build uninstall`) deliberately preserves the
configuration, keys, state, cache, spool, mounts and media data.

## Documentation

| | |
|---|---|
| [Principles and laws](docs/principles-and-laws.md) | [Quick start](docs/quickstart.md) |
| [Install on Linux](docs/install-linux.md) | [Install on macOS](docs/install-macos.md) |
| [Configuration](docs/configuration.md) | [Acquisition and ingest API](docs/acquisition.md) |
| [Storage](docs/storage.md) | [Durability](docs/durability.md) |
| [Metadata replication](docs/metadata.md) | [Cluster and recovery](docs/cluster.md) |
| [Catalogue](docs/catalogue.md) | [Streaming](docs/streaming.md) |
| [Operations](docs/operations.md) | [Management API](docs/management.md) |
| [High availability](docs/HA.md) | [Ownership and GC](docs/ownership.md) |
| [Architecture](ARCHITECTURE.md) | [Security](SECURITY.md) |
| [Release notes](CHANGELOG.md) | [Roadmap](ROADMAP.md) |
| [Validation](VALIDATION.md) | [Contributing](CONTRIBUTING.md) |

Developed with substantial use of AI-assisted implementation. If this troubles you
greatly, there are [many alternatives](https://pinggy.io/blog/best_self_hosted_media_servers).

GPL-3.0-or-later. See [`LICENSE`](LICENSE).

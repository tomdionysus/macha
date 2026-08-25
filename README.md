<img src="gfx/macha_logo.svg" alt="Macha Logo" width="418">

# Macha

*Macha — <span lang="ga">Macha</span> /ˈmˠaxə/ — approximately 'MAKH-uh'*

Macha is a C++20 distributed filesystem and media server for large, mostly immutable video and music files. It stores files as encrypted content-addressed extents across ordinary machines, mounts the namespace with FUSE, keeps a distributed media catalogue, and can serve media directly or as fragmented-MP4 HLS with negotiated remuxing/transcoding.

There is no permanent master, cloud service or account system. Nodes share one cluster key, discover membership through bootstrap peers, and converge placement and replicas in the background.

0.14.7 is usable but experimental. It is deliberately a media filesystem/server rather than a complete general-purpose POSIX filesystem. Streaming was introduced in 0.7.0 and links the FFmpeg libraries directly when enabled; no media subprocesses are launched.

**0.16.0 upgrade:** stop or upgrade every cluster node together. Transport v15 carries filesystem-domain durability tickets `(process epoch, domain, generation, backend incarnation)` and rejects v14-and-earlier peers rather than allowing mixed durability semantics. Existing authoritative objects, metadata, FUSE operation journals, spools and backend accounting remain readable in place.

**0.11.x upgrade:** stop every node before upgrading from 0.9.x. Transport v13 rejects v12-and-earlier peers; mixed-version operation is not supported. Existing 0.9.x SM7 checkpoints, DLT1 metadata journals, encrypted object files and backend accounting are read in place. New metadata is written as SM8/DLT2. Back up `state_path` first; after 0.10.x or 0.11.x has written new metadata, rollback to 0.9.x is unsupported. 0.11.0 does not change the 0.10.0 wire or storage formats.

## Documentation

- [Quick start and two-node demo](docs/quickstart.md)
- [Installation and configuration](docs/configuration.md)
- [Storage, disks and filesystem behaviour](docs/storage.md)
- [Durability architecture](docs/durability.md)
- [Cluster, recovery and transport](docs/cluster.md)
- [Catalogue and scanner](docs/catalogue.md)
- [Streaming and playback API](docs/streaming.md)
- [Maintenance, cache, tests and service files](docs/operations.md)
- [Architecture](ARCHITECTURE.md)
- [Security](SECURITY.md)
- [Roadmap](ROADMAP.md)
- [Changelog](CHANGELOG.md)

The complete configuration example is [`macha.yaml.example`](macha.yaml.example).

Developed with substantial use of AI-assisted implementation

GPL-3.0-or-later. See [`LICENSE`](LICENSE).

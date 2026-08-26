<img src="gfx/macha_logo.svg" alt="Macha Logo" width="418">

# Macha

*Macha — <span lang="ga">Macha</span> /ˈmˠaxə/ — approximately “MAKH-uh”*

Macha is a C++20 distributed filesystem and media server for large, mostly immutable video and music libraries. Files are split into encrypted content-addressed extents, placed across ordinary machines, exposed through FUSE, indexed in a distributed catalogue, and served directly or through in-process FFmpeg remux/transcode pipelines.

There is no permanent master, cloud service or account system. Nodes share a cluster key, discover peers through bootstrap endpoints, and converge data placement and replicas in the background.

## 0.18 storage contract

0.18 starts from a fresh namespace and storage layout. It deliberately refuses non-empty unversioned state/data stores rather than guessing how older state should be interpreted.

The storage model has three explicit classes:

- **DATA** — media extents, artwork, subtitles and other immutable payload objects. DATA is placed by the DHT across eligible node/backend capacity. A full preferred owner falls through to the next deterministic candidate.
- **CONTROL/METADATA** — namespace metadata plus catalogue manifests/shards. These use dedicated priority storage and metadata-voter quorum semantics; ordinary DATA quota cannot block them.
- **CACHE** — opportunistic non-authoritative copies. Cache contents never satisfy DATA or metadata durability.

`dht.min_write_replicas` is the foreground DATA publication floor. `dht.replicas` is the desired converged replica count; repair fills missing replicas after publication when policy permits degraded writes.

Small immutable objects are packed below `LocalStore`. Packing does not change `ObjectId`, DHT placement, catalogue references, replication, repair or GC.

See [Storage](docs/storage.md) and [Durability](docs/durability.md) for the precise contract.

## Documentation

- [Quick start](docs/quickstart.md)
- [Configuration](docs/configuration.md)
- [Storage](docs/storage.md)
- [Durability](docs/durability.md)
- [Cluster and recovery](docs/cluster.md)
- [Catalogue](docs/catalogue.md)
- [Streaming](docs/streaming.md)
- [Operations](docs/operations.md)
- [Architecture](ARCHITECTURE.md)
- [Security](SECURITY.md)
- [Roadmap](ROADMAP.md)
- [Current release notes](CHANGELOG.md)
- [Validation](VALIDATION.md)

The complete configuration example is [`macha.yaml.example`](macha.yaml.example).

Developed with substantial use of AI-assisted implementation.

GPL-3.0-or-later. See [`LICENSE`](LICENSE).

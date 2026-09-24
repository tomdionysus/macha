# Validation

Storage, durability and metadata behaviour is specified by invariants in
[`ARCHITECTURE.md`](ARCHITECTURE.md), [`docs/storage.md`](docs/storage.md) and
[`docs/durability.md`](docs/durability.md). Those invariants are enforced by the
ordinary regression suite, not by a separate certification run: the gate before
any deployment is a clean `./run-tests.sh build` on a host with the documented
dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMACHA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
./run-tests.sh build
```

`tests/TESTING.md` covers the runner's options, the sanitizer builds and how to
run a single case.

## What the suite pins down

The storage contract cases live in `tests/test_storage_v18.cpp`, with the
placement, metadata, catalogue and FUSE cases in their owning test files. They
exist to make a regression in one of these a test failure rather than a
production incident:

**Physical representation.** Small immutable objects are packed while keeping
their logical `ObjectId`; packed objects survive restart with the index rebuilt
by scanning pack records; tombstones and copy-on-write compaction preserve every
live object; a torn final pack record is truncated to the last valid boundary.

**Admission and capacity.** `reserve_free` refuses DATA before the filesystem
actually fills; several local backends fall through from a full one to a larger
eligible one; R=1 logical capacity is the aggregate of eligible nodes rather
than the smallest node's.

**Placement and the write floor.** An R=1 write spills from a full preferred
node to the next deterministic candidate; `min_write_replicas: 2` is satisfied
by fallback candidates when a preferred owner is full; a W=1 publication later
converges to R=2 through repair.

**Class separation.** Catalogue CONTROL objects remain writable when ordinary
DATA is full; artwork uses ordinary DATA placement and fallback and stays
remotely readable from a full node; catalogue manifest and shard CONTROL objects
recover onto a replica from another replica.

**Refusing unversioned storage.** A non-empty state namespace or DATA backend
without the current layout marker is refused rather than silently adopted.

**Memory.** Copying a `MetadataRecord` shares its immutable payload backing
instead of reintroducing namespace-sized heap amplification. The case retains
many large records and asserts an RSS ceiling on Linux, so a return to
payload-deep-copy behaviour fails in testing rather than reaching the OOM killer
in production.

**Mountpoint preflight.** An ordinary directory is accepted; an unrelated
mounted filesystem is refused and never unmounted.

## Dependencies for a complete run

`run-tests.sh` runs every test executable the build produced and nothing
else, so a complete run needs all three built: `macha-tests` (links only
`macha_core`), `macha-tests-runtime` (built only when yaml-cpp and the FFmpeg
development libraries are found) and `macha-tests-torrent` (built only with
libtorrent-rasterbar >= 2.0). With the default `MACHA_BUILD_SERVER=ON`,
configure stops with a fatal error when yaml-cpp or FFmpeg is missing. The
FUSE and torrent plugin cases in `macha-tests` depend on those plugins being
built too. A partial compile of individual translation units is a useful smoke
check while developing; it is not a suite run and must not be reported as one.

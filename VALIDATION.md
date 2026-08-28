# Validation

MachaDFS storage is specified by invariants in `ARCHITECTURE.md`, `docs/storage.md` and `docs/durability.md`. The normal regression suite includes dedicated storage-contract cases in `tests/test_storage_v18.cpp` plus related placement, metadata, catalogue and FUSE cases in the owning test files.

## Storage contract cases

The suite explicitly exercises:

- small immutable objects are physically packed while retaining logical `ObjectId` identity;
- packed objects survive restart with the index rebuilt from pack records;
- logical tombstones and copy-on-write compaction preserve every live object;
- a torn final pack record is truncated to the last valid record boundary;
- `reserve_free` rejects DATA admission before physical filesystem exhaustion;
- multiple local DATA backends fall through from a small/full backend to a larger eligible backend;
- a non-empty unversioned state namespace is refused;
- a non-empty unversioned DATA backend is refused;
- R=1 logical capacity is aggregate rather than limited by the smallest node;
- an R=1 write spills from a full preferred node to the next deterministic candidate;
- `min_write_replicas: 2` is satisfied by fallback candidates when a preferred owner is full;
- W=1 publication can later converge to R=2 through repair;
- catalogue CONTROL objects remain writable when ordinary DATA is full;
- artwork uses ordinary DATA placement/fallback and can be read remotely by a full node;
- catalogue manifest/shard CONTROL objects recover onto a metadata replica from another replica;
- metadata-record copying shares immutable payload backing and does not reintroduce namespace-sized heap amplification;
- mountpoint preflight accepts an ordinary directory and refuses an unrelated mounted filesystem.

## Release-assembly execution

The dependency-free production translation units modified by the storage work were compiled with:

```text
-std=c++20 -Wall -Wextra -Wpedantic -Werror
```

A production-code integration harness, built directly from the current `LocalStore`, `StoragePool`, `NodeRuntime`, `DistributedStore`, `MetadataManager` and `CatalogueManager` sources, passed these scenarios:

```text
pack_restart PASS
reserve_free PASS
pack_compaction PASS
pack_torn_tail PASS
r1_full_fallback PASS
catalogue_control_and_artwork PASS
catalogue_control_recovery PASS
fresh_state_refusal PASS
fresh_genesis_refusal PASS
min_write_2_fallback PASS
floor_then_r2_repair PASS
```

The current metadata regression also passed canonical hashing and in-place delta checks. Retaining 64 copies of an 8 MiB `MetadataRecord` increased RSS by 40 KiB in the final run, rather than approximately 512 MiB from deep payload copies.

The standalone mountpoint preflight proof passed classification of an ordinary directory as unmounted and `/` as an unrelated filesystem; `prepare_fuse_mountpoint()` refused to unmount `/`.

## Environment limitation

The release-assembly container does not contain `yaml-cpp`, FFmpeg development headers or userspace FUSE development headers. Consequently the ordinary CMake configure stops at `find_package(yaml-cpp REQUIRED)` and the complete `macha-tests` executable cannot be built in this environment. This is not represented as a passing full-suite run.

On a normal development host with the documented dependencies installed, the final integration gate is:

```sh
cmake ..
make -j12
./macha-tests
```

The full suite must pass there before deployment.

### 0.18.2 catalogue CONTROL publication fence

The final catalogue-GC correction was audited separately from telemetry/status. Catalogue CONTROL publication is data-before-metadata, so a future root's immutable manifest/shards are temporarily absent from the current metadata live set. CONTROL GC now derives an additional age floor from the current catalogue-root observation epoch on each metadata replica. This adds no RPC, distributed lock, metadata read, or foreground-path wait; it only makes the background local CONTROL sweep conservatively retain objects written after the currently observed root.

`src/catalogue.cpp` and the focused `test_catalogue_control_gc_protects_future_root_staging` source compile cleanly with `-std=c++20 -Wall -Wextra -Wpedantic -Werror` in the release container (using the existing test-only FFmpeg header stub). Full-suite execution remains a development-host gate because this container lacks yaml-cpp/FFmpeg/FUSE development packages.

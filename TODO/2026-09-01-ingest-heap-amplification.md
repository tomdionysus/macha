# Ingest heap-amplification remediation

Date: 2026-09-01

Status: implementation and local verification complete; corrected build
deployment and loaded four-node UAT pending.

## Diagnosis

The original overnight multi-gigabyte OOM was metadata-history retention. That
P0 is already corrected by disk-backed indexed history and a byte-bounded
materialisation cache. Loaded UAT after that correction still grew node 50 from
roughly 166 MiB cold to 1.34 GiB during eight active publications.

Two additional ingest paths were not covered by the logical DATA byte arbiter:

- every immutable extent used `std::async(std::launch::async)`, creating a new
  OS thread per extent throughout an import. Multi-megabyte storage/RPC
  allocations on those transient threads can expand and retain glibc
  per-thread arenas after the logical pipeline drains;
- FUSE copied each incoming write before broker admission. The broker bounded
  request count (up to 4,096) but not aggregate copied payload bytes, so spool
  backpressure could not bound the heap in front of the spool.

The first post-deployment restart exposed a third and larger multiplier. A cold
metadata delta reconstruction retained every complete intermediate decoded
namespace until the chain finished, then admitted all of them to a cache which
estimated each tree as `encoded payload * 4`. Five entries were reported as
117 MiB while fresh processes occupied roughly 1--3.4 GiB. The estimate ignored
tree nodes, string/vector allocations and allocator slack, and eviction ran only
after replay had already reached its peak.

The file itself is never intentionally materialised in RAM: accepted bytes are
written to the durable disk spool, replayed in 256 KiB chunks and assembled into
bounded immutable extents.

## Implemented

- [x] Replace per-extent `std::async` with one process-lifetime executor owning
  exactly `fuse.commit_workers` threads.
- [x] Bound its queue to two tasks per worker and retain exception/future
  semantics used by provisional publication.
- [x] Make executor submission cancellation-aware during service shutdown.
- [x] Add byte admission before the first FUSE write payload copy.
- [x] Add configurable `fuse.max_pending_write_bytes`, default 32 MiB, with an
  event-driven wait and explicit shutdown wakeup.
- [x] Expose current/peak/limit write-request bytes and executor
  worker/queued/active/peak/submitted counters in Status.
- [x] Document the setting and ship it in the example configuration.
- [x] Replay a metadata delta chain through one mutable working snapshot and
  retain only the requested immutable result, never every intermediate tree.
- [x] Replace encoded-size multiplication with a one-time deep structural cache
  weight covering tree nodes, strings, vectors, extents and record payload.
- [x] Stop pinning decoded accepted-head trees. Durable certificates plus the
  disk-backed history are authority; only the active current/committed view is
  a required hot materialisation.

## Verification

- [x] Regression proves a saturated spool holds exactly the configured copied
  write budget and a second request remains before-copy until a real wake event.
- [x] Concurrent publication regression proves worker count is fixed, active
  work never exceeds that count, queue depth never exceeds two tasks per worker,
  and all publications remain byte-exact and atomic.
- [x] Complete `filesystem_fuse` group: 60/60 passed serially.
- [x] Runtime/configuration suite: 3/3 passed.
- [x] Metadata/storage group: 33/33 passed serially.
- [x] Complete core suite: 253/253 passed serially.
- [ ] Synchronize the complete source tree, build all four nodes in parallel,
  install/restart, and verify identical Raspberry Pi binary hashes.
- [ ] Loaded UAT: resume rsync, observe RSS/swap, executor and write-admission
  counters through several publication cycles, and confirm viewer/control
  behaviour remains responsive.

## 2026-09-01 corrected-build deployment checkpoint

The complete source tree was synchronized to all three Linux nodes. Local
verification passed 253/253 core tests and 3/3 runtime tests. The corrected
binary is running on ES-1, GBNI-2 and node 200; the two installed Pi binaries
checked so far are byte-identical (`e0904add...1621629`). Cold post-start
footprints fell to approximately 247 MiB on ES-1, 468 MiB on GBNI-2 and 467 MiB
on node 200, versus approximately 956 MiB, 3.36 GiB and multi-gigabyte growth
before the correction.

GBNI-1/node 50 was still carrying the old 1.46 GiB process when a four-job
compile pushed the host into sustained load around 26 and made SSH stop before
its banner. Its peer telemetry remained available, but no safe remote process
control path remained. Deployment/UAT stays incomplete until the host is reset,
rebuilt with one compile job, installed and included in the hash/RSS check.

## Final heap snapshot before operator-requested shutdown

All four Macha processes were stopped on 2026-09-01 after the corrected build
still showed large RSS growth on the longer-running Linux nodes. Before
shutdown, `malloc_info` reported:

- GBNI-2: 3,639,672,832 bytes of allocator system space and
  3,459,833,379 bytes in the XML `rest` total.
- ES-1: 3,382,837,248 bytes of allocator system space and
  3,209,930,835 bytes in `rest`.
- newly restarted GBNI-1: 363,495,424 bytes of allocator system space and
  170,375,206 bytes in `rest`.

The older processes also mapped dozens of fully resident anonymous regions of
roughly 64 MiB. Those are raw allocator/map observations only. They do **not**
prove which bytes were live objects, reusable allocator space, fragmentation,
or allocations owned by external libraries, and they do not exonerate the
application. The source-wide ownership audit is recorded separately in
`2026-09-01-heap-allocation-audit.md`; it identifies both definite unbounded
owners and large copy/allocation amplifiers that require direct measurement at
their call sites.

## Remaining amplification

The transport still copies an extent into message/encryption/receiver buffers.
Those copies are now executed by fixed threads and remain indirectly bounded by
the DATA resource and executor limits, but they are not zero-copy. Remove them
only as a separate measured optimisation; changing authenticated wire ownership
is higher risk than the bounded-memory correction in this checkpoint.

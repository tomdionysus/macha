# Ownership and lifecycle remediation

Date: 2026-09-02

Status: code checkpoint complete; loaded RSS UAT remains. No deployment
authorized or performed.

The governing contract is documented in `docs/ownership.md`. This checkpoint
is deliberately resumable: each completed item has code-level ownership,
release paths, diagnostics and focused regression proof.

## Completed locally

- [x] Centralized FUSE inode reclamation behind a full quiescence predicate.
- [x] Added explicit durable namespace-operation references, installed at
  admission and released only after the journal `done` marker.
- [x] Covered final handle release, data-publication completion, metadata
  confirmation and remote namespace detachment reclamation paths.
- [x] Added current/detached/peak/reclaimed inode diagnostics and a 64-cycle
  create/close/unlink baseline regression.
- [x] Converted locked metadata reconstruction to one mutable tree, one delta
  body and one final cache insertion. Cold accepted-head validation over an
  80-delta chain retains at most the head and direct policy parent.
- [x] Replaced per-object hydration `std::async` with a fixed
  `max_inflight` executor and bounded queue; exposed worker/queue/submission/
  completion diagnostics.
- [x] Added independent byte ownership to metadata, fast-control, ordinary
  control and aggregate DATA RPC queues. Dequeue, cancellation and shutdown
  release bytes. Found and corrected shutdown's omission of `loader_requests_`.
- [x] Bounded playback probe cache, per-session subtitle cache, provider JSON
  caches and trace-only write overlap history.
- [x] Replaced read-only full catalogue copies in hydration, hint preparation,
  full scanning and management browsing with immutable shared views.
- [x] Bounded per-peer and accepted-session outbound RPC bytes and pending
  reply identities; covered enqueue, active-writer transfer, requeue,
  cancellation, disconnect and shutdown accounting.
- [x] Replaced raw libav scan/transcode/subtitle ownership with scoped owners,
  including the custom AVIO buffer leak in metadata scanning and exception-only
  frame/packet/codec/output/subtitle leaks.
- [x] Corrected curl header-list append ownership and orphan-quarantine file
  descriptor ownership on exceptional exits.
- [x] Bounded playback/media-information publication retries and torrent-search
  acquisition references.
- [x] Added a Linux per-case LSan checkpoint before the process-isolated runner
  calls `_Exit`.
- [x] Bounded synchronous DATA-retention object checks and claim batches by
  `dead_after`; timeout aborts the exact RPC route and releases pending
  ownership even while independent fast health probes remain healthy.

## Verification completed

- [x] FUSE inode ownership repeat-cycle regression.
- [x] Existing dirty-unlink and rename-over-open-inode ordering regression.
- [x] Cold accepted-head locked reconstruction ownership regression.
- [x] Existing hydration scheduler/concurrency regression with fixed-executor
  ownership assertions.
- [x] RPC isolated executor regression including byte rejection for fast
  control, control and foreground DATA.
- [x] Direct playback probe/subtitle cache ceiling assertions through the
  playback status response.
- [x] Re-walked network queues, reply maps, external handles, retry ownership,
  persistent jobs/indexes and all cancellation/exception/shutdown transfers.
- [x] Complete Mac core suite: 255/255 with the normal 12-slot scheduler in
  21.44 seconds (effective parallelism 5.04x); runtime dependencies 3/3.
- [x] Fixed the parallel harness's inherited stdio-buffer contamination, made
  bind failures report their real errno, and made final summaries repeat exact
  failed case names.
- [x] Fixed the catalogue R=1 test's topology race: metadata reference arrival
  is not proof that all nodes yet share a routable membership view. The full
  hydration/catalogue group passes 24/24 in parallel.
- [x] Fixed three architecture/load-sensitive fixture defects rather than
  serializing them: maintenance now isolates one-shot credit deadlines;
  journal-size measurement waits for admitted completion records; metadata
  burst accounting requires one shared accepted head; pending-write ownership
  activates the viewer window needed to suspend a work-conserving loader.
- [x] Complete aarch64 RPi core suite: 255/255 under ASan+LSan with the normal
  four-slot scheduler in 58.07 seconds (effective parallelism 1.72x); runtime
  dependencies 3/3 under ASan+LSan. No sanitizer or leak report.
- [x] RPi rejoin/retention regression completes in 473 ms after the bounded-call
  fix instead of hanging at the 120-second test deadline.

## Remaining diagnostics and loaded UAT

- [ ] Add current/peak byte diagnostics for live namespace/media indexes,
  storage indexes, retention state and service maintenance vectors.
- [ ] Measure external libtorrent live-session ownership separately; paired
  cleanup is not a resident-memory bound. Libav project handles now have scoped
  cleanup, but live codec-library residency still belongs in loaded UAT.
- [ ] Loaded multi-cycle RSS/swap proof remains a UAT gate and must not run until
  the processes are intentionally redeployed.

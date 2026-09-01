# Bounded shutdown and clean cluster rejoin

Date: 2026-09-01

## Problem and root cause

A normal systemd stop on node 50 (matching the earlier node-51 deployment
failure) reached `Service::stop`, waited at `service maintenance joining`, and
was killed after the one-minute systemd timeout. The retained node identity and
metadata were sound, but an unclean process termination is not an acceptable
restart contract.

Live debugger captures found two independent blocking paths in the service
maintenance owner:

1. Catalogue replica convergence could be inside a synchronous outbound RPC.
   The RPC client was closed only later by `NodeRuntime::stop`, after
   `Service::stop` had already tried to join that owner.
2. Pack compaction could be waiting without cancellation for a startup storage
   accounting scan, which took several seconds on `/mnt/diskB`.

After those waits were corrected, live UAT exposed two related lifecycle
details. Libfuse returns a positive signal number when its installed handler
deliberately exits the loop; treating every nonzero result as failure made a
clean systemd stop exit with status 8. Also, closing the RPC client once did not
prevent a maintenance pass already between stop checks from opening a new
outbound route.

## Correction

- `Service::request_stop` now cancels outbound RPC before joining any
  service-owned worker. The RPC server remains available until the ordinary
  `NodeRuntime::stop` ordering, preserving the existing durability-owner
  semantics for admitted inbound work.
- NodeRuntime records outbound cancellation as a permanent shutdown state.
  All later synchronous and asynchronous outbound calls fail immediately, so a
  racing maintenance pass cannot recreate a route after cancellation.
- Local-store pack compaction and the storage-pool traversal accept a stop
  token. A stop callback wakes the accounting condition variable and abandons
  optional compaction without waiting for reconciliation to finish.
- Positive libfuse loop results are intentional signal exits. Only negative
  errno results (or the independently recorded watchdog mount loss) are treated
  as frontend failures.
- No RPC, systemd, or client timeout was increased.

## Automated verification

- `rpc_cluster/test_service_shutdown_cancels_pending_outbound_rpc_before_join`
  gates a peer handler, proves an in-flight synchronous call is cancelled, then
  proves a new outbound call is rejected immediately after shutdown begins.
- `storage_v18/test_pack_compaction_honours_shutdown_before_accounting_wait`
  proves optional compaction honours cancellation without entering an
  accounting wait.
- `filesystem_fuse/test_fuse_signal_exit_is_a_clean_service_shutdown` proves
  zero and positive signal results are clean while a negative errno remains an
  error.
- Cluster/RPC owning suite: 38/38 passed.
- Storage-v18 owning suite: 16/16 passed.
- The final complete contention-free suite passed 245/245 in 148.555 seconds,
  including both new lifecycle regressions, and runtime dependencies passed
  3/3.

Intermediate parallel-suite failures were not hidden: three unrelated,
load-sensitive cases failed separately at higher concurrency and each passed
immediately in isolation; the contention-free serial suite then passed in
full.

## Live UAT

Node 50 alone was deployed with version 0.22.1 for the stop/restart test.

- The original build exceeded systemd's 60-second stop timeout and was killed.
- The first corrected cut exposed the accounting wait; after cancellation was
  threaded through compaction, stop completed in 758 ms but systemd correctly
  revealed the separate FUSE exit-status bug.
- The completed cut stopped Macha in 215 ms. `systemctl` reported
  `Result=success`, `ExecMainStatus=0`, `ActiveState=inactive`, and the host
  remained online.
- On restart, node 50 retained its identity, joined rather than forming genesis,
  became online/writable at metadata generation 1643, and saw all four nodes.
- Node 51 independently reported node 50 online at generation 1643 and the
  cluster writable.

This demonstrates both halves of the required lifecycle: bounded clean process
shutdown and automatic adoption of the existing cluster generation after a
restart.

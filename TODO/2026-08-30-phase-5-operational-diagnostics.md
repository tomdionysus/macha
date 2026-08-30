# Phase 5 operational diagnostics checkpoint

Date: 2026-08-30

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope

This checkpoint closes the observability gap which prevented the final
three-node comparison from measuring accepted-head durability and RPC execution
latency. It adds bounded process-lifetime aggregates only; it does not add a
sampler, timer, maintenance loop, per-request history, metadata publication, or
gossip traffic.

## Implementation

- `MetadataReplicaDiagnostics` now reports successful accepted-head persistence
  writes, encoded bytes, and failed writes. Successful totals advance only
  after the durable replacement succeeds; failures are counted and rethrown.
- `RpcServer` records execution count, queue-wait total/max, and handler-time
  total/max in microseconds for each frame class and wire message type.
- RPC timing uses fixed-size atomic buckets. Handler execution does not acquire
  a diagnostics mutex or allocate a per-request record.
- Rejected metadata work remains visible through `metadata_rejected_jobs` but is
  deliberately absent from executed timing counts.
- `GET /api/v1/status` exposes the local aggregates under
  `diagnostics.metadata` and `diagnostics.rpc_server`. Metadata readiness is
  explicit, so Status remains safe during startup recovery.

## Deterministic verification

- `storage_metadata/test_metadata_commit_store_acceptance_heads_roundtrip`
  proves an invalid certificate causes no accepted-head write, while each valid
  accepted-head change advances writes and bytes exactly once with no failure.
- `rpc_cluster/test_rpc_metadata_mutations_use_bounded_isolated_executor`
  proves each admitted metadata mutation is recorded exactly once, rejected
  work is not executed, gated queued work has non-zero queue time, and CONTROL
  and foreground frame classes remain separately visible.
- `invariants/test_status_uses_membership_without_telemetry` proves the new
  status objects and accepted-head counters are available without changing the
  existing membership/telemetry contract.

## Verification record

- Build completed successfully.
- All three focused tests above passed.
- Runtime dependency suite passed 3/3.
- The first complete default-suite run passed 202/203 and timed out in
  `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`.
  That test then passed explicitly in isolation in 1.024 seconds. No code was
  changed in response to the timeout.
- A second complete default-suite run passed 203/203. In that run the same
  catalogue burst passed in 1.147 seconds and
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed in
  6.401 seconds.
- After formatting and rebuilding, the explicit 1,000-operation FUSE recovery
  test passed in 1.522 seconds, the lagging-third 64-generation RPC recovery
  test passed in 1.497 seconds, and
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed
  explicitly in 6.600 seconds.
- After tightening handler timing to exclude reply delivery, the complete
  `rpc_cluster` group passed 34/34.

The first failure is retained here because one successful rerun does not erase
the evidence of an intermittent suite-load timeout.

## Continuation boundary

Deploy this build to all three nodes before the next operational comparison.
Take one status sample immediately before a controlled recovery/deletion burst,
samples during the burst, and a final pair after drain. Compare deltas for
accepted-head writes/bytes and RPC message/frame timings with existing FUSE
publication, journal-barrier, convergence, CPU, RSS, and namespace counters.
The second settled sample should show no counter movement except explicitly
scheduled physical GC or new external work.

Physical-object GC must be measured separately from namespace publication: it
is intentionally paced and can continue after the namespace backlog is fully
committed.

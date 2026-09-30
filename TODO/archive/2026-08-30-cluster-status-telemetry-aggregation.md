# Cluster Status telemetry aggregation correction

Date: 2026-08-30

## Required contract

One `GET /api/v1/status` request must report the best locally known status of
every known cluster node. A client must not query each node and merge the
responses itself. The HTTP handler remains an in-memory read: connected nodes
disseminate their bounded telemetry through the existing authenticated cluster
control plane.

Null/unavailable fields remain necessary during startup, disconnection, or
before the first observation. They are a truthful fallback, not the expected
steady state for connected nodes.

## Root cause

The data model and `TelemetryStore` already supported cluster-wide aggregation.
The failure was in RPC notification delivery.

`NodeRuntime::telemetry_loop` encoded recent observations and called
`RpcClient::broadcast_best_effort`. That function sends notifications with RPC
request ID zero. Both possible receive paths treated zero as the notification
namespace, but their hard-coded dispatch lists handled metadata notices,
session retirement, promotion, and cancellation only. A `telemetry`
notification was therefore successfully written to the authenticated channel
and then silently discarded by the receiver.

Because canonical route reconciliation may retain either the dialled or
accepted direction, correcting only one reader would have left the defect
nondeterministic between node pairs.

## Correction

- Both the outbound `PeerConnection` reader and accepted `RpcServer` session
  reader now recognize telemetry notifications.
- Notifications are admitted to the existing bounded speculative RPC queue.
  Decoding and `TelemetryStore` mutation do not run on socket-reader threads.
- Notification completion has no reply route; overload may still drop this
  observational work without blocking or disconnecting operational traffic.
- An authenticated peer observation advances an edge-triggered telemetry
  demand epoch and wakes the telemetry worker. Newly connected peers therefore
  receive a coalesced current observation without waiting for the next
  periodic local metrics sample.
- The Status HTTP request still performs no network or disk I/O and never fans
  out to peers.

Periodic local sampling remains because CPU/load/resident-memory measurements
change without a Macha domain event. Peer connection dissemination is
event-driven, and the existing sampler deadline is also the bounded eventual
retry if a best-effort notification loses admission under useful work.

## Tests

Added
`rpc_cluster/test_best_effort_telemetry_notifications_reach_both_route_directions`.
It establishes one authenticated connection and proves delivery through both
the dialler-to-acceptor and acceptor-to-dialler canonical route directions.

Added
`invariants/test_status_collects_connected_peer_telemetry_without_client_fanout`.
It starts two connected services, waits for automatic telemetry exchange, then
queries only the first node's HTTP endpoint and verifies that the second node
has live, numeric storage/cache/backend measurements and that the online
cluster aggregate is available.

Verification completed:

- both new focused regressions: 1/1 passed;
- complete `rpc_cluster`: 35/35 passed;
- complete `invariants`: 36/36 passed;
- runtime-dependency suite: 3/3 passed; and
- `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed in the
  repository-wide run.

The repository-wide default run was **not** clean: 206/207 passed.
`hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
reproduced its already-tracked suite-load timeout at 10.407 seconds, then
passed immediately in isolation in 0.890 seconds. The failure and its output
were preserved and remain active; this checkpoint does not describe the full
suite as passing. A second complete run with concurrency reduced from 12 to 4
also finished 206/207 and reproduced the same assertion at 10.415 seconds.
This is now a repeatable concurrent-suite failure, despite the fast isolated
pass, and must be diagnosed rather than dismissed as incidental timing noise.

## Deployment/UAT result

The deployed three-node UAT passed from all three endpoints. Every independent
response contained all three connected nodes with live numeric storage,
cache, and backend telemetry; online and known aggregates were complete and
identical. The cluster was healthy and writable at generation 1487.

Evidence: [Cluster Status telemetry aggregation UAT](2026-08-30-cluster-status-telemetry-aggregation-uat.md)

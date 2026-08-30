# Phase 4 bounded metadata RPC executor

Date: 2026-08-30

## Scope

This checkpoint isolates inbound metadata mutation and durability work from the RPC execution lanes used by health, membership, ordinary control requests, and foreground object service.

The isolated message types are:

- `put_metadata_history_entry`;
- `put_metadata_commit`; and
- `accept_metadata_commit`.

Wire reading, authenticated frame assembly, message-size enforcement, and admission remain on the transport/session path. Decoding, history import, commit storage, reconstruction needed by acceptance, durable acceptance, and reply construction run on the metadata executor.

## Executor design

- The default node has one dedicated metadata worker.
- The pending queue accepts at most 64 jobs and 256 MiB of payload.
- Both limits must be non-zero and are validated when the RPC server is constructed.
- An over-limit request receives an immediate `error` reply. The connection remains usable; overload does not tear down or retire the peer session.
- Queue statistics expose pending jobs, pending bytes, active jobs, and rejected jobs for deterministic tests and later operational diagnostics.
- Replies are naturally asynchronous: the session reader enqueues the request and continues; the metadata owner queues the reply only after the handler finishes.

One default worker minimizes idle thread and stack overhead and gives the production path an unambiguous mutation order. The executor nevertheless supports an explicit larger worker count. In that mode, one peer retains FIFO ownership across history/store/accept operations, while jobs from different peers may execute concurrently. This prevents an acceptance from overtaking its preceding commit from the same peer.

There is no wire-format, protocol-version, or on-disk-format change in this slice.

## Backpressure and lifecycle semantics

- Queued work has not crossed a durability boundary and can be removed by transfer cancellation.
- Once a worker owns a job, cancellation or peer disconnect detaches the reply route but does not interrupt the handler across an unknown or irreversible durability boundary.
- During server shutdown, the running owner is allowed to finish. Queued jobs tied to closed sessions are abandoned before entering the handler.
- Fast health/membership workers, ordinary control workers, and the foreground-reserved DATA workers never execute these metadata mutations.

## Recovery transfer pipeline

History discovery still walks the immutable dependency graph in dependency-first
order, but missing entries are no longer uploaded with one network round trip per
entry. The sender submits up to eight consecutive history mutations asynchronously.
The receiver's per-peer FIFO guarantee preserves dependency order, while the fixed
window bounds encoded payloads and outstanding RPC state. Sender diagnostics expose
transfer count, submitted entries, and peak in-flight depth.

## Replica critical-section reduction

Cold materialization now copies the required immutable history chain under the
replica mutex, performs snapshot decoding, delta application, encoding, and hash
validation outside that mutex, then installs still-present results through a short
validated cache step. A separate computation owner deduplicates simultaneous cache
misses; eight readers of one cold hash share one calculation.

Inbound history import and commit storage likewise validate/reconstruct and encrypt
their entry before acquiring the durable mutation owner. The history append and
`fsync` execute without the global state mutex; afterward a short state section
publishes the immutable entry, counters, and materialization. Acceptance prewarms
the target and policy-parent materializations through this off-lock path.

Legacy journal/checkpoint APIs remain serialized by the durability owner. Broader
delta-native encoding and checkpoint redesign remain listed under the Phase 1 and
deferred architecture follow-ups rather than being hidden inside this RPC slice.

## Fast-control contract

The complete production allow-list is CONTROL-frame `ping` and `members`. These
handlers may read published in-memory state only; they may not perform filesystem
I/O, durability, network fan-out, reconstruction, or backlog-dependent work. The
operational contract and extension-test requirement are documented in
`docs/operations.md`.

## Tests

`rpc_cluster/test_rpc_metadata_mutations_use_bounded_isolated_executor` proves:

- all three mutation RPCs use the metadata executor;
- one active plus two queued jobs match configured accounting;
- both job-count and payload-byte overload return an RPC error;
- rejected work never reaches the handler;
- ping, membership, ordinary control, and foreground DATA calls complete while metadata is gated; and
- CONTROL and DATA sessions remain usable after backpressure.

`rpc_cluster/test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers` proves:

- a second request from one peer cannot overtake that peer's blocked owner; and
- a second configured worker can serve a different peer concurrently.

`rpc_cluster/test_rpc_metadata_executor_cancellation_and_disconnect_boundaries` proves:

- queued work is removed by cancellation without handler entry; and
- running work completes its durability section after the reply route disconnects.

`rpc_cluster/test_rpc_metadata_executor_shutdown_finishes_owner_and_drops_queue` proves:

- shutdown detaches replies;
- the running owner completes; and
- queued work from the closed session never enters the handler.

## Verification

- The four new gated executor tests passed together.
- The complete `rpc_cluster` suite passed 32/32 before the final lifecycle tests were added; those lifecycle tests then passed together with the other executor tests.
- The first complete repository run found `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` timing out only under full parallel load. It passed alone in 0.89 seconds. The cause was the initial two-worker default multiplying idle threads across the large test cluster population.
- After changing the production default to one metadata worker, `sh run-tests.sh build` passed all 203 default tests and all 3 runtime-dependency tests.
- The final complete run explicitly passed both `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`.

## Completion verification

- `storage_metadata` passed 27/27 after the off-lock durability and materialization changes.
- `storage_metadata/test_metadata_delta_chain_reuses_bounded_materialized_head` proves eight simultaneous cold readers produce exactly one 200-delta reconstruction and return the same shared materialization.
- `rpc_cluster/test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` proves final-state convergence and an observed sender window greater than one and no greater than eight.
- The first complete run exposed a real lost-wake edge: a coalesced metadata run could leave its one follow-up pending without an external event to wake maintenance. The deadline calculation now treats undelayed dirty metadata as runnable immediately.
- `filesystem_fuse/test_disconnected_maintenance_sleeps_until_peer_event` proves that correction does not reintroduce polling or idle spin.
- The final `sh run-tests.sh build` passed 203/203 default tests and 3/3 runtime-dependency tests. The formerly failing catalogue burst completed in 813 ms under full-suite load, and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` also passed.

This phase is ready for a short three-node UAT: create a lagging replica, generate a
linear metadata backlog on the surviving pair, rejoin it, and sample status/RPC
latency, CPU, convergence counters, and return-to-idle behaviour throughout catch-up.

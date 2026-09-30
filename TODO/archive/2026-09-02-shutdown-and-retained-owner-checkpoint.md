# Shutdown lifecycle and retained-owner checkpoint

Status: locally complete; guarded attribution UAT is useful

Date: 2026-09-02

## Hydration shutdown correction

The live `hydration executor is stopping` abort was a deterministic race. The
scheduler could pass its stop-token check, block inside a hint provider, and
then submit after `request_stop()` had closed the fetch executor. `submit()`
threw through the `std::jthread` entry function, invoking `std::terminate`.

Late submission is now normal cancellation rather than an exception. Stop owns
and completes queued promises before joining the scheduler, so queued futures
cannot deadlock shutdown. Executor cancellation and rejection have separate
operational counters. A blocked-provider regression forces the exact ordering:
collect hints, request stop, release the provider, reject the late submission,
and join cleanly with no fetch and no retained queue entry.

## O(1) retained-owner diagnostics

Status now exposes current FUSE retained ownership without scanning the inode
table:

- `retained_data_operations`
- `retained_data_operation_bytes`
- `retained_overlay_ranges`
- `retained_overlay_bytes`
- `retained_publication_operations`
- `retained_publication_operation_bytes`
- `retained_durability_tickets`
- `data_publication_inflight_bytes`

The byte counters include vector capacity and checksum allocations, not merely
logical element payload. Per-inode accounting is adjusted while its mutex owns
each transition and is released before table reclamation. Empty completed
`DataOp` vectors now release their capacity rather than retaining the largest
historical write generation for the lifetime of every clean inode.

The playback Status endpoint now exposes aggregate active-pipeline ownership:

- `segment_store_resident_bytes`
- `segment_store_spill_bytes`
- `segment_store_descriptor_bytes`
- `segment_store_segments`
- `segment_store_planned_segments`

These are maintained by the fragment store itself and do not enumerate files or
fragments during Status requests.

## Verification

- Hydration blocked-provider shutdown race: passed.
- Media segment resident/spill/backpressure ownership: passed.
- Indexed FUSE overlay retained-owner assertions: passed.
- Status contract for all new filesystem counters: passed.
- Focused filesystem/FUSE suite: 62/62 passed in the parallel isolated runner.
- Complete core suite: 260/260 passed in the parallel isolated runner.
- Runtime-dependency suite: 3/3 passed.

The first complete run had 259 passes and one unrelated same-generation sibling
reconciliation process hit its 60-second runner timeout. The exact case then
passed in 522 ms, and the subsequent complete parallel run passed 260/260. This
is recorded explicitly rather than being silently treated as a clean first run.

## Next UAT boundary

A short guarded attribution run is useful now because it can distinguish FUSE
operation/snapshot retention and playback fragment residency from other heap.
It is not permission for overnight ingest and it is not the Phase 3 stable-RSS
exit gate. Use the same 1.25 GiB and consecutive-growth stop conditions as the
failed first UAT. If RSS growth materially exceeds the sum of the newly visible
owners, continue the process-wide ledger into RPC/object buffers and
metadata/catalogue caches before changing concurrency or throttle policy.

# Phase 1D.1 checkpoint: DATA-work provenance

Date: 2026-08-31

Status: first Phase 1D.1 implementation cut complete locally; Phase 1D.1 and the
Phase 1D invariant gate remain open.

## Problem reproduced

The outer FUSE publication scheduler classified spool publication as loader
work, but `WriteHandle` discarded that provenance. Its append-tail fetch and
whole-file materialisation reads were hard-coded as `FrameType::read_ahead`,
and every write refreshed the interactive-activity signal. A loader quantum
could therefore promote its nested DATA RPC/storage work and manufacture false
viewer demand.

## Implementation

- Added immutable `DataWorkContext` state to a write generation. It currently
  carries the DATA frame class and the byte quantum granted at admission.
- Rejected `FrameType::control` at this DATA-only boundary.
- Passed an explicit loader context and the configured publication quantum from
  FUSE spool publication into `WriteHandle`.
- Preserved the context through asynchronous and synchronous extent puts,
  append-tail fetches, materialisation reads, rebuild puts, and the distributed
  durability barrier.
- Removed unconditional interactive activity from writes. Only explicit
  foreground/read-ahead contexts now update their corresponding viewer signal.
- Exposed frame class and admitted quantum in per-handle diagnostics so the
  provenance can be asserted without timing-sensitive observation.

This changes no wire value or transport negotiation. Existing CONTROL/DATA
lanes and dedicated control executors remain intact.

## Deterministic verification

`filesystem_fuse/test_write_data_work_context_preserves_loader_provenance`
forces a non-sequential overwrite through materialisation and rebuild. It proves:

- CONTROL cannot construct a DATA-work context;
- the handle retains loader class and its admitted byte quantum;
- two source extents are materialised without recording foreground or
  interactive viewer activity; and
- an explicitly interactive DATA context still records interactive bytes.

Verification run after a clean incremental build:

- `filesystem_fuse/`: 51/51 passed;
- `rpc_cluster/test_rpc_foreground_not_starved_by_busy_data_workers`: passed;
- `rpc_cluster/test_rpc_health_and_control_not_starved_by_data`: passed.

The complete project suite was not run at this checkpoint.

## Work deliberately still open

This cut propagates class provenance but does not make a whole-file operation
pre-emptible. `DataWorkContext` still needs its cancellation/deadline,
consumable CPU/byte budget, and accounting ticket. Potentially blocking
`DistributedStore` DATA APIs still retain loader-default compatibility
overloads rather than requiring a context. Loader-originated metadata admission
is not yet origin-aware. `materialize()` and `rebuild()` remain synchronous and
can exceed the recorded quantum. These are the next Phase 1D.1/1D.2 cuts; this
checkpoint must not be used to claim that viewer/control priority is already
non-bypassable end to end.

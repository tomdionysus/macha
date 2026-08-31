# Phase 1D.3 checkpoint: DATA resource headroom

Date: 2026-08-31

Status: first end-to-end resource-admission cut complete locally; Phase 1D.3
and the Phase 1D invariant gate remain open pending deployed UAT and the
remaining resource-specific work.

## Problem closed by this cut

RPC worker priority alone did not constrain loader work after admission. A
loader could occupy every blocking local-store/object-transfer operation, so a
later viewer request could enter the foreground RPC worker and still wait for
the same physical work. CONTROL remained separately executable, but viewer
headroom was not an end-to-end invariant.

## Implementation

- Added immutable DATA provenance with class, byte quantum, steady-clock
  deadline and cancellation signal in `DataWorkContext`. CONTROL is rejected at
  this boundary and continues to use its independent transports, executors and
  control store.
- Added an event-driven node-wide `DataResourceArbiter`. Admission is bounded by
  bytes, not operation count. Foreground/read-ahead may use the full configured
  budget; loader/speculative work cannot consume the non-borrowable viewer
  reserve. Waiting viewers close new lower-class admission, and waiting loaders
  precede speculative work.
- There is no maintenance or admission polling loop. Releases, stop and exact
  deadlines wake condition-variable waiters. Oversized work which cannot ever
  fit its class budget fails immediately rather than parking forever.
- Applied credits to distributed puts/gets, direct repair placement, incoming
  DATA get/put handlers, ordinary local-store reads, persistent-cache reads and
  asynchronous fetched-object persistence. A remote transfer is bounded at
  both participating nodes. DATA objects are explicitly limited to the
  configured extent size.
- Preserved existing RPC worker reservation: two of eight DATA workers remain
  foreground-reserved. The new byte boundary is below that executor admission.
- Added configuration under `dht`: `data_inflight_bytes` (default 128 MiB) and
  `data_viewer_reserve_bytes` (default 32 MiB). Validation requires the
  non-viewer remainder to fit a complete configured extent.
- Exposed capacity, reserve, current/peak use and class admission/wait counters
  under Status `diagnostics.data_resources`.

## Deterministic proof

`foundations/test_data_resource_arbiter_reserves_viewer_headroom` saturates all
lower-class capacity, proves a later viewer immediately consumes its reserve,
proves bounded deadline expiry without a polling cadence, proves loader before
speculative ordering, and proves impossible oversized requests fail.

`rpc_cluster/test_storage_data_credit_reserves_viewer_headroom_and_control`
saturates a real node's lower DATA byte budget, blocks a loader object read,
then proves both a foreground object read and CONTROL ping complete using their
reserved paths before the loader is released. It also checks exact peak byte
use and wait/admission counters.

The Status invariant test now verifies the operational diagnostics contract,
and runtime configuration coverage verifies parsed byte sizes.

## Verification

- Build: complete.
- Focused DATA arbiter test: passed.
- Focused distributed storage-credit/control test: passed.
- Runtime YAML configuration test: passed.
- First complete default run at eight slots: 223/225. The operation-journal
  size assertion and the already tracked catalogue final-state timeout failed;
  both passed immediately in isolated reruns.
- Clean complete default rerun at four slots: 225/225, including
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` and both
  initially failing cases.
- Runtime dependency suite: 3/3.

## Remaining Phase 1D work

This cut provides node-wide object/storage-transfer headroom. It does not yet
provide independent per-peer or per-storage-domain credits, byte-bounded hash
and durability-barrier admission, completion tickets which release executors
while durable work is outstanding, origin-aware metadata admission, or maximum
queue-delay/uninterrupted-service measurements. Those remain active Phase
1D.1/1D.3 work. The existing 1 ms put-quorum completion polling remains the
separately recorded Phase 4B item.

## UAT gate

A short loaded UAT is useful and required before the next resource cut. Run the
existing `rsync --append-verify` ingest together with real playback and repeated
seeks. Verify:

1. Status `diagnostics.data_resources` shows loader waits and a bounded peak no
   greater than capacity.
2. Playback starts and seeks while loader work remains in progress; it does not
   wait for the loader queue to drain.
3. Loader publication continues with a non-zero share during viewing and
   accelerates when the viewer becomes idle.
4. CONTROL/status remains responsive, with no RPC timeout or connection churn.

If playback still stalls with viewer admissions present, the next diagnosis
should correlate the stall with storage domain/device and durability counters;
that would identify the next resource-specific boundary rather than weakening
the viewer reserve.

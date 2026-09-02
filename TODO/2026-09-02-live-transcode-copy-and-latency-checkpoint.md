# Live transcode copy and latency checkpoint

Status: **implemented, deployed and UAT passed**

Date: 2026-09-02

## Evidence and scope

The preceding UAT passed physical teardown but measured GBNI-1 rising from
approximately 398 MiB to 783 MiB during one transcode, with 7.56 seconds to the
first fragment. About 79 MiB belonged to the segment store. The remaining live
cost includes libav/x264 working state and repeated remote 4 MiB object-reply
ownership.

This checkpoint corrects two concrete interactive-path multipliers without
changing playback selection or lowering viewer priority.

## Remote object ownership

An object handler returned an owning `RpcMessage`, but the shared inbound reply
callback accepted it by `const&`. Queue admission therefore copied the complete
reply. After receipt, `Reader::bytes()` allocated and copied the complete object
again.

Reply callbacks now take ownership by value and move the handler result into
the bounded transport queue. The receiver validates the unchanged
`ObjectId + length + bytes` wire representation in place, verifies the content
hash, slides the bytes over the 36-byte prefix, and returns the same allocation.
This removes two full-object allocations/copies per remote extent while
preserving encryption, fragmentation, integrity and cancellation semantics.

## Interactive encoder latency

The H.264 encoder used x264's `veryfast` preset but retained the ordinary
look-ahead pipeline. That policy is suitable for offline compression, not for
an interactive fragmented stream: it retains decoded frames and delays the
first output for compression efficiency the viewer cannot use.

When the selected encoder is `libx264`, Macha now requires the supported
`zerolatency` tune in addition to `veryfast`. Bitrate/CRF, resolution, GOP,
stream selection and admission remain unchanged. Frame threading remains
available. Debug diagnostics record the selected decoder and encoder plus their
reported thread counts and whether zero-latency mode was selected.

## Verification

- new 4 MiB fragmented DATA-reply ownership/reuse regression: 1/1 passed;
- three-node remote storage/read integration: 1/1 passed;
- complete RPC/cluster group: 40/40 passed;
- complete core suite: 262/262 passed;
- concrete libav/configuration runtime suite: 3/3 passed;
- project version: 0.22.4.

## UAT gate

Deploy the complete source to all four nodes and repeat the same transcode on
GBNI-1. Record:

- session-create and first-fragment latency;
- live RSS, thread count, segment resident/spill bytes and CPU;
- the `libav interactive video codec` diagnostic;
- canonical DATA connection count; and
- post-stop heap-reclamation counters and settled RSS.

The expected direction is a lower live peak and materially faster first
fragment. This checkpoint does not claim the whole 385 MiB live increase is
removed. If codec working memory still dominates, add explicit codec-thread and
active transport/reassembly ownership diagnostics before choosing any thread
cap; do not trade away real-time viewer throughput on assumption.

## Deployment UAT result

The complete 0.22.4 source was built on all three Raspberry Pis in parallel.
Their installed candidate binaries were byte-identical with SHA-256
`a6a56a90a9c5c52f84a6565ce8293da9b40ceb606aaa27c3ed7380617f79b4d7`.
All four nodes converged healthy and writable at generation 3015 before the
test.

GBNI-1 began at approximately 379 MiB RSS and 108 threads. For the same
transcode shape used by the prior checkpoint:

- first fragment improved from 7.47 seconds to 3.44 seconds;
- total session admission improved from 7.56 seconds to 3.53 seconds;
- live RSS plateau improved from approximately 783 MiB to 673 MiB;
- live threads were 118-119, effectively unchanged from the prior 119;
- current process CPU was approximately 191-200%, about two cores;
- segment ownership at the later sample was about 60 MiB resident and 73 MiB
  spill;
- the codec diagnostic reported HEVC decoder thread count 1, libx264 private
  thread management, and `zero_latency=1`; and
- no duplicate same-peer DATA transport appeared.

After the client stopped playback, Status reached zero sessions, transcodes and
segment ownership. Heap reclamation recorded one request, one run and one
successful release. RSS fell immediately to approximately 414 MiB and remained
approximately 415 MiB after thirty seconds; threads settled at 109. The cluster
remained healthy, writable and four-node online.

This checkpoint passes. Roughly 250-295 MiB of live process growth remains
beyond the segment store, depending on which baseline/plateau sample is used.
The unchanged codec/transport thread count means the next step should expose
active transport/reassembly bytes and codec working ownership before applying
any concurrency cap.

# Isolated rsync and playback attribution UAT

Status: **playback-local amplification reproduced; rsync-only memory stable**

Date: 2026-09-02

## Method

The earlier combined rsync plus playback run could not distinguish ingest,
playback, or an interaction. All four nodes were therefore restarted between
two isolated workloads. Each run began only after the cluster was healthy and
writable and GBNI-1 RSS had settled. The existing 1.25 GiB and consecutive
128 MiB/minute growth stop gates remained in force.

## Rsync-only result

- Settled GBNI-1 baseline: approximately 415 MiB RSS.
- Loaded plateau: approximately 445 MiB RSS.
- 7,728 additional pending write records increased retained operation
  ownership by less than 0.4 MiB.
- Indexed append-verification reads continued to examine approximately one
  overlay range per query.
- Status remained healthy, writable and sub-millisecond locally; swap did not
  grow.
- At 17,179,856,131 spool bytes against the configured 16 GiB limit, rsync
  stopped showing progress. This is separately recorded in `ACTIVE.md` as the
  smooth-backpressure defect.

Rsync alone did not reproduce the rapid RSS increase.

## Transcode-only result

- Clean settled GBNI-1 baseline: approximately 386 MiB RSS.
- First transcode sample: approximately 710 MiB RSS.
- Peak/plateau samples: approximately 777-790 MiB RSS.
- The live segment store accounted for about 27 MiB initially and approximately
  145 MiB resident plus spill at the later plateau; descriptor ownership was
  negligible.
- Linux `smaps_rollup` reported about 750 MiB private anonymous RSS.
- Thread count increased from the 99-thread idle peer baseline to 119.
- Status remained healthy and responsive. Swap did not grow.

After playback stopped, Macha reported zero sessions, zero video transcodes,
zero segments, zero planned segments and zero resident/spill bytes. RSS fell to
approximately 592 MiB but did not return to the pre-playback baseline after the
confirming interval. Thread count fell only to 109.

The ten remaining threads were eight unnamed/futex-waiting workers plus one
additional accepted peer reader/writer pair. Live playback had opened multiple
persistent DATA connections. The transport currently materialises a remote
4 MiB extent repeatedly: local-store output, encoded reply, encrypted wire
fragment(s), reassembly payload, decoded `Reader::bytes` output and integrity
verification all have overlapping ownership. Queue diagnostics exclude active
and transient wire/reassembly/handler payloads.

## Conclusion and continuation

The dominant failure is playback-local, not an rsync memory leak and not an
interaction prerequisite. The existing combined run's larger peak can include
additional pressure, but does not need to be repeated.

Continue the primary structural plan at RPC/object-buffer ownership:

1. expose active and retained RPC payload/reassembly bytes and live transport
   sessions by lane;
2. account for media-engine/libav working memory separately from the segment
   store;
3. remove avoidable full-stripe copies and ensure cancelled playback retires or
   reuses bounded DATA transport ownership;
4. replace thread-per-connection/payload ownership where necessary with the
   bounded executor and process-wide byte ledger from Phases 1 and 3;
5. add teardown regressions proving logical session deletion also returns all
   Macha-owned buffers, requests and connection work to their bounded idle
   state.

Do not tune spool throttling or lower playback priority to disguise this result.

## First correction checkpoint

The first concrete multiplier is now corrected locally. Concurrent cold calls
previously performed the canonical-route check independently, then all dialled
and authenticated before any winner was installed. The live trace showed four
equivalent DATA connections created in the same second. Each loser still owned
two transport threads and exercised the fragmented object-reply buffer path
before retirement.

Connection creation is now single-flight per authenticated peer and transport
lane. Waiters sleep on the in-progress dial and then re-evaluate outbound,
inbound and retry-backoff state. Different peers and CONTROL/DATA lanes remain
independent, so the correction does not serialise unrelated cluster work.

A 24-caller cold-DATA regression proves all replies complete through exactly
one created, canonical connection. Verification after the change:

- focused concurrent-dial regression: 1/1 passed;
- complete RPC/cluster group: 39/39 passed;
- complete core suite: 261/261 passed;
- runtime-dependency suite: 3/3 passed.

A clean playback-only deployment UAT confirmed the transport fix: the cold
transcode created exactly one outbound DATA connection to its serving peer.
The remaining result was nevertheless approximately 411 MiB idle, 757 MiB
live, and 653 MiB after teardown. Status reported zero sessions, transcodes and
segment-store bytes after teardown, and thread count returned to 109 (the two
threads above the clean 107 baseline belong to the one expected canonical DATA
connection).

A one-shot diagnostic `malloc_trim(0)` on the stopped Linux process returned
success and immediately reduced RSS from approximately 653 MiB to 395 MiB
without disrupting the API. This proves that the post-teardown difference was
free memory retained by glibc arenas, not a still-live Macha owner. It does not
explain away the approximately 346 MiB live transcode working set, which
remains separate primary work.

The second correction is now implemented locally. Every transformed-pipeline
teardown requests heap reclamation from the existing event-driven
`macha-play-gc` thread. The request is deferred while any transformed engine
session remains active; no admission, seek, streaming, control or engine-stop
thread calls the allocator reclamation operation. Linux/glibc builds invoke
`malloc_trim(0)` only after the final transformed pipeline has physically
stopped and released its ownership. Other allocators retain their native
behaviour. Status exposes pending, request, run and successful-release counters.

The idle-pipeline regression caught and now covers the distinct GC path that
moves a pipeline out of its session before stopping it. Verification after the
correction:

- focused idle-transcode teardown regression: 1/1 passed;
- complete media/playback group: 23/23 passed;
- complete core suite: 261/261 passed;
- runtime-dependency suite: 3/3 passed.

## Second correction deployment UAT

The correction was deployed from the complete source tree to all four nodes.
The three identical Raspberry Pi builds had the identical SHA-256
`698e10da5b5d4ca43ff63cd348e4b1a735b2e42c45de448b154a68168cb6ac41`.
The cluster converged healthy and writable with four online nodes at generation
3015 before playback began.

GBNI-1 measured approximately 398 MiB RSS and 107 threads before playback. The
transcode reached approximately 783 MiB RSS and 119 threads, with about 61 MiB
resident plus 18 MiB spilled segment data. It used one canonical outbound DATA
connection. The run was not perfectly workload-isolated because two recovered
spool publications continued after restart, although no rsync process was
running; this does not invalidate the teardown ownership result.

When the client stopped playback, session, transcode, segment and spill
ownership all reached zero. Status then reported exactly one heap-reclamation
request, one run and one successful release. The GC-thread log recorded
`playback post-transcode heap reclaim released=1`. RSS immediately fell from
approximately 783 MiB to 406 MiB and was approximately 423 MiB thirty seconds
later while recovered spool publication continued. Threads returned to 109,
the expected baseline plus the canonical DATA reader/writer pair.

This UAT passes the physical teardown gate. Continue with accounting and
bounding the live media-engine and RPC/object-reply working set; allocator
reclamation is not a substitute for reducing the approximately 385 MiB live
increase or improving the observed 7.56-second transcode startup.

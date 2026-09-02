# Transport and allocator attribution checkpoint

Date: 2026-09-02  
Target: Macha 0.22.5

## Outcome

The next transcode UAT can attribute memory still owned by RPC transport and by
glibc instead of inferring ownership from process RSS. This is instrumentation,
not a resource cap, and it does not change viewer scheduling or codec thread
counts.

## Implementation

- Every dialled and accepted CONTROL/DATA connection reports its current full
  payload ownership: queued outbound payloads, the payload actively being
  written, the decrypted fragment being processed, incomplete reassembly,
  pending replies, active inbound requests and active writes.
- Accepted connections are included through weak diagnostic routes, so Status
  does not extend a connection's lifetime.
- Per-connection locks are never acquired while holding the client routing
  lock.
- Linux/glibc allocator attribution reports arena bytes in use, free bytes
  retained in arenas and direct mmap bytes. Sampling occurs on the existing
  low-priority five-second telemetry worker. Status only reads the cached
  snapshot.
- Allocator attribution is local-only and is not added to the fixed peer
  telemetry wire record, preserving rolling-upgrade compatibility. Platforms
  without `mallinfo2`, including macOS, report it as unavailable rather than
  zero.

The Status fields are:

- `diagnostics.rpc_transport.control_connections`
- `diagnostics.rpc_transport.data_connections`
- `diagnostics.rpc_transport.queued_outbound_bytes`
- `diagnostics.rpc_transport.active_outbound_bytes`
- `diagnostics.rpc_transport.received_fragment_bytes`
- `diagnostics.rpc_transport.reassembly_bytes`
- `diagnostics.rpc_transport.pending_requests`
- `diagnostics.rpc_transport.active_inbound_requests`
- `diagnostics.rpc_transport.active_writes`
- `diagnostics.process_memory.allocator_available`
- `diagnostics.process_memory.sampled_at_unix_ms`
- `diagnostics.process_memory.allocator_arena_in_use_bytes`
- `diagnostics.process_memory.allocator_arena_free_bytes`
- `diagnostics.process_memory.allocator_mmap_bytes`
- `diagnostics.process_memory.allocator_tracked_bytes`

## Verification

- A 32 MiB speculative DATA request must become visible as queued or active
  sender ownership, then return every ownership counter to zero.
- An accepted 8 MiB request held inside its handler must appear as one active
  inbound request, then release all receive/reassembly ownership.
- Bidirectional accepted and dialled routes are covered.
- RPC group: 40/40 passed.
- Core suite: 262/262 passed.
- Runtime dependencies: 3/3 passed.
- macOS Release build passed with warnings treated as errors.

## Bounded UAT

After full-source parallel deployment to all four nodes, first record a clean
idle sample. Start the same GBNI-1 transcode used in the previous checkpoint and
sample GBNI-1 once per second through first fragment and for a short stable
playback interval. Stop playback, wait for the event-driven reclaim, and record
the drained values.

The run should answer, without further speculative changes:

1. whether remote object payloads remain owned by transport while RSS is high;
2. whether the live plateau is glibc arena-in-use, arena-free or direct mmap;
3. how much remains after subtracting the already exposed segment-store and
   transport ownership; and
4. whether all transport ownership reaches zero after playback teardown.

Stop if memory grows without a ceiling, the API/control plane becomes slow, or
playback fails. A codec thread cap remains explicitly out of scope until these
measurements establish that it would reduce excess rather than real-time
viewer throughput.

## Four-node UAT result

The first attempt was confounded by a just-restarted GBNI-1 still performing its
storage accounting scan and by an unnecessarily aggressive one-second remote
Status loop. The LAN observation path temporarily failed, but the local API and
Macha process did not crash or restart. That attempt is not used as the clean
performance result.

The repeat used the same GBNI-1 HEVC-to-H.264 transcode after recovery was
complete, with five-second Status samples through the node's localhost API.
The user reported playback as choppy, though less choppy than the first run.

- Session creation took 3,903 ms and first fragment took 3,864 ms.
- The server used approximately 2.0–2.5 cores during sampled intervals.
- The active decoder reported one HEVC decoder thread; libx264 retained its
  automatic encoder thread selection and zero-latency mode.
- HTTP segment deliveries were logged roughly every 2–5 seconds. These are
  consumer-facing request/completion timestamps, not producer-completion
  timings, and their average interval was faster than the four-second media
  duration. They do not explain or establish the reported choppiness.
- RPC queued, active, fragment, reassembly, pending-reply and inbound-request
  ownership were zero at every five-second sample. DATA resource use and viewer
  waits were also zero. Remote-stripe accumulation is therefore not the live
  plateau or the source of this run's choppiness.
- Live RSS rose from about 329.5 MB to 528.8 MB. At the final live sample the
  segment store owned about 78.1 MB resident plus spill, glibc reported about
  304.1 MB arena-in-use and 309.8 MB arena-free, and no direct mmap allocation.
- On stop, sessions/transcodes/segments and all RPC ownership reached zero.
  The event-driven heap reclaim ran once and succeeded. Arena-in-use returned
  to about 147.0 MB, essentially the pre-playback value, proving that playback
  objects were released.
- Drained RSS settled near 350.8 MB. Glibc retained about 450.3 MB of free arena
  address space, up from 272.7 MB before this run. Most is not resident, but the
  roughly 21 MB drained-RSS increase and repeat-session arena expansion require
  a bounded lifecycle/arena policy rather than being dismissed as live object
  ownership.

The memory-attribution conclusion is sound, but this run did not establish the
cause of the playback regression. Playback was not choppy before this
instrumentation checkpoint, and reducing Status frequency coincided with a less
choppy run. Treat both hot-path ownership bookkeeping and Status collection as
suspect until proven non-interfering.

This refines rather than replaces the playback remediation programme:

1. Make transport ownership observation non-blocking. Status must not acquire
   per-connection outbound or pending locks used by viewer traffic. Maintain
   lock-free current counters at the ownership mutation points, and prove that
   frequent observation does not reduce foreground transfer progress.
2. Bound repeat-session allocator growth. The second clean lifecycle returned
   arena-in-use to baseline but added about 178 MB of free arena address space
   and about 21 MB drained RSS. Establish a process-wide glibc arena bound or an
   equivalent reusable ownership policy, with repeated real libav lifecycle
   tests and explicit contention measurements.
3. Add bounded/configurable decoder parallelism. A single HEVC decoder thread
   leaves CPU unused during viewer work, so spending that capacity is still a
   sensible viewer-priority improvement. It is not, however, the assumed cause
   of this regression and must remain within the memory/CPU bounds established
   above.
4. Keep diagnostics observational: producer-publish and request-wait timings
   should use aggregate atomics or existing low-priority sampling, not
   per-fragment logging or synchronous Status work.
5. Repeat matched playback UAT with Status disabled first, then with bounded
   observation. Smoothness and foreground progress must be unchanged; memory
   must plateau across repeated session lifecycles.

## P0 correction after review

The temporary 0.22.5 attribution layer was removed before continuing memory
work. There are no added per-fragment ownership atomics, no Status traversal of
per-connection outbound/pending locks, and no periodic `mallinfo2` sampling in
the next binary. The attribution fields were removed rather than left as
misleading zero values.

Macha 0.22.6 instead applies an explicit process-wide glibc arena maximum before
creating service or codec threads. The default is four arenas and is
configurable as `runtime.glibc_arena_max` in the range 1–64. A live change is
rejected because the policy can only be established safely at process start.
Non-glibc platforms accept the configuration but perform no allocator mutation.

The Linux regression configures two arenas, then performs three waves of 16
simultaneous threads each allocating and touching 4 MiB. `malloc_info()` proves
the process never creates more than two arenas during or after any wave. It
passes on both GBNI Pis. This directly bounds the allocator mechanism that grew
with each transient codec lifecycle; repeated real playback remains the UAT
gate for physical RSS plateau and viewer behaviour.

The final parallel-suite gate also exposed a test isolation defect rather than
a catalogue/GC production regression. The coalesced catalogue burst test had
inherited the harness's aggressive 500 ms peer-death threshold. Under 12-way
host load a descheduling interval could create a false topology edge, which
legitimately caused an additional catalogue refresh and correctly fenced
destructive GC. The test now uses a five-second failure-detector margin because
it tests metadata coalescing and catalogue GC, not failure detection. No test
deadline was extended, no assertion was weakened and the suite remains
parallel. The complete 262-case core suite then passed twice consecutively at
12-way process isolation; the affected case completed in about one second in
both runs.

## 0.22.6 four-node UAT

The complete source tree was synchronized to all three Linux nodes and built in
parallel. The GBNI-1, GBNI-2 and ES-1 executables were byte-identical. All four
nodes, including the Mac, then joined at version 0.22.6 and metadata generation
3019. No ingest/rsync work ran during this test and Macha Status was sampled
only once at each session boundary; live memory samples came from the operating
system.

Three successive forced-transcode sessions were played from GBNI-1. The user
reported smooth playback with no choppiness or stalls in the first two full
runs; the third was the same 90-second confidence run. The pre-playback RSS was
approximately 202 MiB. Live RSS formed bounded plateaus in the approximate
406–436 MiB range, varying with the segment-store footprint rather than growing
without limit.

Every stop reached zero sessions, zero video/audio transcodes and zero segment
store bytes. Heap reclamation recorded three requests, three runs and three
successes. Drained RSS was approximately 223 MiB after run one, 211 MiB after
run two and 212 MiB after run three, remaining byte-for-byte stable during each
post-stop observation window. There is no lifecycle ratchet: the second and
third drained values are lower than the first and within roughly 10 MiB of the
original process baseline.

This passes the allocator-growth P0 and confirms that removing the temporary
instrumentation restored smooth playback. The next playback work may proceed
to bounded/configurable decoder parallelism, retaining the same repeated-
lifecycle memory and smoothness gates.

## 0.23.0 decoder-parallelism implementation checkpoint

The next local phase adds `streaming.video_decoder_threads`, defaulting to two
and validated in the range 1–16. The value is installed on the video decoder
context before `avcodec_open2`; the pipeline rejects any backend result above
the configured limit. Audio decode remains single-threaded. Together with the
existing `max_video_transcodes`, the setting provides both a per-pipeline and a
process-wide requested decoder-thread bound. It is visible as
`video_decoder_threads` in playback Status and requires a process restart to
change, avoiding a live-reload status/configuration mismatch.

Configuration parsing, invalid bounds, defaults, Status exposure and retention
by the real libav backend are covered. Runtime dependencies pass 4/4 and the
complete core suite passes 262/262 at 12-way process isolation. That suite also
exposed and now covers an independent FUSE shutdown race: a waiting writer
must recheck `stopping` after capacity becomes available and before acquiring
byte ownership. Deployment and matched forced-transcode UAT remain the phase
gate.

### 0.23.0 UAT result

The complete source was built concurrently on all three Pi nodes; all three
executables were byte-identical. All four nodes joined healthy at version
0.23.0 and metadata generation 3019. GBNI-1 reported the configured default of
two decoder threads, and the real HEVC codec-open diagnostic confirmed
`decoder_threads=2`.

Two successive forced-transcode lifecycles were smooth with no choppiness or
stalls. In the first, admission completed in 3.58 seconds and the first fragment
in 3.49 seconds, modestly improving the comparable roughly 3.9-second
single-decoder-thread run. Live RSS remained bounded at approximately 389 MiB.
The second run held an exact approximately 402 MiB plateau for 80 seconds; its
larger live value coincided with roughly 63 MiB resident and 75 MiB spilled
segment data.

Both teardowns reached zero sessions, transcodes and segment bytes. Heap reclaim
succeeded 2/2 times. Drained RSS settled at approximately 204 MiB and 206 MiB
and remained byte-for-byte stable during each observation window, from an
approximately 185 MiB startup baseline. There is no repeated-lifecycle ratchet.
The decoder-parallelism phase passes its viewer, CPU-utilisation, memory and
teardown gates.

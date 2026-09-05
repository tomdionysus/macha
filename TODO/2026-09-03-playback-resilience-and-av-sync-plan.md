# Playback resilience and A/V synchronisation plan

Date: 2026-09-03

## Why this is one programme

The current failures interact:

- transcoded audio drifts badly within 30–60 seconds;
- transformed playback from GBNI-2 to node 200 is slow or choppy;
- Status sometimes appears to take several seconds;
- every HTTP response currently closes the connection, magnifying poor Wi-Fi;
- transformed output can exceed source bitrate without a ceiling;
- the producer/network path carries little buffer margin;
- cold planning and distributed stripe reads add avoidable startup cost; and
- retries, seeks and quality changes have historically multiplied physical
  pipelines underneath one logical viewer.

Treating these as unrelated timeout or tuning bugs would hide shared transport,
timeline and lifecycle problems.

## Live baseline

The 2026-09-03 observation of playback hosted by 10.44.1.51 found:

- immutable profile lookup was immediate;
- the Matroska/HEVC Main 10/EAC3 source was about 4.97 Mbps;
- auto planning rejected the materialised seek index and selected H.264/AAC;
- admission took about 10.7 seconds and the first fragment about 10.1 seconds;
- four-second early fragments were roughly 3.5–6.1 MB and arrived every 3–5
  seconds, leaving approximately one fragment of margin;
- no maximum bitrate was requested, so output burst around 7–12 Mbps;
- Wi-Fi delivery fluctuated around 5–10 Mbps with retransmissions and a
  0.5–1.1 MB socket send queue;
- remote 4 MiB source stripes took roughly 0.8–1.1 seconds;
- node 51 used about 2.7 CPU cores, close to real-time production; and
- RSS/segment ownership was bounded and was not the immediate slow-stream cause.

Local Status generation remained sub-millisecond and ordinary Wi-Fi samples
were mostly tens of milliseconds, with an occasional connection-setup outlier.
Peer RPCs to/from node 200 separately stalled for 5–10 seconds. This indicates
poor transport plus needless connection churn, not permission for synchronous
UI waits.

## Phase 0 — Proof without observer distortion

- Add an opt-in aggregate playback snapshot: first/last input and output A/V
  PTS, expected media time, maximum drift, discontinuity and repair counts, and
  fragment media/wall durations.
- Never log per packet, frame or fragment on the critical path. Publish counters
  from already-owned state after bounded aggregation.
- Build a deterministic case longer than 90 seconds with non-zero starts, audio
  priming and a seek/discontinuity. It must reproduce present drift.

Exit: the failure is reproducible and measuring it does not alter playback.

## Phase 1 — One presentation timeline

2026-09-05 partial progress, corrected after a live regression: shipped
bounded drift compensation for transcoded audio after live-reproducing a real
desync report (Apollo 13, EAC3 5.1 source) and measuring the free-running
audio clock's actual drift rate against a re-anchored-every-frame video
clock. The first attempt (0.23.8, `swr_set_compensation()`-based resample-
ratio nudging) measured well (offset bounded to ~±15ms over 8 minutes) but
was wrong: any resample-ratio change shifts pitch, and it was reported live
as an unacceptable, clearly audible artifact. Replaced (0.23.9) with
libswresample's own built-in fill/trim correction (`async=1` +
`swr_next_pts()`), which corrects by injecting silence or dropping samples
and never touches the resample ratio (`max_soft_comp` stays at its disabled
default) -- pitch shift is structurally not possible through this path. That
replacement itself briefly regressed to near-total audio loss during
development (a unit-fraction/GCD math error) before being caught in local
verification, ahead of reaching either node real viewers use. Both incidents
were only caught by live measurement/live listening, not CI, because no
harness exists yet for the real (non-stub) transcode path -- that harness is
still this phase's stated exit-criterion prerequisite and remains
outstanding; building it earlier would very likely have caught both. Also
live-diagnosed a separate, likely larger contributor to the same user
reports: a client-side race (`PlaybackCoordinator.degrade()` not checking for
an in-flight seek mutation) that creates two independent transcode pipelines
on restart/seek, each restarting its own audio+video PTS from zero -- fixed
client-side, tracked in
`TODO/2026-08-31-cluster-any-node-playback-failover.md`. The remaining items
below (session-relative origin, codec delay/priming, generation-replacement
monotonicity, and the Direct/Remux/Transcode regression harness itself) are
still open.

- Define one session-relative presentation origin and explicitly map every
  source stream onto it.
- Apply seek offsets, input start time, codec delay, AAC priming/drain and
  timestamp repair exactly once, in a documented owner.
- Preserve monotonic DTS/PTS across encoder flush, fragment rollover and seek
  generation changes; never derive audio and video wall clocks independently.
- Bound correction of malformed/discontinuous inputs rather than masking drift
  by repeatedly dropping or duplicating arbitrary samples.
- Add Direct, Remux and Transcode regressions for start, long play, seek,
  generation replacement, unequal time bases and missing timestamps.

Exit: accumulated A/V skew stays within a defined small bound, including after
seek, without changing response or planner semantics.

## Phase 2 — Persistent, bounded HTTP transport

- Implement HTTP/1.1 keep-alive for Status, manifests and fragments; retain
  explicit close where requested or required.
- Bound requests per connection, idle lifetime, unread body, queued output and
  concurrent connections per peer/class.
- Ensure cancellation and slow-client backpressure release all ownership. A
  slow Status consumer must not occupy a viewer or control worker.
- Test reconnect, loss, half-close, disappearance and shutdown. Reuse must not
  weaken authentication or idempotency.
- Make ordinary Status a coherent recent snapshot; keep expensive diagnostics
  opt-in and unavailable/stale fields explicit.

Exit: sequential Status and fragment requests reuse TCP; dropped connections
recover without duplicate work; control latency remains bounded.

## Phase 3 — Bandwidth and buffer control

- Require or infer a delivery budget. Without one, use a conservative,
  configurable poor-network default rather than unconstrained CRF.
- Account for video, audio and container overhead. A compatibility transcode
  must not unexpectedly demand materially more bandwidth than its source or
  known path capacity.
- Maintain a bounded target amount of playable media ahead. Viewer work keeps
  its dominant share while loader/background work receives its configured share.
- Adapt only from robust signals and with hysteresis; do not oscillate quality
  or create new sessions for routine short stalls.
- Expose buffer/readiness and effective bitrate decisions without making
  admission wait for speculative measurement.

Exit: impaired-network playback is continuous after bounded startup, output
stays within budget, and throughput changes do not multiply sessions.

## Phase 4 — Cold planning and source delivery

- Persist profile data and Direct/Remux evidence, including seek/index
  suitability, against immutable file identity.
- A hit performs no probe reads. A miss queues globally deduplicated background
  work below loader priority while normal media-engine negotiation continues.
- Coalesce/prefetch sequential remote stripes, remove redundant full-stripe
  copies, and prefer a cheaper local valid representation where appropriate.
- Keep work cancellation/deadline bounded and make corrupt/incomplete metadata
  fall back safely.

Exit: warm admission performs no probing reads; unavoidable cold cost is
bounded; sequential delivery has lookahead without RSS or loader starvation.

## Phase 5 — Codec cost, last

- Re-measure production speed and CPU after preceding corrections.
- Retain proven bounded decoder parallelism and the memory ledger.
- Only then evaluate more software parallelism or hardware acceleration,
  including exact format, quality and fallback behaviour.

Exit: codec work materially increases headroom and passes the same timeline,
memory, teardown and poor-network matrix.

## Final UAT matrix

Run one variable at a time before combined load:

1. Status idle and over an impaired link.
2. Direct Play: start, seek and five-minute continuity.
3. Remux: start, seek and five-minute continuity.
4. Transcode: start, seek, quality change and at least ten minutes for drift.
5. One persistent viewer across retries and generation changes.
6. Two legitimate viewers with exact admission/reclamation.
7. Playback plus rsync/loader work under the configured viewer share.
8. Endpoint loss/failover during admission, manifest and fragment delivery.

Record admission/first-fragment time, A/V skew over time, production/delivery
margin, effective bitrate, reconnects, CPU, RSS/owned bytes, physical pipeline
and logical session counts, and teardown latency. Stop on growing drift,
ambiguous 404, leaked encoder, unbounded memory, lost control service or
starvation of the configured non-viewer share.

# Node telemetry visibility in Status

Date: 2026-09-09

Raised by the operator: "the UI does not always give node telemetry for
nodes". It does not. The API withholds it, and the transport that would keep
it fresh is best-effort in a way that makes withholding the normal case for
any node across the WAN.

## Measured, on the live cluster

All three nodes on 0.36.6, all `healthy`, metadata writable, generations
converged (25722/25723). Each node's own `/api/v1/status`:

| observer | es-1 | gbni-1 | gbni-2 |
|---|---|---|---|
| gbni-1 | stale, `live_age_ms` 291,627, `runtime {}` | self, live | live, full runtime |
| gbni-2 | stale, `live_age_ms` 291,832, `runtime {}` | live, full runtime | self, live |
| es-1 | self, live | stale, `live_age_ms` 293,608, `runtime {}` | stale, `live_age_ms` 292,840, `runtime {}` |

Every WAN pair is blank; only the two LAN nodes see each other. The samples
are ~4.9 minutes old against a 15 s freshness window, so this is not "a bit
late" — telemetry stopped arriving and nothing retried.

## Three separate causes

**1. A stale sample is blanked rather than labelled.**
`node_json()` populates `runtime` only when `effective.authoritative &&
online`, and `effective_telemetry()` requires `live && !stale && phase ==
ready`. One sample crossing the freshness line therefore removes
`uptime_ms`, `rss_bytes`, `process_cpu_percent`, `load1`, `cpu_cores`,
`memory_total_bytes`, `peers_*` and `rpc_connections_*` in a single step,
leaving `"runtime": {}`. `phase` becomes `"unknown"` and
`storage_backends_online` becomes null.

The reasoning in the source (status_api.cpp:155) is sound for capacity and
usage: a stale storage figure presented as current is a fabricated number.
It does not hold for the runtime figures. Those are honest measurements of
the sending process at a stated instant, and the entry already carries
`telemetry_freshness` and `live_age_ms` to say how old they are. The
freshness window is 15 s here (`max(heartbeat * 3, 5s)`, heartbeat_ms 5000),
which is far too short to be the boundary at which an operator stops being
allowed to see a node's load average.

**2. `metadata_generation` prefers membership over the live sample.**

```cpp
node["metadata_generation"] = member ? member->metadata_generation
                                     : (live ? live->metadata_generation : durable.metadata_generation);
```

Membership wins even when it carries 0 and the fresh telemetry in the same
response carries the real generation. Live right now: gbni-1 reports
gbni-2's generation as **0** in an entry whose `telemetry_freshness` is
`live` with a full runtime block, while gbni-2 and es-1 both report 25723
for it. A healthy node reads as generation 0 in the UI.

**3. Telemetry gossip is idle-gated, best-effort, and never retries.**
`NodeRuntime::telemetry_loop` (cluster.cpp:1279) broadcasts only when the
node has been free of foreground *and* read-ahead work for 2 s, and then
only via `broadcast_best_effort(..., FrameType::speculative)` — "no-dial,
no-wait", admitted only if routing and the per-peer outbound lock are
immediately free. A tick that cannot be admitted is dropped, not queued.

So the busier a node is, the less visible it becomes, which inverts what an
operator needs. And a peer in `RetryPolicy` backoff receives nothing at all:
es-1 is currently logging a continuous stream of `peer in retry backoff` for
bootstrap, session sync, metadata head survey and liveness against both home
nodes, while TCP to :7437 connects in both directions and ping is 69–72 ms.
The peers are reachable; the backoff state alone is starving telemetry.

This is the aggregation half of the item already standing in ACTIVE.md; the
freshness half shipped in 0.23.3.

## Plan

**1. Keep the runtime figures when the sample is stale.** Gate `runtime` on
`live && online` rather than on `effective.authoritative && online`. Leave
storage/cache/`storage_backends_online` exactly as they are — those stay
unavailable when stale, because they are the ones a consumer would sum. The
existing regression
`test_status_marks_stale_peer_telemetry_as_unavailable_not_live` asserts the
current blanking and must be updated: it keeps its storage assertions and
gains the inverse runtime assertion, so the split between "old measurement,
labelled" and "would be a fabricated number" is what the test pins.

**2. Take the higher of membership and telemetry for `metadata_generation`.**
Generations advance monotonically per node, so the fresher source is simply
the larger one; fall back to the durable last-known value only when neither
membership nor telemetry carries anything.

**3. Make peer visibility survive a busy node.** Operator decision,
2026-09-09: "telemetry data is important" — remove or loosen the gates that
suppress it. A CONTROL-class variant was written first and then reverted on
the operator's instruction: the gates, not the class, are what made
telemetry late, and SPECULATIVE keeps gossip out of the 64 MiB control
memory reserve and off the two control workers, where it would have sat at
priority 1 ahead of foreground reads. Implemented as:

- `NodeRuntime::telemetry_loop` gossips on every tick at
  `FrameType::speculative`, with the `operationally_idle` gate (2 s free of
  foreground *and* read-ahead work) removed outright. A telemetry set is
  ~200 bytes per entry and capped at 64 entries, so sending it every tick is
  cheap: ~600 bytes on this three-node cluster, ~13 KB worst case.
- The cadence is now `network.telemetry_interval_ms`, **default 10 s**
  (previously a hardcoded 5 s), documented in `docs/configuration.md` as the
  knob that decides how stale a peer's figures can be in another node's
  Status.
- A demand-driven wake — a readiness transition or any peer observation —
  still publishes sooner than that cadence, but **no more than once per
  second**. Without that floor the loop had no minimum spacing at all: its
  wait returns immediately whenever the demand counter moves, and demand is
  bumped on every `members_.observe()`, so a reconnecting peer could have
  turned this into a send loop. Local sampling still runs on every wake, so a
  phase change is published promptly.
- `try_notify()` — both the outbound `PeerConnection` and the inbound
  `RpcServer` session copy — no longer requires an entirely idle writer for a
  *small* notification. A payload of at most `max_notify_payload_bytes`
  (64 KiB) may queue while the writer's pending payload is under
  `max_notify_backlog_bytes` (1 MiB); anything larger still waits for an idle
  writer. This is keyed on size rather than frame class precisely because
  telemetry rides SPECULATIVE: `best_outbound_locked()` still sends every
  more urgent frame first, so a queued notification cannot delay operational
  RPC, and the bound stops a peer that has stopped draining from
  accumulating notifications without limit.

Cost measured against the live cluster before the change: control handlers
were at `handler_us_max` 85 µs over 496 requests, and four full-suite runs
with the change (wall 33.2–35.6 s) were indistinguishable from four without
it (32.4–34.3 s).

**Retry backoff was investigated and is not the gate.** `observe_result()`
caps it at `250ms * 2^min(failures-1, 4)` = **4 s**, and
`RpcClient::connection()` already exempts an established route: backoff
applies only to creating a new TCP connection. So es-1's continuous
`peer in retry backoff` stream is not a long lockout being served — it is a
dial that fails, backs off 4 s, and fails again, every few seconds, while
raw TCP to :7437 connects in both directions and ping is 69–72 ms. Exempting
telemetry from a 4 s window would have bought nothing. **The open question
is why the dial itself keeps failing**, and it is a bigger fish than
telemetry: bootstrap, session sync, metadata head survey and liveness are
all failing on the same route. Note that gbni-1 sees es-1's inbound
connections arriving from `10.255.34.1` while es-1 advertises
`10.34.1.50:7437`, which is the first thing to look at.

## Acceptance

- A node whose sample is minutes old still reports `uptime_ms`, `rss_bytes`,
  `load1`, `process_cpu_percent`, `cpu_cores`, `memory_total_bytes`,
  `peers_*` and `rpc_connections_*`, alongside `telemetry_freshness:
  "stale"` and a truthful `live_age_ms`.
- The same node still reports `storage.available: false` and a null
  `storage.used_bytes`, and the cluster aggregate still refuses to treat it
  as authoritative.
- No node reports another's `metadata_generation` as 0 while holding a
  sample that says otherwise.
- The UI labels the age: yellow past 1 minute, red past 5 minutes (asked of
  the UI session on 2026-09-09).
- A node under load still gossips: `live_age_ms` for a busy peer stays within
  a few heartbeats rather than growing to minutes. Verified on the cluster
  rather than in a unit test — forcing a genuinely busy writer on loopback is
  not deterministic enough to assert on, so the tests cover the class being
  legal and delivered in both route directions, and the bounded-backlog rule
  is proven by the live measurement.

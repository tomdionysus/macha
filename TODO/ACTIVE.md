# Active tasks and concepts to explore

Last updated: 2026-09-03

This is the authoritative, ordered backlog. Detailed plans and UAT records in
this directory remain evidence; completed work belongs in `COMPLETED.md` and is
not repeated here. Work top-to-bottom unless new evidence changes the order.

The governing laws are:

1. Thou Shalt Not Make The Viewer Wait.
2. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.
3. Control traffic must remain promptly serviceable. Viewer priority is a large
   configurable share (95:5 by default), not indefinite starvation of all other work.

## P0 — Playback correctness and poor-network resilience

Execute the phased
[playback resilience and A/V sync plan](2026-09-03-playback-resilience-and-av-sync-plan.md).
This coalesces the newly observed audio drift, slow streaming, intermittent
Status latency, choppy playback, cold admission, and poor-Wi-Fi behaviour into
one causal programme rather than treating each symptom separately.

- [ ] **1. Correct A/V desynchronisation.** Transcoded playback drifts badly
  within 30–60 seconds. Establish a single presentation timeline across seek
  offsets, input discontinuities, codec delay, audio priming/drain, fragment
  boundaries and timestamp repair. Add deterministic long-enough regressions
  for monotonic timestamps and bounded accumulated A/V drift. Diagnostics must
  be aggregate and low-overhead.
- [ ] **2. Remove per-request connection setup from Status and streaming.** The
  server currently emits `Connection: close`; on poor Wi-Fi this makes every
  Status call and HLS fragment pay connection setup and magnifies transient
  packet loss. Implement bounded HTTP keep-alive/reuse, cancellation, idle
  expiry and slow-client isolation.
- [ ] **3. Split lightweight status from expensive diagnostics.** Ordinary
  Status must use a recent coherent snapshot and never synchronously collect
  expensive subsystem state. Report server generation time so clients can
  distinguish server delay from network delay; represent unavailable peer or
  storage samples explicitly, never as zero.
- [ ] **4. Make transformed output bandwidth-aware.** Auto negotiation selected
  H.264/AAC but, without a client maximum bitrate, CRF output expanded a roughly
  5 Mbps source to bursts around 7–12 Mbps. Define a conservative poor-network
  default, honour client limits, and prevent a compatibility transcode from
  silently increasing delivery demand.
- [ ] **5. Build useful buffer margin.** Do not run a four-second-fragment
  producer and network delivery only one fragment ahead. Bound startup work,
  produce ahead under viewer priority, and recover from transient stalls
  without restarting or multiplying sessions.
- [ ] **6. Remove avoidable cold transformed-planning work.** Persist complete
  immutable profiles and remux/seek-index suitability. Admission must use
  stored information, never wait on speculative profiling, coalesce background
  scans, and retain normal media-engine fallback.
- [ ] **7. Improve viewer source delivery.** Coalesce/prefetch sequential remote
  stripes, prefer locality intelligently, and bound reassembly/copies without
  weakening integrity or viewer priority.
- [ ] **8. Optimise codec throughput only after phases 1–7 are measured.** Keep
  proven bounded decoder parallelism and memory lifecycle. Evaluate hardware
  acceleration or more parallelism only against corrected transport/timelines.
- [ ] **9. Run the ordered UAT matrix.** Test Direct, Remux and Transcode; start,
  seek and quality changes; LAN and impaired Wi-Fi; cold/warm profiles; one/two
  logical viewers; and concurrent loader work. Gate on bounded A/V drift,
  prompt Status, stable playback, no ambiguous 404, exact session admission,
  bounded RSS/CPU and prompt teardown.

The completed logical-viewer entitlement work is the foundation: capacity is
tied to one persistent logical viewer/UI session, not each stream generation.
Keep teardown active until UAT proves that seek, quality changes, disconnects,
supersession, failure and failover cannot leak physical encoders or produce
`transcode limit reached` for one viewer.

## P0 — Structural ingest, metadata and retained-memory safety

Resume the
[structural ingest/runtime remediation](2026-09-02-structural-ingest-runtime-remediation.md)
after the immediate playback correctness blocker. Existing checkpoints remain
valid evidence, but do not prove the end-to-end invariants.

- [ ] Finish process-wide retained-memory ownership bounds for decoded metadata,
  catalogue/profile state, reconciliation retries, RPC/reassembly, object
  payloads and playback. Retest unexplained idle RSS growth and the fixed
  `hydration executor is stopping` shutdown race.
- [ ] Replace unbounded metadata-history retention and the disabled compactor
  with an exact-head, cluster-acknowledged checkpoint/ancestry-floor protocol.
  Bound decrypted history/materialisation caches by bytes; expose current RSS,
  swap, peak and cache bytes; shed caches under pressure.
- [ ] Remove serial remote `has_on` checks from metadata mutation critical
  sections. Preserve retention-before-acceptance with batched, bounded checks
  away from control and viewer work.
- [ ] Make catalogue metadata clearing asynchronous and bounded. The observed
  `DELETE /api/v1/catalogue/items/{id}/metadata` hangs because its HTTP handler
  currently performs `repair_once()`, materialises and copies the complete
  catalogue, repeatedly scans all items to discover descendants, walks all
  artwork, and waits for the distributed metadata commit before replying. The
  endpoint must promptly validate the item/revision, durably enqueue one
  idempotent clear job, and return `202 Accepted` with a job/status `Location`;
  retries must recover the same operation. Background execution must use an
  indexed descendant closure and delta-sized mutation, remain below control and
  viewer priority, publish completion/failure explicitly, and preserve atomic
  hierarchy removal plus targeted media rematching. Add large-hierarchy,
  concurrent retry, restart/recovery, conflict, failure and latency regressions;
  neither request handling nor status polling may perform repair, full-snapshot
  copying, remote durability waits or artwork-wide scans.
- [ ] Complete durable namespace batching for mixed create/rename/unlink chains.
  Keep rename a safe singleton until crash/restart proof exists.
- [ ] Make spool backpressure smooth and visibly progressive near its configured
  limit, pacing toward measured publication/drain rate instead of alternating
  full-speed bursts and apparent freezes.
- [ ] Prove large-history, partition/sibling-head, cache-pressure,
  unclean-restart and stale-FUSE recovery, then run a guarded overnight
  four-node rsync UAT. Require bounded RSS/swap/history, automatic rejoin,
  writable metadata, no viewer/control regression and no manual mount cleanup.

Do not tune aggregate ingest throughput around known amplification. Older FUSE
throughput, heap-audit and ownership documents remain detailed evidence but are
absorbed here rather than separate active programmes.

## P1 — Cluster connectivity, status and operations

- [ ] Support multiple advertised endpoints per durable node (LAN/WAN,
  IPv4/IPv6 and configured/discovered), multiple bootstrap candidates,
  reachability-aware racing/fallback, expiry and deduplication by node identity.
  Integrate UPnP/external-IP discovery. Test poor Wi-Fi, partitions, NAT without
  hairpin, endpoint changes, simultaneous dial and commit interruption.
- [ ] Make client/API failover preserve one logical operation: reuse session
  idempotency keys, reconcile ambiguous POST results, fail over endpoints, and
  never turn transient transport loss into a misleading 404 or duplicate lease.
- [ ] Correct aggregated Status truthfulness and freshness. During the 0.23.1
  rolling deployment the endpoint reported all four nodes green, complete with
  plausible load figures, while nodes were stopped or spending 80–100 seconds
  in recovery. Audit whether the server or frontend promotes cached/last-known
  telemetry to live state. Every node and measurement must carry observation
  time, source and freshness; distinguish `live`, `stale`, `recovering`,
  `unreachable`, `offline` and `unknown`, and never colour a node healthy or
  present cached load as current merely because an old sample exists. Peers
  must also not alternate valid generation/disk use with zero; during storage
  recovery report unavailable/stale capacity plus reconciliation state, not
  authoritative zero. Add stop/restart, partition, missed-heartbeat,
  stale-telemetry and client-rendering contract tests with bounded detection
  deadlines.
- [ ] Add optional display-only `node_name` at `.nodes[].node_name`; configure
  `Corvus GBNI-1`, `Corvus GBNI-2`, `Corvus ES-1`, and `Corvus MacBook Pro`.
- [ ] Complete hard-kill stale-FUSE recovery proof and automatic clean rejoin.
- [ ] Diagnose node 50 losing SSH responsiveness with Macha active during a
  build; correlate CPU, runnable tasks, RSS/swap, I/O wait and queues.
- [ ] Diagnose faulty torrent/ingest independently so it does not obscure
  convergence and runtime measurements.
- [ ] Make ingest/torrent job visibility and control cluster-wide.
  `/api/v1/ingest/jobs` and `/api/v1/torrents/jobs` were node-local: which
  imports/downloads a client could see, and which it could pause/resume/
  cancel, depended entirely on which node's API answered. Fan `GET .../jobs`
  (list and single-job) out across the cluster via RPC survey, tag each job
  with its owning `node_id`, and forward pause/resume/retry/cancel/clear to
  the owning node when the receiving node doesn't have the job locally.
  (In progress this session.)
- [ ] Diagnose `ingest failed: metadata acceptance certificate durability
  floor unavailable` failures on torrent ingest once the torrent has
  downloaded, which are also unaccountably slow.

## P2 — Catalogue and media model

- [ ] Make negotiation representation-aware. One Macha work identity may
  reference several immutable files/assets, each with a persisted profile.
  Select the best Direct representation, then cheapest correct Remux/asset
  combination, then cheapest suitable Transcode while respecting quality,
  stream flags, availability, locality, seek cost and resource limits.
- [ ] Replace provider-shaped public IDs (`tmdb:*`) with opaque Macha work IDs.
  Keep provider IDs internal, preserve aliases during migration, and keep work
  identity separate from immutable content hashes.
- [ ] Update clients to consume immutable profiles and send a useful bandwidth
  ceiling plus a persistent logical-viewer/session identity.

## P2 — Diagnostics and repeatable proof

- [ ] Record reproducible local and four-node benchmark recipes without brittle
  default-suite wall-clock thresholds.
- [ ] Keep diagnostics bounded, snapshot-based and disabled by default when
  they perturb viewer behaviour; never instrument per packet or fragment on a
  critical thread merely to diagnose a P0.

## Deployment rule

- [ ] For every deployment, synchronise the complete source tree and all CMake
  inputs, configure after synchronisation, build nodes in parallel, and verify
  installed versions and byte-identical hashes on identical RPi hardware.

# P0 metadata-history memory remediation

Date: 2026-09-01

Status: active release/stability gate. Phases 1 and 2 have reached their first
deployable UAT checkpoint; distributed checkpoint/pruning remains deliberately
disabled.

## Incident and evidence

Node 50 was left processing an overnight rsync. At 08:39 the Linux OOM killer
terminated Macha with approximately 2.4 GB anonymous RSS and about 500 MB
swapped. The hard death left `/mnt/machamedia` as a disconnected FUSE mount;
startup then failed before its configured stale-mount recovery and entered a
three-second systemd restart loop.

The primary memory failure is replicated metadata retention, not an rsync
buffer or the recovered FUSE journal itself:

- node 50 `history.log` grew from 64,265,459 bytes at 00:47 to
  2,134,508,604 bytes at 08:39;
- the four live replicas held approximately 2.15–2.18 GB each;
- the Mac copy contained 998 history frames: 389 full snapshot-sized frames
  above 5 MB and 608 small delta-sized frames;
- every history payload is decrypted and retained in `MetadataReplica::history_`;
- the 64-entry materialisation cache retains at least another 363 MB of encoded
  5.67 MB snapshots before decoded trees and allocator overhead;
- current surviving-process footprints were approximately 4.17 GB RSS on node
  51, 4.95 GB RSS plus 2.0 GB swap on ES-1, and 2.65 GB on the Mac.

Sustained namespace mutations exposed two amplification decisions. Concurrent
writes create sibling accepted heads, and deterministic reconciliation always
publishes a full snapshot even when its change from the primary parent is tiny.
The earlier ancestry incident also required disabling unsafe local history
compaction, leaving no durable retention bound. This combination made ordinary
sustained ingest an unbounded-memory workload.

The FUSE recovery log contained 63,806 durable operation records over 20
inodes, but recovery compacted the operation journal to 4 KB. It increased the
mutation workload; it does not explain the multi-gigabyte retained heap.

## Invariants

1. **Thou Shalt Not Make The Viewer Wait.** Paging or compacting history must
   never put storage, decoding, hashing or checkpoint coordination on viewer or
   critical communications threads.
2. **Control must remain non-bypassable.** Memory pressure, cache misses and
   history maintenance cannot consume reserved control execution/storage
   capacity.
3. Loader work remains useful foreground work, but it must operate within hard
   memory bounds and yield resources to viewer/control work.
4. No pruning is permitted from generation equality, elapsed stability, or one
   node's local view. Exact accepted-head ancestry and durable participant proof
   are required.
5. A crash or reboot must automatically recover the Macha FUSE mount and rejoin
   the existing generation without genesis or manual cleanup.

## Phase 0 — measurement and deterministic reproduction

- [x] Correct `runtime.rss_bytes`, which previously reported lifetime peak
  `ru_maxrss`; it now reports current resident memory on Linux and macOS.
- [ ] Expose separately named current swap and peak RSS on Linux/macOS where
  available.
- [x] Expose history file bytes, indexed records, resident payload bytes, and
  materialisation entries/bytes/limit/evictions.
- [ ] Expose pinned bytes and pressure shedding.
  Measurements are event-driven/read-on-request; do not add a polling loop.
- [x] Add a synthetic history fixture with many large full entries and small
  deltas. Prove current startup memory grows with total history before changing
  mechanics, without requiring a multi-gigabyte CI artifact.
- [x] Add a deterministic four-node sibling/merge workload proving the merged
  history body is smaller than the full immutable record on both replicas.
- [ ] Record the incident baseline above and a reproducible bounded local soak
  recipe outside default timing-sensitive CI.

Checkpoint: diagnostics reproduce both retained-history and merge-frame
amplification, and tests fail for the intended bounds.

## Phase 1 — disk-backed, byte-bounded history

- [x] Replace `std::map<Hash256, MetadataHistoryEntry>` payload retention with a
  compact authenticated index containing only generation, hash, predecessor,
  merge parents, body kind and durable frame offset/length.
- [x] Stream/decrypt/decode a payload on demand from its immutable frame. Cache
  validated payload/materialisation data under one explicit byte budget rather
  than an entry count.
- [x] Coalesce concurrent loads/materialisations for the same hash. Perform file
  I/O and reconstruction outside the replica mutex, then revalidate immutable
  index identity before cache installation.
- [ ] Preserve current/committed/accepted heads, but account pinned bytes. If
  required pins exceed the configured budget, report pressure explicitly and
  avoid admitting optional history work; never silently discard authority.
  Decoded accepted-head trees are no longer pins: acceptance certificates and
  indexed durable history preserve their authority and reconstructibility.
  Current/committed decoded views remain pinned and accurately deep-accounted;
  explicit pinned-byte/pressure Status diagnostics remain to be added.
- [x] Avoid duplicate ownership of the same encoded snapshot between history,
  `MetadataRecord`, materialisation and decoded-view caches where immutable
  sharing is possible.
- [x] Ensure restart scans are O(history bytes) in I/O but O(index bytes plus
  configured cache budget) in resident memory.

Tests:

- [x] Startup over a large synthetic history retains zero history payload bytes
  and remains beneath the configured materialisation envelope.
- [x] Random historical reconstruction, branch ancestry, corruption detection
  and restart return byte-identical records with on-demand paging.
- [x] Concurrent identical misses perform one reconstruction and share the
  immutable result.
- [x] A 200-delta cold reconstruction retains and caches only its requested
  final snapshot, rather than every complete intermediate namespace.
- [ ] Eviction and pressure shedding preserve pinned authority and do no work on
  viewer/control threads.

Checkpoint: deploy only after the complete metadata, RPC, restart and corruption
matrix passes. Measure cold-start and steady RSS on one non-critical node.

## Phase 2 — delta-encode reconciliation history

- [x] Compute the exact delta from the deterministic primary parent to the
  merged snapshot and pass it to history publication whenever it is smaller
  than the full payload.
- [x] Keep the `MetadataRecord` payload and hash exactly unchanged. Only the
  reconstructible history-body representation changes; merge-parent topology,
  acceptance certificates and conflict semantics remain identical.
- [x] Fall back safely to a full entry for non-representable/legacy cases or
  when the delta is not smaller.
- [x] Ensure simultaneous reconcilers still derive one identical merge commit
  and cannot manufacture sibling merges through local mutation sequences.

Tests:

- [x] A no-conflict sibling merge stores a small delta and reconstructs the
  byte-identical full record.
- [x] Conflict creation/resolution, policy transitions, same-generation siblings
  and multi-head folds retain existing semantics.
- [ ] A long concurrent-mutation model has bounded history-byte amplification
  relative to authored deltas rather than namespace snapshot size.

Checkpoint: repeat a loaded namespace/publication burst and verify history
growth tracks useful changes rather than full snapshot size.

## Phase 3 — safe distributed checkpoint and ancestry floor

- [ ] Design an authenticated checkpoint proposal naming the exact accepted
  head set, checkpoint record, ancestry floor and participant/policy epoch.
- [ ] Require durable acknowledgements from every known metadata participant
  whose returning state must remain mergeable. Generation equality alone is
  never sufficient.
- [ ] Persist proof before pruning. A restart must validate the proof before
  using the advanced floor, and a returning node below the floor must adopt the
  checkpoint through the normal join protocol rather than resurrecting an old
  branch.
- [ ] Retain all unresolved heads, conflict roots and required bridge ancestry
  until the exact proof covers them.
- [ ] Define behaviour for offline participants, retired node identities,
  membership changes and rolling-version incompatibility. When proof is
  incomplete, remain safe and unpruned while Phase 1 still keeps RAM bounded.

Tests:

- [ ] Partitioned/returning node, same-generation sibling, concurrent acceptance
  notice, crash between proposal/ack/prune, corrupt proof, participant change,
  and rolling-upgrade matrices.
- [ ] A pruned node and a stale returning node converge automatically without
  genesis, metadata loss or manual repair.

Checkpoint: only then re-enable automatic history compaction.

## Phase 4 — crash recovery and operational guardrails

- [x] Move stale Macha FUSE detection/recovery ahead of any operation which
  dereferences or creates the configured mount path. Retain fail-closed refusal
  of unrelated filesystems.
- [ ] Add an unclean SIGKILL/reboot regression leaving a disconnected mount and
  prove automatic unmount, service start, identity retention and cluster rejoin.
- [ ] Add event-driven memory-pressure cache shedding with counters and explicit
  degraded diagnostics. It may drop reconstructible cache only, never authority
  or admitted durability work.
- [ ] Consider conservative service-manager `MemoryHigh`/`MemoryMax` defaults
  only as a last-resort host guard after Macha owns its bounded-memory policy.
  Limits must not substitute an OOM kill inside a smaller cgroup for a fix.
- [ ] Rate-limit restart policy sufficiently to prevent thousands of failed
  starts while preserving prompt recovery after transient conditions.

## Final UAT gate

Run four nodes with rsync/import long enough to exceed the incident's mutation
and history-frame counts, plus concurrent playback and seek checks.

Pass only if:

- current RSS and swap reach a documented stable ceiling on every node,
  including node 50 and ES-1;
- history growth is proportional to useful deltas and periodically checkpointed
  only with valid distributed proof;
- viewer and control wait/timeout counters do not regress;
- loader throughput remains useful and non-starved;
- a hard-killed node automatically clears only its stale Macha mount, restarts,
  retains identity and joins the current writable generation;
- no manual unmount, metadata repair, genesis, timeout increase or operator
  intervention is required.

After this gate, move the completed phases to `COMPLETED.md` and resume the
remaining Phase 1D/throughput work.

## 2026-09-01 implementation checkpoint

The first deployable cut is complete:

- `history_` is now a compact ancestry/frame index; decrypted payloads are not
  retained there (`history_resident_payload_bytes` is zero by construction).
- Materialisations use `dht.metadata_materialization_cache_bytes`, default
  `128M`, as a conservative logical byte budget while retaining the old
  64-entry ceiling as a secondary guard. Oversized optional historical results
  are returned but not cached.
- Reconciliation gained DLT6 only when merge-parent/conflict replacement is
  needed. Ordinary deltas remain DLT5. A lagging target receives the missing
  parent and retries the delta before the safe full fallback.
- Status exposes history file/record and cache byte counters; `runtime.rss_bytes`
  now means current RSS rather than lifetime peak.
- Startup stale-mount recovery now runs before `create_directories()` can touch
  a disconnected mount.

Verification:

- clean build completed;
- focused disk-backed-history, delta/model, configuration, corruption and
  four-node branch reconciliation tests passed;
- parallel complete runs exposed unrelated timing-sensitive FUSE tests, each
  of which passed alone; the authoritative serial core run passed **246/246**;
- the runtime/libav run passed **3/3**.

UAT is useful now. Deploy all nodes together because reconciliation DLT6 is a
new history-body codec. On node 50, first prove the existing ~2.1 GB history can
be scanned without OOM, the stale Macha mount is detached automatically, and
the same identity rejoins. Then observe all four nodes through a namespace
burst and compare `diagnostics.metadata.history_file_bytes`,
`history_resident_payload_bytes`, `materialization_cache_bytes`, current
`runtime.rss_bytes`, generation convergence and viewer/control wait counters.
Do not resume overnight ingest until this checkpoint passes.

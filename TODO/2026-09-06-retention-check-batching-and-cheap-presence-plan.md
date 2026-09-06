# Retention-check batching and a cheap local presence check

Status: primary active programme — promoted to P0 by a live incident

Date established: 2026-09-06

This unifies two previously-separate backlog entries that turned out to be
one live incident's two halves:

- The already-P0 item "Remove serial remote `has_on` checks from metadata
  mutation critical sections" (`ACTIVE.md`, P0 — Structural ingest section).
- The P1 "Scaling cliffs" item "`LocalStore::valid()` is used as a cheap
  presence check but does a full read + AES-GCM decrypt + SHA-256" — filed
  2026-09-05 as **"not yet urgent at current 3-node/home scale."** It is
  urgent now; this incident is the proof.

## The live incident that forced this (2026-09-06)

The user rsync'd a large batch of movies into the namespace on `corvus-es-1`
(`10.34.1.50`) and, concurrently, ran `rm -rf /mnt/machamedia` on the same
node (itself hitting the still-open, separate `rm -rf`/ENOTEMPTY bug tracked
elsewhere in `ACTIVE.md` — that bug means the delete had no actual effect,
but the attempt still generated namespace-mutation churn). Within minutes:

- Data publication for the newly-written files stalled completely: polled
  three times, 15s apart, `data_publication_bytes_committed` and
  `metadata_generation` were frozen (7615, unchanged) — not slow, stopped.
- `journalctl` showed `FUSE async data publication retry inode=6870
  error=FUSE namespace advanced during data publication` repeating for the
  same inode, and separately `retention claim peer=10.34.1.50 error=control
  RPC deadline exceeded` — a **hard** control-RPC timeout, not the softer
  "stalled (speculative)" debug notice.
- Per-thread CPU accounting (`/proc/<pid>/task/*/stat`) showed one thread,
  `macha-maint` (`Service::loop`, `service.cpp:637`), consuming 13944 ticks
  of user CPU — roughly 10x every other thread combined — pinned near 100%
  of one core for minutes, alongside 19% iowait system-wide.
- gbni-1→es-1 network path was independently confirmed healthy (60ms RTT,
  0% loss, five pings) at the same time, ruling out a transport problem.

## Root cause, confirmed by reading the actual code (not inferred)

`Service::retain_metadata_publication()` (`service.cpp:490`) runs
synchronously as part of accepting a metadata publication (wired via
`metadata->set_publication_retention(...)` in `service.cpp`). It builds a
list of newly-referenced DATA extent IDs and calls
`DistributedStore::retain_data()` (`distributed_store.cpp:580`).

`retain_data()` iterates that list **serially, one extent at a time**
(`distributed_store.cpp:601` `for (const auto& id : ids)`), and for each
extent calls `has_on(candidate, id)` for up to `min_write_replicas`
candidates (`distributed_store.cpp:611`, `634`, `679` — the same pattern
repeated three times in the function: initial claim, post-`put` recheck, and
the fallback-candidate scan). None of this is batched or run concurrently.

`DistributedStore::has_on()` (`distributed_store.cpp:1230`) does one of two
expensive things per call:

- **Local candidate:** `n_.local_store().valid(id)`, which
  (`local_store.cpp:1095`) is `return get(id).has_value();` — the **full**
  read, AES-GCM decrypt and verify path, just to answer a yes/no presence
  question, for every single extent.
- **Remote candidate:** a synchronous `bounded_control_call(target,
  MessageType::have_object, ..., FrameType::speculative)` — a real RPC round
  trip that shares the **same control-plane channel and worker pool** as
  ordinary control traffic (this is the direct explanation for the
  `retention claim ... deadline exceeded` seen on an unrelated peer: an
  unrelated control message got stuck behind, or contended with, a flood of
  `have_object` checks).

At `extent_size: 4M` (this cluster's config), a modest batch of movies is
already tens of thousands of extents. Tens of thousands of sequential
decrypt+hash operations (CPU/IO-bound) interleaved with tens of thousands of
sequential control-plane RPC round trips (network/queue-bound) fully explains
every symptom observed: the pinned CPU core, the frozen publication counters
(everything downstream of metadata acceptance queues behind this one
function call on the single maintenance thread), and the unrelated
control-RPC timeout (shared channel starvation).

This is not new misbehavior — it is the exact, already-documented scaling
cliff, just hit for the first time at real scale. The "not yet urgent"
qualifier on the `LocalStore::valid()` item is retracted by this incident.

## Required invariants

- `retain_data()`'s cost for a batch of N extents must not be N sequential
  round trips of any kind (local decrypt or remote RPC) on the thread that
  also drives metadata-publication acceptance and garbage collection.
- A local presence check must never pay for a full decrypt when existence
  alone is asked for.
- A flood of `have_object` (or any other retention-check) messages must
  never be able to delay an unrelated control-plane message, regardless of
  batch size. This is governing law 3, unconditionally — not "should
  usually hold," but must hold under an adversarial-sized batch.
- Retention-before-acceptance semantics (the actual safety property
  `retain_data`/`retain_control` exist to provide) must not regress: a
  publication must still not be acknowledged as durable until its extents
  have a real placement floor. Speed must come from batching/concurrency and
  cheaper checks, not from weakening the durability guarantee.

## Plan

### Phase 0 — cheap local presence check ✅ done (2026-09-06)

- [x] Add a real existence-only path to `LocalStore`. **Deviation from the
  literal plan text:** `LocalStore::has()` already had exactly this shape
  (packed: `packed_` index membership, no I/O; loose: a single stat, no
  decrypt) — a new `exists()` would have been a duplicate. Instead `has()`
  was hardened: it's now `noexcept` (all filesystem calls use the
  `error_code` overload, wrapped in `try/catch`), and it now additionally
  treats a zero-byte loose file as absent (a cheap `file_size(p, error) > 0`
  check instead of bare `exists()`) — loose writes are temp-file-then-rename
  (`put_loose_locked`), so a real object is never observed partially
  written; zero bytes only happens after external corruption/truncation.
  Documented on the declaration in `local_store.hpp`.
- [x] Switch `DistributedStore::has_on()`'s local branch to the cheap path
  (`local_store.cpp`/`distributed_store.cpp`). **Important correction made
  during implementation:** the plan's list of callers to switch
  (`StoragePool::valid`, `rebalance_step`, `repair_step`) turned out to be
  **wrong for three of the four** — caught by the existing test suite
  (`test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy`,
  `test_replica_repair_does_not_count_corrupt_remote_as_healthy`), not by
  inspection. The distinguishing factor: `has_on()`'s candidate-selection
  probe is safe to weaken **only** because `retain_on()` (both its local
  branch and the `retain_objects` RPC handler) unconditionally re-verifies
  with a full decrypt before ever persisting a retention claim — a corrupt
  candidate gets caught there instead. `rebalance_step`'s per-backend
  discovery loop and `repair_step`'s push/pull presence checks have **no**
  such downstream re-verification step; weakening them silently stops
  corrupt replicas from ever being healed. Those three were left on
  `valid()`, with a comment at each site explaining why. `StoragePool::valid`
  itself was untouched (and has no callers today). Net effect: only
  `has_on()`'s local branch, the new batched `have_objects` server handler
  (Phase 1, safe for the same "commit re-verifies" reason), and the
  single-object `have_object` handler's *local* candidate-selection use are
  on the cheap path; everything with a repair/rebalance placement decision
  stays fully verified.
- [x] Regression: `tests/test_storage_v18.cpp` ::
  `test_has_is_a_cheap_presence_check_not_a_decrypt` — asserts via a new
  `before_loose_read_for_tests_` hook (mirroring the existing
  `before_packed_read_for_tests_` one) that `has()` never triggers the
  decrypt path for either a loose or packed object, that `get()` still does,
  and that a truncated-to-zero loose file is reported absent.

### Phase 1 — batch and parallelise `retain_data` ✅ done (2026-09-06)

- [x] `retain_data()`'s per-object serial candidate scan (both the initial
  scan and the "re-establish placement" rescan) now goes through
  `DistributedStore::select_present_batched()` /
  `batched_have_objects()` (`distributed_store.cpp`/`.hpp`): for every
  object still short of the floor, the next unchecked candidate per object is
  grouped by node into one new `have_objects` (plural) request per node per
  round, rounds proceed concurrently across nodes, and local self-checks
  answer instantly with no RPC. Preference order and the per-object floor are
  unchanged — only the shape of how presence is checked changed. The rare
  per-object fallback loop after a node-level `retain_on` batch failure is
  intentionally left serial (it's an error-recovery path, not the O(N) hot
  path this incident was about).
- [x] Bounded via two new config knobs, `dht.retention_check_batch_size`
  (default 2000 ids/request, validated to fit `max_frame_size`) and
  `dht.retention_check_concurrency` (default 8 batches in flight at once,
  across all peers combined) — documented in `docs/configuration.md` and
  `macha.yaml.example`.
- [x] Retention/claim semantics unchanged: `retain_on`/`retain_objects` still
  fully verify before persisting a claim; the floor requirement
  (`min_write_replicas`) is identical.
- [x] Regression: `tests/test_storage_v18.cpp` ::
  `test_retain_data_batches_a_large_publication_within_bounded_time` — a
  real 2-node cluster with `min_write_replicas == replication == 2` (forcing
  every extent to check *both* nodes), several thousand extents seeded
  directly into both nodes' `LocalStore`s, then `retain_data()` timed
  directly. Uses fewer than "10,000+" (the plan's suggested scale) and is
  registered `MACHA_HEAVY_TEST` — see the note below on why.

  **Test-suite-health note:** this machine's full parallel test run (12
  slots) already flakes intermittently on unrelated timing-sensitive
  integration tests *before any of this branch's changes* — confirmed by
  stashing all changes and running the baseline suite 3 times (2/3 runs had
  one unrelated failure each, always passing in isolation). A first version
  of this regression test used 10,000-12,000 extents and took 25-40s
  (dominated by setup, not `retain_data()` itself), which measurably raised
  that pre-existing flake rate under parallel load; it was reduced to 3,000
  extents (~6s) and marked `MACHA_HEAVY_TEST` (extra scheduling slots) to
  stop contributing to it, while still being an extents-in-the-thousands,
  both-nodes-checked reproduction of the incident shape.

### Phase 2 — get retention-check RPC traffic off the control-plane's shared queue ✅ investigated + regression added (2026-09-06)

- [x] Confirmed by reading `net.cpp`'s `RpcServer` routing
  (`request_class()`/`queue()`/`admit_locked()`/`data_worker_loop()`):
  routing to a worker pool is governed entirely by **`FrameType`**, not
  `MessageType`. `have_object`/`have_objects`/`retain_objects` all use a
  data-lane `FrameType` (`speculative`/`loader`) and are therefore serviced
  by `data_workers_`/`data_worker_loop()` — a pool **already completely
  separate** from `fast_control_workers_`/`control_workers_`, which service
  `ping`/`members` (fast-control) and `put_control_object`/other
  `FrameType::control` traffic. They have never shared a worker pool. Within
  `data_workers_`, `have_object`/`have_objects` (speculative) already sit at
  *lower* dequeue priority than `retain_objects` (loader) and than ordinary
  foreground/read-ahead playback traffic, so a flood of them cannot even
  queue-starve those higher classes.
  **Conclusion on tonight's "retention claim ... deadline exceeded on an
  unrelated peer":** not RPC-queue-level starvation (the queues were never
  shared) but machine-wide CPU/iowait saturation from the O(N) synchronous
  AES-GCM decrypt + disk reads that Phase 0 removes — on both sides, since
  `have_object`'s server handler (used for placement/repair, `cluster.cpp`)
  also does a full decrypt per request and was receiving the same flood from
  every peer's own concurrent `retain_data()` calls. Phase 0's fix (cheap
  presence checks) is what actually removes the CPU/iowait storm; Phase 1's
  batching reduces round-trip *count*, which bounds `retain_data()`'s own
  latency but was not itself the mechanism starving the unrelated peer.
- [x] Given the above, no new worker pool or priority scheme was added —
  the existing `FrameType`-based separation already satisfies "must never
  starve `put_control_object`/other ordinary control messages," and adding
  one would be an unjustified extra abstraction for a property that already
  holds. What *was* missing was a regression test proving it holds under an
  adversarial batch, which is real and worth having independent of Phase 0.
- [x] Regression: `tests/test_rpc_cluster.cpp` ::
  `test_have_objects_flood_does_not_delay_unrelated_control_rpc` — occupies
  every one of the 8 fixed `data_workers_` with blocked `have_objects`
  requests (the adversarial-batch shape), then asserts `ping`/`members`
  still complete in well under their ordinary latency budget throughout.
  This is the test that would have caught a future regression that merges
  the two worker pools or reclassifies `have_objects`'s `FrameType`.

### Phase 3 — UAT (not done — needs the real cluster, out of scope for this session)

This phase requires `corvus-es-1` and the other real cluster nodes
(SSH-deployed hardware, not something this session has standing access to
exercise destructively on its own initiative). Both checklist items below
are unstarted:

- [ ] Reproduce tonight's exact scenario in a test/staging environment: a
  large concurrent batch of new-file writes plus a concurrent bulk-delete
  attempt against the same namespace subtree, on hardware comparable to
  `corvus-es-1`. Verify: control RPCs (session, status, retention-claim,
  arbitrary peer-to-peer control messages) stay within their ordinary
  latency budget throughout; `data_publication_bytes_committed` advances
  continuously rather than freezing; the maintenance thread's CPU usage is
  proportionate to real work, not pinned to one core for minutes at a time.
- [ ] Re-run this exact plan's Phase 0/1/2 regressions against a real
  three-node cluster (not just the in-process test harness) as a final
  check, since tonight's incident was specifically a real-network,
  real-hardware event that an in-process test alone would not necessarily
  reproduce.

## Explicitly out of scope for this plan

- **Short-circuit unlink** (stop publishing/reclaim spool immediately when a
  file is unlinked before its write finishes publishing) is a real, valuable,
  separate optimization the user raised in the same incident discussion. It
  reduces how much work piles up in a write+delete-churn scenario like
  tonight's, but does not fix the underlying O(N) serial-check defect above
  (a pure bulk-write workload with zero deletes hits the same wall). Tracked
  as its own item in `ACTIVE.md`, not a phase of this plan.
- **The `rm -rf`/ENOTEMPTY namespace-consistency bug** and the getattr
  cross-node-projection questions from earlier tonight remain a separate,
  not-yet-root-caused item in `ACTIVE.md`. They are plausibly related (both
  are namespace/publication-pipeline correctness issues surfacing under
  bulk churn) but were not confirmed to share a root cause, and fixing this
  plan's defect does not imply that one is fixed too.
- **Whole-namespace reset-to-empty as an operator action.** No existing
  Macha primitive can safely bulk-empty a populated namespace subtree today:
  the FUSE `rm -rf` path is the already-tracked broken one, and the
  `DELETE /api/v1/manage/filesystem` route (`manage_api.cpp:796`) only does
  one non-recursive `rmdir`/`unlink` per call (and `rmdir` still requires an
  empty directory, so it cannot bulk-clear a populated tree either — and may
  share the same underlying bug). Inventing a new bulk-reset primitive
  during an active incident, on a system where multiple serious namespace
  bugs were found the same night, was deliberately not attempted live.
  If this is still wanted, it needs its own careful design (most likely a
  single durable "replace root's children with empty" metadata mutation,
  not a recursive walk-and-delete) and should be scoped as its own follow-up
  rather than folded into this plan.

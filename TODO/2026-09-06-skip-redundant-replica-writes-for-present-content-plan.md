# Skip redundant replica writes when a target already has the content

Status: **experimental / speculative — not scheduled, not started, do not
pick this up opportunistically.** Explicitly shelved on 2026-09-06 after the
cost/benefit didn't clear the bar: the triggering scenario (re-writing
content byte-identical to something already unreclaimed on a target) is a
"recover from an accidental delete within the grace window" event, not an
ongoing workload pattern, while the implementation cost is real — it
touches `put_impl`, the most complex/correctness-critical function in the
write path, needs a new admission-control design that doesn't exist yet
(see Phase 1), and needs its own multi-dimensional regression suite before
it'd be safe to ship. The cheaper move for an actual one-off "oops" is just
letting `garbage_grace_ms` run out, or lowering it temporarily for that one
cleanup — not new distributed-systems code.

**Revisit this only if the triggering scenario becomes a recurring pattern**
— concretely, if `corvus-es-1`'s link (offsite, the cluster's most
bandwidth-constrained hop) starts repeatedly eating full re-transfers of
large libraries, e.g. from periodic rebuilds/resyncs rather than a single
incident. If that happens, the design work below is already done and
review-hardened; it doesn't need re-deriving.

Follow-on to `2026-09-06-retention-check-batching-and-cheap-presence-plan.md`,
raised in conversation while deploying that plan's fix and discussing what
happens if a user re-syncs already-orphaned-but-not-yet-reclaimed content
within the 24h GC grace window. Design-reviewed in the same conversation
(2026-09-06) before any implementation started — see the admission-control
requirement in Phase 1 and the streaming/pipelining requirement in Phase 2,
both found during that review, not part of the original sketch.

## Problem

`DistributedStore::put_impl()` (the foreground write fan-out) and
`DistributedStore::put_on()` (used by `replicate_all()` and by
`retain_data()`'s re-establish-placement fallback) both unconditionally
transmit the complete extent payload to every remote replica target:

```cpp
Writer writer;
writer.fixed(id.bytes);
writer.bytes(data);                 // full plaintext extent, every time
n_.call_async(owner, type, payload, frame_type);   // put_impl, distributed_store.cpp:319
...
n_.call(target, MessageType::put_object, writer.data(), frame_type);  // put_on, distributed_store.cpp:791
```

Content is addressed by SHA-256 of its bytes (`ObjectId`), so a remote
target may already hold the exact object being written — most commonly:
deleting a file, then re-writing byte-identical content before the deleted
extents are physically reclaimed (`maintenance.garbage_grace_ms`, default
24h). `LocalStore::put_impl`'s reaffirmation path (`local_store.cpp:980-1023`)
already detects and short-circuits this *locally* (same-node): a matching
loose object is touched (mtime refresh), not re-encrypted/rewritten. That
optimization does not extend to replication: every remote target still gets
the full bytes over the network, with no presence check first.

## Why this isn't a trivial "just check have_objects first" change

Two real obstacles, both discovered in conversation, not assumed:

1. **Round-trip tax on the common case.** A presence check before every PUT
   doubles round trips for a single, genuinely-new write (check → miss →
   send), which is the overwhelmingly common case. This is only a good trade
   when there's a natural batch to amortize the check against — the same
   lesson the retain_data() batching plan already established. See "Phase 0"
   below: the actual write-dispatch granularity needs confirming before this
   can be scoped precisely.

2. **The durability-barrier token gap.** `put_deferred()`'s reply carries a
   concrete `(epoch: NodeId, domain: u64, generation: u64, backend_instance:
   u64)` tuple (`cluster.cpp:864-867`, decoded at `distributed_store.cpp:
   370-376`) that `DurabilityBatch`/`object_durability_barrier()` later waits
   on. `have_object`/`have_objects` today return only a boolean — no
   generation. Skipping the PUT needs somewhere to get that tuple from
   instead.

3. **(Found reviewing this plan, before implementation started) The orphan-
   clock gap — this is the one that actually matters for correctness, not
   just efficiency.** `LocalStore::put_impl`'s reaffirmation touch
   (`local_store.cpp:1013-1016`, `last_write_time(loose_path, now)`) is
   *currently the only thing* that resets `StoragePool::gc_step`'s orphan-age
   check (`storage_pool.cpp:896`, `remove_if_older_than(id, orphan_grace)`)
   for an existing object between the moment it's referenced by a new write
   and the moment `retain_data()` actually marks it *retained* (fully immune
   to the orphan check, not just clock-reset) at metadata-publication-
   acceptance time — which can be well after the write itself (the original
   incident had this stage stall for *minutes*; GC's maintenance loop runs
   every `maintenance.interval_ms`, default 1s). A skip that omits the PUT
   also omits this touch. For an object that's already close to its grace
   deadline (exactly the "re-sync near-expiry content" case motivating this
   whole plan), GC can win that race and reclaim it before `retain_data()`
   ever runs.
   **Worse:** the *local* branch of `put_impl` always calls a real
   `LocalStore::put()` (never skipped — only the remote branch is a skip
   candidate), so the originating node always self-protects. But the
   originating node is not guaranteed to be one of `ranked(id)`'s selected
   placement candidates. If it isn't, *every* selected replica for that
   extent is a remote, skip-optimized one; if they're all similarly aged
   (plausible — likely all written in the same original ingest), all of them
   can independently cross their orphan deadline with nothing touching any
   of them. If GC wins on all of them before `retain_data()` runs, there is
   no surviving copy anywhere for the existing "re-establish placement"
   fallback (`distributed_store.cpp:665-696`) to recover from. That's
   unrecoverable data loss for that extent, not self-healing.

## Required invariants

- A write that decides to skip sending bytes to a replica because that
  replica already holds the content **must** produce the same liveness
  guarantee for that copy that an actual `put()` would have: at minimum, an
  immediate orphan-clock refresh (matching `put_impl`'s reaffirmation
  touch), so it can never be reclaimed by GC racing ahead of the eventual
  publication/retention step.
- This liveness side effect must be a **separate operation** from the plain,
  read-only presence probe `retain_data()`'s candidate-selection scan
  already uses (`DistributedStore::has_on()`/`select_present_batched()`/the
  batched `have_objects` from the prior plan), not a variant/flag on it.
  `have_objects` stays completely unmodified and is reused as-is for
  *selection*: deciding which ranked candidates already have the content,
  which necessarily probes candidates that won't all end up chosen, so it
  must stay side-effect-free. The new touch-and-token operation is called
  only on the candidates a write actually decides to keep — this is the
  exact same two-phase shape `has_on()` (probe, no side effect) →
  `retain_on()` (commit, only on the selected set) already uses for
  retention; the write path gets its own commit-side operation playing
  `retain_on()`'s role, rather than overloading the probe.
- Must not weaken `min_write_replicas`/durability-barrier semantics: a
  skipped replica still needs a valid `DurableReplica` (epoch/domain/
  generation/backend_instance) for `object_durability_barrier()` to await,
  exactly as strong as a freshly-written one.
- Must not add a round trip to the common case (a single write of genuinely
  new content) — the presence-plus-touch check must be batchable across
  however many extents are actually in flight together, not paid per extent
  in the sequential-single-write case.
- Existing safety nets (repair's own presence-then-push, same-node dedup,
  `retain_data()`'s full re-verify before persisting a retention claim) stay
  as they are; this plan doesn't touch them, and continues to rely on
  `retain_data()`'s existing re-verify as the *last-resort* backstop for
  this new path too, not as its primary protection (that's what the touch
  invariant above is for).

## Plan

### Phase 0 — confirm the actual write-dispatch batching granularity

`filesystem.cpp` accumulates a shared `DurabilityBatch` across multiple
`put_deferred()` calls as a file streams in (`filesystem.cpp:410`, `:1140`),
but each call dispatches one extent at a time as its data becomes available
— extent N+1's ID isn't known until its bytes have arrived. `fuse.
commit_workers: 8` and `publication_quantum_bytes` suggest some in-flight
parallelism/grouping, but the exact shape (how many extents are typically
ready-to-dispatch-together at a given instant, across one file and across
concurrently-ingesting files) has not been confirmed against the real code.
This determines whether "batch the presence-plus-touch check" is a
meaningful per-publication win (the original incident's shape: many files
ingested around the same time) or needs a different insertion point (e.g.
opportunistic micro-batching of whatever's currently queued across
`commit_workers`, not a whole-publication batch known in advance).

- [ ] Read the actual spool-drain/commit-worker dispatch path in
  `filesystem.cpp` and `fuse_frontend.cpp` and document, concretely, what
  set of not-yet-dispatched extent IDs (if any) are known together at the
  moment a batch of `put_object`/`put_object_deferred` calls would be
  issued, across both a single large file and a burst of many small/medium
  files (the incident's actual shape).

### Phase 1 — wire protocol: a commit-side "activate" op, `have_objects` untouched

Two operations, not one, mirroring `has_on()`/`retain_on()`'s existing
probe-then-commit shape:

- **Selection probe:** the existing, unmodified `have_objects`/`have_objects_
  reply` from the prior plan. Used exactly as `retain_data()` already uses
  it — to find out, for a batch of ids and a batch of ranked candidate
  nodes, which candidates already have the content. Read-only, no side
  effects, no wire changes. Safe to call speculatively on more candidates
  than a write ends up choosing (e.g. capacity-fallback candidates it
  doesn't end up needing).
- **New: `activate_objects` / `activate_objects_reply`.** Called only on the
  candidates a write has *already decided to keep* (its `floor`/`target`-
  many chosen replicas), never speculatively during selection. Request:
  same shape as `have_objects` (count + N ids). Server handler
  (`cluster.cpp`, new case): for each id, re-check presence itself (closing
  the race window between the selection probe and this call — the object
  may have been reclaimed in between), and for each still-present id: touch
  it exactly as `put_impl`'s reaffirmation path does (reuse that logic
  rather than duplicate it — this is the operation that plays `put()`'s
  liveness-protection role for a skipped write), then report back the same
  `(epoch, domain, generation, backend_instance)` tuple `put_object_
  deferred`'s reply already carries — sourced from `provisional_
  generations_` if the object is still tracked there, otherwise the
  backend's current `durable_generation()` (an object outside the
  provisional window must already belong to an earlier, completed
  durability checkpoint, so reporting the current durable watermark is
  correct and makes any later `await_durable()` on it a no-op — no new
  tracking structure needed for the already-durable case). **This fallback
  is safe by construction, not merely probably-fine, provided it's
  implemented as "read `durable_generation()` live, at the moment of the
  check, and report exactly that value"**: `generation <= durable_generation()`
  is then trivially true against itself. There is no cached/stale value in
  play and no way for this to under- or over-claim durability. (Checked in
  review: unlike a wrong *presence* answer here, which `retain_on()`'s later
  full re-verify would catch, nothing downstream re-verifies a *durability*
  claim — `object_durability_barrier()` trusts the generation it's given.
  That makes this fallback worth getting exactly right rather than
  approximately right, but the "read live, report live" rule above is
  sufficient; no additional design is needed here.)
  **Server-handler admission control is a hard requirement, not a nice-to-
  have:** for a *packed* object, this touch is not the loose case's free
  mtime bump — it's `put_impl`'s existing packed-reaffirmation path
  (`local_store.cpp:940-969`), which appends a real `pack_touch` record
  under `pack_io_mutex_` with real disk I/O and a real (small) accounting
  increment. A handler that loops over a batch of a couple thousand ids and
  performs that locked I/O serially, one thread, one request, is the *same
  shape* of bug as the incident this whole line of work started from — just
  with a cheaper per-item cost, not a categorically different one the way
  `have_objects`'s pure index-lookup/stat check is. This needs its own
  bound/backpressure design (a batch-size cap alone, copied from
  `retention_check_batch_size`, is not sufficient justification — that
  number was sized for genuinely-cheap presence checks, not locked pack
  writes) before this phase can be considered done. Any id that
  turns out absent on re-check is reported absent; the caller treats that
  exactly like a fresh id (falls through to a real `put_object`/`put_object_
  deferred` for it) — the race costs one extra round trip for that one id,
  never a lost write.
  Reply: count + per-id `{present: bool, epoch, domain, generation,
  backend_instance}` (only meaningful when present).

### Phase 2 — wire it into the write path

- `DistributedStore::put_impl()`'s remote branch (`distributed_store.cpp:
  ~312-324`) and `put_on()` (`:775-798`) need a selection step in front of
  them: for the candidates about to be dispatched to, batch a `have_objects`
  probe first (batched per Phase 0's finding — a single extent still costs a
  round trip either way, so this only pays off where a real batch exists).
  Exactly how tightly this integrates with `put_impl`'s existing dispatch
  machinery (the `initial`/`next_fallback`/`replacement_needed`/spill
  handling around concurrent primary+fallback candidates) is an open
  implementation-shape question, not assumed here — it may be cleanest as a
  pre-pass that shrinks the "still needs an actual put" candidate set before
  today's existing loop runs unmodified on whatever's left.
  **Hard requirement, not an implementation detail:** today, `put_impl`
  pipelines many concurrent `put_object` RPCs across nodes as extents become
  available while a file streams in — extent 1 and extent 5000 can be in
  flight at the same time. Whatever shape this integration takes, it must
  not turn that into a batch-synchronous "collect everything, probe
  everything, activate everything, *then* start any real sends" gate —
  that would delay genuinely-new content (the overwhelmingly common case)
  behind a check it gets no benefit from, trading a real throughput
  regression for a bandwidth optimization that only helps a narrow case.
  Per-extent streaming/pipelining for the "turned out absent" path has to
  survive this change essentially unchanged.
- For candidates the probe found present: batch an `activate_objects` call
  against them (only this final, already-decided set — never the full
  candidate list). On a hit: record the returned tuple as that target's
  `DurableReplica` directly (mirroring `distributed_store.cpp:384-389`'s
  bookkeeping), without ever building or sending the payload. On a miss for
  a specific id (the presence-probe/activate race) or any error/timeout:
  that id/candidate falls through to today's unconditional `put_object`/
  `put_object_deferred` send, unchanged.
- For candidates the probe found absent: today's unconditional send,
  unchanged, from the start — no wasted round trip for genuinely new
  content beyond the one batched `have_objects` probe already amortized
  across the whole batch.

**Operational caveat, not a blocker:** the safety margin this whole
mechanism relies on (how long an `activate_objects` touch protects a copy
before `retain_data()` gets around to actually retaining it) is proportional
to `maintenance.garbage_grace_ms`. An operator who tunes that down
aggressively for faster space reclamation shrinks that margin too — this
isn't new (the ordinary write-to-publish window already depends on the same
setting today), just worth a line in `docs/configuration.md` when this
ships.

**Reviewed and ruled out as non-issues, recorded so they aren't
re-litigated:** storage accounting stays correct either way (a real
reaffirmation's bytes-used delta is zero, same as today, since no new bytes
land); `scrub_step`'s corruption sweep is unconditional and age-independent
(`storage_pool.cpp`'s `next_physical` walk), so nothing about this hides a
corrupt object from it; the resulting redundancy (N independently-verified
physical copies across N nodes) is identical whether a given copy arrived
via fresh push or reuse, so no fault-tolerance guarantee is being
short-changed; and this doesn't interact with `RetentionStore`'s
observed-remove tombstone semantics at all, since actual retention claims
still go through the unchanged `retain_data()`/`retain_on()` path with a
fresh `RetentionDot` — `activate_objects` only ever touches the physical
storage layer's liveness bookkeeping, never retention state.

### Phase 3 — regression tests

- **The correctness gap that motivated this plan, made concrete:** pre-seed
  a target with a copy whose age is deliberately close to `orphan_grace`
  (inject via the existing `before_*_for_tests_` hook pattern or a test-only
  clock), perform a skip-optimized write referencing it, advance time past
  the original deadline without publication having been accepted yet, run
  `gc_step`, and assert the object survived (the `activate_objects` call
  reset its clock). Then repeat with a version that skips straight from the
  `have_objects` probe to recording a `DurableReplica` without ever calling
  `activate_objects`, to prove the test would have caught the exact
  regression this plan exists to prevent.
- Race safety: force an id to disappear between the `have_objects` probe and
  the `activate_objects` call (e.g. via a test hook that removes it in
  between), and assert the caller correctly falls back to a real
  `put_object` for that one id rather than recording a phantom replica.
- Bulk-`activate_objects`-against-packed-objects must not reintroduce the
  incident's scaling cliff: synthetically reproduce a large batch (thousands
  of ids) of already-present *packed* objects and assert the server handler
  completes in bounded time without monopolizing its worker thread — the
  same shape of regression test as
  `test_retain_data_batches_a_large_publication_within_bounded_time`, aimed
  at `activate_objects`'s packed-reaffirmation path specifically rather than
  the read-only `have_objects` path that test already covers.
- Streaming: assert that, during a large single-file write where most
  extents are genuinely new, a `put_object` for an early extent completes
  without waiting on a batch-wide probe/activate phase for later extents —
  guards the Phase 2 pipelining requirement.
- Skip-vs-send: assert `put_object`/`put_object_deferred` RPC count and
  bytes-on-the-wire drop when the target already holds the content, using
  the same `RpcServer`/`RpcClient`-harness pattern as
  `test_have_objects_flood_does_not_delay_unrelated_control_rpc`.
- Durability-barrier correctness for a skipped-but-not-yet-durable
  replica: confirm `object_durability_barrier()` still genuinely waits
  (doesn't silently no-op) when the reported generation is provisional, not
  yet past the target's own `durable_generation()`.
- Staleness backstop still holds: force the confirmed-present copy to
  vanish/corrupt between the confirm-and-touch call and `retain_data()`'s
  later full verify, and assert the existing re-establish-placement fallback
  (`distributed_store.cpp:665-696`) still correctly recovers — this plan's
  new path must not be the *only* thing standing between "confirmed" and
  "actually durable."

## Explicitly out of scope

- Same-node dedup: already correct (`LocalStore::put_impl`'s reaffirmation).
- `repair_step`'s own presence-then-push convergence loop
  (`maintenance_has_on`/`maintenance_put_on`): already checks presence
  before pushing, untouched by this plan.
- CONTROL/metadata object replication: far smaller volume, not the target of
  this optimization.
- Removing the need for genuinely new content to cross the network at all —
  this plan only avoids re-sending bytes the target can already prove it
  holds.

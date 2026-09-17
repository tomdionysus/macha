# Plan: the test suite must be deterministic

Date: 2026-09-14

Status: next piece of work after 0.41.0. Supersedes the framing of the three
"load-dependent test flakes" items in `ACTIVE.md`, which have been rewritten
five times as observations and never once as defects to fix.

## The position

A test that fails under load and passes in isolation is not a flaky test. It
is either a defect in the product or a defect in the test, and "known flake"
is the name we have been giving the decision not to find out which. That
decision has a cost we paid today, and the suite is now load-bearing for a
subsystem whose whole purpose is surviving faults -- a suite that cries wolf
cannot gate that.

**Evidence from 2026-09-14, during the 0.41.0 work.** Six full-suite runs on
one macOS laptop produced seven failures across six *different* cases, every
one of which passed in isolation:

| case | suite |
|---|---|
| `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node` | rpc_cluster |
| `rpc_cluster/test_bootstrap_joiner_requires_complete_checkpoint_survey` | rpc_cluster |
| `rpc_cluster/test_mutual_bootstrap_prunes_cross_dial` | rpc_cluster |
| `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst` | hydration |
| `storage_v18/test_small_objects_are_packed_and_recover_after_restart` | storage |
| `filesystem_fuse/test_fuse_recovery_thousand_operations_have_bounded_publications` | fuse |

The first one was **not a flake**. It was a real regression introduced that
afternoon: 0.41.0 added `libmacha-fuse.so` to the build's shared plugin
directory, and that test points two full `Service`s at that directory, so both
of them began dlopening libfuse/macFUSE for no reason the test cares about.
Five consecutive clean runs after giving it a directory holding only the
plugin it actually wants. It had already been dismissed once in the same
session as "the known flakes", which is exactly how a real defect hides inside
that phrase.

The other five may each be product, test, or host. Nobody knows, because the
answer has never been produced.

## Progress, 2026-09-15 (first pass, all on the macOS laptop)

Measurement tooling first, then every failure it produced, classified. The
numbers below are from `--repeat` runs at the runner's default parallelism
(12 slots), single runner, one binary per run.

**Runner changes**

- `--repeat N` runs every selected case N times interleaved across the
  slots and prints a per-case failure count. The way to *measure* a suspected
  flake instead of recalling it.
- `MACHA_TEST_LOG_LEVEL=DEBUG` raises the product log level inside each
  case. Output is only ever shown for a failing case, so it is free on a
  green run and is what turned "it timed out" into a `shutdown:` trace.
- A per-run port salt (`MACHA_TEST_PORT_SALT`, exported to children). Two
  runners on one machine previously gave the same case index -- and so the
  same 64-port block -- to two different tests, and because every test
  cluster shares one deterministic key the result was not `bind failed`
  but a foreign node authenticating into the other test's cluster
  (`metadata write-floor policy mismatch peer=…`). My own first "heavy"
  sweep (two suites at once) was invalidated by exactly this before the
  cause was found; its 40-odd failures are discarded.
- `TempDir` now starts empty. Its name is pid-based, a case the runner
  kills on timeout never runs the destructor, and pid reuse then handed a
  later case another test's node state (99 leftover directories found).

**Classified**

| case | measured | verdict | fix |
|---|---|---|---|
| `rpc_cluster/test_mutual_bootstrap_prunes_cross_dial` | 1/5,292 | test: sampled before a late simultaneous dial landed | wait for a quiet window before asserting no churn |
| `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node` | 1/5,292 | test: asserted `state=="queued"` on a reply whose state is re-read after the lock; the worker can re-fail first | assert the 200 (`changed`) and a state resume can lead to |
| `rpc_cluster/test_early_replication_quorum` | 6/10 and 5/20 at DEBUG | test: `put()` right after `start()` with no readiness wait | `wait_local_state_ready` |
| `storage_v18/test_catalogue_metadata_ignores_full_data_quota_…` | 1/2,646 at DEBUG | test: `preferred_for()` assumed the preferred node was already in active membership | the helper waits for that precondition |
| `media_playback/test_instructions_are_performed_not_negotiated` | 1/3,528 at DEBUG | **infrastructure**: inherited a killed test's state dir (above) | `TempDir` starts empty |
| `rpc_cluster/test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` | 1/12 at INFO, 0/38 at DEBUG | test half: `dead_after=200ms` asserted a 200 ms ping round trip on a loaded laptop, which is not the subject; **product half**: see the shutdown hang | `dead_after=2s`, per the precedent in the coalesced-burst test |
| `rpc_cluster/test_replacement_node_recovers_namespace_and_replication` | 2/10 at DEBUG (120 s timeout), 0/14 since | **product**: `RpcServer::stop` hung after `RPC sessions reaped` | see below |
| `rpc_cluster/test_ingest_torrent_jobs_…` (first sighting) | earlier session | product/test coupling: the shared plugin dir | own plugin dir |

**The shutdown hang (product).** The DEBUG trace of both 120 s timeouts
ends at `shutdown: RPC sessions reaped`; the next thing `RpcServer::stop`
did was join the worker pools, and the worker loops only return once their
queue is empty -- so every request that arrived before stop() was executed
*during* shutdown, against a node whose outbound transport, retained-memory
ledger and local writer were already stopped, and queued requests were only
dropped after the join. A handler that blocked there blocked shutdown. Fixed
by dropping the queues (with error replies) before the join, and the join
now logs `RPC workers joining/joined` so the next trace names the stage.
**Not proven closed**: the exact blocking handler was never captured (a
sampler that takes thread stacks of any child alive >50 s is in
`scratchpad`, and 14 further full runs did not reproduce it). The
lagging-replica 60 s timeout at INFO has the same shape and the same fix
applies. Treat a recurrence as the next thing to chase, with the sampler.

**`hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`**
-- the case `ACTIVE.md` records as having been waved past on "at least five
separate occasions" -- was four separate defects in one test, each found by
instrumenting the assertion that fired and then reading the numbers rather
than re-running until green:

1. *Exactly one catalogue repair per burst.* Not a property the product has.
   The gated owner run and its mandatory coalesced follow-up each dirty the
   catalogue if they advance the committed generation, so one or two repairs
   are both correct. Captured: `before=2 after=4 scheduled_delta=2
   requested_delta=20` -- twenty demand events, two repairs, which is
   coalescing working. Now asserted as a ratio (repairs ≤ ¼ of demand
   events), which still catches a per-event storm.
2. *No catalogue repair while metadata repair is gated.* The gate stops this
   node's repair pass; it does not stop its committed generation advancing,
   because `publish_commit` stores and accepts commits on replicas directly.
   Captured: `gated_repairs=4 s1_committed=12 final_generation=13`. The
   assertion is gone; the coalescing bound already spans the gated window.
3. *Baseline sampled after the quiescence wait.* Between the predicate
   returning true and the separate sample, another run was scheduled:
   `before_scheduled=5 before_completed=4`, so every delta was off by one.
   The baseline is now the snapshot that satisfied the wait.
4. *The capture latched the wrong repair.* `capture_catalogue_repair` was set
   before the burst, so the first repair after it could be one for the single
   pre-burst upsert: `repair_scheduled=7 before_scheduled=6
   repair_requested=10 before_requested=9` -- one run, one event, asserted
   against as though it were the burst. It is now latched immediately before
   the gate opens, so it captures the repair that processes the burst.

600+ reps clean afterwards.

**Two product defects found by this method, both fixed in 0.41.1.**

The same test's GC assertion (superseded catalogue artwork reclaimed on both
nodes within 12 s) missed ~1 in 150 reps, and the instrumentation said it was
not a slow sweep: *all four* objects remained, *all on s2 only* -- the node
that wrote them -- with both catalogues converged. Six DEBUG hunts, each
adding the one fact the previous one could not decide, went: the claims are
held on s2 → the claims' dots are all *below* the release clock and no
release was ever applied to them → s2's sweep gate never opened after the
burst → s2's maintenance loop made one pass in 27 s → it is in `wait` → it
went to sleep `busy=1 gc_quiet_ms=-2 wait_ms=2591999883`. A busy pass
suppresses GC and only re-armed its wake-up while the foreground was still
inside its quiet period at the end of the pass; one that started busy and
ended quiet, with the GC window already elapsed, slept until the scrub
deadline. `Service::loop` now always bounds a busy pass by the remaining
quiet period. The diagnostics stayed (see CHANGELOG 0.41.1). Verification:
300/300 reps at `MACHA_TEST_LOG_LEVEL=DEBUG` (previously 1-2 misses per 60), then three full-suite runs, 441/441 each (2026-09-15).

`filesystem_fuse/test_fuse_publication_quanta_are_fair_and_byte_bounded`
failed three assertions at once *after* `wait_for_idle(30s)` returned true
(1 in 2,646 case-runs). `FuseFrontend::status()` sampled the queue, the
deferred inodes and `active_data` at three different times while a
publication cycles between them under `data_queue_mutex`, and had an
uncounted window between a decided enqueue and the push. It now takes every
count under `data_queue_mutex` in one sample and counts
`data_enqueue_pending`. FUSE suites 79/79 and 7/7 after the change.

**Still open.** The two aarch64 cases below (no Pi reachable from here
this session).

## Measured 2026-09-17: the suite is still not deterministic on either platform

Measured while verifying an unrelated metadata change, by building pristine
`HEAD` (69a02ee, 0.43.0) into a separate tree and running the whole suite at
default parallelism on both platforms. Rates on **clean `develop` with no local
changes**:

| Platform | runs | runs with a failure | cases that failed |
|---|---|---|---|
| macOS laptop, 12 slots | 6 | **4** | `storage_v18/test_durability_barrier_reports_objects_a_restarted_peer_lost`, `storage_v18/test_durability_barrier_rederives_placement_after_peer_restart` |
| es-1 (aarch64), ~1.8 effective | 8 | **1** | `rpc_cluster/test_rpc_v15_bidirectional_and_deduplication` |

The same tree carrying a metadata `reserve()` change and a version bump gave
4/6 on macOS (same two cases, same rate) and 3/10 on es-1
(`rpc_cluster/test_rpc_v15_bidirectional_and_deduplication`,
`rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`,
`media_playback/test_immutable_media_profile_survives_cold_playback_manager`).
The change is not the cause: the macOS rate is identical either side, and
`test_rpc_v15_bidirectional_and_deduplication` fails on the pristine baseline
too. With n=8 and n=10 the es-1 difference is not distinguishable from noise.

Two new named cases, both reproducible on clean `HEAD`:

- **`storage_v18/test_durability_barrier_reports_objects_a_restarted_peer_lost`**
  and **`storage_v18/test_durability_barrier_rederives_placement_after_peer_restart`**
  — macOS, roughly 1-in-2 per run across the pair, often both in the same run.
  The highest-rate cases known on any platform; they were not on the list
  before because previous sweeps were run after the FUSE work, not on a
  pristine tree.
- **`rpc_cluster/test_rpc_v15_bidirectional_and_deduplication`** — es-1,
  ~1-in-8, fails in 12 ms, which points at a connection/port race rather than a
  timeout.

Neither is a "known flake" to be waved past; both are the next piece of work
under this plan.

A pristine `HEAD` (69a02ee) tree is built and left at `/root/macha-baseline` on
es-1 for exactly this comparison — `./build/macha-tests` there is 0.43.0 with no
local changes, so a rate can be re-measured against it without spending 25
minutes rebuilding first. Rebuild it from a laptop with
`git archive HEAD | ssh root@10.34.1.50 'tar x -C /root/macha-baseline'` if it
has gone.

## Also 2026-09-17: ordinary tests load the *installed* plugin directory

Not a flake, a test-isolation defect, found because the version bump made it
audible. `config_for` (`tests/test_support.hpp`) deliberately left
`plugin_path` unset, with a comment saying an ordinary test wants no subsystem
plugins and must "never" use the installed default. But `NodeRuntime` runs every
`Config` through `normalize_config`, which fills an *absent* `plugin_path` with
the installed directory (`src/config_base.cpp:553-554`). So every `TestService`
and `TestNode` on a machine with Macha installed was dlopening
`/usr/lib/macha/plugins/libmacha-{torrent,fuse}.so` — testing whatever is
deployed on the build box rather than the build under test, and paying a
libtorrent dlopen in every isolated case, both of which the comment existed to
prevent.

It was silent for as long as installed and built versions matched. Bumping es-1's
build to 0.43.1 against an installed 0.43.0 surfaced it as
`plugin 'libmacha-torrent' build identity mismatch: plugin=0.43.0+unknown
core=0.43.1+unknown; refusing to load (partial deploy?)`.

Fixed by setting `c.plugin_path` to an *engaged but empty* path, which is the
only way to say "builtin subsystems only" — `normalize_config` skips an engaged
optional and `Service` maps an empty path to `<builtin>`
(`src/service.cpp:99`). Note this did **not** explain the failures above: the
es-1 run that failed most recently had zero plugin mismatches.

## What "done" means

- The full suite passes 20 consecutive times on gbni-1 and on es-1 at the
  parallelism CI actually uses, not `--serial`.
- No case in the suite is documented anywhere as expected to fail sometimes.
- A new failure is therefore information, and a red run blocks a deploy.

## Approach

**1. Measure before theorising.** Add `--repeat N` to the runner and a
`tools/flake-sweep` that runs the whole suite N times, at a given
parallelism, and emits a per-case failure rate. Every claim below has to be a
measured rate on named hardware, not a recollection. This is the piece that
does not exist today and is why these items keep being re-observed instead of
fixed: `ACTIVE.md` already records that
`hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
was called "passes in isolation" five separate times before anyone measured
it at 8-in-10 on gbni-1.

**2. Classify each case, and write the answer down.** For each failing case,
one of exactly three verdicts, with the evidence:
- **Product defect** -- a real race the load exposed. Fix the product; the
  test stays as it is. This is the outcome the phrase "known flake" has been
  hiding, and is why nothing may be deleted before it is diagnosed.
- **Test defect** -- the test asserts something it never guaranteed: a
  deadline that is really a performance assumption, a `wait_until` racing a
  background pass, shared global state (the plugin directory above), a port or
  a temp path collision. Fix the test to assert what it means.
- **Host defect** -- the case needs more of the machine than the parallelism
  gives it. Mark it as needing a serial slot *in the runner*, so the suite
  schedules it rather than an operator remembering.

**3. Start with the two that are already measured.** Both are in `ACTIVE.md`
with rates on real hardware, and neither needs load to reproduce, so they are
the cheapest:
- `rpc_cluster/test_concurrent_reads_during_divergence_produce_one_reconciliation`
  -- **Test defect. Fixed 2026-09-15 (0.43.0).** Measured on es-1 at debug
  level before the fix: 10 in 40 (8 at the setup `REQUIRE`, 2 at
  `history_after - history_before == 1`); after: 0 in 40. Both failure modes
  were one fault. The test ran two `Service`s, and accepting a sibling head
  announces it (`accept_metadata_commit` -> `announce_metadata_generation`
  -> `signal_service_event(metadata)` + a `metadata_notice` broadcast), so
  both Services' maintenance loops started reconciling the divergence the
  moment the second sibling was accepted. If that background merge finished
  before the test looked, the setup assertion saw one head; if its history
  frame had been appended (`store_commit`) but its certificate not yet
  installed, the setup assertion saw two heads, `history_before` already
  counted the merge, and the readers found nothing to reconcile -- history
  delta 0. The merge is deterministic (same parents, same commit), so no
  head was ever dropped: the product was working as designed and the test
  was racing it. It now runs bare `NodeRuntime`s and one `MetadataManager`,
  which is the unit the claim is about, and nothing else can reconcile.
- `hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
  -- **Product defect. Fixed 2026-09-15 (0.43.0).** Measured on es-1 before
  the fix: 2 in 6 in isolation; after: 0 in 20. `IngestManager::
  ensure_namespace_parents` does `getattr`, and on ENOENT `mkdir`. Every
  import under one scanner root shares that root (`choose_destination` puts
  a movie at `<movies root>/<title (year)>/<file>`), so with four workers
  planning into a fresh namespace two of them see ENOENT for `/Movies` and
  both ask for it; the metadata mutation retries the loser against the
  winner's commit, `apply_namespace_mutation` answers `EEXIST` with the bare
  message "exists", and `ensure_namespace_parents` let that fail the job.
  It now treats EEXIST as the directory existing and re-checks that it is
  one. The test itself was right; the pool made the race reachable. The
  same race exists in production for any two concurrent imports into a
  series or artist directory that does not exist yet.
- `storage_v18/test_edge_node_never_owns_and_its_writes_land_on_owners` --
  **Test defect. Fixed 2026-09-15 (0.43.0).** Surfaced by the first full
  suite run after the two above were fixed (1 in 457), then measured at 2 in
  30 on es-1. It asserted that two observers agree on every key's owner
  after waiting only for active-set sizes and the edge node's flags.
  Single-replica placement (`fallback_score`) is weighted by the capacity
  each observer holds for a node, a handshake carries the peer's NodeInfo
  as of connect time, and a node connects before its storage has reported
  a capacity -- so one observer can hold the other at capacity 0 (weight 1
  against its own 64 MB) until the first gossip round and claim nearly
  every key. The test now waits for both observers to hold the same
  hosting set with the same, non-zero capacities, which is the precondition
  the agreement actually depends on, and logs both counts when they still
  disagree so the next such failure says what it saw.
- `rpc_cluster/test_three_node_cluster` -- **Product defect (transport
  deadlock). Fixed 2026-09-15 (0.43.0).** Surfaced by the next full run
  (1 in 457), measured at 2 in 20 on es-1 serial and parallel alike, as a
  120 s hang. Caught with gdb attached to the hung case: the test thread was
  in `FileSystem::mkdir` waiting for `MetadataManager::mutation_mutex_`;
  the maintenance thread held it inside `repair_once` -> `read_group` ->
  `accept_metadata_commit` -> `announce_metadata_generation` ->
  `RpcClient::broadcast` -> `PeerConnection::notify`, blocked on
  `future.get()` for a frame queued to the node the test had just stopped.
  The writer loop exits when the reader marks the connection broken, and
  on that exit it left its queue behind: only `close()` drained the queue
  and failed the promises, and a peer that simply went away never called
  it. A notify queued in that window waited forever, under the mutation
  mutex, and the node could never publish metadata again. Both writer
  loops (outbound `PeerConnection` and accepted `Session`) now abandon
  their queue -- every waiter released with an error -- on every exit, and
  `notify()` stops waiting the moment the connection is unusable and never
  waits more than five seconds. The same hang was reachable in production
  whenever a peer restarted while a metadata acceptance was being
  announced to it.
  The case had a second, unrelated failure mode (2 in 20 on es-1, a
  `REQUIRE` on the persistent cache filling): a **test defect**. The
  failover read a few lines earlier queues an opportunistic promotion of
  the object back into node 2's store; that write is asynchronous, landed
  after the test's `remove`, and the fetch meant to populate the cache was
  then served locally. The test now removes and fetches until the fetch has
  to go remote. `enqueue_fetched` and the local writer also log every
  dropped opportunistic write at debug now, so the next "cache did not
  fill" says why.

**4. Then sweep the rest** with the tool from step 1, on gbni-1 (the suite's
slowest realistic host), and work down by measured rate.

**5. Close the loop.** Delete the "three load-dependent test flakes" item from
`ACTIVE.md` when its cases are classified and fixed -- not by re-wording it.

## Non-goals

- Retrying a failed case automatically, or any other mechanism whose effect is
  to make a red run look green. The point is to be able to believe the suite.
- Rewriting the runner's scheduler. Process isolation and the port-block
  scheme work; the gap is measurement and per-case verdicts.

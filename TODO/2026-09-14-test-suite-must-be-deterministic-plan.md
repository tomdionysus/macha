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

**One product-side observation, characterised but not explained.** The same
test's GC assertion (superseded catalogue artwork reclaimed on both nodes
within 12 s) missed twice in ~300 reps, and the instrumentation says it is
not a slow sweep: *all four* objects remained, *all on s2 only* -- the node
that wrote them -- while s1 had reclaimed every one, with both catalogues
converged at the same generation and both agreeing that one artwork is live.
A normal pass reclaims in well under a second (0 of 360 reps exceeded 1 s).
So the writer reclaimed nothing while its peer reclaimed everything.
Candidate mechanisms not yet separated: s2 is the busy writer, so its
`gc_quiescent_until` is pushed forward by its own service events, and its
retention claims for freshly published artwork release only once a later
catalogue root proves them unreferenced. The assertion now classifies its own
failure -- it retries for a bounded 30 s and reports `reclaimed_eventually`
and `total_ms` alongside the remaining object ids and their node -- so the
next occurrence says whether this is slow or stuck without another
investigation from scratch. **Next step:** reproduce with `--repeat` and
`MACHA_TEST_LOG_LEVEL=DEBUG`, and read s2's maintenance/GC decisions.

**A second product-side finding, from the same method.**
`filesystem_fuse/test_fuse_publication_quanta_are_fair_and_byte_bounded`
failed three assertions at once *after* `wait_for_idle(30s)` returned true
(1 in 2,646 case-runs). `FuseFrontend::wait_for_idle` composes its verdict
from three independently-sampled fields, so a publication between "dequeued"
and "active" is invisible to it. Test-only API today, but it is the
quiescence primitive much of the FUSE suite waits on, which makes it a
candidate common cause for other cases in that suite. P1 in `ACTIVE.md`.

**Still open.** The two aarch64 cases below (no Pi reachable from here
this session), the artwork GC leak, and the `wait_for_idle` race.

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
  -- 4-in-10 on both Pis in isolation. Fails at its *setup* assertion,
  `REQUIRE(accepted_heads().size() == 2)`. Either the node reconciles the two
  sibling heads before the test looks (a test defect: it is racing the very
  thing it measures) or a head is being dropped (a product defect). Instrument
  `accepted_heads()` across the gap; both answers are cheap at that rate.
- `hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
  -- 8-in-10 on gbni-1, 4-in-10 on es-1, in isolation, and *not* a 0.40.0
  regression. Every failure logs one job failing `ingest failed id=…: exists`,
  which looks like two workers racing over a destination path;
  `ingest.max_concurrent_jobs` and its claimed-set ownership shipped together
  in 0.37.0. Read that first, not the harness deadline.

**4. Then sweep the rest** with the tool from step 1, on gbni-1 (the suite's
slowest realistic host), and work down by measured rate.

**5. Close the loop.** Delete the "three load-dependent test flakes" item from
`ACTIVE.md` when its cases are classified and fixed -- not by re-wording it.

## Non-goals

- Retrying a failed case automatically, or any other mechanism whose effect is
  to make a red run look green. The point is to be able to believe the suite.
- Rewriting the runner's scheduler. Process isolation and the port-block
  scheme work; the gap is measurement and per-case verdicts.

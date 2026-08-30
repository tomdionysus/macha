# Phase 5 journal and convergence Status UAT

Date: 2026-08-30

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope

Validate the deployed journal/convergence Status diagnostics with a bounded
live namespace burst. This UAT did not pause a replica, run the automated test
suite, or exercise physical DATA garbage collection.

The isolated fixture was
`/.macha-uat-phase5-counters-20260830-2215`: one root directory and 64 child
directories, followed by removal of those same 65 directories.

## Baseline

All three nodes were healthy and writable at metadata generation 1433, with
three metadata replicas online. Metadata executor active/pending job counts and
pending bytes were zero on every node. Convergence was fully drained on every
node: requested and completed epochs were equal and `scheduled` was false.

All filesystem failure, timeout, namespace, and operation-journal counters were
zero after the deployment restart.

## Workload and publication result

- Creating 65 directories completed in less than 0.23 seconds and advanced the
  cluster from generation 1433 to 1436: 65 operations in 3 publications.
- Removing the same 65 directories completed in less than 0.29 seconds and
  advanced the cluster from generation 1436 to 1439: another 65 operations in
  3 publications.
- The full burst therefore admitted, batched, published, and confirmed exactly
  130 operations in 6 publications, or 21.7 operations per publication.
- `namespace_publication_attempts` and `namespace_publication_batches` both
  ended at 6. There were no retries disguised as successful batching.

The exact fixture was permanently removed. A standalone management API lookup
after drain returned `filesystem_error: not found`.

## Journal accounting

After the create half, Status reported 260 appended records in 136 append
batches/durability barriers. After the delete half, the totals were exactly 520
records and 272 batches/barriers.

The accounting is internally exact for both halves:

- each of 65 operations contributes two individually durable admission groups
  (inode descriptor mutation and namespace operation), producing 130 admission
  barriers;
- each of 3 publications contributes one grouped `published` barrier and one
  grouped `done` barrier, producing 6 completion-path barriers;
- 130 admission barriers plus 6 grouped completion barriers equals the observed
  136 barriers per half;
- the two admission records plus `published` and `done` records produce four
  records per operation, matching 260 records per half.

This demonstrates that publication/completion work is batched while live FUSE
admission deliberately retains its current per-operation durability boundary.
The latter is now quantified and can be considered separately if concurrent
admission group commit is ever designed; acknowledgements must not precede
durability.

## Convergence accounting

Every node received 23 convergence events over the complete create/delete
burst. The mounted local node completed 8 convergence runs, while each remote
node completed 12. This is bounded relative to the event stream and reached the
same final generation 1439 everywhere.

At both the create checkpoint and final checkpoint, every node reported:

- `requested_epoch == completed_epoch`;
- `runs_scheduled == runs_completed`;
- `scheduled: false`;
- zero active/pending metadata jobs and zero pending bytes;
- zero rejected jobs, backend failures, and timed-out FUSE requests.

## Idle and responsiveness

Fifteen seconds after removal, self-card process CPU was approximately:

- local (`10.44.1.200`): 0.205%;
- node 50 (`10.44.1.50`): 0.124%;
- node 51 (`10.44.1.51`): 0.066%.

RSS was approximately 120.6 MB, 92.8 MB, and 97.0 MB respectively. Direct
sequential Status latency was 1.1 ms locally, 9.3 ms on node 50, and 12.1 ms on
node 51.

One management request issued immediately before the delayed sequential Status
set received an instantaneous connection refusal. An immediate standalone retry
succeeded, while all following Status calls succeeded and showed continuous
healthy, writable, fully drained state. This matches the previously recorded
adjacent/parallel curl observation artifact and provides no evidence of API or
cluster state loss.

## Result

**PASS.** The deployed counters are coherent, the live namespace workload is
batched at the metadata-publication and journal-completion layers, convergence
is event driven and bounded, control/status access remains responsive, and all
three nodes return to quiet idle after drain.

This closes the Phase 5 live counter comparison. Physical-object GC, repeated
burst RSS ceiling, remote disk-telemetry correctness, and possible future live
admission group commit remain separate work.

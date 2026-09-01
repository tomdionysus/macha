# Manual causal metadata repair

Date: 2026-09-01

## Root cause

The live replicas all held two valid accepted heads:

- generation 1552: `1560d590…d97045`;
- generation 1516: `ee47a616…6fd1ef`.

Local history compaction had independently re-rooted the newer branch after a
generation/stability observation which did not durably prove exact accepted-head
identity across every participant. All replicas consequently lost the common
ancestor needed for ordinary three-way reconciliation. This failure predated
the 0.22.0 deployment; node 51 logged it from at least 2026-08-31 19:25.

Inspection proved the 1552 snapshot strictly dominated the 1516 durable mutation
clock. It had the same catalogue root and garbage count, contained every older
namespace path, added 12 paths and changed 8 paths through later causally-covered
mutations. Using its state therefore preserved deletion and update semantics;
generation alone was not used to choose it.

## Repair

`macha-metadata-repair` supports read-only inspection by default and explicit
offline operations:

- `--plan-causal-merge` performs a dry run;
- `--stage-causal-merge` durably stores but does not accept the record;
- `--accept-causal-merge` accepts an already-staged record with explicitly named
  durable witnesses.

The tool requires exactly two non-mergeable accepted heads and strict one-way
causal dominance. It refuses concurrent or equal clocks, ordinary mergeable
histories, insufficient witnesses and acceptance before local staging. It is
not called by the server and cannot trigger automatic rootless repair.

Offline backups were created as `metadata.pre-manual-repair-20260831` beside the
active metadata directory on nodes 200, 50 and 51. Every dry run produced the
same generation-1553 record `a69efd66…8dce92`. That record uses generation 1552
as its state and primary parent and retains generation 1516 as its additional
merge parent. It was staged on all three replicas before being accepted with
nodes 50 and 51 as the two durable witnesses.

## Prevention and verification

Automatic metadata-history compaction is disabled. Re-enabling it requires a
new durable protocol proving exact accepted-head identity on every known
participant; local generation/stability is insufficient.

Local verification passed 29/29 storage/metadata tests, 234/234 core tests and
3/3 runtime tests. After restart, queued FUSE operations advanced the repaired
branch to generation 1556 and all three nodes became healthy/writable. Overnight,
later mergeable branches reconciled normally at generations 1568 and 1569 with
zero conflicts. On 2026-09-01 each node's own API reported healthy, writable,
three nodes online and generation 1569. Aggregate peer-generation rows remained
temporarily stale, matching the separate telemetry-aggregation TODO.

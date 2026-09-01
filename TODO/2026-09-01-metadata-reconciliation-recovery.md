# Metadata reconciliation recovery — 2026-09-01

## Incident

The four-node cluster held valid generation-2128 library branches but could not
complete convergence. Every replica reported `local metadata commit store
failed`; cluster metadata remained unavailable after restart. This failure began
at 13:09, before the 0.22.1 deployment at 15:01, so that deployment exposed an
existing deterministic failure rather than creating transient divergence.

Each current checkpoint is about 5.7 MB, while the append-only histories are
roughly 2.17–2.19 GB. Restart therefore read gigabytes of historical data and
node 51 retained about 1.9 GB RSS. Poor connectivity to node 200 additionally
stalled membership and metadata RPCs, but was not the cause of the rejected
local commit.

## Root cause

Reconciliation emits the union of garbage/tombstone records in canonical
ObjectId order. Historical snapshots can retain append order. The compact delta
diff compared garbage by identity and ignored ordering, even though the delta
grammar can only erase, replace in place, or append. Applying the claimed delta
therefore produced semantically equal state with different canonical bytes. The
replica's immutable-record validation correctly rejected it.

Remote replica publication already retried a rejected compact body as a full
record. Local publication returned the rejection immediately, then failed its
mandatory-local-replica invariant. No accepted merge could be created.

## 0.22.2 correction

- `metadata_delta()` now proves the target garbage ordering is representable by
  retained-in-place entries followed by newly appended entries. Otherwise it
  selects the ordinary full-record representation.
- A local compact-body rejection receives one bounded full-record attempt,
  matching remote behaviour. It is logged and cannot become a retry loop.
- Ordinary mutations which preserve append order remain compact. The known cold
  recovery cost is one approximately 5.7 MB full merge record, not the complete
  2.2 GB history.

## Verification

- [x] Regression: an append-ordered parent and canonically reordered target do
  not produce a compact delta.
- [x] Regression: a deliberately invalid exact delta is rejected locally, then
  stored and accepted through the full immutable record.
- [x] Full local core/runtime suites pass: 250/250 core tests and 3/3 runtime
  dependency tests on macOS.
- [x] Identical Linux binaries deployed to GBNI-1, GBNI-2 and ES-1; node 200 was
  rebuilt and restarted from the same tested source. Linux SHA-256:
  `1fc6c8473a64525da48d6fead630780615f525f22ae136dd2ff9bdf5731144d0`.
- [x] All live replicas converged to one writable lineage without library
  reset or manual metadata replacement.
- [x] Every catalogue API returned ready with 331 items and 374 artwork objects.
  Playback status remained available; no playback session was created as part
  of this metadata-recovery UAT.
- [x] Linux services remained active with zero restarts. Node 51 fell from about
  1.9 GB RSS during cold recovery to about 249 MB, later settling around 446 MB
  while queued work ran. GBNI-1 temporarily reached 2.5 GB while a recovered
  FUSE publication held metadata mutation ownership, then fell to about 1.4 GB
  after that publication completed and its convergence pass caught up.

## Remaining observation

The histories remain approximately 2.18–2.21 GB because safe cluster-acknowledged
history compaction is deliberately still a separate P0 task. The correction did
not expand the history by gigabytes: recovery added one bounded full snapshot
plus ordinary accepted records, moving from 1,006 records at generation 2128 to
about 1,025 while queued work advanced beyond generation 2147.

A brief stack capture on GBNI-1 found the last long convergence delay was not
the repaired merge. A recovered FUSE `WriteHandle::commit()` held
`mutation_mutex_` inside `retain_metadata_publication()` while
`DistributedStore::retain_data()` performed remote `has_on` checks. This is now
an explicit active P0 follow-up: batch/bound the checks and keep them from
delaying metadata convergence or critical control traffic.

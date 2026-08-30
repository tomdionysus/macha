# Active tasks and concepts to explore

Last updated: 2026-08-30

This is the working backlog for the current session. Add new work here. When an item is implemented and its stated verification is complete, remove it from this file and add a dated entry with evidence to `COMPLETED.md`.

The existing documents in this directory remain the detailed plans, checkpoints, and UAT records. This file is only the current index.

## Phase 1 materialization follow-up

- [ ] Avoid encoding every intermediate snapshot solely to traverse a delta chain; encode only when commit-identity validation requires it.
- [ ] Move reconstruction, decoding, encoding, and hashing outside the main `MetadataReplica` mutex.
- [ ] Install calculated materializations through a short lock/CAS-style validation step.
- [ ] Deduplicate concurrent requests for the same uncached hash so waiters share one computation.
- [ ] Add deterministic concurrent-request coverage proving one materialization.
- [ ] Add explicit corrupt-delta coverage proving the cache cannot make invalid history valid.
- [ ] Complete the edge-case audit for merge commits, same-generation siblings, recovery-required state, policy transitions, corrupt history, and eviction.

## Phase 2 namespace batching follow-up

- [ ] Design a durable batch identity that permits dependency chains such as create/rename/unlink to share a publication without weakening restart proof.
- [ ] Test mixed create/rename/unlink dependencies within a batch and across batch boundaries after that identity exists.
- [ ] Decide whether each published prefix member needs an explicit in-memory association with its accepted commit hash/generation.
- [ ] Preserve rename as a safe singleton boundary until the durable mixed-operation design and crash matrix are complete.

## Diagnostics and operational proof still needed

- [ ] Investigate status disk-usage telemetry alternating between plausible values and zero for connected remote nodes. Capture the raw `/api/v1/status` responses across refreshes, determine whether the API emits transient zero/unavailable storage and cache values or the client incorrectly selects/renders stale peer records, then fix the responsible layer and add regression coverage that distinguishes unavailable telemetry from genuine zero usage.
- [ ] Record a reproducible local benchmark recipe without default-suite timing thresholds.
- [ ] Measure a larger or repeated namespace burst to establish whether materialization-cache RSS reaches a stable ceiling.
- [ ] Recheck the intermittent suite-load timeout in `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`: the first Phase 5 full run timed out at 10 seconds, its immediate isolated run passed in 1.024 seconds, and the second full run passed it in 1.147 seconds. Preserve diagnostics on any recurrence; do not dismiss a future failure as timing noise.
- [ ] Deploy the journal/convergence Status build and repeat a bounded namespace UAT to capture admission/publication journal-barrier deltas and convergence events/runs. The API exposure and deterministic tests are complete; this live delta is the remaining Phase 5 counter comparison.
- [ ] Confirm physical-object GC remains deliberately rate-limited and operationally distinct from namespace publication completion.
- [ ] Update `docs/durability.md`, `docs/metadata.md`, and `docs/operations.md` with the final batching, recovery, convergence, and RPC-isolation semantics.

## Separate known issue

- [ ] Diagnose and correct faulty torrent/ingest behaviour. Pausing the torrent removed the local node's residual CPU during Phase 3 idle UAT. Treat this as a separate subsystem investigation so it does not obscure metadata/convergence measurements.

## Deferred architectural concepts

- [ ] Consider delta-native commit identity based on parent identity, canonical delta, and a state-tree root instead of a fully serialized namespace payload.
- [ ] Consider a persistent or copy-on-write namespace tree with incremental subtree hashing.
- [ ] If either protocol-level design proceeds, define rolling-upgrade negotiation, checkpoint/anchor migration, and independent corruption validation first.

## Additional investigation

- [ ] Investigate whether ingest from the spool has performance problems unrelated to the namespace batching and convergence work currently in progress.
- [ ] Replace the spool's hard-coded 16 GiB capacity limit with a documented, validated configuration setting and a safe default preserving current behaviour.
- [ ] Design occupancy-aware spool admission backpressure that also tracks sustained publication/drain throughput. Admission should be fast while capacity is readily available, progressively slow as occupancy approaches the configured limit, and never admit data beyond the durable capacity bound.
- [ ] Define the backpressure policy precisely, including measurement windows, high/low watermarks or hysteresis, minimum progress, fairness between writers, restart behaviour, and behaviour when publishing stalls or the cluster becomes unwritable.
- [ ] Ensure throttling blocks or paces the ingesting FUSE requests without polling, busy-waiting, unbounded buffering, or consuming critical RPC/control threads.
- [ ] Add deterministic tests for configurable capacity, occupancy accounting, progressive throttling, recovery as the spool drains, a completely stalled publisher, multiple concurrent writers, restart near capacity, and enforcement of the hard upper bound.
- [ ] Run an rsync-style mounted-FUSE UAT: initial writes should run quickly, throughput should progressively approach sustainable publication speed as the spool fills, occupancy should remain bounded, and write speed should recover cleanly after the backlog drains.

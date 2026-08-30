# Phase 1 materialization closeout

Date: 2026-08-30

Parent plan: `TODO/namespace-publication-and-metadata-efficiency.md`

## Scope

Close the remaining bounded-materialization follow-up:

- determine whether historical reconstruction still performs avoidable
  intermediate snapshot encoding;
- prove that a cached ancestor cannot make a corrupt delta valid; and
- complete the edge-case audit across branches, siblings, recovery, policy,
  corruption, and eviction.

## Encoding audit

No production-code removal is correct under the current metadata identity.

Each stored delta history entry names the hash of its exact successor
`MetadataRecord`. That hash commits to generation, primary-parent hash, and the
successor's canonical full-snapshot bytes. Reconstruction must therefore apply
each delta, reproduce the historical snapshot encoding selected by the delta
version, and validate the claimed successor hash before using that successor as
the parent of the next edge.

Skipping an intermediate encode/hash would trust an unauthenticated history
edge and allow corruption before the final target to influence later state.
Legacy DLT1/DLT2/DLT3 entries additionally require their exact SM7/SM8/SM9
encoding for compatibility. The existing encodes are identity validation, not
mere traversal overhead.

Avoiding this work requires the deferred protocol redesign in which a commit
can authenticate a canonical delta plus parent identity and a state-tree root.
It cannot be safely achieved as a local cache optimization.

## Corrupt-delta regression

Added
`storage_metadata/test_metadata_materialization_cache_never_validates_corrupt_delta`.
The test:

1. stores and materializes a valid parent so reconstruction begins from a cache
   hit;
2. supplies a well-formed applicable delta which produces the wrong successor
   under a valid child's claimed hash;
3. proves import fails, no history entry or child materialization is installed,
   and acceptance fails;
4. imports the correct delta and proves its canonical payload/materialization;
5. retries the corrupt duplicate under the now-cached valid child identity and
   proves it is still rejected without displacing or poisoning the valid cache
   entry.

## Edge-case audit

Existing deterministic coverage plus the new corruption test covers:

- merge histories and compacted direct predecessors;
- divergent rename conflicts and resolved-conflict non-resurrection;
- same-generation sibling persistence and live service reconciliation;
- recovery-cache seeds and checkpoints never becoming accepted authority;
- protocol-20 policy/governance encoding and rejection of legacy authority
  mutation paths;
- bounded eviction followed by byte-identical reconstruction;
- corrupt delta rejection both before and after a valid target is cached.

## Verification

- new corrupt-delta regression: 1/1 passed in 9 ms;
- complete `storage_metadata` group: 28/28 passed;
- `rpc_cluster/test_service_same_generation_sibling_notice_triggers_reconciliation`:
  1/1 passed in 771 ms.

Repository-wide verification passed:

- complete default suite: 205/205;
- runtime dependency suite: 3/3;
- both `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
  and `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed
  explicitly within the complete run.

## Combined UAT result

The combined deployment UAT filled every materialization cache to its 64-entry
bound. Across 3,960 new materialization requests there were no new misses,
reconstructions, or applied historical deltas; bounded evictions occurred on
all nodes. Publication, convergence, queues, and idle CPU also passed.

The RSS-ceiling question remains open because Status `runtime.rss_bytes` was
found to use lifetime-peak `ru_maxrss`, not current resident memory, and only
one local round followed the cache first reaching its bound. Correct metric
semantics and repeated post-cap measurement remain in `ACTIVE.md`.

Evidence: [Combined Status availability and repeated-burst UAT](2026-08-30-combined-status-rss-uat.md)

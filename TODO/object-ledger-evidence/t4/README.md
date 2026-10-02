# T4: the metadata contract

## T4a: the survey and the contracts (2026-10-02)

The survey is the compiler's: every component that held a `MetadataManager`
except its owner (`Service`) was given the contract's type instead, and
each call the contract did not offer failed to compile. The contract grew
by exactly those calls, each under its spec name and implemented by
`MetadataManager` as a one-line call of the method the site called before,
so no call site changed what it calls.

- `src/contract/metadata_view.hpp`: `MetadataView` (the view: reads,
  commits, status, diagnostics) and `MetadataMaintenance` (the metadata
  component's upkeep of its replicas, taken only by the maintenance pass).
  `MetadataSnapshotView`, `MetadataAvailability`, `MetadataClusterStatus`,
  `MetadataMutationIdentity` and `MetadataMutationTiming` moved into it from
  `metadata_manager.hpp`.
- Holders now on the contract: `FileSystem`, `CatalogueManager`,
  `TorrentCoordinator`, `ManageApi`, `StatusApi` (`MetadataView`) and
  `Maintenance` (`MetadataView` and `MetadataMaintenance`, both declared
  dependencies in the root). `Service` owns the manager and supplies both.

| contract operation | was | direct sites outside `src/metadata/` |
|---|---|---|
| `current()` | `available_snapshot_view()` | status_api 2, catalogue 6, filesystem 3, maintenance 1, torrent_coordinator 2 |
| `converged()` | `snapshot_view()` | manage_api 1, filesystem 5 (one was `snapshot()`: `*converged().snapshot`), torrent_coordinator 1 |
| `release_head()` | `retention_release_view()` | maintenance 2 |
| `current_generation()`, `current_namespace_revision()` | `available_snapshot_generation()`, `available_namespace_revision()` | filesystem.hpp 1 each (not in the spec's table) |
| `record()` | `read_record()` | catalogue 4 (not in the spec's table: the catalogue's cold start and record reads) |
| `status()` | `cluster_status()` | status_api 1, maintenance 1, torrent_coordinator 1 |
| `mutate_delta()` | same | manage_api 1, catalogue 2, filesystem 2, maintenance 1, torrent_coordinator 6 |
| `mutate()`, `resolve_conflict()` | same | none outside the owner (the spec estimated ~20 for mutate: they are all `mutate_delta`) |
| `conflicts_superseded()`, `conflicts_resolved()`, `mutation_timing()` | same | status_api 1 each (diagnostics; not in the spec's table) |
| `MetadataMaintenance::repair_step()` | `repair_once()` | maintenance 1 |
| `note_replica_validation()`, `repair_unreconstructable_heads()`, `attempt_history_checkpoint()` | same | maintenance 3, 1, 1 (not in the spec's table) |

The spec's text-search estimates (28 `current`, 17 `converged`, 19
`status`) counted calls on `FileSystem`'s and the catalogue's own wrappers
of the same names, which are inside the holders and now reach metadata
through the contract. The counts above are calls made directly on a
metadata reference, by receiver name; that none is missed is the
compiler's guarantee, not the count's.

- Suites 700/700, 17/17; traces 240/240 over 20 runs.

## T4b: the catalogue's hidden repair split out (2026-10-02)

- `CatalogueManager::maintenance_objects()` ran the catalogue's repair
  inside what reads as a read (spec A4's known case). It is now three
  steps the pass calls in order at the same point: `maintenance_head()`
  (the metadata head the inventory is taken against, captured before the
  repair as before, because the read's completeness compares with it),
  `maintenance_repair()` (the repair; false when it failed), and
  `maintenance_objects(head, repaired)`. The builder's `inventory` takes
  the head and the outcome; it repairs and commits nothing.
- The audit's other finding: the read still fetches catalogue objects it
  lacks into the control store (`ensure_control_local`), declared on it and
  recorded in A4.
- Traces identical: 1200/1200 over `--repeat 100`, no fixture changed.
- `tests/test_catalogue_maintenance.cpp`: the catalogue against a fake
  `MetadataView` (possible now that it takes the contract): a repair that
  cannot read metadata reports failure; the read is complete only when
  the repair succeeded and the head is current.
- Mutation (`build/claude-t4b-mutate*.py`): 6, then 2 against the new
  test. Three killed by the trace fixtures and invariants; two (the
  repair's outcome) killed only by the new test; one survives by design:
  **skipping the inventory-time repair changes nothing any test sees,
  because the pass already runs the catalogue's repair in its own
  `catalogue-repair` stage earlier in every pass**
  (`src/service/maintenance.cpp`, the `refresh_needed()` block). The
  inventory-time call is a second repair per pass; each may commit a
  catalogue-root reconciliation. Removing it is a behaviour change for the
  operator (ACTIVE, the conflict loop).
- Suites 702/702, 17/17.

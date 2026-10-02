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

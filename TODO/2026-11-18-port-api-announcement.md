# The port and the API: what 0.95.0 keeps, what waits

The maintainer, 2026-11-18: "We shouldn't change the API. Map them with comments", and for the earlier rulings: "Revert to
main, but leave code comments. The intent is that we use dannan/dagda native primitives, but I want to see
functional fidelity in Macha first." So 0.95.0 sends what 0.94.2 sent; nothing is announced for it.

## Mapped back to 0.94.2 (each with a comment in the code naming the intent)

- **Status `message_timings` keys**: 0.94.2's names by number (`status_message_type`, `src/cluster/dannan_bridge.cpp`).
  The store node's types kept their numbers; Dagda's own types are counted under the type whose work they took over,
  their timings added: `durability_covered` under `object_durability_barrier`, `remove_objects` under `delete_object`,
  `offer_objects` under `have_objects`, `get_objects` under `get_object`, and their replies likewise. Intent:
  `dagda.<name>` keys.
- **Artwork read**: no deadline of its own, as 0.94.2; a store error is 503 `catalogue_unavailable` with the store's own
  message (`src/catalogue/catalogue.cpp`). Intent: a 30 s bounded read answering 503 on timeout.
- **Status `diagnostics.repair`**: 0.94.2's keys only, shown once services are ready (`src/api/status_api.cpp`). Intent:
  `offer_bytes` and `in_flight` (the maintainer asked for them 2026-11-16), and the section from local recovery.
- **Status `diagnostics.data_resources`**: 0.94.2's keys; no `available` key. Intent: `available`, false until the
  store node's store is open.
- **Ingest**: a job blocks as `storage_unavailable` on the store's `StorageUnavailable` again, as 0.94.2
  (`src/acquisition/ingest.cpp`).

## Same shape, values that still differ (for the maintainer; not changed)

- `startup.error`: a store plane's failure carries Dagda's text (the code, `recovery_failed`, is unchanged), and
  `startup.phase` may read `failed` while another plane is still `recovering`.
- `retained_memory.owners.object_payload`: Dagda's accounting, which charges every read from admission (local and
  cache reads too), so it reads higher and busier under viewing.
- `data_resources` figures read zero until the store node's store opens; 0.94.2's own arbiter had them from start.
- Files and media availability: the `extents_unavailable` / `extents_unknown` split is Dagda's survey's; 0.94.2 kept
  what an unanswering peer held as unavailable until it returned. Unverified on the cluster.
- `startup.data_storage`, `control_storage`, `cache`, `retention` and `data_storage_degraded` are marked by the store
  node's readiness; the words are the same, when each flips may differ. Unverified in a live start.

Unchanged and checked: `frame_timings` keys, node id format, Status thread names, `canonical_connections`, the
`data_store` keys, health, and the manage, users, playback, acquisition and files routes and their codes. Store figure
age is unchanged (refresh every heartbeat, as 0.94.2's storage server).

# Port API announcement: what the HTTP API sends, main (0.94.2) against port/dannan-dagda

Draft for Core and every client session, before the deploy. Compared `main...HEAD` (HEAD `a9556c2`). Lines are
HEAD's. No route was added or removed, and no file under `src/api/` changed except `status_api.cpp`/`.hpp`; every
change is a field value, a section's availability, an error's text, or one new status code on one route.

## The known list (TODO/2026-11-14-port-rewrites.md, "Before the flag day")

### 1. Store `message_timings` keys
- **Route:** `GET /api/v1/status`, `diagnostics.rpc_server.message_timings` (`status_api.cpp:1008-1011`).
- **Main:** one key per message type Macha registered, by Macha's name (`get_object`, `have_objects`, `trie_diff`, ...).
- **0.95.0:** every type is keyed by the name its registrar gave it (`registered_type_name`, `dannan_bridge.cpp`).
  The store's types, registered by Dagda, become **`dagda.<name>`**: `get_object` is now `dagda.get_object`,
  `have_objects` is `dagda.have_objects`, and so on for all 28 (`dagda/store/messages.cpp`), including Dagda's
  new ones (`dagda.durability_covered`, `dagda.remove_objects`, `dagda.offer_objects`, `dagda.get_objects` and
  their replies). Macha's own types keep their names (`get_metadata`, `session_sync`, ...); Dannan's core types keep
  theirs (`ping`, `telemetry`, ...); a number nobody registered appears in decimal. One entry per type, never
  merged. (Before this fix the branch keyed Dagda's new types all as `"unknown"`, one overwriting another.)
- **Client check:** any client reading store message timings by key must read the `dagda.` names; treat the key set
  as open.

### 2. `startup.error` text
- **Routes:** `GET /api/v1/status` `startup.error` / `startup.error_code` / `startup.phase` (`status_api.cpp:834-853`);
  every non-status route before services are ready, `503 startup_failed` with `message` (`service.cpp:249-253`).
- **Main:** Macha's own recovery error text.
- **HEAD:** a store plane's failure is Dagda's text, from the node's readiness (`cluster.cpp:191,194`); the 503's
  message is the exception text from opening the store node (`service.cpp:552-553, 590-595`). `error_code` stays
  `recovery_failed`; the 503 code stays `startup_failed`.
- **Also:** `startup.phase` becomes `"failed"` as soon as a store plane fails, while other planes may still read
  `"recovering"` (`cluster.cpp:189-191`). On main it was set when Macha's recovery ended.
- **Client check:** branch on the codes only, never the text (already the rule); accept `failed` beside recovering planes.

### 3. `retained_memory.owners` values
- **Route:** `GET /api/v1/status`, `diagnostics.retained_memory.owners` (`status_api.cpp:1090-1097`). Keys unchanged
  (the same eleven).
- **Main:** `object_payload` was what Macha's store charged (four sites in `distributed_store.cpp`).
- **HEAD:** `object_payload` is Dagda's owner (`retained_memory.cpp:14-15`): every read holds an extent of memory from
  admission, before the local and cache reads, until its last reader lets go, plus the prompt-copy queue
  (Dagda's `distributed_store.cpp:2264, 2524`). Expect it higher and busier under viewing. The other owners are
  charged as before (`fuse_*`, `publication`, `playback_segment`, `rpc_frame`); `durability`, `metadata`, `catalogue`,
  `media_profile` and `cache` had no charger on main either.
- **Client check:** none for shape; any client threshold or chart on `object_payload` should expect the new range.

### 4. Status figure age
- The figures whose source changed:
  - Node telemetry `storage`/`cache` capacity, used, backends online, cache hits/misses/evictions (Status `nodes[]`,
    cluster totals): on HEAD they are the store server's last advert on this node's own record (`cluster.cpp:147-165`),
    refreshed every heartbeat (`store_node.cpp:94-96`, `refresh_interval = config.heartbeat`), the cadence Macha's
    own storage server refreshed at. Same age bound as before (one heartbeat plus telemetry's spread to a peer);
    the source moved, the age did not.
  - Files and media availability `surveyed_unix_ms` / `surveyed_generation` (`files_api.cpp:61-62, 112`, and the
    catalogue items through `item_availability`): on HEAD `surveyed_unix_ms` is the store node's survey time
    (`availability_service.cpp:249`); on main it was when Macha's own survey ran. Dagda's survey cadence against
    Macha's is unverified.
- **Client check:** compute age from the timestamp sent, never assume a refresh interval.

### 5. Artwork 503
- **Route:** `GET /api/v1/catalogue/artwork/{id}` (`catalogue_api.cpp:918-956`).
- **Main:** the read had no deadline; a stuck read held the request open indefinitely. 404 when not found.
- **HEAD:** the read is bounded at 30 s (`catalogue.cpp:1022, 2530`); past it, **`503 catalogue_unavailable`**,
  message `catalogue DATA read timed out` (`catalogue.cpp:1045-1046`, mapped at `catalogue_api.cpp:955-956`). A store
  error is also 503 `catalogue_unavailable`, as on main, but its message is now prefixed
  `catalogue DATA read failed: ` (`catalogue.cpp:1043`). A missing or unreachable object is still `404 not_found`.
- **Client check:** treat 503 on artwork as "try later" (bounded backoff), not as missing art; do not cache it as 404.

### 6. Diagnostics `data_resources.available` (new)
- **Route:** `GET /api/v1/status`, `diagnostics.data_resources` (`status_api.cpp:1027-1058`).
- **Main:** no `available` key; figures live from process start (Macha's arbiter).
- **HEAD:** new boolean **`available`** (`status_api.cpp:1032`); false, with every figure zero, until the store node is
  given to Status. That happens when local state has recovered (`service.cpp:125-129`), which is after the store
  opens, so the window is all of recovery, not just the store's open. Other keys unchanged; `viewer_*` now carry
  Dagda's `foreground_*` figures (`status_api.cpp:1034, 1037, 1040`).
- **Client check:** read `available` first; never chart zeros while it is false.

### 7. Repair `offer_bytes` and `in_flight` (commit b26fb55)
- **Route:** `GET /api/v1/status`, `diagnostics.repair` (`status_api.cpp:1336-1339`), beside `bytes_transferred`.
- **Main:** absent.
- **HEAD:** **`offer_bytes`**: the repair offers' own bytes, paid by the credit, moving no object; **`in_flight`**:
  bytes of transfers started and not yet landed. Both unsigned integers, present whenever `repair.available` is true.
- **Client check:** additive; display or ignore. Marked provisional in ACTIVE.md (Dagda's repair redesign may change them).

## Found beyond the known list

### 8. `repair` and `prompt_replication` sections available earlier
- **Route:** `GET /api/v1/status`, `diagnostics.repair`, `diagnostics.prompt_replication` (`status_api.cpp:1315-1392`).
- **Main:** `repair.available` true only once services were ready; `prompt_replication` absent until then.
- **HEAD:** both read the store node, given once local state recovers (`service.cpp:125-129`), so they appear before
  services are ready. Keys unchanged; the values (gates, pace, prompt counts) are Dagda's figures.
- **Client check:** none required; a client that assumed `repair.available` implies services ready must not.

### 9. Startup plane readiness from Dagda
- **Route:** `GET /api/v1/status`, `startup.data_storage`, `control_storage`, `cache`, `retention`,
  `data_storage_degraded` (`status_api.cpp:842-846`).
- **HEAD:** marked by the store node on Dannan's readiness under the same names (`cluster.cpp:177-186`,
  Dagda `node.cpp:38-39`); `data_storage_degraded` from the store's advertised `backends_online`. Values are the same
  words; when each flips may differ from main. Unverified in a live startup.

### 10. Availability `extents_unavailable` against `extents_unknown`
- **Routes:** the files resource and media/catalogue availability (`files_api.cpp:47-63`).
- **Main:** a peer that could not answer kept what this node lacked as unavailable until it came back.
- **HEAD:** the split comes from Dagda's survey (`availability_service.cpp:247-268`); Macha's `keep_unavailable` rule is
  gone. Expect the same keys and words, possibly more `unknown` and fewer `unavailable` while a peer is away.
  Unverified on the cluster.

### 11. Ingest job `storage_unavailable` (unverified)
- **Route:** `GET /api/v1/ingest/...` job state.
- **HEAD:** a job blocks as `storage_unavailable` only on `FsStorageUnavailable` (`ingest.cpp:1627`); Macha's store
  `StorageUnavailable` is gone. If a Dagda exception other than "landed nowhere" reaches the job, it now fails rather
  than blocks. Whether any can is unverified.

## Checked, unchanged
`frame_timings` keys (Dannan's `frame_type_name` still says `read-ahead`); node ids (32 lowercase hex, now Dannan's
`to_string`); Status `threads` names (`cluster.cpp:637-657`); `rpc_transport.canonical_connections` (Dannan writes
it now); `data_store` section keys; health (`/api/v1/health`); manage, users, playback, acquisition and files routes
and their codes.

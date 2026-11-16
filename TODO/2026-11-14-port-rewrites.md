# Macha onto Dagda: the rewrites, in order (2026-11-14)

The port as the operator ruled it (2026-11-14): Macha calls `dagda::Context` only, no shims, rewritten where its code
is shaped around its own store, losing nothing. Sources: the callers' map (`store-map-callers/`, each section's
"Rewrites in Macha", row numbers below are that section's) and Dagda's store map. Context v2 is gated green at Dagda
`feature/macha-store-gaps` `3ce0e10` (Objects, Claims, Leases, Heads, Tickets, Control, Activity, healing with both
guards, co-owned cancellation, typed `ReadEnd` and `BarrierLoss`); Devices (admission, device monitor) gates next.
Build against that branch until Dagda tags it; record the commit each Macha step built on.

The cutover is one disk event (S, 8a, 8b together; `2026-11-13-store-cutover.md`). **Staging (operator,
2026-11-14): one branch, gate at the end.** Macha's store and Dagda's node cannot run side by side (same disks and
lock, v18 against v19), so R1 to R7 land on `port/dannan-dagda` as commits that each compile (GCC and clang,
warnings as errors) but the suites run once the whole set is in; then fix forward until Macha's suites pass on
Dagda, then the harness and the migration rehearsal. **Deps now (2026-11-15):** Dannan 0.8.0 (`/home/maintainer/deps/prefix-0.8.0`, `build/pi/deps_080.sh`, `dannan-custody`
installed there) and Dagda `6ea9d04` on `feature/macha-store-gaps` (`prefix-dagda-6ea9d04`,
`build/pi/deps_dagda_6ea9d04.sh`; `Objects::logical_capacity`), compiled by `build/pi/macha_port_compile.sh`, which
drops the CMake cache when the Dagda prefix changes. Moving to Dagda `6bfb5b2` (gated: `WriteOptions::ack_level`, `replicated` a majority, figures in one `dagda/figures` advert read through `store::figures_of`): its prefix is `prefix-dagda-6bfb5b2` (`build/pi/deps_dagda_6bfb5b2.sh`), Macha's four writes name `ack_level`. The deploy's
deps come from Dannan's newest 0.8 patch, 0.8.3 (`00a3a12`; ABI 0.8.0's, custody tool unchanged) and Dagda's tag.

**Owner surface (operator, 2026-11-15, relayed by Dagda):** Macha's composition root, which opens and owns the
node, keeps `dagda::Node`'s owner surface: `open` / `OpenLater` and `open_store`, `stop`, `reconfigure`, and the
maintenance steps Macha's interim loop drives until 8c. Every call Macha's other code makes goes through
`dagda::Context`. The retirement batch codec (`dagda::heads`) and `lease::ClaimedCollection` stay library helpers,
not shims.

## R1. The root (composition and lifecycle) [root section rewrites 1-5, 13, 14; metadata 1]

**Progress:** `f975a47` pins Dannan 0.7.0 and Dagda 0.2.0 (the tree compiled unchanged); `05ca9a6` the root builds
`dagda::Node(OpenLater)` over `node_.node()` in `Service`'s constructor, `open_store()` first in
`initialise_services`, `Node::stop()` after `services_->stop()`; `03e10a9` one state lock and one identity: Macha's
`Service::state_lock_` is gone (the Dannan node takes `state_path`'s lock as its first member, `node.cpp:440-444` at
0.7.0; Dagda's `.macha.lock` follows in `open_store()`), `NodeRuntime` takes `ClusterKeys` and builds `NodeIdentity`
from the Dannan node's id and epoch, `TranscodeRateBook` opens after the lock; `cd2c278` the store's readiness planes are the store node's (Macha declares
only `metadata`; `readiness()` reads the four from the node); `88d3d07` HTTP notes viewer presence on
`Context::activity()` (and Macha's own clock while its store remains), `NodeServices` takes `dagda::Context&` and
passes it as `SubsystemContext::store`, `BareNode` builds and opens its store node as `Service` does. Each compiles on build-3 with
`build/pi/macha_port_compile.sh`; nothing runs while `LocalState` still opens Macha's store. **Rest of R1, landing with R2 to R6** (they matter only once the consumers leave Macha's): one `StartupProgress`,
Dannan's events for `NodeEvents`, `NodeResources` dropping `data` and `activity` for Dagda's. **Next: R2**, the
filesystem and FUSE onto Context (`NodeServices::context_` is there to pass). The path-built `NodeIdentity` constructor and `StoreBench` go in R7.

- `LocalState` goes. `Service` builds `dagda::Node(context, options, OpenLater)` after `NodeRuntime` and before
  `node_.start()` (registrations before the Dannan node starts); `open_store()` on the startup thread; `Node::stop()`
  after `services_->stop()`, before `node_.stop()`. Macha keeps only its `MetadataReplica` (heads off until 8c) and
  declares and marks its own `metadata` plane.
- **One state lock, the node's:** Macha's `Service::state_lock_` goes (two `flock`s on `<state>/.macha.lock` conflict
  in one process, verified); every state read and write orders after `Node(OpenLater)`'s lock; name anything that
  cannot (Dagda agreed).
- Options: `store_node.cpp` (`a95e9cc`) maps every key into `NodeOptions`; add the root section's keys (A5, A9, A17,
  A25, A28, A29), `NodeOptions::startup_stage` to Macha's stage names, custody already in `NodeConfig` (`9770427`).
- Readiness from Dannan's planes (Macha keeps `metadata`, completion and its error text); startup progress through
  one `StartupProgress`; `NodeEvents` becomes Dannan's; `NodeResources` drops `data` and `activity` for Dagda's; HTTP
  notes viewer presence into `Context::activity()` (law 2, `docs/principles-and-laws.md:92-98`); `SubsystemContext`
  carries the Context.
- Test seams (`Service::store()`, `repair_diagnostics()`, `local_state()`) re-pointed.

## R2. The filesystem and FUSE reads and publication [filesystem 1-8; FUSE 1-5, 7]

**R2 design, read 2026-11-15 against Dagda `caec517`:** Context offers `Objects::read(id, ReadOptions)` blocking on
the caller's thread with no deadline of its own; `ReadOptions{frame, deadline (relative ms, 0 none), stripe,
shared_ptr<atomic_bool> cancelled}`; `ReadResult{ObjectData object, source, served_by, answer, ReadEnd end}` with
`ReadEnd{served, not_found, cancelled, timed_out, unavailable}`; Dagda's `ObjectData` is
`shared_ptr<const ObjectBuffer{Bytes bytes; MemoryHold memory}>`, the same shape as Macha's. Order:
1. `FileSystem` takes `dagda::Context&` (built in `NodeServices`, `tests/test_support.hpp:702`,
   `tests/test_hydration_catalogue.cpp:4429`); `open_read` hands it to `ReadHandle`.
2. `ReadHandle` (`filesystem.cpp:84-197`) holds the Context and caches Dagda's `ObjectData`; `extent()` calls
   `Objects::read` with the remaining time from the caller's deadline and maps `ReadEnd` to ECANCELED, ETIMEDOUT,
   ENOENT/EIO (no own clock check); activity notes go to `Context::activity().note(frame, bytes)`.
3. **The cancel token cascade (requirement 5):** `ReadHandle::read` takes `shared_ptr<atomic_bool>` instead of
   `atomic_bool*`, so every caller owns one: FUSE `read_impl` (the broker's `cancelled` is already a
   `shared_ptr` in `dispatch`, `fuse_frontend.hpp:567`; pass it into the task instead of the reference), the media
   inputs' `InputIoState::cancelled` (`media_engine.cpp:184`), playback (`playback.cpp:252, 272`),
   `media_information.cpp:27`, `web_api.cpp:89`, `media_catalogue.cpp:90`, `http.hpp:51, 170`.
   Owners found: `MediaSource::cancelled` is already a `shared_ptr` (probes pass `.get()`,
   `media_engine.cpp:1769, 1848, 1935`: pass the pointer itself); playback (`:1357`) and subtitles (`:2074`) pass the
   address of a local `atomic_bool`: make it a `shared_ptr`; `MediaInput::read` (`media_engine.hpp:106`) and
   `InputContext`'s constructor (`media_engine.cpp:303`) take the `shared_ptr`; `FileSystem::io_cancellation_flag`
   (`filesystem.hpp:635`) and `WorkContext::cancelled_` (`contract/work.hpp:39-75`) feed Macha's own store and go
   with it in R7, except where they reach a `ReadHandle`.
4. `read_impl` builds its reply in a Macha-owned buffer from owned parts and copies into libfuse's buffer only on
   the worker before `dispatch` returns (the late-write fix), then publication on tickets, hydration, figures.

- FileSystem, ReadHandle, WriteHandle, CacheHydrator, HydrationManager hold `dagda::Context&`; `FileSystem::store()`
  goes.
- **Reads return owned parts:** ReadHandle returns `ReadResult::object` slices (shared, owned); `read_impl` assembles
  its reply in a Macha-owned buffer and copies into libfuse's buffer only on the worker before `dispatch` returns, so
  the timed-out-while-running read leaves no writer behind (the late-write defect, fixed here once; the low-level FUSE
  move later only swaps the reply plumbing). The request owns its cancel token; outcomes map to ECANCELED or EINTR,
  ETIMEDOUT, EIO from `ReadEnd`, never from Macha's own clock.
- **Publication on tickets:** `DurabilityBatch` becomes `std::vector<dagda::Ticket>`; deferred writes through Context
  (known-id for rebuild and re-put); the commit waits on all tickets at once; `BarrierLoss` replaces `unsatisfiable`
  and English reasons; evidence maps to FsError in one place (`cancelled` EINTR, `holders() == 0`
  `FsStorageUnavailable`; no store exception type leaves the filesystem). The extent executor stays.
- Hydration on Context with the hydrator's stop flag as cancel; activity, `idle_for`, reachability epoch (as a change
  notice; the parked recheck poll goes), capacity and extent size through Context; FUSE memory through Dannan's ledger.

**R2 writes, done 2026-11-15:** every WriteHandle and `truncate_file` store call goes through Context
(`FileSystem::put_extent` / `get_extent`, under the mount's co-owned cancel token). The handle holds `{id, Ticket}` per
deferred extent; `commit` makes one many-ticket wait (the work's deadline, else 30 min) and reads the evidence: met ->
commit; a copy with a `BarrierLoss` or a forgotten ticket -> re-put from any readable copy and wait once more, else
ESTALE; anything else unmet -> EIO (FUSE retries). The "two cancel sources" gap was moot: production publications
carry no cancel of their own, only the mount's. The publication's progress counter is co-owned
(`DataWorkContext::shared_progress`, `FileSystem::write_progress_`). Activity (reads, writes, `idle_for`, the
maintenance pacing reads and the idle gauges) is Context's now. **Not consistent until their steps land:** the torrent
publish hook still writes to Macha's store (R5) and the catalogue still uses `DurabilityBatch` (R4), while every
filesystem read is Dagda's; the namespace tree's CONTROL reads are R3. Hydration is on Context with the hydrator's stop token as
the cancel; the reachability epoch and `logical_capacity` (Dagda `6ea9d04`, `Objects::logical_capacity`) too.
`FileSystem::membership_` is now unused; it leaves the constructor with `LocalState&` in R7.

**Ack levels (the maintainer, 2026-11-15, relayed by Dagda; Kvasir's `../kvasir/TODO/2026-11-15-ack-levels-standard.md`):**
`persisted` becomes "this node's own disk", `replicated` a majority of the replication set on disk (both nodes at
replication 2), `decided` comes with Dannan 0.9.0, the setting `ack_level`. Every Macha write wants its floor (one
copy, this node first, the rest to prompt replication), so every write names `persisted` explicitly (set in
`FileSystem::put_extent`; R4's and R5's writes the same); `replicated` would put the WAN peer on every write and stop
writes while a node is away. **Settled (the maintainer, 2026-11-15, via Dagda):** `persisted` is "the disk that took it": one node's disk,
this node's when it can hold the copy, else the next in placement order, as Dagda already behaves, so the edge and
full-node cases (`test_a_full_preferred_node_spills_data_but_not_control`) stand. Dagda renames the setting
`ack_level` and makes `replicated` a majority; build against its commit once gated.

## R3. Metadata and the control store [metadata 2-7]

**Progress (2026-11-15):** `ControlNamespaceNodeStore` is over `Context::Control` (`has`, `get`, `put`,
`ensure_local`): `for_reading`, `for_commit` and `for_replay` take `dagda::Context&`. `MetadataManager`,
`AvailabilityService` and `NodeHorizonBuilder` take the Context in place of the store and control store; the
replica's replay applier gets it through `LocalServices` and `LocalState` (built after `open_store()`); `Service`
has a `store_context()` seam. Test retired in part: `rpc_cluster/test_store_control_objects_take_no_data_credit_and_commit_alone`
keeps its Macha-store half (renamed `..._take_no_data_credit`, R7's); its "a namespace node is written by a node on
its own" half is Macha's `test_a_commit_claims_the_nodes_it_introduced_and_prunes_the_rest`
(`tests/test_namespace_tree.cpp`) and Dagda's `HeadsControlNodeStore.ACommitHoldsEveryNodeItsRootNamesAndWritesOnlyWhatIsNew`.
The publication claims barrier is on `Context::Claims` (`retain_data_here`, `retain` for DATA sought and CONTROL here,
the peers' share with `every_node`), each named `call_deadline::metadata`; not finished or unheld throws
`MetadataNotReady`, CONTROL not `held_here` throws as before.
**Dagda's answer (2026-11-15) on the rest:** the census is owner surface: `Node::reference_counts()` with the library
helper `heads::ReferenceCensus` (Macha's census moved into Dagda; one `count(root, nodes, pause)` per root), run on
the maintenance thread under the build mutex until 8c. `register_reachability`, `publish_inventory` and the
collector's control epoch are owner surface, the root's. The seed: Dagda's `heads::CacheSeed` serves a head space;
with heads off until 8c none exists, so Macha's replica keeps its own seed in the interim (proposed: its own file
under the state path, not the cache Dagda now opens), and it goes at 8c. Dagda agreed, on one condition: sealed as v19
seals everything at rest (domain in the AAD) and written by Dannan's durable replace; deleted in the same step that
turns `NodeOptions::heads` on.

- `ControlNamespaceNodeStore` and `MetadataManager` over `Context::Control` (the replica's applier built after
  `open_store`).
- The publication claims barrier (`node_services.cpp:346-554`) on `Context::Claims` (DATA here, DATA sought, CONTROL
  here, the peers' share), evidence mapped to Macha's outcomes, deadlines named.
- The `current.meta` seed moves into Macha's metadata code under `with_seed`.
- One `contract::ReachabilityProvider`: the census on Dagda's reference counts, the inventory, release horizon,
  tombstone batches, `control_epoch` / `within_control_epoch` over the catalogue's mutex.
- Out of Macha's maintenance pass: the claim walk, CONTROL collection, release and prune, the control ledger verify;
  Macha's pass calls the node's steps until 8c. Macha's CONTROL handlers in its storage server go.

**R3's last piece, designed 2026-11-15 (the provider and the pass on Dagda's steps):** read against Dagda
`6ea9d04` (`ledger/collector.h`, `store/repair.h`, `node.h:395-475`, `node.cpp:546-663, 1032-1121`, Dagda's own
loop being the reference composition). The ruling stands: Macha's loop drives the steps until 8c
(`MaintenanceOptions::automatic = false`); destructive sweeps stay off on the live nodes until the provider's
answers match the namespace on both.
1. **The provider.** `MachaReachability : dagda::contract::ReachabilityProvider`, replacing `NodeHorizonBuilder`:
   `generation()` and `known_generation()` from the metadata view and server; `inventory(pause)` = the namespace's
   counted views (Dagda's `ReferenceCounts::views()`), the catalogue's DATA and CONTROL sets, the counted tree
   nodes, the tombstones as `Garbage` and their batches, `complete`, the generation, entries and extents;
   `release_head()` / `release(head)` as `NodeHorizonBuilder::release` today; `control_epoch()` and
   `within_control_epoch()` over the catalogue's root epoch and mutex (what `control_gc_step` read);
   `collect_garbage()` = `maintain_garbage_metadata` (erase) plus stamping plus `maintain_tombstone_batches`
   (matured batches). Registered by the root (`register_reachability(domain_v1, provider)` after `open_store()`,
   unregistered before `Node::stop()`).
2. **The census** counts into `Node::reference_counts()` (owner surface handed to FileSystem by the root), Macha's
   census code kept (its tree is Macha's until 8c; `heads::ReferenceCensus` needs Dagda's `Tree<Value>`), its
   vectors moved into `change`.
3. **The pass.** `Maintenance` holds `dagda::Node&` (the owner surface). Kept: Macha's metadata repair, catalogue
   repair, head repair, history truncation, the busy and quiet computations, the event wake. Replaced, in Macha's
   order: repair's credit (`Repair::earn_credit(now, interval)`), gate (`paced_by`, `may_start`, `note_gate`),
   `Collector::inventory_step(pause, may_rebuild)` (held back on a repair-only pass while metadata is not ready),
   `Repair::survey(pause)`, the turn (`begin_turn(.., PeerReading::as_begun)`, `claim_walks`, `transfer(lists())`,
   `settle`), then `tombstone_step`, `release_step`, `control_step`, `data_step` with Macha's `PassDue`
   (garbage due: not busy and metadata clean; GC due: not busy and past the quiet window; `should_yield`; `pause`);
   the local stages through `begin_local_stages({stop, busy, interval})`, `local_rebalance`, `retention_compact`,
   Macha's head repair and history truncation, `compact_packs`, `local_scrub`, `ledger_verify`. The wake: metadata
   and catalogue retries as now; GC's quiet window from `rebuilt` and the data step's deferral and earliest
   retirement; repair from `settle` (`credit`: the credit's time to an extent at the rate `earn_credit` gave;
   `yielded`/`more_work`: one interval; `quiescent`: an event) and `turn_wait`; Dagda's `maintenance_events()`
   joined to Macha's event wait.
4. **Leaves Macha** with this slice (code removed in R7): `RetentionLedger` use in the pass, the claim walks,
   `catalogue_.control_gc_step`, the sightings files' handling, Macha's survey in the pass (Macha's
   `AvailabilityService` keeps serving its API from its last survey until R6 moves the files path table and Status
   onto Dagda's survey), the scrub schedule file (Dagda keeps `<state>/maintenance/scrub.next`, Macha's name).

**R3's last piece, done 2026-11-15 (compile pending, build-3 held by a reboot):** `MachaReachability` (the
provider; `reachability_inventory` and `reachability_release` pure, `test_horizon_builder.cpp` ported onto them)
replaces `NodeHorizonBuilder`; the catalogue answers `control_epoch` and `within_control_epoch`; the census counts
into `Node::references()`; `Maintenance` drives the node's steps (above), and `MaintenanceStopping` aliases Dagda's.
**New config key** `maintenance.collect_garbage` (default false, documented): the root registers the provider only
when it is set, so the live nodes reclaim nothing until the provider's answers are checked against the namespace
(the cutover ruling). The pass waits at most one interval (the store's cadence) until Macha's `NodeEvents` become
Dannan's.

**Before any suite can run** (the gate at the end; each is one of R1's leftovers or R6/R7): `LocalState`'s store
parts go (its pool, control store, cache, retention store and reference counts open the same paths as the store
node; only the replica and its seed stay); Macha's `StorageServer` and its store message handlers go (the node's
answer them); Macha's `AvailabilityService` stops surveying and its `trie_diff` route goes, its API reading Dagda's
survey (R6); the trace fixtures' `ManualMaintenanceClock` reaches the node as `MaintenanceOptions::time`/`wall`.

## R4. The catalogue [catalogue 1-7]

**Progress (2026-11-15, compiles with R3's last piece):** `CatalogueManager(NodeRuntime&, MetadataServer&,
dagda::Context&, MetadataView&)`: its CONTROL reads, writes and offer through `Context::Control`, DATA reads through
`Objects::read` (`read_data`, none when not served), the media index written `persisted`, artwork staged with
`put_here` / `put_deferred_here` into `ArtworkTickets` and made durable by one `Tickets::wait` (every ticket met, else
the batch is kept and retried); `control_gc_step` and its cursor and epoch map are gone (the store's collector sweeps,
the provider answers the epoch). Test changed: `hydration_catalogue/test_catalogue_on_one_node_predicts_caches_and_protects_its_control_objects`
no longer sweeps through `control_gc_step`; it checks the epoch Macha answers (it moves on a new root, and a removal
judged in an earlier epoch does not run); the sweep itself is Dagda's `LedgerCollector.AControlObjectStagedInTheCurrentEpochIsKept`
and `.AControlRemovalNeverRacesANewEpoch`. Row 32 done: every catalogue store call goes through `store_call` (anything
the store throws becomes `CatalogueUnavailable`) or `control_object` (ensure-local then get, one call), and artwork
that cannot be stored is `CatalogueUnavailable`, never a plain error, so a store fault defers a hint without spending
an attempt. Row 31 with R1's leftover, `NodeEvents` becoming Dannan's (built on Dannan 0.8.3 with develop's `fbcbb90` applied, which adds
`NodeEvents::wait_for_change(seen, timeout, std::stop_token)` at Macha's request, ABI kept, for 0.8.4 if the maintainer wants one;
develop itself is 0.9.0, so Macha does not build on it; prefix `prefix-dannan-083-stop`, `build/pi/deps_dannan_083_stop.sh`):
`NodeEvents` is Dannan's type, the Dannan node's instance (`NodeRuntime::events()`, handed to `NodeResources`); Macha's
kinds are `node_event::storage`, `metadata`, `claims` at fixed numbers, named by `NodeRuntime` before the store node
names the same three; Macha's own topology notion is gone: Dannan raises membership (joins, leaves, place,
capabilities) and topology (any advert), and Macha raises metadata for a higher generation or a changed write floor.
The installer retries on storage and membership; maintenance counts through `Node::maintenance_events()` (storage,
metadata, membership, claims) and its clock waits on `NodeEvents`; availability counts membership; `Accounts` gossips
to a peer that joins or restarts through a Dannan roster watch and waits on its own demand. Waits end on the
consumer's stop token (`wait_for_events`).

CONTROL calls onto `Context::Control`; `CatalogueManager::control_gc_step` and its state go (the provider answers
the root epoch); the scanner's and editor's batches become tickets; every store result maps to `CatalogueUnavailable`
(a store failure never spends a hint's attempt); media index and artwork take shared buffers; ReadHandle per R2;
`CatalogueInstaller` waits on Dannan's storage and membership events.

## R5. Ingest and torrent [ingest 1-6]

**Progress (2026-11-15):** the torrent publish hook writes through `Objects::write` at `persisted`, cancelled by the
disk backend's abort flag, now co-owned (`TorrentDiskHooks::publish` takes `shared_ptr<atomic_bool>`); a cancelled or
landed-nowhere write is no extent. Then: `loader_admission(dagda::Context&)` admits through
`Devices::admit_data` at loader class in one-second slices (Dagda's arbiter checks the cancel only in its predicate,
as Macha's did), the hook's abort co-owned; the torrent's I/O observed through `Devices::note_io`; `TorrentManager`
no longer takes `LocalState` or the arbiter (nor does the plugin's context check); its tests run on a `TestNode`'s
store (`LoaderNode`), reading the credit from `Node::credit()->stats()`. One extent size: the hook's
`config.extent_size` is the key `store_node.cpp` gives the node's options, so there is one source. The ingest blocks as
`storage_unavailable` on `FsStorageUnavailable` only (Context's writes answer with evidence; the filesystem maps
landed-nowhere to it); its metadata classification was already typed (`MetadataNotReady`). R5 is done.

Torrent publish through Context (co-owned cancel, evidence to `optional<ObjectId>`); `loader_admission` and the
device monitor through Context's Devices noun; one extent size from the node's options; the ingest's blocked
classification on `FsStorageUnavailable` only; its metadata classification on a typed outcome, never text.

## R6. Status, adverts, telemetry, reload, files [root 6-12]

Adverts from `dagda/capacity` and `dagda/used` (Macha's `macha/*` and `advertise_storage` go); hosting handed to Dagda
(the advert written by one path only); telemetry from Dagda's figures; reload through Dagda (`StorageServer::refresh`
goes, which retires the reload races found in the map); Status's `data_store`, `repair`, `prompt_replication`,
`data_resources` from Dagda with Macha's JSON keys kept; catalogue `ready` on CONTROL presence; artwork read with a
deadline (timeout as 503); the files path table from Dagda's survey, persisted as now.

**R6 and R7, the order (2026-11-16, from the map's rows A8, A16, A19-A24, B1-B4, B12b, B13, B16):** each step one
commit that compiles; Macha's store stays compiled only while a step still needs it.
1. Status through the store node: `StatusSources` carries `dagda::Node*` (the owner's figures: `data().diagnostics()`,
   `store().repair_diagnostics()`, `repair().gates()`, `store().prompt_replication_stats()`, `credit()->stats()`) in
   place of `local` and `store`; Macha's JSON keys kept (`viewer_*` from `foreground_*`, `paced_by` words);
   `Service::repair_diagnostics()` and `store()` seams re-pointed or gone.
2. Macha's `DistributedStore` out of `NodeServices`, `FileSystem` (`s_`, `store()`), the FUSE comment, `Service`.
3. Adverts and telemetry (A23, A24, A8): `node_info_from` reads `dagda/capacity` and `store::used_of`; telemetry and
   `data_storage_degraded` from the node's own record through `store::figures_of`; `advertise_storage`,
   `advertise_storage_backends`, `advertise_cache` and the `macha/capacity`, `macha/used` adverts go; `follow_inbound`
   tells Dagda's server (`set_hosts_extents`) and stops writing `dagda/extents` itself (A22).
4. `StorageServer` and `LocalState`'s store parts go (the node answers the store messages); `LocalState` keeps the
   replica and its seed; reload through `Node::reconfigure` (A19, which retires D1 and D2).
5. `AvailabilityService` on Dagda's survey (`Node::availability()`), the path table built from it and persisted as
   now (A16, B16); its `trie_diff` route goes.
6. The trace clock into `MaintenanceOptions::time`/`wall`; CONTROL presence for catalogue `ready` (B12b); artwork
   read with a deadline, timeout 503 (B13, an API change to announce).
Done so far (2026-11-16): step 1 (`62f63f2`), step 2 (`14d34b6`, with the tests on the store node's parts
`b0e2c61`), step 6's artwork deadline (`bfd13ec`; catalogue `ready` was already on CONTROL presence through Context),
step 5 and step 6's trace clock (below). Step 5: `AvailabilityService` no longer surveys; each pass, after the store
node's survey (`Repair::survey`), it builds the path table over `Node::availability().snapshot()` and this node's
holdings from the store node's ledger, kept, followed through the tree's change, or walked, and persisted as before;
its `trie_diff` route and the sightings files' `UnreferencedSince` and Macha's `RetentionLedger` leave `NodeServices`.
The survey type is still Macha's twin, converted (`survey_of`), until the three survey cases retire with Dagda's
gate and Macha's `ledger/availability` goes; then `AvailabilitySurvey`, `PeerDiff` and `HeldFn` alias
`dagda::ledger`'s (Dagda agreed). The trace clock: `MaintenanceClock` is Dannan's `TimeSource` and `WallClock`, and
`Service` hands it to the store node as `MaintenanceOptions::time` and `wall`.
7. The tests: each case's fate from `../dagda/TODO/2026-11-05-test-mapping.md` (RETIRE against its named Dagda case,
   REWRITE over the node fixtures, KEEP), recorded case by case; then Macha's store code removed (plan S.1's list).
Steps 3, 4 and 7 landed after the paragraph above: reload and hosting through the store node (`dfedd6a`), the last
store cases retired and `StoreBench` gone (`0a777a7`), Macha's store deleted with adverts and telemetry Dagda's
(`deceac1`); checked in the tree 2026-11-16 (`advertise_storage` and the `macha/*` adverts are gone, `Service` calls
`set_hosts_extents`, `LocalState` holds the seed and the replica only). What remains is `ACTIVE.md`'s "Next, in
order"; the deps this file's head names (Dannan 0.8.0, 0.8.3, Dagda `6ea9d04`) are superseded by the ones there.

## R7. Removal

Macha's store code that Dagda now holds goes (plan S.1's list), each test retired against the Dagda case
`../../dagda/TODO/2026-11-05-test-mapping.md` names, recorded case by case.

## Before the flag day (not code)

- **API changes announced to Core and every client before they ship** (operator: the `dagda.<name>` names at the
  cutover, not before): store `message_timings` keys, `startup.error` text, `retained_memory.owners` values, Status
  figure age, artwork 503, diagnostics `data_resources.available` (new: false until the store node's store is open,
  its figures zero meanwhile; the section reads the store node's admission); and (the maintainer, 2026-11-16) repair's
  `offer_bytes` and `in_flight` added to Status's repair section beside `bytes_transferred`, once Dagda's repair
  stage 4 builds them (names from Dagda).
- Custody: Dannan's installed tool (asked 2026-11-14), sealing with the host key, the unit's `LoadCredentialEncrypted=`,
  the backup of the plain root or the sealed root with `credential.secret`; at the next deploy (operator).
- The gates (the maintainer, 2026-11-14): Macha's suites on Dagda; the side-by-side harness on the Pis; the migration rehearsal on
  copies of real state; reliability under crash, restart and partition; then the port against the live baselines.
</content>
</invoke>

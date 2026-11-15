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
Dagda, then the harness and the migration rehearsal. Deps: Dannan 0.7.0 and Dagda `2f7eba6` built on build-3 into
`/home/maintainer/deps/prefix-0.7.0` and `prefix-dagda-2f7eba6` (`build/pi/deps_070.sh`); the cutover pins Dannan 0.8.0
(the `dannan-custody` tool lands there) and Dagda's tag once it builds on 0.8.0.
**On Dannan 0.8.0 since `6dfc975`** (2026-11-15): deps on build-3 in `/home/maintainer/deps/prefix-0.8.0` and
`prefix-dagda-f6e3b58` (`build/pi/deps_080.sh`; `dannan-custody` installed there); Macha has no test double of
`dannan::Nodes`, so 0.8.0's new pure virtuals touch nothing; a clean build directory was needed so CMake would not keep
the 0.7.0 prefix. **Dannan 0.8.1** (2026-11-15, `53cc933`: TCP_NODELAY on accepted sockets, a
`stop_accepting` that never hangs; ABI kept, custody tool unchanged): the pin's minimum 0.8.0 already accepts it;
build the deploy's deps from the newest 0.8 patch: **0.8.2** (`20771a6`: placement copies only its answer, a
listener's key refused for what is actually wrong; ABI 0.8.0's). **0.8.3** (`00a3a12`: the outbound StreamConnector, RS256 verification;
ABI 0.8.0's, custody tool unchanged) is now the newest patch to build the deploy's deps from.

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
writes while a node is away. **Open, put to Dagda for the maintainer:** an edge node or a full node lands Macha's one copy on a
peer (`test_a_full_preferred_node_spills_data_but_not_control`); the new `persisted` cannot answer that. Recommended:
`persisted` = the disk of the node that takes the write, this one when it can.

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

## R4. The catalogue [catalogue 1-7]

CONTROL calls onto `Context::Control`; `CatalogueManager::control_gc_step` and its state go (the provider answers
the root epoch); the scanner's and editor's batches become tickets; every store result maps to `CatalogueUnavailable`
(a store failure never spends a hint's attempt); media index and artwork take shared buffers; ReadHandle per R2;
`CatalogueInstaller` waits on Dannan's storage and membership events.

## R5. Ingest and torrent [ingest 1-6]

Torrent publish through Context (co-owned cancel, evidence to `optional<ObjectId>`); `loader_admission` and the
device monitor through Context's Devices noun; one extent size from the node's options; the ingest's blocked
classification on `FsStorageUnavailable` only; its metadata classification on a typed outcome, never text.

## R6. Status, adverts, telemetry, reload, files [root 6-12]

Adverts from `dagda/capacity` and `dagda/used` (Macha's `macha/*` and `advertise_storage` go); hosting handed to Dagda
(the advert written by one path only); telemetry from Dagda's figures; reload through Dagda (`StorageServer::refresh`
goes, which retires the reload races found in the map); Status's `data_store`, `repair`, `prompt_replication`,
`data_resources` from Dagda with Macha's JSON keys kept; catalogue `ready` on CONTROL presence; artwork read with a
deadline (timeout as 503); the files path table from Dagda's survey, persisted as now.

## R7. Removal

Macha's store code that Dagda now holds goes (plan S.1's list), each test retired against the Dagda case
`../../dagda/TODO/2026-11-05-test-mapping.md` names, recorded case by case.

## Before the flag day (not code)

- **API changes announced to Core and every client before they ship** (operator: the `dagda.<name>` names at the
  cutover, not before): store `message_timings` keys, `startup.error` text, `retained_memory.owners` values, Status
  figure age, artwork 503.
- Custody: Dannan's installed tool (asked 2026-11-14), sealing with the host key, the unit's `LoadCredentialEncrypted=`,
  the backup of the plain root or the sealed root with `credential.secret`; at the next deploy (operator).
- The gates (the maintainer, 2026-11-14): Macha's suites on Dagda; the side-by-side harness on the Pis; the migration rehearsal on
  copies of real state; reliability under crash, restart and partition; then the port against the live baselines.
</content>
</invoke>

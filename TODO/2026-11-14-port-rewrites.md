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

## R3. Metadata and the control store [metadata 2-7]

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

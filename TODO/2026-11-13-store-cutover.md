# The store cutover, stage one: Macha's store onto Dagda 0.1.x (2026-11-13)

Operator, 2026-11-13: "Ok, proceed. Use 10.44.1.202 to build to keep the load off the two extant nodes." Reviewed
first: Dagda's report at 0.1.3 and its migration's write path (crash ordering, torn tags, fsync before markers), in
`2026-11-08-port.md`'s build log and this session's report to the operator. The plan Dagda wrote is
`../dagda/TODO/2026-11-05-second-cutover.md` (step S, 8a, 8b, the migration plan, B1 to B15); its file list was
re-checked against `develop` at `875df7e`: 100 of its 104 Macha paths exist (about 45,000 lines); the predicates and
`tools/namespace_migrate.cpp` went in 0.93.4.

## Settled before the build

- **One disk event.** S, 8a and 8b deploy together (a v19 disk is refused by Macha, v18 by Dagda): both nodes stop,
  migrate, start. No mixed phase. 8c (heads) and 8d later, with no object rewrite.
- **Rulings that answer the plan's questions:** custody from a systemd credential, one state directory (Macha's
  `state_path`, so `--node-state` is `--state`), Macha's maintenance loop until 8c driving Dagda's steps
  (`MaintenanceOptions::automatic = false`), so B5 and B9 wait for 8c.
- **Closed in Dagda already:** B1 (`Node::OpenLater` then `open_store()`), B3 (`set_hosts_extents`), B6 (observations,
  slow-stage lines), B10 (`held_here`, `cache_local`, `hydrate`, `retain_data_here`, `reachability_epoch`), the node's
  stages one by one, about 35 `dagda.*` figures, `device_pressure` for `dht.io_pressure_*`, the publication budget,
  purpose deadlines (0.1.1), the store's journal lines in Macha's words (0.1.2).
- **Destructive maintenance off at first.** With no reachability provider the node removes nothing
  (`NodeMaintenance.WithNoProviderTheCollectorRemovesNothingAndSaysSo`); the provider (8a) is built and gated, but
  sweeps stay off on the live nodes until its answers match the namespace on both.
- **Build and gate on corvus-neptune** (10.44.1.202), never on the live nodes; tell the Dagda session before long
  runs there.

## Order

0. **Link.** Waiting on Dagda: install rules, `dagdaConfig.cmake` (ExactVersion, `dagda::dagda`, `find_dependency(dannan)`),
   SONAME at the full version, `dagda-migrate` installed (asked 2026-11-13, as 0.1.4). Then `find_package(dagda 0.1.4
   CONFIG REQUIRED)` beside Dannan on the laptop (`build/deps/`) and corvus-neptune (`/home/tom/deps/`).
1. **The node in Macha's root.** `LocalState` opens a `dagda::Node` in two parts: constructed before the Dannan node
   starts (planes declared, types registered), `open_store()` on the recovery thread. `LocalState` keeps only Macha's
   `MetadataReplica` until 8c; the control store reaches it as `Node::control()` through an adapter; the cache seed for
   the replica stays Macha's (B11). Macha's 23 store message numbers leave its own registrations at the same moment.
2. **Config** (plan S.3): every store key mapped to `NodeOptions`, with the defaults that differ set explicitly
   (`write_copies` 1, `background_concurrency` resolved from 0, `peer_window`, `prompt_read_admission` off,
   `idle_bandwidth_fraction` into both fields, `no_progress` 100 years for 0).
3. **Callers** onto Dagda's store: filesystem, hydration, catalogue, metadata server and manager, namespace control
   store, torrent disk I/O and manager, media segments, write-behind (one copy, decided with Dannan), the availability
   service (survey to Dagda, path table stays Macha's). `DurabilityBatch` maps to tickets.
4. **Status** (plan S.2): the facade reads the accessors live, not the 10 s published figures; peers' cache and
   backends-online need hosted telemetry fields.
5. **Maintenance:** Macha's loop keeps metadata, catalogue and head repair and truncation; the local stages, repair
   and the survey go to the node's steps; GC through the provider (8a) with sweeps gated off by config at first.
6. **Removal** of the store code Dagda now holds (plan S.1's list), each test retired against the case
   `../dagda/TODO/2026-11-05-test-mapping.md` names, recorded case by case as the transport's were.
7. **8a** the reachability provider, **8b** the lease template, each with its gate.
7b. **Custody** (found 2026-11-13: Macha's Dannan node is given none today, `dannan_bridge.cpp` sets no
   `NodeConfig::custody`, and a Dagda node without custody is refused). A config key naming the systemd credential
   (operator's ruling: a credential, so the nodes restart unattended after a power cut); `NodeConfig::custody =
   KeySource::systemd_credential(name)`; the unit's `LoadCredentialEncrypted=` (no TPM on the Pis: sealed with the
   host key); made once per node with `dannan-node --state <state_path> --init-custody --custody-credential <name>`
   (prints the root to seal with `systemd-creds encrypt`), so the tarball carries `dannan-node`; the custody file and
   the sealed root backed up off-node (losing custody after migration loses that node's claims and reference
   counts). Test fixtures make custody under a test credential directory.
8. **Before the flag day:** suites green on corvus-neptune and in the container; the rehearsal on a fresh copy of a
   0.94.x node's state (control store, ledgers, a pack sample) with the verifier listing every id before and after;
   custody provisioned and backed up off-node; each node's control store copied off-node; then both nodes at once.

## Macha's own items from the stack's needs (Dannan's list P6, 2026-11-13)

Agreed with the Dannan session as Macha's to fix, at the cutover or when its holders become hosted owners:
- Async calls carry no watch: move onto Dannan's per-call `CallWatch` (`Routes::call`), drop the zeroed node-wide watches
  (`src/cluster/dannan_bridge.cpp:55-59`) and `rpc_wait.cpp`'s polling.
- The operator's connectivity check blocks an HTTP thread up to the summed timeouts plus 5 s
  (`src/cluster/cluster.cpp:627-645`, from `src/api/status_api.cpp:1453`): answer it deferred.
- `FrameType` and `WorkClass` cast by value to Dannan's (`dannan_bridge.hpp:50-55`): use Dannan's enums.
- Two memory fits (Macha's `validate`; Dannan's `fit_memory_limit` unused, `dannan_bridge.cpp:71-77`).
- Two durability epochs per process (`node_identity.cpp:45`): goes with the store.
- Payload copies per request (`dannan_host.cpp:21-26`, `:108-111`): adopt Dannan's `net::Message` in handlers.
- `connections_canonical` written twice: Dannan 0.5.3's node writes it (`node.cpp:1066`); Macha's telemetry hook writes it
  again (`src/cluster/cluster.cpp:168-170`, from F15 against an older Dannan): delete Macha's.
Dannan's own fixes (feature/macha-fixes, for 0.7): the key-derivation labels exported (`ClusterKey::from_master`),
`idle_for` from creation, a const `inbound()`, `free_port` below 32768, handlers by value. Withdrawn as not Dannan's:
the first telemetry at join (inferred, never observed).

## The operator's rulings on the stack's needs (2026-11-13)

- **Dannan (`../dannan/TODO/2026-11-13-stack-needs.md`):** P1 the ABI promise first: `libdannan.so.0.5`, package
  `libdannan0.5`, CMake SameMinorVersion, abidiff in CI; Macha's deb depends on `libdannan0.5` at a minimum patch.
  Then P6, all approved: the seven small fixes, the owners' test seam, the advert-change watch, non-blocking logging,
  each sent here for review. P4: CPU pacing moves into Dannan, proved by
  `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy`, `maintenance_trace` and a node baseline (the node
  run at the operator's word).
- **Dagda (`../dagda/TODO/2026-11-13-three-systems-design.md`):** as proposed; 0.1.5 (Dannan 0.5.6, the debs) first in
  slice 0, on the port's critical path; any tag changing a path Macha runs needs Macha's baseline comparison first.
  8b is empty for Macha at stage one.

## Log

- 2026-11-13: Dannan 0.6.0 tagged and pushed (main `45dc96b`): the ABI promise starts here (`libdannan.so.0.6`, package
  `libdannan0.6`), after Macha's review found 0.5.4 broke `libdannan.so.0.5`'s ABI against 0.5.3. Macha's path: Dagda 0.1.5
  on Dannan 0.6.0, then `Depends: libdannan0.6 (>= 0.6.0)`, one relink from 0.5.3. Packages signed and uploaded by the
  operator; nothing on the Pis without his word.
- 2026-11-13, step 1 reading: Dagda registers 20 of Macha's numbers and 3 of its own (`dagda/store/messages.cpp:26-59`);
  Macha's table (`src/cluster/dannan_bridge.cpp:286-365`) drops them at the same moment: requests have_object 515,
  get_object 516, put_object 517, delete_object 525, get/put_control_object 531/532, put_object_deferred 534,
  object_durability_barrier 535, retain_objects 544, have_objects 552, have_control_objects 557, have_valid_objects
  559, trie_diff 561; replies bool_reply 615, object_reply 616, control_object_reply 619, have_objects_reply 630,
  have_control_objects_reply 633, have_valid_objects_reply 635, trie_diff_reply 637. Rule differences, harmless
  across the flag day: delete_object and have_valid_objects are `not_control` in Dagda (any in Macha); bool_reply is
  both lanes (Macha's metadata replies ride it: a superset). **Client-visible:** registration names become
  `dagda.<name>`, so Status figures keyed by type name change: announce to Core and the clients before shipping.
- 2026-11-13: `NodeId` and `Hash256` are Dannan's (`src/types.hpp` aliases; Macha's `to_string(NodeId)` gone for
  Dannan's): one definition across the stack, so object ids cross into Dagda unconverted. Laptop `build-store` 714 of 714.
- 2026-11-13: step 0 done on the laptop: Dagda 0.1.4 (local tag `f3a9852`, not yet on its remote) built into
  `build/deps/prefix-dagda-0.1.4` (Boost 1.88 on the path), Macha's tree `build-store` links it; `storage_v18` 22 of 22
  run from it. The port branch carries the store work; `develop` stays on 0.94.2's line until the cutover is gated
  (CI runs on main and develop only). CI and the container image need Dagda from its tag, so Dagda's tags must be
  pushed (operator's word) before develop takes this. On corvus-neptune Dagda 0.1.4 fails under GCC 14 Release with
  `-DDAGDA_WARNINGS_AS_ERRORS=ON` (`-Wnonnull` at `migrate/compare.cpp:271`, `-Wstringop-overread` in libstdc++;
  reported to Dagda); built there without it.
- 2026-11-13: plan written; baselines on 0.94.2 running (`build/baselines/run_0942.sh`, results in
  `build/baselines/results-0.94.2/`); Dagda asked for its install rules.

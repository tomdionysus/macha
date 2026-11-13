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
- **Build and gate on corvus-develop** (10.44.1.202), never on the live nodes; tell the Dagda session before long
  runs there.

## Order

0. **Link.** Waiting on Dagda: install rules, `dagdaConfig.cmake` (ExactVersion, `dagda::dagda`, `find_dependency(dannan)`),
   SONAME at the full version, `dagda-migrate` installed (asked 2026-11-13, as 0.1.4). Then `find_package(dagda 0.1.4
   CONFIG REQUIRED)` beside Dannan on the laptop (`build/deps/`) and corvus-develop (`/home/tom/deps/`).
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
8. **Before the flag day:** suites green on corvus-develop and in the container; the rehearsal on a fresh copy of a
   0.94.x node's state (control store, ledgers, a pack sample) with the verifier listing every id before and after;
   custody provisioned and backed up off-node; each node's control store copied off-node; then both nodes at once.

## Log

- 2026-11-13: plan written; baselines on 0.94.2 running (`build/baselines/run_0942.sh`, results in
  `build/baselines/results-0.94.2/`); Dagda asked for its install rules.

# The transport's boundary: what Macha uses from it (2026-11-10, branch `port/dannan-dagda` at `7efe943`)

Read-only inventory for the first cutover: every use, outside the transport files, of the names they declare.
Transport files: `src/cluster/net.{hpp,cpp}`, `cluster.{hpp,cpp}`, `membership.{hpp,cpp}`, `telemetry.{hpp,cpp}`,
`message_routes.hpp`, `public_connectivity.{hpp,cpp}`, `cluster_node.hpp`. Found by regex over `src/` and `tests/`,
ambiguous hits read; calls through receivers not named `node_`, `node`, `n_`, `node()`, `context.node`, `runtime` or
`cluster` could be missed.

## The facade's rules (from Dannan's first-cutover findings and capability report at 0.5.2/0.5.3)

- **Every call names its deadline** (operator, ruling A/F2): no default, no blanket constant. `NodeRuntime::call` and
  `call_async` take one; each call site states it. Node-wide watches off (all four `NodeConfig` no-progress and stall
  settings 0); Macha's own no-progress loop runs over `CallHandle::idle_for` for synchronous calls, keeping its
  journal lines (F1).
- **Topology and metadata events** are Macha's, derived from `Nodes::watch` by diffing adverts: topology on joined,
  left, a place change, `macha/hosts-extents` or the replicas advert; metadata on a higher generation. Dannan's own
  kinds are not consumed (F7). `remote_metadata_generation` raised by CAS only.
- **Activity**: Macha keeps its own `ActivityClocks`; `Context::activity()` is not read (F8).
- **Construction order** (F9): hold `.macha.lock`, run the layout check, build the node, then everything that charges
  `node.memory()`. The observation recorder is the node's (remove Service's own writer); the `shutdown` event before
  `node.stop()`. `Node::request_stop` exists.
- **Readiness** (F5): a sixth plane, `local_services`, marked where `service.cpp` marks recovery complete
  (`Readiness::hold_until_marked`, `mark_complete`); Status reads the five planes by name.
- **metadata_notice** registered as a plain notification handled on the session (`Registration::on_session`), read
  as it arrives ahead of every queue; its handler never waits (F10). Not also fast control: Dannan refuses that
  combination, and the session path already passes every queue.
- **Version**: `NodeConfig::version = kServerVersion` (F11). `SupervisorOptions::system_name = "macha"`.
  `inbound_journal_fields` adds ` hosts_extents=`. UPnP: `NodeConfig::gateway = make_shared<reach::UpnpGateway>()` (F4).
- **Memory** (F18): keep Macha's `validate` as the one fit, pass its results, `memory_limit_bytes` 0.
- **Numbers**: Macha's types at 512 + current; retire 549, 550, 560, 636; Dannan's core types replace ping, members,
  session_retire, promote_*, cancel_transfer, telemetry, identity_resets, dial_request, dial_back(_probe), ok, error and
  their replies (first-cutover 7.1, 7.2).
- **Status gaps**: `canonical_connections` (F15), thread names (F14), `read-ahead` spelling (fixed in Dannan).
- **Importer**: `dannan-import-macha` (refuses on `.macha.lock` and `.dannan.lock`, keeps `*.macha` for rollback, F6,
  F17). Rollback restores the three `*.macha` files.
- Open on Dannan's side: the macOS TSan handshake timeout (on Macha's path until shown otherwise).

## The facade's design (decided 2026-11-10, before code)

- **`DannanHost`** (new, `src/cluster/dannan_host.{hpp,cpp}`): owns the `boost::asio::io_context`, its threads (a
  deliberate count, 2 to start, measured on the Pis), and the `dannan::Node`. Builds `NodeConfig` from Macha's
  `Config` (first-cutover section 3); registers every Macha type with its `Registration` (section 7.2) and retires
  549, 550, 560, 636 before `Node::start`; one hosted handler per type forwards to `MessageRoutes::dispatch`, so the 43
  bind sites stay as they are.
- **`NodeRuntime`** keeps its public interface and delegates. Calls gain a deadline parameter (ruling A); each call
  site names one from purpose-named constants (control, metadata, object transfer), never one blanket default.
  Synchronous calls run Macha's own no-progress loop over `CallHandle::idle_for` (F1); `AsyncRpc` wraps a promise and
  the `CallHandle` (cancel, abort, promote, idle_for).
- **`Membership` is a view, not a cache**: Macha's `Membership` expires nodes on its own `dead_after`, and the
  roster watch reports changes, not heartbeats, so a fed cache would expire live peers. With a Dannan source set,
  `all`, `active`, `snapshot`, `directly_reachable`, `all_known_reachable` ask `dannan::Nodes` and convert each
  `NodeRecord` to `NodeInfo` (capacity, used, metadata generation, replicas, flags from adverts); without one (test
  fakes) it keeps its local behaviour. Adverts: `macha/capacity`, `macha/used`, `macha/metadata-generation`,
  `macha/hosts-extents`, the replicas advert.
- **Events** from `Nodes::watch` (F7), **telemetry** through hosted fields 1024 to 1045 (first-cutover section 6), and
  `TelemetryStore` as a view decoding them.
- The old `RpcClient`, `RpcServer`, `SecureChannel`, `MessageAssembler`, Macha's `membership` merge,
  `public_connectivity` and the three loops go when the facade is in and the 52 internal tests are retired.

## Operator's decisions, 2026-11-11

- **Deadlines**: a purpose-named set, each call site naming one: control 2 min, metadata 5 min, object transfer
  30 min, presence probe 2 min. The no-progress watch still ends a stalled call sooner.
- **Flag day**: back to back. Install inverbeg, check it starts cleanly alone, then macnessa within minutes;
  rollback to 0.93.6 on both.
- **Status thread names**: the facade maps Dannan's threads to Macha's names in `threads[]` (`cluster-telemetry`,
  `cluster-maintenance`, `cluster-connectivity`; rows with no counterpart dropped). Changing the names in every
  client to Dannan's is a TODO (`ACTIVE.md`).
- **Push**: `port/dannan-dagda` is pushed after each tested step.

## What the facade keeps (its interface), and what retires

Kept, with Dannan behind them: `MessageType` (enumerators by name; values change to 512+n, the transport's own types
become Dannan's core types), `RpcMessage {type, payload}`, `RpcReply {peer, message}`, `AsyncRpc` (`wait_for`, `get`,
`cancel`, `abort`, `promote`, `promotion_callback`, `idle_for`), `MessageRoutes` (`bind`, `unbind`, `dispatch`;
`Handler = std::function<RpcMessage(const NodeInfo&, FrameType, const RpcMessage&)>`), `NodeRuntime` (methods below),
`ClusterNode` (whole interface: `DistributedStore`, `ControlObjectFetch`, `LocalState`), `Membership` (as a view),
`TelemetryStore` (as a view), `RpcStats` (`connections_created`, `connections_reused` read), `RpcServerWorkStats` (every
field, Status only), `frame_type_name`, `message_type_name`, `frame_type_priority`, `have_valid_objects_max`,
`max_pending_rpc_requests`, `max_peer_outbound_messages`, `RpcLinks`/`NetworkLinks` as a test injection point
(`ServiceInstruments::links`).

Retired (no use outside): `TransportLane`, `TransportTraffic`, `transport_lane_name`, `outbound_refusal`,
`peer_socket_options`, `WireFragment`, `MessageAssembler`, `SecureChannel`, `RpcRoute`, `RpcFrame`, `RpcClient`,
`RpcServer`, `RpcServerExecutionLimits`, `default_frame_type`, `max_peer_outbound_bytes`, `small_outbound_*`.

Numeric `MessageType` use outside: two error strings (`distributed_store.cpp:499`, `:794`) and the order of
`std::map<MessageType, Timing>` in Status (`status_api.cpp:992`). Nothing indexes by it; the 128-slot table is inside
`message_routes.hpp` (lines 39, 56-62) and goes.

## NodeRuntime methods used outside

start, request_stop, stop (service.cpp); cancel_outbound_calls (node_services.cpp:324); advertise_storage
(storage_server, distributed_store via ClusterNode); advertise_storage_backends, advertise_cache (storage_server);
advertise_metadata_generation (local_services, metadata_server); readiness (status_api, service); config (~150 sites);
keys (catalogue.hpp:425); node_id (~100); durability_epoch (distributed_store); publish_self (local_services);
signal_telemetry_refresh (service.cpp:526); membership (~36); telemetry (status_api, cluster_unmatched,
availability_service); broadcast_notify (accounts.cpp); call (cluster_jobs, status_api, cluster_unmatched,
metadata_manager, availability_service, torrent_coordinator, control_objects, distributed_store); call_async
(metadata_manager, distributed_store); announce_metadata_generation (metadata_server.cpp:100); reconfigure_local
(service_config.cpp:43); peer_viewers_active (maintenance.cpp:416); remote_metadata_generation (metadata_server);
remote_metadata_epoch (metadata_manager, maintenance); apply_identity_reset, propagate_identity_reset, identity_resets
(manage_api, metadata_manager, metadata_server, status_api); public_connectivity_status, refresh_public_connectivity,
inbound_resolution, rpc_stats, peer_latencies, rpc_server_work_stats (status_api); peer_latency (metadata_manager,
distributed_store). wait_local_state_ready: tests only. Not used outside: broadcast_best_effort, traffic_totals,
metadata_announcements, inbound_capable, hosts_extents.

## Membership, NodeInfo, telemetry

Membership outside: `active` (about 30 sites), `all`, `snapshot` (`.all`, `.active`), `directly_reachable`
(metadata_manager.cpp:2005), `all_known_reachable` (maintenance.cpp:864); `observe`, `storage` and the constructor in
tests only. NodeInfo (types.hpp:52, codec.cpp:114-140) fields read outside: id, host, port, failure_domain everywhere;
capacity, used (placement, distributed_store, filesystem); seen_unix_ms (status_api); metadata_generation,
metadata_write_replicas_required (metadata, metadata_merge, metadata_manager); flags through `node_inbound_capable`
and `node_hosts_extents`. TelemetryStore outside: persist, views, persisted (status_api), all (cluster_unmatched,
availability_service); `TelemetryView`, `runs_at_least`, `NodePhase`, `node_phase_name`, `CacheActivity`,
`TranscodeRate`; NodeTelemetry fields read only by status_api.

## Route bind sites (43)

accounts.cpp: user_sync, session_sync. storage_server.cpp: have_object, have_objects, have_valid_objects,
have_control_objects, get_object, get_control_object, put_object, put_object_deferred, put_control_object,
object_durability_barrier, retain_objects, delete_object. metadata_server.cpp: get_metadata, get_committed_metadata,
get_metadata_identity, get_metadata_history_entry, get_metadata_history_record, has_metadata_history_entry,
put_metadata_history_entry, get_metadata_heads, put_metadata_commit, accept_metadata_commit. availability_service.cpp:
trie_diff. node_services.cpp: get_ingest_jobs, ingest_job_action, submit_ingest_job, get_unmatched_hints,
torrent_intent. torrent_plugin.cpp: get_torrent_jobs, torrent_job_action. cluster.cpp (transport, become Dannan's):
ping, dial_request, dial_back_probe, members, telemetry, identity_resets.

## Tests that touch transport internals (to retire against named Dannan cases)

test_rpc_cluster.cpp: 50 of 116 cases. MessageAssembler/WireFragment 65, 116; socket options 25, 7167; direct
RpcClient/RpcServer/NetworkLinks 182, 278, 1483, 1563, 1625, 1672, 1909, 1981, 2068, 2121, 2204, 2282, 2428, 2519,
2564, 2607, 2681, 2765, 2892, 5534, 6564, 6703, 7386, 7443, 7526, 7584, 7621, 7710, 7794, 7852, 7888, 7928, 7994;
Service with injected FaultyLinks 5588, 5660, 5758, 6524; SecureChannel helpers 7218, 7250, 7257, 7273, 7304, 7328,
7348, 7377; queue constants 6869. Also 155 (`test_async_rpc_move_ownership`) constructs AsyncRpc directly.
test_invariants.cpp: 2472 `test_rpc_pre_auth_admission_is_bounded`, 3403
`test_authenticated_receiver_enforces_transport_lane`. Every other test drives Service, TestNode, BareNode,
StoreBench or FakeClusterNode and keeps running through the facade. Helpers: `tests/fake_cluster_node.hpp` (ClusterNode
fake, builds AsyncRpc), `tests/test_support.hpp:384-450` (BareNodeResources with MessageRoutes and NetworkLinks;
BareNode subclasses NodeRuntime).

Header coupling to watch: `rpc_refusal.hpp`, `data_work.hpp`, `filesystem/hydration.hpp` include net.hpp;
`filesystem/filesystem.hpp` includes membership.hpp; `catalogue/cluster_unmatched.hpp`, `transcode_rates.hpp` include
telemetry.hpp; accounts, local_services, storage_server, metadata_server, availability_service include
message_routes.hpp (which includes rpc_refusal.hpp).

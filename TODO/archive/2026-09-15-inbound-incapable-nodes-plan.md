# Plan: nodes that cannot accept inbound connections

Date: 2026-09-15

Status: Stages A, B and C implemented in 0.42.0 (2026-09-15, protocol 21);
docs and tests done; cluster UAT and the retention-drain question open. P0
business feature. Two node properties, both
self-declared and gossiped: `network.inbound_capable` and
`storage.hosts_extents`, each `true | false | auto`, default `auto`.

Two node shapes motivate this, and they usually coincide:

- **The node that cannot be connected to.** It lives somewhere it can
  connect *out* but never *in* -- CGNAT, a corporate NAT, a network where
  exposing a port is not permitted at a system level -- and still
  participates fully: mounts the namespace, plays media, ingests, and (if it
  has storage and the cluster shape allows) hosts extents.
- **The edge node.** It provides the API and media to its own network and
  caches what its viewers watch, but stores no media at all: no
  `storage.data`, no share of the cluster's replicas, nothing to repair. A
  small box in a household or office that makes the library local without
  making it responsible for any of it.

The purpose is **not** to expose such a node's HTTP API to the internet; it
serves API clients on its own network exactly as it does today, and nothing
here changes that.

## What the code actually looks like (verified 2026-09-15)

The transport is closer to this than the backlog assumes. What exists:

- **Every connection is bidirectional, and the client prefers an inbound
  route over dialling.** When `RpcServer` accepts a session it registers it
  with the shared `RpcClient` as an `InboundRoute` keyed by `(peer id,
  lane)` (`src/net.cpp:3737-3780` → `register_inbound` at `:1945`).
  `RpcClient::connection()` (`:1983-2016`) tries the outbound map, then the
  inbound map, and dials only if neither is usable. If node F dials node P,
  P can send *anything* to F over that session -- `exchange()` even says so
  (`src/cluster.cpp:1578-1581`).
- **Health and liveness already work over inbound routes.** `register_inbound`
  populates `endpoints_`/`endpoint_peers_` from the peer's advertised
  endpoint, so `health_loop` (`:2403`) pings F over the inbound route and a
  reply refreshes membership `direct_seen` via `peer_observer_`. The server's
  own observer marks F direct on handshake. So `Membership::all_known_reachable()`
  -- the destructive-GC fence (`src/membership.hpp:45-49`) -- is satisfied
  from P's side by F's outbound connection alone.
- **Lanes are separate TCP sessions using the same machinery.** `lane_for()`
  (`:1807`) sends `get_object`/`put_object`/`put_object_deferred`/
  `object_durability_barrier` on DATA, everything else on CONTROL; the
  handshake carries the lane (`:745-761`). An inbound DATA session registers
  as an inbound route exactly like CONTROL.
- **DATA sessions are dialled on demand and then never closed for being
  idle.** No idle reaper exists. A `PeerConnection` is usable until broken or
  retiring (`:1601`); a `Session` until done/retiring (`:2798`). Teardown
  happens only on socket error, `reconcile_locked` (simultaneous dial,
  `:1920`: the lower NodeId keeps its outbound), `close_endpoint` after a
  `dead_after` liveness failure (drops both lanes), identity reset, or stop.
- **Only the CONTROL lane is probed.** `health_loop` pings `route_key(peer,
  control)`; an idle DATA lane carries nothing. `socket_options` (`:84`)
  sets `SO_KEEPALIVE` at the OS default (2 h on Linux before the first
  probe). Through CGNAT, whose idle-mapping timeout is minutes, an idle DATA
  lane dies silently and the next extent read pays a stall and a redial.
  Today that is a hidden cost on any NAT'd node; for an inbound-incapable
  node the redial can only come from *its* side.
- **Nothing can ask a peer to dial.** `connection()` on P, finding no route
  to F, dials F's advertised endpoint, fails, and enters `health_` backoff
  (250 ms × 2ⁿ, `:1870-1873`), with `bootstrap: connect: Connection refused`
  at DEBUG on every heartbeat (`src/cluster.cpp:1617-1645`).
- **`NodeInfo` has no reachability or hosting bits.** `src/types.hpp:46`;
  fixed-field encoding at `src/codec.cpp:99-123`; carried in the handshake
  hello/ack (`:761`, `:870`) and the `members` reply (`src/cluster.cpp:795`).
  Adding fields is a `protocol_version` bump; peers already refuse a
  mismatch ("wrong cluster/protocol"), so it is a rolling-upgrade event, not
  a compatibility layer.
- **`advertise` is assumed dialable.** `self_info` (`src/cluster.cpp:~100`)
  falls back to the hostname; `server_handshake` substitutes the remote
  address for `0.0.0.0`/`::` (`:851`); `Membership::observe` rejects empty
  host/port (`src/membership.cpp:171`). An inbound-incapable node therefore
  advertises its NAT-side address today and peers dutifully dial it.
- **DATA placement has one choke point.** `DistributedStore::ranked()`
  (`src/distributed_store.cpp:123`) is
  `capacity_placement_nodes(id, membership().active(), replication)`;
  `owners()`, `should_own()`, `retain_data`'s candidate scan, repair
  push/pull, prompt replication and rebalance all go through it.
  `capacity_placement_nodes` (`src/placement.cpp:333`) does **not** exclude
  a zero-capacity node: quota 0 keeps it out of the preferred set, but
  `fallback_score` (`:267`) gives capacity 0 a weight of 1, so it still
  appears in the fallback order and can hold spill copies. "Advertise
  capacity 0" is not an exclusion.
- **Surplus copies are already reclaimed.** Repair's push pass walks every
  local object; for a live object this node does not own it pushes copies
  until `replication` owners hold it, then removes the local copy unless a
  retention claim pins it (`src/distributed_store.cpp:2280-2347`). A node
  that stops being an owner therefore drains itself through ordinary repair.
  "Universal" objects (catalogue control set, `everywhere`) are pushed to
  *every* active node and never removed (`:2283`, `:2340`).
- **`Membership::active()` is gossip-fresh, not direct** (`src/membership.cpp:266`):
  seen within `dead_after` by any path. Placement inputs are therefore the
  same on every node modulo the ordinary gossip window.
- **`validate()` requires at least one `storage.data` backend**
  (`src/config_base.cpp:49-54`). There is no such thing as a storage-less
  node today.
- **`PublicConnectivity`** has UPnP, external-IP lookup and a self-probe
  (`src/public_connectivity.cpp:442`) which, as its own log line says,
  cannot tell "port closed" from "no NAT hairpin". Input to `auto`, not a
  verdict.

## Decisions

1. **Two self-declared node properties, gossiped in `NodeInfo`.**
   `inbound_capable` and `hosts_extents` describe the node's situation and
   travel with it; every peer sees the same bits, so every peer makes the
   same placement and dialling decisions. Neither is a cluster policy
   observers apply locally -- that would break placement determinism (two
   nodes disagreeing about who owns an object is the bug the DHT exists to
   prevent). A cluster-wide override, if ever wanted, would be a metadata
   policy like `data_replication` with a policy-transition commit; not now.
2. **One hosting flag, not two.** The only reason a node keeps an extent in
   `storage.data` is being an owner; cache, spool and control store are
   separate stores. `hosts_extents: false` means "never an owner, never a
   fallback holder, capacity 0, `storage.data` optional" -- the edge node.
   It still has the block cache (`cache.*`, which already serves playback
   and read-ahead: `get_shared` goes local → cache → owners,
   `src/distributed_store.cpp:1605-1650`), the control store
   (`storage.metadata`, which holds the catalogue and metadata replica and
   stays required), the FUSE spool and ingest staging. Its writes go to
   owners over outbound DATA lanes; the hydrator fills its cache from
   viewer and catalogue hints exactly as it does on any non-owner.
3. **No relay.** With extents hosted only on inbound-capable nodes, every
   fetch by anyone is from an owner the fetcher can dial. Two
   inbound-incapable nodes never need a path to each other; metadata already
   reaches both through the capable replicas. A relay would only serve a
   cluster with *no* capable storage node, which is out of scope. (An
   inbound-incapable node *may* host extents -- `hosts_extents: true` -- and
   then its extents are reachable from capable peers only. That is allowed,
   with a warning, because a cluster of one capable hub and one incapable
   storage site is a legitimate shape.)
4. **Both default to `auto`.** `inbound_capable: auto` resolves from
   evidence (below) and re-checks periodically; `hosts_extents: auto` is
   `false` when no `storage.data` backend is configured or `inbound_capable`
   resolves `false`, `true` otherwise. Resolved values are what gossip
   carries; Status shows both the configured mode and the resolved value.
5. **An `auto` resolution is sticky.** It is persisted under `state_path` and
   changes only on strong evidence (a dial-back succeeding after it had been
   failing, or vice versa, across two probe rounds). Flapping would look
   like a node joining and leaving placement every few minutes.
6. **Refuse the impossible cluster at start-up.** A joining node whose every
   bootstrap peer is inbound-incapable, or a founding node that is
   inbound-incapable itself, cannot form a cluster anyone can use; say so
   and exit, rather than half-work.
7. **Kernel keepalive on every transport socket, plus a DATA-lane probe.**
   `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` (60 s / 15 s / 4) keep NAT
   mappings warm at no application cost and benefit every NAT'd node today;
   the health probe extended to the DATA lane is what actually detects a dead
   session and redials before a viewer needs it.

## Design

### Configuration

```yaml
network:
  inbound_capable: auto    # true | false | auto (default)
storage:
  hosts_extents: auto      # true | false | auto (default)
```

Validation (`src/config_base.cpp`, `validate()`):

- `hosts_extents: true` with no `storage.data` backend → error: "storage.hosts_extents is true but storage.data has no backends".
- `hosts_extents: false` with backends configured → warn once at start:
  "storage.data is configured but this node hosts no extents; the backends
  will drain and stay empty".
- `hosts_extents: true` with `inbound_capable: false` → warn: "extents
  hosted here are reachable from inbound-capable peers only".
- `storage.data` may be empty when `hosts_extents` is `false` or `auto`;
  `auto` with no backends resolves `false`.
- `inbound_capable: false` makes `network.advertise`, `upnp`, `external_ip`
  and `connectivity_check` irrelevant; warn if any is set.

`Config` gains `inbound_capable` and `hosts_extents` as a three-state enum
(`Tristate { yes, no, automatic }`), parsed in `parse_network` /
`parse_storage` (`src/config.cpp`).

### A node with no data backends

`storage.data` empty is allowed when `hosts_extents` is `false` or `auto`.
Rather than making `local_store()` optional at its ~40 call sites, such a
node runs an *empty* `StoragePool` -- zero backends, `used() == 0`,
`limit() == 0`. Every existing call behaves as "not present"/"no space":
`has()`/`valid()` false, `get()` empty, `put()` refused with the existing
"storage limit reached", `next_object()` complete at once, `gc_step` and
`rebalance_step` complete at once. `retain_on` for the local node checks
`local_store().has(id)` before accepting a claim (`:816-820`), so an edge
node can never become a retention holder -- correct, since it is never an
owner. `recover_storage` marks `ready_data_storage` immediately and logs
"node data storage ready capacity=0 (hosts no extents)". `validate()` drops
the "at least one storage backend" rule when the node does not host extents
and keeps it otherwise.

Status for such a node: `storage.available: true, capacity_bytes: 0` with
`hosts_extents: false` beside it, so the cluster aggregate and the
"one or more nodes report zero capacity" style conditions do not read it as
data loss. The cache figures are reported as today.

### Wire

`NodeInfo` gains `uint8_t flags` after `metadata_write_replicas_required`:
bit 0 `inbound_capable`, bit 1 `hosts_extents`. `encode_node_info`/
`decode_node_info` (`src/codec.cpp:99-123`) and the known-node roster
(`src/membership.cpp:24-138`, a v3 magic; v1/v2 decode with both bits set,
which is what every existing node is). `protocol_version` bumps.

`MessageType` gains:

- `dial_request` -- a notification (`request_id == 0`, sent over the CONTROL
  inbound route). Payload: the lane the sender wants. The receiver dials that
  lane to the sender's advertised endpoint.
- `dial_back_probe` -- a request. Payload: the sender's advertised endpoint.
  The receiver opens a *fresh* TCP connection to it, completes a handshake
  (which authenticates the target as the sender), closes it, and replies with
  the result and the error text. Never uses an existing route.

### Transport (`src/net.cpp`)

- `RpcClient::connection()`: when the target peer's gossiped
  `inbound_capable` is `false` and no route exists, do not dial. If a CONTROL
  inbound route exists, send `dial_request{lane}` over it and wait on
  `connection_cv_` for the inbound route to appear, bounded by
  `connect_timeout`; the existing in-flight-dial wait (`:2023-2032`) is the
  same shape and the same condition variable. No CONTROL route: fail
  immediately with "peer accepts no inbound connections and is not connected"
  (a transient error, retried by the caller like any dial failure).
- `RpcClient` gains a *lane maintenance* loop for the local node when it is
  inbound-incapable: for every active peer that is inbound-capable, keep a
  CONTROL session dialled (reconnect with the existing `health_` backoff) and
  dial DATA eagerly too. An inbound-incapable node is the only one that can
  restore its own reachability, so it never waits to be asked when it can
  avoid it; `dial_request` is the fallback for the window after a drop.
- `health_loop`: probe the DATA lane as well as CONTROL (a `ping` is a
  control-class message; `class_allowed` (`:387`) already permits `ok` on any
  lane -- add `ping` to the DATA lane's allow-list in `allowed_on_lane`
  (`:104`)). A dead DATA session is closed and, on the incapable side,
  redialled by the maintenance loop.
- `socket_options` (`:84`): set `TCP_KEEPIDLE` 60 s, `TCP_KEEPINTVL` 15 s,
  `TCP_KEEPCNT` 4 (`TCP_KEEPALIVE` on macOS for the idle value).
- `reconcile_locked` (`:1920`): unchanged -- a peer's dial to an incapable
  node never completes, so there is never a pair to reconcile. Guard it
  anyway: never retire the outbound side of an inbound-incapable node.
- `dial_back_probe` handler: a bounded, one-shot dial using
  `connect_socket` + `client_handshake` on a throwaway `SecureChannel`,
  never registered as a route. Rate-limited per peer (one in flight, one per
  10 s) so it cannot be used to make a node hammer an address.

### Membership and node loop (`src/cluster.cpp`, `src/membership.cpp`)

- `NodeRuntime::loop` skips `exchange()` dials to inbound-incapable peers
  that have no route (the `exchange(NodeInfo)` form already reuses an inbound
  route when one exists). No more refused-connection noise for them.
- `self_info`: an inbound-incapable node advertises host/port as it does
  today (peers need *some* key for `endpoint_peers_`), but the flag tells
  them not to dial it. Status labels the endpoint "not dialable".
- `Membership::all_known_reachable()` excludes pairs where *both* this node
  and the known node are inbound-incapable: they can never authenticate
  directly and it is not a fault. Everything else keeps the current rule.
- `Membership` exposes `inbound_capable(id)` / `hosts_extents(id)` from the
  gossiped bits; `self()` carries the resolved local values.

### Placement (`src/distributed_store.cpp`, `src/placement.cpp`)

- `ranked()`: filter `membership().active()` to `hosts_extents` nodes before
  `capacity_placement_nodes`. That single change covers owners, `should_own`,
  retention candidates, repair, prompt replication and rebalance.
- The "everywhere"/universal set (`repair_step` `:2283`, catalogue universal
  objects): restrict to hosting nodes too. A non-hosting node fetches those
  on demand into its cache like anything else.
- `placement_logical_capacity` and the Status capacity aggregate count only
  hosting nodes.
- A hosting node that becomes non-hosting drains through the existing repair
  push (`:2280-2347`); no new mechanism. Its retention claims are released by
  the ordinary release path once the owners hold the copies and the head no
  longer needs it locally -- verify in the test below that
  `retention_store().retained()` does not pin the drained copies forever
  (the push path skips removal for retained objects at `:2342`).
- `min_write_replicas` / `replication` must be satisfiable from hosting
  nodes alone. Status `cluster.conditions` reports "replication N requires N
  extent-hosting nodes; M are known" when it is not, and writes fail with
  the existing "DATA retention placement unavailable" reason.

### `auto` resolution (`src/public_connectivity.cpp`, `NodeRuntime`)

`inbound_capable: auto` resolves in `connectivity_worker_`:

1. Start as `true` if a persisted resolution exists, else `unknown`
   (behave as `true` -- dial and accept -- until evidence arrives).
2. After the first CONTROL session to any inbound-capable peer is up, send
   `dial_back_probe` to that peer with the advertised endpoint. Two
   consecutive failures from a peer that itself is reachable → `false`. A
   success → `true`.
3. Re-probe every 10 minutes while `false` (a port-forward may have been
   added) and every hour while `true` (it may have been removed).
4. Persist the resolution under `state_path/connectivity/inbound.bin`; on
   change, log at INFO with the peer that decided it and update the
   advertised flags (`members_.endpoint`-style, `server_.set_local`).

`hosts_extents: auto` follows: `false` when `storage.data` is empty or the
resolved `inbound_capable` is `false`; else `true`. A resolution change
re-gossips the flag; placement moves as for a join/leave.

### Status (`src/status_api.cpp`)

- `nodes[]`: `inbound_capable` and `hosts_extents` (resolved) plus
  `inbound_capable_mode` / `hosts_extents_mode` (configured) on the local
  node; `endpoint.dialable: false` for incapable peers.
- `connectivity` block: `inbound_capable` with `source` (`configured`,
  `probe:<peer>`, `persisted`) and `last_probe_unix_ms`.
- `cluster.conditions`: "N node(s) accept no inbound connections", "no
  inbound-capable node hosts extents" (fatal), "replication N requires N
  extent-hosting nodes; M known".
- DEBUG line for each `dial_request` round trip and each dial-back probe.

## Stages

### Stage A -- transport and flags

- [x] `Tristate` config type; `network.inbound_capable`, `storage.hosts_extents`;
  validation and warnings above; `storage.data` optional when not hosting;
  the empty `StoragePool` and its readiness/Status treatment.
- [x] `NodeInfo.flags`, codec, roster v3, `protocol_version` bump.
- [x] Kernel keepalive on transport sockets; DATA-lane health probe.
- [x] `dial_request` notification; `connection()` waits for the reverse
  dial instead of dialling an incapable peer; lane maintenance loop on an
  incapable node.
- [x] `NodeRuntime::loop` and `health_loop` stop dialling incapable peers.
- [x] `all_known_reachable()` pairwise exclusion; start-up refusal of the
  impossible cluster.
- [x] Tests (`tests/test_rpc_cluster.cpp`): a `TestNode` that advertises a
  black-hole address (192.0.2.1, TEST-NET-1) with a short `connect_timeout`
  and `inbound_capable: false` -- no OS firewall needed. Cases: control
  requests flow both ways over the reverse route; a peer's `get_object` to
  the incapable node triggers a dial request and completes; a dropped DATA
  session is redialled without a request; two incapable nodes see each other
  in `members` and neither counts the other in the GC fence; a peer never
  attempts a TCP dial to an incapable node (assert `connections_created`
  unchanged).

### Stage B -- hosting

- [x] `ranked()` and the universal set filter on `hosts_extents`; capacity
  aggregates; write-floor condition.
- [x] Tests (`tests/test_storage_v18.cpp`; retention-claim release still unproven): an
  edge node (no `storage.data`) starts, joins, serves the catalogue API,
  plays media through its cache and publishes a FUSE write to the owners; a
  non-hosting node is never an owner or fallback for any key; a node
  flipping hosting→non-hosting drains through repair and its retention
  claims release; FUSE publication and ingest from a storage-less node land
  on owners; `replication` unsatisfiable from hosting nodes is reported and
  refused.

### Stage C -- `auto`

- [x] `dial_back_probe`, resolution state machine, persistence, re-probe
  cadence, Status fields.
- [x] Tests: a black-hole-advertised node in `auto` resolves `false` after
  two failed probes and its `hosts_extents: auto` follows; a node whose
  advertised address becomes dialable resolves back to `true`; the
  resolution survives restart.

### Docs

- [x] `docs/configuration.md` Network and Storage sections; `docs/operations.md`
  gets a "Nodes behind CGNAT" section (what works, what the shape rules are,
  how to read Status); `macha.yaml.example`; CHANGELOG.

### UAT (cluster)

fi-1 is the natural subject: leave the WireGuard tunnel up for the API and
SSH, but point Macha at the peers' public endpoints so its own RPC port is
genuinely unreachable. Expect: `auto` resolves `false` within two probe
rounds; the mount, playback and ingest work; gbni-1/es-1 never log a refused
dial to it; `all_known_reachable` holds on all three; a 20-minute idle
period followed by a read from fi-1 does not stall (keepalive + DATA probe).

## Risks and open questions

- **Placement churn on `auto` flips.** A resolution change is a join/leave
  for placement. Sticky resolution and the two-round rule bound it; Status
  must show the last change so an operator can tell.
- **Retention claims on a draining node.** The push path skips removal for
  retained objects; if the release clock never dominates a claim on a node
  that no longer mutates, the copy stays. The Stage B drain test decides
  whether the release path needs to treat a non-hosting node's claims
  specially (release once owners hold the object, regardless of clock).
- **`dial_request` storms.** A peer that repeatedly loses a lane could make
  the incapable node redial in a loop. The lane maintenance loop owns the
  backoff; `dial_request` only wakes it, never bypasses it.
- **Protocol bump.** Every node in a cluster must upgrade together; the
  rolling-upgrade note in `docs/operations.md` already says so for protocol
  bumps.
- **Two incapable nodes that both host extents** (`hosts_extents: true`
  forced on both) cannot repair each other's objects. Allowed with a warning
  because each still has capable owners; the warning names the peer.

## Sizing

Stage A is the bulk: `net.cpp` changes (dial request, reverse-dial wait,
lane maintenance, DATA probe, keepalive) plus codec/membership/loop edits
and their tests. Stage B is small once the flag is gossiped: one filter and
the aggregates, plus the drain test that answers the retention question.
Stage C is moderate: a probe message, a small state machine, persistence,
Status. Docs and UAT as usual. Comparable to the FUSE-supervisor work
overall; Stage A alone is shippable and already removes the refused-dial
noise for any NAT'd node.

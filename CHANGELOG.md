# Changelog

## 0.4.0 - 2026-08-14

- renamed the project to Macha across the executable, build targets, configuration, service files, C++ namespace, FUSE identity and local storage markers;
- changed cluster cryptographic derivation labels and metadata-placement namespace to `macha/*`;
- bumped the wire protocol to v4, changed the frame magic to `MCH4`, and changed the authenticated session label to `macha/session/v4`;
- intentionally removed compatibility with pre-Macha nodes, state directories, backend markers, wire protocol, encrypted storage and service/configuration names; 0.4.0 is a clean compatibility boundary.

## 0.3.0 - 2026-08-13

- added yaml-cpp based YAML node configuration, including multiple storage backends, persistent cache and adaptive-maintenance policy;
- separated durable node identity/metadata state from bulk storage backends;
- added `StoragePool` with per-disk identities, aggregate capacity, live add/remove/return detection and deterministic local rebalance;
- added a persistent encrypted non-DHT block cache bounded by block count, with LRU-like eviction and separately retained current metadata;
- added offline read-only metadata fallback to the last valid persisted snapshot;
- made joining nodes proactively pull live objects for which they become DHT owners;
- made foreground remote reads cache their already-fetched bytes and promote them asynchronously when the reading node is also an owner, avoiding a second network transfer;
- replaced the fixed block-count repair cadence with byte-budgeted adaptive maintenance driven by observed bandwidth, foreground activity and CPU load;
- gave network repair, local rebalance and scrub separate budgets, with local rebalance/scrub suppressed during foreground activity;
- added SIGHUP reload for storage-backend/cache configuration and updated systemd/launchd examples;
- fixed Apple libc++ compilation of `std::deque<RpcServer::RequestJob>` by making `RequestJob` complete at declaration, and fixed the duplicate `yes` socket-option declaration on macOS;
- expanded tests for multi-disk operation, disk disappearance/return, persistent cache behaviour, playback-assisted promotion and automatic new-node convergence;
- replaced RPC wall-clock completion deadlines with progress-observed, peer-liveness-based transport semantics: slow healthy RPCs remain valid indefinitely, while `dead_after` governs actual peer death;
- split peer transport into independent health/control/data lanes and server execution queues so bulk object transfers cannot starve membership/metadata traffic;
- made each health/control/data session bidirectional and canonical by authenticated node identity and lane; lane identity is negotiated in the authenticated handshake, simultaneous cross-dial and endpoint aliases converge on one physical connection per peer/lane, and in-flight work drains before duplicate shutdown;
- bumped the wire protocol to v3 and removed v2 handshake/session compatibility;
- removed automatic `current.meta`-only metadata-state upgrading; existing state now requires both working and committed checkpoints;
- made socket shutdown wake blocked I/O without closing/recycling the descriptor underneath active transport threads;
- removed pre-v3 configuration aliases and the legacy single-backend CLI/state-path fallback; YAML configuration is now required and old option names are rejected.
- added conventional ALL/DEBUG/INFO/WARN/ERROR logging, config-driven log level, and INFO connection lifecycle messages.
- fixed FUSE open-write size reporting so macOS does not zero-fill and replay dirty ranges against a file still advertised as size zero.
- added durable committed metadata checkpoints on every active node and automatic voter/namespace recovery when a destroyed node is replaced with a fresh identity.
- extended replacement recovery tests through namespace restoration, object repopulation, byte verification and subsequent metadata mutation.

## 0.2.0 - 2026-08-13

WAN-hardening release:

- replaced one-RPC-per-TCP transport with persistent multiplexed peer sessions;
- added v2 request IDs, out-of-order response handling and unsolicited metadata-generation notices;
- added ephemeral X25519 with authenticated transcript/HKDF session derivation for forward secrecy;
- fixed an AES-GCM empty-payload framing bug exposed by persistent sessions;
- made metadata voter reads, CAS writes, repair and genesis discovery quorum-parallel;
- added bounded generation-aware read-only metadata caching; mutations always perform a fresh quorum read;
- made foreground object writes return as soon as durability quorum is reached while background repair restores full replication;
- added adaptive peer retry backoff and latency-derived control RPC deadlines while retaining bandwidth-appropriate data timeouts;
- added explicit failure-domain labels and deterministic HRW placement that prefers replica diversity across domains;
- added WAN transport/configuration options and documented the direct-reachability/NAT constraint;
- expanded tests for X25519, v2 framing, persistent connection reuse, true multiplexing, adaptive backoff, metadata cache invalidation, topology diversity and early replication quorum;
- bumped wire protocol to v2; mixed 0.1/0.2 peers are rejected.

## 0.1.0 - 2026-08-13

Initial complete minimal implementation:

- authenticated encrypted cluster transport;
- gossip membership and deterministic DHT-style placement;
- encrypted content-addressed extent store;
- configurable data replication, full-node fallback placement and throttled repair/rebalance;
- quorum-replicated metadata with founder-safe genesis, voter replacement and no permanent authority;
- media-oriented sequential ingest, cached range reads and multi-extent read-ahead;
- explicit metadata garbage tombstones with a conservative collection grace;
- crash-durable node identity and exclusive storage-path locking;
- FUSE3/macFUSE adapter;
- Linux systemd and macOS launchd examples;
- CMake/CTest build and multi-node integration/fault-injection tests.

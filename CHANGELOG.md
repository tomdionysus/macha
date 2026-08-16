# Changelog

## 0.7.1 - 2026-08-16

- split peer transport into two canonical authenticated bidirectional TCP lanes: CONTROL for health/membership/metadata and DATA for object transfer traffic. Each lane is independently deduplicated by NodeId, isolating control-plane latency from TCP head-of-line blocking and congestion on bulk transfers.
- bumped the wire protocol to v8 (`MCH8`, `macha/session/v8`). v7 peers are intentionally rejected because the authenticated handshake now identifies the transport lane.
- made DATA connections lazy: cluster formation needs only CONTROL; a DATA lane is established on the first object transfer and then reused bidirectionally.
- kept transfer promotion/cancellation on the DATA lane because transfer request IDs are scoped to that connection.
- made LocalStore capacity accounting asynchronous so large object stores no longer block process startup before the backend can come online. The initial scan serialises writes/removes until accounting is reconciled while reads remain immediately available.
- retained the 0.7.0 maintenance pre-emption, bounded repair probes, in-flight transfer cancellation/promotion and route-recovery fixes developed during streaming stress testing.

## 0.7.0 - 2026-08-15

- added negotiated playback sessions with direct play, fragmented-MP4 HLS remux and selective transcoding;
- added a media-engine abstraction with an in-process libav implementation; probing, demuxing, decoding, encoding and muxing no longer shell out to `ffprobe`/`ffmpeg`;
- added custom seekable libav `AVIOContext` input backed directly by pinned Macha `ReadHandle` objects, removing the loopback HTTP source and preserving DHT/playback-hydrator semantics;
- added direct fragmented-MP4 publication through an in-process bounded `MediaSegmentStore`, removing generated-playlist filesystem polling and signal-based FFmpeg back-pressure;
- added media probe caching, multi-representation selection, validated audio/subtitle track selection, WebVTT subtitle extraction, quality limits and seek-driven pipeline replacement; path-based probes are versioned and active playback pins the resolved file snapshot;
- added explicit probe, pipeline-analysis and first-fragment startup deadlines, propagated per-caller probe cancellation through filesystem reads into independent DHT object RPCs, and added stage-specific playback 503 bodies/trace IDs for correlating server failures with client logs;
- removed two synchronous/startup CPU traps: media-ID lookup is now cached by metadata generation instead of re-hashing the complete namespace for each playback candidate, and probe/subtitle reads no longer register as active playback or trigger hydration;
- added capability-scoped stream URLs so native players can fetch media without receiving the permanent catalogue API Bearer token;
- split the HTTP server from catalogue routing and replaced the single-request server with a bounded concurrent worker server supporting streaming response bodies, HTTP byte ranges and simultaneous HLS fragment requests;
- added hard concurrent playback-session, video-transcode and audio-transcode admission limits with pending-pipeline reservations, plus idle expiry and generated-fragment cleanup;
- fixed settled-node maintenance CPU spin: no-progress repair/rebalance/scrub passes now clear accumulated byte credit and back off, catalogue repair is throttled, full filesystem live-object enumeration no longer runs every 500 ms when there is no work, and catalogue-sequence hydration no longer snapshots/repairs the catalogue when there is no active playback;
- fixed remux timestamp handling after seek: stream-copy PTS/DTS are now repaired after conversion to the muxer's final timebase, missing timestamps are synthesised, equal/backwards DTS values receive a persistent monotonic correction, and repairs are logged per stream instead of allowing the MP4 muxer to abort playback;
- added optional `seek_ms` to session creation so resumed transformed playback can create its first HLS generation at the requested position instead of requiring an immediate create-then-PATCH pipeline restart;
- added DEBUG playlist/fragment request logging for correlating server generation/segment progress with client buffering diagnostics;
- made CMake discover Homebrew's keg-only `ffmpeg@7`/`ffmpeg` pkg-config prefix automatically on macOS;
- added regression coverage for direct byte ranges, HLS session generation, initial transformed seek, remux timestamp repair, session reconfiguration, subtitle output, transcode limits, concurrent HTTP serving and segment-store back-pressure/spill behaviour;
- reduced `README.md` to an overview and moved operational documentation into topic files under `docs/`, including a two-node `quickstart.md` and streaming API guide.

## 0.6.2 - 2026-08-15

- added a per-node metadata mutation sequence clock (snapshot format 7) so quorum-CAS retries recognise an operation that became canonical, even after other writers committed descendants of it; this fixes the symmetric two-voter `EEXIST` race without making filesystem operations artificially idempotent;

- replaced equal-node media extent placement with a virtual 32-bit (2^32) stable capacity-weighted placement-shard space;
- computed exact per-node shard-slot quotas using the distinct-replica capacity constraint, avoiding the severe small-node over-selection of naive weighted sampling for replica counts greater than one;
- retained `failure_domain` diversity by placing across capacity-weighted failure-domain aggregates before selecting a node within each chosen domain;
- made deterministic fallback ordering capacity-aware while keeping current free space out of placement weights;
- applied capacity-weighted rendezvous over the stable shard space to local storage backends, so adding a disk gives it a proportional authoritative share while only moving shards won by the new backend;
- kept previously adopted configured backends in placement capacity while temporarily offline, preventing transient disk loss from redefining cluster and local ownership; permanent configuration removal drops the capacity;
- corrected `statfs` logical capacity for heterogeneous nodes using the maximum capacity supportable by `R` distinct replicas rather than `sum(capacity)/R` alone;
- bumped the wire protocol to v7 (`MCH7`, `macha/session/v7`) because v6 and v7 nodes calculate different data owners; mixed versions are intentionally rejected and no placement migration compatibility is provided.

## 0.6.1 - 2026-08-15

- added automatic distributed-filesystem catalogue scanning for movies, TV episodes and music, with conservative filename/path recognition and stable media-ID binding;
- added pluggable metadata-provider interfaces, using TMDB for movies/TV and MusicBrainz for artists/albums/tracks;
- added online artwork acquisition: movie posters/backdrops, show posters/backdrops, season posters, episode stills and Cover Art Archive album covers are downloaded into Macha's immutable object store and replicated with catalogue metadata;
- made the lowest active node ID the scanner coordinator so normally configured clusters perform one set of external lookups, with deterministic failover when membership changes;
- made scans idempotent for already-bound media, prune only scanner-owned records when files disappear, and abort deletion reconciliation if a configured root cannot be read;
- added MusicBrainz request pacing and release/cover caching, TMDB show/season caching, multi-disc music path recognition and conservative provider-match thresholds;
- fixed catalogue `external_ids` decoding to read key/value fields in defined order; the previous expression depended on function-argument evaluation order and could reverse provider/id pairs after a snapshot reload;
- added scanner/provider/artwork/reconciliation and catalogue-codec regression coverage; wire protocol remains v6.

## 0.6.0 - 2026-08-15

- replaced protocol v5 with v6 (`MCH6`, `macha/session/v6`); mixed-version peers are intentionally rejected and no compatibility shim is retained;
- collapsed health, control and data lanes into one canonical authenticated bidirectional TCP connection per peer;
- added variable-length independently authenticated transport frames with negotiated `network.max_frame_size` (default 256 KiB, allowed 4 KiB..4 MiB), independent of storage extent size;
- made frame type the sole wire priority authority: control > foreground > read-ahead > speculative, with no independent numeric priority field;
- made outbound scheduling pre-emptive at frame boundaries by selecting the highest-priority runnable transfer after every frame;
- moved heartbeat/liveness and protocol control onto the same highest-priority stream used by real peer traffic;
- added transfer promotion and cancellation control messages so a speculative/read-ahead object already in flight can become foreground without changing request identity;
- tagged filesystem retrieval as foreground, current-file/read-ahead hydration as read-ahead, and catalogue prediction as speculative; coalesced object retrieval propagates promotions to the live transport;
- kept canonical cross-dial deduplication and graceful duplicate draining under the unified connection model;
- added v6 regression coverage for single-connection reuse, frame-type ordering, variable-length framing, foreground pre-emption and frame-size configuration.

## 0.5.2 - 2026-08-15

- added a shared replica selector below filesystem reads and cache hydration;
- spread independent extent fetches across suitable replicas using current in-flight load, recent transfer latency and failure history instead of pinning reads to the first owner;
- kept foreground reads latency-oriented while speculative hydration yields aggressively to peers carrying foreground work;
- coalesced concurrent requests for the same object so foreground playback can promote an in-flight speculative fetch instead of downloading the extent twice;
- added bounded concurrent hydration with `hydration.max_inflight` (default 4), preserving ordered file prefixes and weighted-fair interleaving while allowing several replicas to contribute bandwidth at once;
- added selector and concurrent-hydration regression tests, including striping, latency preference, foreground/speculative accounting, failure penalties and the configured in-flight bound;
- kept wire protocol v5 unchanged; 0.5.2 is an internal scheduling/retrieval change and remains wire-compatible with 0.5.1.

## 0.5.1 - 2026-08-14

- added a priority-based cache hydrator with pluggable hint-provider interfaces and weighted-fair interleaving of ordered file runs;
- reimplemented sequential read-ahead as hydration hints instead of per-read-handle asynchronous futures;
- added whole-current-file prediction at lower priority than the immediate read-ahead window;
- added catalogue prediction for the next TV episode, including season transitions, and the next movie in a collection;
- added stable content-manifest media IDs so catalogue prediction survives file renames, with path bindings retained as a fallback;
- made speculative hydration populate only the persistent non-DHT cache and never authoritative replica placement;
- added configurable hydration engine enablement, priorities, cadence, activity timeout and catalogue lookahead;
- added scheduler, prediction and two-node cache-fetch tests, including ordered-prefix and non-starvation behaviour.

## 0.5.0 - 2026-08-14

- added a quorum-rooted distributed media catalogue for movies, shows, seasons, episodes, artists, albums and tracks;
- made the complete current catalogue and all referenced artwork universal metadata objects held on every active node, with joiner synchronisation ahead of ordinary media repair;
- added local catalogue browse/search and optimistic revision-based mutation;
- added an optional HTTP/JSON API for catalogue status, list/search, item mutation and artwork upload/retrieval;
- made superseded catalogue snapshots and orphaned artwork use persistent metadata garbage tombstones so every node, including later rejoiners, converges deletion after the configured grace period;
- added `maintenance.garbage_grace_ms` and `catalogue.api` YAML configuration;
- kept 0.4.0 metadata snapshots readable, defaulting their absent catalogue root to an empty catalogue;
- bumped the wire protocol to v5 (`MCH5`, `macha/session/v5`) and reject v4-and-earlier peers.

## 0.4.0 - 2026-08-14

- renamed the project to Macha across the executable, build targets, configuration, service files, C++ namespace, FUSE identity and local storage markers;
- changed cluster cryptographic derivation labels and metadata-placement namespace to `macha/*`;
- bumped the wire protocol to v4, changed the frame magic to `MCH4`, and changed the authenticated session label to `macha/session/v4`;
- intentionally removed compatibility with pre-Macha nodes, state directories, backend markers, wire protocol, encrypted storage and service/configuration names; 0.4.0 is a clean compatibility boundary;
- fixed metadata genesis for symmetric bootstrap and metadata voter counts greater than one;
- made data and metadata replica counts mutable by coordinated whole-cluster restart, with old-quorum voter-set transitions and automatic object convergence; `extent_size` remains immutable for an existing namespace.

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

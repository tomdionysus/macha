# Macha: the stack duplication audit (2026-11-20)

The maintainer, relayed by the coordinating session, which correlates the eight reports: "Any functionality that is required by two
or more projects that share a project dependency, that functionality goes into the shared dependency." Read-only; **nothing
moves until the maintainer decides.** Read on `port/dannan-dagda` at `92c7d3d` against Dannan 0.9.2 (tag; its tree is on
`feature/advert-push`), Dagda 0.3.2 (and its develop, 13 past the tag), Lugh, Louhi, Kvasir, Mimir; Lir has no code
yet on its checkout. Line counts are approximate.

## 1. Macha's dependencies in the stack

- **Dannan 0.9.2**, a linked library (`find_package(dannan 0.9.2)`): transport, membership, identity and custody, work
  classes, retained memory ledger, time source base, telemetry, node events.
- **Dagda 0.3.2**, a linked library (`find_package(dagda 0.3.2)`): the store node, objects, tickets and durability,
  claims, ledger and GC, repair, availability survey. Not its heads, tree, retirement or lease modules.
- Nothing over the network from another project. Macha's sign-on is its own (see 3e).

## 2. What Macha owns that others use or could use

- The engineering laws and disciplines (`docs/principles-and-laws.md`), copied into each project.
- The FUSE frontend (`src/fuse/`): journalled spool, group commit, publication pipeline. Its group commit
  (`fuse_frontend.cpp` `DurabilityTicket`) is a candidate for Dannan beside Dagda's `DurabilityDomain`.
- The media catalogue, playback and transcode, ingest, torrent, books: Macha's own purpose; no other project needs them.
- The short-file rules (`b0b037c`): the view never drops this node's own commit, the sole usable head displaces a
  set-aside head, adoption forward-only while a fold is pending. Dagda mirrors them in Space after Macha's release.

## 3. Duplication, and where each belongs

**a. Dannan's foundations kept as Macha copies** (the port's unfinished part; Dagda's `docs/macha-foundations-map.md`
names each home). All belong in Dannan, which already has them:
- thread-safety macros and lock wrappers `src/contract/thread_safety.hpp` (211) vs `dannan/contract/thread_safety.h`
- logging `src/log.*` (221) vs `dannan/status/log.h`; the bridge runs the wrong way (Dannan's log into Macha's)
- crypto primitives `src/crypto.*` (~250; SHA-256, HMAC, HKDF, AES-GCM, X25519, random) vs `dannan/identity/crypto.h`;
  `ClusterKeys` without secure memory vs `identity/cluster_key.h`, `secure_memory.h`
- byte codec `src/codec.*` (195) vs `dannan/net/codec.h`
- durable files `src/durable_file.*` (144) vs `dannan/identity/durable_file.h` (Macha already calls both)
- time source `src/contract/time_source.hpp` (38), `Published<T>` (54), walk budget `contract/walk.hpp` (122),
  `RetryPolicy` `src/retry_policy.hpp` (113), `WorkContext`/`WaitGuard` `contract/work.*` (~150): each in Dannan
- supervision `src/supervised.*` (263) and the subsystem supervisor `src/subsystem/*` (~580) vs `dannan/supervise/`
- observations `src/observation.*` (591) and process diagnostics `src/diagnostics.*` (~260) vs `dannan/status/`
- placement `src/cluster/placement.*` (455; only `order_commit_replicas` is live, the rest tests only) vs
  `dannan/placement/placement.h` and Dagda's
- core message numbers and names re-declared in `src/cluster/net.hpp`, `message.cpp` vs `dannan/net/routes.h`
- test fixtures (`SteppedTime`, `TempDir`, `free_port`) vs `dannan/test/fixtures/`
- YAML config parsing (`src/config*.cpp`, yaml-cpp directly) vs `dannan/config` (Lugh uses it; the schema stays Macha's)

**b. Dagda's modules kept as Macha copies.** All belong in Dagda, which has them, generalised from Macha:
- **the head space** (largest): `src/metadata/metadata.cpp` `MetadataReplica` (~2,400) and codecs, `metadata_manager.*`
  (~2,400), `metadata_server.*`, `metadata_seed.*`, `service/convergence_demand.hpp`, `MetadataDot`/`clock_covers` vs
  `dagda/heads/` (`replica`, `record`, `space`, `exchange`, `convergence_demand`). Macha keeps its codec and merge
  through Dagda's `Codec`/`Merge` interfaces. Not ready to switch: Space lacks `b0b037c` (Dagda's Next 4); a heads
  migration is needed (`dagda/migrate/heads_metadata.cpp`); merge equivalence hash for hash is open in Dagda.
- **the namespace tree**: `src/metadata/namespace_tree.*` (~2,400) and `namespace_control_store.*` vs
  `dagda/heads/tree.*` and `control_node_store.*`; Dagda checks its tree against Macha's 25 vectors. Largely ready.
- **tombstone/garbage batches**: `metadata.cpp:564-668`, `ledger/macha_reachability.cpp:169-227` vs
  `dagda/heads/retirement.h`.
- **leases**: the torrent coordinator's claim, takeover and epoch fence (`torrent/torrent_request.*`,
  `torrent_coordinator.cpp`, ~200) vs `dagda/lease/` (`Lease`, `LeaseKeeper`, `EpochFence`).
- **write-behind**: `src/write_behind.*` is the same text as `dagda/store/write_behind.*`.
- **dead code**: `src/cluster/replica_selector.*` (197) duplicates Dagda's selector, with no user outside tests.

**c. Shared with peers, owned by none yet.** Belongs in Dannan:
- **HTTP**: Macha (`src/http/`, ~1,900, its own poll reactor, no TLS), Lugh and Louhi each have a server, router,
  JSON body helpers and error format, and the three error shapes already differ. Only Dannan's listener is shared.
  Louhi's `api/http.h:31` says "HTTP is Louhi's own, as the stack settled", and Dannan's README scope test is "every
  hosted system needs it, in the same shape". **A ruling is needed before HTTP can move.**
- **JSON**: Macha's own `Json` (`src/json.*`, 559) vs Boost.JSON helpers in Lugh and Louhi.
- **health and status endpoints**: Macha `/api/v1/health`, `/api/v1/status`; Lugh `/health`, `/ready`.
- **rate limiting and sign-in back-off**: Macha's per-user lockout (`src/api/session_api.*`) and hand-rolled 429s vs
  `dannan::listen::Backoff` (shared by Lugh and Kvasir) and Lugh's `RateLimiter`.
  Macha's lockout is keyed by account name only, with no bound: an unknown name reaches `end_check` and adds an entry
  that only a successful sign-in removes, so made-up names grow `failures_` without limit (a Macha defect, found
  2026-11-20). It goes to Dannan's keyed map beside `listen::Bucket`, or is fixed in Macha first if that is far off.
- **password hashing**: Macha's scrypt (`src/auth/users.hpp`) and `password_work.hpp` vs Dannan's Argon2id (Lugh, Kvasir).
- **last-writer-wins records with tombstones**: Macha's `UserRecord`, `AuthSession` vs Lugh's `merge::winner`;
  belongs in Dagda (whether Dagda has a generic one: unverified).

**d. Generic infrastructure only Macha has today** (candidates for Dannan if a second project needs them): RPC routes
with handlers and drain (`src/cluster/message_routes.hpp`) and `wait_for_reply` with its no-progress deadline;
`DataWorkContext`; the allocator setup; the ad-hoc worker pools in ingest, books, filesystem, hydration and FUSE vs
`dannan/sched/worker_pool.h`; the membership fallback's own liveness table vs Dannan's.

**e. Macha doing Lugh's job.** `src/auth/` (accounts, users, sessions, root recovery key), `src/api/session_api.*`,
`users_api.*` and `tools/users_admin.cpp` are a complete identity system: gossiped accounts with roles, bearer
sessions, password sign-in, an anonymous session. That is Lugh's role; Louhi already signs in through Lugh's client.
Under the rule, Macha becomes a relying party of Lugh (OIDC), and its password and session code goes.

## 4. Things Macha depends on that sit in the wrong project

- None found below Macha. Two notes: Dannan's README scope test ("every hosted system, same shape") reads stricter than
  The maintainer's rule ("two or more projects sharing a dependency"), and the HTTP line in Louhi records an earlier settlement.
  The correlator should put that difference to the maintainer.

## What it means for Macha

The work is in three parts, each a change the maintainer orders:
1. Finish the port: replace each copy in 3a and 3b with the library's, delete the dead code (3b last item).
   Mechanical but wide: about 11,000 lines go.
2. The head space onto Dagda's heads (3b first item) once Space has `b0b037c` and the heads migration exists:
   the largest single change, and it needs its own cutover step on the live nodes.
3. HTTP, JSON and sign-on (3c, 3e): wait on the maintainer's HTTP ruling and on Lugh as Macha's identity provider; the client
   API stays 0.94.2's, so sign-on changes reach every client and are announced first.

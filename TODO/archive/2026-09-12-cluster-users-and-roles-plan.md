# Plan: cluster-replicated users, passwords and roles

Date: 2026-09-12

Subsumes the P0 "No authorization tiers yet, though the seam now exists" item
in `ACTIVE.md`. Design decided in discussion; the two properties the design
is built around, in the operator's words:

> it all needs to be asynchronous, and we need to be able to authenticate
> users AND generate sessions even if the node is temporarily alone.

## What already exists, and what it commits us to

0.24.0 built the session half of this with the extension in mind:

- `AuthSession` (`src/session.hpp:22`) already carries a `roles` vector, an
  LWW `version`, and a `revoked` flag; `SessionManager::apply` merges LWW with
  revoked winning ties (`src/session.cpp:165-182`).
- Every node holds the full session replica; `validate()` is one shared-lock
  map lookup (`src/session.hpp:52`). `session.cpp` includes only `codec`,
  `crypto`, `durable_file` and `log` — no metadata, no catalogue.
- Session creation is exempt from the readiness gate by design:
  `src/service.cpp:349` ("must be exempt regardless of local readiness") and
  `src/cluster.hpp:84-86` (control plane is constructed before any storage or
  metadata backend).
- `CredentialValidator` (`src/session_api.hpp:17`) is an explicit seam;
  `AnonymousCredentialValidator` refuses any non-empty credentials.
- `session_has_role()` exists and has zero production callers.

So the auth path is already independent of `MetadataAvailability` — a node
whose metadata is `read_only` or `unavailable` (`src/metadata_manager.hpp:33`)
still mints sessions today. The user store must be built to the same rule:
**it depends on the control plane and the local disk, and on nothing else.**
Putting users in the metadata layer was rejected precisely because it would
couple login to the subsystem most likely to be sick when you need to log in.

## Three defects in the session path that the plan fixes on the way

1. **Login blocks on synchronous peer RPC.** `propagate_session`
   (`src/cluster.cpp:1387-1402`) loops over `members_.active()` serially and
   each `call()` runs to `control_no_progress_deadline` = 30 s
   (`src/config.hpp:654`). A peer marked active but not answering — gbni-2 on
   flaky wifi — stalls `POST /api/v1/session` for up to 30 s per such peer,
   and `SessionApi::handle` calls it before responding
   (`src/session_api.cpp:70`). The local write has already happened by then
   and the gossip tick (`src/cluster.cpp:1327-1336`) is the documented
   backstop, so the response never needed to wait.
2. **Session persistence is idle-deferred** — `sessions().persist()` runs
   only after 30 s with no foreground/read-ahead activity
   (`src/status_api.cpp:379-384`). Right for observational state; wrong for a
   credential store. Users get write-through persistence and do not share
   this policy.
3. **The gossip backstop carries a window, not a table** —
   `sessions_.recent(gossip_ttl, 64)` (`src/cluster.cpp:772`). A node offline
   longer than `gossip_ttl` never learns what it missed. Acceptable for
   sessions (a client re-mints); never acceptable for users.

A further consequence of (1) worth naming: today a token minted on gbni-1 is
invalid on gbni-2 until it propagates. With a replicated user table gbni-2 can
verify the password against its own replica and mint its own session, so
login stops depending on reaching the node that first authenticated you.

## Invariants the design establishes

> **Login is local.** Verifying a password and minting a session read and
> write only this node's memory and its own `state_path`. No RPC is awaited
> in the request path. An isolated node authenticates every known user.

> **User state converges without loss or resurrection.** Every node holds
> the full user table including tombstones; merge is deterministic and
> commutative; no record is ever evicted to make room; a deleted user cannot
> come back from a stale replica.

> **A credential change invalidates every session it minted, everywhere,
> by replicating one fact** — the user record — rather than by enumerating
> sessions.

## Design

### The record

`src/users.hpp` / `src/users.cpp`, new. Deliberately tiny, per the same
"small replicated payload" discipline as `AuthSession` and `NodeInfo`.

```
struct UserRecord {
    std::string id;                    // opaque, 16 random bytes hex; stable across renames
    std::string username;              // unique, NFC + lowercased at create; <= 64 bytes
    uint8_t     kdf{1};                // 1 = scrypt
    std::array<uint8_t,16> salt{};
    uint32_t    kdf_n{}, kdf_r{}, kdf_p{};   // stored, so parameters can be raised later
    Hash256     password_hash{};       // 32 bytes of scrypt output
    std::vector<std::string> roles;    // explicit full set, see "Roles"
    uint64_t    credential_generation{}; // bumped on password change and on delete
    uint64_t    created_unix_ms{}, updated_unix_ms{};
    uint64_t    version{};             // LWW counter
    NodeId      updated_by{};          // deterministic tie-break
    bool        tombstone{};
};
```

Password hashing is scrypt via `EVP_PBE_scrypt` — already in the OpenSSL
Macha links (`CMakeLists.txt:131`; `<openssl/evp.h>` is included in
`crypto.cpp:8`). Argon2id needs OpenSSL 3.2, which the Debian nodes do not
have. Parameters N=2^15, r=8, p=1 (~32 MiB, ~50–100 ms on a Pi 4) as the
initial default, stored per-record. Comparison through the existing
`constant_time_equal`.

### The store

`UserStore` mirrors `SessionManager`'s shape (shared_mutex, map, `apply()`),
and differs from it in exactly the places the discussion identified:

- **Merge:** higher `version` wins. Equal version: tombstone wins; both
  tombstones or neither, higher `updated_by` wins. Deterministic, so two
  partitioned replicas converge to the same record rather than flapping.
- **No eviction.** `max_users{4096}` is a corruption guard enforced at
  `create()`; `apply()` refuses beyond it at `Log::warn`, never `debug`, and
  never drops an existing record to admit a new one. Tombstones are retained
  indefinitely — they are ~100 bytes and users are rarely deleted; retention
  bounded by age is exactly the resurrection bug this store must not have.
- **Write-through persistence.** Every local mutation and every remote
  `apply()` that changed state rewrites `state_path/users/users.bin` via
  `durable_replace_file` before returning. The file is the whole table (tiny)
  under magic `MACHUSR1`, sealed with `aes_gcm_seal` under
  `derive(master, "macha/users/v1")` — the same HKDF pattern as `auth` and
  `storage` (`src/crypto.cpp:114-115`) — with the magic as AAD. This is the
  only place password hashes touch disk, and es-1's disk is outside the
  house. Per `SECURITY.md:3` the cluster key already grants everything, so
  sealing does not change the trust model; it covers a stolen or discarded
  drive.
- **Load** tolerates a missing file (empty table) and, like sessions
  (`src/session.cpp:115-120`), warns and starts empty on an undecodable one —
  the cluster is the source of truth and a node must never fail to start
  over its user cache. Unlike sessions, an undecodable file is renamed aside
  (`users.bin.corrupt.<ts>`) rather than overwritten, so it can be examined.

### Replication — asynchronous by construction

New `MessageType::user_sync = 41` / `user_sync_reply = 119`. Payload is
always the **full table**: at tens of records it is smaller than the
telemetry set already broadcast every tick.

- **On mutation:** `apply()` locally, persist, then
  `client_.broadcast_best_effort({user_sync, encode_users(all)},
  FrameType::speculative)` (`src/net.cpp:2555`). This queues on usable
  control-lane connections and returns — it never dials, never blocks, and
  returns 0 when the client mutex is busy. The request path never calls
  `NodeRuntime::call()`.
- **Periodic:** the same broadcast on every gossip tick, beside the existing
  `session_sync` broadcast (`src/cluster.cpp:1327-1336`). Tick is
  `min(telemetry_interval, 1 s)` (`src/cluster.cpp:1292`), so a peer whose
  link comes back converges within two ticks in each direction without
  either side doing anything special. No startup pull: a fresh node's table
  is empty for at most a tick or two and Status says so.
- **Inbound:** the `user_sync` case merges the payload and replies with the
  full local table. Replies to notify frames are discarded by the transport,
  so the reply matters only to a future explicit `call()`; an old node that
  does not know type 41 returns `error_reply("unsupported request")`
  (`src/cluster.cpp:1142`), which a notify sender ignores. Mixed-version
  clusters are therefore safe during rollout.

`propagate_session` is changed the same way: `apply_session` locally, then
`broadcast_best_effort` of the single record. The synchronous loop goes.
This is defect (1) above and lands in the first release regardless of the
rest.

### Sessions learn who they belong to

`AuthSession` gains `std::string user_id` (empty for anonymous) and
`uint64_t credential_generation`. The HTTP authenticator (`src/http.cpp:456`,
the `authenticate_` callback) becomes: session lookup, then — only when
`user_id` is non-empty — a `UserStore` lookup checking the user is not a
tombstone and `session.credential_generation == user.credential_generation`.
Two O(1) shared-lock reads instead of one; the "no other locks taken"
comment on `validate()` is updated to say so.

This is how a password change or deletion logs the user out everywhere:
the user record propagates (one fact) and every session it minted fails the
generation check on every node as the record arrives. No session enumeration
and no revoke broadcast. Anonymous sessions are untouched.

**Wire compatibility.** `decode_sessions` calls `reader.finish()`
(`src/session.cpp:85`), so extra fields are a hard break. Two-release
protocol change:

- Release A: decode `MACHSES1` *and* `MACHSES2`; still emit SES1. Persisted
  file also SES1.
- Release B: emit SES2 everywhere. By then every node decodes it.

No `session.cpp` compatibility shim is retained beyond the next major.

### Credential validation and login

`CredentialValidator::validate` returns `std::optional<ValidatedCredentials
{roles, user_id, credential_generation}>` instead of bare roles.
`PasswordCredentialValidator` (replacing `AnonymousCredentialValidator`,
which it subsumes):

- `{}` or absent `credentials` → anonymous, with roles from
  `SessionConfig::anonymous_roles`. Empty list → `403 anonymous_disabled`.
- `{"username","password"}` → local `UserStore` lookup, scrypt, constant-time
  compare. Unknown user and wrong password both return `401
  invalid_credentials` after running the KDF against a fixed dummy salt so
  timing does not reveal which.
- Anything else → `400 unsupported_credentials` (unchanged).

Two local, unreplicated brakes on the unauthenticated endpoint, because
scrypt on a Pi is a CPU DoS vector: at most
`session.max_concurrent_password_checks{2}` KDF runs in flight per node
(excess → `429 try_later`, `Retry-After: 1`), and per-username failure
lockout (`session.failed_login_lockout{30 s}` after 5 failures, in-memory,
this node only). Neither is state worth replicating.

### Roles

Flat strings, additive, checked with the existing `session_has_role`. The
hierarchy is resolved **at mint time**, not at check time: an `admin` user's
session carries `{"admin","operator","viewer"}`. The gate stays one call.

| role       | grants                                                      |
|------------|-------------------------------------------------------------|
| `viewer`   | every GET, playback, own session (`/api/v1/session`), own record (`PATCH /api/v1/users/me`) |
| `operator` | mutating catalogue routes (`src/catalogue_api.cpp:464,494,520,530`); `/api/v1/manage/**` except identity-association reset |
| `admin`    | identity-association reset (`src/manage_api.cpp:554,580` — cluster-wide destructive, can be wildcard); `/api/v1/users/**` |
| `anonymous`| marker only; grants nothing by itself                       |

`SessionConfig::anonymous_roles` defaults to
`{"anonymous","viewer","operator"}` in releases A/B — **no behaviour
change** — and to `{"anonymous","viewer"}` in release C. Operators who want
no anonymous access at all set it empty.

The gate is one function, `required_role(const HttpRequest&)`, called from
`Service::handle_http` (`src/service.cpp:159`) before dispatch. `403
forbidden` names the missing role. `manage_api.cpp:490` stops advertising
`"privileged": false`.

### Users API (admin unless stated)

```
GET    /api/v1/users                 list (no hashes; never)
POST   /api/v1/users                 {username, password, roles}  → 201
GET    /api/v1/users/{id}
PATCH  /api/v1/users/{id}            {password?, roles?}
DELETE /api/v1/users/{id}            → tombstone; 204
GET    /api/v1/users/me              (viewer)
PATCH  /api/v1/users/me              {password} (viewer) — response carries a
                                     fresh session so the caller is not
                                     logged out by their own change
```

Every mutation: bump `version`, set `updated_by = id_`, bump
`credential_generation` on password change or delete, `apply`, persist,
broadcast, return. During a partition both sides accept writes and LWW
resolves them; a password changed on both sides at once loses one change.
Decided: availability wins, because refusing user writes without quorum
would mean being unable to fix an account during exactly the degraded window
this is designed for.

### Bootstrap

`macha-users` (new `tools/users_admin.cpp`, alongside `macha-metadata-dump`):

```
macha-users <state_path> <cluster.key> create <username> --roles admin
macha-users <state_path> <cluster.key> list
```

Reads the password from the terminal, never from argv. Writes
`users.bin` directly; **the node must be stopped**, since a running daemon
would overwrite the file from its own copy — the tool refuses if the
daemon's state lock is held. Root on the box already holds the cluster key,
so this grants nothing new. The first admin is created during the release-B
rolling restart, on one node; it replicates on start.

Rejected: "first `POST /api/v1/users` from anonymous is allowed while the
table is empty". Convenient, and a race with anyone who can reach the es-1
port in the minutes after upgrade.

### Observability

`/api/v1/status` gains per-node `users: {records, tombstones, table_hash,
last_sync_unix_ms}` so cross-node convergence is visible the way
`metadata_generation` is. `session` status gains `password_checks_in_flight`
and `lockouts`.

## Client contract (macha-ts, separate repo)

- `POST /api/v1/session` with `{"credentials": {"username", "password"}}`;
  the response shape is unchanged (`token`, `roles`, …) plus `user_id`.
- On `401` from any route the client re-authenticates; a session can now
  die because its user's password changed, not only because it expired.
- `403` is new and terminal for that action: show the missing role, do not
  retry.

## Releases

**A — 0.38.0: asynchronous propagation and the gate, no behaviour change.**
`propagate_session` becomes `broadcast_best_effort`; `session_sync` inbound
unchanged. `decode_sessions` accepts SES1 and SES2, emits SES1. `required_role`
gate lands with `anonymous_roles` granting everything it grants today.
`CredentialValidator` returns `ValidatedCredentials`. Two-node runtime test
asserting `POST /api/v1/session` returns in under one second with an
unreachable-but-active peer configured — the test that fails on today's code.

**B — 0.39.0: users.** `UserStore`, `user_sync`, sealed write-through
persistence, `PasswordCredentialValidator`, the users API, `macha-users`,
status fields, `docs/management.md` and `docs/configuration.md`. Sessions
emit SES2. First admin created on one node during the rolling restart.

**C — 0.40.0: anonymous becomes read-only.** `anonymous_roles` default →
`{"anonymous","viewer"}`. `SECURITY.md` rewritten: the shared-key model is
unchanged for nodes; HTTP now has per-person identity and tiers.
`ACTIVE.md` P0 authorization item closed.

## Tests (each release blocks on its set)

Fast (`MACHA_FAST_TEST`, `tests/test_users.cpp`, `tests/test_session.cpp`):

- merge: higher version wins; equal version tombstone wins; equal version
  both live → higher `updated_by` wins, and the result is the same when
  applied in either order.
- `apply()` at `max_users` refuses the new record and keeps every existing
  one; a tombstone never ages out.
- persist → reload round-trips the sealed table; a corrupt file is renamed
  aside and the store starts empty.
- scrypt verify: correct password, wrong password, unknown user, tombstoned
  user; parameters stored in the record are the ones used.
- SES1 decodes into a record with empty `user_id` and generation 0; SES2
  round-trips; SES1 encoder output is byte-identical to 0.37.x.
- generation check: a session minted at generation N is refused once the
  user record at N+1 is applied; anonymous sessions unaffected.
- role gate matrix over every route family × `{viewer, operator, admin,
  anonymous-with-defaults, anonymous-empty}`.
- lockout and concurrency brakes.

Runtime (`MACHA_TEST`, pattern of `test_session_cluster_propagation`,
`tests/test_session.cpp:192`):

- create user on n1 → login succeeds on n2 with no `call()` from n1 (assert
  via the RPC counters).
- n2 stopped; create, then delete, a user on n1; start n2 → n2 converges to
  the tombstone within 3 ticks and never accepted the live record.
- n2 stopped; create user on n2's replica via `macha-users`; start n2 → n1
  learns it.
- **alone:** n1 with n2 configured and unreachable — login with a password
  returns 201 in < 1 s; the session validates on n1 immediately.
- password change on n1 while n2 is partitioned; n2's copy of the old
  session keeps working until the partition heals, then fails within 3
  ticks.

## Out of scope, named so it is not discovered

- Online cluster-key rotation (`SECURITY.md:43`) — unchanged.
- Per-user resource limits or per-user playback quotas.
- Wildcard CORS (`ACTIVE.md`, same P0 section) — separate item, unchanged
  by this work.
- The macha-ts login UI — contract above; the client is its own repo.

---

## What actually shipped, and where it diverged (same day)

Recorded here rather than by editing the plan above, so the divergence stays
visible. The plan was right about the shape and wrong about several specifics;
each change below came from the operator pushing back on a decision this plan
had made silently.

**Roles are capabilities, not a ladder.** The plan proposed
`viewer`/`operator`/`admin` with admin implying operator implying viewer. What
shipped is `media_viewer`/`importer`/`manager`/`manage_users`, where the only
implication is that every role implies `media_viewer`. Importing torrents does
not confer the right to delete the catalogue.

**Anonymous is an account, not a config key.** The plan had
`session.anonymous_roles` in YAML. That made "what may an unauthenticated
television reach" a restart-time decision and a second mechanism beside the
user table. It is now the `anonymous` account's roles, edited like anyone
else's and effective on the next session. `session.allow_anonymous` survives as
the on/off switch.

**No recovery key.** Designed, built, then deliberately disabled. A recovery
key must be presentable without an account to be useful, which means a standing
unauthenticated path to the most privileged account in the cluster -- on a
surface that includes an offsite node -- bought with a capability that already
exists behind strictly more access, since anyone who could present one has root
on a node and can run `macha-users passwd root`. The machinery (an X25519
envelope sealing the cluster key, so a key could be verified without anything
derived from it being stored) is kept and tested but uncalled; see
`create_initial_accounts()`. `test_no_recovery_route_is_exposed` stops the
route being reintroduced by accident.

**The invariant is about the role, not about root.** The plan said root was
special. It is not: its roles are ordinary, and a `manage_users` holder can
reset its password. What is protected is that *some* account always holds
`manage_users` -- enforced in `UserStore`, not only in the API, so no second
caller can route around it. Only root's and anonymous's names are special
(neither can be renamed or deleted).

**Session wire compatibility did not need two releases.** The plan proposed
decode-then-emit across 0.38 and 0.39. The format is chosen per payload
instead: a payload of purely anonymous sessions still encodes as `MACHSES1`,
byte-identical to 0.37.x, so a pre-0.38 peer keeps merging sessions through a
rolling upgrade and only rejects payloads describing users it has no concept
of.

**Three latent defects the plan assumed away.** It described the session gossip
tick as "the self-healing backstop". It had never run: `session_sync` on
`FrameType::speculative` failed `class_allowed()` on send, and both inbound
readers dispatched only `telemetry` as a notification and discarded the rest.
Separately, every inbound notification is admitted through the peer's bounded
server request queue, so the plan's "broadcast the full table every tick" would
have spent a real RPC admission slot on every peer forever; gossip now
announces only on change or on a peer appearing that has not been told.

**The upgrade path was missing entirely.** The plan's bootstrap assumed a new
cluster. An existing cluster has bootstrap peers, so no node treats itself as
founding one, so nothing creates accounts -- and with no `anonymous` account
every route refuses. `macha-users init` and a loud startup warning close that;
`test_an_upgraded_cluster_announces_that_it_has_no_accounts` covers it.

**Named for what it is.** "Genesis" is an existing metadata concept
(`genesis_metadata()`, `accept_pristine_genesis_authority`) and account
creation stopped borrowing it: `create_initial_accounts`, `InitialAccounts`,
`<state_path>/initial-root-password`.

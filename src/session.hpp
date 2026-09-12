// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// Small, cluster-replicated auth record. Kept deliberately tiny -- id, roles,
// timestamps -- per the same "small payload" discipline as NodeInfo/
// IdentityAssociationReset, since the whole record is gossiped and persisted
// on every node.
struct AuthSession {
    std::string id;                 // public, opaque; safe in JSON/logs
    Hash256 token_hash{};           // SHA-256(bearer token); the raw token
                                     // itself never appears here or on the
                                     // wire again once minted
    std::vector<std::string> roles; // resolved at mint time -- an admin's
                                     // session carries every role admin
                                     // implies, so a gate is one lookup
    std::string user_id;            // empty for an anonymous session
    // The user's credential_generation as of the mint. A password change or
    // deletion bumps that counter, so every session minted before it fails
    // this comparison on every node the user record reaches -- one replicated
    // fact logs a person out cluster-wide without enumerating their sessions.
    uint64_t credential_generation{};
    uint64_t created_unix_ms{};
    uint64_t expires_unix_ms{};
    uint64_t version{};             // last-write-wins counter
    bool revoked{};
};

bool session_has_role(const AuthSession&, std::string_view role);
bool session_live(const AuthSession&, uint64_t now_unix_ms);
SessionIdentity session_identity(const AuthSession&);

// Wire/disk format is chosen per payload, not per build: a payload whose
// records are all anonymous encodes as MACHSES1, byte-for-byte what 0.37.x
// emits, and anything carrying a user identity encodes as MACHSES2. Both are
// decoded. A pre-0.38 peer therefore keeps merging anonymous sessions across
// a rolling upgrade and only rejects payloads describing users it has no
// concept of. Retire SES1 emission once no pre-0.38 node can rejoin.
Bytes encode_sessions(const std::vector<AuthSession>&);
std::vector<AuthSession> decode_sessions(std::span<const uint8_t>);

struct MintedSession {
    AuthSession session;
    std::string bearer_token; // exists only here and in the client's copy
};

// Cluster-replicated session store. Every node holds its own full copy --
// there is no separate cache layer distinct from this map -- so a local
// validate() is always an O(1) lookup with no network round trip, and a
// pushed mutation (see NodeRuntime::propagate_session) *is* the cache
// invalidation everywhere else.
class SessionManager {
  public:
    SessionManager(std::chrono::milliseconds anonymous_ttl, size_t max_sessions,
                   std::filesystem::path persisted_path = {});

    // Hot path: one shared-lock map lookup, no allocation, no other locks
    // taken. Called once per gated HTTP request.
    std::optional<AuthSession> validate(std::string_view bearer_token) const;
    // Same lookup as validate(), keyed directly by an already-known hash
    // (e.g. HttpRequest::session->token_hash) instead of re-hashing a raw
    // token. Used to re-resolve the caller's own session for introspection.
    std::optional<AuthSession> find(const Hash256& token_hash) const;

    // nullopt means the local store is at max_sessions capacity -- a runaway
    // client minting in a loop (observed live: a broken proactive-refresh
    // timer re-minting on every request) must hit a hard local cap rather
    // than growing this node's replica without bound.
    std::optional<MintedSession> create(std::vector<std::string> roles,
                                        std::string user_id = {},
                                        uint64_t credential_generation = 0);
    bool apply(AuthSession);                 // local merge, LWW by version
    std::optional<AuthSession> revoke(const Hash256& token_hash);

    std::vector<AuthSession> recent(std::chrono::milliseconds max_age,
                                     size_t max_records = 4096) const;
    void prune_expired(uint64_t now_unix_ms);
    void persist();

  private:
    struct Record {
        AuthSession session;
        Clock::time_point received{Clock::now()};
    };

    mutable std::shared_mutex mutex_;
    std::map<Hash256, Record> by_token_hash_;
    std::chrono::milliseconds anonymous_ttl_;
    size_t max_sessions_;
    std::filesystem::path persisted_path_;
};

} // namespace macha

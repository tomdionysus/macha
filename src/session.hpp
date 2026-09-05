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
    std::vector<std::string> roles; // {"anonymous"} for v1
    uint64_t created_unix_ms{};
    uint64_t expires_unix_ms{};
    uint64_t version{};             // last-write-wins counter
    bool revoked{};
};

bool session_has_role(const AuthSession&, std::string_view role);
bool session_live(const AuthSession&, uint64_t now_unix_ms);
SessionIdentity session_identity(const AuthSession&);

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
    std::optional<MintedSession> create(std::vector<std::string> roles);
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

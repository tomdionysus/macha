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

// Cluster-replicated auth record, kept small: it is gossiped and persisted on
// every node.
struct AuthSession {
    std::string id;                 // public, opaque; safe in JSON/logs
    Hash256 token_hash{};           // SHA-256(bearer token); the raw token is
                                     // never stored or sent after minting
    std::vector<std::string> roles; // resolved at mint, implied roles
                                     // included, so a gate is one lookup
    std::string user_id;            // empty for an anonymous session
    // The user's credential_generation at mint. A password change or deletion
    // bumps it, invalidating every older session cluster-wide without
    // enumerating them.
    uint64_t credential_generation{};
    uint64_t created_unix_ms{};
    uint64_t expires_unix_ms{};
    uint64_t version{};             // last-write-wins counter
    bool revoked{};
};

bool session_has_role(const AuthSession&, std::string_view role);
bool session_live(const AuthSession&, uint64_t now_unix_ms);
SessionIdentity session_identity(const AuthSession&);

// Wire/disk format per payload: all-anonymous encodes as MACHSES1, anything
// with a user identity as MACHSES2. Both decode.
Bytes encode_sessions(const std::vector<AuthSession>&);
std::vector<AuthSession> decode_sessions(std::span<const uint8_t>);

struct MintedSession {
    AuthSession session;
    std::string bearer_token; // exists only here and in the client's copy
};

// Cluster-replicated session store. Every node holds a full copy, so
// validate() is local, and a pushed mutation (NodeRuntime::propagate_session)
// is the invalidation everywhere else.
class SessionManager {
  public:
    SessionManager(std::chrono::milliseconds anonymous_ttl, size_t max_sessions,
                   std::filesystem::path persisted_path = {});

    // Hot path, once per gated request: one shared-lock lookup, no other locks.
    std::optional<AuthSession> validate(std::string_view bearer_token) const;
    // validate() by an already-known token hash.
    std::optional<AuthSession> find(const Hash256& token_hash) const;

    // nullopt at max_sessions: a client minting in a loop hits a hard cap
    // rather than growing the replica without bound.
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

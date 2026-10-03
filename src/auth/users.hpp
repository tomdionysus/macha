// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "crypto.hpp"
#include "types.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

// Roles are additive capabilities, not a ladder: an importer may not delete
// the catalogue. Implications are expanded at mint (expand_roles), so a gate
// is one session_has_role() lookup and a new implication reaches existing
// accounts without migration.
//
//   view_status   see cluster and node health
//   media_viewer  read all media and play it back
//   importer      acquire content (torrents, ingest)
//   manager       manage files, namespaces, catalogue matches, and the
//                 cluster itself (identity-association reset)
//   manage_users  add, edit and remove accounts
//
// view_status is implied by everything, implies nothing, and is grantable
// alone: give it to anonymous to show health publicly; an account granted
// nothing cannot see health.
inline constexpr std::string_view role_view_status = "view_status";
inline constexpr std::string_view role_media_viewer = "media_viewer";
inline constexpr std::string_view role_importer = "importer";
inline constexpr std::string_view role_manager = "manager";
inline constexpr std::string_view role_manage_users = "manage_users";

// The full set, given to the genesis root account.
std::vector<std::string> all_roles();

bool known_role(std::string_view);
// Every role but view_status implies media_viewer and view_status; nothing
// else implies anything. Returns the closed set, deduplicated, in a stable
// order, keeping any marker role (e.g. "anonymous").
std::vector<std::string> expand_roles(const std::vector<std::string>&);

// The cluster key sealed to a one-time X25519 public key whose private half
// is the recovery key, shown once when issued and never stored; the public half
// is discarded after sealing. The envelope verifies a recovery key but cannot
// produce one, and reveals nothing beyond the cluster key every node already
// holds. Unlike a derived key, the recovery key is an independent factor: the
// cluster key does not yield it. Bound to the cluster key it was sealed
// against; a rotated key would need a re-issued envelope.
struct RecoveryEnvelope {
    std::array<uint8_t, 32> ephemeral_public{};
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    std::array<uint8_t, 32> ciphertext{}; // the sealed 32-byte cluster master key
    bool present() const;
};

struct IssuedRecoveryKey {
    RecoveryEnvelope envelope;
    // 64 hex characters: the raw X25519 scalar, not a passphrase.
    std::string key;
};

// Seals the cluster key to a fresh keypair: the envelope to store, and the
// private key to show the operator.
IssuedRecoveryKey issue_recovery_key(const ClusterKeys&);
// True when `offered` (64 hex) decrypts the envelope to this cluster's key.
// Fails closed on a malformed or wrong key, or another cluster's envelope.
bool recovery_key_matches(const RecoveryEnvelope&, const ClusterKeys&, std::string_view offered);

// A cluster-replicated account, kept small: the whole table is gossiped and
// persisted on every node.
struct UserRecord {
    std::string id;       // opaque, stable across renames; safe in JSON/logs
    std::string username; // unique, lowercased at create
    uint8_t kdf{1};       // 1 = scrypt
    std::array<uint8_t, 16> salt{};
    uint32_t kdf_n{}, kdf_r{}, kdf_p{};
    Hash256 password_hash{};
    std::vector<std::string> roles;
    // Bumped on password change and deletion; sessions minted against an older
    // value stop validating on every node.
    uint64_t credential_generation{};
    uint64_t created_unix_ms{};
    uint64_t updated_unix_ms{};
    uint64_t version{}; // last-write-wins counter
    NodeId updated_by{}; // deterministic tie-break at equal version
    bool tombstone{};

    // Root only; see RecoveryEnvelope.
    RecoveryEnvelope recovery;
};

bool user_has_role(const UserRecord&, std::string_view role);
std::string normalize_username(std::string_view);


Bytes encode_users(const std::vector<UserRecord>&);
std::vector<UserRecord> decode_users(std::span<const uint8_t>);

struct UserCredentialCheck {
    bool ok{};
    std::string user_id;
    std::vector<std::string> roles;
    uint64_t credential_generation{};
};

// Cluster-replicated user table. Every node holds a full copy, tombstones
// included, so login is a local lookup plus a KDF with no RPC, metadata or
// catalogue: a lone node can authenticate everyone it knows.
//   - merge is deterministic and commutative (version, then tombstone, then
//     updated_by), so partitioned replicas converge;
//   - nothing is evicted and tombstones are kept forever, or a stale replica
//     could resurrect a deleted account;
//   - every mutation is written through, so a password change is durable on
//     return.
class UserStore {
  public:
    UserStore(size_t max_users, std::filesystem::path persisted_path = {},
              std::array<uint8_t, 32> seal_key = {});

    // One shared-lock lookup, then the KDF. ok=false alike for unknown,
    // tombstoned and wrong password; an unknown user still runs the KDF on a
    // dummy salt so timing does not tell.
    UserCredentialCheck verify(std::string_view username, std::string_view password) const;

    std::optional<UserRecord> find(std::string_view user_id) const;
    std::optional<UserRecord> find_by_username(std::string_view username) const;
    // Live (non-tombstone) records only, ordered by username.
    std::vector<UserRecord> list() const;
    // Everything, tombstones included, for replication and persistence.
    std::vector<UserRecord> all() const;
    // True when this account is the only live one holding manage_users. At
    // least one account must always hold it; which one is not fixed.
    bool sole_user_manager(std::string_view user_id) const;
    size_t size() const;
    size_t tombstones() const;
    // Equal across nodes for equal contents; shown in status to make
    // convergence visible.
    Hash256 table_hash() const;

    // nullopt when the username is taken or the table is at max_users.
    std::optional<UserRecord> create(std::string_view username, std::string_view password,
                                     const std::vector<std::string>& roles, const NodeId& by);
    // An account with no password (kdf 0, no salt, no hash): `anonymous`,
    // reached only by a credential-less mint, so `allow_anonymous: false`
    // closes the only way in.
    std::optional<UserRecord> create_without_password(std::string_view username,
                                                      const std::vector<std::string>& roles,
                                                      const NodeId& by);
    // nullopt when the user is absent or tombstoned, or for a password on
    // `anonymous` (enforced here so no caller routes around it). An empty
    // password or nullopt roles leaves that part alone.
    std::optional<UserRecord> update(std::string_view user_id, std::string_view password,
                                     const std::optional<std::vector<std::string>>& roles,
                                     const NodeId& by);
    std::optional<UserRecord> remove(std::string_view user_id, const NodeId& by);

    // Sets root's password without the old one. The caller must have verified
    // a recovery key first; this checks nothing.
    std::optional<UserRecord> reset_root_password(std::string_view new_password,
                                                  const NodeId& by);
    std::optional<RecoveryEnvelope> root_recovery() const;
    // Replaces the envelope, invalidating the previous recovery key.
    bool set_root_recovery(const RecoveryEnvelope&, const NodeId& by);

    // Local merge. True when the table changed: persist and re-broadcast.
    bool apply(UserRecord);
    bool apply_all(const std::vector<UserRecord>&);

    void persist() const;

  private:
    std::optional<UserRecord> mutate(std::string_view user_id,
                                     const std::function<bool(UserRecord&)>& change,
                                     const NodeId& by);
    // An empty password makes a kdf-0 record, which verify() refuses before the KDF.
    std::optional<UserRecord> insert(std::string_view username, std::string_view password,
                                     const std::vector<std::string>& roles, const NodeId& by);
    void persist_locked() const MACHA_REQUIRES_SHARED(mutex_);

    // Held shared while the table is persisted.
    mutable IoSharedMutex mutex_;
    std::map<std::string, UserRecord> by_id_ MACHA_GUARDED_BY(mutex_);
    const size_t max_users_;
    const std::filesystem::path persisted_path_;
    const std::array<uint8_t, 32> seal_key_;
};

// A high-entropy password safe to retype: no vowels (spells nothing) and no
// characters confusable in a terminal font.
std::string generate_password();

inline constexpr std::string_view root_username = "root";
// The account a credential-less session is bound to; its roles are what an
// unauthenticated visitor may do, edited like any user's. It has no password
// and cannot be given one: logging in as it would bypass
// `session.allow_anonymous: false`, which must be the only door.
inline constexpr std::string_view anonymous_username = "anonymous";

// root and anonymous cannot be renamed or removed; their roles stay editable.
bool reserved_username(std::string_view);

// What may be changed about one account, carried on every user record so
// clients need not know which names are special.
struct UserMutability {
    bool rename{};
    bool remove{};
    bool set_password{};
    bool set_roles{};
    // Roles that may not be removed: manage_users on the last account holding it.
    std::vector<std::string> required_roles;
    // Why required_roles is non-empty, as a code a client can show.
    bool last_user_manager{};
};

// Exposed to clients so their validation matches the server's.
inline constexpr size_t min_password_length = 8;

struct InitialAccounts {
    UserRecord root;
    UserRecord anonymous;
    std::string password;     // root's; exists only here and in the file below
    std::string recovery_key; // shown once; Macha keeps only the envelope
    std::filesystem::path path; // where the operator can read both back
};

// Creates root and anonymous once, on the founding node. nullopt on a joining
// node (bootstrap peers configured) or when the table holds anything,
// tombstones included. The password goes to `state_path/initial-root-password`
// (mode 0600), never the log; the caller says where it is.
std::optional<InitialAccounts> create_initial_accounts(UserStore&, const ClusterKeys&,
                                               const std::filesystem::path& state_path,
                                               const NodeId& by);

} // namespace macha

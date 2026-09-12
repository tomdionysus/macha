// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// Roles are flat strings and additive capabilities rather than a ladder: a
// person who imports torrents is not thereby allowed to delete the catalogue.
// The one implication is that every role can read, so expand_roles() is
// resolved once at mint time and a route gate stays a single
// session_has_role() lookup on the session the caller already presented.
//
//   media_viewer  read all media, playback, and cluster status
//   importer      acquire content (torrents, ingest)
//   manager       manage files, namespaces, catalogue matches, and the
//                 cluster itself (identity-association reset)
//   manage_users  add, edit and remove accounts
inline constexpr std::string_view role_media_viewer = "media_viewer";
inline constexpr std::string_view role_importer = "importer";
inline constexpr std::string_view role_manager = "manager";
inline constexpr std::string_view role_manage_users = "manage_users";

// The full set, which is what the genesis root account is created with.
std::vector<std::string> all_roles();

bool known_role(std::string_view);
// Every role implies media_viewer; nothing else implies anything. Returns the
// closed set, deduplicated and in a stable order, preserving any marker role
// (e.g. "anonymous") it was given.
std::vector<std::string> expand_roles(const std::vector<std::string>&);

// The cluster key, sealed to a one-time X25519 public key whose private half
// is the recovery key. The private half is shown once at genesis and is never
// written anywhere by Macha; the public half is discarded immediately after
// sealing. What remains -- this envelope -- can verify a recovery key offered
// later but cannot produce one, and yields nothing an attacker does not
// already have: every node holds the cluster key in plaintext anyway, so this
// is a verifier and not a second copy of a secret.
//
// The point of the asymmetry, versus deriving a recovery key from the cluster
// key: holding the cluster key does NOT yield the recovery key. It is a
// genuinely independent factor, which a derived one could never be.
//
// Bound to the cluster key it was sealed against. There is no online
// cluster-key rotation (SECURITY.md), so that is moot today -- but if one
// ever arrives, a stale envelope stops verifying rather than failing loudly,
// and re-issuing is then part of rotating.
struct RecoveryEnvelope {
    std::array<uint8_t, 32> ephemeral_public{};
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    std::array<uint8_t, 32> ciphertext{}; // the sealed 32-byte cluster master key
    bool present() const;
};

struct IssuedRecoveryKey {
    RecoveryEnvelope envelope;
    // 64 hex characters: the raw X25519 scalar, not a passphrase and not
    // derived from one. Deliberately not friendly -- it is written down once,
    // kept somewhere safe, and used when everything else has failed.
    std::string key;
};

// Seal the cluster key to a fresh keypair and return both halves of the
// result: the envelope to store, and the private key to show the operator.
IssuedRecoveryKey issue_recovery_key(const ClusterKeys&);
// True when `offered` (64 hex characters) decrypts the envelope back to this
// cluster's key. Fails closed on a malformed key, a wrong key, and on an
// envelope sealed against a different cluster key.
bool recovery_key_matches(const RecoveryEnvelope&, const ClusterKeys&, std::string_view offered);

// A cluster-replicated account. Deliberately tiny, per the same "small
// replicated payload" discipline as AuthSession and NodeInfo: the whole table
// is gossiped and persisted on every node.
struct UserRecord {
    std::string id;       // opaque, stable across renames; safe in JSON/logs
    std::string username; // unique, lowercased at create
    uint8_t kdf{1};       // 1 = scrypt
    std::array<uint8_t, 16> salt{};
    uint32_t kdf_n{}, kdf_r{}, kdf_p{};
    Hash256 password_hash{};
    std::vector<std::string> roles;
    // Bumped on every password change and on deletion. AuthSession carries the
    // value it was minted against, so replicating this one number is what logs
    // a person out on every node at once.
    uint64_t credential_generation{};
    uint64_t created_unix_ms{};
    uint64_t updated_unix_ms{};
    uint64_t version{}; // last-write-wins counter
    NodeId updated_by{}; // deterministic tie-break at equal version
    bool tombstone{};

    // Set on root alone, at genesis. See RecoveryEnvelope: this holds the
    // cluster key sealed to a public key whose private half was handed to the
    // operator and never kept, so it verifies a recovery key without being
    // one, and without anything derived from one being stored.
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

// Cluster-replicated user table. Every node holds a full copy including
// tombstones, so verifying a password is one shared-lock lookup plus a KDF --
// no RPC, no metadata, no catalogue. A node that is alone can authenticate
// every user it knows about, which is the whole point: the subsystem most
// likely to be sick when you need to log in must not be in the login path.
//
// Differs from SessionManager in the three places that matter for a
// credential store rather than a cache:
//   - merge is deterministic and commutative (version, then tombstone, then
//     updated_by), so two partitioned replicas converge to the same record
//     instead of flapping;
//   - nothing is ever evicted to make room, and tombstones are kept forever --
//     ageing one out would let a stale replica resurrect a deleted account;
//   - persistence is write-through on every mutation, not deferred to an idle
//     window, because a password change must be durable when it returns.
class UserStore {
  public:
    UserStore(size_t max_users, std::filesystem::path persisted_path = {},
              std::array<uint8_t, 32> seal_key = {});

    // Hot path: one shared-lock lookup, then the KDF. Returns ok=false for an
    // unknown user, a tombstoned user and a wrong password alike -- and runs
    // the KDF against a fixed dummy salt in the unknown-user case so the
    // timing does not say which.
    UserCredentialCheck verify(std::string_view username, std::string_view password) const;

    std::optional<UserRecord> find(std::string_view user_id) const;
    std::optional<UserRecord> find_by_username(std::string_view username) const;
    // Live (non-tombstone) records only, ordered by username.
    std::vector<UserRecord> list() const;
    // Everything, tombstones included: what replication and persistence send.
    std::vector<UserRecord> all() const;
    // True when this account holds manage_users and no other live account
    // does. The cluster invariant is that at least one account can always
    // administer accounts: it is not a property of any particular user (root
    // included), so it moves as the role moves.
    bool sole_user_manager(std::string_view user_id) const;
    size_t size() const;
    size_t tombstones() const;
    // Stable across nodes for the same table contents; surfaced in status so
    // cross-node convergence is visible the way metadata_generation is.
    Hash256 table_hash() const;

    // nullopt when the username is taken or the table is at max_users.
    std::optional<UserRecord> create(std::string_view username, std::string_view password,
                                     const std::vector<std::string>& roles, const NodeId& by);
    // nullopt when the user is absent or tombstoned. An empty password leaves
    // the credential (and generation) alone; nullopt roles leaves roles alone.
    std::optional<UserRecord> update(std::string_view user_id, std::string_view password,
                                     const std::optional<std::vector<std::string>>& roles,
                                     const NodeId& by);
    std::optional<UserRecord> remove(std::string_view user_id, const NodeId& by);

    // Set root's password without presenting the old one. The caller is
    // responsible for having verified a recovery key first; this store holds
    // nothing about recovery keys and checks nothing. An account holding
    // manage_users can also set root's password the ordinary way -- the
    // recovery key is the route back when nobody can sign in at all, not the
    // only route.
    std::optional<UserRecord> reset_root_password(std::string_view new_password,
                                                  const NodeId& by);
    std::optional<RecoveryEnvelope> root_recovery() const;
    // Replace the envelope, invalidating the previous recovery key. Used at
    // genesis and by an operator re-issuing a lost one.
    bool set_root_recovery(const RecoveryEnvelope&, const NodeId& by);

    // Local merge. True when the table changed and the caller should persist
    // and re-broadcast.
    bool apply(UserRecord);
    bool apply_all(const std::vector<UserRecord>&);

    void persist() const;

  private:
    std::optional<UserRecord> mutate(std::string_view user_id,
                                     const std::function<bool(UserRecord&)>& change,
                                     const NodeId& by);
    void persist_locked() const;

    mutable std::shared_mutex mutex_;
    std::map<std::string, UserRecord> by_id_;
    size_t max_users_;
    std::filesystem::path persisted_path_;
    std::array<uint8_t, 32> seal_key_{};
};

// A high-entropy password a person can read off a screen and retype without
// getting it wrong: no vowels (so it cannot spell anything), and none of the
// character pairs that are indistinguishable in a terminal font.
std::string generate_password();

inline constexpr std::string_view root_username = "root";
// Anonymous access is an ordinary account, not a special case in the auth
// path: a session minted with no credentials is bound to this user and carries
// whatever roles it currently holds. Changing what an unauthenticated visitor
// may do is therefore an ordinary PATCH of an ordinary user, visible in the
// same list as everyone else, rather than a config key that only takes effect
// on restart.
inline constexpr std::string_view anonymous_username = "anonymous";

// root and anonymous cannot be renamed or removed -- root is the way back in
// when every other account is locked out, and anonymous is what unauthenticated
// visitors are. Everything else about them is ordinary: roles, password and
// all the usual routes.
bool reserved_username(std::string_view);

// What may be changed about one account. Stated by the server and carried on
// every user record, so a client renders the right controls without needing to
// know that "root" and "anonymous" are special -- a `username == "root"` test
// in a client is wrong the moment these names change, and wrong in four
// clients at once.
struct UserMutability {
    bool rename{};
    bool remove{};
    bool set_password{};
    bool set_roles{};
    // Roles that may not be taken off this account, while everything else
    // about its roles stays editable. Today this holds manage_users, and only
    // for the last account that has it -- the cluster must never reach a state
    // where nobody can administer accounts, and that is a statement about the
    // role rather than about the account, so it moves as the role does.
    std::vector<std::string> required_roles;
    // Why required_roles is non-empty, as a code a client can show.
    bool last_user_manager{};
};

// Minimum password length. Exposed to clients so inline validation matches
// what the server will actually accept, instead of drifting from it.
inline constexpr size_t min_password_length = 8;

struct InitialAccounts {
    UserRecord root;
    UserRecord anonymous;
    std::string password;     // root's; exists only here and in the file below
    std::string recovery_key; // shown once; Macha keeps only the envelope
    std::filesystem::path path; // where the operator can read both back
};

// Create the two accounts every cluster has -- root and anonymous -- once, on
// the node that founds the cluster. Returns nullopt when this is not that
// moment: a joining node (one with configured bootstrap peers) never does
// this, and neither does a founder whose table already holds anything at all,
// tombstones included.
//
// The generated password is written to `state_path/initial-root-password` with
// mode 0600 rather than logged, because a log line is shipped, rotated and
// read by more people than the file is. The caller is expected to say loudly
// where it is.
std::optional<InitialAccounts> create_initial_accounts(UserStore&, const ClusterKeys&,
                                               const std::filesystem::path& state_path,
                                               const NodeId& by);

} // namespace macha

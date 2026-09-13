// SPDX-License-Identifier: GPL-3.0-or-later
#include "users.hpp"

#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> magic{'M', 'A', 'C', 'H', 'U', 'S', 'R', '1'};
constexpr size_t max_roles = 64;
constexpr size_t max_role_length = 64;
constexpr size_t max_username_length = 64;
constexpr size_t max_id_length = 128;
constexpr size_t id_bytes = 16;

// ~32 MiB, ~50-100 ms on a Pi 4. Stored per record, so these can be raised
// later without invalidating existing passwords: a record is verified with the
// parameters it was written with.
constexpr uint32_t default_kdf_n = 1u << 15;
constexpr uint32_t default_kdf_r = 8;
constexpr uint32_t default_kdf_p = 1;
// EVP_PBE_scrypt refuses to allocate past this; must exceed 128*r*N.
constexpr uint64_t scrypt_max_memory = 64ull * 1024 * 1024;

constexpr std::string_view recovery_label = "macha/recovery/v1";

std::array<uint8_t, 32> recovery_wrap_key(std::span<const uint8_t, 32> shared) {
    return hkdf_sha256(shared, {},
                       std::span<const uint8_t>(
                           reinterpret_cast<const uint8_t*>(recovery_label.data()),
                           recovery_label.size()));
}

Hash256 scrypt_hash(std::string_view password, std::span<const uint8_t, 16> salt, uint32_t n,
                    uint32_t r, uint32_t p) {
    Hash256 out{};
    if (EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), n, r, p,
                       scrypt_max_memory, out.bytes.data(), out.bytes.size()) != 1)
        throw std::runtime_error("scrypt failed");
    return out;
}

void encode(Writer& writer, const UserRecord& value) {
    writer.string(value.id);
    writer.string(value.username);
    writer.u8(value.kdf);
    writer.fixed(value.salt);
    writer.u32(value.kdf_n);
    writer.u32(value.kdf_r);
    writer.u32(value.kdf_p);
    writer.fixed(value.password_hash.bytes);
    writer.u32(static_cast<uint32_t>(value.roles.size()));
    for (const auto& role : value.roles)
        writer.string(role);
    writer.u64(value.credential_generation);
    writer.u64(value.created_unix_ms);
    writer.u64(value.updated_unix_ms);
    writer.u64(value.version);
    writer.fixed(value.updated_by.bytes);
    writer.u8(value.tombstone ? 1 : 0);
    writer.fixed(value.recovery.ephemeral_public);
    writer.fixed(value.recovery.nonce);
    writer.fixed(value.recovery.tag);
    writer.fixed(value.recovery.ciphertext);
}

UserRecord decode(Reader& reader) {
    UserRecord value;
    value.id = reader.string(max_id_length);
    value.username = reader.string(max_username_length);
    value.kdf = reader.u8();
    value.salt = reader.fixed<16>();
    value.kdf_n = reader.u32();
    value.kdf_r = reader.u32();
    value.kdf_p = reader.u32();
    value.password_hash.bytes = reader.fixed<32>();
    const auto role_count = reader.u32();
    if (role_count > max_roles)
        throw DecodeError("user has too many roles");
    value.roles.reserve(role_count);
    for (uint32_t i = 0; i < role_count; ++i)
        value.roles.push_back(reader.string(max_role_length));
    value.credential_generation = reader.u64();
    value.created_unix_ms = reader.u64();
    value.updated_unix_ms = reader.u64();
    value.version = reader.u64();
    value.updated_by.bytes = reader.fixed<16>();
    value.tombstone = reader.u8() != 0;
    value.recovery.ephemeral_public = reader.fixed<32>();
    value.recovery.nonce = reader.fixed<12>();
    value.recovery.tag = reader.fixed<16>();
    value.recovery.ciphertext = reader.fixed<32>();
    if (value.id.empty())
        throw DecodeError("user id must be nonempty");
    return value;
}

// Deterministic and commutative, so two replicas that saw the same writes in
// different orders converge on the same record. Version first; at equal
// version a tombstone wins (a deletion must not be undone by a concurrent
// edit); then the higher updated_by, purely to break the remaining tie the
// same way on both sides.
bool incoming_wins(const UserRecord& existing, const UserRecord& incoming) {
    if (incoming.version != existing.version)
        return incoming.version > existing.version;
    if (incoming.tombstone != existing.tombstone)
        return incoming.tombstone;
    return std::lexicographical_compare(existing.updated_by.bytes.begin(),
                                        existing.updated_by.bytes.end(),
                                        incoming.updated_by.bytes.begin(),
                                        incoming.updated_by.bytes.end());
}
} // namespace

std::vector<std::string> all_roles() {
    return {std::string(role_manage_users), std::string(role_manager),
            std::string(role_importer), std::string(role_media_viewer),
            std::string(role_view_status)};
}

bool known_role(std::string_view role) {
    return role == role_media_viewer || role == role_importer || role == role_manager ||
           role == role_manage_users || role == role_view_status;
}

bool reserved_username(std::string_view username) {
    const auto normalized = normalize_username(username);
    return normalized == root_username || normalized == anonymous_username;
}

std::vector<std::string> expand_roles(const std::vector<std::string>& roles) {
    auto holds = [&](std::string_view role) {
        return std::find(roles.begin(), roles.end(), role) != roles.end();
    };
    // Capabilities are additive, not a ladder: importer does not imply manager
    // and manager does not imply admin. The single implication is that holding
    // any of them means you can read, since none of them is usable otherwise.
    const bool manage_users = holds(role_manage_users);
    const bool manager = holds(role_manager);
    const bool importer = holds(role_importer);
    const bool viewer = manage_users || manager || importer || holds(role_media_viewer);
    // Seeing cluster health is implied by every capability, for the same
    // reason reading is: an importer watching an ingest, or a manager repairing
    // the cluster, cannot do the job blind, and the status screen earns its
    // place exactly when things are going wrong. It is deliberately not part of
    // the disjunction above -- granting view_status alone must not hand out
    // media access.
    const bool view_status = viewer || holds(role_view_status);

    std::vector<std::string> out;
    // An unknown role is carried through rather than dropped: nothing grants it
    // access, and silently discarding it would hide a typo in a config or a
    // role from a newer version of the software.
    for (const auto& role : roles)
        if (!known_role(role))
            if (std::find(out.begin(), out.end(), role) == out.end())
                out.push_back(role);
    if (manage_users)
        out.emplace_back(role_manage_users);
    if (manager)
        out.emplace_back(role_manager);
    if (importer)
        out.emplace_back(role_importer);
    if (viewer)
        out.emplace_back(role_media_viewer);
    if (view_status)
        out.emplace_back(role_view_status);
    return out;
}

bool user_has_role(const UserRecord& user, std::string_view role) {
    return std::find(user.roles.begin(), user.roles.end(), role) != user.roles.end();
}

std::string normalize_username(std::string_view username) {
    std::string out;
    out.reserve(username.size());
    for (const char c : username)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

Bytes encode_users(const std::vector<UserRecord>& values) {
    Writer writer;
    writer.raw(magic);
    writer.u32(static_cast<uint32_t>(values.size()));
    for (const auto& value : values)
        encode(writer, value);
    return writer.take();
}

std::vector<UserRecord> decode_users(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    auto got = reader.raw(magic.size());
    if (!std::equal(got.begin(), got.end(), magic.begin()))
        throw DecodeError("bad user payload");
    const auto count = reader.u32();
    if (count > 65536)
        throw DecodeError("too many users");
    std::vector<UserRecord> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        values.push_back(decode(reader));
    reader.finish();
    return values;
}

UserStore::UserStore(size_t max_users, std::filesystem::path persisted_path,
                     std::array<uint8_t, 32> seal_key)
    : max_users_(max_users), persisted_path_(std::move(persisted_path)), seal_key_(seal_key) {
    if (persisted_path_.empty() || !std::filesystem::exists(persisted_path_))
        return;
    try {
        const auto size = std::filesystem::file_size(persisted_path_);
        constexpr uint64_t max_persisted_bytes = 16ull * 1024 * 1024;
        if (size > max_persisted_bytes)
            throw std::runtime_error("persisted users are too large");
        std::ifstream input(persisted_path_, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open persisted users");
        Bytes sealed(static_cast<size_t>(size));
        if (size && !input.read(reinterpret_cast<char*>(sealed.data()),
                                static_cast<std::streamsize>(size)))
            throw std::runtime_error("cannot read persisted users");

        Reader reader(sealed);
        const auto nonce = reader.fixed<12>();
        const auto tag = reader.fixed<16>();
        const auto ciphertext = reader.bytes();
        reader.finish();
        const auto plain = aes_gcm_open(seal_key_, nonce, tag, ciphertext, magic);
        for (auto& user : decode_users(plain)) {
            auto id = user.id;
            by_id_.emplace(std::move(id), std::move(user));
        }
    } catch (const std::exception& error) {
        // Unlike the session cache, an unreadable user table is kept for
        // inspection rather than silently overwritten -- it is the only copy
        // of a credential this node holds. Starting empty is still correct:
        // the cluster is the source of truth and gossip refills the table,
        // and a node must never fail to start over its own auth cache.
        Log::warn("persisted users ignored: " + std::string(error.what()));
        std::error_code ec;
        auto aside = persisted_path_;
        aside += ".corrupt." + std::to_string(unix_ms());
        std::filesystem::rename(persisted_path_, aside, ec);
        if (ec)
            Log::warn("could not set aside unreadable user table: " + ec.message());
        else
            Log::warn("unreadable user table kept at " + aside.string());
    }
}

UserCredentialCheck UserStore::verify(std::string_view username,
                                      std::string_view password) const {
    const auto normalized = normalize_username(username);
    std::optional<UserRecord> user;
    // `anonymous` is never reachable by password, whatever its record happens
    // to hold. A cluster created before 0.38.4 still carries the random
    // password genesis used to generate for it; this is what makes that
    // credential inert rather than a standing way past allow_anonymous.
    if (normalized != anonymous_username) {
        std::shared_lock lock(mutex_);
        for (const auto& [_, record] : by_id_)
            if (!record.tombstone && record.username == normalized) {
                user = record;
                break;
            }
    }

    if (!user) {
        // Run the KDF anyway, at the default cost, so an unknown username is
        // not distinguishable from a wrong password by how long the answer
        // took. The result is discarded.
        static constexpr std::array<uint8_t, 16> dummy_salt{};
        try {
            (void)scrypt_hash(password, dummy_salt, default_kdf_n, default_kdf_r, default_kdf_p);
        } catch (const std::exception&) {
        }
        return {};
    }

    if (user->kdf != 1)
        return {};
    Hash256 candidate{};
    try {
        candidate = scrypt_hash(password, user->salt, user->kdf_n, user->kdf_r, user->kdf_p);
    } catch (const std::exception& error) {
        Log::warn("password check failed: " + std::string(error.what()));
        return {};
    }
    if (!constant_time_equal(candidate.bytes, user->password_hash.bytes))
        return {};
    // Expanded here, not merely as stored: implications are a derivation of the
    // granted set, so one added in a later version applies to accounts written
    // before it without rewriting the table. Idempotent for anything create()
    // or update() already expanded.
    return {true, user->id, expand_roles(user->roles), user->credential_generation};
}

std::optional<UserRecord> UserStore::find(std::string_view user_id) const {
    std::shared_lock lock(mutex_);
    auto found = by_id_.find(std::string(user_id));
    if (found == by_id_.end() || found->second.tombstone)
        return std::nullopt;
    return found->second;
}

std::optional<UserRecord> UserStore::find_by_username(std::string_view username) const {
    const auto normalized = normalize_username(username);
    std::shared_lock lock(mutex_);
    for (const auto& [_, record] : by_id_)
        if (!record.tombstone && record.username == normalized)
            return record;
    return std::nullopt;
}

std::vector<UserRecord> UserStore::list() const {
    std::shared_lock lock(mutex_);
    std::vector<UserRecord> out;
    for (const auto& [_, record] : by_id_)
        if (!record.tombstone)
            out.push_back(record);
    std::sort(out.begin(), out.end(),
              [](const UserRecord& a, const UserRecord& b) { return a.username < b.username; });
    return out;
}

std::vector<UserRecord> UserStore::all() const {
    std::shared_lock lock(mutex_);
    std::vector<UserRecord> out;
    out.reserve(by_id_.size());
    for (const auto& [_, record] : by_id_)
        out.push_back(record);
    return out;
}

bool UserStore::sole_user_manager(std::string_view user_id) const {
    std::shared_lock lock(mutex_);
    auto found = by_id_.find(std::string(user_id));
    if (found == by_id_.end() || found->second.tombstone ||
        !user_has_role(found->second, role_manage_users))
        return false;
    for (const auto& [id, record] : by_id_)
        if (!record.tombstone && id != found->first && user_has_role(record, role_manage_users))
            return false;
    return true;
}

size_t UserStore::size() const {
    std::shared_lock lock(mutex_);
    return static_cast<size_t>(
        std::count_if(by_id_.begin(), by_id_.end(),
                      [](const auto& item) { return !item.second.tombstone; }));
}

size_t UserStore::tombstones() const {
    std::shared_lock lock(mutex_);
    return static_cast<size_t>(
        std::count_if(by_id_.begin(), by_id_.end(),
                      [](const auto& item) { return item.second.tombstone; }));
}

Hash256 UserStore::table_hash() const {
    std::shared_lock lock(mutex_);
    std::vector<UserRecord> ordered;
    ordered.reserve(by_id_.size());
    for (const auto& [_, record] : by_id_)
        ordered.push_back(record);
    // by_id_ is already ordered by id, which is stable across nodes.
    const auto encoded = encode_users(ordered);
    return sha256(encoded);
}

std::optional<UserRecord> UserStore::create(std::string_view username, std::string_view password,
                                            const std::vector<std::string>& roles,
                                            const NodeId& by) {
    if (password.empty())
        return std::nullopt;
    return insert(username, password, roles, by);
}

std::optional<UserRecord> UserStore::create_without_password(
    std::string_view username, const std::vector<std::string>& roles, const NodeId& by) {
    return insert(username, {}, roles, by);
}

std::optional<UserRecord> UserStore::insert(std::string_view username, std::string_view password,
                                            const std::vector<std::string>& roles,
                                            const NodeId& by) {
    const auto normalized = normalize_username(username);
    if (normalized.empty() || normalized.size() > max_username_length)
        return std::nullopt;

    UserRecord user;
    user.id = hex(random_bytes(id_bytes));
    user.username = normalized;
    if (password.empty()) {
        // kdf 0 is "no credential": verify() refuses it before reaching a KDF,
        // so this account cannot be logged into by any password at all --
        // including one a later bug or a hand-edited record might install.
        user.kdf = 0;
    } else {
        user.kdf = 1;
        const auto salt = random_bytes(user.salt.size());
        std::copy(salt.begin(), salt.end(), user.salt.begin());
        user.kdf_n = default_kdf_n;
        user.kdf_r = default_kdf_r;
        user.kdf_p = default_kdf_p;
        user.password_hash = scrypt_hash(password, user.salt, user.kdf_n, user.kdf_r, user.kdf_p);
    }
    user.roles = expand_roles(roles);
    user.credential_generation = 1;
    user.created_unix_ms = user.updated_unix_ms = unix_ms();
    user.version = 1;
    user.updated_by = by;

    {
        std::unique_lock lock(mutex_);
        for (const auto& [_, record] : by_id_)
            if (!record.tombstone && record.username == normalized)
                return std::nullopt;
        if (by_id_.size() >= max_users_)
            return std::nullopt;
        by_id_[user.id] = user;
        persist_locked();
    }
    return user;
}

std::optional<UserRecord> UserStore::mutate(std::string_view user_id,
                                            const std::function<bool(UserRecord&)>& change,
                                            const NodeId& by) {
    std::unique_lock lock(mutex_);
    auto found = by_id_.find(std::string(user_id));
    if (found == by_id_.end() || found->second.tombstone)
        return std::nullopt;
    UserRecord next = found->second;
    if (!change(next))
        return std::nullopt;
    ++next.version;
    next.updated_unix_ms = unix_ms();
    next.updated_by = by;
    found->second = next;
    persist_locked();
    return next;
}

std::optional<UserRecord> UserStore::update(std::string_view user_id, std::string_view password,
                                            const std::optional<std::vector<std::string>>& roles,
                                            const NodeId& by) {
    // Checked here rather than only in the API, so there is one place the
    // invariant lives and no second caller can route around it.
    if (roles && std::find(roles->begin(), roles->end(), role_manage_users) == roles->end() &&
        sole_user_manager(user_id))
        return std::nullopt;
    return mutate(
        user_id,
        [&](UserRecord& user) {
            // anonymous has no password and cannot be given one; see
            // anonymous_username. Refusing the whole update rather than
            // applying the roles half keeps the caller's request and what
            // happened to it the same thing.
            if (!password.empty() && user.username == anonymous_username)
                return false;
            if (!password.empty()) {
                const auto salt = random_bytes(user.salt.size());
                std::copy(salt.begin(), salt.end(), user.salt.begin());
                user.kdf = 1;
                user.kdf_n = default_kdf_n;
                user.kdf_r = default_kdf_r;
                user.kdf_p = default_kdf_p;
                user.password_hash =
                    scrypt_hash(password, user.salt, user.kdf_n, user.kdf_r, user.kdf_p);
                // Every session minted against the old password dies as this
                // record reaches each node.
                ++user.credential_generation;
            }
            if (roles) {
                user.roles = expand_roles(*roles);
                // A session carries the roles it was minted with, so a
                // demotion that did not retire existing sessions would not
                // take effect until they expired -- up to the whole TTL. For a
                // security control that is not a delay, it is a hole.
                if (password.empty())
                    ++user.credential_generation;
            }
            return !password.empty() || roles.has_value();
        },
        by);
}

std::optional<UserRecord> UserStore::remove(std::string_view user_id, const NodeId& by) {
    // Deleting the last account that can manage accounts reaches the same
    // forbidden state as demoting it.
    if (sole_user_manager(user_id))
        return std::nullopt;
    return mutate(
        user_id,
        [](UserRecord& user) {
            user.tombstone = true;
            // The credential bump is what invalidates the deleted user's live
            // sessions; the tombstone alone would only stop new logins.
            ++user.credential_generation;
            user.password_hash = {};
            user.salt = {};
            return true;
        },
        by);
}

std::optional<RecoveryEnvelope> UserStore::root_recovery() const {
    auto root = find_by_username(root_username);
    if (!root || !root->recovery.present())
        return std::nullopt;
    return root->recovery;
}

bool UserStore::set_root_recovery(const RecoveryEnvelope& envelope, const NodeId& by) {
    auto root = find_by_username(root_username);
    if (!root)
        return false;
    return mutate(
               root->id,
               [&](UserRecord& user) {
                   user.recovery = envelope;
                   return true;
               },
               by)
        .has_value();
}

std::optional<UserRecord> UserStore::reset_root_password(std::string_view new_password,
                                                         const NodeId& by) {
    if (new_password.empty())
        return std::nullopt;
    auto root = find_by_username(root_username);
    if (!root)
        return std::nullopt;
    return mutate(
        root->id,
        [&](UserRecord& user) {
            const auto salt = random_bytes(user.salt.size());
            std::copy(salt.begin(), salt.end(), user.salt.begin());
            user.kdf = 1;
            user.kdf_n = default_kdf_n;
            user.kdf_r = default_kdf_r;
            user.kdf_p = default_kdf_p;
            user.password_hash =
                scrypt_hash(new_password, user.salt, user.kdf_n, user.kdf_r, user.kdf_p);
            // Retires every session root had, everywhere -- including any held
            // by whoever locked the operator out.
            ++user.credential_generation;
            return true;
        },
        by);
}

bool UserStore::apply(UserRecord incoming) {
    std::unique_lock lock(mutex_);
    auto found = by_id_.find(incoming.id);
    if (found != by_id_.end()) {
        if (!incoming_wins(found->second, incoming))
            return false;
        found->second = std::move(incoming);
        persist_locked();
        return true;
    }
    if (by_id_.size() >= max_users_) {
        // Never evict to make room: dropping a live account or a tombstone
        // would either lock someone out or resurrect a deleted user.
        Log::warn("user table is at capacity (" + std::to_string(max_users_) +
                  "); refusing replicated record " + incoming.id);
        return false;
    }
    auto id = incoming.id;
    by_id_.emplace(std::move(id), std::move(incoming));
    persist_locked();
    return true;
}

bool UserStore::apply_all(const std::vector<UserRecord>& values) {
    bool changed = false;
    for (const auto& value : values)
        changed = apply(value) || changed;
    return changed;
}

bool RecoveryEnvelope::present() const {
    return ephemeral_public != std::array<uint8_t, 32>{};
}

IssuedRecoveryKey issue_recovery_key(const ClusterKeys& keys) {
    // The recipient keypair. Its private half becomes the recovery key and is
    // returned to the caller; its public half is used once here and dropped.
    const auto recipient = x25519_generate();
    // A fresh ephemeral pair per envelope, so re-issuing never reuses a
    // wrap key even against the same recipient.
    const auto ephemeral = x25519_generate();
    const auto wrap = recovery_wrap_key(x25519_shared(ephemeral.private_key,
                                                      recipient.public_key));
    const auto sealed = aes_gcm_seal(wrap, keys.master, ephemeral.public_key);
    if (sealed.ciphertext.size() != 32)
        throw std::runtime_error("unexpected recovery envelope size");

    RecoveryEnvelope envelope;
    envelope.ephemeral_public = ephemeral.public_key;
    envelope.nonce = sealed.nonce;
    envelope.tag = sealed.tag;
    std::copy(sealed.ciphertext.begin(), sealed.ciphertext.end(), envelope.ciphertext.begin());
    return IssuedRecoveryKey{envelope, hex(recipient.private_key)};
}

bool recovery_key_matches(const RecoveryEnvelope& envelope, const ClusterKeys& keys,
                          std::string_view offered) {
    if (!envelope.present())
        return false;
    auto raw = unhex(std::string(offered));
    if (!raw || raw->size() != 32)
        return false;
    std::array<uint8_t, 32> secret{};
    std::copy(raw->begin(), raw->end(), secret.begin());

    try {
        const auto wrap = recovery_wrap_key(x25519_shared(secret, envelope.ephemeral_public));
        // AES-GCM's tag already rejects a wrong key; this throws rather than
        // returning plausible bytes.
        const auto plain = aes_gcm_open(wrap, envelope.nonce, envelope.tag, envelope.ciphertext,
                                        envelope.ephemeral_public);
        // Belt and braces, and not redundant: it catches an envelope that was
        // sealed against a *different* cluster key -- a stale one carried over
        // from a rebuilt cluster -- which would otherwise verify a key that no
        // longer unlocks anything.
        return plain.size() == keys.master.size() &&
               constant_time_equal(plain, keys.master);
    } catch (const std::exception&) {
        return false;
    }
}

std::string generate_password() {
    // Deliberately not base64: an operator reads this off a terminal (or a
    // systemd journal) and retypes it, so 0/O, 1/l/I and 5/S must not appear
    // together, and no vowels means it can never render as a word.
    // No vowels, so it can never render as a word; no 0/o, 1/l, and no 5
    // beside s. 26 symbols over 20 characters is ~94 bits, which is far past
    // anything scrypt then has to defend against.
    static constexpr std::string_view alphabet = "2346789bcdfghjkmnpqrstvwxyz";
    static constexpr size_t groups = 4;
    static constexpr size_t per_group = 5;
    const auto bytes = random_bytes(groups * per_group);
    std::string out;
    out.reserve(groups * (per_group + 1));
    for (size_t i = 0; i < groups * per_group; ++i) {
        if (i && i % per_group == 0)
            out.push_back('-');
        out.push_back(alphabet[bytes[i] % alphabet.size()]);
    }
    return out;
}


std::optional<InitialAccounts> create_initial_accounts(UserStore& users, const ClusterKeys& keys,
                                               const std::filesystem::path& state_path,
                                               const NodeId& by) {
    // all() rather than list(): a tombstone counts. Deleting root must not mean
    // the next restart of the founding node silently mints a fresh one with a
    // fresh password and full privileges.
    if (!users.all().empty())
        return std::nullopt;

    auto password = generate_password();
    auto root = users.create(root_username, password, all_roles(), by);
    if (!root) {
        Log::warn("could not create the root account");
        return std::nullopt;
    }
    // Recovery keys are deliberately not issued. The machinery below them
    // (issue_recovery_key / recovery_key_matches / RecoveryEnvelope) is kept
    // and tested, but nothing calls it, because in this deployment model it
    // earns nothing: the only party who could present a recovery key is the
    // operator, and the operator already has root on a node, where
    // `macha-users passwd root` does the same job without a secret that had to
    // be kept safe for months. Issuing one would mean a standing
    // unauthenticated path to the most privileged account in the cluster in
    // exchange for a capability that already exists behind strictly more
    // access.
    //
    // The code stays because the design is sound for a model that does not
    // exist yet -- an appliance, or someone else hosting the nodes -- where
    // the operator genuinely cannot get a shell. Rebuild it against that user,
    // not against a hypothetical.
    (void)keys;
    const std::string recovery_key;
    // Anonymous has no password at all. Until 0.38.4 it was given a random one
    // nobody was ever told, which was pointless in the good case and a way
    // past `allow_anonymous: false` in the bad one -- that switch guards the
    // no-credentials path, not the login path, so an anonymous account with a
    // settable password was a second door beside it.
    auto anonymous = users.create_without_password(anonymous_username,
                                                   {std::string(role_media_viewer)}, by);
    if (!anonymous) {
        Log::warn("could not create the anonymous account");
        return std::nullopt;
    }

    const auto path = state_path / "initial-root-password";
    std::string contents = "Macha genesis administrator\n\n";
    contents += "  username: " + root->username + "\n";
    contents += "  password: " + password + "\n\n";
    contents += "This account was created once, when this node founded the cluster.\n";
    contents += "Sign in, change the password, and then delete this file.\n\n";
    contents += "If this password is lost and no account holding manage_users can sign\n";
    contents += "in, reset it on any node, with the node stopped:\n\n";
    contents += "  macha-users <state_path> <cluster.key> passwd root\n";
    try {
        std::filesystem::create_directories(state_path);
        // Written before the mode is narrowed would be a window where the file
        // is world-readable, so create it closed and write into it.
        std::filesystem::path temporary = path;
        temporary += ".new";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out)
                throw std::runtime_error("cannot open " + temporary.string());
            std::filesystem::permissions(temporary,
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace);
            out << contents;
            if (!out)
                throw std::runtime_error("cannot write " + temporary.string());
        }
        std::filesystem::rename(temporary, path);
    } catch (const std::exception& error) {
        // The account exists either way; without the file the operator has no
        // way to learn the password, so say so rather than leaving them to
        // discover it at the login prompt.
        Log::error("genesis root account created, but its password could not be written to " +
                   path.string() + ": " + error.what() +
                   " -- recover with: macha-users <state_path> <cluster.key> passwd root");
        return std::nullopt;
    }

    return InitialAccounts{std::move(*root), std::move(anonymous.value()), std::move(password),
                       std::move(recovery_key), path};
}

void UserStore::persist() const {
    std::shared_lock lock(mutex_);
    persist_locked();
}

void UserStore::persist_locked() const {
    if (persisted_path_.empty())
        return;
    std::vector<UserRecord> values;
    values.reserve(by_id_.size());
    for (const auto& [_, record] : by_id_)
        values.push_back(record);
    try {
        // Password hashes replicate to every node, including one that is
        // physically offsite. Sealing does not change the trust model
        // (SECURITY.md: the cluster key already grants everything) but it does
        // mean a stolen or discarded disk is not a credential dump.
        const auto plain = encode_users(values);
        const auto sealed = aes_gcm_seal(seal_key_, plain, magic);
        Writer writer;
        writer.fixed(sealed.nonce);
        writer.fixed(sealed.tag);
        writer.bytes(sealed.ciphertext);
        const auto encoded = writer.take();
        durable_replace_file(
            persisted_path_,
            std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
    } catch (const std::exception& error) {
        Log::warn("could not persist users: " + std::string(error.what()));
    }
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
#include <fstream>

namespace macha {
namespace {
constexpr std::array<uint8_t, 8> magic_v1{'M', 'A', 'C', 'H', 'S', 'E', 'S', '1'};
constexpr std::array<uint8_t, 8> magic_v2{'M', 'A', 'C', 'H', 'S', 'E', 'S', '2'};
constexpr size_t max_roles = 64;
constexpr size_t max_role_length = 64;
constexpr size_t max_user_id_length = 128;
constexpr size_t token_bytes = 32;
constexpr size_t id_bytes = 16;

// An anonymous record is identical in both versions; v2 only appends the two
// user-identity fields, so carries_identity() decides which magic a whole
// payload needs.
bool carries_identity(const AuthSession& value) {
    return !value.user_id.empty() || value.credential_generation != 0;
}

void encode(Writer& writer, const AuthSession& value, bool v2) {
    writer.string(value.id);
    writer.fixed(value.token_hash.bytes);
    writer.u32(static_cast<uint32_t>(value.roles.size()));
    for (const auto& role : value.roles)
        writer.string(role);
    writer.u64(value.created_unix_ms);
    writer.u64(value.expires_unix_ms);
    writer.u64(value.version);
    writer.u8(value.revoked ? 1 : 0);
    if (v2) {
        writer.string(value.user_id);
        writer.u64(value.credential_generation);
    }
}

AuthSession decode(Reader& reader, bool v2) {
    AuthSession value;
    value.id = reader.string(128);
    value.token_hash.bytes = reader.fixed<32>();
    const auto role_count = reader.u32();
    if (role_count > max_roles)
        throw DecodeError("session has too many roles");
    value.roles.reserve(role_count);
    for (uint32_t i = 0; i < role_count; ++i)
        value.roles.push_back(reader.string(max_role_length));
    value.created_unix_ms = reader.u64();
    value.expires_unix_ms = reader.u64();
    value.version = reader.u64();
    value.revoked = reader.u8() != 0;
    if (v2) {
        value.user_id = reader.string(max_user_id_length);
        value.credential_generation = reader.u64();
    }
    if (value.id.empty())
        throw DecodeError("session id must be nonempty");
    return value;
}
} // namespace

bool session_has_role(const AuthSession& session, std::string_view role) {
    return std::find(session.roles.begin(), session.roles.end(), role) != session.roles.end();
}

bool session_live(const AuthSession& session, uint64_t now_unix_ms) {
    return !session.revoked && now_unix_ms < session.expires_unix_ms;
}

SessionIdentity session_identity(const AuthSession& session) {
    return SessionIdentity{session.id, session.token_hash, session.roles, session.user_id};
}

Bytes encode_sessions(const std::vector<AuthSession>& values) {
    const bool v2 = std::any_of(values.begin(), values.end(), carries_identity);
    Writer writer;
    writer.raw(v2 ? magic_v2 : magic_v1);
    writer.u32(static_cast<uint32_t>(values.size()));
    for (const auto& value : values)
        encode(writer, value, v2);
    return writer.take();
}

std::vector<AuthSession> decode_sessions(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    auto got = reader.raw(magic_v1.size());
    const bool v1 = std::equal(got.begin(), got.end(), magic_v1.begin());
    const bool v2 = std::equal(got.begin(), got.end(), magic_v2.begin());
    if (!v1 && !v2)
        throw DecodeError("bad session payload");
    const auto count = reader.u32();
    if (count > 65536)
        throw DecodeError("too many sessions");
    std::vector<AuthSession> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        values.push_back(decode(reader, v2));
    reader.finish();
    return values;
}

SessionManager::SessionManager(std::chrono::milliseconds anonymous_ttl, size_t max_sessions,
                               std::filesystem::path persisted_path)
    : anonymous_ttl_(anonymous_ttl), max_sessions_(max_sessions),
      persisted_path_(std::move(persisted_path)) {
    if (persisted_path_.empty() || !std::filesystem::exists(persisted_path_))
        return;
    try {
        const auto size = std::filesystem::file_size(persisted_path_);
        constexpr uint64_t max_persisted_bytes = 16ULL * 1024 * 1024;
        if (size > max_persisted_bytes)
            throw std::runtime_error("persisted sessions are too large");
        std::ifstream input(persisted_path_, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open persisted sessions");
        Bytes bytes(static_cast<size_t>(size));
        if (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("cannot read persisted sessions");
        const auto now = unix_ms();
        for (auto& session : decode_sessions(bytes)) {
            if (!session_live(session, now))
                continue;
            if (by_token_hash_.size() >= max_sessions_)
                break;
            const auto key = session.token_hash;
            by_token_hash_[key] = Record{std::move(session), Clock::now()};
        }
    } catch (const std::exception& error) {
        // A session cache is a durability convenience, not authoritative
        // state -- the cluster is the source of truth. Corruption or an
        // incompatible file must never prevent the node from starting.
        Log::warn("persisted sessions ignored: " + std::string(error.what()));
    }
}

std::optional<AuthSession> SessionManager::validate(std::string_view token) const {
    const auto key = sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(token.data()), token.size()));
    return find(key);
}

std::optional<AuthSession> SessionManager::find(const Hash256& token_hash) const {
    std::shared_lock lock(mutex_);
    auto found = by_token_hash_.find(token_hash);
    if (found == by_token_hash_.end())
        return std::nullopt;
    if (!session_live(found->second.session, unix_ms()))
        return std::nullopt;
    return found->second.session;
}

std::optional<MintedSession> SessionManager::create(std::vector<std::string> roles,
                                                    std::string user_id,
                                                    uint64_t credential_generation) {
    // Hash the token in the exact form it will be presented back (the hex
    // string), not the pre-hex random bytes -- validate() only ever sees the
    // former, since that's what actually crosses the wire as the bearer token.
    const auto bearer_token = hex(random_bytes(token_bytes));
    const auto id = hex(random_bytes(id_bytes));
    const auto now = unix_ms();

    AuthSession session;
    session.id = id;
    session.token_hash = sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bearer_token.data()), bearer_token.size()));
    session.roles = std::move(roles);
    session.user_id = std::move(user_id);
    session.credential_generation = credential_generation;
    session.created_unix_ms = now;
    session.expires_unix_ms = now + static_cast<uint64_t>(anonymous_ttl_.count());
    session.version = 1;

    {
        std::unique_lock lock(mutex_);
        if (by_token_hash_.size() >= max_sessions_)
            return std::nullopt;
        by_token_hash_[session.token_hash] = Record{session, Clock::now()};
    }
    return MintedSession{std::move(session), bearer_token};
}

bool SessionManager::apply(AuthSession incoming) {
    std::unique_lock lock(mutex_);
    auto found = by_token_hash_.find(incoming.token_hash);
    if (found != by_token_hash_.end()) {
        const auto& existing = found->second.session;
        if (existing.version > incoming.version)
            return false;
        if (existing.version == incoming.version && existing.revoked)
            return false; // tie: a revoked record always wins (fail closed)
        found->second = Record{std::move(incoming), Clock::now()};
        return true;
    }
    if (by_token_hash_.size() >= max_sessions_)
        return false;
    const auto key = incoming.token_hash;
    by_token_hash_[key] = Record{std::move(incoming), Clock::now()};
    return true;
}

std::optional<AuthSession> SessionManager::revoke(const Hash256& token_hash) {
    std::unique_lock lock(mutex_);
    auto found = by_token_hash_.find(token_hash);
    if (found == by_token_hash_.end())
        return std::nullopt;
    found->second.session.revoked = true;
    ++found->second.session.version;
    found->second.received = Clock::now();
    return found->second.session;
}

std::vector<AuthSession> SessionManager::recent(std::chrono::milliseconds max_age,
                                                size_t max_records) const {
    std::shared_lock lock(mutex_);
    const auto now = Clock::now();
    std::vector<AuthSession> out;
    out.reserve(std::min(by_token_hash_.size(), max_records));
    for (const auto& [_, record] : by_token_hash_) {
        if (now - record.received <= max_age)
            out.push_back(record.session);
    }
    std::sort(out.begin(), out.end(), [](const AuthSession& a, const AuthSession& b) {
        return a.version > b.version;
    });
    if (out.size() > max_records)
        out.resize(max_records);
    return out;
}

void SessionManager::prune_expired(uint64_t now_unix_ms) {
    std::unique_lock lock(mutex_);
    std::erase_if(by_token_hash_, [&](const auto& item) {
        return item.second.session.expires_unix_ms <= now_unix_ms;
    });
}

void SessionManager::persist() {
    if (persisted_path_.empty())
        return;
    std::vector<AuthSession> values;
    {
        std::shared_lock lock(mutex_);
        values.reserve(by_token_hash_.size());
        for (const auto& [_, record] : by_token_hash_)
            values.push_back(record.session);
    }
    if (values.size() > max_sessions_)
        values.resize(max_sessions_);
    const auto encoded = encode_sessions(values);
    const auto contents = std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    durable_replace_file(persisted_path_, contents);
}

} // namespace macha

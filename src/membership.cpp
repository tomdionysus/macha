// SPDX-License-Identifier: GPL-3.0-or-later
#include "membership.hpp"
#include "codec.hpp"
#include "durable_file.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <utility>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> known_magic{'M', 'A', 'C', 'H', 'M', 'E', 'M', '1'};
constexpr uint32_t max_known_nodes = 65536;
constexpr uint64_t max_known_bytes = 16ULL * 1024 * 1024;
}

Membership::Membership(NodeInfo s, std::chrono::milliseconds d, std::filesystem::path known_path)
    : self_(std::move(s)), dead_(d), known_path_(std::move(known_path)) {
    load_known();
}

void Membership::load_known() {
    if (known_path_.empty() || !std::filesystem::exists(known_path_))
        return;
    const auto size = std::filesystem::file_size(known_path_);
    if (size > max_known_bytes)
        throw std::runtime_error("known-node roster is too large");
    std::ifstream input(known_path_, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read known-node roster " + known_path_.string());
    Bytes bytes(static_cast<size_t>(size));
    if (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read known-node roster " + known_path_.string());
    Reader reader(bytes);
    if (reader.fixed<8>() != known_magic)
        throw DecodeError("bad known-node roster magic");
    const auto count = reader.u32();
    if (count > max_known_nodes)
        throw DecodeError("too many known nodes");
    const auto stale = Clock::now() - dead_ - std::chrono::milliseconds(1);
    for (uint32_t i = 0; i < count; ++i) {
        NodeInfo node;
        node.id.bytes = reader.fixed<16>();
        node.host = reader.string(4096);
        node.failure_domain = reader.string(4096);
        node.port = reader.u16();
        if (node.id == NodeId{} || node.id == self_.id || node.host.empty() || !node.port)
            throw DecodeError("bad known-node roster entry");
        if (!nodes_.emplace(node.id, R{std::move(node), stale, std::nullopt}).second)
            throw DecodeError("duplicate known-node roster entry");
    }
    reader.finish();
}

void Membership::persist_known_locked() const {
    if (known_path_.empty())
        return;
    if (nodes_.size() > max_known_nodes)
        throw std::runtime_error("too many known nodes");
    std::vector<NodeInfo> ordered;
    ordered.reserve(nodes_.size());
    for (const auto& [_, record] : nodes_)
        ordered.push_back(record.info);
    std::sort(ordered.begin(), ordered.end(), [](const NodeInfo& a, const NodeInfo& b) {
        return a.id < b.id;
    });
    Writer writer;
    writer.fixed(known_magic);
    writer.u32(static_cast<uint32_t>(ordered.size()));
    for (const auto& node : ordered) {
        writer.fixed(node.id.bytes);
        writer.string(node.host);
        writer.string(node.failure_domain);
        writer.u16(node.port);
    }
    const auto& bytes = writer.data();
    if (bytes.size() > max_known_bytes)
        throw std::runtime_error("known-node roster is too large");
    durable_replace_file(
        known_path_, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

NodeInfo Membership::self() const {
    std::lock_guard g(m_);
    auto s = self_;
    s.seen_unix_ms = unix_ms();
    return s;
}
void Membership::usage(uint64_t u) {
    std::lock_guard g(m_);
    self_.used = u;
    self_.seen_unix_ms = unix_ms();
}
void Membership::storage(uint64_t used, uint64_t capacity) {
    std::lock_guard g(m_);
    self_.used = used;
    self_.capacity = capacity;
    self_.seen_unix_ms = unix_ms();
}
void Membership::endpoint(std::string host, uint16_t port) {
    std::lock_guard g(m_);
    if (host.empty() || !port)
        throw std::runtime_error("membership endpoint must be complete");
    self_.host = std::move(host);
    self_.port = port;
    self_.seen_unix_ms = unix_ms();
}
void Membership::metadata_generation(uint64_t generation) {
    std::lock_guard g(m_);
    self_.metadata_generation = std::max(self_.metadata_generation, generation);
    self_.seen_unix_ms = unix_ms();
}
void Membership::observe(NodeInfo n, bool direct) {
    if (n.id == self_.id || n.host.empty() || !n.port)
        return;
    std::lock_guard g(m_);
    for (const auto& [_, reset] : identity_resets_) {
        if (!identity_reset_matches_endpoint(reset, n.host, n.port) ||
            !identity_reset_matches_node(reset, n.id))
            continue;
        // A reset is a freshness boundary, not a permanent blacklist. Stale
        // gossip cannot recreate the invalidated association, while a directly
        // authenticated peer may establish it again. Once that fresh observation
        // propagates with a post-reset seen time, ordinary gossip is valid too.
        if (!direct && n.seen_unix_ms <= reset.reset_unix_ms)
            return;
    }
    auto now = Clock::now();
    auto i = nodes_.find(n.id);
    bool durable_roster_changed = false;
    if (i == nodes_.end()) {
        R record{std::move(n), now, direct ? std::optional<Clock::time_point>(now) : std::nullopt};
        nodes_.emplace(record.info.id, std::move(record));
        durable_roster_changed = true;
    } else {
        const bool association_changed = i->second.info.host != n.host ||
                                         i->second.info.port != n.port ||
                                         i->second.info.failure_domain != n.failure_domain;
        if (direct || n.seen_unix_ms > i->second.info.seen_unix_ms) {
            i->second.info = std::move(n);
            i->second.seen = now;
        }
        if (direct)
            i->second.direct_seen = now;
        durable_roster_changed = association_changed;
    }
    if (durable_roster_changed)
        persist_known_locked();
}

bool Membership::apply_identity_reset(const IdentityAssociationReset& reset) {
    if (reset.host.empty() || !reset.epoch)
        return false;
    std::lock_guard g(m_);
    const auto key = identity_reset_key(reset.host, reset.port);
    auto found = identity_resets_.find(key);
    if (found != identity_resets_.end() && found->second.epoch >= reset.epoch)
        return false;
    identity_resets_[key] = reset;
    const auto before = nodes_.size();
    std::erase_if(nodes_, [&](const auto& item) {
        const auto& info = item.second.info;
        return identity_reset_matches_endpoint(reset, info.host, info.port) &&
               identity_reset_matches_node(reset, info.id);
    });
    if (nodes_.size() != before)
        persist_known_locked();
    return true;
}

std::vector<IdentityAssociationReset> Membership::identity_resets() const {
    std::lock_guard g(m_);
    std::vector<IdentityAssociationReset> out;
    out.reserve(identity_resets_.size());
    for (const auto& [_, reset] : identity_resets_)
        out.push_back(reset);
    return out;
}
MembershipSnapshot Membership::snapshot() const {
    std::lock_guard g(m_);
    const auto now = Clock::now();
    const auto seen = unix_ms();
    MembershipSnapshot out;
    out.all.reserve(nodes_.size() + 1);
    out.active.reserve(nodes_.size() + 1);
    auto self = self_;
    self.seen_unix_ms = seen;
    out.all.push_back(self);
    out.active.push_back(std::move(self));
    for (const auto& [_, record] : nodes_) {
        out.all.push_back(record.info);
        if (now - record.seen <= dead_)
            out.active.push_back(record.info);
    }
    return out;
}

std::vector<NodeInfo> Membership::all() const {
    std::lock_guard g(m_);
    std::vector<NodeInfo> out{self_};
    out[0].seen_unix_ms = unix_ms();
    out.reserve(nodes_.size() + 1);
    for (const auto& [_, record] : nodes_)
        out.push_back(record.info);
    return out;
}

std::vector<NodeInfo> Membership::active() const {
    std::lock_guard g(m_);
    const auto now = Clock::now();
    std::vector<NodeInfo> out{self_};
    out[0].seen_unix_ms = unix_ms();
    out.reserve(nodes_.size() + 1);
    for (const auto& [_, record] : nodes_)
        if (now - record.seen <= dead_)
            out.push_back(record.info);
    return out;
}

bool Membership::all_known_reachable() const {
    std::lock_guard g(m_);
    const auto now = Clock::now();
    return std::all_of(nodes_.begin(), nodes_.end(), [&](const auto& item) {
        return item.second.direct_seen && now - *item.second.direct_seen <= dead_;
    });
}
} // namespace macha

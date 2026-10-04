// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/membership.hpp"
#include "codec.hpp"
#include "durable_file.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <utility>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> known_magic_v1{'M', 'A', 'C', 'H', 'M', 'E', 'M', '1'};
constexpr std::array<uint8_t, 8> known_magic_v2{'M', 'A', 'C', 'H', 'M', 'E', 'M', '2'};
// v3 appends NodeInfo::flags to every entry, so a restarting node knows which
// peers it must not dial before it has heard from anyone.
constexpr std::array<uint8_t, 8> known_magic_v3{'M', 'A', 'C', 'H', 'M', 'E', 'M', '3'};
// v4 appends this node's wall clock when it last heard of each entry.
constexpr std::array<uint8_t, 8> known_magic_v4{'M', 'A', 'C', 'H', 'M', 'E', 'M', '4'};
// How far a node's last-heard time may move before the roster is rewritten.
constexpr uint64_t seen_persist_step_ms = 60ULL * 60 * 1000;
constexpr uint32_t max_known_nodes = 65536;
constexpr uint32_t max_identity_resets = 65536;
constexpr uint64_t max_known_bytes = 16ULL * 1024 * 1024;
}

Membership::Membership(NodeInfo s, std::chrono::milliseconds d, std::filesystem::path known_path,
                       std::chrono::milliseconds forget_after)
    // Never sooner than a node is given up for dead.
    : self_(std::move(s)), dead_(d), forget_after_(std::max(forget_after, 2 * d)),
      known_path_(std::move(known_path)) {
    Lock lock(m_);
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
    const auto magic = reader.fixed<8>();
    const bool version4 = magic == known_magic_v4;
    const bool version3 = version4 || magic == known_magic_v3;
    const bool version2 = version3 || magic == known_magic_v2;
    if (magic != known_magic_v1 && !version2)
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
        if (version2)
            node.seen_unix_ms = reader.u64();
        // v1/v2 carry no flags: NodeInfo's default (dialable storage node) applies.
        if (version3)
            node.flags = reader.u8();
        // An older roster has no last-heard time: the wait starts now.
        const uint64_t last_seen = version4 ? reader.u64() : unix_ms();
        if (node.id == NodeId{} || node.id == self_.id || node.host.empty() || !node.port)
            throw DecodeError("bad known-node roster entry");
        if (!nodes_.emplace(node.id, R{std::move(node), stale, std::nullopt, last_seen, last_seen})
                 .second)
            throw DecodeError("duplicate known-node roster entry");
    }
    if (version2) {
        const auto reset_count = reader.u32();
        if (reset_count > max_identity_resets)
            throw DecodeError("too many identity association resets");
        for (uint32_t i = 0; i < reset_count; ++i) {
            IdentityAssociationReset reset;
            reset.host = reader.string(4096);
            reset.port = reader.u16();
            reset.stale_node_id.bytes = reader.fixed<16>();
            reset.epoch = reader.u64();
            reset.reset_unix_ms = reader.u64();
            reset.reset_by.bytes = reader.fixed<16>();
            reset.reason = reader.string(4096);
            if (reset.host.empty() || !reset.epoch)
                throw DecodeError("bad identity association reset");
            if (!identity_resets_.emplace(identity_reset_key(reset.host, reset.port),
                                          std::move(reset)).second)
                throw DecodeError("duplicate identity association reset");
        }
        std::erase_if(nodes_, [&](const auto& item) MACHA_REQUIRES(m_) {
            const auto& node = item.second.info;
            return std::any_of(identity_resets_.begin(), identity_resets_.end(),
                               [&](const auto& reset_item) {
                                   const auto& reset = reset_item.second;
                                   return identity_reset_matches_endpoint(reset, node.host,
                                                                          node.port) &&
                                          identity_reset_matches_node(reset, node.id) &&
                                          node.seen_unix_ms <= reset.reset_unix_ms;
                               });
        });
    }
    reader.finish();
}

void Membership::persist_known_locked() {
    if (known_path_.empty())
        return;
    if (nodes_.size() > max_known_nodes)
        throw std::runtime_error("too many known nodes");
    std::vector<const R*> ordered;
    ordered.reserve(nodes_.size());
    for (const auto& [_, record] : nodes_)
        ordered.push_back(&record);
    std::sort(ordered.begin(), ordered.end(), [](const R* a, const R* b) {
        return a->info.id < b->info.id;
    });
    Writer writer;
    writer.fixed(known_magic_v4);
    writer.u32(static_cast<uint32_t>(ordered.size()));
    for (const auto* record : ordered) {
        const auto& node = record->info;
        writer.fixed(node.id.bytes);
        writer.string(node.host);
        writer.string(node.failure_domain);
        writer.u16(node.port);
        writer.u64(node.seen_unix_ms);
        writer.u8(node.flags);
        writer.u64(record->last_seen_unix_ms);
    }
    if (identity_resets_.size() > max_identity_resets)
        throw std::runtime_error("too many identity association resets");
    std::vector<std::pair<std::string, IdentityAssociationReset>> ordered_resets;
    ordered_resets.reserve(identity_resets_.size());
    for (const auto& item : identity_resets_)
        ordered_resets.push_back(item);
    std::sort(ordered_resets.begin(), ordered_resets.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    writer.u32(static_cast<uint32_t>(ordered_resets.size()));
    for (const auto& [_, reset] : ordered_resets) {
        writer.string(reset.host);
        writer.u16(reset.port);
        writer.fixed(reset.stale_node_id.bytes);
        writer.u64(reset.epoch);
        writer.u64(reset.reset_unix_ms);
        writer.fixed(reset.reset_by.bytes);
        writer.string(reset.reason);
    }
    const auto& bytes = writer.data();
    if (bytes.size() > max_known_bytes)
        throw std::runtime_error("known-node roster is too large");
    durable_replace_file(
        known_path_, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    for (auto& [_, record] : nodes_)
        record.persisted_seen_unix_ms = record.last_seen_unix_ms;
}

NodeInfo Membership::self() const {
    Lock g(m_);
    auto s = self_;
    s.seen_unix_ms = unix_ms();
    return s;
}
void Membership::usage(uint64_t u) {
    Lock g(m_);
    self_.used = u;
    self_.seen_unix_ms = unix_ms();
}
void Membership::storage(uint64_t used, uint64_t capacity) {
    Lock g(m_);
    self_.used = used;
    self_.capacity = capacity;
    self_.seen_unix_ms = unix_ms();
}
void Membership::endpoint(std::string host, uint16_t port) {
    Lock g(m_);
    if (host.empty() || !port)
        throw std::runtime_error("membership endpoint must be complete");
    self_.host = std::move(host);
    self_.port = port;
    self_.seen_unix_ms = unix_ms();
}
void Membership::metadata_generation(uint64_t generation) {
    Lock g(m_);
    self_.metadata_generation = std::max(self_.metadata_generation, generation);
    self_.seen_unix_ms = unix_ms();
}
bool Membership::set_flags(bool inbound_capable, bool hosts_extents) {
    Lock g(m_);
    const auto flags = node_flags_for(inbound_capable, hosts_extents);
    if (self_.flags == flags)
        return false;
    self_.flags = flags;
    self_.seen_unix_ms = unix_ms();
    return true;
}
bool Membership::inbound_capable(const NodeId& id) const {
    Lock g(m_);
    if (id == self_.id)
        return node_inbound_capable(self_);
    const auto found = nodes_.find(id);
    return found == nodes_.end() || node_inbound_capable(found->second.info);
}
bool Membership::hosts_extents(const NodeId& id) const {
    Lock g(m_);
    if (id == self_.id)
        return node_hosts_extents(self_);
    const auto found = nodes_.find(id);
    return found == nodes_.end() || node_hosts_extents(found->second.info);
}
void Membership::observe(NodeInfo n, bool direct) {
    if (n.host.empty() || !n.port)
        return;
    Lock g(m_);
    if (n.id == self_.id)
        return;
    for (const auto& [_, reset] : identity_resets_) {
        if (!identity_reset_matches_endpoint(reset, n.host, n.port) ||
            !identity_reset_matches_node(reset, n.id))
            continue;
        // A reset is a freshness boundary, not a blacklist: only a direct
        // observation or gossip seen after the reset re-establishes the node.
        if (!direct && n.seen_unix_ms <= reset.reset_unix_ms)
            return;
    }
    auto now = Clock::now();
    const auto wall = unix_ms();
    auto i = nodes_.find(n.id);
    bool durable_roster_changed = false;
    if (i == nodes_.end()) {
        // Gossip about a node nobody has heard from for the forgetting
        // horizon would only teach back what was forgotten.
        if (!direct && wall >= n.seen_unix_ms &&
            wall - n.seen_unix_ms >= static_cast<uint64_t>(forget_after_.count()))
            return;
        R record{std::move(n), now, direct ? std::optional<Clock::time_point>(now) : std::nullopt,
                 wall, 0};
        nodes_.emplace(record.info.id, std::move(record));
        durable_roster_changed = true;
    } else {
        const bool association_changed = i->second.info.host != n.host ||
                                         i->second.info.port != n.port ||
                                         i->second.info.failure_domain != n.failure_domain ||
                                         i->second.info.flags != n.flags;
        // A direct observation carries the peer's handshake-time NodeInfo,
        // which gossip may have superseded (flags included, which drive
        // placement). The newer record wins; direct only refreshes liveness.
        const bool newer = n.seen_unix_ms > i->second.info.seen_unix_ms;
        if (newer) {
            i->second.info = std::move(n);
            i->second.seen = now;
            durable_roster_changed = association_changed;
        } else if (direct) {
            i->second.seen = now;
        }
        if (direct)
            i->second.direct_seen = now;
        if (newer || direct) {
            i->second.last_seen_unix_ms = wall;
            durable_roster_changed =
                durable_roster_changed ||
                wall - i->second.persisted_seen_unix_ms >= seen_persist_step_ms;
        }
    }
    if (durable_roster_changed)
        persist_known_locked();
}

bool Membership::apply_identity_reset(const IdentityAssociationReset& reset) {
    if (reset.host.empty() || !reset.epoch)
        return false;
    Lock g(m_);
    const auto key = identity_reset_key(reset.host, reset.port);
    auto found = identity_resets_.find(key);
    if (found != identity_resets_.end() && found->second.epoch >= reset.epoch)
        return false;
    identity_resets_[key] = reset;
    std::erase_if(nodes_, [&](const auto& item) {
        const auto& info = item.second.info;
        return identity_reset_matches_endpoint(reset, info.host, info.port) &&
               identity_reset_matches_node(reset, info.id);
    });
    // Persist the tombstone even when the member is absent from the roster, so
    // the reset survives a restart and works without cluster metadata.
    persist_known_locked();
    return true;
}

std::vector<IdentityAssociationReset> Membership::identity_resets() const {
    Lock g(m_);
    std::vector<IdentityAssociationReset> out;
    out.reserve(identity_resets_.size());
    for (const auto& [_, reset] : identity_resets_)
        out.push_back(reset);
    return out;
}
MembershipSnapshot Membership::snapshot() const {
    Lock g(m_);
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
    Lock g(m_);
    std::vector<NodeInfo> out{self_};
    out[0].seen_unix_ms = unix_ms();
    out.reserve(nodes_.size() + 1);
    for (const auto& [_, record] : nodes_)
        out.push_back(record.info);
    return out;
}

std::vector<NodeInfo> Membership::active() const {
    Lock g(m_);
    const auto now = Clock::now();
    std::vector<NodeInfo> out{self_};
    out[0].seen_unix_ms = unix_ms();
    out.reserve(nodes_.size() + 1);
    for (const auto& [_, record] : nodes_)
        if (now - record.seen <= dead_)
            out.push_back(record.info);
    return out;
}

bool Membership::directly_reachable(const NodeId& id) const {
    Lock g(m_);
    if (id == self_.id)
        return true;
    const auto found = nodes_.find(id);
    return found != nodes_.end() && found->second.direct_seen &&
           Clock::now() - *found->second.direct_seen <= dead_;
}

size_t Membership::forget_unseen() {
    Lock g(m_);
    const auto horizon = forget_after_;
    const auto now = Clock::now();
    const auto wall = unix_ms();
    const auto forgotten = std::erase_if(nodes_, [&](const auto& item) {
        const auto& record = item.second;
        return now - record.seen > dead_ && wall >= record.last_seen_unix_ms &&
               wall - record.last_seen_unix_ms >= static_cast<uint64_t>(horizon.count());
    });
    if (forgotten)
        persist_known_locked();
    return forgotten;
}

bool Membership::all_known_reachable() const {
    Lock g(m_);
    const auto now = Clock::now();
    const bool self_capable = node_inbound_capable(self_);
    return std::all_of(nodes_.begin(), nodes_.end(), [&](const auto& item) {
        if (!self_capable && !node_inbound_capable(item.second.info))
            return true; // Neither side can be dialled: not a fault, no fence.
        return item.second.direct_seen && now - *item.second.direct_seen <= dead_;
    });
}
} // namespace macha

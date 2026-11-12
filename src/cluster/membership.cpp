// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/membership.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace macha {

Membership::Membership(NodeInfo s, std::chrono::milliseconds d) : self_(std::move(s)), dead_(d) {}

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
void Membership::follow(std::shared_ptr<const MembershipView> view) {
    Lock g(m_);
    view_ = std::move(view);
}

std::shared_ptr<const MembershipView> Membership::view() const {
    Lock g(m_);
    return view_;
}

namespace {
const NodeInfo* find_peer(const std::vector<NodeInfo>& nodes, const NodeId& id) {
    for (const auto& node : nodes)
        if (node.id == id)
            return &node;
    return nullptr;
}
} // namespace

bool Membership::inbound_capable(const NodeId& id) const {
    if (const auto followed = view()) {
        if (id == self().id)
            return node_inbound_capable(self());
        const auto peers = followed->peers();
        const auto* found = find_peer(peers.all, id);
        return !found || node_inbound_capable(*found);
    }
    Lock g(m_);
    if (id == self_.id)
        return node_inbound_capable(self_);
    const auto found = nodes_.find(id);
    return found == nodes_.end() || node_inbound_capable(found->second.info);
}
bool Membership::hosts_extents(const NodeId& id) const {
    if (const auto followed = view()) {
        if (id == self().id)
            return node_hosts_extents(self());
        const auto peers = followed->peers();
        const auto* found = find_peer(peers.all, id);
        return !found || node_hosts_extents(*found);
    }
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
    if (view_ || n.id == self_.id)
        return;
    const auto now = Clock::now();
    auto i = nodes_.find(n.id);
    if (i == nodes_.end()) {
        R record{std::move(n), now, direct ? std::optional<Clock::time_point>(now) : std::nullopt};
        nodes_.emplace(record.info.id, std::move(record));
        return;
    }
    if (n.seen_unix_ms > i->second.info.seen_unix_ms) {
        i->second.info = std::move(n);
        i->second.seen = now;
    } else if (direct) {
        i->second.seen = now;
    }
    if (direct)
        i->second.direct_seen = now;
}

MembershipSnapshot Membership::snapshot() const {
    if (const auto followed = view()) {
        auto peers = followed->peers();
        auto self_now = self();
        self_now.seen_unix_ms = unix_ms();
        MembershipSnapshot out;
        out.all.reserve(peers.all.size() + 1);
        out.active.reserve(peers.active.size() + 1);
        out.all.push_back(self_now);
        out.active.push_back(std::move(self_now));
        for (auto& node : peers.all)
            out.all.push_back(std::move(node));
        for (auto& node : peers.active)
            out.active.push_back(std::move(node));
        return out;
    }
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
    if (view())
        return snapshot().all;
    Lock g(m_);
    std::vector<NodeInfo> out{self_};
    out[0].seen_unix_ms = unix_ms();
    out.reserve(nodes_.size() + 1);
    for (const auto& [_, record] : nodes_)
        out.push_back(record.info);
    return out;
}

std::vector<NodeInfo> Membership::active() const {
    if (view())
        return snapshot().active;
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
    if (const auto followed = view())
        return id == self().id || followed->directly_reachable(id);
    Lock g(m_);
    if (id == self_.id)
        return true;
    const auto found = nodes_.find(id);
    if (found == nodes_.end())
        return false;
    const auto& direct_seen = found->second.direct_seen;
    return direct_seen && Clock::now() - *direct_seen <= dead_;
}

bool Membership::all_known_reachable() const {
    if (const auto followed = view())
        return followed->all_known_reachable();
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

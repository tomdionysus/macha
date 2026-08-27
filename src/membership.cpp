// SPDX-License-Identifier: GPL-3.0-or-later
#include "membership.hpp"
#include <algorithm>
namespace macha {
Membership::Membership(NodeInfo s, std::chrono::milliseconds d) : self_(std::move(s)), dead_(d) {}
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
    if (i == nodes_.end())
        nodes_.emplace(n.id, R{std::move(n), now});
    else if (direct || n.seen_unix_ms > i->second.info.seen_unix_ms) {
        i->second = {std::move(n), now};
    }
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
    std::erase_if(nodes_, [&](const auto& item) {
        const auto& info = item.second.info;
        return identity_reset_matches_endpoint(reset, info.host, info.port) &&
               identity_reset_matches_node(reset, info.id);
    });
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
} // namespace macha

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
    auto now = Clock::now();
    auto i = nodes_.find(n.id);
    if (i == nodes_.end())
        nodes_.emplace(n.id, R{std::move(n), now});
    else if (direct || n.seen_unix_ms > i->second.info.seen_unix_ms) {
        i->second = {std::move(n), now};
    }
}
std::vector<NodeInfo> Membership::all() const {
    std::lock_guard g(m_);
    std::vector<NodeInfo> o{self_};
    o[0].seen_unix_ms = unix_ms();
    for (auto& [_, r] : nodes_)
        o.push_back(r.info);
    return o;
}
std::vector<NodeInfo> Membership::active() const {
    std::lock_guard g(m_);
    auto now = Clock::now();
    std::vector<NodeInfo> o{self_};
    o[0].seen_unix_ms = unix_ms();
    for (auto& [_, r] : nodes_)
        if (now - r.seen <= dead_)
            o.push_back(r.info);
    return o;
}
} // namespace macha

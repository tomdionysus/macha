// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"
#include "types.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
namespace macha {

struct MembershipSnapshot {
    std::vector<NodeInfo> all;
    std::vector<NodeInfo> active;
};

// The roster as the node beneath Macha holds it. While Membership follows one, its peers,
// their liveness and reachability are the view's, and only this node's own entry is kept
// here.
class MembershipView {
  public:
    virtual ~MembershipView() = default;
    // Every known peer and those active, this node excluded.
    virtual MembershipSnapshot peers() const = 0;
    virtual bool directly_reachable(const NodeId&) const = 0;
    virtual bool all_known_reachable() const = 0;
};

// The cluster's nodes as Macha reads them: this node's own entry and figures, and its
// peers from the node beneath (a view). A node built without one (a test's fake node)
// keeps the peers it is told of itself, each active for dead_after from when it was last
// heard of.
class Membership {
    struct R {
        NodeInfo info;
        Clock::time_point seen;
        std::optional<Clock::time_point> direct_seen;
    };
    mutable Mutex m_;
    NodeInfo self_ MACHA_GUARDED_BY(m_);
    const std::chrono::milliseconds dead_;
    std::unordered_map<NodeId, R, NodeIdHash> nodes_ MACHA_GUARDED_BY(m_);
    std::shared_ptr<const MembershipView> view_ MACHA_GUARDED_BY(m_);

    std::shared_ptr<const MembershipView> view() const;

  public:
    Membership(NodeInfo, std::chrono::milliseconds dead_after);
    // From here the peers are the view's, and observe does nothing. Set before the
    // node starts.
    void follow(std::shared_ptr<const MembershipView>);
    NodeInfo self() const;
    void usage(uint64_t);
    void storage(uint64_t used, uint64_t capacity);
    void endpoint(std::string host, uint16_t port);
    void metadata_generation(uint64_t);
    // Sets this node's NodeInfo::flags bits; true when either changed.
    bool set_flags(bool inbound_capable, bool hosts_extents);
    // As the node reads them (self included); unknown ids report true.
    bool inbound_capable(const NodeId&) const;
    bool hosts_extents(const NodeId&) const;
    // Without a view: a peer this node is told of. The newer record wins; a direct
    // observation also refreshes its liveness.
    void observe(NodeInfo, bool direct = false);
    MembershipSnapshot snapshot() const;
    std::vector<NodeInfo> all() const;
    std::vector<NodeInfo> active() const;
    // Authenticated directly within dead_after; gossip does not count.
    bool directly_reachable(const NodeId&) const;
    // Every known node authenticated directly within dead_after (gossip does
    // not count). Pairs of nodes that both refuse inbound connections cannot
    // authenticate each other and are excluded.
    bool all_known_reachable() const;
};
} // namespace macha

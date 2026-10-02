// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
namespace macha {

struct MembershipSnapshot {
    std::vector<NodeInfo> all;
    std::vector<NodeInfo> active;
};

class Membership {
    struct R {
        NodeInfo info;
        Clock::time_point seen;
        std::optional<Clock::time_point> direct_seen;
    };
    mutable std::mutex m_;
    NodeInfo self_;
    std::chrono::milliseconds dead_;
    std::filesystem::path known_path_;
    std::unordered_map<NodeId, R, NodeIdHash> nodes_;
    std::unordered_map<std::string, IdentityAssociationReset> identity_resets_;

    void load_known();
    void persist_known_locked() const;

  public:
    Membership(NodeInfo, std::chrono::milliseconds, std::filesystem::path known_path = {});
    NodeInfo self() const;
    void usage(uint64_t);
    void storage(uint64_t used, uint64_t capacity);
    void endpoint(std::string host, uint16_t port);
    void metadata_generation(uint64_t);
    // Sets the gossiped NodeInfo::flags bits; true when either changed.
    bool set_flags(bool inbound_capable, bool hosts_extents);
    // As gossiped (self included); unknown ids report true.
    bool inbound_capable(const NodeId&) const;
    bool hosts_extents(const NodeId&) const;
    void observe(NodeInfo, bool direct = false);
    bool apply_identity_reset(const IdentityAssociationReset&);
    std::vector<IdentityAssociationReset> identity_resets() const;
    MembershipSnapshot snapshot() const;
    std::vector<NodeInfo> all() const;
    std::vector<NodeInfo> active() const;

    // Gates destructive GC: every durably-known node authenticated directly
    // within dead_after (gossip does not count), so a node loaded from disk
    // fences GC until contacted. Pairs of nodes that both refuse inbound
    // connections cannot authenticate each other and are excluded.
    bool all_known_reachable() const;
};
} // namespace macha

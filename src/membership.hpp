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
    void observe(NodeInfo, bool direct = false);
    bool apply_identity_reset(const IdentityAssociationReset&);
    std::vector<IdentityAssociationReset> identity_resets() const;
    MembershipSnapshot snapshot() const;
    std::vector<NodeInfo> all() const;
    std::vector<NodeInfo> active() const;

    // Destructive GC is allowed only when every durably-known node has been
    // authenticated directly within dead_after. Gossip freshness deliberately
    // does not satisfy this predicate. A node loaded from disk after restart is
    // therefore a GC fence until it has been contacted again.
    bool all_known_reachable() const;
};
} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.hpp"
#include <mutex>
#include <unordered_map>
namespace macha {
class Membership {
    struct R {
        NodeInfo info;
        Clock::time_point seen;
    };
    mutable std::mutex m_;
    NodeInfo self_;
    std::chrono::milliseconds dead_;
    std::unordered_map<NodeId, R, NodeIdHash> nodes_;
    std::unordered_map<std::string, IdentityAssociationReset> identity_resets_;

  public:
    Membership(NodeInfo, std::chrono::milliseconds);
    NodeInfo self() const;
    void usage(uint64_t);
    void storage(uint64_t used, uint64_t capacity);
    void metadata_generation(uint64_t);
    void observe(NodeInfo, bool direct = false);
    bool apply_identity_reset(const IdentityAssociationReset&);
    std::vector<IdentityAssociationReset> identity_resets() const;
    std::vector<NodeInfo> all() const;
    std::vector<NodeInfo> active() const;
};
} // namespace macha

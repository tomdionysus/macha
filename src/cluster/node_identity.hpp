// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crypto.hpp"
#include "types.hpp"

#include <dannan/context.h>

#include <filesystem>

namespace macha {

// Who this node is: the cluster keys, its durable id and this process's
// durability epoch (fresh on every start, so peers can tell a restart from a
// reconnect). Read-only once built.
struct NodeIdentity {
    // The Dannan node's: one id file and one epoch per process, read under the
    // node's state lock.
    NodeIdentity(ClusterKeys keys, const dannan::Identity& node);
    // From the state path alone, under the caller's lock: Macha's own store's
    // layout marker and id file (its fixture, until that store goes).
    NodeIdentity(const std::filesystem::path& state_path, ClusterKeys keys);

    ClusterKeys keys;
    NodeId id;
    NodeId durability_epoch;
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crypto.hpp"
#include "types.hpp"

#include <filesystem>

namespace macha {

// Who this node is: the cluster keys, its durable id (kept under the state
// path, created on first start) and this process's durability epoch (fresh on
// every start, so peers can tell a restart from a reconnect). Built by the
// root while it holds the state path's lock; read-only afterwards.
struct NodeIdentity {
    NodeIdentity(const std::filesystem::path& state_path, ClusterKeys keys);

    ClusterKeys keys;
    NodeId id;
    NodeId durability_epoch;
};

} // namespace macha

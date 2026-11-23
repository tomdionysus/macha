// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crypto.hpp"
#include "types.hpp"

#include <dannan/context.h>

namespace macha {

// Who this node is: the cluster keys and its durable id. Read-only once built.
struct NodeIdentity {
    // The Dannan node's id, read under the node's state lock.
    NodeIdentity(ClusterKeys keys, const dannan::Identity& node);

    ClusterKeys keys;
    NodeId id;
};

} // namespace macha

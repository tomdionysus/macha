// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crypto.hpp"
#include "types.hpp"

#include <dannan/context.h>

namespace macha {

// Who this node is: the cluster keys, its durable id and this process's
// durability epoch (fresh on every start, so peers can tell a restart from a
// reconnect). Read-only once built.
struct NodeIdentity {
    // The Dannan node's: one id file and one epoch per process, read under the
    // node's state lock.
    NodeIdentity(ClusterKeys keys, const dannan::Identity& node);

    ClusterKeys keys;
    NodeId id;
    NodeId durability_epoch;
};

} // namespace macha

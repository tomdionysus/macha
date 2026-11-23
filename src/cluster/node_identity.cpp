// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/node_identity.hpp"

namespace macha {

NodeIdentity::NodeIdentity(ClusterKeys cluster_keys, const dannan::Identity& node)
    : keys(std::move(cluster_keys)), id(node.id()) {}

} // namespace macha

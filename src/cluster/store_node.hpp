// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "crypto.hpp"

#include "dagda/node.h"

#include <chrono>

namespace macha {

// The store's options from Macha's configuration: every store key mapped onto
// Dagda's node, and each default where Dagda's differs from Macha's set to
// Macha's. The node runs under Macha's loop (maintenance not automatic), with
// heads off while Macha's replica keeps the metadata, and with no device
// reserve. `hosts_extents` is the node's current inbound answer; it follows
// the answer at run time through the server.
dagda::NodeOptions store_node_options(const Config& config, const ClusterKeys& keys,
                                      bool hosts_extents,
                                      std::chrono::milliseconds durability_batch_window);

} // namespace macha

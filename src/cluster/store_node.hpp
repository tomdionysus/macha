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

// The bytes repair may have in flight, whatever it takes the link to be: an
// eighth of the retained-memory budget, at least one extent. Resident, that is
// the window queued to send plus about an extent being read.
uint64_t repair_window_bytes(const Config& config);

// What a live reload changes on the store node: its DATA backends and its
// cache, from the same keys as its options.
dagda::NodeReload store_node_reload(const Config& config);

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/activity_clocks.hpp"
#include "cluster/data_work.hpp"
#include "cluster/transcode_rates.hpp"
#include "config.hpp"
#include "retained_memory.hpp"
#include "storage/local_store.hpp"

namespace macha {

// The node-wide budgets and books every layer shares, built by the root before
// the node and handed out by reference. Members are in dependency order; the
// owner calls stop() before stopping anything that waits on them.
class NodeResources {
  public:
    NodeResources(const Config&, ActivityClocks::Source activity_clock = {});
    NodeResources(const NodeResources&) = delete;
    NodeResources& operator=(const NodeResources&) = delete;

    // Held first: no state file is read before this process owns state_path.
    StorageLock state_lock;
    ActivityClocks activity;
    // Law 3: viewer presence reads `activity` over maintenance.foreground_quiet,
    // since playback holds no byte credit between extents.
    DataResourceArbiter data;
    RetainedMemoryLedger memory;
    TranscodeRateBook transcode_rates;

    // Refuses new leases and wakes every waiter. Idempotent.
    void stop();
};

} // namespace macha

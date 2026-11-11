// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/activity_clocks.hpp"
#include "cluster/data_work.hpp"
#include "cluster/node_events.hpp"
#include "cluster/transcode_rates.hpp"
#include "config.hpp"
#include "retained_memory.hpp"

#include <dannan/sched/retained_memory.h>

namespace macha {

// What the root builds before the node: the books the node writes to, read and
// counted by every layer after it. Built under the state path's lock.
struct NodeBooks {
    explicit NodeBooks(const Config&);
    NodeBooks(const NodeBooks&) = delete;
    NodeBooks& operator=(const NodeBooks&) = delete;

    TranscodeRateBook transcode_rates;
    NodeEvents events;
};

// The node-wide budgets every layer shares, built by the root after the node and
// handed out by reference: retained memory is the node's own ledger, so the
// transport and Macha's holders share one total. Members are in dependency order;
// the owner calls stop() before stopping anything that waits on them.
class NodeResources {
  public:
    NodeResources(const Config&, NodeBooks&, dannan::sched::RetainedMemoryLedger& node_memory,
                  ActivityClocks::Source activity_clock = {});
    NodeResources(const NodeResources&) = delete;
    NodeResources& operator=(const NodeResources&) = delete;

    ActivityClocks activity;
    // Law 3: viewer presence reads `activity` over maintenance.foreground_quiet,
    // since playback holds no byte credit between extents.
    DataResourceArbiter data;
    RetainedMemoryLedger memory;
    TranscodeRateBook& transcode_rates;
    NodeEvents& events;

    // Refuses new leases and wakes every waiter. Idempotent.
    void stop();
};

// A ledger sized from the configuration, for parts built without a node (tests and
// tools); a node's is its own.
std::unique_ptr<dannan::sched::RetainedMemoryLedger> standalone_memory(const Config&);

} // namespace macha

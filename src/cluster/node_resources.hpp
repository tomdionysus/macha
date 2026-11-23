// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/node_events.hpp"
#include "cluster/transcode_rates.hpp"
#include "config.hpp"
#include "retained_memory.hpp"

#include <dannan/sched/retained_memory.h>

namespace macha {

// What the root builds before the node: the books the node writes to, read and
// counted by every layer after it. Built before the state path is locked, so
// reading nothing; the root opens them once the node holds the lock.
struct NodeBooks {
    explicit NodeBooks(const Config&);
    NodeBooks(const NodeBooks&) = delete;
    NodeBooks& operator=(const NodeBooks&) = delete;

    TranscodeRateBook transcode_rates;
};

// The node-wide budgets every layer shares, built by the root after the node and
// handed out by reference: retained memory is the node's own ledger, so the
// transport and Macha's holders share one total. Members are in dependency order;
// the owner calls stop() before stopping anything that waits on them.
class NodeResources {
  public:
    NodeResources(NodeBooks&, dannan::sched::RetainedMemoryLedger& node_memory,
                  NodeEvents& node_events);
    NodeResources(const NodeResources&) = delete;
    NodeResources& operator=(const NodeResources&) = delete;

    RetainedMemoryLedger memory;
    TranscodeRateBook& transcode_rates;
    // The node's, counted by its Dannan node.
    NodeEvents& events;

    // Refuses new leases and wakes every waiter. Idempotent.
    void stop();
};

} // namespace macha

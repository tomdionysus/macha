// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/node_resources.hpp"

namespace macha {

NodeBooks::NodeBooks(const Config& cfg)
    : transcode_rates(cfg.state_path / "playback" / "transcode-rates.json") {}

NodeResources::NodeResources(NodeBooks& books, dannan::sched::RetainedMemoryLedger& node_memory,
                             NodeEvents& node_events)
    : memory(node_memory), transcode_rates(books.transcode_rates), events(node_events) {}

void NodeResources::stop() {
    memory.stop();
}

} // namespace macha

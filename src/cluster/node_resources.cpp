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

std::unique_ptr<dannan::sched::RetainedMemoryLedger> standalone_memory(const Config& cfg) {
    return std::make_unique<dannan::sched::RetainedMemoryLedger>(
        cfg.runtime.retained_memory_bytes, cfg.runtime.control_memory_reserve_bytes,
        cfg.runtime.viewer_memory_reserve_bytes, cfg.runtime.loader_memory_reserve_bytes,
        cfg.runtime.reassembly_memory_reserve_bytes);
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/node_resources.hpp"

#include <algorithm>
#include <thread>

namespace macha {

NodeResources::NodeResources(const Config& cfg, ActivityClocks::Source activity_clock)
    : state_lock(cfg.state_path), activity(std::move(activity_clock)),
      data(cfg.data_inflight_bytes, cfg.data_viewer_reserve_bytes,
           cfg.maintenance.background_concurrency
               ? cfg.maintenance.background_concurrency
               : std::max<size_t>(1, std::thread::hardware_concurrency() / 2),
           cfg.data_credit_no_progress_deadline,
           [this, window = cfg.maintenance.foreground_quiet] {
               return activity.viewer_recently_active(window);
           }),
      memory(cfg.runtime.retained_memory_bytes, cfg.runtime.control_memory_reserve_bytes,
             cfg.runtime.viewer_memory_reserve_bytes, cfg.runtime.loader_memory_reserve_bytes,
             cfg.runtime.reassembly_memory_reserve_bytes),
      transcode_rates(cfg.state_path / "playback" / "transcode-rates.json") {}

void NodeResources::stop() {
    data.stop();
    memory.stop();
}

} // namespace macha

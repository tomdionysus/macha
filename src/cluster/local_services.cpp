// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/local_services.hpp"

#include "cluster/cluster.hpp"
#include "metadata/metadata.hpp"
#include "storage/storage_pool.hpp"

namespace macha {

LocalServices::LocalServices(const Config& cfg, const NodeIdentity& identity,
                             RecoveryProgress& progress, const LocalState::StageHook& hook,
                             std::stop_token stop, NodeRuntime& node, NodeResources& resources,
                             MessageRoutes& routes)
    : resources_(resources), state_(cfg, identity, node, progress, hook, stop),
      metadata_(node, state_.replica(), state_.cache(), routes, cfg.metadata_min_write_replicas,
                cfg.heartbeat),
      storage_(node, identity,
               StorageServer::Stores{state_.data(), state_.control(), state_.retention(),
                                     state_.cache()},
               resources.data, resources.activity, resources.events, routes, cfg.extent_size,
               cfg.cache.max_blocks, cfg.heartbeat) {
    if (cfg.io_pressure_slowdown_percent)
        resources_.data.observe_device(&state_.data().service_monitor(),
                                       cfg.io_pressure_min_background);
    node.advertise_metadata_generation(state_.replica().committed().generation);
    node.publish_self();
}

LocalServices::~LocalServices() {
    resources_.data.observe_device(nullptr, 1);
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/local_services.hpp"

#include <utility>

#include "cluster/cluster.hpp"
#include "metadata/metadata.hpp"
#include "metadata/namespace_control_store.hpp"

namespace macha {

LocalServices::LocalServices(const Config& cfg, const NodeIdentity& identity,
                             RecoveryProgress& progress, const LocalState::StageHook& hook,
                             std::stop_token stop, NodeRuntime& node, dagda::Context& store,
                             MessageRoutes& routes)
    : // Replay writes the tree's nodes here and replicates nothing: the
      // commit reached the floor when made, and rebuilding a local head must
      // not depend on peers.
      state_(
          cfg, identity,
          [&store](const ObjectId& root, const MetadataDelta& delta) {
              auto nodes = ControlNamespaceNodeStore::for_replay(store);
              return apply_delta_to_namespace_tree(root, nodes, delta);
          },
          progress, hook, stop),
      metadata_(node, state_.replica(), state_.seed(), routes, cfg.heartbeat) {
    node.advertise_metadata_generation(state_.replica().committed().generation);
    node.publish_self();
}

} // namespace macha

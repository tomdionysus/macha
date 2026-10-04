// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/local_state.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/node_resources.hpp"
#include "cluster/storage_server.hpp"
#include "metadata/metadata_server.hpp"

#include <chrono>
#include <stop_token>

namespace macha {

class NodeRuntime;

// What the root builds once the node's control plane is online: local state,
// recovered by construction, then the servers built from it, in dependency
// order (destruction reverses it, the servers unbinding their routes before
// the stores go). Construction attaches the DATA pool's monitor to the
// arbiter, advertises the stores' figures and republishes the node's
// NodeInfo; the root marks recovery complete once it holds the result. It
// throws RecoveryCancelled when stopped between recovery stages, and
// otherwise the recovery failure, which `progress` has recorded.
class LocalServices {
  public:
    LocalServices(const Config&, const NodeIdentity&, RecoveryProgress&,
                  const LocalState::StageHook&, std::stop_token, NodeRuntime&, NodeResources&,
                  MessageRoutes&, std::chrono::milliseconds durability_batch_window);
    // Detaches the DATA pool's monitor from the arbiter before the pool goes.
    ~LocalServices();
    LocalServices(const LocalServices&) = delete;
    LocalServices& operator=(const LocalServices&) = delete;

    LocalState& state() noexcept { return state_; }
    MetadataServer& metadata() noexcept { return metadata_; }
    StorageServer& storage() noexcept { return storage_; }

  private:
    NodeResources& resources_;
    LocalState state_;
    MetadataServer metadata_;
    StorageServer storage_;
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/local_state.hpp"
#include "cluster/message_routes.hpp"
#include "metadata/metadata_server.hpp"

#include <chrono>
#include <stop_token>

namespace dagda {
class Context;
}

namespace macha {

class NodeRuntime;

// What the root builds once the store is open: this node's metadata state,
// recovered by construction, and the metadata server over it (destruction
// unbinds its routes before the state goes). Construction advertises the
// replica's generation and republishes the node's NodeInfo; the root marks
// recovery complete once it holds the result. It throws RecoveryCancelled when
// stopped first, and otherwise the recovery failure, which `progress` has
// recorded.
class LocalServices {
  public:
    LocalServices(const Config&, const NodeIdentity&, RecoveryProgress&,
                  const LocalState::StageHook&, std::stop_token, NodeRuntime&,
                  dagda::Context& store, MessageRoutes&);
    LocalServices(const LocalServices&) = delete;
    LocalServices& operator=(const LocalServices&) = delete;

    LocalState& state() noexcept {
        return state_;
    }
    MetadataServer& metadata() noexcept {
        return metadata_;
    }

  private:
    LocalState state_;
    MetadataServer metadata_;
};

} // namespace macha

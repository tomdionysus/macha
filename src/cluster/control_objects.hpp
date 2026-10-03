// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <functional>

namespace macha {

class LocalStore;
class NodeRuntime;

// Brings a control object this node lacks into its control store.
class ControlObjectSource {
  public:
    virtual ~ControlObjectSource() = default;
    // True when the object is local on return.
    virtual bool ensure_control_local(const ObjectId&) = 0;
};

// Pulls control objects from peers. They are metadata-replica data, not DHT
// replicas: every active peer is asked over CONTROL at speculative priority,
// so the search never blocks health or quorum traffic or needs a DATA
// session, and takes no DATA credit (the control store is not on the DATA
// device). Any thread.
class ControlObjectFetch final : public ControlObjectSource {
  public:
    // Called with the bytes and duration of each object received.
    using Observer = std::function<void(uint64_t bytes, Clock::duration)>;

    ControlObjectFetch(NodeRuntime&, LocalStore& control);
    bool ensure_control_local(const ObjectId& id) override {
        return pull(id, {});
    }
    bool pull(const ObjectId&, const Observer&);

  private:
    NodeRuntime& node_;
    LocalStore& control_;
};

} // namespace macha

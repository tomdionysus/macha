// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "cluster/activity_clocks.hpp"
#include "cluster/data_work.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/node_events.hpp"
#include "cluster/node_identity.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace macha {

class ClaimStore;
class LocalStore;
class NodeRuntime;
class PersistentBlockCache;
class StoragePool;

// The serving side of this node's stores: answers peers' DATA and CONTROL
// object requests (presence, reads, writes, durability barriers, retention
// and deletion) and keeps the stores' figures advertised to the control
// plane. Built once local state has recovered; binds its routes on
// construction and unbinds them, then stops its refresher, on destruction.
class StorageServer {
  public:
    struct Stores {
        StoragePool& data;
        LocalStore& control;
        ClaimStore& claims;
        PersistentBlockCache& cache;
    };
    StorageServer(NodeRuntime& node, const NodeIdentity& identity, Stores stores,
                  DataResourceArbiter& data_resources, ActivityClocks& activity,
                  NodeEvents& events, MessageRoutes& routes, size_t extent_size,
                  uint64_t cache_max_blocks, std::chrono::milliseconds refresh_interval);
    ~StorageServer();
    StorageServer(const StorageServer&) = delete;
    StorageServer& operator=(const StorageServer&) = delete;

    // Refreshes the DATA pool's accounting and advertises the stores' figures.
    void refresh();

  private:
    void route(MessageType, MessageRoutes::Handler);
    void bind_routes();
    void refresh_loop(std::stop_token);

    NodeRuntime& node_;
    const NodeIdentity& identity_;
    StoragePool& local_;
    LocalStore& control_;
    ClaimStore& claims_;
    PersistentBlockCache& cache_;
    DataResourceArbiter& data_resources_;
    ActivityClocks& activity_;
    NodeEvents& events_;
    MessageRoutes& routes_;
    const size_t extent_size_;
    const uint64_t cache_max_blocks_;
    const std::chrono::milliseconds refresh_interval_;
    std::vector<MessageType> bound_;
    // Guards nothing; the refresher waits on it.
    Mutex refresh_mutex_;
    std::condition_variable_any refresh_cv_;
    std::jthread refresher_;
};

} // namespace macha

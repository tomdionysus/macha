// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "cluster/message_routes.hpp"
#include "metadata/metadata.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace macha {

class NodeRuntime;
class PersistentBlockCache;

// The serving side of this node's metadata replica: answers peers' metadata
// requests, accepts and imports commits, and records history checkpoints.
// Built once the replica has recovered, which applies the committed
// snapshot's identity-reset tombstones before any metadata route answers;
// binds its routes on construction and unbinds them on destruction (unbind
// waits for calls in flight). Announcements go out through the node, and a
// refresher keeps the replica's generation advertised.
class MetadataServer {
  public:
    MetadataServer(NodeRuntime& node, MetadataReplica& replica, PersistentBlockCache& cache,
                   MessageRoutes& routes, std::chrono::milliseconds refresh_interval);
    ~MetadataServer();
    MetadataServer(const MetadataServer&) = delete;
    MetadataServer& operator=(const MetadataServer&) = delete;

    // Accepts a commit whose resulting policy matches this node's; announces
    // when the accepted-head set changes. Thread-safe.
    bool accept_commit(const MetadataAcceptance&);
    std::vector<MetadataAcceptance> heads() const;
    // The newest generation this node knows of: its replica's, or a newer
    // one a peer has announced. Lock-free on the node side.
    uint64_t known_generation() const;

  private:
    void route(MessageType, MessageRoutes::Handler);
    void bind_routes();
    void refresh_loop(std::stop_token);

    NodeRuntime& node_;
    MetadataReplica& replica_;
    PersistentBlockCache& cache_;
    MessageRoutes& routes_;
    const std::chrono::milliseconds refresh_interval_;
    std::vector<MessageType> bound_;
    // Guards nothing; the refresher waits on it.
    Mutex refresh_mutex_;
    std::condition_variable_any refresh_cv_;
    std::jthread refresher_;
};

} // namespace macha

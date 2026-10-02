// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/message_routes.hpp"
#include "metadata/metadata.hpp"

#include <vector>

namespace macha {

class NodeRuntime;
class PersistentBlockCache;

// The serving side of this node's metadata replica: answers peers' metadata
// requests, accepts and imports commits, and records history checkpoints.
// Built once the replica has recovered, which applies the committed
// snapshot's identity-reset tombstones before any metadata route answers;
// binds its routes on construction and unbinds them on destruction (unbind
// waits for calls in flight). Announcements go out through the node.
class MetadataServer {
  public:
    MetadataServer(NodeRuntime& node, MetadataReplica& replica, PersistentBlockCache& cache,
                   MessageRoutes& routes, size_t min_write_replicas);
    ~MetadataServer();
    MetadataServer(const MetadataServer&) = delete;
    MetadataServer& operator=(const MetadataServer&) = delete;

    // Accepts a commit whose resulting policy matches this node's; announces
    // when the accepted-head set changes. Thread-safe.
    bool accept_commit(const MetadataAcceptance&);
    std::vector<MetadataAcceptance> heads() const;

  private:
    void route(MessageType, MessageRoutes::Handler);
    void bind_routes();

    NodeRuntime& node_;
    MetadataReplica& replica_;
    PersistentBlockCache& cache_;
    MessageRoutes& routes_;
    const size_t min_write_replicas_;
    std::vector<MessageType> bound_;
};

} // namespace macha

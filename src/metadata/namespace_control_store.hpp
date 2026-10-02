// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "metadata/namespace_tree.hpp"

#include <stdexcept>

namespace macha {

// The namespace tree in the cluster's content-addressed control store, via
// `replicate_control` and `ensure_control_local` as the catalogue uses: it
// inherits replication, repair and GC with no second durability model.
// The only place NamespaceNodeStore (cluster-agnostic, so the tree can be
// built offline) meets the cluster.
class ControlNamespaceNodeStore final : public NamespaceNodeStore {
  public:
    // `required` is the commit's metadata write floor: a root may be
    // committed only once every node it addresses has durably reached it.
    // A reader (floor zero) refuses `put`; zero never means "no floor".
    static ControlNamespaceNodeStore for_reading(LocalStore& control, DistributedStore& store) {
        return ControlNamespaceNodeStore(control, store, 0, Mode::read);
    }
    static ControlNamespaceNodeStore for_commit(LocalStore& control, DistributedStore& store,
                                                size_t required) {
        if (!required)
            throw std::invalid_argument("namespace commit requires a metadata write floor");
        return ControlNamespaceNodeStore(control, store, required, Mode::commit);
    }
    // Replay writes locally and replicates nothing: a materialised history
    // entry already reached the floor when committed, and replicating would
    // stop a node with peers down from rebuilding its own head. Content
    // addressing makes a locally rebuilt node identical to the committed one.
    static ControlNamespaceNodeStore for_replay(LocalStore& control, DistributedStore& store) {
        return ControlNamespaceNodeStore(control, store, 0, Mode::replay);
    }

    // Writes the node and returns its content address. Throws
    // MetadataNotReady when the node fails to reach the floor, so a commit
    // never publishes a root addressing an unreachable node.
    ObjectId put(std::span<const uint8_t> node) override;

    // Fetches the node, pulling it local first. Empty, not a throw, when it
    // cannot be obtained: callers (a stat, a prefix scan) distinguish "not
    // here" from "not found" and need to know which node was missing.
    std::optional<Bytes> get(const ObjectId& id) const override;

    // Nodes written since construction, in write order: the dirty set a
    // commit replicated.
    const std::vector<ObjectId>& written() const noexcept {
        return written_;
    }

  private:
    enum class Mode : uint8_t { read, commit, replay };
    ControlNamespaceNodeStore(LocalStore& control, DistributedStore& store, size_t required,
                              Mode mode);

    LocalStore& control_;
    DistributedStore& store_;
    size_t required_;
    Mode mode_;
    std::vector<ObjectId> written_;
};

// The tree over one node's control store alone, for offline tools (a
// migration runs with the cluster stopped): nothing goes to or comes from
// peers.
class LocalNamespaceNodeStore final : public NamespaceNodeStore {
  public:
    explicit LocalNamespaceNodeStore(LocalStore& store) : store_(store) {}

    ObjectId put(std::span<const uint8_t> node) override;
    std::optional<Bytes> get(const ObjectId& id) const override;

    const std::vector<ObjectId>& written() const noexcept {
        return written_;
    }
    uint64_t bytes_written() const noexcept {
        return bytes_written_;
    }

  private:
    LocalStore& store_;
    std::vector<ObjectId> written_;
    uint64_t bytes_written_{};
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "metadata/namespace_tree.hpp"

#include <stdexcept>

namespace macha {

// The namespace tree in the cluster's content-addressed control store, read
// through `ensure_control_local` as the catalogue reads: it inherits
// replication, repair and GC with no second durability model.
// The only place NamespaceNodeStore (cluster-agnostic, so the tree can be
// built offline) meets the cluster.
class ControlNamespaceNodeStore final : public NamespaceNodeStore {
  public:
    // A reader refuses `put`.
    static ControlNamespaceNodeStore for_reading(LocalStore& control, DistributedStore& store) {
        return ControlNamespaceNodeStore(control, store, Mode::read);
    }
    // A commit writes each new node on this node alone; a root is committed
    // once this node holds every node it addresses. The peers are sent them
    // afterwards.
    static ControlNamespaceNodeStore for_commit(LocalStore& control, DistributedStore& store) {
        return ControlNamespaceNodeStore(control, store, Mode::commit);
    }
    // Replay writes every node it is given: content addressing makes a
    // locally rebuilt node identical to the committed one.
    static ControlNamespaceNodeStore for_replay(LocalStore& control, ControlObjectSource& source) {
        return ControlNamespaceNodeStore(control, source, Mode::replay);
    }

    // Writes the node and returns its content address. Throws when this node
    // cannot hold it, so a commit never publishes a root it cannot read.
    ObjectId put(std::span<const uint8_t> node) override;

    // Fetches the node, pulling it local first. Empty, not a throw, when it
    // cannot be obtained: callers (a stat, a prefix scan) distinguish "not
    // here" from "not found" and need to know which node was missing.
    std::optional<Bytes> get(const ObjectId& id) const override;

    // Nodes written since construction, in write order.
    const std::vector<ObjectId>& written() const noexcept {
        return written_;
    }

  private:
    enum class Mode : uint8_t { read, commit, replay };
    ControlNamespaceNodeStore(LocalStore& control, ControlObjectSource& source, Mode mode);

    LocalStore& control_;
    ControlObjectSource& source_;
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

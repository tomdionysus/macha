// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "distributed_store.hpp"
#include "namespace_tree.hpp"

#include <stdexcept>

namespace macha {

// The namespace tree living in the cluster's existing content-addressed
// control store -- the path the catalogue's manifest and shards already take,
// via `replicate_control` and `ensure_control_local`. That choice was made in
// the plan for a reason worth restating: it inherits replication, repair and
// GC, and it adds no second durability model and no new dependency on the Pi
// nodes.
//
// `NamespaceNodeStore` itself deliberately knows nothing about a cluster, so
// the tree can be built and measured offline. This is the adapter that gives
// it one, and it is the only place the two meet.
class ControlNamespaceNodeStore final : public NamespaceNodeStore {
  public:
    // `required` is the metadata write floor the commit is being made under. A
    // namespace root may be referenced by a metadata commit only once every
    // node it addresses has durably reached that floor, exactly as a catalogue
    // manifest may not name a shard that has not.
    //
    // Zero means read-only and `put` refuses, rather than zero meaning "no
    // floor". A tree written under no durability requirement is a root that
    // can address nodes no peer holds, and the readers that want this class --
    // reachability walks, the catalogue scan -- never write.
    static ControlNamespaceNodeStore for_reading(NodeRuntime& node, DistributedStore& store) {
        return ControlNamespaceNodeStore(node, store, 0, Mode::read);
    }
    static ControlNamespaceNodeStore for_commit(NodeRuntime& node, DistributedStore& store,
                                                size_t required) {
        if (!required)
            throw std::invalid_argument("namespace commit requires a metadata write floor");
        return ControlNamespaceNodeStore(node, store, required, Mode::commit);
    }
    // Replay writes locally and replicates nothing. A history entry being
    // materialised is a commit that already happened: its nodes reached the
    // floor when it was made, and re-establishing that here would turn a local
    // materialisation into a network dependency -- a node coming back with
    // peers still down could not rebuild its own head. Reconstructed nodes are
    // content-addressed, so a locally written one is either identical to the
    // node the committer wrote or it is not that node at all.
    static ControlNamespaceNodeStore for_replay(NodeRuntime& node, DistributedStore& store) {
        return ControlNamespaceNodeStore(node, store, 0, Mode::replay);
    }

    // Writes the node and returns its content address. A node that fails to
    // reach the floor throws rather than returning an id the commit would then
    // publish -- an unreachable node is a hole in the namespace, and the whole
    // point of the tree is that a hole is reachable from the root.
    ObjectId put(std::span<const uint8_t> node) override;

    // Fetches the node, pulling it local first if this node does not hold it.
    // Returns empty rather than throwing when the object cannot be obtained:
    // the callers that matter -- a stat, a prefix scan -- distinguish "not
    // here" from "not found", and turning an unavailable peer into an
    // exception several levels down a tree walk loses which node was missing.
    std::optional<Bytes> get(const ObjectId& id) const override;

    // Nodes written by this store since construction, in write order. This is
    // the dirty set a commit replicated, and it is what makes the cost of a
    // namespace write observable rather than asserted.
    const std::vector<ObjectId>& written() const noexcept {
        return written_;
    }

  private:
    enum class Mode : uint8_t { read, commit, replay };
    ControlNamespaceNodeStore(NodeRuntime& node, DistributedStore& store, size_t required,
                              Mode mode);

    NodeRuntime& node_;
    DistributedStore& store_;
    size_t required_;
    Mode mode_;
    std::vector<ObjectId> written_;
};

} // namespace macha

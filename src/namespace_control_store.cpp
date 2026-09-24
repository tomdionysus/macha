// SPDX-License-Identifier: GPL-3.0-or-later
#include "namespace_control_store.hpp"

#include "crypto.hpp"
#include "metadata_manager.hpp"

#include <stdexcept>

namespace macha {

ControlNamespaceNodeStore::ControlNamespaceNodeStore(NodeRuntime& node, DistributedStore& store,
                                                     size_t required, Mode mode)
    : node_(node), store_(store), required_(required), mode_(mode) {}

ObjectId ControlNamespaceNodeStore::put(std::span<const uint8_t> node) {
    if (mode_ == Mode::read)
        throw std::logic_error("namespace node store opened for reading cannot write a node");
    const auto id = object_id(node);
    if (mode_ == Mode::replay) {
        if (!node_.control_store().put(id, node))
            throw std::runtime_error("namespace node could not be written locally during replay: " +
                                     to_string(id));
        written_.push_back(id);
        return id;
    }
    // An unchanged node is already here, and it was replicated when it was
    // first written: content addressing makes it immutable, so a second
    // identical write has nothing to establish. Skipping it is the difference
    // between a commit that costs one round trip per genuinely new node and one
    // that costs a round trip per node the re-chunk touched.
    //
    // This mattered enormously and was measured the hard way. update_namespace_tree
    // recomputes the spine, so a commit writes roughly a dozen nodes of which
    // all but the changed leaf and its path are byte-identical to what is
    // already stored. Replicating each of them synchronously to peers 60 ms
    // away held an ingest to 1.8 MB/s on a node sitting at 0.5 load and 2%
    // iowait -- 2.6 hours for a 36 GB import, with nothing whatsoever
    // saturated. The bytes were never the problem; the round trips were.
    if (node_.control_store().has(id))
        return id;
    // MetadataNotReady, not a bare runtime_error: a peer dropping out mid
    // commit is the same transient cluster condition as a floor missing
    // before it, and callers (an ingest, 0.57.0) block and retry on that type
    // rather than failing the job.
    if (store_.replicate_control(id, node) < required_)
        throw MetadataNotReady("namespace node could not reach the metadata durability floor: " +
                               to_string(id));
    written_.push_back(id);
    return id;
}

std::optional<Bytes> ControlNamespaceNodeStore::get(const ObjectId& id) const {
    if (auto local = node_.control_store().get(id))
        return local;
    if (!store_.ensure_control_local(id))
        return {};
    return node_.control_store().get(id);
}

ObjectId LocalNamespaceNodeStore::put(std::span<const uint8_t> node) {
    const auto id = object_id(node);
    if (!store_.put(id, node))
        throw std::runtime_error("namespace node could not be written: " + to_string(id));
    written_.push_back(id);
    bytes_written_ += node.size();
    return id;
}

std::optional<Bytes> LocalNamespaceNodeStore::get(const ObjectId& id) const {
    return store_.get(id);
}

} // namespace macha

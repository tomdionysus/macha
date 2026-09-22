// SPDX-License-Identifier: GPL-3.0-or-later
#include "namespace_control_store.hpp"

#include "crypto.hpp"

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
    // Storing the same bytes twice is not an error and must return the same
    // id, which content addressing gives for free -- and which is what makes
    // an unchanged subtree cost nothing on a commit that rewrites its
    // neighbour.
    if (store_.replicate_control(id, node) < required_)
        throw std::runtime_error("namespace node could not reach the metadata durability floor: " +
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

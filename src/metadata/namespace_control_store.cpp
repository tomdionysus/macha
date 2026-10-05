// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata/namespace_control_store.hpp"

#include "crypto.hpp"
#include "metadata/metadata_manager.hpp"

#include <stdexcept>

namespace macha {

ControlNamespaceNodeStore::ControlNamespaceNodeStore(LocalStore& control,
                                                     ControlObjectSource& source, Mode mode)
    : control_(control), source_(source), mode_(mode) {}

ObjectId ControlNamespaceNodeStore::put(std::span<const uint8_t> node) {
    if (mode_ == Mode::read)
        throw std::logic_error("namespace node store opened for reading cannot write a node");
    const auto id = object_id(node);
    if (mode_ == Mode::replay) {
        if (!control_.put(id, node))
            throw std::runtime_error("namespace node could not be written locally during replay: " +
                                     to_string(id));
        written_.push_back(id);
        return id;
    }
    // A node already held is immutable by content address, so skip it. A new
    // one is written here and nowhere else: a commit asks no peer. The peers
    // get it from the replicator's claims, or build it themselves from the
    // commit's delta.
    if (control_.has(id))
        return id;
    if (!control_.put(id, node))
        throw std::runtime_error("namespace node could not be written locally: " + to_string(id));
    written_.push_back(id);
    return id;
}

std::optional<Bytes> ControlNamespaceNodeStore::get(const ObjectId& id) const {
    if (auto local = control_.get(id))
        return local;
    if (!source_.ensure_control_local(id))
        return {};
    return control_.get(id);
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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/presence_index.hpp"

namespace macha {

void PresenceIndex::installed(const ObjectId& id) {
    present_.insert(id);
    pruned_.erase(id);
}

void PresenceIndex::observed(const ObjectId& id) {
    present_.insert(id);
}

void PresenceIndex::forgotten(const ObjectId& id) {
    present_.erase(id);
    if (!authoritative_)
        forgotten_while_warming_.insert(id);
}

void PresenceIndex::pruned(const ObjectId& id) {
    forgotten(id);
    pruned_.insert(id);
}

void PresenceIndex::listed(std::span<const ObjectId> ids) {
    for (const auto& id : ids)
        if (!pruned_.contains(id) && !forgotten_while_warming_.contains(id))
            present_.insert(id);
}

void PresenceIndex::warmed() {
    authoritative_ = true;
    forgotten_while_warming_.clear();
}

PresenceIndex::Answer PresenceIndex::answer(const ObjectId& id) const {
    if (present_.contains(id))
        return Answer::present;
    return authoritative_ ? Answer::absent : Answer::unknown;
}

} // namespace macha

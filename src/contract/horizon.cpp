// SPDX-License-Identifier: GPL-3.0-or-later
#include "contract/horizon.hpp"

#include <algorithm>
#include <utility>

namespace macha {

namespace {

std::vector<ObjectId> sorted_unique(std::vector<ObjectId> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace

ReferencedSets::ReferencedSets(std::vector<ObjectId> data, std::vector<ObjectId> control)
    : data_(sorted_unique(std::move(data))), control_(sorted_unique(std::move(control))) {}

const std::vector<ObjectId>& ReferencedSets::of(RetentionClass type) const {
    return type == RetentionClass::data ? data_ : control_;
}

bool ReferencedSets::referenced(RetentionClass type, const ObjectId& id) const {
    const auto& ids = of(type);
    return std::binary_search(ids.begin(), ids.end(), id);
}

std::span<const ObjectId> ReferencedSets::referenced_ids(RetentionClass type) const {
    return of(type);
}

size_t ReferencedSets::size(RetentionClass type) const { return of(type).size(); }

InventoryHorizon::InventoryHorizon(uint64_t generation, bool catalogue_complete,
                                   std::vector<ObjectId> data, std::vector<ObjectId> control,
                                   const std::vector<GarbageRef>& garbage,
                                   std::vector<ObjectId> outside_namespace)
    : ReferencedSets(std::move(data), std::move(control)), generation_(generation),
      catalogue_complete_(catalogue_complete),
      outside_namespace_(sorted_unique(std::move(outside_namespace))) {
    garbage_.reserve(garbage.size());
    stale_garbage_.reserve(garbage.size());
    for (const auto& candidate : garbage) {
        if (referenced(RetentionClass::data, candidate.id))
            stale_garbage_.push_back(candidate);
        else
            garbage_.push_back(candidate);
    }
}

ReleaseHorizon::ReleaseHorizon(Hash256 head, RetentionClock clock, std::vector<ObjectId> data,
                               std::vector<ObjectId> control)
    : ReferencedSets(std::move(data), std::move(control)), head_(head), clock_(std::move(clock)) {}

} // namespace macha

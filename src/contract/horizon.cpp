// SPDX-License-Identifier: GPL-3.0-or-later
#include "contract/horizon.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace macha {

namespace {

std::vector<ObjectId> sorted_unique(std::vector<ObjectId> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace

ReferencedSets::Set ReferencedSets::make(std::optional<ObjectTrie::Snapshot> view,
                                         std::vector<ObjectId> others) {
    Set set;
    set.others = sorted_unique(std::move(others));
    set.size = set.others.size();
    if (view) {
        for (const auto& id : set.others)
            if (view->get(id))
                --set.size;
        set.size += view->size();
    }
    set.view = std::move(view);
    return set;
}

ReferencedSets::ReferencedSets(std::vector<ObjectId> data, std::vector<ObjectId> control)
    : data_(make(std::nullopt, std::move(data))), control_(make(std::nullopt, std::move(control))) {}

ReferencedSets::ReferencedSets(Views views, std::vector<ObjectId> data,
                               std::vector<ObjectId> control)
    : data_(make(std::move(views.data), std::move(data))),
      control_(make(std::move(views.control), std::move(control))) {}

const ReferencedSets::Set& ReferencedSets::of(RetentionClass type) const {
    return type == RetentionClass::data ? data_ : control_;
}

bool ReferencedSets::referenced(RetentionClass type, const ObjectId& id) const {
    const auto& set = of(type);
    return std::binary_search(set.others.begin(), set.others.end(), id) ||
           (set.view && set.view->get(id).has_value());
}

IdLookup ReferencedSets::lookup(RetentionClass type) const {
    const auto& set = of(type);
    if (!set.view)
        return IdLookup(std::span<const ObjectId>(set.others));
    return IdLookup([this, type](const ObjectId& id) { return referenced(type, id); });
}

std::vector<ObjectId> ReferencedSets::next(RetentionClass type,
                                           const std::optional<ObjectId>& after,
                                           size_t limit) const {
    const auto& set = of(type);
    std::vector<ObjectId> viewed;
    if (set.view)
        for (auto& record : set.view->next(after, limit))
            viewed.push_back(record.first);
    // A full page of the view says nothing past its last id.
    const bool bounded = viewed.size() == limit;
    auto from = after ? std::upper_bound(set.others.begin(), set.others.end(), *after)
                      : set.others.begin();
    auto to = bounded ? std::upper_bound(from, set.others.end(), viewed.back()) : set.others.end();
    if (static_cast<size_t>(to - from) > limit)
        to = from + static_cast<std::ptrdiff_t>(limit);
    std::vector<ObjectId> out;
    out.reserve(viewed.size() + static_cast<size_t>(to - from));
    std::set_union(viewed.begin(), viewed.end(), from, to, std::back_inserter(out));
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

std::vector<ObjectId> ReferencedSets::all(RetentionClass type) const {
    std::vector<ObjectId> out;
    std::optional<ObjectId> after;
    while (true) {
        const auto page = next(type, after, 4096);
        out.insert(out.end(), page.begin(), page.end());
        if (page.size() < 4096)
            return out;
        after = page.back();
    }
}

size_t ReferencedSets::size(RetentionClass type) const { return of(type).size; }

InventoryHorizon::InventoryHorizon(uint64_t generation, bool catalogue_complete,
                                   std::vector<ObjectId> data, std::vector<ObjectId> control,
                                   const std::vector<GarbageRef>& garbage,
                                   std::vector<ObjectId> outside_namespace)
    : InventoryHorizon(generation, catalogue_complete,
                       ReferencedSets(std::move(data), std::move(control)), garbage,
                       std::move(outside_namespace)) {}

InventoryHorizon::InventoryHorizon(uint64_t generation, bool catalogue_complete,
                                   ReferencedSets sets,
                                   const std::vector<GarbageRef>& garbage,
                                   std::vector<ObjectId> outside_namespace,
                                   std::vector<TombstoneBatch> tombstone_batches)
    : ReferencedSets(std::move(sets)), generation_(generation),
      catalogue_complete_(catalogue_complete),
      outside_namespace_(sorted_unique(std::move(outside_namespace))),
      tombstone_batches_(std::move(tombstone_batches)) {
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

ReleaseHorizon::ReleaseHorizon(Hash256 head, RetentionClock clock, ReferencedSets sets)
    : ReferencedSets(std::move(sets)), head_(head), clock_(std::move(clock)) {}

} // namespace macha

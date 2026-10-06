// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/item_availability.hpp"

#include <vector>

namespace macha {

namespace {

const PathAvailability* facts_of_media(const AvailabilitySnapshot* survey,
                                       std::string_view media_id) {
    if (!survey)
        return nullptr;
    const auto found = survey->by_hash.find(media_id);
    if (found == survey->by_hash.end())
        return nullptr;
    const auto path = survey->paths.find(found->second);
    return path == survey->paths.end() ? nullptr : &path->second;
}

// complete > partial > unknown > unavailable: the best a viewer can get.
int rank(Availability a) noexcept {
    switch (a) {
    case Availability::complete:
        return 3;
    case Availability::partial:
        return 2;
    case Availability::unknown:
        return 1;
    case Availability::unavailable:
        return 0;
    }
    return 0;
}

void count(ItemAvailability& set, Availability member) noexcept {
    ++set.members;
    switch (member) {
    case Availability::complete:
        ++set.complete;
        break;
    case Availability::partial:
        ++set.partial;
        break;
    case Availability::unavailable:
        ++set.unavailable;
        break;
    case Availability::unknown:
        ++set.unknown;
        break;
    }
}

Availability of_set(const ItemAvailability& set) noexcept {
    if (!set.members)
        return Availability::unknown;
    if (set.complete == set.members)
        return Availability::complete;
    if (set.unavailable == set.members)
        return Availability::unavailable;
    if (set.unknown && !set.partial && !set.unavailable)
        return Availability::unknown;
    return Availability::partial;
}

} // namespace

const char* availability_name(Availability a) noexcept {
    switch (a) {
    case Availability::complete:
        return "complete";
    case Availability::partial:
        return "partial";
    case Availability::unavailable:
        return "unavailable";
    case Availability::unknown:
        return "unknown";
    }
    return "unknown";
}

Availability availability_of(const PathAvailability* facts) noexcept {
    if (!facts)
        return Availability::unknown;
    if (facts->extents_unavailable)
        return facts->extents_unavailable == facts->extents ? Availability::unavailable
                                                             : Availability::partial;
    if (facts->extents_unknown)
        return Availability::unknown;
    return Availability::complete;
}

ItemAvailabilityTable item_availability(const CatalogueView& catalogue,
                                        const AvailabilitySnapshot* survey) {
    ItemAvailabilityTable table;
    // Items with files: the best of their files.
    std::vector<const CatalogueItem*> with_files;
    for (const auto& [id, item] : catalogue.items()) {
        auto& entry = table[id];
        if (item.media_ids.empty())
            continue;
        Availability best = Availability::unavailable;
        for (const auto& media_id : item.media_ids) {
            const auto one = availability_of(facts_of_media(survey, media_id));
            if (rank(one) > rank(best))
                best = one;
        }
        entry.status = best;
        with_files.push_back(&item);
    }
    // Each is a member of every set above it. A parent chain longer than the
    // catalogue has a cycle in it and is abandoned.
    for (const auto* item : with_files) {
        const auto own = table.at(item->id).status;
        const CatalogueItem* at = item;
        for (size_t depth = 0; at->parent_id && depth < catalogue.item_count(); ++depth) {
            const auto* parent = catalogue.item(*at->parent_id);
            if (!parent)
                break;
            count(table[parent->id], own);
            at = parent;
        }
    }
    for (auto& [id, entry] : table)
        if (entry.members)
            entry.status = of_set(entry);
    return table;
}

std::shared_ptr<const ItemAvailabilityTable>
ItemAvailabilityCache::table(const std::shared_ptr<const CatalogueView>& catalogue,
                             const std::shared_ptr<const AvailabilitySnapshot>& survey) {
    Lock lock(mutex_);
    if (!table_ || catalogue_ != catalogue || survey_ != survey) {
        table_ = std::make_shared<const ItemAvailabilityTable>(
            item_availability(*catalogue, survey.get()));
        catalogue_ = catalogue;
        survey_ = survey;
    }
    return table_;
}

} // namespace macha

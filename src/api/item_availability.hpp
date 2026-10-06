// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "service/availability_service.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace macha {

// How much of something the reachable cluster holds: the one vocabulary of
// the files resource, the playback media listing and every catalogue item.
enum class Availability : uint8_t {
    complete,    // every extent is held by a reachable node
    partial,     // some are, some are held by no reachable node
    unavailable, // none are
    unknown,     // not surveyed, or some extents could not be decided
};
const char* availability_name(Availability) noexcept;

// A file's (or a directory's) code from its extent counts; null is unknown.
// A file with no extents needs nothing and is complete.
Availability availability_of(const PathAvailability* facts) noexcept;

// A catalogue item's availability. An item with files takes the best of
// them, since any one can be played. A set (a show, a season, an artist, an
// album) is judged over its members, the items beneath it that have files:
// complete if all are, unavailable if all are, unknown if some are unknown
// and none is short, otherwise partial. With no members it is unknown.
struct ItemAvailability {
    Availability status{Availability::unknown};
    // Members by code; all zero for an item that is not a set.
    uint32_t members{};
    uint32_t complete{};
    uint32_t partial{};
    uint32_t unavailable{};
    uint32_t unknown{};
    auto operator<=>(const ItemAvailability&) const = default;
};

using ItemAvailabilityTable = std::map<std::string, ItemAvailability, std::less<>>;

// Every item of `catalogue` against `survey` (null: nothing surveyed). One
// pass over the items; a pure function of the two snapshots.
ItemAvailabilityTable item_availability(const CatalogueView& catalogue,
                                        const AvailabilitySnapshot* survey);

// The table for the current pair of snapshots, rebuilt only when either
// changes. Thread-safe; a rebuild is memory work.
class ItemAvailabilityCache {
  public:
    std::shared_ptr<const ItemAvailabilityTable>
    table(const std::shared_ptr<const CatalogueView>& catalogue,
          const std::shared_ptr<const AvailabilitySnapshot>& survey);

  private:
    Mutex mutex_;
    std::shared_ptr<const CatalogueView> catalogue_ MACHA_GUARDED_BY(mutex_);
    std::shared_ptr<const AvailabilitySnapshot> survey_ MACHA_GUARDED_BY(mutex_);
    std::shared_ptr<const ItemAvailabilityTable> table_ MACHA_GUARDED_BY(mutex_);
};

} // namespace macha

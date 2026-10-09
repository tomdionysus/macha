// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace macha {

// A set of object ids asked one at a time: a sorted, duplicate-free span, or
// any membership test (a horizon's on-disk view). Empty by default. A span
// is not copied: it must outlive the lookup.
class IdLookup {
  public:
    IdLookup() = default;
    IdLookup(std::span<const ObjectId> sorted) : sorted_(sorted) {}
    IdLookup(const std::vector<ObjectId>& sorted) : sorted_(sorted) {}
    explicit IdLookup(std::function<bool(const ObjectId&)> contains)
        : contains_(std::move(contains)) {}

    bool contains(const ObjectId& id) const {
        return contains_ ? contains_(id) : std::binary_search(sorted_.begin(), sorted_.end(), id);
    }
    // The span it was made from; empty for a membership test.
    std::span<const ObjectId> sorted() const noexcept {
        return contains_ ? std::span<const ObjectId>{} : sorted_;
    }

  private:
    std::span<const ObjectId> sorted_;
    std::function<bool(const ObjectId&)> contains_;
};

} // namespace macha

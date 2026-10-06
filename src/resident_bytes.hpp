// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <string>

// What a decoded structure holds on the heap, estimated from its containers'
// sizes and capacities. Saturates rather than wraps.
namespace macha::resident {

inline void add(uint64_t& total, uint64_t value) {
    total = value > std::numeric_limits<uint64_t>::max() - total
                ? std::numeric_limits<uint64_t>::max()
                : total + value;
}

// One node per value; four pointers cover parent/children and allocator
// bookkeeping beyond sizeof(value_type).
template <class Map> void map_nodes(uint64_t& total, const Map& values) {
    add(total, static_cast<uint64_t>(values.size()) *
                   (sizeof(typename Map::value_type) + 4 * sizeof(void*)));
}

// capacity() includes allocator slack.
inline void string(uint64_t& total, const std::string& value) {
    add(total, static_cast<uint64_t>(value.capacity()) + 1);
}

template <class Vector> void vector(uint64_t& total, const Vector& values) {
    add(total, static_cast<uint64_t>(values.capacity()) * sizeof(typename Vector::value_type));
}

} // namespace macha::resident

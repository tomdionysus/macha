// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include "dagda/contract/id_lookup.h"

namespace macha {

// A set of object ids asked one at a time, a sorted span or a membership
// test: Dagda's.
using IdLookup = dagda::contract::IdLookup;

} // namespace macha

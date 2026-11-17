// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dagda/contract/claim_store.h"

namespace macha {

// A claim's class, its causal dot (the writing node and its sequence), and
// what a metadata head has observed of each node's claim writes: Dagda's.
using RetentionClass = dagda::contract::RetentionClass;
using RetentionDot = dagda::contract::RetentionDot;
using RetentionClock = dagda::contract::RetentionClock;

} // namespace macha

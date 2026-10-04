// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/horizon.hpp"

#include <cstdint>
#include <string>

// The ledger's three destructive gates (object ledger spec, B3), one per row
// of the gate table: whether the maintenance pass may proceed, and why not.
// A gate reads the inventory's stamp and the pass's facts; it fetches nothing.
namespace macha {

// What the pass knows when it reaches the gates.
struct PassFacts {
    // Tombstone collection is due: no foreground work, no metadata dirty.
    bool garbage_due{};
    // Garbage collection is due: no foreground work, past its quiet window.
    bool gc_due{};
    // Why GC is not due: foreground work, or GC finished and waiting for an
    // event (its quiet window unbounded).
    bool busy{};
    bool gc_waiting_for_event{};
    // The inventory was rebuilt in this pass: not yet used destructively.
    bool rebuilt_inventory{};
    // A sole accepted head to release against.
    bool release_view{};
    // The metadata generation this node knows of.
    uint64_t known_generation{};
};

struct GateVerdict {
    bool permitted{};
    // Empty when permitted: why the gate is shut, for the log.
    std::string reason;
    // Every condition the gate read, as `name=0|1` flags, for the trace.
    std::string conditions;
};

// Every gate reads this node's own state only: no gate waits for another
// node. What protects an absent node's references is the deletion grace the
// sweeps apply on this node's clock.

// Tombstone collection: due, the inventory not rebuilt this pass, the
// catalogue's inventory complete.
GateVerdict tombstone_gate(const PassFacts&, const InventoryHorizon*);

// Control release and GC: due, not rebuilt, a release view, the catalogue
// complete, an inventory, at or past the known generation.
GateVerdict control_gate(const PassFacts&, const InventoryHorizon*);

// DATA release and sweep: the control gate's conditions; the reason is the
// first that fails, in the pass's log order.
GateVerdict data_gate(const PassFacts&, const InventoryHorizon*);

} // namespace macha

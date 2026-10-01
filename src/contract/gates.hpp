// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/horizon.hpp"

#include <cstdint>
#include <string>

// The ledger's three destructive gates (the object ledger spec, B3), one per
// row of the gate table: each is exactly the condition the maintenance pass
// tests, returning whether it is permitted and why not. A gate reads the
// inventory's stamp and the facts the pass gives it; it fetches nothing.
namespace macha {

// What the pass knows when it reaches the gates.
struct PassFacts {
    // Tombstone collection is due: no foreground work, no metadata dirty.
    bool garbage_due{};
    // Garbage collection is due: no foreground work, past its quiet window.
    bool gc_due{};
    // Why GC is not due, for its reason: foreground work, or GC finished and
    // waiting for an event (its quiet window unbounded).
    bool busy{};
    bool gc_waiting_for_event{};
    // The inventory was rebuilt in this pass: not yet used destructively.
    bool rebuilt_inventory{};
    // Every durably-known node reachable, and metadata stable as well.
    bool reachable{};
    bool metadata_stable{};
    // A sole accepted head to release against, and its retention baseline.
    bool release_view{};
    bool retention_baseline_complete{};
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

// Tombstone collection: due, the inventory not rebuilt this pass, the
// cluster stable, the catalogue's inventory complete.
GateVerdict tombstone_gate(const PassFacts&, const InventoryHorizon*);

// Control release and GC: due, not rebuilt, destructive GC enabled (stable,
// a release view, its baseline complete), the catalogue complete, an
// inventory, at or past the known generation.
GateVerdict control_gate(const PassFacts&, const InventoryHorizon*);

// DATA release and sweep: the control gate's conditions, its reason the
// first that fails in the order the pass has always logged it.
GateVerdict data_gate(const PassFacts&, const InventoryHorizon*);

} // namespace macha

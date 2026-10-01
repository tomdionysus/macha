// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"
#include "types.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

// The claims a node holds (the object ledger spec, B3, "claimed"): which
// objects it has promised to keep, durably, by causal dot. The storage layer
// owns it -- the node's RPC handlers serve peers' claim writes and delete
// refusals from it, and the distributed store claims what it publishes --
// and the ledger forwards to it; nothing above the ledger reaches it except
// through the ledger. Implemented by RetentionStore, journal and checkpoints
// unchanged.
namespace macha {

enum class RetentionClass : uint8_t {
    data = 1,
    control = 2,
};

// One claim's causal identity: the node that wrote it and that node's
// sequence.
struct RetentionDot {
    NodeId origin{};
    uint64_t sequence{};
    auto operator<=>(const RetentionDot&) const = default;
};

// What a metadata head has observed of each node's claim writes.
using RetentionClock = std::map<NodeId, uint64_t>;

class ClaimStore {
  public:
    virtual ~ClaimStore() = default;

    // Claim writes: one durable journal frame each. Waits on the state
    // device.
    static constexpr Waits write_waits = Waits::state_device;
    virtual void retain(RetentionClass, const ObjectId&, const RetentionDot&) = 0;
    virtual void retain_batch(RetentionClass, const std::vector<ObjectId>&,
                              const RetentionDot&) = 0;

    // Claim reads, from the in-memory map. Thread-safe; wait on nothing.
    static constexpr Waits read_waits = Waits::none;
    virtual bool retained(RetentionClass, const ObjectId&) const = 0;
    virtual std::optional<ObjectId> next_retained(RetentionClass, std::optional<ObjectId>& cursor,
                                                  bool& complete) const = 0;
    virtual std::vector<ObjectId> retained_ids(RetentionClass) const = 0;
    virtual size_t claim_objects(RetentionClass) const = 0;
    // Diagnostic: the observed-remove state of one object, `adds` and
    // `removed` keyed by origin. Empty when the object has no state.
    struct Claims {
        std::map<NodeId, uint64_t> adds;
        std::map<NodeId, uint64_t> removed;
    };
    virtual Claims claims(RetentionClass, const ObjectId&) const = 0;

    // Release the claims a complete horizon no longer refers to and that its
    // clock has observed; `live` sorted and unique. Bounded; one journal
    // frame per slice.
    virtual size_t release_unreferenced(RetentionClass, std::span<const ObjectId> live,
                                        const RetentionClock& observed,
                                        size_t operation_budget) = 0;
    // Forget causality tombstones once no claim remains and the object is
    // absent. Bounded, cursor-based.
    virtual size_t prune_unclaimed(RetentionClass,
                                   const std::function<bool(const ObjectId&)>& exists,
                                   size_t operation_budget) = 0;
    // Compact the journal into checkpoint shards past a threshold.
    virtual bool compact_if_needed(size_t record_threshold = 4096) = 0;
};

} // namespace macha

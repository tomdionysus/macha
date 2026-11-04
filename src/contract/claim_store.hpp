// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/id_lookup.hpp"
#include "contract/thread_safety.hpp"
#include "contract/work.hpp"
#include "types.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// The claims a node holds (object ledger spec, B3, "claimed"): the objects it
// has durably promised to keep, by causal dot. Owned by the storage layer;
// above the ledger it is reached only through the ledger. Implemented by
// RetentionStore.
namespace macha {

enum class RetentionClass : uint8_t {
    data = 1,
    control = 2,
};

// One claim's causal identity: the writing node and its sequence.
struct RetentionDot {
    NodeId origin{};
    uint64_t sequence{};
    auto operator<=>(const RetentionDot&) const = default;
};

// What a metadata head has observed of each node's claim writes.
using RetentionClock = std::map<NodeId, uint64_t>;

// This node's guard over an object's claims against its copy. A claim's
// presence check and its write, and a delete's check that no claim stands
// and its unlink, each hold the guard for the ids they touch, so no claim
// lands between a delete's check and the delete. Striped by the id's first
// byte; a batch takes its stripes in ascending order, so two batches never
// deadlock. Taken before the object store's and the claim store's locks,
// never under them; a holder waits on the state and DATA devices.
class ClaimGuard {
    std::vector<std::unique_lock<std::mutex>> held_;
    friend class ClaimLocks;

  public:
    ClaimGuard() = default;
    ClaimGuard(ClaimGuard&&) noexcept = default;
    ClaimGuard& operator=(ClaimGuard&&) noexcept = default;
    ClaimGuard(const ClaimGuard&) = delete;
    ClaimGuard& operator=(const ClaimGuard&) = delete;
};

class ClaimLocks {
    static constexpr size_t stripes = 64;
    std::array<IoMutex, stripes> stripes_;

  public:
    ClaimGuard lock(std::span<const ObjectId> ids) MACHA_EXCLUDES(no_io) {
        std::array<bool, stripes> wanted{};
        for (const auto& id : ids)
            wanted[id.bytes[0] % stripes] = true;
        ClaimGuard guard;
        for (size_t stripe = 0; stripe < stripes; ++stripe)
            if (wanted[stripe])
                guard.held_.emplace_back(stripes_[stripe].native());
        return guard;
    }
};

class ClaimStore {
    mutable ClaimLocks locks_;

  public:
    virtual ~ClaimStore() = default;

    // The guard above, for the ids given. Waits on the devices its other
    // holders wait on. Const: a guard changes no claim.
    static constexpr Waits guard_waits = Waits::state_device | Waits::data_device | Waits::locks;
    static constexpr ThreadSafety guard_safety = ThreadSafety::thread_safe;
    ClaimGuard guard(std::span<const ObjectId> ids) const MACHA_EXCLUDES(no_io) {
        return locks_.lock(ids);
    }
    ClaimGuard guard(const ObjectId& id) const MACHA_EXCLUDES(no_io) {
        return locks_.lock(std::span<const ObjectId>(&id, 1));
    }

    // Every operation takes the store's lock, which writes, lookups (a
    // ledger node read) and compaction hold across their state-device I/O
    // and nothing holds across DATA or network I/O.

    // Claim writes: one durable journal frame per 65,536 ids.
    static constexpr Waits write_waits = Waits::state_device | Waits::locks;
    static constexpr ThreadSafety write_safety = ThreadSafety::thread_safe;
    virtual void retain(RetentionClass, const ObjectId&, const RetentionDot&) = 0;
    virtual void retain_batch(RetentionClass, const std::vector<ObjectId>&,
                              const RetentionDot&) = 0;

    // Claim reads, from the class's ledger; `retained_ids` and
    // `claim_objects` are linear in the class's claims.
    static constexpr Waits read_waits = Waits::state_device | Waits::locks;
    static constexpr ThreadSafety read_safety = ThreadSafety::thread_safe;
    virtual bool retained(RetentionClass, const ObjectId&) const = 0;
    virtual std::optional<ObjectId> next_retained(RetentionClass, std::optional<ObjectId>& cursor,
                                                  bool& complete) const = 0;
    virtual std::vector<ObjectId> retained_ids(RetentionClass) const = 0;
    virtual size_t claim_objects(RetentionClass) const = 0;
    // Diagnostic: the observed-remove state of one object, `adds` and
    // `removed` keyed by origin. Empty when the object has no state. A read.
    struct Claims {
        std::map<NodeId, uint64_t> adds;
        std::map<NodeId, uint64_t> removed;
    };
    virtual Claims claims(RetentionClass, const ObjectId&) const = 0;

    // Release the claims a complete horizon no longer refers to and that its
    // clock has observed; `live` sorted and unique. Bounded; one journal
    // frame per slice. Single owner (the maintenance pass): the store keeps
    // the walk's position.
    static constexpr Waits release_waits = write_waits;
    static constexpr ThreadSafety release_safety = ThreadSafety::single_owner;
    virtual size_t release_unreferenced(RetentionClass, const IdLookup& live,
                                        const RetentionClock& observed,
                                        size_t operation_budget) = 0;
    // Forget causality tombstones once no claim remains and the object is
    // absent. Bounded; one journal frame. `exists` is called without the
    // lock, so this also waits on whatever `exists` waits on. Single owner
    // (the maintenance pass): the store keeps the walk's position.
    static constexpr Waits prune_waits = Waits::state_device | Waits::locks;
    static constexpr ThreadSafety prune_safety = ThreadSafety::single_owner;
    virtual size_t prune_unclaimed(RetentionClass,
                                   const std::function<bool(const ObjectId&)>& exists,
                                   size_t operation_budget) = 0;
    // Checkpoint the ledger and empty the journal past a threshold.
    static constexpr Waits compact_waits = Waits::state_device | Waits::locks;
    static constexpr ThreadSafety compact_safety = ThreadSafety::thread_safe;
    virtual bool compact_if_needed(size_t record_threshold = 4096) = 0;
};

} // namespace macha

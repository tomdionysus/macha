// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/id_lookup.hpp"
#include "contract/thread_safety.hpp"
#include "contract/work.hpp"
#include "types.hpp"

#include "dagda/contract/claim_store.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <vector>

// The claims a node holds (object ledger spec, B3, "claimed"): the objects it
// has durably promised to keep, by causal dot. Owned by the storage layer;
// above the ledger it is reached only through the ledger. Implemented by
// RetentionStore.
namespace macha {

// A claim's class, its causal dot (the writing node and its sequence), and
// what a metadata head has observed of each node's claim writes: Dagda's.
using RetentionClass = dagda::contract::RetentionClass;
using RetentionDot = dagda::contract::RetentionDot;
using RetentionClock = dagda::contract::RetentionClock;

// This node's guard over an object's claims against its copy, so no claim
// lands between a delete's check that none stands and the delete. A claim's
// presence check and its write hold the guard for the ids they touch. A
// delete holds it only to check and to mark the id as being deleted, then
// releases it and unlinks under the mark alone, so the unlink waits on no
// stripe and a claim path never waits on the DATA device: a claim that
// arrives meanwhile finds the id marked and treats the object as not
// present. Striped by the id's first byte; a batch takes its stripes in
// ascending order, so two batches never deadlock. Taken before the object
// store's and the claim store's locks, never under them; a holder waits on
// the state device (a claim's journal write).
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

// A delete's leave to go: `claimed` when a claim stood at the check, so the
// delete must not go; otherwise the id is marked until this goes out of
// scope.
class ClaimDeletion {
    class ClaimLocks* locks_{};
    std::optional<ObjectId> marked_;
    bool claimed_{};
    friend class ClaimLocks;
    friend class ClaimStore;

  public:
    ClaimDeletion() = default;
    ClaimDeletion(ClaimDeletion&& other) noexcept
        : locks_(other.locks_), marked_(other.marked_), claimed_(other.claimed_) {
        other.locks_ = nullptr;
        other.marked_.reset();
    }
    ClaimDeletion& operator=(ClaimDeletion&& other) noexcept {
        if (this != &other) {
            release();
            locks_ = other.locks_;
            marked_ = other.marked_;
            claimed_ = other.claimed_;
            other.locks_ = nullptr;
            other.marked_.reset();
        }
        return *this;
    }
    ClaimDeletion(const ClaimDeletion&) = delete;
    ClaimDeletion& operator=(const ClaimDeletion&) = delete;
    ~ClaimDeletion() {
        release();
    }
    bool claimed() const noexcept {
        return claimed_;
    }

  private:
    inline void release() noexcept;
};

class ClaimLocks {
    static constexpr size_t stripes = 64;
    std::array<IoMutex, stripes> stripes_;
    mutable Mutex deleting_mutex_;
    // One instance per delete under way: two may mark the same id.
    std::multiset<ObjectId> deleting_ MACHA_GUARDED_BY(deleting_mutex_);
    friend class ClaimDeletion;

    void end_deleting(const ObjectId& id) noexcept {
        Lock lock(deleting_mutex_);
        if (const auto found = deleting_.find(id); found != deleting_.end())
            deleting_.erase(found);
    }

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
    // Under the id's stripe (held by the caller): whether a delete has it.
    bool deleting(const ObjectId& id) const {
        Lock lock(deleting_mutex_);
        return deleting_.contains(id);
    }
    // Under the id's stripe (held by the caller): marks it until the result
    // goes out of scope.
    ClaimDeletion begin_deleting(const ObjectId& id) {
        {
            Lock lock(deleting_mutex_);
            deleting_.insert(id);
        }
        ClaimDeletion deletion;
        deletion.locks_ = this;
        deletion.marked_ = id;
        return deletion;
    }
};

inline void ClaimDeletion::release() noexcept {
    if (locks_ && marked_)
        locks_->end_deleting(*marked_);
    locks_ = nullptr;
    marked_.reset();
}

class ClaimStore {
    mutable ClaimLocks locks_;

  public:
    virtual ~ClaimStore() = default;

    // The guard above, for the ids a claim path checks and claims. Waits on
    // the state device, as other holders do. Const: a guard changes no claim.
    static constexpr Waits guard_waits = Waits::state_device | Waits::locks;
    static constexpr ThreadSafety guard_safety = ThreadSafety::thread_safe;
    ClaimGuard guard(std::span<const ObjectId> ids) const MACHA_EXCLUDES(no_io) {
        return locks_.lock(ids);
    }
    ClaimGuard guard(const ObjectId& id) const MACHA_EXCLUDES(no_io) {
        return locks_.lock(std::span<const ObjectId>(&id, 1));
    }
    // Under the guard: whether a delete has the object, in which case a
    // claim treats it as not present.
    bool deleting(const ObjectId& id) const {
        return locks_.deleting(id);
    }
    // A delete's leave: under the id's stripe, no claim stands and the id is
    // marked, or `claimed`. The stripe is released before this returns.
    static constexpr Waits deletion_waits = guard_waits;
    static constexpr ThreadSafety deletion_safety = ThreadSafety::thread_safe;
    ClaimDeletion deletion(RetentionClass type, const ObjectId& id) const MACHA_EXCLUDES(no_io) {
        const auto stripe = locks_.lock(std::span<const ObjectId>(&id, 1));
        if (retained(type, id)) {
            ClaimDeletion claimed;
            claimed.claimed_ = true;
            return claimed;
        }
        return locks_.begin_deleting(id);
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

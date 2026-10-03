// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace macha {

enum class MemoryClass : uint8_t { control, viewer, loader, speculative };

enum class MemoryOwner : uint8_t {
    fuse_request,
    fuse_operation,
    publication,
    rpc_frame,
    object_payload,
    durability,
    metadata,
    catalogue,
    media_profile,
    playback_segment,
    cache,
    count,
};

struct RetainedMemoryStats {
    uint64_t capacity_bytes{};
    uint64_t control_reserve_bytes{};
    uint64_t viewer_reserve_bytes{};
    uint64_t loader_reserve_bytes{};
    uint64_t reassembly_reserve_bytes{};
    uint64_t used_bytes{};
    uint64_t peak_used_bytes{};
    uint64_t reclaimable_bytes{};
    std::array<uint64_t, static_cast<size_t>(MemoryOwner::count)> owner_bytes{};
    std::array<uint64_t, 4> admissions{};
    std::array<uint64_t, 4> waits{};
    uint64_t shed_requests{};
    uint64_t cancelled_waits{};
    uint64_t restored_bytes{};
};

// One process-wide ownership ledger for heap retained across an asynchronous
// boundary. Durable/non-reconstructible lower-priority work cannot consume
// protected headroom. Reconstructible owners may borrow idle headroom only when
// they provide a shed callback; higher-priority admission requests that memory
// back synchronously before waiting.
class RetainedMemoryLedger {
  public:
    using Clock = std::chrono::steady_clock;

    class Lease {
        friend class RetainedMemoryLedger;
        RetainedMemoryLedger* owner_{};
        uint64_t id_{};
        uint64_t bytes_{};

        Lease(RetainedMemoryLedger& owner, uint64_t id, uint64_t bytes)
            : owner_(&owner), id_(id), bytes_(bytes) {}

      public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : owner_(other.owner_), id_(other.id_), bytes_(other.bytes_) {
            other.owner_ = nullptr;
            other.id_ = 0;
            other.bytes_ = 0;
        }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = other.owner_;
                id_ = other.id_;
                bytes_ = other.bytes_;
                other.owner_ = nullptr;
                other.id_ = 0;
                other.bytes_ = 0;
            }
            return *this;
        }
        ~Lease() { reset(); }
        void reset();
        uint64_t bytes() const noexcept { return bytes_; }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };

  private:
    struct Allocation {
        MemoryClass memory_class{};
        MemoryOwner owner{};
        uint64_t bytes{};
        bool reclaimable{};
        bool shed_requested{};
        std::function<void()> shed;
    };

    uint64_t capacity_bytes_{};
    uint64_t control_reserve_bytes_{};
    uint64_t viewer_reserve_bytes_{};
    uint64_t loader_reserve_bytes_{};
    uint64_t reassembly_reserve_bytes_{};
    mutable Mutex mutex_;
    std::condition_variable cv_;
    std::map<uint64_t, Allocation> allocations_ MACHA_GUARDED_BY(mutex_);
    uint64_t next_id_ MACHA_GUARDED_BY(mutex_){1};
    uint64_t used_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t reclaimable_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t lower_durable_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t speculative_durable_bytes_ MACHA_GUARDED_BY(mutex_){};
    uint64_t peak_used_bytes_ MACHA_GUARDED_BY(mutex_){};
    std::array<uint64_t, static_cast<size_t>(MemoryOwner::count)> owner_bytes_{};
    std::array<uint64_t, 4> admissions_ MACHA_GUARDED_BY(mutex_){};
    std::array<uint64_t, 4> waits_ MACHA_GUARDED_BY(mutex_){};
    std::array<uint64_t, 4> waiters_ MACHA_GUARDED_BY(mutex_){};
    uint64_t shed_requests_ MACHA_GUARDED_BY(mutex_){};
    uint64_t cancelled_waits_ MACHA_GUARDED_BY(mutex_){};
    uint64_t restored_bytes_ MACHA_GUARDED_BY(mutex_){};
    bool stopping_ MACHA_GUARDED_BY(mutex_){};

    static constexpr size_t index(MemoryClass value) {
        return static_cast<size_t>(value);
    }
    static constexpr size_t index(MemoryOwner value) {
        return static_cast<size_t>(value);
    }
    static constexpr bool lower(MemoryClass value) {
        return value == MemoryClass::loader || value == MemoryClass::speculative;
    }
    static constexpr int priority(MemoryClass value) {
        return 3 - static_cast<int>(value);
    }
    uint64_t charge(uint64_t bytes) const noexcept { return std::max<uint64_t>(1, bytes); }
    bool available_locked(MemoryClass, MemoryOwner, uint64_t, bool reclaimable) const MACHA_REQUIRES(mutex_);
    std::vector<std::function<void()>> request_shedding_locked(MemoryClass) MACHA_REQUIRES(mutex_);
    void release(uint64_t id);

  public:
    RetainedMemoryLedger(uint64_t capacity_bytes, uint64_t control_reserve_bytes,
                         uint64_t viewer_reserve_bytes, uint64_t loader_reserve_bytes,
                         uint64_t reassembly_reserve_bytes = 32ULL * 1024 * 1024);

    std::optional<Lease> acquire(MemoryClass, MemoryOwner, uint64_t bytes,
                                 Clock::time_point deadline = {},
                                 std::atomic_bool* cancelled = nullptr,
                                 bool reclaimable = false,
                                 std::function<void()> shed = {});
    std::optional<Lease> try_acquire(MemoryClass, MemoryOwner, uint64_t bytes,
                                     bool reclaimable = false,
                                     std::function<void()> shed = {});
    // Reconstruct ownership for already-acknowledged durable work. Recovery is
    // never refused merely because an operator lowered the limit; the resulting
    // overcommit blocks new admissions until normal completion releases it.
    Lease restore(MemoryClass, MemoryOwner, uint64_t bytes);
    void stop();
    RetainedMemoryStats stats() const;
};

inline RetainedMemoryLedger::RetainedMemoryLedger(uint64_t capacity_bytes,
                                                   uint64_t control_reserve_bytes,
                                                   uint64_t viewer_reserve_bytes,
                                                   uint64_t loader_reserve_bytes,
                                                   uint64_t reassembly_reserve_bytes)
    : capacity_bytes_(capacity_bytes), control_reserve_bytes_(control_reserve_bytes),
      viewer_reserve_bytes_(viewer_reserve_bytes), loader_reserve_bytes_(loader_reserve_bytes),
      reassembly_reserve_bytes_(reassembly_reserve_bytes) {
    if (!capacity_bytes_ || !control_reserve_bytes_ || !viewer_reserve_bytes_ ||
        !loader_reserve_bytes_ || !reassembly_reserve_bytes_ ||
        control_reserve_bytes_ > capacity_bytes_ ||
        viewer_reserve_bytes_ > capacity_bytes_ - control_reserve_bytes_ ||
        loader_reserve_bytes_ >
            capacity_bytes_ - control_reserve_bytes_ - viewer_reserve_bytes_)
        throw std::invalid_argument("retained-memory reserves exceed capacity");
}

inline bool RetainedMemoryLedger::available_locked(MemoryClass memory_class, MemoryOwner owner,
                                                    uint64_t bytes, bool reclaimable) const {
    if (bytes > capacity_bytes_ || used_bytes_ > capacity_bytes_ - bytes)
        return false;
    if (memory_class == MemoryClass::control)
        return true;
    const auto non_control_capacity = capacity_bytes_ - control_reserve_bytes_;
    if (bytes > non_control_capacity || used_bytes_ > non_control_capacity - bytes)
        return false;
    if (memory_class == MemoryClass::viewer)
        return true;
    if (waiters_[index(MemoryClass::control)] || waiters_[index(MemoryClass::viewer)])
        return false;
    // Inbound RPC reassembly gets a small dedicated reserve ahead of the gates
    // below: it releases what they protect, since publication holds its bytes until
    // a peer's confirmation frame is reassembled here. Without the reserve the
    // loader gate and durable-lower budget deadlock against it.
    // It sits below the control/viewer waiter gate (law 2: the viewer never waits)
    // and is bounded, so inbound frames from several peers cannot starve this
    // node's own publication; beyond it reassembly queues like anything else.
    if (owner == MemoryOwner::rpc_frame &&
        owner_bytes_[index(MemoryOwner::rpc_frame)] + bytes <= reassembly_reserve_bytes_)
        return true;
    if (memory_class == MemoryClass::speculative && waiters_[index(MemoryClass::loader)])
        return false;
    if (reclaimable)
        return true;
    // RPC reassembly is exempt from the durable-lower budget: completing a durable
    // write is what releases that budget, so charging both to it deadlocks. The
    // exemption is bounded by MessageAssembler's max_partial_bytes_; the class
    // reserves above still apply.
    if (owner == MemoryOwner::rpc_frame)
        return true;
    const auto durable_lower_capacity = non_control_capacity - viewer_reserve_bytes_;
    if (bytes > durable_lower_capacity || lower_durable_bytes_ > durable_lower_capacity - bytes)
        return false;
    if (memory_class == MemoryClass::loader)
        return true;
    const auto speculative_capacity = durable_lower_capacity - loader_reserve_bytes_;
    return bytes <= speculative_capacity &&
           speculative_durable_bytes_ <= speculative_capacity - bytes;
}

inline std::vector<std::function<void()>>
RetainedMemoryLedger::request_shedding_locked(MemoryClass incoming) {
    std::vector<std::function<void()>> callbacks;
    for (auto& [_, allocation] : allocations_) {
        if (!allocation.reclaimable || allocation.shed_requested ||
            priority(allocation.memory_class) >= priority(incoming))
            continue;
        allocation.shed_requested = true;
        ++shed_requests_;
        callbacks.push_back(allocation.shed);
    }
    return callbacks;
}

inline std::optional<RetainedMemoryLedger::Lease>
RetainedMemoryLedger::acquire(MemoryClass memory_class, MemoryOwner owner,
                              uint64_t requested_bytes, Clock::time_point deadline,
                              std::atomic_bool* cancelled, bool reclaimable,
                              std::function<void()> shed) {
    if (reclaimable && !shed)
        throw std::invalid_argument("reclaimable retained memory requires a shed callback");
    const auto bytes = charge(requested_bytes);
    const auto absolute_class_capacity =
        memory_class == MemoryClass::control
            ? capacity_bytes_
            : capacity_bytes_ - control_reserve_bytes_;
    if (bytes > absolute_class_capacity)
        return {};
    Lock lock(mutex_);
    bool counted_wait = false;
    for (;;) {
        if (stopping_ || (cancelled && cancelled->load(std::memory_order_relaxed)) ||
            (deadline != Clock::time_point{} && Clock::now() >= deadline)) {
            if (counted_wait)
                --waiters_[index(memory_class)];
            ++cancelled_waits_;
            cv_.notify_all();
            return {};
        }
        if (available_locked(memory_class, owner, bytes, reclaimable))
            break;
        if (!counted_wait) {
            counted_wait = true;
            ++waiters_[index(memory_class)];
            ++waits_[index(memory_class)];
        }
        auto callbacks = request_shedding_locked(memory_class);
        if (!callbacks.empty()) {
            lock.unlock();
            for (auto& callback : callbacks) {
                try {
                    callback();
                } catch (...) {
                    // Shedding is best effort. The still-live lease remains
                    // charged and the higher-priority waiter stays bounded.
                }
            }
            lock.lock();
            continue;
        }
        if (deadline == Clock::time_point{})
            cv_.wait(lock.native());
        else
            cv_.wait_until(lock.native(), deadline);
    }
    if (counted_wait)
        --waiters_[index(memory_class)];
    const auto id = next_id_++;
    allocations_.emplace(id, Allocation{memory_class, owner, bytes, reclaimable, false,
                                        std::move(shed)});
    used_bytes_ += bytes;
    owner_bytes_[index(owner)] += bytes;
    if (reclaimable)
        reclaimable_bytes_ += bytes;
    else if (lower(memory_class)) {
        lower_durable_bytes_ += bytes;
        if (memory_class == MemoryClass::speculative)
            speculative_durable_bytes_ += bytes;
    }
    peak_used_bytes_ = std::max(peak_used_bytes_, used_bytes_);
    ++admissions_[index(memory_class)];
    return Lease(*this, id, bytes);
}

inline RetainedMemoryLedger::Lease RetainedMemoryLedger::restore(
    MemoryClass memory_class, MemoryOwner owner, uint64_t bytes) {
    bytes = charge(bytes);
    Lock lock(mutex_);
    const auto id = next_id_++;
    allocations_.emplace(id, Allocation{memory_class, owner, bytes, false, false, {}});
    used_bytes_ += bytes;
    owner_bytes_[index(owner)] += bytes;
    if (lower(memory_class)) {
        lower_durable_bytes_ += bytes;
        if (memory_class == MemoryClass::speculative)
            speculative_durable_bytes_ += bytes;
    }
    restored_bytes_ += bytes;
    peak_used_bytes_ = std::max(peak_used_bytes_, used_bytes_);
    return Lease(*this, id, bytes);
}

inline std::optional<RetainedMemoryLedger::Lease>
RetainedMemoryLedger::try_acquire(MemoryClass memory_class, MemoryOwner owner, uint64_t bytes,
                                  bool reclaimable, std::function<void()> shed) {
    if (reclaimable && !shed)
        throw std::invalid_argument("reclaimable retained memory requires a shed callback");
    bytes = charge(bytes);
    Lock lock(mutex_);
    if (stopping_ || !available_locked(memory_class, owner, bytes, reclaimable))
        return {};
    const auto id = next_id_++;
    allocations_.emplace(id, Allocation{memory_class, owner, bytes, reclaimable, false,
                                        std::move(shed)});
    used_bytes_ += bytes;
    owner_bytes_[index(owner)] += bytes;
    if (reclaimable)
        reclaimable_bytes_ += bytes;
    else if (lower(memory_class)) {
        lower_durable_bytes_ += bytes;
        if (memory_class == MemoryClass::speculative)
            speculative_durable_bytes_ += bytes;
    }
    peak_used_bytes_ = std::max(peak_used_bytes_, used_bytes_);
    ++admissions_[index(memory_class)];
    return Lease(*this, id, bytes);
}

inline void RetainedMemoryLedger::release(uint64_t id) {
    Lock lock(mutex_);
    const auto found = allocations_.find(id);
    if (found == allocations_.end())
        return;
    const auto allocation = std::move(found->second);
    allocations_.erase(found);
    used_bytes_ -= allocation.bytes;
    owner_bytes_[index(allocation.owner)] -= allocation.bytes;
    if (allocation.reclaimable)
        reclaimable_bytes_ -= allocation.bytes;
    else if (lower(allocation.memory_class)) {
        lower_durable_bytes_ -= allocation.bytes;
        if (allocation.memory_class == MemoryClass::speculative)
            speculative_durable_bytes_ -= allocation.bytes;
    }
    cv_.notify_all();
}

inline void RetainedMemoryLedger::Lease::reset() {
    if (!owner_)
        return;
    owner_->release(id_);
    owner_ = nullptr;
    id_ = 0;
    bytes_ = 0;
}

inline void RetainedMemoryLedger::stop() {
    Lock lock(mutex_);
    stopping_ = true;
    cv_.notify_all();
}

inline RetainedMemoryStats RetainedMemoryLedger::stats() const {
    Lock lock(mutex_);
    return {capacity_bytes_, control_reserve_bytes_, viewer_reserve_bytes_,
            loader_reserve_bytes_, reassembly_reserve_bytes_, used_bytes_, peak_used_bytes_,
            reclaimable_bytes_,
            owner_bytes_, admissions_, waits_, shed_requests_, cancelled_waits_, restored_bytes_};
}

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace macha {

struct ConvergenceDemandSnapshot {
    uint64_t epoch{};
    uint64_t generation{};
};

struct ConvergenceDemandDiagnostics {
    uint64_t events_received{};
    uint64_t runs_scheduled{};
    uint64_t runs_completed{};
    uint64_t requested_epoch{};
    uint64_t completed_epoch{};
    uint64_t latest_generation{};
    bool scheduled{};
};

// Single-consumer, multi-producer edge-triggered demand. Every semantically
// relevant event advances the epoch, including a same-generation sibling or
// topology transition. Generations are a diagnostic high-water mark, not the
// identity of the demand.
class ConvergenceDemand {
    std::atomic_uint64_t requested_epoch_{};
    std::atomic_uint64_t completed_epoch_{};
    std::atomic_uint64_t latest_generation_{};
    std::atomic_bool scheduled_{};
    std::atomic_uint64_t events_received_{};
    std::atomic_uint64_t runs_scheduled_{};
    std::atomic_uint64_t runs_completed_{};

    bool schedule_edge() noexcept {
        bool expected = false;
        if (scheduled_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
            runs_scheduled_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

  public:
    bool request(uint64_t generation) noexcept {
        auto current = latest_generation_.load(std::memory_order_relaxed);
        while (current < generation &&
               !latest_generation_.compare_exchange_weak(
                   current, generation, std::memory_order_relaxed)) {
        }
        events_received_.fetch_add(1, std::memory_order_relaxed);
        requested_epoch_.fetch_add(1, std::memory_order_release);
        return schedule_edge();
    }

    std::optional<ConvergenceDemandSnapshot> begin() const noexcept {
        if (!scheduled_.load(std::memory_order_acquire))
            return std::nullopt;
        // request() publishes the generation before the epoch release.
        const auto epoch = requested_epoch_.load(std::memory_order_acquire);
        return ConvergenceDemandSnapshot{
            epoch, latest_generation_.load(std::memory_order_relaxed)};
    }

    // Returns true when an event arrived during the completed run and exactly
    // one follow-up pass is now scheduled. Clearing the edge before rechecking
    // the epoch closes both request/complete race orderings without polling.
    bool complete(const ConvergenceDemandSnapshot& run) noexcept {
        completed_epoch_.store(run.epoch, std::memory_order_release);
        runs_completed_.fetch_add(1, std::memory_order_relaxed);
        scheduled_.store(false, std::memory_order_release);
        if (requested_epoch_.load(std::memory_order_acquire) != run.epoch)
            schedule_edge();
        return scheduled_.load(std::memory_order_acquire);
    }

    bool pending() const noexcept {
        return scheduled_.load(std::memory_order_acquire);
    }

    ConvergenceDemandDiagnostics diagnostics() const noexcept {
        return {
            events_received_.load(std::memory_order_relaxed),
            runs_scheduled_.load(std::memory_order_relaxed),
            runs_completed_.load(std::memory_order_relaxed),
            requested_epoch_.load(std::memory_order_acquire),
            completed_epoch_.load(std::memory_order_acquire),
            latest_generation_.load(std::memory_order_relaxed),
            scheduled_.load(std::memory_order_acquire),
        };
    }
};

} // namespace macha

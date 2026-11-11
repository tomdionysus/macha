// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"

#include <dannan/sched/retained_memory.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace macha {

// Macha's holders of retained memory, for Status. Its frames are the transport's
// own owner; the rest are hosted owners, numbered from Dannan's hosted_first.
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

dannan::sched::MemoryOwner dannan_owner(MemoryOwner owner) noexcept;

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

// The node's ledger of heap retained across an asynchronous boundary, in Macha's
// terms: one per node, the node's own (Dannan's), so the transport's frames and
// Macha's holders share one total and one set of reserves by class. Standalone
// (tests, tools), it owns a ledger of its own. Thread-safe.
class RetainedMemoryLedger {
  public:
    using Clock = std::chrono::steady_clock;
    using Lease = dannan::sched::RetainedMemoryLedger::Lease;

    // A ledger of its own. Throws std::invalid_argument unless every reserve is
    // nonzero and they fit inside the total: control, viewer, loader, then reassembly.
    RetainedMemoryLedger(uint64_t capacity_bytes, uint64_t control_reserve_bytes,
                         uint64_t viewer_reserve_bytes, uint64_t loader_reserve_bytes,
                         uint64_t reassembly_reserve_bytes);
    // The node's; it outlives this view.
    explicit RetainedMemoryLedger(dannan::sched::RetainedMemoryLedger& node);
    RetainedMemoryLedger(const RetainedMemoryLedger&) = delete;
    RetainedMemoryLedger& operator=(const RetainedMemoryLedger&) = delete;

    // Waits until `bytes` may be held, `deadline` passes (none when default),
    // `cancelled` is set and the ledger woken, or the ledger stops.
    std::optional<Lease> acquire(WorkClass, MemoryOwner, uint64_t bytes,
                                 Clock::time_point deadline = {},
                                 std::atomic_bool* cancelled = nullptr, bool reclaimable = false,
                                 std::function<void()> shed = {});
    std::optional<Lease> try_acquire(WorkClass, MemoryOwner, uint64_t bytes,
                                     bool reclaimable = false, std::function<void()> shed = {});
    // Ownership of work already acknowledged, reconstructed at recovery; never refused.
    Lease restore(WorkClass, MemoryOwner, uint64_t bytes);
    // Wakes every waiter to re-check its cancellation flag.
    void wake();
    void stop();
    RetainedMemoryStats stats() const;

  private:
    std::unique_ptr<dannan::sched::RetainedMemoryLedger> owned_;
    dannan::sched::RetainedMemoryLedger& ledger_;
};

} // namespace macha

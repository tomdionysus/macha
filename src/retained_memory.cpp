// SPDX-License-Identifier: GPL-3.0-or-later
#include "retained_memory.hpp"

namespace macha {

namespace {
dannan::sched::WorkClass dannan_work(WorkClass work) noexcept {
    return static_cast<dannan::sched::WorkClass>(static_cast<uint8_t>(work));
}
} // namespace

dannan::sched::MemoryOwner dannan_owner(MemoryOwner owner) noexcept {
    if (owner == MemoryOwner::rpc_frame)
        return dannan::sched::MemoryOwner::rpc_frame;
    return static_cast<dannan::sched::MemoryOwner>(
        static_cast<uint16_t>(dannan::sched::MemoryOwner::hosted_first) +
        static_cast<uint16_t>(owner));
}

RetainedMemoryLedger::RetainedMemoryLedger(uint64_t capacity_bytes,
                                           uint64_t control_reserve_bytes,
                                           uint64_t viewer_reserve_bytes,
                                           uint64_t loader_reserve_bytes,
                                           uint64_t reassembly_reserve_bytes)
    : owned_(std::make_unique<dannan::sched::RetainedMemoryLedger>(
          capacity_bytes, control_reserve_bytes, viewer_reserve_bytes, loader_reserve_bytes,
          reassembly_reserve_bytes)),
      ledger_(*owned_) {}

RetainedMemoryLedger::RetainedMemoryLedger(dannan::sched::RetainedMemoryLedger& node)
    : ledger_(node) {}

std::optional<RetainedMemoryLedger::Lease>
RetainedMemoryLedger::acquire(WorkClass work, MemoryOwner owner, uint64_t bytes,
                              Clock::time_point deadline, std::atomic_bool* cancelled,
                              bool reclaimable, std::function<void()> shed) {
    return ledger_.acquire(dannan_work(work), dannan_owner(owner), bytes, deadline, cancelled,
                           reclaimable, std::move(shed));
}

std::optional<RetainedMemoryLedger::Lease>
RetainedMemoryLedger::try_acquire(WorkClass work, MemoryOwner owner, uint64_t bytes,
                                  bool reclaimable, std::function<void()> shed) {
    return ledger_.try_acquire(dannan_work(work), dannan_owner(owner), bytes, reclaimable,
                               std::move(shed));
}

RetainedMemoryLedger::Lease RetainedMemoryLedger::restore(WorkClass work, MemoryOwner owner,
                                                          uint64_t bytes) {
    return ledger_.restore(dannan_work(work), dannan_owner(owner), bytes);
}

void RetainedMemoryLedger::wake() {
    ledger_.wake();
}

void RetainedMemoryLedger::stop() {
    ledger_.stop();
}

RetainedMemoryStats RetainedMemoryLedger::stats() const {
    const auto stats = ledger_.stats();
    RetainedMemoryStats out;
    out.capacity_bytes = stats.capacity_bytes;
    out.control_reserve_bytes = stats.control_reserve_bytes;
    out.viewer_reserve_bytes = stats.foreground_reserve_bytes;
    out.loader_reserve_bytes = stats.loader_reserve_bytes;
    out.reassembly_reserve_bytes = stats.reassembly_reserve_bytes;
    out.used_bytes = stats.used_bytes;
    out.peak_used_bytes = stats.peak_used_bytes;
    out.reclaimable_bytes = stats.reclaimable_bytes;
    for (size_t i = 0; i < out.owner_bytes.size(); ++i) {
        const auto found = stats.owner_bytes.find(dannan_owner(static_cast<MemoryOwner>(i)));
        if (found != stats.owner_bytes.end())
            out.owner_bytes[i] = found->second;
    }
    out.admissions = stats.admissions;
    out.waits = stats.waits;
    out.shed_requests = stats.shed_requests;
    out.cancelled_waits = stats.cancelled_waits;
    out.restored_bytes = stats.restored_bytes;
    return out;
}

} // namespace macha

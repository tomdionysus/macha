// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace macha {

enum class DurabilityUrgency : uint8_t {
    batchable,
    immediate,
};

// One DurabilityDomain owns physical stable-storage barriers for one mounted
// filesystem. Stores never issue data fsync/syncfs themselves: they report a
// completed filesystem mutation, receive a monotonically increasing generation,
// and callers wait for the required generation through this coordinator.
//
// Generations are deliberately process-local. Cluster callers bind them to the
// node process epoch before using them as remote durability evidence.
class DurabilityDomain {
  public:
    using Generation = uint64_t;

  private:
    struct PortableMutation {
        Generation generation{};
        std::filesystem::path file;
        std::filesystem::path directory;
    };

    uint64_t id_{};
    std::vector<std::filesystem::path> representatives_;
    std::chrono::milliseconds batch_window_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::jthread worker_;
    Generation mutation_generation_{};
    Generation durable_generation_{};
    Generation requested_generation_{};
    bool immediate_requested_{};
    std::optional<std::chrono::steady_clock::time_point> batch_deadline_;
    std::exception_ptr failure_;
    std::vector<PortableMutation> portable_mutations_;
    std::atomic_uint64_t physical_barriers_{};

    void loop(std::stop_token);
    void perform_barrier(Generation cut, std::vector<PortableMutation> portable);

  public:
    DurabilityDomain(uint64_t id, std::filesystem::path representative,
                     std::chrono::milliseconds batch_window = std::chrono::milliseconds(500));
    ~DurabilityDomain();
    DurabilityDomain(const DurabilityDomain&) = delete;
    DurabilityDomain& operator=(const DurabilityDomain&) = delete;

    uint64_t id() const noexcept { return id_; }
    void add_representative(std::filesystem::path);

    // Called only after the mutation's write/rename/unlink syscalls have
    // completed. A barrier cut taken before this call cannot claim the mutation;
    // a cut taken after it can. That ordering is the core generation invariant.
    Generation complete_mutation(std::filesystem::path file = {},
                                 std::filesystem::path directory = {});

    void await_durable(Generation, DurabilityUrgency = DurabilityUrgency::batchable);
    void flush();

    Generation current_generation() const;
    Generation durable_generation() const;
    bool failed() const;
    uint64_t physical_barriers() const noexcept {
        return physical_barriers_.load(std::memory_order_relaxed);
    }
};

} // namespace macha

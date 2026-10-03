// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/node_identity.hpp"
#include "config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace macha {

class ControlObjectFetch;
class LocalStore;
class MetadataReplica;
class NodeRuntime;
class PersistentBlockCache;
class RetentionStore;
class StoragePool;

// How far this node's local state has recovered: plane by plane, the first
// failure, and completion once local state and the parts built from it
// exist. LocalState marks the planes, whoever builds those parts marks
// completion; the node's readiness, Status and the startup diagnostics read
// it. Thread-safe.
class RecoveryProgress {
  public:
    enum Plane : uint32_t {
        data_storage = 1U << 0,
        control_storage = 1U << 1,
        cache = 1U << 2,
        retention = 1U << 3,
        metadata = 1U << 4,
    };
    void mark(Plane plane) noexcept { planes_.fetch_or(plane, std::memory_order_acq_rel); }
    bool has(Plane plane) const noexcept {
        return (planes_.load(std::memory_order_acquire) & plane) != 0;
    }
    // Keeps the first failure.
    void fail(std::string error) {
        {
            std::lock_guard lock(mutex_);
            if (!failed_.exchange(true, std::memory_order_acq_rel))
                error_ = std::move(error);
        }
        cv_.notify_all();
    }
    void mark_complete(uint64_t now_unix_ms) {
        {
            std::lock_guard lock(mutex_);
            ready_unix_ms_.store(now_unix_ms, std::memory_order_release);
            complete_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }
    bool complete() const noexcept { return complete_.load(std::memory_order_acquire); }
    uint64_t ready_unix_ms() const noexcept {
        return ready_unix_ms_.load(std::memory_order_acquire);
    }
    // Waits until complete, failed, or `timeout`; true when complete.
    bool wait_complete(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return complete() || failed(); });
        return complete();
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    std::string error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }

  private:
    std::atomic_uint32_t planes_{};
    std::atomic_bool failed_{};
    std::atomic_bool complete_{};
    std::atomic_uint64_t ready_unix_ms_{};
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::string error_;
};

// Recovery was stopped between stages; nothing failed.
struct RecoveryCancelled : std::runtime_error {
    RecoveryCancelled() : std::runtime_error("local state recovery cancelled") {}
};

// This node's local stores, recovered by construction: the DATA pool, and
// alongside it the control store, persistent cache, retention claims and the
// metadata replica (which keeps its records in the cache). Construction
// returns once both halves have recovered, marking each plane in `progress`
// as it completes. It throws RecoveryCancelled when `stop` is requested
// between stages, and otherwise, after recording the failure in `progress`,
// the first failure of either half.
class LocalState {
  public:
    using StageHook = std::function<void(std::string_view)>;
    LocalState(const Config&, const NodeIdentity&, NodeRuntime&, RecoveryProgress&,
               const StageHook&, std::stop_token);
    ~LocalState();
    LocalState(const LocalState&) = delete;
    LocalState& operator=(const LocalState&) = delete;

    StoragePool& data() noexcept { return *data_; }
    const StoragePool& data() const noexcept { return *data_; }
    LocalStore& control() noexcept { return *control_; }
    const LocalStore& control() const noexcept { return *control_; }
    ControlObjectFetch& control_fetch() noexcept { return *control_fetch_; }
    PersistentBlockCache& cache() noexcept { return *cache_; }
    RetentionStore& retention() noexcept { return *retention_; }
    const RetentionStore& retention() const noexcept { return *retention_; }
    MetadataReplica& replica() noexcept { return *replica_; }
    const MetadataReplica& replica() const noexcept { return *replica_; }

    // Applies a live reload's storage backends and cache settings.
    void reconfigure(const Config&);

  private:
    void recover_data(const Config&, const NodeIdentity&, RecoveryProgress&, const StageHook&,
                      std::stop_token);
    void recover_state(const Config&, const NodeIdentity&, NodeRuntime&, RecoveryProgress&,
                       const StageHook&, std::stop_token);

    // The replica is declared after the cache it reads, so destroyed first.
    std::unique_ptr<StoragePool> data_;
    std::unique_ptr<LocalStore> control_;
    std::unique_ptr<ControlObjectFetch> control_fetch_;
    std::unique_ptr<PersistentBlockCache> cache_;
    std::unique_ptr<RetentionStore> retention_;
    std::unique_ptr<MetadataReplica> replica_;
};

} // namespace macha

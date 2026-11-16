// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"

#include "cluster/node_identity.hpp"
#include "config.hpp"
#include "metadata/metadata.hpp"
#include "metadata/metadata_seed.hpp"

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

class MetadataReplica;

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
    void mark(Plane plane) noexcept {
        planes_.fetch_or(plane, std::memory_order_acq_rel);
    }
    bool has(Plane plane) const noexcept {
        return (planes_.load(std::memory_order_acquire) & plane) != 0;
    }
    // Keeps the first failure.
    void fail(std::string error) {
        {
            Lock lock(mutex_);
            if (!failed_.exchange(true, std::memory_order_acq_rel))
                error_ = std::move(error);
        }
        cv_.notify_all();
    }
    void mark_complete(uint64_t now_unix_ms) {
        {
            Lock lock(mutex_);
            ready_unix_ms_.store(now_unix_ms, std::memory_order_release);
            complete_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }
    bool complete() const noexcept {
        return complete_.load(std::memory_order_acquire);
    }
    uint64_t ready_unix_ms() const noexcept {
        return ready_unix_ms_.load(std::memory_order_acquire);
    }
    // Waits until complete, failed, or `timeout`; true when complete.
    bool wait_complete(std::chrono::milliseconds timeout) const {
        Lock lock(mutex_);
        cv_.wait_for(lock.native(), timeout,
                     [this]() MACHA_REQUIRES(mutex_) { return complete() || failed(); });
        return complete();
    }
    bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }
    std::string error() const {
        Lock lock(mutex_);
        return error_;
    }

  private:
    std::atomic_uint32_t planes_{};
    std::atomic_bool failed_{};
    std::atomic_bool complete_{};
    std::atomic_uint64_t ready_unix_ms_{};
    mutable Mutex mutex_;
    mutable std::condition_variable cv_;
    std::string error_ MACHA_GUARDED_BY(mutex_);
};

// Recovery was stopped between stages; nothing failed.
struct RecoveryCancelled : std::runtime_error {
    RecoveryCancelled() : std::runtime_error("local state recovery cancelled") {}
};

// This node's own metadata state, recovered by construction: the metadata
// replica and its sealed seed (the store's parts are the store node's).
// Construction marks the metadata plane in `progress`. It throws
// RecoveryCancelled when `stop` is requested before it starts, and otherwise,
// after recording the failure in `progress`, the failure.
class LocalState {
  public:
    using StageHook = std::function<void(std::string_view)>;
    // `apply_namespace`: how metadata replay brings a namespace tree to a
    // delta, writing its nodes where the owner keeps them.
    LocalState(const Config&, const NodeIdentity&, NamespaceDeltaApplier apply_namespace,
               RecoveryProgress&, const StageHook&, const std::stop_token&);
    ~LocalState();
    LocalState(const LocalState&) = delete;
    LocalState& operator=(const LocalState&) = delete;

    MetadataSeed& seed() noexcept {
        return *seed_;
    }
    MetadataReplica& replica() noexcept {
        return *replica_;
    }

  private:
    std::unique_ptr<MetadataSeed> seed_;
    std::unique_ptr<MetadataReplica> replica_;
};

} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cluster.hpp"
#include "replica_selector.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace macha {
class DistributedStore {
  public:
    struct RepairResult {
        uint64_t bytes_transferred{};
        size_t push_examined{};
        size_t pull_examined{};
        size_t remote_operations{};
        bool complete{true};
        bool yielded{};
    };

  private:
    struct SharedFetch {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{};
        bool foreground{};
        FrameType frame_type{FrameType::speculative};
        std::function<void(FrameType)> promote_network;
        bool opportunistic_persist{};
        bool foreground_accounted{};
        bool persist_queued{};
        std::atomic_size_t waiters{};
        std::optional<NodeInfo> active_peer;
        ReplicaWorkClass active_class{ReplicaWorkClass::speculative};
        std::optional<Bytes> result;
    };

    NodeRuntime& n_;
    StoragePool::Cursor repair_push_cursor_;
    std::optional<ObjectId> repair_push_pending_;
    std::optional<ObjectId> repair_pull_after_;
    const std::vector<ObjectId>* repair_live_identity_{};
    uint64_t repair_live_generation_{};
    bool repair_push_complete_{};
    bool repair_pull_complete_{};
    std::atomic<double> network_bps_{};
    mutable std::mutex fetch_mutex_;
    std::map<ObjectId, std::weak_ptr<SharedFetch>> fetches_;
    ReplicaSelector replica_selector_;

    std::vector<NodeInfo> ranked(const ObjectId&) const;
    std::vector<NodeInfo> owners(const ObjectId&) const;
    bool put_on(const NodeInfo&, const ObjectId&, std::span<const uint8_t>, bool foreground);
    std::optional<Bytes> get_from(const NodeInfo&, const ObjectId&, FrameType,
                                  const std::shared_ptr<SharedFetch>&,
                                  Clock::time_point deadline, std::atomic_bool* cancelled,
                                  const std::function<bool()>& abort = {});
    std::optional<Bytes> get_remote(const ObjectId&, size_t stripe, FrameType, bool foreground,
                                    bool opportunistic_persist, Clock::time_point deadline = {},
                                    std::atomic_bool* cancelled = nullptr,
                                    const std::function<bool()>& abort = {});
    void note_foreground(uint64_t);
    void note_network(uint64_t, Clock::duration);

  public:
    explicit DistributedStore(NodeRuntime& n) : n_(n) {}
    ObjectId put(std::span<const uint8_t>, std::atomic_bool* cancelled = nullptr);
    bool put(const ObjectId&, std::span<const uint8_t>, std::atomic_bool* cancelled = nullptr);
    std::optional<Bytes> get(const ObjectId&, size_t stripe = 0, bool foreground = true,
                             Clock::time_point deadline = {}, std::atomic_bool* cancelled = nullptr);
    std::optional<Bytes> get(const ObjectId&, size_t stripe, FrameType,
                             Clock::time_point deadline = {}, std::atomic_bool* cancelled = nullptr);
    bool has_on(const NodeInfo&, const ObjectId&);
    bool should_own(const ObjectId&) const;
    size_t replicate_all(const ObjectId&, std::span<const uint8_t>, bool foreground = false);
    size_t replicate_metadata_all(const ObjectId&, std::span<const uint8_t>);
    bool ensure_local(const ObjectId&, bool foreground = false);
    bool ensure_metadata_local(const ObjectId&);
    bool locally_available(const ObjectId&) const;
    bool cache_local(const ObjectId&, std::span<const uint8_t>);
    bool hydration_available() const;
    bool hydrate(const ObjectId&, size_t stripe = 0,
                 FrameType frame_type = FrameType::speculative);
    void erase_all(const ObjectId&);
    void foreground_activity(uint64_t bytes) { note_foreground(bytes); }
    void interactive_activity(uint64_t bytes) { n_.note_activity(FrameType::read_ahead, bytes); }

    // Converges remote placement and proactively pulls live objects for which
    // this node has become an owner. Bounded repair_step() calls retain push/pull
    // cursors across scheduler slices; they never rebuild complete object vectors.
    // The byte limit is network transfer, not block count; zero means unlimited.
    uint64_t repair_once(uint64_t byte_budget = 0, const std::vector<ObjectId>* live = nullptr,
                         const std::vector<ObjectId>* universal = nullptr);
    RepairResult repair_step(uint64_t byte_budget, size_t operation_budget,
                             const std::vector<ObjectId>* live = nullptr,
                             const std::vector<ObjectId>* universal = nullptr,
                             const std::function<bool()>& should_yield = {},
                             uint64_t live_generation = 0);
    uint64_t scrub_once(uint64_t byte_budget = 0);

    uint64_t take_foreground_bytes() { return n_.take_activity_bytes(FrameType::foreground); }
    uint64_t take_interactive_bytes() { return n_.take_activity_bytes(FrameType::read_ahead); }
    std::chrono::milliseconds foreground_idle_for() const;
    std::chrono::milliseconds interactive_idle_for() const {
        return n_.activity_idle_for(FrameType::read_ahead);
    }
    double estimated_network_bps() const {
        return network_bps_.load();
    }
};
} // namespace macha

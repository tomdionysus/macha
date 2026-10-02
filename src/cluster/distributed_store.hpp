// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cluster/cluster.hpp"
#include "contract/predicates.hpp"
#include "cluster/data_work.hpp"
#include "cluster/replica_selector.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <vector>

namespace macha {
// Where repair keeps its place across restarts (none: each pass starts at the
// beginning), and the repair decision trace (tests only).
struct DistributedStoreOptions {
    std::optional<std::filesystem::path> repair_position;
    std::function<void(std::string_view, std::string_view)> repair_trace;
};

class DistributedStore final : public Placement {
  public:
    struct ObjectBuffer {
        Bytes bytes;
        std::shared_ptr<std::vector<RetainedMemoryLedger::Lease>> retained_memory;
    };
    using ObjectData = std::shared_ptr<const ObjectBuffer>;

    struct RepairResult {
        uint64_t bytes_transferred{};
        size_t push_examined{};
        size_t pull_examined{};
        size_t remote_operations{};
        // Objects this pass wanted to pull and no peer would supply.
        size_t pull_unsourceable{};
        bool complete{true};
        bool yielded{};
        // The step stopped at an object that needs a transfer the byte
        // budget could not cover; that object is the first of the next step.
        bool credit_limited{};
    };

    // A cumulative count plus a bounded sample of ids, so Status can show
    // whether anything is unreachable.
    struct RepairDiagnostics {
        uint64_t pull_unsourceable{};
        uint64_t local_unreadable{};
        std::vector<ObjectId> unsourceable_sample;
        // Progress, cumulative since start.
        uint64_t push_examined{};
        uint64_t pull_examined{};
        uint64_t bytes_transferred{};
        uint64_t passes_completed{};
        bool push_phase_complete{};
        // Per maintenance pass: why a repair step did or did not run.
        // gate_credit counts steps stopped at a transfer credit could not cover.
        uint64_t gate_ran{};
        uint64_t gate_share{};
        uint64_t gate_quiescent{};
        uint64_t gate_credit{};
        uint64_t last_credit_bytes{};
        // Prompt replication (copy of each new object to a second owner),
        // cumulative since start.
        uint64_t prompt_queued{};
        uint64_t prompt_copies{};
        uint64_t prompt_failures{};
        uint64_t prompt_skipped_no_room{};
        uint64_t prompt_dropped{};
    };

    struct DurableReplica {
        NodeId id{};
        NodeId epoch{};
        uint64_t domain{};
        uint64_t generation{};
        uint64_t backend_instance{};

        friend bool operator==(const DurableReplica&, const DurableReplica&) = default;
    };

    struct DurabilityRequirement {
        ObjectId id{};
        size_t required{};
        std::vector<DurableReplica> replicas;
    };

    struct DurabilityBatch {
        std::vector<DurabilityRequirement> requirements;
        bool empty() const noexcept { return requirements.empty(); }
        void clear() { requirements.clear(); }
        void add(DurabilityRequirement);
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
        ObjectData result;
    };

    NodeRuntime& n_;
    ActivityClocks& activity_;
    DataResourceArbiter& data_resources_;
    RetainedMemoryLedger& retained_memory_;
    NodeEvents& events_;
    StoragePool::Cursor repair_push_cursor_;
    // Objects taken from the push cursor and not yet settled, in cursor order,
    // with one batched presence round's findings. A step that stops leaves
    // both in place; the next resumes at the same object.
    std::deque<ObjectId> repair_push_window_;
    std::map<std::pair<NodeId, ObjectId>, bool> repair_push_presence_;
    std::set<ObjectId> repair_push_probed_;
    bool repair_push_cursor_exhausted_{};
    // Maximum concurrent repair pushes.
    static constexpr size_t repair_sends_in_flight = 8;
    // Objects the current push pass has settled, saved so a restart resumes
    // there (persist_repair_position), and the value to resume from.
    uint64_t repair_push_settled_{};
    std::optional<uint64_t> repair_push_resume_;
    bool repair_pass_spans_change_{};
    std::filesystem::path repair_position_path_;
    Clock::time_point repair_position_saved_at_{};
    uint64_t repair_position_saved_{};
    void save_repair_position(bool force);
    std::optional<ObjectId> repair_pull_after_;
    // The live set's identity when no generation is given: its data, or none
    // for no live set.
    std::optional<const ObjectId*> repair_live_identity_;
    uint64_t repair_live_generation_{};
    bool repair_push_complete_{};
    bool repair_pull_complete_{};
    std::atomic<double> network_bps_{};
    // Prompt second copy. A put stops at min_write_replicas; objects that
    // reached only that floor are queued here and pushed to the next owner by
    // one worker as speculative DATA work, so a writer's death does not
    // strand recent data until the repair cursor comes round.
    std::mutex prompt_mutex_;
    std::condition_variable_any prompt_cv_;
    std::deque<ObjectId> prompt_queue_;
    std::set<ObjectId> prompt_queued_;
    std::jthread prompt_thread_;
    std::atomic_uint64_t prompt_copies_{};
    std::atomic_uint64_t prompt_failures_{};
    std::atomic_uint64_t prompt_skipped_no_room_{};
    std::atomic_uint64_t prompt_dropped_{};
    // Repair's silent failures, counted. Pull: a live object this node should
    // own that is not here, not cached, and no peer would supply (an
    // unavailable extent). Push: a listed local object that cannot be read
    // back (a failing disk). A single miss proves nothing (busy peer, failed
    // RPC, budget); a total climbing across passes, or ids recurring in the
    // sample, is the signal.
    std::atomic_uint64_t repair_pull_unsourceable_{};
    std::atomic_uint64_t repair_push_examined_total_{};
    std::atomic_uint64_t repair_pull_examined_total_{};
    std::atomic_uint64_t repair_bytes_total_{};
    std::atomic_uint64_t repair_passes_completed_{};
    std::atomic_bool repair_push_phase_complete_{};
    std::atomic_uint64_t repair_gate_ran_{};
    std::atomic_uint64_t repair_gate_share_{};
    std::atomic_uint64_t repair_gate_quiescent_{};
    std::atomic_uint64_t repair_gate_credit_{};
    std::atomic_uint64_t repair_last_credit_{};
    // Decision trace for repair: each copy pushed, pull and local drop, in
    // decision order (verifications are not decisions). Set once before
    // maintenance starts.
    std::function<void(std::string_view kind, std::string_view detail)> repair_trace_;
    void trace_repair(const std::string& detail) const {
        if (repair_trace_)
            repair_trace_("repair", detail);
    }

  public:
    enum class RepairGate { ran, share, quiescent, credit };
    void note_repair_gate(RepairGate gate, double credit) noexcept {
        repair_last_credit_.store(static_cast<uint64_t>(std::max(0.0, credit)),
                                  std::memory_order_relaxed);
        switch (gate) {
        case RepairGate::ran: repair_gate_ran_.fetch_add(1, std::memory_order_relaxed); break;
        case RepairGate::share: repair_gate_share_.fetch_add(1, std::memory_order_relaxed); break;
        case RepairGate::quiescent:
            repair_gate_quiescent_.fetch_add(1, std::memory_order_relaxed);
            break;
        case RepairGate::credit: repair_gate_credit_.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

  private:
    std::atomic_uint64_t repair_local_unreadable_{};
    mutable std::mutex repair_sample_mutex_;
    std::deque<ObjectId> repair_unsourceable_sample_;
    Clock::time_point repair_unsourceable_last_log_{};
    Clock::time_point repair_unreadable_last_log_{};
    void note_repair_unsourceable(const ObjectId&);
    void note_repair_local_unreadable(const ObjectId&);
    void queue_prompt_replication(const ObjectId&);
    void prompt_replication_loop(std::stop_token);
    mutable std::mutex fetch_mutex_;
    std::map<ObjectId, std::weak_ptr<SharedFetch>> fetches_;
    ReplicaSelector replica_selector_;

    bool put_impl(const ObjectId&, std::span<const uint8_t>, FrameType, std::atomic_bool*,
                  DurabilityBatch*, const DataWorkContext* work = nullptr);
    // Active nodes that host extents: the input to every placement decision.
    std::vector<NodeInfo> hosting_nodes() const;
    std::vector<NodeInfo> ranked(const ObjectId&) const;
    std::vector<NodeInfo> owners(const ObjectId&) const;
    bool put_on(const NodeInfo&, const ObjectId&, std::span<const uint8_t>, bool foreground);
    RpcReply bounded_control_call(const NodeInfo&, MessageType, std::span<const uint8_t>,
                                  FrameType = FrameType::control);
    bool retain_on(const NodeInfo&, RetentionClass, const std::vector<ObjectId>&,
                   const RetentionDot&);
    // For each object, walks its candidates in preference order collecting up
    // to `floor` present nodes, with presence checked in node-grouped
    // have_objects rounds rather than one RPC per (object, candidate).
    std::map<ObjectId, std::vector<NodeInfo>>
    select_present_batched(const std::map<ObjectId, std::vector<NodeInfo>>& candidates_by_object,
                           size_t floor);
    // Presence for every (node, ids) pair: local entries answered directly,
    // the rest by chunked have_objects RPCs, at most
    // retention_check_concurrency chunks in flight across all peers.
    std::map<NodeId, std::map<ObjectId, bool>>
    batched_have_objects(const std::map<NodeId, NodeInfo>& node_info,
                        const std::map<NodeId, std::vector<ObjectId>>& ids_by_node,
                        FrameType frame_type = FrameType::loader);
    // Repair's presence round: whether each peer holds a copy that reads back
    // intact, have_valid_objects_max ids per request, all requests concurrent.
    // Peers without have_valid_objects are asked per id via have_object. A
    // failed request answers false for its ids.
    std::map<NodeId, std::map<ObjectId, bool>>
    validated_presence(const std::map<NodeId, NodeInfo>& node_info,
                       const std::map<NodeId, std::vector<ObjectId>>& ids_by_node);
    ObjectData get_from(const NodeInfo&, const ObjectId&, FrameType,
                        const std::shared_ptr<SharedFetch>&,
                        Clock::time_point deadline, std::atomic_bool* cancelled,
                        const std::function<bool()>& abort = {});
    ObjectData get_remote(const ObjectId&, size_t stripe, FrameType, bool foreground,
                          bool opportunistic_persist, Clock::time_point deadline = {},
                          std::atomic_bool* cancelled = nullptr,
                          const std::function<bool()>& abort = {});
    void note_foreground(uint64_t);
    void note_network(uint64_t, Clock::duration);

  public:
    DistributedStore(NodeRuntime& n, ActivityClocks& activity, DataResourceArbiter& data_resources,
                     RetainedMemoryLedger& retained_memory, NodeEvents& events,
                     DistributedStoreOptions options = {});
    ~DistributedStore();
    struct PromptReplicationStats {
        uint64_t queued{};
        uint64_t copies{};
        uint64_t failures{};
    };
    PromptReplicationStats prompt_replication_stats() const;
    ObjectId put(std::span<const uint8_t>, std::atomic_bool* cancelled = nullptr);
    ObjectId put(std::span<const uint8_t>, FrameType, std::atomic_bool* cancelled = nullptr);
    bool put(const ObjectId&, std::span<const uint8_t>, std::atomic_bool* cancelled = nullptr);
    bool put(const ObjectId&, std::span<const uint8_t>, FrameType,
             std::atomic_bool* cancelled = nullptr);
    ObjectId put_deferred(std::span<const uint8_t>, DurabilityBatch&,
                          std::atomic_bool* cancelled = nullptr);
    // A `work` no-progress budget fails the put (retryably) once no replica
    // in the pipeline has moved for that long.
    ObjectId put_deferred(std::span<const uint8_t>, DurabilityBatch&, FrameType,
                          std::atomic_bool* cancelled = nullptr,
                          const DataWorkContext* work = nullptr);
    bool put_deferred(const ObjectId&, std::span<const uint8_t>, DurabilityBatch&,
                      std::atomic_bool* cancelled = nullptr);
    bool put_deferred(const ObjectId&, std::span<const uint8_t>, DurabilityBatch&, FrameType,
                      std::atomic_bool* cancelled = nullptr,
                      const DataWorkContext* work = nullptr);
    // True when every requirement in `batch` has reached its durability floor.
    // A replica whose placement token died with a peer's process or backend
    // incarnation is re-derived by probing and the batch re-stamped in place;
    // ids a peer no longer holds go to `unsatisfiable` (if given) for re-put.
    bool durability_barrier(DurabilityBatch&, FrameType = FrameType::loader,
                            std::vector<ObjectId>* unsatisfiable = nullptr);
    // Publication liveness barrier: every object a metadata mutation
    // references gets a causal retention claim before the commit is accepted.
    // DATA needs dht.min_write_replicas; CONTROL the given metadata floor.
    bool retain_data(const std::vector<ObjectId>&, const RetentionDot&);
    bool retain_control(const std::vector<ObjectId>&, const RetentionDot&, size_t required);
    std::optional<Bytes> get(const ObjectId&, size_t stripe = 0, bool foreground = true,
                             Clock::time_point deadline = {}, std::atomic_bool* cancelled = nullptr);
    std::optional<Bytes> get(const ObjectId&, size_t stripe, FrameType,
                             Clock::time_point deadline = {}, std::atomic_bool* cancelled = nullptr);
    ObjectData get_shared(const ObjectId&, size_t stripe, FrameType,
                          Clock::time_point deadline = {},
                          std::atomic_bool* cancelled = nullptr);
    bool has_on(const NodeInfo&, const ObjectId&);
    bool should_own(const ObjectId&) const;
    // Placement: whether this node is in the object's owner set.
    bool owns(const ObjectId& id) const override { return should_own(id); }
    size_t replicate_all(const ObjectId&, std::span<const uint8_t>, bool foreground = false);
    size_t replicate_control(const ObjectId&, std::span<const uint8_t>);
    bool ensure_local(const ObjectId&, bool foreground = false);
    bool ensure_control_local(const ObjectId&);
    bool locally_available(const ObjectId&) const;
    bool cache_local(const ObjectId&, std::span<const uint8_t>);
    bool hydration_available() const;
    bool hydrate(const ObjectId&, size_t stripe = 0,
                 FrameType frame_type = FrameType::speculative);
    void erase_all(const ObjectId&);
    void foreground_activity(uint64_t bytes) { note_foreground(bytes); }
    void interactive_activity(uint64_t bytes) { activity_.note(FrameType::read_ahead, bytes); }
    void loader_activity(uint64_t bytes) { activity_.note(FrameType::loader, bytes); }

    // Pushes local objects to their owners and pulls live objects this node
    // should own. repair_step() keeps push/pull cursors across calls and never
    // materialises full object lists. The byte budget covers transfers only
    // (repair_once: zero = unlimited); probes and lookups are bounded by
    // operation_budget, and a step stops at the first transfer the budget
    // cannot cover (credit_limited). should_yield is consulted between
    // operations, so a transfer in flight completes and is kept. With a
    // repair_position file the push pass resumes there after a restart
    // (saved at most every 30 s and at each pass end).
    uint64_t repair_once(uint64_t byte_budget = 0,
                         std::optional<std::span<const ObjectId>> live = std::nullopt);
    RepairResult repair_step(uint64_t byte_budget, size_t operation_budget,
                             std::optional<std::span<const ObjectId>> live = std::nullopt,
                             const std::function<bool()>& should_yield = {},
                             uint64_t live_generation = 0);
    uint64_t scrub_once(uint64_t byte_budget = 0);
    RepairDiagnostics repair_diagnostics() const;

    uint64_t take_foreground_bytes() { return activity_.take_bytes(FrameType::foreground); }
    uint64_t take_interactive_bytes() { return activity_.take_bytes(FrameType::read_ahead); }
    uint64_t take_loader_bytes() { return activity_.take_bytes(FrameType::loader); }
    std::chrono::milliseconds foreground_idle_for() const;
    std::chrono::milliseconds interactive_idle_for() const {
        return activity_.idle_for(FrameType::read_ahead);
    }
    // Durable user-requested work (FUSE publication, ingest, acquisition);
    // law 3 ranks it above background maintenance, which must see it.
    std::chrono::milliseconds loader_idle_for() const {
        return activity_.idle_for(FrameType::loader);
    }
    double estimated_network_bps() const {
        return network_bps_.load();
    }
};
} // namespace macha

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cluster/cluster.hpp"
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
#include <set>
#include <vector>

namespace macha {
class DistributedStore {
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

    // Bounded, snapshot-shaped: a cumulative count plus a small sample of the
    // object ids involved, so Status can answer "is anything unreachable?"
    // without an operator grepping journals on every node.
    struct RepairDiagnostics {
        uint64_t pull_unsourceable{};
        uint64_t local_unreadable{};
        std::vector<ObjectId> unsourceable_sample;
        // Progress, cumulative since start (0.62.2). Until then the only view
        // of whether repair was moving was a trace-level log line.
        uint64_t push_examined{};
        uint64_t pull_examined{};
        uint64_t bytes_transferred{};
        uint64_t passes_completed{};
        bool push_phase_complete{};
        // Why the maintenance loop did or did not run a repair step, per
        // pass. With progress stuck at zero there was no way to tell a gate
        // from a loop that never got here. gate_credit counts steps that ran
        // and stopped at a transfer the credit could not yet cover.
        uint64_t gate_ran{};
        uint64_t gate_share{};
        uint64_t gate_quiescent{};
        uint64_t gate_credit{};
        uint64_t last_credit_bytes{};
        // Prompt replication (0.64.1), cumulative since start: the loop that
        // copies each new object to a second owner at once. Until 0.64.1 it
        // was counted nowhere, and it pushed every new object into fi-1's
        // full backend and retried the refusal every 30 s for ever.
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
    StoragePool::Cursor repair_push_cursor_;
    // Objects taken from the push cursor and not yet settled, in cursor order,
    // with what one batched presence round found for each on its candidate
    // peers. A step that stops (credit, a viewer, a failed operation) leaves
    // both in place and the next step resumes at the same object.
    std::deque<ObjectId> repair_push_window_;
    std::map<std::pair<NodeId, ObjectId>, bool> repair_push_presence_;
    std::set<ObjectId> repair_push_probed_;
    bool repair_push_cursor_exhausted_{};
    // At most this many repair pushes in flight at once (0.73.1).
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
    const std::vector<ObjectId>* repair_live_identity_{};
    uint64_t repair_live_generation_{};
    bool repair_push_complete_{};
    bool repair_pull_complete_{};
    std::atomic<double> network_bps_{};
    // Prompt second copy. A put stops at min_write_replicas; until 0.32.13 the
    // extra copies waited for the repair cursor to come round, which on a
    // busy import was hours, and a writer's death in that window stranded
    // its recent data (129 files, 2026-09-07). Objects that reached only
    // the floor are queued here and pushed to the next placement owner by
    // one worker, admitted as speculative DATA work behind viewers.
    std::mutex prompt_mutex_;
    std::condition_variable_any prompt_cv_;
    std::deque<ObjectId> prompt_queue_;
    std::set<ObjectId> prompt_queued_;
    std::jthread prompt_thread_;
    std::atomic_uint64_t prompt_copies_{};
    std::atomic_uint64_t prompt_failures_{};
    std::atomic_uint64_t prompt_skipped_no_room_{};
    std::atomic_uint64_t prompt_dropped_{};
    // Repair's two silent failures, made countable. The pull side is the one
    // that matters after a node leaves: an object the live namespace still
    // references, that this node should own, that is not here, not in the
    // block cache, and that no peer would supply. That is an unavailable
    // extent, and until 0.40.0 repair passed over it without a word -- there
    // is no Log:: call anywhere in repair_step(). The push side counts a local
    // object the cursor listed but the store could not read back, which is
    // what a failing disk looks like from here.
    // Neither is proof on its own: a pull can miss because a peer was busy,
    // the RPC failed, or a budget ran out, and the next pass will try again.
    // A total that keeps climbing across passes, or the same ids reappearing
    // in the sample, is the signal.
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
    // The decision trace's view of repair (the object ledger plan, T1): each
    // copy pushed, each pull and each local copy dropped, in the order repair
    // decides them; objects it only verifies are not decisions. Set once
    // before maintenance starts.
    std::function<void(std::string_view kind, std::string_view detail)> repair_trace_;
    void trace_repair(const std::string& detail) const {
        if (repair_trace_)
            repair_trace_("repair", detail);
    }

  public:
    void set_repair_trace(std::function<void(std::string_view, std::string_view)> trace) {
        repair_trace_ = std::move(trace);
    }
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
    // Batched, bounded-concurrency replacement for a per-(object, candidate)
    // serial have_object scan. For every object, walks its candidate list in
    // preference order accumulating up to `floor` present nodes, but checks
    // presence in node-grouped have_objects round trips (many ids per peer per
    // round) instead of one RPC/decrypt per (object, candidate) pair. Selection
    // order and the floor requirement are unchanged from the serial form; only
    // the shape of how presence gets checked changes.
    std::map<ObjectId, std::vector<NodeInfo>>
    select_present_batched(const std::map<ObjectId, std::vector<NodeInfo>>& candidates_by_object,
                           size_t floor);
    // One round of batched_have_objects: checks presence of every (node, ids)
    // pair in ids_by_node, answering the local node's entries directly (cheap
    // presence check, no RPC) and the rest via chunked have_objects RPCs, at
    // most retention_check_concurrency chunks in flight at once across every
    // peer combined.
    std::map<NodeId, std::map<ObjectId, bool>>
    batched_have_objects(const std::map<NodeId, NodeInfo>& node_info,
                        const std::map<NodeId, std::vector<ObjectId>>& ids_by_node,
                        FrameType frame_type = FrameType::loader);
    // Repair's presence round: whether each peer holds a copy that reads back
    // intact, have_valid_objects_max ids a request, all requests in flight at
    // once. A peer that does not know have_valid_objects is asked one
    // have_object at a time. A failed request answers false for its ids, as a
    // failed have_object always has.
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
    explicit DistributedStore(NodeRuntime& n);
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
    // `work`, when it carries a no-progress budget, bounds a put whose remote
    // replicas have all gone silent: the put fails (retryably) once nothing in
    // the pipeline has moved for the budget, instead of waiting forever.
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
    // incarnation is re-derived by probing the peer with the object ids and
    // the batch is re-stamped in place, so the next call is ordinary; ids a
    // peer no longer holds are appended to `unsatisfiable` (if given) and the
    // caller must re-put them.
    bool durability_barrier(DurabilityBatch&, FrameType = FrameType::loader,
                            std::vector<ObjectId>* unsatisfiable = nullptr);
    // Publication liveness barrier. Every referenced object touched by a metadata
    // mutation acquires a causal retention claim before that metadata commit may
    // be accepted. DATA uses dht.min_write_replicas; CONTROL uses the supplied
    // metadata write floor.
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
    void interactive_activity(uint64_t bytes) { n_.note_activity(FrameType::read_ahead, bytes); }
    void loader_activity(uint64_t bytes) { n_.note_activity(FrameType::loader, bytes); }

    // Converges remote placement and proactively pulls live objects for which
    // this node has become an owner. Bounded repair_step() calls retain push/pull
    // cursors across scheduler slices; they never rebuild complete object vectors.
    // The byte limit is network transfer, not block count (repair_once: zero
    // means unlimited). It gates transfers only: presence probes and index
    // lookups run without it, bounded by operation_budget, and a step stops at
    // the first transfer the budget cannot cover (RepairResult::credit_limited).
    // should_yield is consulted between operations only: an extent already in
    // flight completes and is kept, so a pacer can shorten repair's turns
    // without ever making them fruitless.
    // Keep where the push pass has reached in `path`, saved at most every
    // 30 s and at the end of each pass, and resume from it: a restart then
    // does not re-check (a full read each on the peer) everything it had
    // already copied. The node's own repair store enables this; a store
    // without it starts each pass at the beginning.
    void persist_repair_position(std::filesystem::path path);
    uint64_t repair_once(uint64_t byte_budget = 0, const std::vector<ObjectId>* live = nullptr,
                         const std::vector<ObjectId>* universal = nullptr);
    RepairResult repair_step(uint64_t byte_budget, size_t operation_budget,
                             const std::vector<ObjectId>* live = nullptr,
                             const std::vector<ObjectId>* universal = nullptr,
                             const std::function<bool()>& should_yield = {},
                             uint64_t live_generation = 0);
    uint64_t scrub_once(uint64_t byte_budget = 0);
    RepairDiagnostics repair_diagnostics() const;

    uint64_t take_foreground_bytes() { return n_.take_activity_bytes(FrameType::foreground); }
    uint64_t take_interactive_bytes() { return n_.take_activity_bytes(FrameType::read_ahead); }
    uint64_t take_loader_bytes() { return n_.take_activity_bytes(FrameType::loader); }
    std::chrono::milliseconds foreground_idle_for() const;
    std::chrono::milliseconds interactive_idle_for() const {
        return n_.activity_idle_for(FrameType::read_ahead);
    }
    // Durable work the user asked for -- FUSE publication, ingest, acquisition
    // -- which must finish but need not finish first. Law 3 puts it above
    // background maintenance, so maintenance has to be able to see it.
    std::chrono::milliseconds loader_idle_for() const {
        return n_.activity_idle_for(FrameType::loader);
    }
    double estimated_network_bps() const {
        return network_bps_.load();
    }
    RetainedMemoryLedger& retained_memory() noexcept { return n_.retained_memory(); }
};
} // namespace macha

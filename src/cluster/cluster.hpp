// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "cluster/activity_clocks.hpp"
#include "cluster/data_work.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/node_events.hpp"
#include "cluster/node_identity.hpp"
#include "storage/local_store.hpp"
#include "cluster/membership.hpp"
#include "metadata/metadata.hpp"
#include "cluster/net.hpp"
#include "storage/persistent_cache.hpp"
#include "cluster/public_connectivity.hpp"
#include "storage/retention.hpp"
#include "retained_memory.hpp"
#include "auth/session.hpp"
#include "auth/users.hpp"
#include "storage/storage_pool.hpp"
#include "cluster/telemetry.hpp"
#include "cluster/transcode_rates.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string_view>

namespace macha {
// How this node answers "can peers connect to me?" and "do I host extents?",
// with the configured modes beside the resolved values. `source` decided the
// inbound answer: "configured", "persisted", "default" (auto, no evidence yet,
// behaving as capable) or "probe:<peer>".
struct InboundResolution {
    Tristate inbound_capable_mode{Tristate::automatic};
    Tristate hosts_extents_mode{Tristate::automatic};
    bool inbound_capable{true};
    bool hosts_extents{true};
    std::string source{"default"};
    uint64_t decided_unix_ms{};
    uint64_t last_probe_unix_ms{};
    std::string last_probe_peer;
    std::string last_probe_error;
    unsigned consecutive_probe_failures{};
};

struct NodeReadiness {
    bool control_plane_online{};
    bool data_storage_ready{};
    bool control_storage_ready{};
    bool cache_ready{};
    bool retention_ready{};
    bool metadata_ready{};
    bool local_state_ready{};
    bool failed{};
    uint64_t started_unix_ms{};
    uint64_t ready_unix_ms{};
    std::string error;
};

class NodeRuntime {
  public:
    using StartupStageHook = std::function<void(std::string_view)>;

  private:
    enum ReadyBit : uint32_t {
        ready_control_plane = 1U << 0,
        ready_data_storage = 1U << 1,
        ready_control_storage = 1U << 2,
        ready_cache = 1U << 3,
        ready_retention = 1U << 4,
        ready_metadata = 1U << 5,
        ready_failed = 1U << 31,
    };

    Config cfg_;
    // Owned by the root, which built it under the state path's lock.
    const NodeIdentity& identity_;
    // Owned by the root (NodeResources), which stops them before this node.
    ActivityClocks& activity_;
    DataResourceArbiter& data_resources_;
    RetainedMemoryLedger& retained_memory_;
    // Playback records, telemetry publishes.
    TranscodeRateBook& transcode_rates_;
    // Inbound requests are dispatched here; this node binds the types it
    // answers and records them for unbinding.
    MessageRoutes& routes_;
    std::vector<MessageType> bound_routes_;
    // Storage, metadata and topology changes are counted here for whoever
    // watches; the node never calls a consumer.
    NodeEvents& events_;
    // Declared before members_ so the roster is built with the right flags.
    mutable std::mutex inbound_mutex_;
    InboundResolution inbound_;

    // The control plane is constructed before storage and metadata, so the
    // node is reachable while its durable state recovers.
    Membership members_;
    PublicConnectivity public_connectivity_;
    TelemetryStore telemetry_;
    SessionManager sessions_;
    UserStore users_;
    // The last gossiped table and which peers have been told it. A set of ids,
    // not a count, so membership churn does not re-broadcast.
    Hash256 gossiped_user_table_{};
    std::set<NodeId> gossiped_user_peers_;
    Clock::time_point gossip_users_retry_after_{};
    Clock::time_point gossiped_users_at_{};
    Hash256 gossiped_sessions_{};
    std::set<NodeId> gossiped_session_peers_;
    Clock::time_point gossip_sessions_retry_after_{};
    Clock::time_point gossiped_sessions_at_{};
    RpcClient client_;
    RpcServer server_;

    std::unique_ptr<StoragePool> local_;
    std::unique_ptr<LocalStore> control_;
    std::unique_ptr<PersistentBlockCache> cache_;
    std::unique_ptr<RetentionStore> retention_;
    std::unique_ptr<MetadataReplica> meta_;
    StartupStageHook startup_stage_hook_;
    std::atomic_uint32_t ready_bits_{};
    uint64_t startup_unix_ms_{};
    std::atomic_uint64_t ready_unix_ms_{};
    mutable std::mutex readiness_mutex_;
    std::condition_variable readiness_cv_;
    std::string recovery_error_;
    std::jthread storage_recovery_;
    std::jthread state_recovery_;
    std::jthread connectivity_worker_;
    std::mutex connectivity_wait_mutex_;
    std::condition_variable_any connectivity_wait_cv_;
    std::atomic_uint64_t connectivity_wake_{};
    // One dial-back probe in flight per requesting peer, and one per 10 s: a
    // peer cannot use the probe to make this node hammer an address.
    std::mutex dial_back_mutex_;
    std::map<NodeId, Clock::time_point> dial_back_last_;

    std::atomic_uint64_t remote_metadata_generation_{};
    std::atomic_uint64_t remote_metadata_epoch_{};
    std::atomic_uint64_t metadata_announcements_{};
    std::atomic_uint64_t telemetry_storage_used_{};
    std::atomic_uint64_t telemetry_storage_capacity_{};
    std::atomic_uint64_t telemetry_metadata_generation_{};
    std::atomic_uint32_t telemetry_peers_known_{1};
    std::atomic_uint32_t telemetry_peers_active_{1};
    std::atomic_uint64_t telemetry_demand_{1};
    std::jthread maintenance_;
    std::jthread telemetry_worker_;
    std::mutex telemetry_wait_mutex_;
    std::condition_variable_any telemetry_wait_cv_;
    std::mutex maintenance_wait_mutex_;
    std::condition_variable_any maintenance_wait_cv_;
    std::atomic_bool started_{};
    std::atomic_bool outbound_calls_stopped_{};


    bool ready(ReadyBit bit) const noexcept {
        return (ready_bits_.load(std::memory_order_acquire) & static_cast<uint32_t>(bit)) != 0;
    }
    void mark_ready(ReadyBit bit);
    void mark_recovery_failed(std::string);
    void recover_storage(std::stop_token);
    void recover_state(std::stop_token);
    bool all_local_state_ready() const noexcept;

    void bind_control_routes();
    void bind_storage_routes();
    void bind_metadata_routes();
    void route(MessageType, MessageRoutes::Handler);
    void unbind_routes();
    void loop(std::stop_token);
    // Public-endpoint discovery, then (for `inbound_capable: auto`) the
    // dial-back resolution state machine, for the node's life.
    void connectivity_loop(std::stop_token);
    bool resolve_hosts_extents_for(bool inbound_capable) const;
    void apply_inbound_resolution(bool inbound_capable, std::string source);
    void persist_inbound_resolution_locked() const;
    void refuse_impossible_cluster() const;
    void exchange(const Endpoint&);
    void exchange(const NodeInfo&);
    void merge(std::span<const uint8_t>);
    void refresh_telemetry();
    void signal_telemetry_refresh();
    void telemetry_loop(std::stop_token);
    std::chrono::milliseconds stall_notice_for(MessageType) const;
    std::chrono::milliseconds no_progress_deadline_for(MessageType) const;

  public:
    // The caller holds state_path's StorageLock for this node's life.
    NodeRuntime(Config, const NodeIdentity&, ActivityClocks&, DataResourceArbiter&, RetainedMemoryLedger&,
                TranscodeRateBook&, MessageRoutes&, NodeEvents&, StartupStageHook startup_stage_hook = {});
    ~NodeRuntime();
    void start();
    void request_stop();
    // Closes client routes and fails every pending synchronous call now, so
    // Service-owned workers can leave an RPC wait before Service joins them.
    void cancel_outbound_calls();
    void stop();
    bool wait_local_state_ready(std::chrono::milliseconds timeout);
    NodeReadiness readiness() const;
    const Config& config() const {
        return cfg_;
    }
    const ClusterKeys& keys() const {
        return identity_.keys;
    }
    NodeId node_id() const {
        return identity_.id;
    }
    NodeId durability_epoch() const {
        return identity_.durability_epoch;
    }
    StoragePool& local_store();
    const StoragePool& local_store() const;
    LocalStore& control_store();
    const LocalStore& control_store() const;
    PersistentBlockCache& block_cache();
    // The object ledger's claimed half. Throws while retention is recovering.
    ClaimStore& claims();
    const ClaimStore& claims() const;
    MetadataReplica& metadata_replica();
    const MetadataReplica& metadata_replica() const;
    Membership& membership() {
        return members_;
    }
    const Membership& membership() const {
        return members_;
    }
    TelemetryStore& telemetry() {
        return telemetry_;
    }
    const TelemetryStore& telemetry() const {
        return telemetry_;
    }
    SessionManager& sessions() {
        return sessions_;
    }
    const SessionManager& sessions() const {
        return sessions_;
    }
    UserStore& users() {
        return users_;
    }
    const UserStore& users() const {
        return users_;
    }
    bool apply_session(const AuthSession&);
    // Notify-only: merge locally, queue on already-usable control-lane
    // connections, and return. No HTTP request path waits on a peer; an
    // unreachable peer converges on the next gossip tick.
    void propagate_session(const AuthSession&);
    bool apply_user(const UserRecord&);
    void propagate_users();
    // Periodic repair, not a heartbeat: sends only when the table changed or
    // a peer appeared that may not have seen it.
    void gossip_users_if_changed();
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t> payload = {});
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t> payload = {});
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType);
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType);
    // Test-only pass-throughs to RpcClient's silent-peer fixture.
    void stall_peer_for_tests(const NodeId& peer, std::optional<MessageType> message = {}) {
        client_.stall_peer_for_tests(peer, message);
    }
    void release_peer_for_tests(const NodeId& peer) { client_.release_peer_for_tests(peer); }
    size_t stalled_calls_for_tests() const { return client_.stalled_calls_for_tests(); }
    bool store_metadata_commit(const MetadataHistoryEntry&);
    bool accept_metadata_commit(const MetadataAcceptance&);
    std::vector<MetadataAcceptance> metadata_heads() const;
    // RPC side of MetadataManager::attempt_history_checkpoint(); dispatches
    // straight to the replica, as NodeRuntime has no MetadataManager.
    bool accept_history_checkpoint_proposal(const HistoryCheckpointProof&);
    bool commit_history_checkpoint(const Hash256& floor_hash, const Hash256& epoch);
    void announce_metadata_generation(uint64_t);
    void reconfigure_local(const Config&);
    // Cluster bytes by frame class since start (dialled and served together).
    TrafficTotals traffic_totals() const;
    // Whether another node reports viewer-class traffic in a sample no older
    // than `fresh_for`. Repair paces against it as against local viewers,
    // since its transfers share their links.
    bool peer_viewers_active(std::chrono::milliseconds fresh_for) const;
    uint64_t remote_metadata_generation() const {
        return remote_metadata_generation_.load();
    }
    uint64_t remote_metadata_epoch() const {
        return remote_metadata_epoch_.load(std::memory_order_acquire);
    }
    uint64_t metadata_announcements() const {
        return metadata_announcements_.load(std::memory_order_acquire);
    }
    uint64_t known_metadata_generation() const {
        const auto local = ready(ready_metadata) ? metadata_replica().generation() : 0;
        const auto remote = remote_metadata_generation_.load();
        return local > remote ? local : remote;
    }
    bool apply_identity_reset(const IdentityAssociationReset&);
    void propagate_identity_reset(const IdentityAssociationReset&);
    std::vector<IdentityAssociationReset> identity_resets() const {
        return members_.identity_resets();
    }
    PublicConnectivityStatus public_connectivity_status() const;
    PublicConnectivityStatus refresh_public_connectivity(bool probe, bool force_probe = false);
    InboundResolution inbound_resolution() const;
    bool inbound_capable() const {
        return inbound_resolution().inbound_capable;
    }
    bool hosts_extents() const {
        return inbound_resolution().hosts_extents;
    }
    // Test-only: run a dial-back probe round now.
    void probe_inbound_now_for_tests() {
        connectivity_wake_.fetch_add(1, std::memory_order_acq_rel);
        connectivity_wait_cv_.notify_all();
    }
    RpcStats rpc_stats() const {
        return client_.stats();
    }
    std::optional<std::chrono::milliseconds> peer_latency(const NodeId& peer) const {
        return client_.peer_latency(peer);
    }
    std::map<NodeId, std::chrono::milliseconds> peer_latencies() const {
        return client_.peer_latencies();
    }
    RpcServerWorkStats rpc_server_work_stats() const {
        return server_.work_stats();
    }
};
} // namespace macha

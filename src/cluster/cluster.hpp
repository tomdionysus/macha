// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "contract/thread_safety.hpp"
#include "cluster/activity_clocks.hpp"
#include "cluster/data_work.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/node_events.hpp"
#include "cluster/local_state.hpp"
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
    Config cfg_;
    // Owned by the root, which built it under the state path's lock.
    const NodeIdentity& identity_;
    // Owned by the root; the node reads it for readiness and telemetry.
    RecoveryProgress& progress_;
    // Owned by the root (NodeResources), which stops them before this node.
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
    // Held across the resolution file's durable replace and its failure log.
    mutable IoMutex inbound_mutex_;
    InboundResolution inbound_ MACHA_GUARDED_BY(inbound_mutex_);

    // The control plane is constructed before storage and metadata, so the
    // node is reachable while its durable state recovers.
    Membership members_;
    PublicConnectivity public_connectivity_;
    TelemetryStore telemetry_;
    RpcClient client_;
    RpcServer server_;

    StartupStageHook startup_stage_hook_;
    std::atomic_bool control_plane_online_{};
    uint64_t startup_unix_ms_{};
    std::jthread connectivity_worker_;
    // Guards no state: the connectivity worker's wait lock.
    Mutex connectivity_wait_mutex_;
    std::condition_variable_any connectivity_wait_cv_;
    std::atomic_uint64_t connectivity_wake_{};
    // One dial-back probe in flight per requesting peer, and one per 10 s: a
    // peer cannot use the probe to make this node hammer an address.
    Mutex dial_back_mutex_;
    std::map<NodeId, Clock::time_point> dial_back_last_ MACHA_GUARDED_BY(dial_back_mutex_);

    std::atomic_uint64_t remote_metadata_generation_{};
    std::atomic_uint64_t remote_metadata_epoch_{};
    std::atomic_uint64_t metadata_announcements_{};
    // The latest local-state figures their owners advertised; zero until
    // the first push, so a recovering node reports zero rather than vanishing.
    std::atomic_uint64_t advertised_storage_used_{};
    std::atomic_uint64_t advertised_storage_capacity_{};
    std::atomic_uint32_t advertised_backends_online_{};
    std::atomic_uint64_t advertised_metadata_generation_{};
    std::atomic_uint64_t advertised_cache_capacity_{};
    std::atomic_uint64_t advertised_cache_used_{};
    std::atomic_uint64_t advertised_cache_hits_{};
    std::atomic_uint64_t advertised_cache_misses_{};
    std::atomic_uint64_t advertised_cache_evictions_{};
    std::atomic_uint32_t telemetry_peers_known_{1};
    std::atomic_uint32_t telemetry_peers_active_{1};
    std::atomic_uint64_t telemetry_demand_{1};
    std::jthread maintenance_;
    std::jthread telemetry_worker_;
    // Guards no state: the telemetry worker's wait lock.
    Mutex telemetry_wait_mutex_;
    std::condition_variable_any telemetry_wait_cv_;
    // Guards no state: the maintenance thread's wait lock.
    Mutex maintenance_wait_mutex_;
    std::condition_variable_any maintenance_wait_cv_;
    std::atomic_bool started_{};
    std::atomic_bool outbound_calls_stopped_{};


    bool all_local_state_ready() const noexcept;

    void bind_control_routes();
    void route(MessageType, MessageRoutes::Handler);
    void unbind_routes();
    void loop(std::stop_token);
    // Public-endpoint discovery, then (for `inbound_capable: auto`) the
    // dial-back resolution state machine, for the node's life.
    void connectivity_loop(std::stop_token);
    bool resolve_hosts_extents_for(bool inbound_capable) const;
    void apply_inbound_resolution(bool inbound_capable, std::string source);
    void persist_inbound_resolution_locked() const MACHA_REQUIRES(inbound_mutex_);
    void refuse_impossible_cluster() const;
    void exchange(const Endpoint&);
    void exchange(const NodeInfo&);
    void merge(std::span<const uint8_t>);
    void refresh_telemetry();
    void telemetry_loop(std::stop_token);
    std::chrono::milliseconds stall_notice_for(MessageType) const;
    std::chrono::milliseconds no_progress_deadline_for(MessageType) const;

  public:
    // The caller holds state_path's StorageLock for this node's life.
    NodeRuntime(Config, const NodeIdentity&, RecoveryProgress&, RetainedMemoryLedger&,
                TranscodeRateBook&, MessageRoutes&, NodeEvents&, StartupStageHook startup_stage_hook = {});
    ~NodeRuntime();
    void start();
    void request_stop();
    // Closes client routes and fails every pending synchronous call now, so
    // Service-owned workers can leave an RPC wait before Service joins them.
    void cancel_outbound_calls();
    void stop();
    // Local-state facts the control plane publishes. The owners of the stores
    // push them; membership and telemetry carry the latest. Lock-free, any
    // thread.
    void advertise_storage(uint64_t used, uint64_t capacity);
    void advertise_storage_backends(uint32_t online);
    void advertise_metadata_generation(uint64_t generation);
    void advertise_cache(uint64_t capacity, uint64_t used, const CacheActivity& activity);
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
    // Republishes this node's NodeInfo (storage figures, metadata
    // generation) to peers that connect; owners call it after advertising.
    void publish_self();
    // Republishes telemetry now rather than at the next sample, e.g. when
    // the node's phase changes.
    void signal_telemetry_refresh();
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
    // A no-dial notification on established routes; never blocks. Returns
    // how many peers it was queued for.
    size_t broadcast_best_effort(const RpcMessage& message, FrameType frame_type) {
        return client_.broadcast_best_effort(message, frame_type);
    }
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
    // Tells peers this node's accepted metadata changed: advances the
    // announcement epoch, advertises the generation and broadcasts a notice.
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

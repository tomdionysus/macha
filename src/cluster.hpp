// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "data_work.hpp"
#include "local_store.hpp"
#include "membership.hpp"
#include "metadata.hpp"
#include "net.hpp"
#include "persistent_cache.hpp"
#include "public_connectivity.hpp"
#include "retention.hpp"
#include "storage_pool.hpp"
#include "telemetry.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string_view>

namespace macha {
enum class ServiceEvent : uint8_t {
    storage,
    metadata,
    topology,
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
    struct LocalCopyJob {
        ObjectId id;
        Bytes data;
        bool promote{};
        bool cache{};
    };

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
    ClusterKeys keys_;
    StorageLock state_lock_;
    NodeId id_;
    NodeId durability_epoch_;
    DataResourceArbiter data_resources_;

    // The control plane is intentionally constructed before any storage or
    // metadata backend. A node is therefore reachable/authenticated while its
    // local durable state is still recovering.
    Membership members_;
    PublicConnectivity public_connectivity_;
    TelemetryStore telemetry_;
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
    std::jthread local_writer_;
    std::mutex local_copy_mutex_;
    std::condition_variable local_copy_cv_;
    std::deque<LocalCopyJob> local_copies_;
    size_t local_copy_bytes_{};
    std::atomic_bool started_{};
    std::atomic_bool outbound_calls_stopped_{};
    std::atomic_uint64_t playback_activity_bytes_{};
    std::atomic_uint64_t interactive_activity_bytes_{};
    std::atomic_int64_t last_playback_activity_ms_{};
    std::atomic_int64_t last_interactive_activity_ms_{};
    mutable std::mutex service_event_mutex_;
    std::function<void(ServiceEvent)> service_event_;

    void signal_service_event(ServiceEvent);

    bool ready(ReadyBit bit) const noexcept {
        return (ready_bits_.load(std::memory_order_acquire) & static_cast<uint32_t>(bit)) != 0;
    }
    void mark_ready(ReadyBit bit);
    void mark_recovery_failed(std::string);
    void recover_storage(std::stop_token);
    void recover_state(std::stop_token);
    bool all_local_state_ready() const noexcept;

    RpcMessage handle(const NodeInfo&, FrameType, const RpcMessage&);
    void loop(std::stop_token);
    void local_writer_loop(std::stop_token);
    void exchange(const Endpoint&);
    void exchange(const NodeInfo&);
    void merge(std::span<const uint8_t>);
    void refresh_telemetry();
    void signal_telemetry_refresh();
    void telemetry_loop(std::stop_token);
    std::chrono::milliseconds stall_notice_for(MessageType) const;

  public:
    NodeRuntime(Config, ClusterKeys, StartupStageHook startup_stage_hook = {});
    ~NodeRuntime();
    void start();
    void request_stop();
    // Close outbound/inbound client routes and fail every pending synchronous
    // call without waiting for the rest of NodeRuntime teardown. Service-owned
    // workers must be able to leave an RPC wait before Service joins them.
    void cancel_outbound_calls();
    void stop();
    void set_service_event_callback(std::function<void(ServiceEvent)> callback);
    void notify_storage_mutation();
    bool wait_local_state_ready(std::chrono::milliseconds timeout);
    NodeReadiness readiness() const;
    const Config& config() const {
        return cfg_;
    }
    const ClusterKeys& keys() const {
        return keys_;
    }
    NodeId node_id() const {
        return id_;
    }
    NodeId durability_epoch() const {
        return durability_epoch_;
    }
    DataResourceArbiter& data_resources() noexcept { return data_resources_; }
    const DataResourceArbiter& data_resources() const noexcept { return data_resources_; }
    StoragePool& local_store();
    const StoragePool& local_store() const;
    LocalStore& control_store();
    const LocalStore& control_store() const;
    PersistentBlockCache& block_cache();
    RetentionStore& retention_store();
    const RetentionStore& retention_store() const;
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
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t> payload = {});
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t> payload = {});
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType);
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType);
    bool store_metadata_commit(const MetadataHistoryEntry&);
    bool accept_metadata_commit(const MetadataAcceptance&);
    std::vector<MetadataAcceptance> metadata_heads() const;
    void announce_metadata_generation(uint64_t);
    void enqueue_fetched(const ObjectId&, std::span<const uint8_t>, bool promote);
    void reconfigure_local(const Config&);
    void note_activity(FrameType, uint64_t bytes = 0);
    uint64_t take_activity_bytes(FrameType);
    std::chrono::milliseconds activity_idle_for(FrameType) const;
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
    RpcStats rpc_stats() const {
        return client_.stats();
    }
    RpcServerWorkStats rpc_server_work_stats() const {
        return server_.work_stats();
    }
};
} // namespace macha

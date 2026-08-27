// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "local_store.hpp"
#include "membership.hpp"
#include "metadata.hpp"
#include "net.hpp"
#include "persistent_cache.hpp"
#include "storage_pool.hpp"
#include "telemetry.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>

namespace macha {
class NodeRuntime {
    struct LocalCopyJob {
        ObjectId id;
        Bytes data;
        bool promote{};
        bool cache{};
    };

    Config cfg_;
    ClusterKeys keys_;
    StorageLock state_lock_;
    NodeId id_;
    NodeId durability_epoch_;
    StoragePool local_;
    LocalStore control_;
    PersistentBlockCache cache_;
    MetadataReplica meta_;
    Membership members_;
    TelemetryStore telemetry_;
    // Immutable network identity copied once during construction.  The
    // telemetry worker must not contend on Membership's mutex merely to report
    // our own host/failure-domain/port every few seconds.
    NodeInfo telemetry_identity_;
    std::atomic_uint64_t remote_metadata_generation_{};
    std::atomic_uint64_t telemetry_storage_used_{};
    std::atomic_uint64_t telemetry_storage_capacity_{};
    std::atomic_uint64_t telemetry_metadata_generation_{};
    std::atomic_uint32_t telemetry_peers_known_{1};
    std::atomic_uint32_t telemetry_peers_active_{1};
    RpcClient client_;
    RpcServer server_;
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
    std::atomic_uint64_t playback_activity_bytes_{};
    std::atomic_uint64_t interactive_activity_bytes_{};
    std::atomic_int64_t last_playback_activity_ms_{};
    std::atomic_int64_t last_interactive_activity_ms_{};

    RpcMessage handle(const NodeInfo&, FrameType, const RpcMessage&);
    void loop(std::stop_token);
    void local_writer_loop(std::stop_token);
    void exchange(const Endpoint&);
    void exchange(const NodeInfo&);
    void merge(std::span<const uint8_t>);
    void refresh_telemetry();
    void telemetry_loop(std::stop_token);
    std::chrono::milliseconds stall_notice_for(MessageType) const;

  public:
    NodeRuntime(Config, ClusterKeys);
    ~NodeRuntime();
    void start();
    void request_stop();
    void stop();
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
    StoragePool& local_store() {
        return local_;
    }
    const StoragePool& local_store() const {
        return local_;
    }
    LocalStore& control_store() {
        return control_;
    }
    const LocalStore& control_store() const {
        return control_;
    }
    PersistentBlockCache& block_cache() {
        return cache_;
    }
    MetadataReplica& metadata_replica() {
        return meta_;
    }
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
    bool seed_metadata(const MetadataRecord&);
    bool checkpoint_metadata(const MetadataRecord&);
    bool checkpoint_metadata_delta(const MetadataRecord& base, std::span<const uint8_t>,
                                   const MetadataRecord& committed);
    bool commit_metadata(uint64_t, const Hash256&);
    bool cas_metadata(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool cas_metadata_delta(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    void announce_metadata_generation(uint64_t);
    void enqueue_fetched(const ObjectId&, std::span<const uint8_t>, bool promote);
    void reconfigure_local(const Config&);
    void note_activity(FrameType, uint64_t bytes = 0);
    uint64_t take_activity_bytes(FrameType);
    std::chrono::milliseconds activity_idle_for(FrameType) const;
    uint64_t remote_metadata_generation() const {
        return remote_metadata_generation_.load();
    }
    uint64_t known_metadata_generation() const {
        const auto local = meta_.generation();
        const auto remote = remote_metadata_generation_.load();
        return local > remote ? local : remote;
    }
    bool apply_identity_reset(const IdentityAssociationReset&);
    void propagate_identity_reset(const IdentityAssociationReset&);
    std::vector<IdentityAssociationReset> identity_resets() const { return members_.identity_resets(); }
    RpcStats rpc_stats() const {
        return client_.stats();
    }
};
} // namespace macha

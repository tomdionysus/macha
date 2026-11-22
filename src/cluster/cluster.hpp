// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "auth/session.hpp"
#include "auth/users.hpp"
#include "cluster/cluster_node.hpp"
#include "cluster/dannan_host.hpp"
#include "cluster/data_work.hpp"
#include "cluster/inbound_resolution.hpp"
#include "cluster/local_state.hpp"
#include "cluster/membership.hpp"
#include "cluster/message_routes.hpp"
#include "cluster/net.hpp"
#include "cluster/node_events.hpp"
#include "cluster/node_identity.hpp"
#include "cluster/public_connectivity.hpp"
#include "cluster/telemetry.hpp"
#include "cluster/transcode_rates.hpp"
#include "config.hpp"
#include "contract/retention_class.hpp"
#include "contract/thread_safety.hpp"
#include "metadata/metadata.hpp"
#include "retained_memory.hpp"
#include "supervised.hpp"

#include <boost/asio/strand.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string_view>

namespace macha {

struct NodeReadiness {
    bool control_plane_online{};
    bool data_storage_ready{};
    // Ready, with disks configured and none of them online: the node runs, and
    // holds no extents until one comes back.
    bool data_storage_degraded{};
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

// A node's control store, as its telemetry reports it: the limit and the bytes held.
struct ControlStorageFigures {
    uint64_t capacity{};
    uint64_t used{};
};

// The client API URL a node advertises: the configured endpoint as given (it
// may sit behind a TLS proxy), else http on the advertised RPC host and the
// API port, never the usually undialable listen address; empty with no API.
std::string advertised_api_endpoint(const CatalogueApiConfig&, std::string advertise_host);

// The playback budgets a node with this streaming configuration enforces;
// zeros ("cannot say") when it serves no playback. Transcode rates are not
// configuration and are left empty.
PlaybackBudgets enforced_playback_budgets(const StreamingConfig&);

class NodeRuntime : public ClusterNode {
  public:
    using StartupStageHook = std::function<void(std::string_view)>;
    // At the transport's edge, for a test: for a call about to be placed on `peer`, a
    // result to stand for it (a call never answered, say), or none to place it.
    using CallFault = std::function<std::optional<AsyncRpc>(const NodeInfo& peer, MessageType)>;

  private:
    Config cfg_;
    // Before host_, whose configuration they key.
    ClusterKeys keys_;
    // Owned by the root; the node reads it for readiness and telemetry.
    RecoveryProgress& progress_;
    // Playback records, telemetry publishes.
    TranscodeRateBook& transcode_rates_;

    StartupStageHook startup_stage_hook_;
    std::atomic_bool control_plane_online_{};
    uint64_t startup_unix_ms_{};

    // The roster as the watch last told it, read only on the watch's strand.
    std::map<NodeId, NodeInfo> roster_told_;
    std::optional<uint64_t> roster_watch_;
    // The inbound answer last applied; the runtime thread's.
    std::optional<bool> inbound_applied_;
    // Told whether this node hosts extents, each time the answer moves: the
    // store's server, which alone writes the advert.
    Mutex hosts_mutex_;
    std::function<void(bool)> hosts_listener_ MACHA_GUARDED_BY(hosts_mutex_);
    // Reads the control store's figures for telemetry: the store node's, once open.
    mutable Mutex control_storage_mutex_;
    std::function<ControlStorageFigures()>
        control_storage_ MACHA_GUARDED_BY(control_storage_mutex_);

    std::atomic_uint64_t remote_metadata_generation_{};
    // Raises it to `generation` if higher, never lowers it: two readers that
    // each loaded, took their maximum and stored could put an older value back.
    void raise_remote_metadata_generation(uint64_t generation) noexcept {
        auto current = remote_metadata_generation_.load(std::memory_order_relaxed);
        while (current < generation &&
               !remote_metadata_generation_.compare_exchange_weak(current, generation)) {
        }
    }
    // Owned by the loop thread.
    std::optional<Clock::time_point> remote_generation_unbacked_since_;
    std::atomic_uint64_t remote_metadata_epoch_{};
    std::atomic_uint64_t metadata_announcements_{};
    // The replica's generation as last advertised; zero until the first.
    std::atomic_uint64_t advertised_metadata_generation_{};
    std::jthread runtime_;
    // Guards no state: the runtime thread's wait lock.
    Mutex runtime_wait_mutex_;
    std::condition_variable_any runtime_wait_cv_;
    std::atomic_uint64_t runtime_demand_{};
    std::atomic_bool started_{};
    std::atomic_bool outbound_calls_stopped_{};
    // Set before start, read by every caller after.
    CallFault call_fault_;

    // Dannan's node: the transport, the roster, telemetry, public connectivity and
    // the inbound answer. Built before storage and metadata, so the node is
    // reachable while its durable state recovers; built after everything its
    // callbacks read, and destroyed before it. Requests reach `routes`.
    DannanHost host_;
    // The node's id and epoch, after host_, whose node holds the state lock and reads them.
    NodeIdentity identity_;
    // Views over the node's roster and telemetry; this node's own figures are
    // Membership's.
    Membership members_;
    TelemetryStore telemetry_;

    bool all_local_state_ready() const noexcept;

    // Builds the node's configuration: Macha's, with what only this node gives it
    // (version, planes, telemetry fields, the inbound journal fields).
    dannan::NodeConfig node_config();
    // The hosted telemetry fields of this node's next record.
    void write_telemetry(dannan::status::Telemetry&) const;
    // A peer's metadata notice, on its session: must not wait.
    void on_metadata_notice(const RpcMessage& notice);
    // A change to the active roster, on the watch's strand: metadata on a higher
    // generation or a changed write floor.
    void on_roster_change(const dannan::RosterChange&);
    // Each heartbeat or wake: the readiness planes, the advertised endpoint, the
    // inbound answer, and the generation heard of.
    void runtime_loop(const std::stop_token&);
    void mirror_readiness();
    void follow_self();
    void follow_inbound();
    void settle_remote_generation();
    void wake_runtime();
    std::chrono::milliseconds stall_notice_for(MessageType) const;
    std::chrono::milliseconds no_progress_deadline_for(MessageType) const;
    void refuse_when_stopping() const;
    AsyncRpc place(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType,
                   std::chrono::milliseconds deadline);
    dannan::Node& readable_node() const;

  public:
    // Its Dannan node takes state_path's lock first, before any state is read, for this node's
    // life.
    NodeRuntime(Config, ClusterKeys, RecoveryProgress&, TranscodeRateBook&, MessageRoutes&,
                StartupStageHook startup_stage_hook);
    ~NodeRuntime() override;
    // Before start().
    void set_call_fault(CallFault fault) {
        call_fault_ = std::move(fault);
    }
    // Tells `listener` whether this node hosts extents now and each time the
    // answer moves; an empty one stops. Thread-safe; the listener runs under
    // a lock and must not call back.
    void follow_hosts_extents(std::function<void(bool)> listener);
    // What this node's telemetry reports of its control store, read at each refresh; an
    // empty reader reports nothing. Thread-safe; the reader runs under a lock and must
    // not call back.
    void follow_control_storage(std::function<ControlStorageFigures()> reader);
    void start();
    void request_stop();
    // Closes client routes and fails every pending synchronous call now, so
    // Service-owned workers can leave an RPC wait before Service joins them.
    void cancel_outbound_calls();
    void stop();
    // The metadata replica's generation, published with this node's record.
    // Lock-free, any thread.
    void advertise_metadata_generation(uint64_t generation);
    bool wait_local_state_ready(std::chrono::milliseconds timeout);
    NodeReadiness readiness() const;
    const Config& config() const override {
        return cfg_;
    }
    const ClusterKeys& keys() const {
        return identity_.keys;
    }
    const NodeIdentity& identity() const noexcept {
        return identity_;
    }
    NodeId node_id() const override {
        return identity_.id;
    }
    NodeId durability_epoch() const override {
        return identity_.durability_epoch;
    }
    // Republishes this node's metadata generation to peers that connect;
    // owners call it after advertising.
    void publish_self();
    // Republishes telemetry now rather than at the next sample, e.g. when
    // the node's phase changes.
    void signal_telemetry_refresh();
    Membership& membership() override {
        return members_;
    }
    // What has happened on this node, counted by its Dannan node for whoever
    // watches; Macha's kinds are named before the node starts.
    NodeEvents& events() {
        return host_.node().events();
    }
    const Membership& membership() const override {
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
    size_t broadcast_best_effort(const RpcMessage& message, FrameType frame_type);
    // The same, never skipped because the route table was momentarily in use.
    size_t broadcast_notify(const RpcMessage& message, FrameType frame_type);
    // Each call names its deadline (call_deadline.hpp); without a frame, the type's.
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t> payload,
                  std::chrono::milliseconds deadline);
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t> payload,
                  std::chrono::milliseconds deadline);
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType,
                  std::chrono::milliseconds deadline) override;
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType,
                  std::chrono::milliseconds deadline);
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t> payload,
                        std::chrono::milliseconds deadline);
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t> payload,
                        std::chrono::milliseconds deadline);
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType,
                        std::chrono::milliseconds deadline) override;
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType,
                        std::chrono::milliseconds deadline);
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
    std::vector<IdentityAssociationReset> identity_resets() const;
    PublicConnectivityStatus public_connectivity_status() const;
    // Waits for the node's refresh, within the connectivity timeouts.
    PublicConnectivityStatus refresh_public_connectivity(bool probe, bool force_probe = false);
    InboundResolution inbound_resolution() const;
    bool inbound_capable() const {
        return inbound_resolution().inbound_capable;
    }
    bool hosts_extents() const {
        return inbound_resolution().hosts_extents;
    }
    RpcStats rpc_stats() const;
    std::optional<std::chrono::milliseconds> peer_latency(const NodeId& peer) const override;
    std::map<NodeId, std::chrono::milliseconds> peer_latencies() const;
    RpcServerWorkStats rpc_server_work_stats() const;
    // Every supervised thread Status lists: Macha's, and the node's that share Macha's names.
    std::vector<SupervisedThreadStatus> thread_statuses() const;
    // The node beneath, for Status and the parts that will speak to it directly.
    dannan::Node& node() noexcept {
        return host_.node();
    }
    const dannan::Node& node() const noexcept {
        return host_.node();
    }
};
} // namespace macha

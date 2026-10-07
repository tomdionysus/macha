// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "contract/thread_safety.hpp"
#include "contract/work.hpp"
#include "crypto.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>
#include "retained_memory.hpp"

namespace macha {
enum class MessageType : uint16_t {
    ping = 1,
    members = 2,
    have_object = 3,
    get_object = 4,
    put_object = 5,
    get_metadata = 6,
    cas_metadata = 7,
    seed_metadata = 8,
    metadata_notice = 9,
    get_committed_metadata = 10,
    checkpoint_metadata = 11,
    session_retire = 12,
    delete_object = 13,
    promote_read_ahead = 14,
    promote_foreground = 15,
    cancel_transfer = 16,
    commit_metadata = 17,
    cas_metadata_delta = 18,
    get_control_object = 19,
    put_control_object = 20,
    get_metadata_identity = 21,
    put_object_deferred = 22,
    object_durability_barrier = 23,
    telemetry = 24,
    identity_resets = 25,
    get_metadata_history_entry = 26,
    has_metadata_history_entry = 27,
    put_metadata_history_entry = 28,
    get_metadata_heads = 29,
    put_metadata_commit = 30,
    accept_metadata_commit = 31,
    retain_objects = 32,
    get_ingest_jobs = 33,
    ingest_job_action = 34,
    get_torrent_jobs = 35,
    torrent_job_action = 36,
    // 37 and 38 are not in use.
    session_sync = 39,
    have_objects = 40,
    // CONTROL-plane counterpart of have_objects (which answers from
    // local_store() only), so a metadata publication skips control objects
    // the peer already holds. Reply: have_control_objects_reply; a peer that
    // errors is sent the whole graph.
    have_control_objects = 45,
    // An action on a claimed torrent while metadata cannot be written: the
    // owner applies it and journals the intent until it can publish.
    // Reply: torrent_intent_reply.
    torrent_intent = 46,
    // Repair's batched presence probe. Unlike have_objects (index lookup),
    // each id is read, decrypted and hashed like have_object, so a corrupt
    // copy reports absent. At most have_valid_objects_max ids. Reply:
    // have_valid_objects_reply; a peer that errors is probed per object.
    have_valid_objects = 47,
    // The availability survey's question: for each namespace tree node id,
    // how many extents lie beneath it, how many this node holds, and which
    // of its children it holds whole. At most tree_holdings_max ids. Reply:
    // tree_holdings_reply. A peer older than 0.82.0 drops a connection that
    // carries it, so it is sent only to peers that advertise a version with
    // it.
    tree_holdings = 48,
    // A history hash's record as a self-contained full-body entry,
    // materialised by the server (get_metadata_history_entry returns the
    // stored frame, possibly a delta the caller cannot replay).
    // Reply: metadata_history_entry_reply.
    get_metadata_history_record = 41,
    user_sync = 42,
    // For nodes that accept no inbound connections. dial_request is a
    // notification (request_id 0) over an existing CONTROL route asking a
    // non-dialable peer to open a lane to the sender. dial_back_probe asks a
    // peer to make one throwaway connection to the sender's advertised
    // endpoint and report whether the handshake completed; it is how
    // `network.inbound_capable: auto` resolves.
    dial_request = 43,
    dial_back_probe = 44,
    ok = 100,
    error = 101,
    members_reply = 102,
    bool_reply = 103,
    object_reply = 104,
    metadata_reply = 105,
    cas_reply = 106,
    control_object_reply = 107,
    metadata_identity_reply = 108,
    telemetry_reply = 109,
    identity_resets_reply = 110,
    metadata_history_entry_reply = 111,
    metadata_heads_reply = 112,
    ingest_jobs_reply = 113,
    ingest_job_action_reply = 114,
    torrent_jobs_reply = 115,
    torrent_job_action_reply = 116,
    session_sync_reply = 117,
    have_objects_reply = 118,
    user_sync_reply = 119,
    dial_back_probe_reply = 120,
    have_control_objects_reply = 121,
    torrent_intent_reply = 122,
    have_valid_objects_reply = 123,
    tree_holdings_reply = 124
};

// Each id in a have_valid_objects request is a full read on the peer, so a
// request is bounded to one data worker's handler-sized job.
inline constexpr size_t have_valid_objects_max = 16;

// Transport priority derives from the frame type; there is no separate
// priority field on the wire.
enum class TransportLane : uint8_t {
    control = 1,
    data = 2,
    // Dial-back probe: handshake (authenticating both ends), then close.
    // Nothing is sent and no route is registered, so a probe cannot retire a
    // real session in reconcile_locked().
    probe = 3,
};

const char* transport_lane_name(TransportLane) noexcept;

// Per-connection limits shared by every caller on a lane: messages queued for
// the writer and replies outstanding. A pipelining caller must size its window
// against these. The queue is the binding limit and is timing-dependent, as
// the writer drains it concurrently.
inline constexpr size_t max_pending_rpc_requests = 512;
inline constexpr size_t max_peer_outbound_messages = 256;
// Cluster bytes in and out by frame class since start, every sealed fragment
// with header and AEAD overhead. One instance is shared by the node's client,
// server and all their channels. Only Macha's own traffic.
struct TransportTraffic {
    static constexpr size_t classes = 6; // indexed by FrameType's wire value
    std::array<std::atomic_uint64_t, classes> in_bytes{};
    std::array<std::atomic_uint64_t, classes> out_bytes{};

    void note(bool outbound, FrameType type, uint64_t bytes) noexcept {
        const auto index = static_cast<size_t>(type);
        if (index >= classes)
            return;
        (outbound ? out_bytes : in_bytes)[index].fetch_add(bytes, std::memory_order_relaxed);
    }
};

const char* frame_type_name(FrameType) noexcept;
const char* message_type_name(MessageType) noexcept;
unsigned frame_type_priority(FrameType) noexcept;
FrameType default_frame_type(MessageType) noexcept;

struct RpcMessage {
    MessageType type{MessageType::error};
    Bytes payload;
    // Fragment memory leases follow the message through executor/future
    // handoffs, so no asynchronous gap goes uncharged.
    std::shared_ptr<std::vector<RetainedMemoryLedger::Lease>> retained_memory;

    RpcMessage() = default;
    RpcMessage(MessageType message_type, Bytes message_payload,
               std::shared_ptr<std::vector<RetainedMemoryLedger::Lease>> memory = {})
        : type(message_type), payload(std::move(message_payload)),
          retained_memory(std::move(memory)) {}
};

struct RpcFrame {
    uint64_t request_id{};
    FrameType frame_type{FrameType::control};
    RpcMessage message;
};

struct RpcReply {
    NodeInfo peer;
    RpcMessage message;
};

struct RpcStats {
    uint64_t connections_created{};
    uint64_t connections_reused{};
    uint64_t canonical_connections{};
};

struct RpcServerExecutionLimits {
    // One worker by default avoids idle threads across large clusters; more
    // may be configured, with per-peer FIFO preserved.
    size_t metadata_workers{1};
    size_t metadata_pending_jobs{64};
    size_t metadata_pending_bytes{256ULL * 1024 * 1024};
    // Byte-bounded queues, separate so loader/speculative payloads cannot
    // consume memory reserved for control or viewer work.
    size_t fast_control_pending_bytes{1ULL * 1024 * 1024};
    size_t control_pending_bytes{16ULL * 1024 * 1024};
    size_t data_pending_bytes{64ULL * 1024 * 1024};
};

struct RpcServerWorkStats {
    struct Timing {
        uint64_t requests{};
        uint64_t queue_wait_us_total{};
        uint64_t queue_wait_us_max{};
        uint64_t handler_us_total{};
        uint64_t handler_us_max{};
    };

    size_t metadata_pending_jobs{};
    size_t metadata_pending_bytes{};
    size_t metadata_active_jobs{};
    uint64_t metadata_rejected_jobs{};
    size_t fast_control_pending_bytes{};
    size_t control_pending_bytes{};
    size_t data_pending_bytes{};
    uint64_t rejected_jobs{};
    std::map<FrameType, Timing> frame_timings;
    std::map<MessageType, Timing> message_timings;
};

struct WireFragment {
    uint64_t request_id{};
    FrameType frame_type{FrameType::control};
    MessageType message_type{MessageType::error};
    bool first{};
    bool last{};
    Bytes payload;
};

// Fragment reassembly with aggregate limits beyond the per-message limit;
// standalone so its accounting is testable without sockets.
class MessageAssembler {
    struct Partial {
        FrameType frame_type{FrameType::control};
        MessageType message_type{MessageType::error};
        Bytes payload;
        std::shared_ptr<std::vector<RetainedMemoryLedger::Lease>> retained_memory;
    };

    std::map<uint64_t, Partial> partial_;
    size_t partial_bytes_{};
    size_t max_partial_messages_;
    size_t max_partial_bytes_;
    size_t max_message_bytes_;
    RetainedMemoryLedger* retained_memory_{};

  public:
    explicit MessageAssembler(size_t max_partial_messages = 64,
                              size_t max_partial_bytes = 256ULL * 1024 * 1024,
                              size_t max_message_bytes = 128ULL * 1024 * 1024,
                              RetainedMemoryLedger* retained_memory = nullptr);
    void promote(uint64_t request_id, FrameType);
    void discard(uint64_t request_id);
    std::optional<RpcFrame> push(WireFragment);
    size_t incomplete_messages() const noexcept {
        return partial_.size();
    }
    size_t incomplete_bytes() const noexcept {
        return partial_bytes_;
    }
};

class SecureChannel {
    // Fixed from construction until the destructor closes it.
    int fd_{-1};
    ClusterKeys keys_;
    NodeInfo local_;
    std::array<uint8_t, 32> tx_{}, rx_{};
    std::array<uint8_t, 32> session_id_{};
    uint64_t tx_counter_{}, rx_counter_{};
    size_t configured_max_frame_size_{};
    size_t negotiated_max_frame_size_{};
    TransportLane lane_{TransportLane::control};
    bool ready_{};
    // Held across shutdown(2), close(2) and setsockopt(2) on the socket.
    IoMutex close_mutex_;
    bool shutdown_ MACHA_GUARDED_BY(close_mutex_){};
    std::shared_ptr<TransportTraffic> traffic_;
    void close_fd();

  public:
    SecureChannel(int, ClusterKeys, NodeInfo, size_t max_frame_size);
    // Set before the channel carries frames; handshake bytes are not counted.
    void set_traffic(std::shared_ptr<TransportTraffic> traffic) { traffic_ = std::move(traffic); }
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;
    NodeInfo client_handshake(TransportLane);
    NodeInfo server_handshake(const std::string& remote_host);
    void set_io_timeout(std::chrono::milliseconds);
    void send_fragment(uint64_t request_id, FrameType, MessageType, bool first, bool last,
                       std::span<const uint8_t>, const std::function<void(size_t)>& progress = {});
    WireFragment receive_fragment(const std::function<void(uint64_t, size_t)>& progress = {});
    void shutdown();
    size_t max_frame_size() const noexcept {
        return negotiated_max_frame_size_;
    }
    const std::array<uint8_t, 32>& session_id() const noexcept {
        return session_id_;
    }
    TransportLane lane() const noexcept {
        return lane_;
    }
};

class AsyncRpc {
    std::future<RpcReply> future_;
    std::function<void()> cancel_;
    std::function<void()> abort_;
    std::function<void(FrameType)> promote_;
    std::function<std::chrono::milliseconds()> idle_;

  public:
    AsyncRpc() = default;
    AsyncRpc(std::future<RpcReply>, std::function<void()>, std::function<void()>,
             std::function<void(FrameType)> = {}, std::function<std::chrono::milliseconds()> = {});
    ~AsyncRpc();
    AsyncRpc(AsyncRpc&&) noexcept;
    AsyncRpc& operator=(AsyncRpc&&) noexcept;
    AsyncRpc(const AsyncRpc&) = delete;
    AsyncRpc& operator=(const AsyncRpc&) = delete;

    bool valid() const;
    std::future_status wait_for(std::chrono::milliseconds);
    RpcReply get();
    void cancel();
    void abort();
    void promote(FrameType);
    std::function<void(FrameType)> promotion_callback() const {
        return promote_;
    }
    std::chrono::milliseconds idle_for() const;
};

// An established, authenticated lane to one peer, as a call is placed on it:
// a session this node dialled or one the peer opened.
class RpcRoute {
  public:
    virtual ~RpcRoute() = default;
    virtual bool usable() const = 0;
    // Queues the call; throws when the route refuses it (closed, retiring, or
    // a queue or pending-reply limit reached).
    virtual AsyncRpc call(MessageType, std::span<const uint8_t>, FrameType) = 0;
};

// Where RpcClient meets its peers: the socket a lane is dialled on, the route
// a call is placed on, and a session a peer opened becoming a route. RpcClient
// owns the routing between them. NetworkLinks is the production implementation.
class RpcLinks {
  public:
    virtual ~RpcLinks() = default;

    // A connected TCP socket to `endpoint` for `lane`, owned by the caller.
    // Throws when no connection is made within `timeout`.
    static constexpr Waits connect_waits = Waits::network;
    static constexpr ThreadSafety connect_safety = ThreadSafety::thread_safe;
    virtual int connect(const Endpoint&, TransportLane, std::chrono::milliseconds timeout) = 0;

    // Places one call to `peer` on `route`, which carries `lane`. Empty when
    // the route no longer takes calls; throws when it refuses this one.
    // Queues only: never waits for the peer.
    static constexpr Waits send_waits = Waits::none;
    static constexpr ThreadSafety send_safety = ThreadSafety::thread_safe;
    virtual std::optional<AsyncRpc> send(const NodeId& peer, TransportLane lane, MessageType,
                                         std::span<const uint8_t>, FrameType,
                                         RpcRoute& route) = 0;

    // A session `peer` opened on `lane` has authenticated; `install` makes it
    // a route, under the transport's route lock.
    static constexpr Waits admit_waits = Waits::locks;
    static constexpr ThreadSafety admit_safety = ThreadSafety::thread_safe;
    virtual void admit(const NodeInfo& peer, TransportLane lane, std::function<void()> install) = 0;
};

// Dials over TCP, places each call on the route it is given, and installs
// every session as soon as it authenticates. Stateless.
class NetworkLinks final : public RpcLinks {
  public:
    int connect(const Endpoint&, TransportLane, std::chrono::milliseconds timeout) override;
    std::optional<AsyncRpc> send(const NodeId& peer, TransportLane lane, MessageType,
                                 std::span<const uint8_t>, FrameType, RpcRoute& route) override;
    void admit(const NodeInfo& peer, TransportLane lane, std::function<void()> install) override;
};

class RpcClient {
    class PeerConnection;
    friend class RpcServer;

    // Replies are moved from the handler into the transport queue, not
    // copied: they can be multi-megabyte objects.
    using InboundReply = std::function<void(RpcMessage)>;
    using InboundHandler = std::function<void(const NodeInfo&, RpcFrame, InboundReply)>;
    using InboundPromoter = std::function<void(const NodeInfo&, uint64_t, FrameType)>;
    using InboundCanceller = std::function<void(const NodeInfo&, uint64_t)>;

    struct InboundRoute {
        NodeInfo peer;
        TransportLane lane{TransportLane::control};
        std::array<uint8_t, 32> session_id{};
        std::function<AsyncRpc(MessageType, std::span<const uint8_t>, FrameType)> call;
        std::function<void(const RpcMessage&)> notify;
        std::function<bool(const RpcMessage&, FrameType)> try_notify;
        std::function<void()> retire;
        std::function<void()> close;
        std::function<bool()> usable;
    };

    struct PeerHealth {
        unsigned failures{};
        Clock::time_point retry_after{};
        // Smoothed round trip of the health loop's CONTROL-lane heartbeat
        // pings only, so payload size and handler work do not distort it.
        std::optional<double> control_latency_ms;
    };

    // Fixed at construction.
    RpcLinks& links_;
    ClusterKeys keys_;
    std::function<NodeInfo()> local_;
    std::function<void(const NodeInfo&)> peer_observer_;
    std::function<void(uint64_t)> metadata_observer_;
    std::chrono::milliseconds connect_timeout_;
    std::chrono::milliseconds heartbeat_;
    std::chrono::milliseconds dead_after_;
    size_t max_frame_size_{};
    RetainedMemoryLedger* retained_memory_{};
    std::shared_ptr<TransportTraffic> traffic_{std::make_shared<TransportTraffic>()};
    // Held across the local_ callback, inbound routes' usable() callbacks and
    // logging.
    mutable IoMutex mutex_;
    std::condition_variable connection_cv_;
    // Dialling is expensive (two persistent threads, multi-megabyte DATA
    // buffers): concurrent cold callers for one peer/lane share one dial.
    std::set<std::string> connection_dials_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, std::shared_ptr<PeerConnection>> connections_ MACHA_GUARDED_BY(mutex_);
    std::vector<std::shared_ptr<PeerConnection>> retired_connections_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, InboundRoute> inbound_routes_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, NodeId> endpoint_peers_ MACHA_GUARDED_BY(mutex_);
    // Waits, up to the connect timeout, for any usable route to `peer`.
    bool await_route(const NodeId& peer, TransportLane lane);
    std::map<std::string, PeerHealth> health_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, Endpoint> endpoints_ MACHA_GUARDED_BY(mutex_);
    std::map<std::string, IdentityAssociationReset> identity_resets_ MACHA_GUARDED_BY(mutex_);
    // Each authenticated peer's NodeInfo::flags, from the handshake and from
    // note_peer(). A peer without inbound_capable is never dialled; it is asked
    // to dial instead.
    std::map<NodeId, uint8_t> peer_flags_ MACHA_GUARDED_BY(mutex_);
    // Lanes the health thread must open from this side (a peer's dial_request,
    // or the maintained peers when this node is inbound-incapable). Keyed by route_key.
    std::map<std::string, std::pair<NodeInfo, TransportLane>> requested_lanes_
        MACHA_GUARDED_BY(mutex_);
    std::function<std::vector<NodeInfo>()> maintained_peers_ MACHA_GUARDED_BY(mutex_);
    std::atomic_uint64_t lane_wakeups_{};
    std::atomic_uint64_t dial_requests_sent_{};
    std::atomic_uint64_t dial_requests_received_{};
    std::atomic_uint64_t connections_created_{};
    std::atomic_uint64_t connections_reused_{};
    std::jthread health_thread_;
    // Guards nothing: the health thread's wait lock; wakes are counted in
    // lane_wakeups_.
    Mutex health_wait_mutex_;
    std::condition_variable_any health_wait_cv_;
    // Held across the inbound handler, promoter and canceller callbacks.
    IoMutex inbound_mutex_;
    InboundHandler inbound_handler_ MACHA_GUARDED_BY(inbound_mutex_);
    InboundPromoter inbound_promoter_ MACHA_GUARDED_BY(inbound_mutex_);
    InboundCanceller inbound_canceller_ MACHA_GUARDED_BY(inbound_mutex_);

    static std::string endpoint_key(const Endpoint&);
    static std::string peer_key(const NodeId&);
    static std::string route_key(const NodeId&, TransportLane);
    static std::string dial_key(const Endpoint&, TransportLane);
    static TransportLane lane_for(MessageType, FrameType) noexcept;
    std::shared_ptr<PeerConnection> connection(const Endpoint&, const NodeId* expected,
                                               NodeId* actual, TransportLane);
    AsyncRpc call_async_known(const Endpoint&, const NodeId*, MessageType, std::span<const uint8_t>,
                              FrameType);
    // Sends on an existing route (outbound first, then inbound); never dials.
    // Empty when no usable route exists on `lane`. `why`, if given, receives
    // the reason a usable route refused the call (e.g. pending-reply budget).
    std::optional<AsyncRpc> call_existing(const NodeId&, TransportLane, MessageType,
                                          std::span<const uint8_t>, FrameType,
                                          std::string* why = nullptr);
    bool local_inbound_capable() const;
    bool peer_inbound_capable_locked(const NodeId&) const MACHA_REQUIRES(mutex_);
    bool route_usable_locked(const NodeId&, TransportLane) const MACHA_REQUIRES(mutex_);
    void note_peer_locked(const NodeInfo&) MACHA_REQUIRES(mutex_);
    // Waits (bounded by connect_timeout) for a non-dialable peer to open
    // `lane` to us after a dial_request; throws with the reason if it never
    // does. `lock` holds mutex_ on entry and on return; released when it throws.
    void await_reverse_dial(Lock& lock, const NodeId& peer, TransportLane lane,
                            const std::string& retry_key) MACHA_REQUIRES(mutex_);
    void open_requested_lanes(std::stop_token);
    void observe_result(const std::string&, bool, std::chrono::milliseconds,
                        bool latency_sample = false);
    void health_loop(std::stop_token);
    void close_endpoint(const Endpoint&, const std::string&);
    void set_inbound_handler(InboundHandler);
    void set_inbound_transfer_control(InboundPromoter, InboundCanceller);
    void dispatch_inbound(const NodeInfo&, RpcFrame, InboundReply);
    void dispatch_inbound_promotion(const NodeInfo&, uint64_t, FrameType);
    void dispatch_inbound_cancel(const NodeInfo&, uint64_t);
    // Hands an accepted session to links_.admit(), which installs it.
    void register_inbound(InboundRoute);
    void install_inbound(InboundRoute);
    void unregister_inbound(const NodeId&, TransportLane,
                            const std::array<uint8_t, 32>& session_id);
    void reconcile_locked(const NodeId&, TransportLane, std::vector<std::function<void()>>& retire)
        MACHA_REQUIRES(mutex_);
    // Snapshots the CONTROL routes; `wait` for mutex_, else false if it is busy.
    bool try_control_routes(
        std::vector<std::shared_ptr<PeerConnection>>& outbound,
        std::vector<std::function<bool(const RpcMessage&, FrameType)>>& inbound, bool wait);
    size_t notify_control_routes(const RpcMessage&, FrameType, bool wait);
    void reap_retired();

  public:
    // `links` outlives the client.
    RpcClient(RpcLinks& links, ClusterKeys, std::function<NodeInfo()>,
              std::function<void(const NodeInfo&)>, std::function<void(uint64_t)>,
              std::chrono::milliseconds connect_timeout,
              std::chrono::milliseconds heartbeat = std::chrono::seconds(5),
              std::chrono::milliseconds dead_after = std::chrono::seconds(30),
              size_t max_frame_size = 256 * 1024,
              RetainedMemoryLedger* retained_memory = nullptr);
    ~RpcClient();
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType);
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    // `no_progress_deadline`: cancel and fail the call once it has made no
    // progress for this long; zero waits indefinitely.
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>,
                  std::chrono::milliseconds stall_notice,
                  std::chrono::milliseconds no_progress_deadline = {});
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>,
                  std::chrono::milliseconds stall_notice,
                  std::chrono::milliseconds no_progress_deadline = {});
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType,
                  std::chrono::milliseconds stall_notice,
                  std::chrono::milliseconds no_progress_deadline = {});
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType,
                  std::chrono::milliseconds stall_notice,
                  std::chrono::milliseconds no_progress_deadline = {});
    RpcStats stats() const;
    // Shared with the node's server: the node's whole cluster traffic.
    const std::shared_ptr<TransportTraffic>& traffic() const noexcept { return traffic_; }
    // Smoothed CONTROL-lane round trip to a peer, if any call has completed.
    std::optional<std::chrono::milliseconds> peer_latency(const NodeId&) const;
    std::map<NodeId, std::chrono::milliseconds> peer_latencies() const;

    // Teaches the transport a peer's flags (inbound-capable or not) before it
    // has met the peer in a handshake.
    void note_peer(const NodeInfo&);
    bool has_route(const NodeId&, TransportLane) const;
    // Queues `lane` to `peer` for the health thread to dial under the ordinary
    // retry backoff, so a peer that keeps losing a lane cannot cause a redial loop.
    void request_lane(const NodeInfo& peer, TransportLane lane);
    // While this node is inbound-incapable, keep CONTROL and DATA dialled to
    // every peer the callback returns: only this side can restore its reachability.
    void set_maintained_peers(std::function<std::vector<NodeInfo>()>);
    // One-shot dial-back: a fresh connection to `endpoint` whose handshake
    // must authenticate as `expected`, then closed; never a route. The
    // evidence for `inbound_capable: auto`. Returns the error text, empty on success.
    std::string probe_dial(const Endpoint& endpoint, const NodeId& expected);
    uint64_t dial_requests_sent() const {
        return dial_requests_sent_.load(std::memory_order_relaxed);
    }
    uint64_t dial_requests_received() const {
        return dial_requests_received_.load(std::memory_order_relaxed);
    }
    void broadcast(const RpcMessage&);
    size_t broadcast_best_effort(const RpcMessage&, FrameType);
    // As broadcast_best_effort, but waits for the route table instead of
    // sending nothing when it is momentarily in use. Still never dials and
    // never waits on a peer: one whose route is busy is not queued for.
    size_t broadcast_notify(const RpcMessage&, FrameType);
    void invalidate_identity_association(const IdentityAssociationReset&);
    void stop();
};

class RpcServer {
  public:
    using Handler = std::function<RpcMessage(const NodeInfo&, FrameType, const RpcMessage&)>;
    using Observer = std::function<void(const NodeInfo&)>;

  private:
    struct Session;
    struct RequestJob {
        std::shared_ptr<Session> session;
        NodeInfo peer;
        RpcFrame frame;
        RpcClient::InboundReply reply;
        Clock::time_point queued_at{Clock::now()};
        RetainedMemoryLedger::Lease memory;
    };

    struct AtomicTiming {
        std::atomic_uint64_t requests{};
        std::atomic_uint64_t queue_wait_us_total{};
        std::atomic_uint64_t queue_wait_us_max{};
        std::atomic_uint64_t handler_us_total{};
        std::atomic_uint64_t handler_us_max{};
    };

    std::string host_;
    uint16_t port_;
    ClusterKeys keys_;
    NodeInfo local_ MACHA_GUARDED_BY(local_mutex_);
    mutable Mutex local_mutex_;
    Handler handler_;
    Observer observer_;
    size_t max_frame_size_{};
    std::atomic_int listen_fd_{-1};
    uint16_t bound_port_{};
    std::jthread accept_thread_;
    enum class RequestClass { control, foreground, read_ahead, loader, speculative };
    std::vector<std::jthread> fast_control_workers_;
    std::vector<std::jthread> control_workers_;
    std::vector<std::jthread> metadata_workers_;
    std::vector<std::jthread> data_workers_;
    mutable Mutex request_mutex_;
    std::condition_variable request_cv_;
    std::deque<RequestJob> fast_control_requests_ MACHA_GUARDED_BY(request_mutex_);
    std::deque<RequestJob> control_requests_ MACHA_GUARDED_BY(request_mutex_);
    std::deque<RequestJob> metadata_requests_ MACHA_GUARDED_BY(request_mutex_);
    std::deque<RequestJob> foreground_requests_ MACHA_GUARDED_BY(request_mutex_);
    std::deque<RequestJob> read_ahead_requests_ MACHA_GUARDED_BY(request_mutex_);
    std::deque<RequestJob> loader_requests_ MACHA_GUARDED_BY(request_mutex_);
    std::deque<RequestJob> speculative_requests_ MACHA_GUARDED_BY(request_mutex_);
    RpcServerExecutionLimits execution_limits_;
    RetainedMemoryLedger* retained_memory_{};
    size_t metadata_request_bytes_ MACHA_GUARDED_BY(request_mutex_){};
    size_t fast_control_request_bytes_ MACHA_GUARDED_BY(request_mutex_){};
    size_t control_request_bytes_ MACHA_GUARDED_BY(request_mutex_){};
    size_t data_request_bytes_ MACHA_GUARDED_BY(request_mutex_){};
    std::set<NodeId> metadata_active_peers_ MACHA_GUARDED_BY(request_mutex_);
    std::atomic_size_t active_metadata_requests_{};
    std::atomic_uint64_t rejected_metadata_requests_{};
    std::atomic_uint64_t rejected_requests_{};
    // Fixed buckets over the small wire namespaces: bounded, lock- and
    // allocation-free on the handler path.
    std::array<AtomicTiming, 6> frame_timings_{};
    std::array<AtomicTiming, 256> message_timings_{};
    size_t active_nonforeground_data_ MACHA_GUARDED_BY(request_mutex_){};
    Mutex sessions_mutex_;
    std::vector<std::shared_ptr<Session>> sessions_ MACHA_GUARDED_BY(sessions_mutex_);
    std::atomic_size_t pre_auth_sessions_{};
    RpcClient* shared_client_{};
    std::shared_ptr<TransportTraffic> traffic_{std::make_shared<TransportTraffic>()};

    static RequestClass request_class(FrameType);
    static bool fast_control_request(const RpcFrame&);
    static bool metadata_mutation_request(const RpcFrame&);
    std::deque<RequestJob>& queue(RequestClass) MACHA_REQUIRES(request_mutex_);
    bool admit_locked(RequestJob) MACHA_REQUIRES(request_mutex_);
    bool data_ready() const MACHA_REQUIRES(request_mutex_);
    RequestClass next_data_class() const MACHA_REQUIRES(request_mutex_);
    void accept_loop(std::stop_token);
    void session_loop(Session*);
    void fast_control_worker_loop(std::stop_token);
    void control_worker_loop(std::stop_token);
    void metadata_worker_loop(std::stop_token);
    void data_worker_loop(std::stop_token);
    void execute(RequestJob);
    void reap_sessions(bool all);
    void enqueue_shared(const NodeInfo&, RpcFrame, RpcClient::InboundReply);
    void enqueue_notification(const NodeInfo&, RpcFrame);
    void promote_queued(const NodeInfo&, uint64_t, FrameType);
    void cancel_queued(const NodeInfo&, uint64_t);

  public:
    RpcServer(std::string, uint16_t, ClusterKeys, NodeInfo, Handler, Observer,
              size_t max_frame_size = 256 * 1024, RpcServerExecutionLimits execution_limits = {},
              RetainedMemoryLedger* retained_memory = nullptr);
    ~RpcServer();
    void start();
    void stop();
    void attach_client(RpcClient&);
    void set_local(NodeInfo);
    void broadcast(const RpcMessage&);
    uint16_t bound_port() const {
        return bound_port_;
    }
    RpcServerWorkStats work_stats() const;
};
} // namespace macha

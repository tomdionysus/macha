// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crypto.hpp"
#include "types.hpp"

#include <atomic>
#include <condition_variable>
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
    ok = 100,
    error = 101,
    members_reply = 102,
    bool_reply = 103,
    object_reply = 104,
    metadata_reply = 105,
    cas_reply = 106
};

// Transport priority is a property of the frame type itself. There is no
// independent priority field on the wire which can contradict it.
enum class TransportLane : uint8_t {
    control = 1,
    data = 2,
};

const char* transport_lane_name(TransportLane) noexcept;

enum class FrameType : uint8_t {
    control = 1,
    foreground = 2,
    read_ahead = 3,
    speculative = 4,
};

const char* frame_type_name(FrameType) noexcept;
unsigned frame_type_priority(FrameType) noexcept;
FrameType default_frame_type(MessageType) noexcept;

struct RpcMessage {
    MessageType type{MessageType::error};
    Bytes payload;
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

struct WireFragment {
    uint64_t request_id{};
    FrameType frame_type{FrameType::control};
    MessageType message_type{MessageType::error};
    bool first{};
    bool last{};
    Bytes payload;
};

class SecureChannel {
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
    std::mutex close_mutex_;
    bool shutdown_{};
    void close_fd();

  public:
    SecureChannel(int, ClusterKeys, NodeInfo, size_t max_frame_size);
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;
    NodeInfo client_handshake(TransportLane);
    NodeInfo server_handshake(const std::string& remote_host);
    void send_fragment(uint64_t request_id, FrameType, MessageType, bool first, bool last,
                       std::span<const uint8_t>,
                       const std::function<void(size_t)>& progress = {});
    WireFragment receive_fragment(
        const std::function<void(uint64_t, size_t)>& progress = {});
    void shutdown();
    size_t max_frame_size() const noexcept { return negotiated_max_frame_size_; }
    const std::array<uint8_t, 32>& session_id() const noexcept { return session_id_; }
    TransportLane lane() const noexcept { return lane_; }
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
             std::function<void(FrameType)> = {},
             std::function<std::chrono::milliseconds()> = {});
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
    std::function<void(FrameType)> promotion_callback() const { return promote_; }
    std::chrono::milliseconds idle_for() const;
};

class RpcClient {
    class PeerConnection;
    friend class RpcServer;

    using InboundReply = std::function<void(const RpcMessage&)>;
    using InboundHandler = std::function<void(const NodeInfo&, RpcFrame, InboundReply)>;
    using InboundPromoter = std::function<void(const NodeInfo&, uint64_t, FrameType)>;
    using InboundCanceller = std::function<void(const NodeInfo&, uint64_t)>;

    struct InboundRoute {
        NodeInfo peer;
        TransportLane lane{TransportLane::control};
        std::array<uint8_t, 32> session_id{};
        std::function<AsyncRpc(MessageType, std::span<const uint8_t>, FrameType)> call;
        std::function<void(const RpcMessage&)> notify;
        std::function<void()> retire;
        std::function<void()> close;
        std::function<bool()> usable;
    };

    struct PeerHealth {
        unsigned failures{};
        Clock::time_point retry_after{};
    };

    ClusterKeys keys_;
    std::function<NodeInfo()> local_;
    std::function<void(const NodeInfo&)> peer_observer_;
    std::function<void(uint64_t)> metadata_observer_;
    std::chrono::milliseconds connect_timeout_;
    std::chrono::milliseconds heartbeat_;
    std::chrono::milliseconds dead_after_;
    size_t max_frame_size_{};
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<PeerConnection>> connections_;
    std::vector<std::shared_ptr<PeerConnection>> retired_connections_;
    std::map<std::string, InboundRoute> inbound_routes_;
    std::map<std::string, NodeId> endpoint_peers_;
    std::map<std::string, PeerHealth> health_;
    std::map<std::string, Endpoint> endpoints_;
    std::atomic_uint64_t connections_created_{};
    std::atomic_uint64_t connections_reused_{};
    std::jthread health_thread_;
    std::mutex inbound_mutex_;
    InboundHandler inbound_handler_;
    InboundPromoter inbound_promoter_;
    InboundCanceller inbound_canceller_;

    static std::string endpoint_key(const Endpoint&);
    static std::string peer_key(const NodeId&);
    static std::string route_key(const NodeId&, TransportLane);
    static std::string dial_key(const Endpoint&, TransportLane);
    static TransportLane lane_for(MessageType, FrameType) noexcept;
    std::shared_ptr<PeerConnection> connection(const Endpoint&, const NodeId* expected,
                                               NodeId* actual, TransportLane);
    AsyncRpc call_async_known(const Endpoint&, const NodeId*, MessageType,
                              std::span<const uint8_t>, FrameType);
    void observe_result(const std::string&, bool, std::chrono::milliseconds);
    void health_loop(std::stop_token);
    void close_endpoint(const Endpoint&, const std::string&);
    void set_inbound_handler(InboundHandler);
    void set_inbound_transfer_control(InboundPromoter, InboundCanceller);
    void dispatch_inbound(const NodeInfo&, RpcFrame, InboundReply);
    void dispatch_inbound_promotion(const NodeInfo&, uint64_t, FrameType);
    void dispatch_inbound_cancel(const NodeInfo&, uint64_t);
    void register_inbound(InboundRoute);
    void unregister_inbound(const NodeId&, TransportLane,
                            const std::array<uint8_t, 32>& session_id);
    void reconcile_locked(const NodeId&, TransportLane,
                          std::vector<std::function<void()>>& retire);
    void reap_retired();

  public:
    RpcClient(ClusterKeys, std::function<NodeInfo()>, std::function<void(const NodeInfo&)>,
              std::function<void(uint64_t)>, std::chrono::milliseconds connect_timeout,
              std::chrono::milliseconds heartbeat = std::chrono::seconds(5),
              std::chrono::milliseconds dead_after = std::chrono::seconds(30),
              size_t max_frame_size = 256 * 1024);
    ~RpcClient();
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t> payload = {});
    AsyncRpc call_async(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType);
    AsyncRpc call_async(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType);
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>,
                  std::chrono::milliseconds stall_notice);
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>,
                  std::chrono::milliseconds stall_notice);
    RpcReply call(const Endpoint&, MessageType, std::span<const uint8_t>, FrameType,
                  std::chrono::milliseconds stall_notice);
    RpcReply call(const NodeInfo&, MessageType, std::span<const uint8_t>, FrameType,
                  std::chrono::milliseconds stall_notice);
    RpcStats stats() const;
    void broadcast(const RpcMessage&);
    void stop();
};

class RpcServer {
  public:
    using Handler = std::function<RpcMessage(const NodeInfo&, const RpcMessage&)>;
    using Observer = std::function<void(const NodeInfo&)>;

  private:
    struct Session;
    struct RequestJob {
        std::shared_ptr<Session> session;
        NodeInfo peer;
        RpcFrame frame;
        std::function<void(const RpcMessage&)> reply;
    };

    std::string host_;
    uint16_t port_;
    ClusterKeys keys_;
    NodeInfo local_;
    Handler handler_;
    Observer observer_;
    size_t max_frame_size_{};
    std::atomic_int listen_fd_{-1};
    uint16_t bound_port_{};
    std::jthread accept_thread_;
    enum class RequestClass { control, foreground, read_ahead, speculative };
    std::vector<std::jthread> control_workers_;
    std::vector<std::jthread> data_workers_;
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::deque<RequestJob> control_requests_;
    std::deque<RequestJob> foreground_requests_;
    std::deque<RequestJob> read_ahead_requests_;
    std::deque<RequestJob> speculative_requests_;
    std::mutex sessions_mutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    RpcClient* shared_client_{};

    static RequestClass request_class(FrameType);
    std::deque<RequestJob>& queue(RequestClass);
    bool data_ready() const;
    RequestClass next_data_class() const;
    void accept_loop(std::stop_token);
    void session_loop(Session*);
    void control_worker_loop(std::stop_token);
    void data_worker_loop(std::stop_token);
    void execute(RequestJob);
    void reap_sessions(bool all);
    void enqueue_shared(const NodeInfo&, RpcFrame, RpcClient::InboundReply);
    void promote_queued(const NodeInfo&, uint64_t, FrameType);
    void cancel_queued(const NodeInfo&, uint64_t);

  public:
    RpcServer(std::string, uint16_t, ClusterKeys, NodeInfo, Handler, Observer,
              size_t max_frame_size = 256 * 1024);
    ~RpcServer();
    void start();
    void stop();
    void attach_client(RpcClient&);
    void broadcast(const RpcMessage&);
    uint16_t bound_port() const { return bound_port_; }
};
} // namespace macha

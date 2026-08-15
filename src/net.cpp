// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace macha {
namespace {
constexpr uint16_t protocol_version = 7;
constexpr uint32_t frame_magic = 0x4d434837; // "MCH7"
constexpr size_t protocol_min_frame_size = 4 * 1024;
constexpr size_t protocol_max_frame_size = 4 * 1024 * 1024;
constexpr size_t max_message_size = 128 * 1024 * 1024;
constexpr size_t control_worker_count = 2;
constexpr size_t data_worker_count = 8;
constexpr size_t max_pending_requests = 512;
constexpr size_t max_peer_outbound = 256;
constexpr size_t frame_header_size = 28;
constexpr uint8_t frame_first = 0x01;
constexpr uint8_t frame_last = 0x02;

void validate_frame_limit(size_t size) {
    if (size < protocol_min_frame_size || size > protocol_max_frame_size)
        throw std::runtime_error("max frame size must be 4K..4M");
}

void socket_options(int fd) {
    int yes = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

void send_all(int fd, std::span<const uint8_t> bytes,
              const std::function<void(size_t)>& progress = {}) {
    size_t done = 0;
    while (done < bytes.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        auto count = ::send(fd, bytes.data() + done, bytes.size() - done, send_flags);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string("send: ") + strerror(errno));
        }
        if (count == 0)
            throw std::runtime_error("closed socket");
        done += static_cast<size_t>(count);
        if (progress)
            progress(static_cast<size_t>(count));
    }
}

void recv_all(int fd, std::span<uint8_t> bytes,
              const std::function<void(size_t)>& progress = {}) {
    size_t done = 0;
    while (done < bytes.size()) {
        auto count = ::recv(fd, bytes.data() + done, bytes.size() - done, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string("recv: ") + strerror(errno));
        }
        if (count == 0)
            throw std::runtime_error("peer closed");
        done += static_cast<size_t>(count);
        if (progress)
            progress(static_cast<size_t>(count));
    }
}

void send_blob(int fd, std::span<const uint8_t> bytes) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(bytes.size()));
    send_all(fd, writer.data());
    send_all(fd, bytes);
}

Bytes recv_blob(int fd, size_t maximum) {
    std::array<uint8_t, 4> header{};
    recv_all(fd, header);
    Reader reader(header);
    auto size = reader.u32();
    if (size > maximum)
        throw std::runtime_error("blob too large");
    Bytes out(size);
    recv_all(fd, out);
    return out;
}

Bytes label(const char* prefix, std::span<const uint8_t> data) {
    Bytes out(reinterpret_cast<const uint8_t*>(prefix),
              reinterpret_cast<const uint8_t*>(prefix) + strlen(prefix));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Bytes session_info(std::span<const uint8_t> transcript, const NodeId& client,
                   const NodeId& server, const char* direction) {
    Writer writer;
    writer.string("macha/session/v7");
    writer.string(direction);
    writer.fixed(sha256(transcript).bytes);
    writer.fixed(client.bytes);
    writer.fixed(server.bytes);
    return writer.take();
}

int connect_socket(const Endpoint& endpoint, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* results = nullptr;
    auto port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results))
        throw std::runtime_error("resolve failed");

    int fd = -1;
    int last_error = ECONNREFUSED;
    for (auto* result = results; result; result = result->ai_next) {
        fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (fd < 0)
            continue;

        auto flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, result->ai_addr, result->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd poll_fd{fd, POLLOUT, 0};
            rc = poll(&poll_fd, 1, static_cast<int>(timeout.count()));
            if (rc > 0) {
                int error = 0;
                socklen_t size = sizeof(error);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
                if (error == 0)
                    rc = 0;
                else {
                    errno = error;
                    rc = -1;
                }
            } else {
                errno = ETIMEDOUT;
                rc = -1;
            }
        }

        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            socket_options(fd);
            break;
        }

        last_error = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);

    if (fd < 0)
        throw std::runtime_error(std::string("connect: ") + strerror(last_error));
    return fd;
}

int bind_socket(const std::string& host, uint16_t port, uint16_t* bound_port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* results = nullptr;
    auto port_string = std::to_string(port);
    if (getaddrinfo((host.empty() || host == "*") ? nullptr : host.c_str(), port_string.c_str(),
                    &hints, &results)) {
        throw std::runtime_error("bind resolve failed");
    }

    int fd = -1;
    for (auto* result = results; result; result = result->ai_next) {
        fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (fd < 0)
            continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, result->ai_addr, result->ai_addrlen) == 0 && listen(fd, 128) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0)
        throw std::runtime_error("bind failed");

    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    if (address.ss_family == AF_INET)
        *bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    else
        *bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    return fd;
}

std::string numeric_host(const sockaddr_storage& address, socklen_t size) {
    char host[NI_MAXHOST]{};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), size, host, sizeof(host), nullptr,
                    0, NI_NUMERICHOST)) {
        return {};
    }
    return host;
}

std::exception_ptr rpc_error(const std::string& text) {
    return std::make_exception_ptr(std::runtime_error(text));
}

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

FrameType decode_frame_type(uint8_t value) {
    switch (value) {
    case static_cast<uint8_t>(FrameType::control):
        return FrameType::control;
    case static_cast<uint8_t>(FrameType::foreground):
        return FrameType::foreground;
    case static_cast<uint8_t>(FrameType::read_ahead):
        return FrameType::read_ahead;
    case static_cast<uint8_t>(FrameType::speculative):
        return FrameType::speculative;
    default:
        throw std::runtime_error("bad frame type");
    }
}

bool is_bulk_message(MessageType type) {
    return type == MessageType::get_object || type == MessageType::put_object ||
           type == MessageType::object_reply;
}

bool class_allowed(MessageType type, FrameType frame_type) {
    if (is_bulk_message(type))
        return frame_type != FrameType::control;
    if (type == MessageType::ok || type == MessageType::error)
        return true; // Replies to a bulk request inherit that request's frame type.
    return frame_type == FrameType::control;
}

void validate_frame_semantics(MessageType type, FrameType frame_type) {
    if (!class_allowed(type, frame_type))
        throw std::runtime_error("message used an invalid frame type");
}

FrameType more_urgent(FrameType a, FrameType b) {
    return frame_type_priority(a) <= frame_type_priority(b) ? a : b;
}

std::optional<FrameType> promotion_type(MessageType type) {
    if (type == MessageType::promote_foreground)
        return FrameType::foreground;
    if (type == MessageType::promote_read_ahead)
        return FrameType::read_ahead;
    return {};
}

RpcMessage transfer_control(MessageType type, uint64_t request_id) {
    Writer writer;
    writer.u64(request_id);
    return {type, writer.take()};
}

uint64_t transfer_target(const RpcMessage& message) {
    Reader reader(message.payload);
    auto id = reader.u64();
    reader.finish();
    if (!id)
        throw std::runtime_error("invalid transfer target");
    return id;
}

struct MessageAssembler {
    struct Partial {
        FrameType frame_type{FrameType::control};
        MessageType message_type{MessageType::error};
        Bytes payload;
    };

    std::map<uint64_t, Partial> partial;

    void promote(uint64_t request_id, FrameType type) {
        auto found = partial.find(request_id);
        if (found != partial.end())
            found->second.frame_type = more_urgent(type, found->second.frame_type);
    }

    void discard(uint64_t request_id) {
        partial.erase(request_id);
    }

    std::optional<RpcFrame> push(WireFragment fragment) {
        if (fragment.request_id == 0) {
            if (!fragment.first || !fragment.last)
                throw std::runtime_error("notification must fit one frame");
            return RpcFrame{0, fragment.frame_type,
                            {fragment.message_type, std::move(fragment.payload)}};
        }

        auto found = partial.find(fragment.request_id);
        if (fragment.first) {
            if (found != partial.end())
                throw std::runtime_error("duplicate first fragment");
            Partial item;
            item.frame_type = fragment.frame_type;
            item.message_type = fragment.message_type;
            found = partial.emplace(fragment.request_id, std::move(item)).first;
        } else if (found == partial.end()) {
            throw std::runtime_error("continuation without first fragment");
        }

        if (found->second.message_type != fragment.message_type)
            throw std::runtime_error("message type changed during transfer");
        if (frame_type_priority(fragment.frame_type) >
            frame_type_priority(found->second.frame_type)) {
            throw std::runtime_error("frame type was demoted during transfer");
        }
        found->second.frame_type = more_urgent(fragment.frame_type, found->second.frame_type);
        if (found->second.payload.size() + fragment.payload.size() > max_message_size)
            throw std::runtime_error("RPC message too large");
        found->second.payload.insert(found->second.payload.end(), fragment.payload.begin(),
                                     fragment.payload.end());

        if (!fragment.last)
            return {};

        RpcFrame complete{fragment.request_id,
                          found->second.frame_type,
                          {found->second.message_type, std::move(found->second.payload)}};
        partial.erase(found);
        return complete;
    }
};

} // namespace

const char* frame_type_name(FrameType type) noexcept {
    switch (type) {
    case FrameType::control:
        return "control";
    case FrameType::foreground:
        return "foreground";
    case FrameType::read_ahead:
        return "read-ahead";
    case FrameType::speculative:
        return "speculative";
    }
    return "unknown";
}

unsigned frame_type_priority(FrameType type) noexcept {
    return static_cast<unsigned>(type);
}

FrameType default_frame_type(MessageType type) noexcept {
    if (type == MessageType::get_object || type == MessageType::put_object)
        return FrameType::foreground;
    return FrameType::control;
}

SecureChannel::SecureChannel(int fd, ClusterKeys keys, NodeInfo local, size_t max_frame_size)
    : fd_(fd), keys_(keys), local_(std::move(local)), configured_max_frame_size_(max_frame_size) {
    validate_frame_limit(configured_max_frame_size_);
    socket_options(fd_);
}

SecureChannel::~SecureChannel() {
    close_fd();
}

void SecureChannel::close_fd() {
    std::lock_guard lock(close_mutex_);
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        close(fd_);
        fd_ = -1;
        shutdown_ = true;
    }
}

void SecureChannel::shutdown() {
    std::lock_guard lock(close_mutex_);
    if (fd_ >= 0 && !shutdown_) {
        ::shutdown(fd_, SHUT_RDWR);
        shutdown_ = true;
    }
}

NodeInfo SecureChannel::client_handshake() {
    auto nonce_bytes = random_bytes(32);
    std::array<uint8_t, 32> client_nonce{};
    std::copy(nonce_bytes.begin(), nonce_bytes.end(), client_nonce.begin());
    session_id_ = client_nonce;
    auto ephemeral = x25519_generate();

    Writer hello_writer;
    hello_writer.u16(protocol_version);
    hello_writer.u32(static_cast<uint32_t>(configured_max_frame_size_));
    hello_writer.fixed(keys_.cluster_id);
    hello_writer.fixed(local_.id.bytes);
    hello_writer.fixed(client_nonce);
    hello_writer.fixed(ephemeral.public_key);
    encode_node_info(hello_writer, local_);
    auto hello = hello_writer.take();

    Writer envelope;
    envelope.bytes(hello);
    envelope.fixed(hmac_sha256(keys_.auth, label("client/v7", hello)));
    send_blob(fd_, envelope.data());

    auto response = recv_blob(fd_, 16384);
    Reader response_reader(response);
    auto ack = response_reader.bytes(16384);
    auto remote_mac = response_reader.fixed<32>();
    response_reader.finish();

    auto authenticated = label("server/v7", hello);
    authenticated.insert(authenticated.end(), ack.begin(), ack.end());
    if (!constant_time_equal(remote_mac, hmac_sha256(keys_.auth, authenticated)))
        throw std::runtime_error("peer auth failed");

    Reader reader(ack);
    if (reader.u16() != protocol_version)
        throw std::runtime_error("wrong cluster/protocol");
    auto negotiated = static_cast<size_t>(reader.u32());
    validate_frame_limit(negotiated);
    if (negotiated > configured_max_frame_size_)
        throw std::runtime_error("peer negotiated an invalid frame size");
    if (reader.fixed<16>() != keys_.cluster_id)
        throw std::runtime_error("wrong cluster/protocol");
    NodeId server_id{reader.fixed<16>()};
    auto server_nonce = reader.fixed<32>();
    auto server_public = reader.fixed<32>();
    auto peer = decode_node_info(reader);
    reader.finish();
    if (peer.id != server_id)
        throw std::runtime_error("peer identity mismatch");

    auto shared = x25519_shared(ephemeral.private_key, server_public);
    OPENSSL_cleanse(ephemeral.private_key.data(), ephemeral.private_key.size());
    Writer transcript_writer;
    transcript_writer.bytes(hello);
    transcript_writer.bytes(ack);
    auto transcript = transcript_writer.take();

    Writer client_info;
    client_info.raw(session_info(transcript, local_.id, peer.id, "c2s"));
    client_info.fixed(client_nonce);
    client_info.fixed(server_nonce);
    Writer server_info;
    server_info.raw(session_info(transcript, local_.id, peer.id, "s2c"));
    server_info.fixed(client_nonce);
    server_info.fixed(server_nonce);
    tx_ = hkdf_sha256(shared, keys_.auth, client_info.data());
    rx_ = hkdf_sha256(shared, keys_.auth, server_info.data());
    OPENSSL_cleanse(shared.data(), shared.size());
    negotiated_max_frame_size_ = negotiated;
    ready_ = true;
    return peer;
}

NodeInfo SecureChannel::server_handshake(const std::string& remote_host) {
    auto envelope = recv_blob(fd_, 16384);
    Reader envelope_reader(envelope);
    auto hello = envelope_reader.bytes(16384);
    auto remote_mac = envelope_reader.fixed<32>();
    envelope_reader.finish();

    if (!constant_time_equal(remote_mac, hmac_sha256(keys_.auth, label("client/v7", hello))))
        throw std::runtime_error("client auth failed");

    Reader reader(hello);
    if (reader.u16() != protocol_version)
        throw std::runtime_error("wrong cluster/protocol");
    auto peer_max = static_cast<size_t>(reader.u32());
    validate_frame_limit(peer_max);
    if (reader.fixed<16>() != keys_.cluster_id)
        throw std::runtime_error("wrong cluster/protocol");
    NodeId client_id{reader.fixed<16>()};
    auto client_nonce = reader.fixed<32>();
    session_id_ = client_nonce;
    auto client_public = reader.fixed<32>();
    auto peer = decode_node_info(reader);
    reader.finish();
    if (peer.id != client_id)
        throw std::runtime_error("client identity mismatch");
    if (peer.host.empty() || peer.host == "0.0.0.0" || peer.host == "::")
        peer.host = remote_host;

    negotiated_max_frame_size_ = std::min(peer_max, configured_max_frame_size_);
    validate_frame_limit(negotiated_max_frame_size_);

    auto nonce_bytes = random_bytes(32);
    std::array<uint8_t, 32> server_nonce{};
    std::copy(nonce_bytes.begin(), nonce_bytes.end(), server_nonce.begin());
    auto ephemeral = x25519_generate();

    Writer ack_writer;
    ack_writer.u16(protocol_version);
    ack_writer.u32(static_cast<uint32_t>(negotiated_max_frame_size_));
    ack_writer.fixed(keys_.cluster_id);
    ack_writer.fixed(local_.id.bytes);
    ack_writer.fixed(server_nonce);
    ack_writer.fixed(ephemeral.public_key);
    encode_node_info(ack_writer, local_);
    auto ack = ack_writer.take();

    auto authenticated = label("server/v7", hello);
    authenticated.insert(authenticated.end(), ack.begin(), ack.end());
    Writer response;
    response.bytes(ack);
    response.fixed(hmac_sha256(keys_.auth, authenticated));
    send_blob(fd_, response.data());

    auto shared = x25519_shared(ephemeral.private_key, client_public);
    OPENSSL_cleanse(ephemeral.private_key.data(), ephemeral.private_key.size());
    Writer transcript_writer;
    transcript_writer.bytes(hello);
    transcript_writer.bytes(ack);
    auto transcript = transcript_writer.take();

    Writer client_info;
    client_info.raw(session_info(transcript, peer.id, local_.id, "c2s"));
    client_info.fixed(client_nonce);
    client_info.fixed(server_nonce);
    Writer server_info;
    server_info.raw(session_info(transcript, peer.id, local_.id, "s2c"));
    server_info.fixed(client_nonce);
    server_info.fixed(server_nonce);
    rx_ = hkdf_sha256(shared, keys_.auth, client_info.data());
    tx_ = hkdf_sha256(shared, keys_.auth, server_info.data());
    OPENSSL_cleanse(shared.data(), shared.size());
    ready_ = true;
    return peer;
}

void SecureChannel::send_fragment(uint64_t request_id, FrameType frame_type,
                                  MessageType message_type, bool first, bool last,
                                  std::span<const uint8_t> payload,
                                  const std::function<void(size_t)>& progress) {
    if (!ready_ || !negotiated_max_frame_size_ || payload.size() > negotiated_max_frame_size_)
        throw std::runtime_error("bad secure send");
    validate_frame_semantics(message_type, frame_type);

    uint8_t flags = 0;
    if (first)
        flags |= frame_first;
    if (last)
        flags |= frame_last;

    Writer header;
    header.u32(frame_magic);
    header.u8(static_cast<uint8_t>(frame_type));
    header.u8(flags);
    header.u16(static_cast<uint16_t>(message_type));
    header.u32(static_cast<uint32_t>(payload.size()));
    header.u64(++tx_counter_);
    header.u64(request_id);
    auto sealed = aes_gcm_seal(tx_, payload, header.data());
    send_all(fd_, header.data(), progress);
    send_all(fd_, sealed.nonce, progress);
    send_all(fd_, sealed.tag, progress);
    send_all(fd_, sealed.ciphertext, progress);
}

WireFragment SecureChannel::receive_fragment(
    const std::function<void(uint64_t, size_t)>& progress) {
    std::array<uint8_t, frame_header_size> header_bytes{};
    recv_all(fd_, header_bytes, [&](size_t count) {
        if (progress)
            progress(0, count);
    });
    Reader header(header_bytes);
    auto magic = header.u32();
    if (magic != frame_magic)
        throw std::runtime_error("bad frame magic " + std::to_string(magic) + " expected " +
                                 std::to_string(frame_magic));
    const auto frame_type = decode_frame_type(header.u8());
    const auto flags = header.u8();
    if (flags & ~(frame_first | frame_last))
        throw std::runtime_error("bad frame flags");
    const auto message_type = static_cast<MessageType>(header.u16());
    auto size = header.u32();
    auto counter = header.u64();
    auto request_id = header.u64();
    if (progress)
        progress(request_id, 0);
    if (size > negotiated_max_frame_size_ || counter != rx_counter_ + 1)
        throw std::runtime_error("bad frame sequence");
    validate_frame_semantics(message_type, frame_type);

    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    auto body_progress = [&](size_t count) {
        if (progress)
            progress(request_id, count);
    };
    recv_all(fd_, nonce, body_progress);
    recv_all(fd_, tag, body_progress);
    Bytes ciphertext(size);
    recv_all(fd_, ciphertext, body_progress);
    auto payload = aes_gcm_open(rx_, nonce, tag, ciphertext, header_bytes);
    rx_counter_ = counter;
    return {request_id, frame_type, message_type, (flags & frame_first) != 0,
            (flags & frame_last) != 0, std::move(payload)};
}

AsyncRpc::AsyncRpc(std::future<RpcReply> future, std::function<void()> cancel,
                   std::function<void()> abort, std::function<void(FrameType)> promote,
                   std::function<std::chrono::milliseconds()> idle)
    : future_(std::move(future)), cancel_(std::move(cancel)), abort_(std::move(abort)),
      promote_(std::move(promote)), idle_(std::move(idle)) {}

AsyncRpc::AsyncRpc(AsyncRpc&& other) noexcept
    : future_(std::move(other.future_)), cancel_(std::move(other.cancel_)),
      abort_(std::move(other.abort_)), promote_(std::move(other.promote_)),
      idle_(std::move(other.idle_)) {
    other.cancel_ = {};
    other.abort_ = {};
    other.promote_ = {};
    other.idle_ = {};
}

AsyncRpc& AsyncRpc::operator=(AsyncRpc&& other) noexcept {
    if (this == &other)
        return *this;
    cancel();
    future_ = std::move(other.future_);
    cancel_ = std::move(other.cancel_);
    abort_ = std::move(other.abort_);
    promote_ = std::move(other.promote_);
    idle_ = std::move(other.idle_);
    other.cancel_ = {};
    other.abort_ = {};
    other.promote_ = {};
    other.idle_ = {};
    return *this;
}

AsyncRpc::~AsyncRpc() {
    cancel();
}

bool AsyncRpc::valid() const {
    return future_.valid();
}

std::future_status AsyncRpc::wait_for(std::chrono::milliseconds timeout) {
    return future_.wait_for(timeout);
}

RpcReply AsyncRpc::get() {
    cancel_ = {};
    abort_ = {};
    promote_ = {};
    idle_ = {};
    return future_.get();
}

void AsyncRpc::cancel() {
    if (cancel_) {
        auto cancel = std::move(cancel_);
        abort_ = {};
        promote_ = {};
        idle_ = {};
        cancel();
    }
}

void AsyncRpc::abort() {
    if (abort_) {
        auto abort = std::move(abort_);
        cancel_ = {};
        promote_ = {};
        idle_ = {};
        abort();
    } else {
        cancel();
    }
}

void AsyncRpc::promote(FrameType type) {
    if (promote_)
        promote_(type);
}

std::chrono::milliseconds AsyncRpc::idle_for() const {
    if (!idle_)
        return std::chrono::milliseconds::max();
    return idle_();
}

class RpcClient::PeerConnection : public std::enable_shared_from_this<RpcClient::PeerConnection> {
    struct Pending {
        std::promise<RpcReply> promise;
        Clock::time_point started{Clock::now()};
        std::atomic<int64_t> last_progress_ns{steady_ns()};
    };

    struct Outbound {
        uint64_t request_id{};
        FrameType frame_type{FrameType::control};
        RpcMessage message;
        size_t offset{};
        bool reply{};
        std::shared_ptr<std::promise<void>> sent;
    };

    SecureChannel channel_;
    NodeInfo peer_;
    std::function<void(const NodeInfo&)> peer_observer_;
    std::function<void(uint64_t)> metadata_observer_;
    InboundHandler inbound_handler_;
    InboundPromoter inbound_promoter_;
    InboundCanceller inbound_canceller_;
    std::function<void(bool, std::chrono::milliseconds)> result_observer_;

    std::mutex admission_mutex_;
    std::mutex pending_mutex_;
    std::map<uint64_t, std::shared_ptr<Pending>> pending_;
    std::mutex outbound_mutex_;
    std::condition_variable outbound_cv_;
    std::deque<Outbound> outbound_;
    std::map<uint64_t, FrameType> inbound_classes_; // guarded by outbound_mutex_
    std::set<uint64_t> cancelled_inbound_;          // guarded by outbound_mutex_
    std::atomic_uint64_t next_request_{1};           // TCP dialler owns odd request IDs.
    std::atomic_bool broken_{};
    std::atomic_bool retiring_{};
    std::atomic_bool peer_retired_{};
    bool retire_notice_sent_{}; // guarded by outbound_mutex_
    std::atomic_uint64_t inbound_active_{};
    std::atomic_uint64_t writes_active_{};
    std::jthread reader_;
    std::jthread writer_;

    void touch(uint64_t request_id) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(pending_mutex_);
            auto found = pending_.find(request_id);
            if (found == pending_.end())
                return;
            pending = found->second;
        }
        pending->last_progress_ns.store(steady_ns(), std::memory_order_relaxed);
    }

    std::chrono::milliseconds idle_for(uint64_t request_id) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(pending_mutex_);
            auto found = pending_.find(request_id);
            if (found == pending_.end())
                return std::chrono::milliseconds(0);
            pending = found->second;
        }
        const auto idle_ns = std::max<int64_t>(
            0, steady_ns() - pending->last_progress_ns.load(std::memory_order_relaxed));
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(idle_ns));
    }

    void fail_all(const std::string& text) {
        std::map<uint64_t, std::shared_ptr<Pending>> pending;
        {
            std::lock_guard lock(pending_mutex_);
            pending.swap(pending_);
        }
        for (auto& [_, item] : pending) {
            result_observer_(false, std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now() - item->started));
            try {
                item->promise.set_exception(rpc_error(text));
            } catch (...) {
            }
        }
    }

    bool drained() {
        std::scoped_lock lock(pending_mutex_, outbound_mutex_);
        return pending_.empty() && outbound_.empty() && inbound_active_.load() == 0 &&
               writes_active_.load() == 0;
    }

    void finish_retire_if_drained() {
        if (retiring_.load() && peer_retired_.load() && drained())
            close();
    }

    void maybe_queue_retire_locked() {
        if (!retiring_.load() || retire_notice_sent_ || !outbound_.empty() ||
            writes_active_.load() != 0)
            return;
        retire_notice_sent_ = true;
        outbound_.push_back({0, FrameType::control, {MessageType::session_retire, {}}, 0, false, {}});
    }

    bool queue_message(uint64_t request_id, FrameType frame_type, const RpcMessage& message,
                       bool reply, std::shared_ptr<std::promise<void>> sent = {}) {
        validate_frame_semantics(message.type, frame_type);
        {
            std::lock_guard lock(outbound_mutex_);
            if (broken_.load())
                throw std::runtime_error("peer channel is closed");
            if (reply) {
                if (cancelled_inbound_.erase(request_id)) {
                    inbound_classes_.erase(request_id);
                    return false;
                }
                if (auto found = inbound_classes_.find(request_id); found != inbound_classes_.end())
                    frame_type = more_urgent(found->second, frame_type);
            }
            if (outbound_.size() >= max_peer_outbound)
                throw std::runtime_error("peer outbound queue full");
            outbound_.push_back({request_id,
                                 frame_type,
                                 {message.type, Bytes(message.payload.begin(), message.payload.end())},
                                 0,
                                 reply,
                                 std::move(sent)});
        }
        outbound_cv_.notify_one();
        return true;
    }

    void queue_control(const RpcMessage& message) {
        (void)queue_message(0, FrameType::control, message, false);
    }

    void register_inbound(uint64_t request_id, FrameType type) {
        std::lock_guard lock(outbound_mutex_);
        inbound_classes_[request_id] = type;
    }

    void promote_inbound(uint64_t request_id, FrameType type) {
        if (type != FrameType::foreground && type != FrameType::read_ahead)
            return;
        {
            std::lock_guard lock(outbound_mutex_);
            auto found = inbound_classes_.find(request_id);
            if (found != inbound_classes_.end())
                found->second = more_urgent(type, found->second);
            for (auto& item : outbound_) {
                if (item.reply && item.request_id == request_id)
                    item.frame_type = more_urgent(type, item.frame_type);
            }
        }
        outbound_cv_.notify_one();
    }

    void cancel_inbound(uint64_t request_id) {
        std::lock_guard lock(outbound_mutex_);
        cancelled_inbound_.insert(request_id);
        inbound_classes_.erase(request_id);
        std::erase_if(outbound_, [&](const Outbound& item) {
            return item.reply && item.request_id == request_id;
        });
    }

    void promote_outgoing(uint64_t request_id, FrameType type) {
        MessageType control_type{};
        if (type == FrameType::foreground)
            control_type = MessageType::promote_foreground;
        else if (type == FrameType::read_ahead)
            control_type = MessageType::promote_read_ahead;
        else
            return;

        {
            std::lock_guard lock(outbound_mutex_);
            for (auto& item : outbound_) {
                if (!item.reply && item.request_id == request_id)
                    item.frame_type = more_urgent(type, item.frame_type);
            }
        }
        try {
            queue_control(transfer_control(control_type, request_id));
        } catch (...) {
        }
    }

    void cancel_outgoing(uint64_t request_id) {
        {
            std::lock_guard lock(outbound_mutex_);
            std::erase_if(outbound_, [&](const Outbound& item) {
                return !item.reply && item.request_id == request_id;
            });
        }
        try {
            queue_control(transfer_control(MessageType::cancel_transfer, request_id));
        } catch (...) {
        }
    }

    void dispatch_request(RpcFrame frame) {
        if (!inbound_handler_)
            throw std::runtime_error("no inbound RPC handler installed");

        register_inbound(frame.request_id, frame.frame_type);
        ++inbound_active_;
        std::weak_ptr<PeerConnection> weak = shared_from_this();
        const auto id = frame.request_id;
        const auto fallback = frame.frame_type;
        try {
            inbound_handler_(peer_, std::move(frame), [weak, id, fallback](const RpcMessage& reply) {
                if (auto self = weak.lock()) {
                    try {
                        (void)self->queue_message(id, fallback, reply, true);
                    } catch (...) {
                        self->close();
                    }
                    --self->inbound_active_;
                    self->finish_retire_if_drained();
                }
            });
        } catch (...) {
            {
                std::lock_guard lock(outbound_mutex_);
                inbound_classes_.erase(id);
            }
            --inbound_active_;
            finish_retire_if_drained();
            throw;
        }
    }

    std::deque<Outbound>::iterator best_outbound_locked() {
        return std::min_element(outbound_.begin(), outbound_.end(), [](const Outbound& a,
                                                                      const Outbound& b) {
            return frame_type_priority(a.frame_type) < frame_type_priority(b.frame_type);
        });
    }

    void writer_loop(std::stop_token stop) {
        try {
            while (true) {
                Outbound item;
                {
                    std::unique_lock lock(outbound_mutex_);
                    maybe_queue_retire_locked();
                    outbound_cv_.wait(lock, [&] {
                        maybe_queue_retire_locked();
                        return stop.stop_requested() || broken_.load() || !outbound_.empty();
                    });
                    if ((stop.stop_requested() || broken_.load()) && outbound_.empty())
                        return;
                    if (broken_.load())
                        return;
                    auto best = best_outbound_locked();
                    item = std::move(*best);
                    outbound_.erase(best);
                    if (item.reply) {
                        if (auto found = inbound_classes_.find(item.request_id);
                            found != inbound_classes_.end())
                            item.frame_type = more_urgent(found->second, item.frame_type);
                    }
                }

                const auto& payload = item.message.payload;
                const size_t remaining = payload.size() - item.offset;
                const size_t amount = std::min(remaining, channel_.max_frame_size());
                const bool first = item.offset == 0;
                const bool last = remaining <= channel_.max_frame_size();
                std::span<const uint8_t> fragment;
                if (amount)
                    fragment = std::span<const uint8_t>(payload).subspan(item.offset, amount);

                ++writes_active_;
                try {
                    channel_.send_fragment(item.request_id, item.frame_type, item.message.type, first,
                                           last, fragment, [this, id = item.request_id](size_t) {
                                               if (id)
                                                   touch(id);
                                           });
                } catch (...) {
                    --writes_active_;
                    throw;
                }
                --writes_active_;

                if (!last) {
                    item.offset += amount;
                    std::lock_guard lock(outbound_mutex_);
                    if (item.reply) {
                        if (cancelled_inbound_.contains(item.request_id)) {
                            cancelled_inbound_.erase(item.request_id);
                            inbound_classes_.erase(item.request_id);
                        } else {
                            if (auto found = inbound_classes_.find(item.request_id);
                                found != inbound_classes_.end())
                                item.frame_type = more_urgent(found->second, item.frame_type);
                            outbound_.push_back(std::move(item));
                        }
                    } else {
                        outbound_.push_back(std::move(item));
                    }
                    maybe_queue_retire_locked();
                    outbound_cv_.notify_one();
                } else {
                    if (item.reply) {
                        std::lock_guard lock(outbound_mutex_);
                        inbound_classes_.erase(item.request_id);
                        cancelled_inbound_.erase(item.request_id);
                        maybe_queue_retire_locked();
                    }
                    if (item.sent) {
                        try {
                            item.sent->set_value();
                        } catch (...) {
                        }
                    }
                }
                finish_retire_if_drained();
            }
        } catch (const std::exception& error) {
            if (!broken_.exchange(true))
                Log::debug(std::string("peer writer: ") + error.what());
            channel_.shutdown();
            fail_all(error.what());
            {
                std::lock_guard lock(outbound_mutex_);
                for (auto& item : outbound_) {
                    if (!item.sent)
                        continue;
                    try {
                        item.sent->set_exception(rpc_error(error.what()));
                    } catch (...) {
                    }
                }
                outbound_.clear();
            }
            outbound_cv_.notify_all();
        }
    }

    void peer_retire() {
        peer_retired_ = true;
        retire();
        finish_retire_if_drained();
    }

    void reader_loop(std::stop_token stop) {
        MessageAssembler assembler;
        try {
            while (!stop.stop_requested()) {
                auto fragment = channel_.receive_fragment([this](uint64_t request_id, size_t) {
                    if (request_id && (request_id & 1U))
                        touch(request_id);
                });
                auto frame = assembler.push(std::move(fragment));
                if (!frame)
                    continue;

                if (frame->request_id == 0) {
                    if (frame->message.type == MessageType::metadata_notice) {
                        Reader reader(frame->message.payload);
                        const auto generation = reader.u64();
                        reader.finish();
                        metadata_observer_(generation);
                    } else if (frame->message.type == MessageType::session_retire) {
                        peer_retire();
                    } else if (auto promoted = promotion_type(frame->message.type)) {
                        const auto target = transfer_target(frame->message);
                        assembler.promote(target, *promoted);
                        promote_inbound(target, *promoted);
                        if (inbound_promoter_)
                            inbound_promoter_(peer_, target, *promoted);
                    } else if (frame->message.type == MessageType::cancel_transfer) {
                        const auto target = transfer_target(frame->message);
                        assembler.discard(target);
                        cancel_inbound(target);
                        if (inbound_canceller_)
                            inbound_canceller_(peer_, target);
                    }
                    continue;
                }

                if ((frame->request_id & 1U) == 0) {
                    dispatch_request(std::move(*frame));
                    continue;
                }

                std::shared_ptr<Pending> pending;
                {
                    std::lock_guard lock(pending_mutex_);
                    auto found = pending_.find(frame->request_id);
                    if (found == pending_.end())
                        continue;
                    pending = std::move(found->second);
                    pending_.erase(found);
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - pending->started);
                result_observer_(true, elapsed);
                pending->promise.set_value({peer_, std::move(frame->message)});
                finish_retire_if_drained();
            }
        } catch (const std::exception& error) {
            if (!broken_.exchange(true))
                Log::debug(std::string("peer channel: ") + error.what());
            channel_.shutdown();
            fail_all(error.what());
            outbound_cv_.notify_all();
        }
    }

  public:
    PeerConnection(int fd, ClusterKeys keys, NodeInfo local, size_t max_frame_size,
                   std::function<void(const NodeInfo&)> peer_observer,
                   std::function<void(uint64_t)> metadata_observer,
                   InboundHandler inbound_handler, InboundPromoter inbound_promoter,
                   InboundCanceller inbound_canceller,
                   std::function<void(bool, std::chrono::milliseconds)> result_observer)
        : channel_(fd, keys, std::move(local), max_frame_size),
          peer_observer_(std::move(peer_observer)),
          metadata_observer_(std::move(metadata_observer)),
          inbound_handler_(std::move(inbound_handler)),
          inbound_promoter_(std::move(inbound_promoter)),
          inbound_canceller_(std::move(inbound_canceller)),
          result_observer_(std::move(result_observer)) {
        peer_ = channel_.client_handshake();
        peer_observer_(peer_);
        reader_ = std::jthread([this](std::stop_token stop) { reader_loop(stop); });
        writer_ = std::jthread([this](std::stop_token stop) { writer_loop(stop); });
    }

    ~PeerConnection() {
        close();
        if (writer_.joinable())
            writer_.join();
        if (reader_.joinable())
            reader_.join();
    }

    bool usable() const { return !broken_.load() && !retiring_.load(); }
    bool finished() const { return broken_.load(); }
    const NodeInfo& peer() const { return peer_; }
    const std::array<uint8_t, 32>& session_id() const noexcept { return channel_.session_id(); }

    AsyncRpc call(MessageType type, std::span<const uint8_t> payload, FrameType frame_type) {
        validate_frame_semantics(type, frame_type);

        uint64_t id = 0;
        auto pending = std::make_shared<Pending>();
        auto future = pending->promise.get_future();
        {
            std::lock_guard admission(admission_mutex_);
            if (broken_.load())
                throw std::runtime_error("peer channel is closed");
            if (retiring_.load())
                throw std::runtime_error("peer channel is retiring");
            id = next_request_.fetch_add(2);
            if (!id)
                throw std::runtime_error("RPC request id exhausted");
            {
                std::lock_guard lock(pending_mutex_);
                pending_.emplace(id, pending);
            }
            try {
                (void)queue_message(id, frame_type,
                                    {type, Bytes(payload.begin(), payload.end())}, false);
            } catch (...) {
                std::lock_guard lock(pending_mutex_);
                pending_.erase(id);
                throw;
            }
        }

        std::weak_ptr<PeerConnection> weak = shared_from_this();
        auto cancel = [weak, id] {
            if (auto self = weak.lock()) {
                bool existed = false;
                {
                    std::lock_guard lock(self->pending_mutex_);
                    existed = self->pending_.erase(id) != 0;
                }
                if (existed)
                    self->cancel_outgoing(id);
                self->finish_retire_if_drained();
            }
        };
        auto abort = [weak, id] {
            if (auto self = weak.lock()) {
                {
                    std::lock_guard lock(self->pending_mutex_);
                    self->pending_.erase(id);
                }
                self->close();
            }
        };
        auto promote = [weak, id](FrameType frame_type) {
            if (auto self = weak.lock())
                self->promote_outgoing(id, frame_type);
        };
        auto idle = [weak, id] {
            if (auto self = weak.lock())
                return self->idle_for(id);
            return std::chrono::milliseconds::max();
        };
        return AsyncRpc(std::move(future), std::move(cancel), std::move(abort),
                        std::move(promote), std::move(idle));
    }

    void notify(const RpcMessage& message) {
        if (!usable())
            return;
        auto sent = std::make_shared<std::promise<void>>();
        auto future = sent->get_future();
        if (queue_message(0, FrameType::control, message, false, sent))
            future.get();
    }

    void retire() {
        {
            std::lock_guard admission(admission_mutex_);
            bool expected = false;
            if (!retiring_.compare_exchange_strong(expected, true))
                return;
        }
        {
            std::lock_guard lock(outbound_mutex_);
            maybe_queue_retire_locked();
        }
        outbound_cv_.notify_all();
        finish_retire_if_drained();
    }

    void close() {
        if (!broken_.exchange(true)) {
            channel_.shutdown();
            fail_all("peer channel closed");
        }
        {
            std::lock_guard lock(outbound_mutex_);
            for (auto& item : outbound_) {
                if (item.sent) {
                    try {
                        item.sent->set_exception(rpc_error("peer channel closed"));
                    } catch (...) {
                    }
                }
            }
            outbound_.clear();
        }
        if (reader_.joinable())
            reader_.request_stop();
        if (writer_.joinable())
            writer_.request_stop();
        outbound_cv_.notify_all();
    }
};

RpcClient::RpcClient(ClusterKeys keys, std::function<NodeInfo()> local,
                     std::function<void(const NodeInfo&)> peer_observer,
                     std::function<void(uint64_t)> metadata_observer,
                     std::chrono::milliseconds connect_timeout,
                     std::chrono::milliseconds heartbeat,
                     std::chrono::milliseconds dead_after, size_t max_frame_size)
    : keys_(keys), local_(std::move(local)), peer_observer_(std::move(peer_observer)),
      metadata_observer_(std::move(metadata_observer)), connect_timeout_(connect_timeout),
      heartbeat_(heartbeat), dead_after_(dead_after), max_frame_size_(max_frame_size) {
    validate_frame_limit(max_frame_size_);
    health_thread_ = std::jthread([this](std::stop_token stop) { health_loop(stop); });
}

RpcClient::~RpcClient() {
    stop();
}

std::string RpcClient::endpoint_key(const Endpoint& endpoint) {
    return endpoint.host + ':' + std::to_string(endpoint.port);
}

std::string RpcClient::peer_key(const NodeId& peer) {
    return to_string(peer);
}

void RpcClient::set_inbound_handler(InboundHandler handler) {
    std::lock_guard lock(inbound_mutex_);
    inbound_handler_ = std::move(handler);
}

void RpcClient::dispatch_inbound(const NodeInfo& peer, RpcFrame frame, InboundReply reply) {
    std::lock_guard lock(inbound_mutex_);
    if (!inbound_handler_)
        throw std::runtime_error("no inbound RPC handler installed");
    inbound_handler_(peer, std::move(frame), std::move(reply));
}

void RpcClient::set_inbound_transfer_control(InboundPromoter promoter, InboundCanceller canceller) {
    std::lock_guard lock(inbound_mutex_);
    inbound_promoter_ = std::move(promoter);
    inbound_canceller_ = std::move(canceller);
}

void RpcClient::dispatch_inbound_promotion(const NodeInfo& peer, uint64_t request_id,
                                           FrameType type) {
    std::lock_guard lock(inbound_mutex_);
    if (inbound_promoter_)
        inbound_promoter_(peer, request_id, type);
}

void RpcClient::dispatch_inbound_cancel(const NodeInfo& peer, uint64_t request_id) {
    std::lock_guard lock(inbound_mutex_);
    if (inbound_canceller_)
        inbound_canceller_(peer, request_id);
}

void RpcClient::observe_result(const std::string& connection_key, bool success,
                               std::chrono::milliseconds) {
    std::lock_guard lock(mutex_);
    auto& health = health_[connection_key];
    if (success) {
        health.failures = 0;
        health.retry_after = {};
        return;
    }

    health.failures = std::min(health.failures + 1, 8U);
    auto shift = std::min(health.failures - 1, 4U);
    auto backoff = std::chrono::milliseconds(250 * (1U << shift));
    health.retry_after = Clock::now() + backoff;
}

void RpcClient::reap_retired() {
    std::vector<std::shared_ptr<PeerConnection>> reap;
    {
        std::lock_guard lock(mutex_);
        for (auto it = retired_connections_.begin(); it != retired_connections_.end();) {
            if (*it && (*it)->finished()) {
                reap.push_back(std::move(*it));
                it = retired_connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    reap.clear();
}

void RpcClient::reconcile_locked(const NodeId& peer,
                                 std::vector<std::function<void()>>& retire) {
    const auto k = peer_key(peer);
    auto outbound = connections_.find(k);
    auto inbound = inbound_routes_.find(k);
    const bool have_outbound = outbound != connections_.end() && outbound->second &&
                               outbound->second->usable();
    const bool have_inbound = inbound != inbound_routes_.end() && inbound->second.usable &&
                              inbound->second.usable();
    if (!have_outbound || !have_inbound)
        return;

    if (local_().id < peer) {
        auto fn = inbound->second.retire;
        inbound_routes_.erase(inbound);
        if (fn)
            retire.push_back(std::move(fn));
    } else {
        auto connection = std::move(outbound->second);
        connections_.erase(outbound);
        retired_connections_.push_back(connection);
        retire.push_back([connection] { connection->retire(); });
    }
}

void RpcClient::register_inbound(InboundRoute route) {
    reap_retired();
    const auto peer = route.peer.id;
    const auto k = peer_key(peer);
    std::vector<std::function<void()>> retire;
    {
        std::lock_guard lock(mutex_);
        if (!route.peer.host.empty() && route.peer.port) {
            Endpoint advertised{route.peer.host, route.peer.port};
            endpoints_[endpoint_key(advertised)] = advertised;
            endpoint_peers_[endpoint_key(advertised)] = peer;
        }

        auto found = inbound_routes_.find(k);
        if (found == inbound_routes_.end() || !found->second.usable ||
            !found->second.usable()) {
            inbound_routes_[k] = std::move(route);
        } else if (route.session_id < found->second.session_id) {
            if (found->second.retire)
                retire.push_back(found->second.retire);
            found->second = std::move(route);
        } else if (route.retire) {
            retire.push_back(route.retire);
        }
        reconcile_locked(peer, retire);
    }
    for (auto& fn : retire)
        fn();
}

void RpcClient::unregister_inbound(const NodeId& peer,
                                   const std::array<uint8_t, 32>& session_id) {
    std::lock_guard lock(mutex_);
    auto found = inbound_routes_.find(peer_key(peer));
    if (found != inbound_routes_.end() && found->second.session_id == session_id)
        inbound_routes_.erase(found);
}

std::shared_ptr<RpcClient::PeerConnection>
RpcClient::connection(const Endpoint& endpoint, const NodeId* expected, NodeId* actual) {
    reap_retired();
    const auto retry_key = endpoint_key(endpoint);
    std::optional<NodeId> known;
    {
        std::lock_guard lock(mutex_);
        if (expected)
            known = *expected;
        else if (auto p = endpoint_peers_.find(endpoint_key(endpoint)); p != endpoint_peers_.end())
            known = p->second;

        auto health = health_.find(retry_key);
        if (health != health_.end() && Clock::now() < health->second.retry_after)
            throw std::runtime_error("peer in retry backoff");

        if (known) {
            if (actual)
                *actual = *known;
            auto found = connections_.find(peer_key(*known));
            if (found != connections_.end() && found->second && found->second->usable()) {
                ++connections_reused_;
                return found->second;
            }
            auto inbound = inbound_routes_.find(peer_key(*known));
            if (inbound != inbound_routes_.end() && inbound->second.usable &&
                inbound->second.usable()) {
                ++connections_reused_;
                return {};
            }
        }
    }

    try {
        int fd = connect_socket(endpoint, connect_timeout_);
        auto fresh = std::make_shared<PeerConnection>(
            fd, keys_, local_(), max_frame_size_, peer_observer_, metadata_observer_,
            [this](const NodeInfo& peer, RpcFrame frame, InboundReply reply) {
                dispatch_inbound(peer, std::move(frame), std::move(reply));
            },
            [this](const NodeInfo& peer, uint64_t request_id, FrameType type) {
                dispatch_inbound_promotion(peer, request_id, type);
            },
            [this](const NodeInfo& peer, uint64_t request_id) {
                dispatch_inbound_cancel(peer, request_id);
            },
            [this, retry_key](bool success, std::chrono::milliseconds elapsed) {
                observe_result(retry_key, success, elapsed);
            });
        ++connections_created_;

        if (expected && fresh->peer().id != *expected) {
            fresh->close();
            throw std::runtime_error("RPC endpoint authenticated as the wrong node");
        }
        if (actual)
            *actual = fresh->peer().id;

        std::vector<std::function<void()>> retire;
        std::shared_ptr<PeerConnection> winner;
        bool installed = false;
        {
            std::lock_guard lock(mutex_);
            endpoints_[endpoint_key(endpoint)] = endpoint;
            endpoint_peers_[endpoint_key(endpoint)] = fresh->peer().id;
            if (!fresh->peer().host.empty() && fresh->peer().port) {
                Endpoint advertised{fresh->peer().host, fresh->peer().port};
                endpoints_[endpoint_key(advertised)] = advertised;
                endpoint_peers_[endpoint_key(advertised)] = fresh->peer().id;
            }

            const auto k = peer_key(fresh->peer().id);
            auto found = connections_.find(k);
            if (found == connections_.end() || !found->second || !found->second->usable()) {
                if (found != connections_.end() && found->second) {
                    retired_connections_.push_back(std::move(found->second));
                    found->second = fresh;
                } else if (found != connections_.end()) {
                    found->second = fresh;
                } else {
                    connections_.emplace(k, fresh);
                }
                installed = true;
            } else if (fresh->session_id() < found->second->session_id()) {
                auto loser = std::move(found->second);
                found->second = fresh;
                retired_connections_.push_back(loser);
                retire.push_back([loser] { loser->retire(); });
                installed = true;
            } else {
                auto loser = fresh;
                retired_connections_.push_back(loser);
                retire.push_back([loser] { loser->retire(); });
                ++connections_reused_;
            }

            reconcile_locked(fresh->peer().id, retire);
            auto canonical = connections_.find(k);
            if (canonical != connections_.end() && canonical->second && canonical->second->usable())
                winner = canonical->second;
        }

        for (auto& fn : retire)
            fn();

        if (installed && winner == fresh) {
            Log::info("node connection outbound peer=" +
                      to_string(fresh->peer().id).substr(0, 12) +
                      " endpoint=" + endpoint_key(endpoint));
        }
        return winner;
    } catch (...) {
        observe_result(retry_key, false, std::chrono::milliseconds(0));
        throw;
    }
}

AsyncRpc RpcClient::call_async_known(const Endpoint& endpoint, const NodeId* expected,
                                     MessageType type, std::span<const uint8_t> payload,
                                     FrameType frame_type) {
    validate_frame_semantics(type, frame_type);

    auto try_existing = [&](const NodeId& peer) -> std::optional<AsyncRpc> {
        std::shared_ptr<PeerConnection> outbound;
        std::function<AsyncRpc(MessageType, std::span<const uint8_t>, FrameType)> inbound;
        {
            std::lock_guard lock(mutex_);
            const auto k = peer_key(peer);
            auto out = connections_.find(k);
            if (out != connections_.end() && out->second && out->second->usable())
                outbound = out->second;
            if (!outbound) {
                auto in = inbound_routes_.find(k);
                if (in != inbound_routes_.end() && in->second.usable && in->second.usable())
                    inbound = in->second.call;
            }
        }
        if (outbound) {
            ++connections_reused_;
            try {
                return outbound->call(type, payload, frame_type);
            } catch (const std::exception&) {
            }
        }
        if (inbound) {
            ++connections_reused_;
            try {
                return inbound(type, payload, frame_type);
            } catch (const std::exception&) {
            }
        }
        return std::nullopt;
    };

    if (expected)
        if (auto existing = try_existing(*expected))
            return std::move(*existing);

    NodeId actual{};
    auto outbound = connection(endpoint, expected, &actual);
    if (outbound && outbound->usable()) {
        try {
            return outbound->call(type, payload, frame_type);
        } catch (const std::exception&) {
        }
    }
    if (auto existing = try_existing(actual))
        return std::move(*existing);
    throw std::runtime_error("no canonical RPC route to peer");
}

AsyncRpc RpcClient::call_async(const Endpoint& endpoint, MessageType type,
                               std::span<const uint8_t> payload) {
    return call_async_known(endpoint, nullptr, type, payload, default_frame_type(type));
}

AsyncRpc RpcClient::call_async(const NodeInfo& node, MessageType type,
                               std::span<const uint8_t> payload) {
    Endpoint endpoint{node.host, node.port};
    return call_async_known(endpoint, &node.id, type, payload, default_frame_type(type));
}

AsyncRpc RpcClient::call_async(const Endpoint& endpoint, MessageType type,
                               std::span<const uint8_t> payload, FrameType frame_type) {
    return call_async_known(endpoint, nullptr, type, payload, frame_type);
}

AsyncRpc RpcClient::call_async(const NodeInfo& node, MessageType type,
                               std::span<const uint8_t> payload, FrameType frame_type) {
    Endpoint endpoint{node.host, node.port};
    return call_async_known(endpoint, &node.id, type, payload, frame_type);
}

RpcReply RpcClient::call(const Endpoint& endpoint, MessageType type,
                         std::span<const uint8_t> payload,
                         std::chrono::milliseconds stall_notice) {
    return call(endpoint, type, payload, default_frame_type(type), stall_notice);
}

RpcReply RpcClient::call(const NodeInfo& node, MessageType type,
                         std::span<const uint8_t> payload,
                         std::chrono::milliseconds stall_notice) {
    return call(node, type, payload, default_frame_type(type), stall_notice);
}

RpcReply RpcClient::call(const Endpoint& endpoint, MessageType type,
                         std::span<const uint8_t> payload, FrameType frame_type,
                         std::chrono::milliseconds stall_notice) {
    auto async = call_async(endpoint, type, payload, frame_type);
    if (stall_notice.count() <= 0)
        return async.get();

    while (async.wait_for(stall_notice) != std::future_status::ready) {
        const auto idle = async.idle_for();
        if (idle >= stall_notice) {
            Log::debug(std::string("RPC stalled (") + frame_type_name(frame_type) + ") peer=" +
                       endpoint_key(endpoint) + " no_progress_ms=" +
                       std::to_string(idle.count()) +
                       "; request remains active while peer health is monitored");
        }
    }
    return async.get();
}

RpcReply RpcClient::call(const NodeInfo& node, MessageType type,
                         std::span<const uint8_t> payload, FrameType frame_type,
                         std::chrono::milliseconds stall_notice) {
    auto async = call_async(node, type, payload, frame_type);
    if (stall_notice.count() <= 0)
        return async.get();

    while (async.wait_for(stall_notice) != std::future_status::ready) {
        const auto idle = async.idle_for();
        if (idle >= stall_notice) {
            Log::debug(std::string("RPC stalled (") + frame_type_name(frame_type) + ") peer=" +
                       to_string(node.id).substr(0, 12) + " no_progress_ms=" +
                       std::to_string(idle.count()) +
                       "; request remains active while peer health is monitored");
        }
    }
    return async.get();
}

void RpcClient::close_endpoint(const Endpoint& endpoint, const std::string& reason) {
    std::vector<std::shared_ptr<PeerConnection>> outbound;
    std::vector<std::function<void()>> inbound;
    std::optional<NodeId> peer;
    {
        std::lock_guard lock(mutex_);
        auto known = endpoint_peers_.find(endpoint_key(endpoint));
        if (known != endpoint_peers_.end())
            peer = known->second;
        if (!peer)
            return;

        const auto k = peer_key(*peer);
        auto out = connections_.find(k);
        if (out != connections_.end()) {
            if (out->second)
                outbound.push_back(std::move(out->second));
            connections_.erase(out);
        }
        auto in = inbound_routes_.find(k);
        if (in != inbound_routes_.end()) {
            if (in->second.close)
                inbound.push_back(in->second.close);
            inbound_routes_.erase(in);
        }

        for (auto it = retired_connections_.begin(); it != retired_connections_.end();) {
            if (*it && (*it)->peer().id == *peer) {
                outbound.push_back(std::move(*it));
                it = retired_connections_.erase(it);
            } else {
                ++it;
            }
        }
    }

    Log::debug("peer " + endpoint_key(endpoint) + " liveness failure: " + reason);
    for (auto& close : inbound)
        close();
    for (auto& connection : outbound)
        connection->close();
}

void RpcClient::health_loop(std::stop_token stop) {
    struct Probe {
        NodeId peer;
        Endpoint endpoint;
        std::optional<AsyncRpc> rpc;
        Clock::time_point deadline;
        Clock::time_point next_attempt;
        std::string last_error;
        bool done{};
    };

    while (!stop.stop_requested()) {
        auto until = Clock::now() + heartbeat_;
        while (!stop.stop_requested() && Clock::now() < until)
            std::this_thread::sleep_for(std::min(std::chrono::milliseconds(50), heartbeat_));
        if (stop.stop_requested())
            return;

        reap_retired();
        std::map<NodeId, Endpoint> active_peers;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [base, endpoint] : endpoints_) {
                auto known = endpoint_peers_.find(base);
                if (known == endpoint_peers_.end())
                    continue;
                const auto& peer = known->second;
                const auto k = peer_key(peer);
                bool active = false;
                auto out = connections_.find(k);
                if (out != connections_.end() && out->second && out->second->usable())
                    active = true;
                auto in = inbound_routes_.find(k);
                if (!active && in != inbound_routes_.end() && in->second.usable &&
                    in->second.usable())
                    active = true;
                if (active)
                    active_peers.try_emplace(peer, endpoint);
            }
        }

        std::vector<Probe> probes;
        probes.reserve(active_peers.size());
        const auto started = Clock::now();
        for (const auto& [peer, endpoint] : active_peers)
            probes.push_back({peer, endpoint, std::nullopt, started + dead_after_, started, {}, false});

        size_t remaining = probes.size();
        while (remaining && !stop.stop_requested()) {
            bool progressed = false;
            const auto now = Clock::now();
            for (auto& probe : probes) {
                if (probe.done)
                    continue;

                if (now >= probe.deadline) {
                    if (probe.rpc)
                        probe.rpc->abort();
                    probe.done = true;
                    --remaining;
                    progressed = true;
                    close_endpoint(
                        probe.endpoint,
                        probe.last_error.empty()
                            ? "health could not be established before dead_after"
                            : "health unavailable until dead_after: " + probe.last_error);
                    continue;
                }

                if (probe.rpc) {
                    if (probe.rpc->wait_for(std::chrono::milliseconds(0)) !=
                        std::future_status::ready)
                        continue;
                    progressed = true;
                    try {
                        auto reply = probe.rpc->get();
                        probe.rpc.reset();
                        if (reply.message.type == MessageType::ok) {
                            peer_observer_(reply.peer);
                            probe.done = true;
                            --remaining;
                            continue;
                        }
                        probe.last_error = "health probe rejected";
                    } catch (const std::exception& error) {
                        probe.rpc.reset();
                        probe.last_error = error.what();
                    }
                    probe.next_attempt = now + std::chrono::milliseconds(50);
                    continue;
                }

                if (now < probe.next_attempt)
                    continue;
                try {
                    probe.rpc.emplace(call_async_known(probe.endpoint, &probe.peer,
                                                       MessageType::ping, {}, FrameType::control));
                    progressed = true;
                } catch (const std::exception& error) {
                    probe.last_error = error.what();
                    probe.next_attempt = now + std::chrono::milliseconds(50);
                }
            }
            if (!progressed)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (stop.stop_requested()) {
            for (auto& probe : probes)
                if (!probe.done && probe.rpc)
                    probe.rpc->cancel();
            return;
        }
    }
}

RpcStats RpcClient::stats() const {
    std::lock_guard lock(mutex_);
    RpcStats stats{connections_created_.load(), connections_reused_.load()};
    for (const auto& [_, connection] : connections_)
        if (connection && connection->usable())
            ++stats.canonical_connections;
    for (const auto& [_, route] : inbound_routes_)
        if (route.usable && route.usable())
            ++stats.canonical_connections;
    return stats;
}

void RpcClient::broadcast(const RpcMessage& message) {
    std::vector<std::shared_ptr<PeerConnection>> outbound;
    std::vector<std::function<void(const RpcMessage&)>> inbound;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, connection] : connections_)
            if (connection && connection->usable())
                outbound.push_back(connection);
        for (const auto& [_, route] : inbound_routes_)
            if (route.usable && route.usable() && route.notify)
                inbound.push_back(route.notify);
    }
    for (auto& connection : outbound) {
        try {
            connection->notify(message);
        } catch (...) {
        }
    }
    for (auto& notify : inbound) {
        try {
            notify(message);
        } catch (...) {
        }
    }
}

void RpcClient::stop() {
    Log::debug("shutdown: RpcClient::stop begin");
    if (health_thread_.joinable()) {
        health_thread_.request_stop();
        health_thread_.join();
    }
    set_inbound_handler({});
    set_inbound_transfer_control({}, {});

    std::vector<std::shared_ptr<PeerConnection>> connections;
    std::vector<std::function<void()>> inbound;
    {
        std::lock_guard lock(mutex_);
        for (auto& [_, connection] : connections_)
            if (connection)
                connections.push_back(std::move(connection));
        for (auto& connection : retired_connections_)
            if (connection)
                connections.push_back(std::move(connection));
        for (auto& [_, route] : inbound_routes_)
            if (route.close)
                inbound.push_back(route.close);
        connections_.clear();
        retired_connections_.clear();
        inbound_routes_.clear();
        endpoints_.clear();
        endpoint_peers_.clear();
        health_.clear();
    }
    for (auto& close : inbound)
        close();
    for (auto& connection : connections)
        connection->close();
    Log::debug("shutdown: RpcClient::stop complete connections=" +
               std::to_string(connections.size()));
}

struct RpcServer::Session : public std::enable_shared_from_this<RpcServer::Session> {
    struct Pending {
        std::promise<RpcReply> promise;
        std::atomic<int64_t> last_progress_ns{steady_ns()};
    };
    struct Outbound {
        uint64_t request_id{};
        FrameType frame_type{FrameType::control};
        RpcMessage message;
        size_t offset{};
        bool reply{};
        std::shared_ptr<std::promise<void>> sent;
    };

    std::unique_ptr<SecureChannel> channel;
    NodeInfo peer;
    std::string remote_host;
    std::atomic_bool ready{};
    std::atomic_bool done{};
    std::atomic_bool retiring{};
    std::atomic_bool peer_retired{};
    bool retire_notice_sent{}; // guarded by outbound_mutex
    std::atomic_uint64_t active_requests{};
    std::atomic_uint64_t writes_active{};

    std::mutex admission_mutex;
    std::mutex pending_mutex;
    std::map<uint64_t, std::shared_ptr<Pending>> pending;
    std::mutex outbound_mutex;
    std::condition_variable outbound_cv;
    std::deque<Outbound> outbound;
    std::map<uint64_t, FrameType> inbound_classes;
    std::set<uint64_t> cancelled_inbound;
    std::atomic_uint64_t next_request{2}; // TCP acceptor owns even request IDs.
    std::jthread reader;
    std::jthread writer;

    void fail_pending(const std::string& text) {
        std::map<uint64_t, std::shared_ptr<Pending>> failed;
        {
            std::lock_guard lock(pending_mutex);
            failed.swap(pending);
        }
        for (auto& [_, item] : failed) {
            try {
                item->promise.set_exception(rpc_error(text));
            } catch (...) {
            }
        }
    }

    bool drained() {
        std::scoped_lock lock(pending_mutex, outbound_mutex);
        return pending.empty() && outbound.empty() && active_requests.load() == 0 &&
               writes_active.load() == 0;
    }

    void maybe_finish_retire() {
        if (retiring.load() && peer_retired.load() && drained() && channel)
            channel->shutdown();
    }

    bool usable() const {
        return ready.load() && !done.load() && !retiring.load();
    }

    void maybe_queue_retire_locked() {
        if (!retiring.load() || retire_notice_sent || !outbound.empty() ||
            writes_active.load() != 0)
            return;
        retire_notice_sent = true;
        outbound.push_back({0, FrameType::control, {MessageType::session_retire, {}}, 0, false, {}});
    }

    bool queue_message(uint64_t request_id, FrameType frame_type, const RpcMessage& message,
                       bool reply, std::shared_ptr<std::promise<void>> sent = {}) {
        validate_frame_semantics(message.type, frame_type);
        {
            std::lock_guard lock(outbound_mutex);
            if (!ready.load() || done.load())
                throw std::runtime_error("accepted peer session is closed");
            if (reply) {
                if (cancelled_inbound.erase(request_id)) {
                    inbound_classes.erase(request_id);
                    return false;
                }
                if (auto found = inbound_classes.find(request_id); found != inbound_classes.end())
                    frame_type = more_urgent(found->second, frame_type);
            }
            if (outbound.size() >= max_peer_outbound)
                throw std::runtime_error("peer outbound queue full");
            outbound.push_back({request_id,
                                frame_type,
                                {message.type, Bytes(message.payload.begin(), message.payload.end())},
                                0,
                                reply,
                                std::move(sent)});
        }
        outbound_cv.notify_one();
        return true;
    }

    void queue_control(const RpcMessage& message) {
        (void)queue_message(0, FrameType::control, message, false);
    }

    void register_inbound(uint64_t request_id, FrameType type) {
        std::lock_guard lock(outbound_mutex);
        inbound_classes[request_id] = type;
    }

    void promote_inbound(uint64_t request_id, FrameType type) {
        if (type != FrameType::foreground && type != FrameType::read_ahead)
            return;
        {
            std::lock_guard lock(outbound_mutex);
            auto found = inbound_classes.find(request_id);
            if (found != inbound_classes.end())
                found->second = more_urgent(type, found->second);
            for (auto& item : outbound) {
                if (item.reply && item.request_id == request_id)
                    item.frame_type = more_urgent(type, item.frame_type);
            }
        }
        outbound_cv.notify_one();
    }

    void cancel_inbound(uint64_t request_id) {
        std::lock_guard lock(outbound_mutex);
        cancelled_inbound.insert(request_id);
        inbound_classes.erase(request_id);
        std::erase_if(outbound, [&](const Outbound& item) {
            return item.reply && item.request_id == request_id;
        });
    }

    void promote_outgoing(uint64_t request_id, FrameType type) {
        MessageType control_type{};
        if (type == FrameType::foreground)
            control_type = MessageType::promote_foreground;
        else if (type == FrameType::read_ahead)
            control_type = MessageType::promote_read_ahead;
        else
            return;
        {
            std::lock_guard lock(outbound_mutex);
            for (auto& item : outbound) {
                if (!item.reply && item.request_id == request_id)
                    item.frame_type = more_urgent(type, item.frame_type);
            }
        }
        try {
            queue_control(transfer_control(control_type, request_id));
        } catch (...) {
        }
    }

    void cancel_outgoing(uint64_t request_id) {
        {
            std::lock_guard lock(outbound_mutex);
            std::erase_if(outbound, [&](const Outbound& item) {
                return !item.reply && item.request_id == request_id;
            });
        }
        try {
            queue_control(transfer_control(MessageType::cancel_transfer, request_id));
        } catch (...) {
        }
    }

    std::deque<Outbound>::iterator best_outbound_locked() {
        return std::min_element(outbound.begin(), outbound.end(), [](const Outbound& a,
                                                                     const Outbound& b) {
            return frame_type_priority(a.frame_type) < frame_type_priority(b.frame_type);
        });
    }

    void writer_loop(std::stop_token stop) {
        try {
            while (true) {
                Outbound item;
                {
                    std::unique_lock lock(outbound_mutex);
                    maybe_queue_retire_locked();
                    outbound_cv.wait(lock, [&] {
                        maybe_queue_retire_locked();
                        return stop.stop_requested() || done.load() || !outbound.empty();
                    });
                    if ((stop.stop_requested() || done.load()) && outbound.empty())
                        return;
                    if (done.load())
                        return;
                    auto best = best_outbound_locked();
                    item = std::move(*best);
                    outbound.erase(best);
                    if (item.reply) {
                        if (auto found = inbound_classes.find(item.request_id);
                            found != inbound_classes.end())
                            item.frame_type = more_urgent(found->second, item.frame_type);
                    }
                }

                const auto& payload = item.message.payload;
                const size_t remaining = payload.size() - item.offset;
                const size_t amount = std::min(remaining, channel->max_frame_size());
                const bool first = item.offset == 0;
                const bool last = remaining <= channel->max_frame_size();
                std::span<const uint8_t> fragment;
                if (amount)
                    fragment = std::span<const uint8_t>(payload).subspan(item.offset, amount);

                ++writes_active;
                try {
                    channel->send_fragment(item.request_id, item.frame_type, item.message.type, first,
                                           last, fragment, [this, id = item.request_id](size_t) {
                                               if (!id || (id & 1U))
                                                   return;
                                               std::lock_guard lock(pending_mutex);
                                               auto found = pending.find(id);
                                               if (found != pending.end())
                                                   found->second->last_progress_ns.store(steady_ns());
                                           });
                } catch (...) {
                    --writes_active;
                    throw;
                }
                --writes_active;

                if (!last) {
                    item.offset += amount;
                    std::lock_guard lock(outbound_mutex);
                    if (item.reply) {
                        if (cancelled_inbound.contains(item.request_id)) {
                            cancelled_inbound.erase(item.request_id);
                            inbound_classes.erase(item.request_id);
                        } else {
                            if (auto found = inbound_classes.find(item.request_id);
                                found != inbound_classes.end())
                                item.frame_type = more_urgent(found->second, item.frame_type);
                            outbound.push_back(std::move(item));
                        }
                    } else {
                        outbound.push_back(std::move(item));
                    }
                    maybe_queue_retire_locked();
                    outbound_cv.notify_one();
                } else {
                    if (item.reply) {
                        std::lock_guard lock(outbound_mutex);
                        inbound_classes.erase(item.request_id);
                        cancelled_inbound.erase(item.request_id);
                        maybe_queue_retire_locked();
                    }
                    if (item.sent) {
                        try {
                            item.sent->set_value();
                        } catch (...) {
                        }
                    }
                }
                maybe_finish_retire();
            }
        } catch (const std::exception& error) {
            Log::debug(std::string("accepted peer writer: ") + error.what());
            ready = false;
            fail_pending(error.what());
            if (channel)
                channel->shutdown();
            {
                std::lock_guard lock(outbound_mutex);
                for (auto& item : outbound) {
                    if (!item.sent)
                        continue;
                    try {
                        item.sent->set_exception(rpc_error(error.what()));
                    } catch (...) {
                    }
                }
                outbound.clear();
            }
            outbound_cv.notify_all();
        }
    }

    void start_writer() {
        writer = std::jthread([this](std::stop_token stop) { writer_loop(stop); });
    }

    void notify(const RpcMessage& message) {
        if (!usable())
            return;
        auto sent = std::make_shared<std::promise<void>>();
        auto future = sent->get_future();
        if (queue_message(0, FrameType::control, message, false, sent))
            future.get();
    }

    void retire() {
        {
            std::lock_guard admission(admission_mutex);
            bool expected = false;
            if (!retiring.compare_exchange_strong(expected, true))
                return;
        }
        {
            std::lock_guard lock(outbound_mutex);
            maybe_queue_retire_locked();
        }
        outbound_cv.notify_all();
        maybe_finish_retire();
    }

    void peer_retire() {
        peer_retired = true;
        retire();
        maybe_finish_retire();
    }

    AsyncRpc call(MessageType type, std::span<const uint8_t> payload, FrameType frame_type) {
        validate_frame_semantics(type, frame_type);

        uint64_t id = 0;
        auto item = std::make_shared<Pending>();
        auto future = item->promise.get_future();
        {
            std::lock_guard admission(admission_mutex);
            if (!usable())
                throw std::runtime_error("accepted peer session is unavailable");
            id = next_request.fetch_add(2);
            if (!id)
                throw std::runtime_error("RPC request id exhausted");
            {
                std::lock_guard lock(pending_mutex);
                pending.emplace(id, item);
            }
            try {
                (void)queue_message(id, frame_type,
                                    {type, Bytes(payload.begin(), payload.end())}, false);
            } catch (...) {
                std::lock_guard lock(pending_mutex);
                pending.erase(id);
                throw;
            }
        }

        const std::weak_ptr<Session> weak = shared_from_this();
        auto cancel = [weak, id] {
            if (auto self = weak.lock()) {
                bool existed = false;
                {
                    std::lock_guard lock(self->pending_mutex);
                    existed = self->pending.erase(id) != 0;
                }
                if (existed)
                    self->cancel_outgoing(id);
                self->maybe_finish_retire();
            }
        };
        auto abort = [weak, id] {
            if (auto self = weak.lock()) {
                {
                    std::lock_guard lock(self->pending_mutex);
                    self->pending.erase(id);
                }
                self->close();
            }
        };
        auto promote = [weak, id](FrameType frame_type) {
            if (auto self = weak.lock())
                self->promote_outgoing(id, frame_type);
        };
        auto idle = [weak, id] {
            auto self = weak.lock();
            if (!self)
                return std::chrono::milliseconds(0);
            std::shared_ptr<Pending> item;
            {
                std::lock_guard lock(self->pending_mutex);
                auto found = self->pending.find(id);
                if (found != self->pending.end())
                    item = found->second;
            }
            if (!item)
                return std::chrono::milliseconds(0);
            const auto idle_ns = std::max<int64_t>(
                0, steady_ns() - item->last_progress_ns.load(std::memory_order_relaxed));
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::nanoseconds(idle_ns));
        };
        return AsyncRpc(std::move(future), std::move(cancel), std::move(abort),
                        std::move(promote), std::move(idle));
    }

    void close() {
        ready = false;
        done = true;
        if (channel)
            channel->shutdown();
        fail_pending("accepted peer session closed");
        {
            std::lock_guard lock(outbound_mutex);
            for (auto& item : outbound) {
                if (item.sent) {
                    try {
                        item.sent->set_exception(rpc_error("accepted peer session closed"));
                    } catch (...) {
                    }
                }
            }
            outbound.clear();
        }
        if (writer.joinable())
            writer.request_stop();
        outbound_cv.notify_all();
    }
};

RpcServer::RpcServer(std::string host, uint16_t port, ClusterKeys keys, NodeInfo local,
                     Handler handler, Observer observer, size_t max_frame_size)
    : host_(std::move(host)), port_(port), keys_(keys), local_(std::move(local)),
      handler_(std::move(handler)), observer_(std::move(observer)),
      max_frame_size_(max_frame_size) {
    validate_frame_limit(max_frame_size_);
}

RpcServer::~RpcServer() {
    stop();
}

RpcServer::RequestClass RpcServer::request_class(FrameType type) {
    switch (type) {
    case FrameType::control:
        return RequestClass::control;
    case FrameType::foreground:
        return RequestClass::foreground;
    case FrameType::read_ahead:
        return RequestClass::read_ahead;
    case FrameType::speculative:
        return RequestClass::speculative;
    }
    return RequestClass::speculative;
}

std::deque<RpcServer::RequestJob>& RpcServer::queue(RequestClass cls) {
    switch (cls) {
    case RequestClass::control:
        return control_requests_;
    case RequestClass::foreground:
        return foreground_requests_;
    case RequestClass::read_ahead:
        return read_ahead_requests_;
    case RequestClass::speculative:
        return speculative_requests_;
    }
    return speculative_requests_;
}

bool RpcServer::data_ready() const {
    return !foreground_requests_.empty() || !read_ahead_requests_.empty() ||
           !speculative_requests_.empty();
}

RpcServer::RequestClass RpcServer::next_data_class() const {
    if (!foreground_requests_.empty())
        return RequestClass::foreground;
    if (!read_ahead_requests_.empty())
        return RequestClass::read_ahead;
    return RequestClass::speculative;
}

void RpcServer::attach_client(RpcClient& client) {
    shared_client_ = &client;
    client.set_inbound_handler(
        [this](const NodeInfo& peer, RpcFrame frame, RpcClient::InboundReply reply) {
            enqueue_shared(peer, std::move(frame), std::move(reply));
        });
    client.set_inbound_transfer_control(
        [this](const NodeInfo& peer, uint64_t request_id, FrameType type) {
            promote_queued(peer, request_id, type);
        },
        [this](const NodeInfo& peer, uint64_t request_id) { cancel_queued(peer, request_id); });
}

void RpcServer::enqueue_shared(const NodeInfo& peer, RpcFrame frame,
                               RpcClient::InboundReply reply) {
    const auto cls = request_class(frame.frame_type);
    {
        std::lock_guard lock(request_mutex_);
        auto& requests = queue(cls);
        if (requests.size() >= max_pending_requests)
            throw std::runtime_error("RPC server request queue full");
        requests.push_back({{}, peer, std::move(frame), std::move(reply)});
    }
    request_cv_.notify_all();
}

void RpcServer::promote_queued(const NodeInfo& peer, uint64_t request_id, FrameType type) {
    if (type != FrameType::foreground && type != FrameType::read_ahead)
        return;

    std::lock_guard lock(request_mutex_);
    auto promote_from = [&](std::deque<RequestJob>& requests) {
        auto found = std::find_if(requests.begin(), requests.end(), [&](const RequestJob& job) {
            return job.peer.id == peer.id && job.frame.request_id == request_id;
        });
        if (found == requests.end())
            return false;

        found->frame.frame_type = more_urgent(type, found->frame.frame_type);
        auto target_class = request_class(found->frame.frame_type);
        if (&requests == &queue(target_class))
            return true;

        auto job = std::move(*found);
        requests.erase(found);
        queue(target_class).push_back(std::move(job));
        return true;
    };

    if (!promote_from(speculative_requests_))
        if (!promote_from(read_ahead_requests_))
            (void)promote_from(foreground_requests_);
    request_cv_.notify_all();
}

void RpcServer::cancel_queued(const NodeInfo& peer, uint64_t request_id) {
    std::vector<RpcClient::InboundReply> replies;
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(request_mutex_);
        auto cancel_from = [&](std::deque<RequestJob>& requests) {
            for (auto it = requests.begin(); it != requests.end();) {
                if (it->peer.id != peer.id || it->frame.request_id != request_id) {
                    ++it;
                    continue;
                }
                if (it->reply)
                    replies.push_back(std::move(it->reply));
                if (it->session) {
                    if (it->session->active_requests.load())
                        --it->session->active_requests;
                    sessions.push_back(it->session);
                }
                it = requests.erase(it);
            }
        };
        cancel_from(foreground_requests_);
        cancel_from(read_ahead_requests_);
        cancel_from(speculative_requests_);
    }
    for (auto& reply : replies) {
        try {
            reply({MessageType::error, {}});
        } catch (...) {
        }
    }
    for (auto& session : sessions)
        session->maybe_finish_retire();
}

void RpcServer::start() {
    uint16_t bound = 0;
    int fd = bind_socket(host_, port_, &bound);
    bound_port_ = bound;
    listen_fd_ = fd;

    control_workers_.reserve(control_worker_count);
    data_workers_.reserve(data_worker_count);
    for (size_t i = 0; i < control_worker_count; ++i)
        control_workers_.emplace_back(
            [this](std::stop_token stop) { control_worker_loop(stop); });
    for (size_t i = 0; i < data_worker_count; ++i)
        data_workers_.emplace_back([this](std::stop_token stop) { data_worker_loop(stop); });

    accept_thread_ = std::jthread([this](std::stop_token stop) { accept_loop(stop); });
}

void RpcServer::stop() {
    Log::debug("shutdown: RpcServer::stop begin");
    if (shared_client_) {
        shared_client_->set_inbound_handler({});
        shared_client_->set_inbound_transfer_control({}, {});
    }

    if (accept_thread_.joinable())
        accept_thread_.request_stop();
    int fd = listen_fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (accept_thread_.joinable()) {
        Log::debug("shutdown: RPC accept thread joining");
        accept_thread_.join();
        Log::debug("shutdown: RPC accept thread joined");
    }

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(sessions_mutex_);
        sessions = sessions_;
    }
    for (auto& session : sessions) {
        session->close();
        if (session->reader.joinable())
            session->reader.request_stop();
    }
    Log::debug("shutdown: RPC sessions reaping count=" + std::to_string(sessions.size()));
    reap_sessions(true);
    Log::debug("shutdown: RPC sessions reaped");

    for (auto& worker : control_workers_)
        worker.request_stop();
    for (auto& worker : data_workers_)
        worker.request_stop();
    request_cv_.notify_all();
    control_workers_.clear();
    data_workers_.clear();

    std::vector<std::function<void(const RpcMessage&)>> dropped;
    {
        std::lock_guard lock(request_mutex_);
        for (auto* requests : {&control_requests_, &foreground_requests_, &read_ahead_requests_,
                               &speculative_requests_}) {
            for (auto& job : *requests) {
                if (job.reply)
                    dropped.push_back(job.reply);
                if (job.session && job.session->active_requests.load())
                    --job.session->active_requests;
            }
            requests->clear();
        }
    }
    for (auto& reply : dropped) {
        try {
            reply({MessageType::error, {}});
        } catch (...) {
        }
    }

    shared_client_ = nullptr;
    Log::debug("shutdown: RpcServer::stop complete");
}

void RpcServer::accept_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        reap_sessions(false);
        int fd = listen_fd_;
        if (fd < 0)
            break;
        pollfd poll_fd{fd, POLLIN, 0};
        int rc = poll(&poll_fd, 1, 250);
        if (rc <= 0)
            continue;

        sockaddr_storage address{};
        socklen_t size = sizeof(address);
        int client = accept(fd, reinterpret_cast<sockaddr*>(&address), &size);
        if (client < 0)
            continue;
        socket_options(client);

        auto session = std::make_shared<Session>();
        session->remote_host = numeric_host(address, size);
        session->channel = std::make_unique<SecureChannel>(client, keys_, local_, max_frame_size_);
        {
            std::lock_guard lock(sessions_mutex_);
            sessions_.push_back(session);
        }
        session->reader = std::jthread([this, raw = session.get()](std::stop_token) {
            session_loop(raw);
        });
    }
}

void RpcServer::session_loop(Session* session) {
    MessageAssembler assembler;
    try {
        session->peer = session->channel->server_handshake(session->remote_host);
        session->ready = true;
        session->start_writer();
        observer_(session->peer);

        if (shared_client_) {
            std::shared_ptr<Session> shared;
            {
                std::lock_guard sessions_lock(sessions_mutex_);
                auto found = std::find_if(sessions_.begin(), sessions_.end(), [&](const auto& item) {
                    return item.get() == session;
                });
                if (found != sessions_.end())
                    shared = *found;
            }
            if (shared) {
                std::weak_ptr<Session> weak = shared;
                RpcClient::InboundRoute route;
                route.peer = session->peer;
                route.session_id = session->channel->session_id();
                route.call = [weak](MessageType type, std::span<const uint8_t> payload,
                                    FrameType frame_type) {
                    if (auto s = weak.lock())
                        return s->call(type, payload, frame_type);
                    throw std::runtime_error("accepted peer session is closed");
                };
                route.notify = [weak](const RpcMessage& message) {
                    if (auto s = weak.lock())
                        s->notify(message);
                };
                route.retire = [weak] {
                    if (auto s = weak.lock())
                        s->retire();
                };
                route.close = [weak] {
                    if (auto s = weak.lock())
                        s->close();
                };
                route.usable = [weak] {
                    if (auto s = weak.lock())
                        return s->usable();
                    return false;
                };
                shared_client_->register_inbound(std::move(route));
            }
        }

        Log::info("node connection inbound peer=" + to_string(session->peer.id).substr(0, 12) +
                  " remote=" + session->remote_host + " advertised=" + session->peer.host + ':' +
                  std::to_string(session->peer.port));
        while (true) {
            auto fragment = session->channel->receive_fragment([session](uint64_t request_id, size_t) {
                if (!request_id || (request_id & 1U))
                    return;
                std::lock_guard lock(session->pending_mutex);
                auto found = session->pending.find(request_id);
                if (found != session->pending.end())
                    found->second->last_progress_ns.store(steady_ns());
            });
            auto frame = assembler.push(std::move(fragment));
            if (!frame)
                continue;

            if (frame->request_id == 0) {
                if (frame->message.type == MessageType::metadata_notice && shared_client_) {
                    Reader reader(frame->message.payload);
                    const auto generation = reader.u64();
                    reader.finish();
                    shared_client_->metadata_observer_(generation);
                } else if (frame->message.type == MessageType::session_retire) {
                    session->peer_retire();
                } else if (auto promoted = promotion_type(frame->message.type)) {
                    const auto target = transfer_target(frame->message);
                    assembler.promote(target, *promoted);
                    session->promote_inbound(target, *promoted);
                    promote_queued(session->peer, target, *promoted);
                } else if (frame->message.type == MessageType::cancel_transfer) {
                    const auto target = transfer_target(frame->message);
                    assembler.discard(target);
                    session->cancel_inbound(target);
                    cancel_queued(session->peer, target);
                }
                continue;
            }

            if ((frame->request_id & 1U) == 0) {
                std::shared_ptr<Session::Pending> pending;
                {
                    std::lock_guard lock(session->pending_mutex);
                    auto found = session->pending.find(frame->request_id);
                    if (found == session->pending.end())
                        continue;
                    pending = std::move(found->second);
                    session->pending.erase(found);
                }
                pending->promise.set_value({session->peer, std::move(frame->message)});
                session->maybe_finish_retire();
                continue;
            }

            const auto cls = request_class(frame->frame_type);
            session->register_inbound(frame->request_id, frame->frame_type);
            bool queued = false;
            {
                std::lock_guard lock(request_mutex_);
                if (queue(cls).size() < max_pending_requests) {
                    std::shared_ptr<Session> shared;
                    {
                        std::lock_guard sessions_lock(sessions_mutex_);
                        auto found = std::find_if(sessions_.begin(), sessions_.end(),
                                                  [&](const auto& item) {
                                                      return item.get() == session;
                                                  });
                        if (found != sessions_.end())
                            shared = *found;
                    }
                    if (shared) {
                        ++shared->active_requests;
                        queue(cls).push_back({std::move(shared), session->peer,
                                             std::move(*frame), {}});
                        queued = true;
                    }
                }
            }
            if (!queued)
                throw std::runtime_error("RPC server request queue full");
            request_cv_.notify_all();
        }
    } catch (const std::exception& error) {
        Log::debug(std::string("RPC session: ") + error.what());
        session->fail_pending(error.what());
    }

    if (shared_client_ && session->peer.id != NodeId{})
        shared_client_->unregister_inbound(session->peer.id, session->channel->session_id());
    session->ready = false;
    session->done = true;
    if (session->channel)
        session->channel->shutdown();
    session->outbound_cv.notify_all();
}

void RpcServer::execute(RequestJob job) {
    if (job.session && !job.session->ready.load()) {
        if (job.session->active_requests.load())
            --job.session->active_requests;
        job.session->maybe_finish_retire();
        return;
    }

    try {
        auto reply = handler_(job.peer, job.frame.message);
        if (job.reply) {
            job.reply(reply);
        } else if (job.session) {
            (void)job.session->queue_message(job.frame.request_id, job.frame.frame_type, reply, true);
        }
    } catch (const std::exception& error) {
        Log::debug(std::string("RPC handler: ") + error.what());
        if (job.reply) {
            try {
                job.reply({MessageType::error, {}});
            } catch (...) {
            }
        } else if (job.session) {
            try {
                (void)job.session->queue_message(job.frame.request_id, job.frame.frame_type,
                                                 {MessageType::error, {}}, true);
            } catch (...) {
            }
        }
    }

    if (job.session) {
        if (job.session->active_requests.load())
            --job.session->active_requests;
        job.session->maybe_finish_retire();
    }
}

void RpcServer::control_worker_loop(std::stop_token stop) {
    while (true) {
        RequestJob job;
        {
            std::unique_lock lock(request_mutex_);
            request_cv_.wait(lock, [&] {
                return stop.stop_requested() || !control_requests_.empty();
            });
            if (stop.stop_requested() && control_requests_.empty())
                return;
            job = std::move(control_requests_.front());
            control_requests_.pop_front();
        }
        execute(std::move(job));
    }
}

void RpcServer::data_worker_loop(std::stop_token stop) {
    while (true) {
        RequestJob job;
        {
            std::unique_lock lock(request_mutex_);
            request_cv_.wait(lock, [&] { return stop.stop_requested() || data_ready(); });
            if (stop.stop_requested() && !data_ready())
                return;
            auto cls = next_data_class();
            auto& requests = queue(cls);
            job = std::move(requests.front());
            requests.pop_front();
        }
        execute(std::move(job));
    }
}

void RpcServer::reap_sessions(bool all) {
    std::vector<std::shared_ptr<Session>> reap;
    {
        std::lock_guard lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (all || (*it)->done.load()) {
                reap.push_back(*it);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& session : reap) {
        if (all)
            session->close();
        if (session->reader.joinable()) {
            session->reader.request_stop();
            session->reader.join();
        }
        if (session->writer.joinable()) {
            session->writer.request_stop();
            session->outbound_cv.notify_all();
            session->writer.join();
        }
    }
}

void RpcServer::broadcast(const RpcMessage& message) {
    if (shared_client_) {
        shared_client_->broadcast(message);
        return;
    }
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(sessions_mutex_);
        sessions = sessions_;
    }
    for (auto& session : sessions) {
        try {
            session->notify(message);
        } catch (...) {
        }
    }
}

} // namespace macha

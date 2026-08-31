// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.hpp"
#include "http.hpp"

#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace macha {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::map<std::string, std::string, std::less<>> parse_query(std::string_view query) {
    std::map<std::string, std::string, std::less<>> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        auto part = query.substr(pos, amp == std::string_view::npos ? query.size() - pos : amp - pos);
        auto eq = part.find('=');
        out[http_url_decode(part.substr(0, eq))] =
            eq == std::string_view::npos ? "" : http_url_decode(part.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

std::optional<std::string> read_token(const std::optional<std::filesystem::path>& path) {
    if (!path) return {};
    std::ifstream file(*path);
    if (!file) throw std::runtime_error("cannot open catalogue API token file");
    std::string token;
    std::getline(file, token);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
    if (token.empty()) throw std::runtime_error("catalogue API token file is empty");
    return token;
}


void set_client_io_timeout(int fd, std::chrono::milliseconds timeout) {
    timeval value{};
    value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0)
        throw std::runtime_error("cannot set HTTP client I/O timeout: " +
                                 std::string(std::strerror(errno)));
}


ssize_t recv_before(int fd, void* data, size_t size, Clock::time_point deadline) {
    while (true) {
        const auto now = Clock::now();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd item{fd, POLLIN, 0};
        const auto wait_ms = static_cast<int>(std::max<int64_t>(1, remaining.count()));
        const int ready = ::poll(&item, 1, wait_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            if (ready == 0) errno = ETIMEDOUT;
            return -1;
        }
        const auto n = ::recv(fd, data, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

bool send_all(int fd, const void* data, size_t size, int flags) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < size) {
        auto n = ::send(fd, bytes + sent, size - sent, flags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}


} // namespace

HttpResponse http_json(int status, std::string value) {
    return {status, "application/json; charset=utf-8", {}, Bytes(value.begin(), value.end()), {}};
}

HttpResponse http_error(int status, std::string_view code, std::string_view message) {
    Json::Object root;
    root["error"] = Json::Object{{"code", std::string(code)}, {"message", std::string(message)}};
    return http_json(status, Json(std::move(root)).dump());
}

std::string http_url_decode(std::string_view value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = nibble(value[i + 1]), lo = nibble(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

HttpServer::HttpServer(CatalogueApiConfig config,
                       std::function<HttpResponse(const HttpRequest&)> handler,
                       std::function<bool(const HttpRequest&)> bearer_exempt)
    : config_(std::move(config)), handler_(std::move(handler)),
      bearer_exempt_(std::move(bearer_exempt)), bearer_token_(read_token(config_.token_file)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    if (running_.exchange(true)) return;
    {
        std::lock_guard lock(startup_mutex_);
        startup_complete_ = false;
        startup_error_.clear();
    }
    const auto worker_count = std::max<size_t>(1, config_.workers);
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i)
        workers_.emplace_back([this](std::stop_token stop) { worker(stop); });
    accept_thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    std::unique_lock lock(startup_mutex_);
    startup_cv_.wait(lock, [this] { return startup_complete_; });
    if (!startup_error_.empty()) {
        const auto error = startup_error_;
        lock.unlock();
        stop();
        throw std::runtime_error(error);
    }
}

void HttpServer::close_queued_clients() {
    std::lock_guard lock(queue_mutex_);
    while (!queue_.empty()) {
        ::shutdown(queue_.front(), SHUT_RDWR);
        ::close(queue_.front());
        queue_.pop_front();
    }
}

void HttpServer::stop() {
    if (!running_.load()) return;
    request_stop();
    if (!running_.exchange(false)) return;
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    for (auto& worker_thread : workers_) {
        if (worker_thread.joinable()) worker_thread.join();
    }
    workers_.clear();
    bound_port_ = 0;
}

void HttpServer::request_stop() {
    if (!running_.load()) return;
    if (const int listen_fd = listen_fd_.exchange(-1); listen_fd >= 0) {
        ::shutdown(listen_fd, SHUT_RDWR);
        ::close(listen_fd);
    }
    if (accept_thread_.joinable())
        accept_thread_.request_stop();
    close_queued_clients();
    {
        std::lock_guard lock(active_mutex_);
        for (int fd : active_fds_) ::shutdown(fd, SHUT_RDWR);
    }
    for (auto& worker_thread : workers_) worker_thread.request_stop();
    queue_cv_.notify_all();
}

void HttpServer::run(std::stop_token stop) {
    set_thread_name("macha-http-acc");
    int listen_fd = -1;
    try {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* results = nullptr;
        auto port = std::to_string(config_.port);
        const char* host = config_.listen.empty() ? nullptr : config_.listen.c_str();
        if (getaddrinfo(host, port.c_str(), &hints, &results))
            throw std::runtime_error("catalogue API address resolution failed");
        for (auto* result = results; result; result = result->ai_next) {
            listen_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (listen_fd < 0) continue;
            int yes = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (bind(listen_fd, result->ai_addr, result->ai_addrlen) == 0 && listen(listen_fd, 128) == 0)
                break;
            ::close(listen_fd);
            listen_fd = -1;
        }
        freeaddrinfo(results);
        if (listen_fd < 0) throw std::runtime_error("cannot bind catalogue API");

        int expected = -1;
        if (!listen_fd_.compare_exchange_strong(expected, listen_fd)) {
            ::close(listen_fd);
            listen_fd = -1;
            throw std::runtime_error("HTTP API listener already active");
        }
        if (!running_ || stop.stop_requested()) {
            if (const int fd = listen_fd_.exchange(-1); fd >= 0) {
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
            }
            listen_fd = -1;
            return;
        }

        sockaddr_storage bound{};
        socklen_t bound_size = sizeof(bound);
        if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0) {
            if (bound.ss_family == AF_INET)
                bound_port_ = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
            else if (bound.ss_family == AF_INET6)
                bound_port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
            else
                bound_port_ = config_.port;
        } else {
            bound_port_ = config_.port;
        }

        {
            std::lock_guard lock(startup_mutex_);
            startup_complete_ = true;
        }
        startup_cv_.notify_all();

        Log::info("HTTP API listening on " + config_.listen + ":" + std::to_string(bound_port()));
        while (!stop.stop_requested()) {
            int fd = accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (stop.stop_requested() || !running_) break;
                continue;
            }
            {
                std::lock_guard lock(queue_mutex_);
                if (queue_.size() >= config_.max_queued_connections) {
                    ::shutdown(fd, SHUT_RDWR);
                    ::close(fd);
                    continue;
                }
                queue_.push_back(fd);
            }
            queue_cv_.notify_one();
        }
    } catch (const std::exception& e) {
        {
            std::lock_guard lock(startup_mutex_);
            if (!startup_complete_) {
                startup_error_ = e.what();
                startup_complete_ = true;
            }
        }
        startup_cv_.notify_all();
        if (running_) Log::error("HTTP API: " + std::string(e.what()));
    }

    if (listen_fd >= 0) {
        int expected = listen_fd;
        if (listen_fd_.compare_exchange_strong(expected, -1)) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
        }
    }
}

void HttpServer::worker(std::stop_token stop) {
    set_thread_name("macha-http-work");
    while (!stop.stop_requested()) {
        int fd = -1;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested()) break;
            fd = queue_.front();
            queue_.pop_front();
        }
        {
            std::lock_guard lock(active_mutex_);
            active_fds_.insert(fd);
        }
        try {
            handle_client(fd);
        } catch (const std::exception& e) {
            Log::debug("HTTP client: " + std::string(e.what()));
        }
        {
            std::lock_guard lock(active_mutex_);
            active_fds_.erase(fd);
        }
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

void HttpServer::handle_client(int fd) {
    // Queue depth and worker count bound admission; the socket timeout also
    // bounds occupancy so a stalled/incomplete client cannot pin a worker forever.
    set_client_io_timeout(fd, config_.client_io_timeout);
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif

    const auto request_deadline = Clock::now() + config_.client_io_timeout;
    std::string input;
    std::array<char, 8192> buffer{};
    auto header_end = std::string::npos;
    while ((header_end = input.find("\r\n\r\n")) == std::string::npos) {
        auto n = recv_before(fd, buffer.data(), buffer.size(), request_deadline);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        input.append(buffer.data(), static_cast<size_t>(n));
        if (input.size() > 64 * 1024) return;
    }

    std::istringstream headers(input.substr(0, header_end));
    std::string request_line;
    std::getline(headers, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
    std::istringstream request_stream(request_line);
    HttpRequest request;
    std::string target, version;
    request_stream >> request.method >> target >> version;
    if (request.method.empty() || target.empty()) return;
    auto question = target.find('?');
    request.path = http_url_decode(target.substr(0, question));
    if (question != std::string::npos) request.query = parse_query(target.substr(question + 1));

    std::string line;
    size_t content_length = 0;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        request.headers[key] = value;
        if (key == "content-length") {
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), content_length);
            if (ec != std::errc{} || end != value.data() + value.size()) return;
        }
    }

    HttpResponse response;
    if (request.method == "OPTIONS") {
        response = {204, "text/plain", {}, {}, {}};
    } else if (content_length > config_.max_request_bytes) {
        response = http_error(413, "too_large", "request body too large");
    } else {
        auto body_start = header_end + 4;
        request.body.insert(request.body.end(), input.begin() + static_cast<std::ptrdiff_t>(body_start), input.end());
        while (request.body.size() < content_length) {
            auto n = recv_before(fd, buffer.data(),
                                 std::min(buffer.size(), content_length - request.body.size()),
                                 request_deadline);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            request.body.insert(request.body.end(), buffer.begin(), buffer.begin() + n);
        }
        if (request.body.size() > content_length) request.body.resize(content_length);

        const bool exempt = bearer_exempt_ && bearer_exempt_(request);
        if (bearer_token_ && !exempt) {
            auto authorization = request.headers.find("authorization");
            const std::string expected = "Bearer " + *bearer_token_;
            if (authorization == request.headers.end() || authorization->second != expected)
                response = http_error(401, "unauthorized", "bearer token required");
            else
                response = handler_(request);
        } else {
            response = handler_(request);
        }
    }

    response.headers.try_emplace("Access-Control-Allow-Origin", "*");
    response.headers.try_emplace("Access-Control-Allow-Headers", "Authorization, Content-Type, If-Match, Range, Idempotency-Key");
    response.headers.try_emplace("Access-Control-Allow-Methods", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
    response.headers.try_emplace("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range, Location, Idempotency-Key, X-Macha-Idempotency");
    response.headers.try_emplace("Accept-Ranges", response.stream ? "bytes" : "none");

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << reason(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.content_length() << "\r\n";
    out << "Connection: close\r\n";
    for (const auto& [key, value] : response.headers) out << key << ": " << value << "\r\n";
    out << "\r\n";
    auto head = out.str();
    if (!send_all(fd, head.data(), head.size(), send_flags)) return;
    if (request.method == "HEAD") return;

    if (response.stream) {
        Bytes chunk(std::max<size_t>(16 * 1024, config_.stream_chunk_bytes));
        uint64_t offset = 0;
        const auto total = response.stream->size();
        while (offset < total) {
            const auto wanted = static_cast<size_t>(std::min<uint64_t>(chunk.size(), total - offset));
            auto n = response.stream->read(offset, std::span<uint8_t>(chunk.data(), wanted));
            if (!n) break;
            if (!send_all(fd, chunk.data(), n, send_flags)) break;
            offset += n;
        }
    } else if (!response.body.empty()) {
        (void)send_all(fd, response.body.data(), response.body.size(), send_flags);
    }
}

} // namespace macha

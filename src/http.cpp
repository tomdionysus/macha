// SPDX-License-Identifier: GPL-3.0-or-later
#include "http.hpp"
#include "diagnostics.hpp"
#include "http_compression.hpp"

#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace macha {
namespace {

using namespace std::chrono_literals;

constexpr size_t max_header_bytes = 64 * 1024;
constexpr size_t recv_buffer_bytes = 16 * 1024;
constexpr size_t recv_per_pass_bytes = 64 * 1024;
// Input a connection may accumulate while it is not being read for a
// request (a pipelining client sending ahead): past this the reactor stops
// asking for POLLIN until the current response is done.
constexpr size_t parked_input_bytes = max_header_bytes;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::map<std::string, std::string, std::less<>> parse_query(std::string_view query) {
    std::map<std::string, std::string, std::less<>> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        auto part =
            query.substr(pos, amp == std::string_view::npos ? query.size() - pos : amp - pos);
        auto eq = part.find('=');
        out[http_url_decode(part.substr(0, eq))] =
            eq == std::string_view::npos ? "" : http_url_decode(part.substr(eq + 1));
        if (amp == std::string_view::npos)
            break;
        pos = amp + 1;
    }
    return out;
}

std::string reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 416:
        return "Range Not Satisfiable";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "OK";
    }
}

bool set_non_blocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return true;
}

#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

int64_t ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

// What the compute pool hands back to the reactor. The reactor never sees
// the HttpBodySource itself: it sees a pump, whose only reactor-callable
// surface is memory already read.
class BodyPump {
    std::shared_ptr<HttpBodySource> source_;
    uint64_t size_{};
    const uint8_t* resident_{};

  public:
    // Constructed on a pool thread: size() and resident() are the source's
    // to answer, and this is the one place they are asked.
    explicit BodyPump(std::shared_ptr<HttpBodySource> source)
        : source_(std::move(source)), size_(source_->size()), resident_(source_->resident()) {}
    uint64_t size() const noexcept {
        return size_;
    }
    const uint8_t* resident() const noexcept {
        return resident_;
    }
    // Pool only.
    Bytes read(uint64_t offset, size_t length) {
        Bytes out(length);
        const auto n = source_->read(offset, std::span<uint8_t>(out.data(), out.size()));
        out.resize(n);
        return out;
    }
};

enum class EventKind { response, chunk, wake, stop };

struct Event {
    EventKind kind{};
    uint64_t connection{};
    uint64_t generation{};
    HttpResponse response;
    std::shared_ptr<BodyPump> pump;
    std::optional<HttpRequest> request; // carried back when the handler deferred
    Bytes bytes;
    bool ok{};
};

// The one way anything reaches the reactor from another thread. A pipe
// rather than eventfd because macOS has no eventfd, and one byte is all the
// reactor needs to know there is something to drain.
class Inbox {
    std::mutex mutex_;
    std::deque<Event> events_;
    int wake_read_{-1};
    int wake_write_{-1};
    std::atomic_bool signalled_{false};
    bool closed_{};

  public:
    Inbox() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0)
            throw std::runtime_error("cannot create HTTP reactor wake pipe: " +
                                     std::string(std::strerror(errno)));
        wake_read_ = fds[0];
        wake_write_ = fds[1];
        set_non_blocking(wake_read_);
        set_non_blocking(wake_write_);
    }
    ~Inbox() {
        if (wake_read_ >= 0)
            ::close(wake_read_);
        if (wake_write_ >= 0)
            ::close(wake_write_);
    }
    Inbox(const Inbox&) = delete;
    Inbox& operator=(const Inbox&) = delete;

    int wake_fd() const noexcept {
        return wake_read_;
    }

    void post(Event event) {
        {
            std::lock_guard lock(mutex_);
            if (closed_)
                return;
            events_.push_back(std::move(event));
        }
        if (!signalled_.exchange(true)) {
            const char byte = 'w';
            while (::write(wake_write_, &byte, 1) < 0 && errno == EINTR) {
            }
        }
    }

    std::deque<Event> drain() {
        std::array<char, 64> sink{};
        while (::read(wake_read_, sink.data(), sink.size()) > 0) {
        }
        // Cleared before the events are taken, so a post that lands between
        // the two writes the pipe again rather than being missed.
        signalled_.store(false);
        std::deque<Event> out;
        std::lock_guard lock(mutex_);
        out.swap(events_);
        return out;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        events_.clear();
    }
};

// A bounded pool of threads that only ever compute: handlers and body reads.
struct Lane {
    struct Job {
        std::function<void()> run;
        Clock::time_point queued_at;
    };

    const char* name;
    const char* thread_name;
    size_t max_queued{};
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Job> queue;
    std::vector<std::jthread> threads;
    std::atomic<uint64_t> busy{};
    std::atomic<uint64_t> peak_queued{};
    std::atomic<uint64_t> queue_wait_ms_max{};
    std::atomic<uint64_t> handled{};

    Lane(const char* lane_name, const char* lane_thread_name)
        : name(lane_name), thread_name(lane_thread_name) {}

    void start(size_t workers) {
        threads.reserve(workers);
        for (size_t i = 0; i < workers; ++i)
            threads.emplace_back([this](std::stop_token stop) {
                run_supervised(name, [this, stop] { loop(stop); });
            });
    }

    void request_stop() {
        for (auto& thread : threads)
            thread.request_stop();
        cv.notify_all();
    }

    void join() {
        for (auto& thread : threads)
            if (thread.joinable())
                thread.join();
        threads.clear();
        std::lock_guard lock(mutex);
        queue.clear();
    }

    // False when the lane is full and the caller has to answer for itself.
    // Body reads bypass the cap: they are bounded by the staging windows of
    // the connections already admitted, and a viewer mid-fragment is not a
    // new request to refuse.
    bool post(std::function<void()> run, bool bypass_cap = false) {
        {
            std::lock_guard lock(mutex);
            if (!bypass_cap && max_queued && queue.size() >= max_queued)
                return false;
            queue.push_back({std::move(run), Clock::now()});
            const auto depth = static_cast<uint64_t>(queue.size());
            auto peak = peak_queued.load(std::memory_order_relaxed);
            while (depth > peak &&
                   !peak_queued.compare_exchange_weak(peak, depth, std::memory_order_relaxed)) {
            }
        }
        cv.notify_one();
        return true;
    }

    size_t queued() {
        std::lock_guard lock(mutex);
        return queue.size();
    }

    void loop(std::stop_token stop) {
        set_thread_name(thread_name);
        while (!stop.stop_requested()) {
            Job job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, stop, [this] { return !queue.empty(); });
                if (stop.stop_requested())
                    break;
                job = std::move(queue.front());
                queue.pop_front();
            }
            const auto waited = static_cast<uint64_t>(ms_between(job.queued_at, Clock::now()));
            auto wait_max = queue_wait_ms_max.load(std::memory_order_relaxed);
            while (waited > wait_max && !queue_wait_ms_max.compare_exchange_weak(
                                            wait_max, waited, std::memory_order_relaxed)) {
            }
            busy.fetch_add(1, std::memory_order_relaxed);
            try {
                job.run();
            } catch (const std::exception& e) {
                Log::debug("HTTP " + std::string(name) + " lane job: " + e.what());
            } catch (...) {
            }
            busy.fetch_sub(1, std::memory_order_relaxed);
            handled.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

enum class ConnectionState { reading, dispatched, deferred, writing };

// Everything the reactor knows about one socket. Deliberately nothing here
// can be called into: no handler, no body source, no ReadHandle, no
// authenticator. The pump exposes memory the pool has already filled and
// nothing else, so the reactor cannot sleep on anything it holds.
struct Connection {
    uint64_t id{};
    int fd{-1};
    ConnectionState state{ConnectionState::reading};
    // Bumped when a request starts; an event carrying an older generation
    // belongs to a request this connection has already finished with.
    uint64_t generation{};
    std::string input;
    size_t requests_served{};
    bool continuation{};
    bool input_closed{};
    Clock::time_point deadline{Clock::time_point::max()};

    // The request in flight, kept so a deferral can be resumed.
    HttpRequest request;
    bool head_only{};
    bool keep_alive{};
    std::optional<HttpDeferral> deferral;

    // The response being written: status line, headers and, for an inline
    // body, the body itself, in one buffer.
    std::string out;
    size_t out_sent{};
    std::shared_ptr<BodyPump> pump;
    uint64_t body_total{};
    uint64_t body_sent{};
    std::deque<Bytes> staged;
    size_t staged_sent{};
    bool read_in_flight{};
    uint64_t read_offset{};
    bool want_write{};
};

} // namespace

HttpResponse http_json(int status, std::string value) {
    return {status, "application/json; charset=utf-8", {}, Bytes(value.begin(), value.end()), {}};
}

HttpResponse http_error(int status, std::string_view code, std::string_view message) {
    Json::Object root;
    root["error"] = Json::Object{{"code", std::string(code)}, {"message", std::string(message)}};
    return http_json(status, Json(std::move(root)).dump());
}

HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason) {
    Json::Object root;
    root["error"] = Json::Object{{"code", std::string(code)},
                                 {"message", std::string(message)},
                                 {"reason", std::string(reason)}};
    return http_json(status, Json(std::move(root)).dump());
}

const char* failure_scope_name(FailureScope scope) noexcept {
    switch (scope) {
    case FailureScope::content: return "content";
    case FailureScope::node: return "node";
    case FailureScope::request: return "request";
    }
    return "node";
}

HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason, const FailureAxes& axes) {
    Json::Object error{{"code", std::string(code)}, {"message", std::string(message)}};
    if (!reason.empty())
        error["reason"] = std::string(reason);
    // Omitted rather than defaulted: an axis this server cannot honestly state
    // must read as "no answer" to the client, not as "false".
    if (axes.scope)
        error["scope"] = std::string(failure_scope_name(*axes.scope));
    if (axes.node_healthy)
        error["node_healthy"] = *axes.node_healthy;
    if (axes.alternative_may_succeed)
        error["alternative_may_succeed"] = *axes.alternative_may_succeed;
    Json::Object root;
    root["error"] = std::move(error);
    return http_json(status, Json(std::move(root)).dump());
}

std::string http_url_decode(std::string_view value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
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

void HttpWaker::fire() {
    std::function<void()> wake;
    {
        std::lock_guard lock(mutex_);
        if (fired_)
            return;
        fired_ = true;
        wake = std::move(wake_);
    }
    if (wake)
        wake();
}

void HttpWaker::arm(std::function<void()> wake) {
    bool now = false;
    {
        std::lock_guard lock(mutex_);
        if (fired_)
            now = true;
        else
            wake_ = std::move(wake);
    }
    if (now && wake)
        wake();
}

HttpResponse http_resolve(const std::function<HttpResponse(const HttpRequest&)>& handler,
                          HttpRequest request) {
    struct Flag {
        std::mutex mutex;
        std::condition_variable cv;
        bool woken{};
    };
    while (true) {
        auto response = handler(request);
        if (!response.defer)
            return response;
        auto deferral = std::move(*response.defer);
        auto flag = std::make_shared<Flag>();
        if (deferral.waker)
            deferral.waker->arm([flag] {
                std::lock_guard lock(flag->mutex);
                flag->woken = true;
                flag->cv.notify_all();
            });
        {
            std::unique_lock lock(flag->mutex);
            while (!flag->woken && Clock::now() < deferral.deadline)
                flag->cv.wait_for(lock, 1s, [&] { return flag->woken; });
        }
        request.resumed = true;
        request.resumed_state = std::move(deferral.state);
        request.resume_deadline = deferral.deadline;
    }
}

struct HttpServer::Impl {
    CatalogueApiConfig config;
    std::function<HttpResponse(const HttpRequest&)> handler;
    std::function<bool(const HttpRequest&)> bearer_exempt;
    SessionAuthenticator authenticate;
    std::vector<std::string> control_prefixes;
    std::function<void()> pass_hook;

    std::shared_ptr<Inbox> inbox;
    Lane control{"http-control", "macha-http-ctl"};
    Lane data{"http-data", "macha-http-data"};
    std::jthread reactor;
    std::atomic_bool running{};
    std::atomic_bool stopping{};
    std::atomic<uint16_t> bound_port{};
    std::mutex startup_mutex;
    std::condition_variable startup_cv;
    bool startup_complete{};
    std::string startup_error;

    // Reactor-owned. Nothing below is touched from any other thread.
    std::map<uint64_t, std::unique_ptr<Connection>> connections;
    uint64_t next_connection_id{1};
    std::vector<pollfd> pollfds;
    std::vector<uint64_t> poll_ids;
    Clock::time_point last_accept_warning{};

    // Diagnostics: written by the reactor and the lanes, read from anywhere.
    std::atomic<uint64_t> passes{};
    std::atomic<uint64_t> stalls{};
    std::atomic<uint64_t> longest_pass_ms{};
    std::atomic<uint64_t> open{};
    std::atomic<uint64_t> idle{};
    std::atomic<uint64_t> writing{};
    std::atomic<uint64_t> deferred{};
    std::atomic<uint64_t> refused{};
    std::atomic<uint64_t> staged_bytes{};
    std::atomic<uint64_t> served{};
    std::atomic<uint64_t> deferrals{};
    std::atomic<uint64_t> overloaded{};
    std::atomic<uint64_t> slow{};
    mutable std::atomic<uint64_t> compressed_responses{};
    mutable std::atomic<uint64_t> compression_saved{};

    Impl(CatalogueApiConfig c, std::function<HttpResponse(const HttpRequest&)> h,
         std::function<bool(const HttpRequest&)> exempt, SessionAuthenticator auth)
        : config(std::move(c)), handler(std::move(h)), bearer_exempt(std::move(exempt)),
          authenticate(std::move(auth)) {}

    bool control_route(std::string_view path) const noexcept {
        for (const auto& prefix : control_prefixes) {
            if (path == prefix)
                return true;
            if (path.size() > prefix.size() && path.starts_with(prefix) &&
                path[prefix.size()] == '/')
                return true;
        }
        return false;
    }

    size_t chunk_bytes() const noexcept {
        return std::max<size_t>(16 * 1024, config.stream_chunk_bytes);
    }

    size_t staging_chunks() const noexcept {
        return std::max<size_t>(1, config.staging_chunks);
    }

    // ---- the compute side: what a lane job does -------------------------

    HttpResponse run_handler(HttpRequest& request) {
        const auto started = Clock::now();
        HttpResponse response;
        try {
            const bool exempt = bearer_exempt && bearer_exempt(request);
            if (authenticate && !exempt) {
                std::string_view token;
                if (auto it = request.headers.find("authorization"); it != request.headers.end()) {
                    static constexpr std::string_view prefix = "Bearer ";
                    const std::string_view value = it->second;
                    if (value.substr(0, prefix.size()) == prefix)
                        token = value.substr(prefix.size());
                }
                auto identity = !token.empty() ? authenticate(token) : std::nullopt;
                if (!identity) {
                    response =
                        http_error(401, "unauthorized", "a valid session bearer token is required");
                } else {
                    request.session = std::move(identity);
                    response = handler(request);
                }
            } else {
                response = handler(request);
            }
        } catch (const std::exception& e) {
            Log::debug("HTTP handler " + request.method + " " + request.path + ": " + e.what());
            response = http_error(500, "internal", "the request could not be completed");
        }
        const auto elapsed = ms_between(started, Clock::now());
        if (!response.defer && elapsed >= config.slow_request_threshold.count()) {
            slow.fetch_add(1, std::memory_order_relaxed);
            Log::info("HTTP slow request method=" + request.method + " path=" + request.path +
                      " status=" + std::to_string(response.status) +
                      " elapsed_ms=" + std::to_string(elapsed) +
                      " lane=" + (control_route(request.path) ? "control" : "data"));
        }
        return response;
    }

    // Runs here, on a lane worker, never on the reactor: gzip is CPU work and
    // the reactor may not do any (see the class comment on HttpServer). By the
    // time the reactor sees this response the body is final, so the
    // Content-Length it writes is already the compressed length.
    //
    // Only a complete in-memory body is eligible. A response carrying a
    // `stream` is media or a large file being pumped chunk by chunk, and the
    // reactor sends it from resident memory with no copy; that path is left
    // exactly as it was.
    void compress_response(const HttpRequest& request, HttpResponse& response) const {
        if (!config.compression.enabled || response.stream)
            return;
        // 204 and 304 have no body to compress; 206 is a range, and an
        // encoding applied to one part of a representation is not something a
        // client can reassemble.
        if (response.status == 204 || response.status == 206 || response.status == 304)
            return;
        if (!compressible_content_type(response.content_type))
            return;
        // A handler that already chose an encoding owns that negotiation, and
        // its entity tag -- if it set one -- names the representation it
        // chose. WebApi does exactly this for the client's assets.
        if (response.headers.contains("Content-Encoding"))
            return;

        // Say the response varies even when this particular client did not ask
        // for gzip. On a deployment where some nodes sit behind a proxy and
        // some are exposed directly, a shared cache that stored the identity
        // body under an unqualified key would go on to hand it to a client
        // that did ask -- and, worse, the reverse.
        response.headers.try_emplace("Vary", "Accept-Encoding");

        if (response.body.size() < config.compression.min_bytes)
            return;
        if (!client_accepts_gzip(request))
            return;
        auto compressed = gzip_compress(response.body, config.compression.level);
        if (!compressed)
            return;

        // Entity tags are deliberately left alone. The suffix convention other
        // servers use would corrupt the catalogue's `rev-N` tags, which are
        // If-Match concurrency tokens a client sends back on a write rather
        // than cache validators. Vary above is the mechanism that keeps caches
        // honest here.
        compression_saved.fetch_add(response.body.size() - compressed->size(),
                                    std::memory_order_relaxed);
        compressed_responses.fetch_add(1, std::memory_order_relaxed);
        response.body = std::move(*compressed);
        response.headers["Content-Encoding"] = "gzip";
    }

    void post_request(Connection& connection, HttpRequest request) {
        ++connection.generation;
        connection.state = ConnectionState::dispatched;
        connection.deadline = Clock::time_point::max();
        connection.request = HttpRequest{}; // the job owns it until it answers
        Lane& lane = control_route(request.path) ? control : data;
        const auto id = connection.id;
        const auto generation = connection.generation;
        std::weak_ptr<Inbox> weak_inbox = inbox;
        const bool posted =
            lane.post([this, id, generation, weak_inbox, request = std::move(request)]() mutable {
                Event event;
                event.kind = EventKind::response;
                event.connection = id;
                event.generation = generation;
                event.response = run_handler(request);
                if (!event.response.defer)
                    compress_response(request, event.response);
                if (event.response.stream) {
                    // The one place the source is asked anything on behalf of
                    // the reactor: here, on the pool.
                    event.pump = std::make_shared<BodyPump>(event.response.stream);
                    event.response.stream.reset();
                }
                if (event.response.defer)
                    event.request = std::move(request);
                if (auto strong = weak_inbox.lock())
                    strong->post(std::move(event));
            });
        if (!posted) {
            overloaded.fetch_add(1, std::memory_order_relaxed);
            // Answered by the reactor with no handler involved, so it says
            // what the health route would have said about who is answering:
            // a client confirming an endpoint by its body must still be able
            // to tell a busy Macha node from something that is not Macha.
            Json::Object root;
            root["service"] = "macha";
            root["status"] = "busy";
            root["error"] = Json::Object{{"code", "overloaded"},
                                         {"message", "the node is busy; retry shortly"}};
            auto response = http_json(503, Json(std::move(root)).dump());
            response.headers["Retry-After"] = "1";
            begin_response(connection, std::move(response), {});
        }
    }

    void post_chunk_read(Connection& connection) {
        const auto length = static_cast<size_t>(
            std::min<uint64_t>(chunk_bytes(), connection.body_total - connection.read_offset));
        const auto offset = connection.read_offset;
        const auto id = connection.id;
        const auto generation = connection.generation;
        auto pump = connection.pump;
        std::weak_ptr<Inbox> weak_inbox = inbox;
        connection.read_in_flight = true;
        connection.read_offset += length;
        data.post(
            [id, generation, weak_inbox, pump, offset, length] {
                Event event;
                event.kind = EventKind::chunk;
                event.connection = id;
                event.generation = generation;
                try {
                    event.bytes = pump->read(offset, length);
                    event.ok = !event.bytes.empty();
                } catch (const std::exception& e) {
                    Log::debug("HTTP body read: " + std::string(e.what()));
                    event.ok = false;
                }
                if (auto strong = weak_inbox.lock())
                    strong->post(std::move(event));
            },
            true);
    }

    // ---- the reactor ---------------------------------------------------

    int bind_listener() {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* results = nullptr;
        auto port = std::to_string(config.port);
        const char* host = config.listen.empty() ? nullptr : config.listen.c_str();
        if (getaddrinfo(host, port.c_str(), &hints, &results))
            throw std::runtime_error("catalogue API address resolution failed");
        int listen_fd = -1;
        for (auto* result = results; result; result = result->ai_next) {
            listen_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (listen_fd < 0)
                continue;
            int yes = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            const int backlog =
                static_cast<int>(std::clamp<size_t>(config.max_connections, 16, 1024));
            if (bind(listen_fd, result->ai_addr, result->ai_addrlen) == 0 &&
                listen(listen_fd, backlog) == 0 && set_non_blocking(listen_fd))
                break;
            ::close(listen_fd);
            listen_fd = -1;
        }
        freeaddrinfo(results);
        if (listen_fd < 0)
            throw std::runtime_error("cannot bind catalogue API");

        sockaddr_storage bound{};
        socklen_t bound_size = sizeof(bound);
        if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0) {
            if (bound.ss_family == AF_INET)
                bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
            else if (bound.ss_family == AF_INET6)
                bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
            else
                bound_port = config.port;
        } else {
            bound_port = config.port;
        }
        return listen_fd;
    }

    void reactor_loop(std::stop_token stop) {
        set_thread_name("macha-http-io");
        int listen_fd = -1;
        try {
            listen_fd = bind_listener();
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(startup_mutex);
                startup_error = e.what();
                startup_complete = true;
            }
            startup_cv.notify_all();
            return;
        }
        {
            std::lock_guard lock(startup_mutex);
            startup_complete = true;
        }
        startup_cv.notify_all();
        Log::info("HTTP API listening on " + config.listen + ":" +
                  std::to_string(bound_port.load()));

        bool stop_seen = false;
        while (!stop.stop_requested() && !stop_seen) {
            build_pollfds(listen_fd);
            const int timeout = poll_timeout_ms();
            const int ready = ::poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), timeout);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                Log::error("HTTP API poll failed: " + std::string(std::strerror(errno)));
                break;
            }
            const auto pass_started = Clock::now();
            if (pass_hook)
                pass_hook();

            // Cross-thread events first: a response or a chunk that is ready
            // is what most often makes a socket worth writing to.
            if (pollfds[1].revents & POLLIN) {
                for (auto& event : inbox->drain()) {
                    if (event.kind == EventKind::stop) {
                        stop_seen = true;
                        break;
                    }
                    handle_event(event);
                }
                if (stop_seen)
                    break;
            }
            if (pollfds[0].revents & POLLIN)
                accept_ready(listen_fd);

            for (size_t i = 2; i < pollfds.size(); ++i) {
                const auto revents = pollfds[i].revents;
                if (!revents)
                    continue;
                auto found = connections.find(poll_ids[i]);
                if (found == connections.end())
                    continue;
                Connection& connection = *found->second;
                if (revents & (POLLERR | POLLNVAL)) {
                    close_connection(connection.id);
                    continue;
                }
                if (revents & POLLIN) {
                    on_readable(connection);
                    if (!connections.contains(connection.id))
                        continue;
                }
                if (revents & POLLOUT) {
                    flush(connection);
                    if (!connections.contains(connection.id))
                        continue;
                }
                if ((revents & POLLHUP) && !(revents & POLLIN))
                    close_connection(connection.id);
            }

            expire(Clock::now());
            publish_counts();

            const auto pass_ms = static_cast<uint64_t>(ms_between(pass_started, Clock::now()));
            passes.fetch_add(1, std::memory_order_relaxed);
            if (pass_ms > longest_pass_ms.load(std::memory_order_relaxed))
                longest_pass_ms.store(pass_ms, std::memory_order_relaxed);
            if (pass_ms >= static_cast<uint64_t>(config.reactor_stall_threshold.count()))
                stalls.fetch_add(1, std::memory_order_relaxed);
        }

        for (auto& [id, connection] : connections) {
            ::shutdown(connection->fd, SHUT_RDWR);
            ::close(connection->fd);
        }
        connections.clear();
        staged_bytes.store(0);
        publish_counts();
        ::shutdown(listen_fd, SHUT_RDWR);
        ::close(listen_fd);
    }

    void build_pollfds(int listen_fd) {
        pollfds.clear();
        poll_ids.clear();
        pollfds.push_back({listen_fd, POLLIN, 0});
        poll_ids.push_back(0);
        pollfds.push_back({inbox->wake_fd(), POLLIN, 0});
        poll_ids.push_back(0);
        for (const auto& [id, connection] : connections) {
            short events = 0;
            const bool parked = connection->state != ConnectionState::reading;
            if (!connection->input_closed &&
                (!parked || connection->input.size() < parked_input_bytes))
                events |= POLLIN;
            if (connection->state == ConnectionState::writing && connection->want_write)
                events |= POLLOUT;
            pollfds.push_back({connection->fd, events, 0});
            poll_ids.push_back(id);
        }
    }

    int poll_timeout_ms() const {
        auto soonest = Clock::time_point::max();
        for (const auto& [id, connection] : connections)
            soonest = std::min(soonest, connection->deadline);
        if (soonest == Clock::time_point::max())
            return 1000;
        const auto remaining = ms_between(Clock::now(), soonest);
        return static_cast<int>(std::clamp<int64_t>(remaining, 0, 1000));
    }

    void accept_ready(int listen_fd) {
        while (true) {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                if (errno == EMFILE || errno == ENFILE) {
                    const auto now = Clock::now();
                    if (now - last_accept_warning > 5s) {
                        last_accept_warning = now;
                        Log::warn("HTTP API cannot accept: " + std::string(std::strerror(errno)));
                    }
                }
                return;
            }
            if (connections.size() >= config.max_connections || !set_non_blocking(fd)) {
                refused.fetch_add(1, std::memory_order_relaxed);
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
                continue;
            }
            int yes = 1;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            auto connection = std::make_unique<Connection>();
            connection->id = next_connection_id++;
            connection->fd = fd;
            connection->deadline = Clock::now() + config.client_io_timeout;
            connections.emplace(connection->id, std::move(connection));
        }
    }

    void close_connection(uint64_t id) {
        auto found = connections.find(id);
        if (found == connections.end())
            return;
        Connection& connection = *found->second;
        for (const auto& chunk : connection.staged)
            staged_bytes.fetch_sub(chunk.size(), std::memory_order_relaxed);
        ::shutdown(connection.fd, SHUT_RDWR);
        ::close(connection.fd);
        // Dropping the connection drops its pump, its deferral state and
        // its parked request: a hold is released here, a body source
        // closed, within the pass that noticed the client had gone.
        connections.erase(found);
    }

    void on_readable(Connection& connection) {
        std::array<char, recv_buffer_bytes> buffer{};
        size_t read_this_pass = 0;
        while (read_this_pass < recv_per_pass_bytes) {
            const auto n = ::recv(connection.fd, buffer.data(), buffer.size(), 0);
            if (n > 0) {
                connection.input.append(buffer.data(), static_cast<size_t>(n));
                read_this_pass += static_cast<size_t>(n);
                continue;
            }
            if (n == 0) {
                connection.input_closed = true;
                break;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            close_connection(connection.id);
            return;
        }
        if (connection.state == ConnectionState::reading) {
            try_parse(connection);
            return;
        }
        // Parked between requests: input is kept for the pipelined next
        // request, bounded by build_pollfds.
    }

    void try_parse(Connection& connection) {
        if (connection.state != ConnectionState::reading)
            return;
        auto& input = connection.input;
        const auto header_end = input.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (input.size() > max_header_bytes || connection.input_closed)
                close_connection(connection.id);
            return;
        }

        std::istringstream headers(input.substr(0, header_end));
        std::string request_line;
        std::getline(headers, request_line);
        if (!request_line.empty() && request_line.back() == '\r')
            request_line.pop_back();
        std::istringstream request_stream(request_line);
        HttpRequest request;
        std::string target, version;
        request_stream >> request.method >> target >> version;
        if (request.method.empty() || target.empty()) {
            close_connection(connection.id);
            return;
        }
        auto question = target.find('?');
        request.path = http_url_decode(target.substr(0, question));
        if (question != std::string::npos)
            request.query = parse_query(target.substr(question + 1));

        std::string line;
        size_t content_length = 0;
        while (std::getline(headers, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            auto key = lower(line.substr(0, colon));
            auto value = line.substr(colon + 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());
            request.headers[key] = value;
            if (key == "content-length") {
                auto [end, ec] =
                    std::from_chars(value.data(), value.data() + value.size(), content_length);
                if (ec != std::errc{} || end != value.data() + value.size()) {
                    close_connection(connection.id);
                    return;
                }
            }
        }

        // HTTP/1.1 connections default to persistent unless the client asks
        // to close; HTTP/1.0 connections default to close unless the client
        // explicitly asks to keep the connection alive.
        const auto connection_header = request.headers.find("connection");
        const std::string connection_value = connection_header != request.headers.end()
                                                 ? lower(connection_header->second)
                                                 : std::string();
        const bool requested_close = connection_value.find("close") != std::string::npos;
        const bool requested_keep_alive = connection_value.find("keep-alive") != std::string::npos;
        bool keep_alive = version == "HTTP/1.1" ? !requested_close : requested_keep_alive;
        if (keep_alive && connection.requests_served + 1 >= config.keep_alive_max_requests)
            keep_alive = false;
        connection.head_only = request.method == "HEAD";
        connection.keep_alive = keep_alive;

        const auto body_start = header_end + 4;
        if (content_length > config.max_request_bytes) {
            // The body is never read, so the connection cannot be reused:
            // the next "request" on it would be the tail of this one.
            input.clear();
            connection.keep_alive = false;
            begin_response(connection, http_error(413, "too_large", "request body too large"), {});
            return;
        }
        if (input.size() - body_start < content_length)
            return; // the rest is still arriving
        request.body.assign(input.begin() + static_cast<std::ptrdiff_t>(body_start),
                            input.begin() +
                                static_cast<std::ptrdiff_t>(body_start + content_length));
        input.erase(0, body_start + content_length);

        if (request.method == "OPTIONS") {
            begin_response(connection, HttpResponse{204, "text/plain", {}, {}, {}}, {});
            return;
        }
        post_request(connection, std::move(request));
    }

    void handle_event(Event& event) {
        auto found = connections.find(event.connection);
        if (found == connections.end())
            return;
        Connection& connection = *found->second;
        if (event.generation != connection.generation)
            return;
        switch (event.kind) {
        case EventKind::response:
            if (connection.state != ConnectionState::dispatched)
                return;
            if (event.response.defer) {
                park(connection, std::move(*event.response.defer), std::move(event.request));
                return;
            }
            begin_response(connection, std::move(event.response), std::move(event.pump));
            return;
        case EventKind::chunk:
            if (connection.state != ConnectionState::writing || !connection.read_in_flight)
                return;
            connection.read_in_flight = false;
            if (!event.ok) {
                // Fewer bytes than the declared Content-Length: the response
                // cannot be completed and the connection cannot be reused.
                close_connection(connection.id);
                return;
            }
            staged_bytes.fetch_add(event.bytes.size(), std::memory_order_relaxed);
            connection.staged.push_back(std::move(event.bytes));
            flush(connection);
            return;
        case EventKind::wake:
            if (connection.state == ConnectionState::deferred)
                resume(connection);
            return;
        case EventKind::stop:
            return;
        }
    }

    void park(Connection& connection, HttpDeferral deferral, std::optional<HttpRequest> request) {
        deferrals.fetch_add(1, std::memory_order_relaxed);
        connection.state = ConnectionState::deferred;
        connection.deadline = deferral.deadline;
        if (request)
            connection.request = std::move(*request);
        const auto id = connection.id;
        const auto generation = connection.generation;
        std::weak_ptr<Inbox> weak_inbox = inbox;
        auto waker = deferral.waker;
        connection.deferral = std::move(deferral);
        if (waker)
            waker->arm([id, generation, weak_inbox] {
                Event event;
                event.kind = EventKind::wake;
                event.connection = id;
                event.generation = generation;
                if (auto strong = weak_inbox.lock())
                    strong->post(std::move(event));
            });
    }

    void resume(Connection& connection) {
        auto request = std::move(connection.request);
        request.resumed = true;
        if (connection.deferral) {
            request.resumed_state = std::move(connection.deferral->state);
            request.resume_deadline = connection.deferral->deadline;
        }
        connection.deferral.reset();
        post_request(connection, std::move(request));
    }

    void begin_response(Connection& connection, HttpResponse response,
                        std::shared_ptr<BodyPump> pump) {
        response.headers.try_emplace("Access-Control-Allow-Origin", "*");
        response.headers.try_emplace("Access-Control-Allow-Headers",
                                     "Authorization, Content-Type, If-Match, Range");
        response.headers.try_emplace("Access-Control-Allow-Methods",
                                     "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
        response.headers.try_emplace(
            "Access-Control-Expose-Headers",
            "Accept-Ranges, Content-Length, Content-Range, Location, Retry-After");
        response.headers.try_emplace("Accept-Ranges", pump ? "bytes" : "none");
        // A node can advertise a public endpoint, and everything it serves
        // is a private media library: the API, artwork, the web client. The
        // client's own robots meta tag covers HTML and nothing else; this
        // covers all of it. Not a security control -- the session gate is
        // that -- just the standard way of staying out of search results.
        response.headers.try_emplace("X-Robots-Tag", "noindex, nofollow");
        const uint64_t content_length = pump ? pump->size() : response.body.size();

        std::ostringstream out;
        out << "HTTP/1.1 " << response.status << ' ' << reason(response.status) << "\r\n";
        out << "Content-Type: " << response.content_type << "\r\n";
        out << "Content-Length: " << content_length << "\r\n";
        out << "Connection: " << (connection.keep_alive ? "keep-alive" : "close") << "\r\n";
        for (const auto& [key, value] : response.headers)
            out << key << ": " << value << "\r\n";
        out << "\r\n";
        connection.out = out.str();
        connection.out_sent = 0;
        if (!connection.head_only && !pump && !response.body.empty())
            connection.out.append(response.body.begin(), response.body.end());
        connection.pump = connection.head_only ? nullptr : std::move(pump);
        connection.body_total = connection.pump ? content_length : 0;
        connection.body_sent = 0;
        connection.read_offset = 0;
        connection.read_in_flight = false;
        connection.staged.clear();
        connection.staged_sent = 0;
        connection.state = ConnectionState::writing;
        connection.deadline = Clock::now() + config.client_io_timeout;
        top_up(connection);
        flush(connection);
    }

    // Keep the staging window full: at most one read outstanding, at most
    // staging_chunks chunks waiting, never past the end of the body.
    void top_up(Connection& connection) {
        if (!connection.pump || connection.pump->resident())
            return;
        if (connection.read_in_flight || connection.staged.size() >= staging_chunks())
            return;
        if (connection.read_offset >= connection.body_total)
            return;
        post_chunk_read(connection);
    }

    // Send whatever is ready. Returns with want_write set when the socket
    // would block, and with the connection finished or closed otherwise.
    void flush(Connection& connection) {
        if (connection.state != ConnectionState::writing)
            return;
        connection.want_write = false;
        while (true) {
            const uint8_t* data = nullptr;
            size_t length = 0;
            enum { head, resident, staged } from = head;
            if (connection.out_sent < connection.out.size()) {
                data =
                    reinterpret_cast<const uint8_t*>(connection.out.data()) + connection.out_sent;
                length = connection.out.size() - connection.out_sent;
            } else if (connection.body_sent >= connection.body_total) {
                finish_response(connection);
                return;
            } else if (const auto* whole = connection.pump->resident()) {
                data = whole + connection.body_sent;
                length = static_cast<size_t>(std::min<uint64_t>(
                    connection.body_total - connection.body_sent, chunk_bytes()));
                from = resident;
            } else if (connection.staged.empty()) {
                top_up(connection); // waiting on the pool for the next chunk
                return;
            } else {
                const auto& front = connection.staged.front();
                data = front.data() + connection.staged_sent;
                length = front.size() - connection.staged_sent;
                from = staged;
            }

            const auto n = ::send(connection.fd, data, length, send_flags);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    connection.want_write = true;
                    return;
                }
                close_connection(connection.id);
                return;
            }
            const auto sent = static_cast<size_t>(n);
            connection.deadline = Clock::now() + config.client_io_timeout;
            switch (from) {
            case head:
                connection.out_sent += sent;
                break;
            case resident:
                connection.body_sent += sent;
                break;
            case staged:
                connection.staged_sent += sent;
                connection.body_sent += sent;
                if (connection.staged_sent == connection.staged.front().size()) {
                    staged_bytes.fetch_sub(connection.staged.front().size(),
                                           std::memory_order_relaxed);
                    connection.staged.pop_front();
                    connection.staged_sent = 0;
                    top_up(connection);
                }
                break;
            }
        }
    }

    void finish_response(Connection& connection) {
        served.fetch_add(1, std::memory_order_relaxed);
        connection.out.clear();
        connection.out.shrink_to_fit();
        connection.out_sent = 0;
        connection.pump.reset();
        connection.body_total = 0;
        connection.body_sent = 0;
        connection.staged.clear();
        connection.staged_sent = 0;
        connection.read_in_flight = false;
        connection.read_offset = 0;
        connection.request = HttpRequest{};
        connection.deferral.reset();
        if (!connection.keep_alive || connection.input_closed) {
            close_connection(connection.id);
            return;
        }
        ++connection.requests_served;
        connection.continuation = true;
        connection.state = ConnectionState::reading;
        connection.deadline = Clock::now() + config.keep_alive_idle_timeout;
        // A pipelining client may already have sent the next request.
        try_parse(connection);
    }

    void expire(Clock::time_point now) {
        std::vector<uint64_t> expired;
        std::vector<uint64_t> due;
        for (const auto& [id, connection] : connections) {
            if (connection->deadline > now)
                continue;
            if (connection->state == ConnectionState::deferred)
                due.push_back(id);
            else if (connection->state != ConnectionState::dispatched)
                expired.push_back(id);
        }
        for (const auto id : expired)
            close_connection(id);
        for (const auto id : due)
            if (auto found = connections.find(id); found != connections.end())
                resume(*found->second);
    }

    void publish_counts() {
        uint64_t idle_count = 0, writing_count = 0, deferred_count = 0;
        for (const auto& [id, connection] : connections) {
            switch (connection->state) {
            case ConnectionState::reading:
                if (connection->continuation && connection->input.empty())
                    ++idle_count;
                break;
            case ConnectionState::writing:
                ++writing_count;
                break;
            case ConnectionState::deferred:
                ++deferred_count;
                break;
            case ConnectionState::dispatched:
                break;
            }
        }
        open.store(connections.size(), std::memory_order_relaxed);
        idle.store(idle_count, std::memory_order_relaxed);
        writing.store(writing_count, std::memory_order_relaxed);
        deferred.store(deferred_count, std::memory_order_relaxed);
    }
};

HttpServer::HttpServer(CatalogueApiConfig config,
                       std::function<HttpResponse(const HttpRequest&)> handler,
                       std::function<bool(const HttpRequest&)> bearer_exempt,
                       SessionAuthenticator authenticate)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(handler), std::move(bearer_exempt),
                                   std::move(authenticate))) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::set_control_prefixes(std::vector<std::string> prefixes) {
    impl_->control_prefixes = std::move(prefixes);
}

void HttpServer::set_reactor_pass_hook(std::function<void()> hook) {
    impl_->pass_hook = std::move(hook);
}

bool HttpServer::running() const {
    return impl_->running.load();
}

uint16_t HttpServer::bound_port() const {
    return impl_->bound_port.load();
}

void HttpServer::start() {
    if (impl_->running.exchange(true))
        return;
    impl_->stopping = false;
    {
        std::lock_guard lock(impl_->startup_mutex);
        impl_->startup_complete = false;
        impl_->startup_error.clear();
    }
    impl_->inbox = std::make_shared<Inbox>();
    impl_->control.max_queued = impl_->config.max_queued_requests;
    impl_->data.max_queued = impl_->config.max_queued_requests;
    impl_->control.start(std::max<size_t>(1, impl_->config.control_workers));
    impl_->data.start(std::max<size_t>(1, impl_->config.workers));
    impl_->reactor = std::jthread([this](std::stop_token stop) {
        run_supervised("http-reactor", [this, stop] { impl_->reactor_loop(stop); });
    });
    std::unique_lock lock(impl_->startup_mutex);
    impl_->startup_cv.wait(lock, [this] { return impl_->startup_complete; });
    if (!impl_->startup_error.empty()) {
        const auto error = impl_->startup_error;
        lock.unlock();
        stop();
        throw std::runtime_error(error);
    }
}

void HttpServer::request_stop() {
    if (!impl_->running.load())
        return;
    if (impl_->stopping.exchange(true))
        return;
    if (impl_->inbox) {
        Event event;
        event.kind = EventKind::stop;
        impl_->inbox->post(std::move(event));
    }
    if (impl_->reactor.joinable())
        impl_->reactor.request_stop();
    impl_->control.request_stop();
    impl_->data.request_stop();
}

void HttpServer::stop() {
    if (!impl_->running.load())
        return;
    request_stop();
    if (impl_->reactor.joinable())
        impl_->reactor.join();
    impl_->control.join();
    impl_->data.join();
    if (impl_->inbox)
        impl_->inbox->close();
    impl_->inbox.reset();
    impl_->bound_port = 0;
    impl_->running = false;
    impl_->stopping = false;
}

HttpServerDiagnostics HttpServer::diagnostics() const {
    auto& impl = *impl_;
    HttpServerDiagnostics out;
    out.reactor_passes = impl.passes.load(std::memory_order_relaxed);
    out.reactor_stalls = impl.stalls.load(std::memory_order_relaxed);
    out.reactor_longest_pass_ms = impl.longest_pass_ms.load(std::memory_order_relaxed);
    out.connections_open = impl.open.load(std::memory_order_relaxed);
    out.connections_idle_keep_alive = impl.idle.load(std::memory_order_relaxed);
    out.connections_writing = impl.writing.load(std::memory_order_relaxed);
    out.connections_deferred = impl.deferred.load(std::memory_order_relaxed);
    out.connections_refused = impl.refused.load(std::memory_order_relaxed);
    out.staged_bytes = impl.staged_bytes.load(std::memory_order_relaxed);
    out.requests_served = impl.served.load(std::memory_order_relaxed);
    out.requests_deferred = impl.deferrals.load(std::memory_order_relaxed);
    out.requests_overloaded = impl.overloaded.load(std::memory_order_relaxed);
    out.slow_requests = impl.slow.load(std::memory_order_relaxed);
    out.responses_compressed = impl.compressed_responses.load(std::memory_order_relaxed);
    out.compression_bytes_saved = impl.compression_saved.load(std::memory_order_relaxed);
    const auto lane = [](Lane& source) {
        HttpServerDiagnostics::Lane out_lane;
        out_lane.workers = source.threads.size();
        out_lane.busy = source.busy.load(std::memory_order_relaxed);
        out_lane.queued = source.queued();
        out_lane.peak_queued = source.peak_queued.load(std::memory_order_relaxed);
        out_lane.queue_wait_ms_max = source.queue_wait_ms_max.load(std::memory_order_relaxed);
        out_lane.handled = source.handled.load(std::memory_order_relaxed);
        return out_lane;
    };
    out.control = lane(impl.control);
    out.data = lane(impl.data);
    return out;
}

} // namespace macha

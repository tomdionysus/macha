// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace macha {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string, std::less<>> query;
    std::map<std::string, std::string, std::less<>> headers;
    Bytes body;
    // Set by HttpServer once the bearer token has been validated against the
    // cluster session store. Absent for exempt routes (session creation,
    // signed streaming/asset capability URLs).
    std::optional<SessionIdentity> session;
};

class HttpBodySource {
  public:
    virtual ~HttpBodySource() = default;
    virtual uint64_t size() const = 0;
    virtual size_t read(uint64_t offset, std::span<uint8_t> destination) = 0;
};

struct HttpResponse {
    int status{200};
    std::string content_type{"application/json; charset=utf-8"};
    std::map<std::string, std::string, std::less<>> headers;
    Bytes body;
    std::shared_ptr<HttpBodySource> stream;

    HttpResponse() = default;
    HttpResponse(int response_status, std::string type,
                 std::map<std::string, std::string, std::less<>> response_headers,
                 Bytes response_body, std::shared_ptr<HttpBodySource> response_stream = {})
        : status(response_status), content_type(std::move(type)),
          headers(std::move(response_headers)), body(std::move(response_body)),
          stream(std::move(response_stream)) {}

    uint64_t content_length() const {
        return stream ? stream->size() : body.size();
    }
};

HttpResponse http_json(int status, std::string value);
HttpResponse http_error(int status, std::string_view code, std::string_view message);
// Same, plus a machine-readable reason the client can act on. Used where the
// server states why it could not answer and leaves the decision to the client.
HttpResponse http_error(int status, std::string_view code, std::string_view message,
                        std::string_view reason);
std::string http_url_decode(std::string_view value);

class HttpServer {
  public:
    using SessionAuthenticator = std::function<std::optional<SessionIdentity>(std::string_view)>;

  private:
    CatalogueApiConfig config_;
    std::function<HttpResponse(const HttpRequest&)> handler_;
    std::function<bool(const HttpRequest&)> bearer_exempt_;
    SessionAuthenticator authenticate_;
    std::jthread accept_thread_;
    std::vector<std::jthread> workers_;
    std::atomic_bool running_{};
    std::atomic<uint16_t> bound_port_{};
    std::atomic_int listen_fd_{-1};
    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    bool startup_complete_{};
    std::string startup_error_;

    std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    std::deque<int> queue_;
    std::mutex active_mutex_;
    std::set<int> active_fds_;

    void run(std::stop_token);
    void worker(std::stop_token);
    void handle_client(int);
    bool handle_one_request(int fd, int send_flags, bool is_continuation, size_t requests_served);
    bool queue_has_backlog();
    void close_queued_clients();

  public:
    HttpServer(CatalogueApiConfig, std::function<HttpResponse(const HttpRequest&)>,
               std::function<bool(const HttpRequest&)> bearer_exempt = {},
               SessionAuthenticator authenticate = {});
    ~HttpServer();
    void start();
    void request_stop();
    void stop();
    bool running() const { return running_.load(); }
    uint16_t bound_port() const { return bound_port_.load(); }
};

} // namespace macha
